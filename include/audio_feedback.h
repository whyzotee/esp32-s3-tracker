#pragma once

// State loops run while the ESP32 is awake; trip/alarm states keep it awake.
// Board::prepareSleep() always silences the buzzer before sleep.
namespace AudioFeedback {
enum class Event : unsigned char {
    PowerOn,
    PowerOff,
    Sos,
    Fall,
    LoRaJoining,
    LoRaConnected,
    OtaMode,
    DeepSleep,
};

// Start the buzzer worker after Board::begin() has configured GPIO4.
void begin();

// Queue the event's distinct melody without blocking the tracker cycle.
void play(Event event);

// Repeat an event melody in the background until stopLoop() is called. This is
// used for preparation/no ACK, off-route and SOS/Fall alerts.
void startLoop(Event event);
void stopLoop(Event event);

// Use for events that immediately enter deep sleep, so the user can hear the
// complete melody before the buzzer rail is shut down.
void playAndWait(Event event);

// Cancels queued audio and returns GPIO4 to a normal LOW GPIO output.
// Board::prepareSleep() calls this before applying GPIO hold.
void silence();
}
