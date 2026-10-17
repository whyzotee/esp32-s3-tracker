#pragma once
#include <stdint.h>
#include <stddef.h>

// FPort 11: one-byte commands. State survives deep sleep, not battery removal.
namespace TripControl {
enum class Phase : uint8_t { Preparing, Waiting, Travelling };
void reset();
Phase phase();
bool initialFixReady();
void markInitialFix();
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
// Feedback priority: SOS/Fall > off-route > preparing/waiting/no ACK.
void refreshAudio();
bool keepAwake();
}
