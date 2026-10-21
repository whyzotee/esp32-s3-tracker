#pragma once
#include <stdint.h>
#include <stddef.h>

// FPort 11: one-byte commands. State survives deep sleep, not battery removal.
namespace TripControl {
enum class Phase : uint8_t { Preparing, Waiting, Travelling };
enum class Preparation : uint8_t { Working, Ready, Failed };
struct Feedback {
    bool alarm, outside;
    Preparation preparation;
    bool notifyPreparation;
    uint8_t receipts;
    bool notifyCancellation;
};
void reset();
Phase phase();
bool initialFixReady();
void markInitialFix();
void markLoRaConnected();
void preparationFailed();
// Re-arm preparation feedback only at the next automatic retry after failure.
bool retryPreparation();
bool handleDownlink(uint8_t port, const uint8_t *data, size_t length);
bool endRequested();
bool offRoute();
void raiseAlarm();
bool alarmActive();
bool cancelAlarm();
bool takeCancellation();
bool wellbeingPending();
uint32_t revision();
void acknowledgeWellbeing(uint32_t sentRevision);
void setAcknowledged(bool acknowledged);
// Atomically consume one-shot notifications; no hardware/audio side effects.
Feedback takeFeedback();
bool keepAwake();
}
