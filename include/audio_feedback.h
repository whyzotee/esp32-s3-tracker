#pragma once

// State loops run while the ESP32 is awake; trip/alarm states keep it awake.
// Board::prepareSleep() closes audio admission and silences the buzzer.
namespace AudioFeedback {
enum class Event : unsigned char {
    PowerOn,
    PowerOff,
    Sos,
    Fall,
    LoRaJoining,
    LoRaConnected,
    Error,
    OtaMode,
    DeepSleep,
};

// Start the buzzer worker after Board::begin() has configured GPIO4.
void begin();

// Queue the event's distinct melody without blocking the tracker cycle.
void play(Event event);

// Repeat an event melody in the background until stopLoop() is called. This is
// used for active preparation, off-route and SOS/Fall alerts.
void startLoop(Event event);
void stopLoop(Event event);

// Replace prior work and wait for the worker to finish the confirmation.
void playAndWait(Event event);

// Cancels queued audio and returns GPIO4 to a normal LOW GPIO output.
// Unlike prepareSleep(), this permits later audio commands.
void silence();

// Terminal barrier: cancel all prior work, play PowerOff once, then reject
// further melodies until reboot. Returns only when LEDC is detached/pin LOW.
void shutdown();
// Terminal silent barrier used immediately before applying GPIO sleep holds.
void prepareSleep();
}
