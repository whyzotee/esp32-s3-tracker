#include <Arduino.h>
#include <trip_control.h>

namespace {
RTC_DATA_ATTR bool initialFix = false;
RTC_DATA_ATTR bool startRequested = false;
RTC_DATA_ATTR bool finishRequested = false;
RTC_DATA_ATTR bool outsideRoute = false;
RTC_DATA_ATTR bool alarmLatched = false;
RTC_DATA_ATTR bool wellbeing = false;
RTC_DATA_ATTR bool lastAcknowledged = false;
RTC_DATA_ATTR uint32_t generation = 0;
using TripControl::Preparation;
uint8_t pendingReceipts = 0;
RTC_DATA_ATTR Preparation preparation = Preparation::Working;
RTC_DATA_ATTR bool loraConnected = false;
RTC_DATA_ATTR bool preparationNotified = false;
bool cancellation = false;
bool cancellationFeedback = false;
portMUX_TYPE tripLock = portMUX_INITIALIZER_UNLOCKED;

struct Guard {
    Guard() { portENTER_CRITICAL(&tripLock); }
    ~Guard() { portEXIT_CRITICAL(&tripLock); }
};
}

namespace TripControl {
void reset() {
    Guard guard;
    initialFix = startRequested = finishRequested = outsideRoute = false;
    alarmLatched = wellbeing = cancellation = lastAcknowledged = false;
    preparation = Preparation::Working;
    loraConnected = preparationNotified = false;
    pendingReceipts = 0;
    cancellationFeedback = false;
    ++generation;
}

Phase phase() {
    Guard guard;
    return !initialFix ? Phase::Preparing
        : (startRequested ? Phase::Travelling : Phase::Waiting);
}

bool initialFixReady() { Guard guard; return initialFix; }
void markInitialFix() {
    Guard guard;
    initialFix = true;
    if (loraConnected && preparation == Preparation::Working) preparation = Preparation::Ready;
}

void markLoRaConnected() {
    Guard guard;
    loraConnected = true;
    if (initialFix && preparation == Preparation::Working) preparation = Preparation::Ready;
}

void preparationFailed() {
    Guard guard;
    if (preparation == Preparation::Working) preparation = Preparation::Failed;
}

bool retryPreparation() {
    Guard guard;
    if (preparation != Preparation::Failed) return false;
    preparation = Preparation::Working;
    preparationNotified = false;
    loraConnected = false;
    return true;
}

bool handleDownlink(uint8_t port, const uint8_t *data, size_t length) {
    if (port != 11 || data == nullptr || length != 1) return false;
    if (data[0] == 5 || data[0] == 6) {
        // Application receipt is separate from the network ACK. Queue a success
        // suffix after the current alarm phrase without stopping its loop or
        // clearing pending delivery. Ignore late receipts after cancellation.
        Guard guard;
        if (alarmLatched && pendingReceipts < 4) ++pendingReceipts;
        return true;
    }
    Guard guard;
    switch (data[0]) {
    case 1: startRequested = true; return true;
    case 2: finishRequested = true; return true;
    case 3:
        if (!initialFix || !startRequested) return false;
        outsideRoute = true; return true;
    case 4: outsideRoute = false; return true;
    default: return false;
    }
}
bool endRequested() { Guard guard; return finishRequested; }
bool offRoute() { Guard guard; return outsideRoute; }
void raiseAlarm() {
    Guard guard;
    alarmLatched = true;
    // A newer emergency supersedes an unsent OK from an older cancellation.
    // Never send "I am OK" for a newly active alarm that has not been cancelled.
    wellbeing = false;
    cancellationFeedback = false;
    ++generation;
}
bool alarmActive() { Guard guard; return alarmLatched; }
bool cancelAlarm() {
    Guard guard;
    if (!alarmLatched) return false;
    alarmLatched = false;
    pendingReceipts = 0;
    wellbeing = cancellation = true;
    cancellationFeedback = true;
    ++generation;
    return true;
}
bool takeCancellation() {
    Guard guard;
    const bool value = cancellation;
    cancellation = false;
    return value;
}
bool wellbeingPending() { Guard guard; return wellbeing; }
uint32_t revision() { Guard guard; return generation; }
void acknowledgeWellbeing(uint32_t sentRevision) {
    Guard guard;
    if (generation == sentRevision) wellbeing = false;
}
void setAcknowledged(bool acknowledged) { Guard guard; lastAcknowledged = acknowledged; }
bool keepAwake() {
    Guard guard;
    return !initialFix || !startRequested || outsideRoute || alarmLatched ||
        wellbeing || !lastAcknowledged;
}
Feedback takeFeedback() {
    Guard guard;
    const bool notify = !alarmLatched && !outsideRoute &&
        preparation != Preparation::Working && !preparationNotified;
    if (notify) preparationNotified = true;
    const Feedback feedback{alarmLatched, outsideRoute, preparation, notify,
                            uint8_t(alarmLatched ? pendingReceipts : 0),
                            cancellationFeedback};
    pendingReceipts = 0;
    cancellationFeedback = false;
    return feedback;
}
}
