#include <tracker_log.h>
#include <Arduino.h>
#include <audio_feedback.h>
#include <board_pins.h>
#include <atomic>

namespace {
struct Note {
    uint16_t hz;
    uint16_t onMs;
    uint16_t offMs;
};

struct Melody {
    const Note *notes;
    uint8_t count;
    const char *name;
};

// Every state has a recognisably different rhythm, modelled after the
// state-feedback melodies used by commercial badge trackers.
constexpr Note powerOn[] = {
    {1568, 70, 45}, {2093, 70, 45}, {2637, 150, 0},
};
constexpr Note powerOff[] = {
    {2637, 80, 45}, {2093, 80, 45}, {1568, 180, 0},
};
constexpr Note sos[] = {
    {2900, 130, 90}, {2900, 130, 90}, {2900, 130, 1500},
};
constexpr Note sosAcknowledged[] = {
    {2900, 130, 90}, {2900, 130, 90}, {2900, 130, 90}, {2640, 150, 1500},
};
constexpr Note fall[] = {
    {3300, 160, 70}, {3300, 160, 70}, {3300, 160, 70}, {3300, 320, 1500},
};
constexpr Note loraJoining[] = {
    {1200, 100, 60}, {1200, 100, 2000},
};
constexpr Note loraConnected[] = {
    {1760, 70, 40}, {2200, 70, 40}, {2640, 150, 0},
};
constexpr Note error[] = {
    {700, 180, 90}, {500, 250, 0},
};
constexpr Note otaMode[] = {
    {1200, 100, 55}, {1600, 100, 55}, {1200, 100, 55}, {1600, 100, 55},
    {2400, 220, 0},
};
constexpr Note deepSleep[] = {
    {900, 180, 0},
};

Melody melodyFor(AudioFeedback::Event event)
{
    switch (event) {
    case AudioFeedback::Event::PowerOn:
        return {powerOn, uint8_t(sizeof(powerOn) / sizeof(powerOn[0])), "power on"};
    case AudioFeedback::Event::PowerOff:
        return {powerOff, uint8_t(sizeof(powerOff) / sizeof(powerOff[0])), "power off"};
    case AudioFeedback::Event::Sos:
        return {sos, uint8_t(sizeof(sos) / sizeof(sos[0])), "SOS"};
    case AudioFeedback::Event::SosAcknowledged:
        return {sosAcknowledged, uint8_t(sizeof(sosAcknowledged) / sizeof(sosAcknowledged[0])), "SOS/Fall application acknowledged"};
    case AudioFeedback::Event::Fall:
        return {fall, uint8_t(sizeof(fall) / sizeof(fall[0])), "fall"};
    case AudioFeedback::Event::LoRaJoining:
        return {loraJoining, uint8_t(sizeof(loraJoining) / sizeof(loraJoining[0])), "LoRa joining"};
    case AudioFeedback::Event::LoRaConnected:
        return {loraConnected, uint8_t(sizeof(loraConnected) / sizeof(loraConnected[0])), "LoRa connected"};
    case AudioFeedback::Event::Error:
        return {error, uint8_t(sizeof(error) / sizeof(error[0])), "error"};
    case AudioFeedback::Event::OtaMode:
        return {otaMode, uint8_t(sizeof(otaMode) / sizeof(otaMode[0])), "OTA mode"};
    case AudioFeedback::Event::DeepSleep:
        return {deepSleep, uint8_t(sizeof(deepSleep) / sizeof(deepSleep[0])), "deep sleep"};
    }
    return {nullptr, 0, "unknown"};
}

// All LEDC access belongs to this worker. Producers only enqueue commands.
struct Command {
    AudioFeedback::Event event;
    uint32_t generation;
    bool silent;
    SemaphoreHandle_t completion;
};
QueueHandle_t eventQueue = nullptr;
SemaphoreHandle_t commandLock = nullptr;
bool buzzerAttached = false;
bool loopEnabled = false; // Protected by commandLock.
AudioFeedback::Event loopEvent = AudioFeedback::Event::LoRaJoining;
std::atomic<uint32_t> audioGeneration{0};
bool shuttingDown = false; // Protected by commandLock; terminal until reboot.

bool waitNote(uint32_t durationMs, uint32_t generation)
{
    const uint32_t started = millis();
    while (uint32_t(millis() - started) < durationMs) {
        if (audioGeneration.load() != generation) return false;
        delay(5);
    }
    return audioGeneration.load() == generation;
}

void stopBuzzer()
{
    if (buzzerAttached) {
        ledcWriteTone(Board::buzzer, 0);
        ledcDetach(Board::buzzer);
        buzzerAttached = false;
    }
    pinMode(Board::buzzer, OUTPUT);
    digitalWrite(Board::buzzer, LOW);
}

void playMelody(const Command &command)
{
    const Melody melody = melodyFor(command.event);
    if (command.silent || !melody.notes ||
        command.generation != audioGeneration.load()) return;
    if (!ledcAttach(Board::buzzer, melody.notes[0].hz, 10)) {
        TrackerLog.println("[AUDIO] Failed to attach buzzer LEDC");
        return;
    }
    buzzerAttached = true;
    for (uint8_t i = 0; i < melody.count; ++i) {
        if (command.generation != audioGeneration.load()) break;
        const Note &note = melody.notes[i];
        ledcWriteTone(Board::buzzer, note.hz);
        if (!waitNote(note.onMs, command.generation)) break;
        ledcWriteTone(Board::buzzer, 0);
        if (note.offMs && !waitNote(note.offMs, command.generation)) break;
    }
}

// Caller holds commandLock. Complete cancelled synchronous requests as well:
// resetting the queue alone would strand their waiting callers.
void cancelPending()
{
    loopEnabled = false;
    ++audioGeneration;
    Command command;
    while (xQueueReceive(eventQueue, &command, 0) == pdTRUE) {
        if (command.completion) xSemaphoreGive(command.completion);
    }
}

void worker(void *)
{
    Command command;
    for (;;) {
        if (xQueueReceive(eventQueue, &command, portMAX_DELAY) != pdTRUE) continue;
        playMelody(command);
        stopBuzzer();
        if (command.completion) xSemaphoreGive(command.completion);
        xSemaphoreTake(commandLock, portMAX_DELAY);
        if (!shuttingDown && loopEnabled && !command.silent &&
            command.event == loopEvent && command.generation == audioGeneration.load()) {
            command.completion = nullptr;
            xQueueSend(eventQueue, &command, 0);
        }
        xSemaphoreGive(commandLock);
    }
}

// A generation change cancels a dequeued-but-not-yet-started melody too.
// The barrier completes only after the worker has silenced the physical pin.
void synchronous(AudioFeedback::Event event, bool silent, bool terminal)
{
    if (!eventQueue || !commandLock) return;
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        // Still stop any current/queued work; do not play a partial shutdown.
        xSemaphoreTake(commandLock, portMAX_DELAY);
        shuttingDown = true;
        cancelPending();
        xSemaphoreGive(commandLock);
        while (true) delay(1000); // Watchdog recovers an allocation failure.
    }
    xSemaphoreTake(commandLock, portMAX_DELAY);
    if (shuttingDown && !silent) {
        xSemaphoreGive(commandLock);
        vSemaphoreDelete(done);
        return;
    }
    if (terminal) shuttingDown = true;
    cancelPending();
    const Command command{event, audioGeneration.load(), silent, done};
    xQueueSend(eventQueue, &command, portMAX_DELAY);
    xSemaphoreGive(commandLock);
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}
}

