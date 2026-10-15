#pragma once
#include <stdint.h>

namespace DeepSleep {
void logWakeReason();
// Caller handles button actions before entering either sleep mode.
// Event retry shortening must not bypass a radio/backoff minimum.
[[noreturn]] void timed(uint32_t durationMs, uint32_t minimumMs = 0);
[[noreturn]] void powerOff();
}
