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
constexpr Note fall[] = {
    {3300, 160, 70}, {3300, 160, 70}, {3300, 160, 70}, {3300, 320, 1500},
};
constexpr Note loraJoining[] = {
    {1200, 100, 60}, {1200, 100, 2000},
};
constexpr Note loraConnected[] = {
    {1760, 70, 40}, {2200, 70, 40}, {2640, 150, 0},
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
    case AudioFeedback::Event::Fall:
        return {fall, uint8_t(sizeof(fall) / sizeof(fall[0])), "fall"};
    case AudioFeedback::Event::LoRaJoining:
        return {loraJoining, uint8_t(sizeof(loraJoining) / sizeof(loraJoining[0])), "LoRa joining"};
    case AudioFeedback::Event::LoRaConnected:
        return {loraConnected, uint8_t(sizeof(loraConnected) / sizeof(loraConnected[0])), "LoRa connected"};
    case AudioFeedback::Event::OtaMode:
        return {otaMode, uint8_t(sizeof(otaMode) / sizeof(otaMode[0])), "OTA mode"};
    case AudioFeedback::Event::DeepSleep:
        return {deepSleep, uint8_t(sizeof(deepSleep) / sizeof(deepSleep[0])), "deep sleep"};
    }
    return {nullptr, 0, "unknown"};
}

QueueHandle_t eventQueue = nullptr;
SemaphoreHandle_t buzzerLock = nullptr;
bool buzzerAttached = false;
std::atomic<bool> loopEnabled{false};
std::atomic<AudioFeedback::Event> loopEvent{AudioFeedback::Event::LoRaJoining};
std::atomic<uint32_t> audioGeneration{0};

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

void playMelody(AudioFeedback::Event event)
{
    const Melody melody = melodyFor(event);
    if (!melody.notes || !buzzerLock) return;
    xSemaphoreTake(buzzerLock, portMAX_DELAY);
    const uint32_t generation = audioGeneration.load();
    if (!ledcAttach(Board::buzzer, melody.notes[0].hz, 10)) {
        Serial.println("[AUDIO] Failed to attach buzzer LEDC");
        xSemaphoreGive(buzzerLock);
        return;
    }
    buzzerAttached = true;
    for (uint8_t i = 0; i < melody.count; ++i) {
        const Note &note = melody.notes[i];
        ledcWriteTone(Board::buzzer, note.hz);
        if (!waitNote(note.onMs, generation)) break;
        ledcWriteTone(Board::buzzer, 0);
        if (note.offMs && !waitNote(note.offMs, generation)) break;
    }
    stopBuzzer();
    xSemaphoreGive(buzzerLock);
}

void worker(void *)
{
    AudioFeedback::Event event;
    for (;;) {
        if (xQueueReceive(eventQueue, &event, portMAX_DELAY) == pdTRUE) {
            playMelody(event);
            if (loopEnabled && event == loopEvent)
                xQueueSend(eventQueue, &event, 0);
        }
    }
}
}

namespace AudioFeedback {
void begin()
{
    if (eventQueue) return;
    buzzerLock = xSemaphoreCreateMutex();
    eventQueue = xQueueCreate(4, sizeof(Event));
    if (!buzzerLock || !eventQueue ||
        xTaskCreate(worker, "buzzer", 2048, nullptr, 1, nullptr) != pdPASS) {
        Serial.println("[AUDIO] Buzzer worker unavailable");
        eventQueue = nullptr;
    }
}

void play(Event event)
{
    const Melody melody = melodyFor(event);
    if (!melody.notes || !eventQueue) return;
    if (xQueueSend(eventQueue, &event, 0) != pdTRUE) {
        Serial.printf("[AUDIO] Dropped %s melody (queue busy)\n", melody.name);
        return;
    }
    Serial.printf("[AUDIO] %s melody queued\n", melody.name);
}

void startLoop(Event event)
{
    if (loopEnabled && loopEvent == event) return;
    const Melody melody = melodyFor(event);
    if (!melody.notes || !eventQueue) return;
    loopEvent = event;
    loopEnabled = true;
    ++audioGeneration;
    xQueueReset(eventQueue);
    if (xQueueSend(eventQueue, &event, 0) == pdTRUE)
        Serial.printf("[AUDIO] %s melody loop started\n", melody.name);
}

void stopLoop(Event event)
{
    if (!loopEnabled || loopEvent != event) return;
    loopEnabled = false;
    ++audioGeneration;
    if (eventQueue) xQueueReset(eventQueue);
    Serial.printf("[AUDIO] %s melody loop stopped\n", melodyFor(event).name);
}

void playAndWait(Event event)
{
    const Melody melody = melodyFor(event);
    if (!melody.notes || !buzzerLock) return;
    playMelody(event);
}

void silence()
{
    if (!buzzerLock) return;
    loopEnabled = false;
    ++audioGeneration;
    if (eventQueue) xQueueReset(eventQueue);
    xSemaphoreTake(buzzerLock, portMAX_DELAY);
    stopBuzzer();
    xSemaphoreGive(buzzerLock);
}
}