namespace AudioFeedback {
void begin()
{
    if (eventQueue) return;
    stopBuzzer(); // Before any worker exists.
    commandLock = xSemaphoreCreateMutex();
    eventQueue = xQueueCreate(4, sizeof(Command));
    if (!commandLock || !eventQueue ||
        xTaskCreate(worker, "buzzer", 3072, nullptr, 1, nullptr) != pdPASS) {
        TrackerLog.println("[AUDIO] Buzzer worker unavailable");
        if (eventQueue) vQueueDelete(eventQueue);
        if (commandLock) vSemaphoreDelete(commandLock);
        eventQueue = nullptr;
        commandLock = nullptr;
    }
}

void play(Event event)
{
    if (!eventQueue) return;
    xSemaphoreTake(commandLock, portMAX_DELAY);
    if (!shuttingDown) {
        const Command command{event, audioGeneration.load(), false, nullptr};
        if (xQueueSend(eventQueue, &command, 0) == pdTRUE)
            TrackerLog.printf("[AUDIO] %s melody queued\n", melodyFor(event).name);
        else
            TrackerLog.printf("[AUDIO] Dropped %s melody (queue busy)\n", melodyFor(event).name);
    }
    xSemaphoreGive(commandLock);
}

void startLoop(Event event)
{
    if (!eventQueue) return;
    xSemaphoreTake(commandLock, portMAX_DELAY);
    if (!shuttingDown && !(loopEnabled && loopEvent == event)) {
        cancelPending();
        loopEvent = event;
        loopEnabled = true;
        const Command command{event, audioGeneration.load(), false, nullptr};
        xQueueSend(eventQueue, &command, 0);
        TrackerLog.printf("[AUDIO] %s melody loop started\n", melodyFor(event).name);
    }
    xSemaphoreGive(commandLock);
}

void stopLoop(Event event)
{
    if (!eventQueue) return;
    xSemaphoreTake(commandLock, portMAX_DELAY);
    if (!shuttingDown && loopEnabled && loopEvent == event) {
        cancelPending();
        TrackerLog.printf("[AUDIO] %s melody loop stopped\n", melodyFor(event).name);
    }
    xSemaphoreGive(commandLock);
}

void playAndWait(Event event) { synchronous(event, false, false); }
void silence() { synchronous(Event::DeepSleep, true, false); }
void shutdown() { synchronous(Event::PowerOff, false, true); }
void prepareSleep() { synchronous(Event::DeepSleep, true, true); }
}
