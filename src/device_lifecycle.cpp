#include <tracker_log.h>
#include <Arduino.h>
#include <atomic>
#include <device_lifecycle.h>
#include <debug_web.h>
#include <audio_feedback.h>
#include <board_pins.h>
#include <deep_sleep.h>
#include <trip_control.h>
#include <fall_detection.h>

namespace {
std::atomic<bool> stopping{false};
}

namespace DeviceLifecycle {
bool shuttingDown() { return stopping.load(); }

[[noreturn]] void powerOff()
{
    stopping = true;
    // Close the audio admission gate before resetting state: preparation reset
    // must not restart its working melody during shutdown.
    AudioFeedback::shutdown();
    DebugWeb::stop();
    TripControl::reset();
    acknowledge_fall_detection();
    TrackerLog.println("[POWER] Off selected; release button to finish shutdown");
    // EXT0 is level-triggered: entering sleep while held would wake immediately.
    while (digitalRead(Board::button) == LOW) delay(10);
    delay(40);
    DeepSleep::powerOff();
}
}
