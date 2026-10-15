#pragma once

#include <stdint.h>

struct BatteryReading {
    uint16_t millivolts = 0;
    uint8_t percent = 0;
    bool valid = false;
};

namespace Battery {
BatteryReading read();
// Sample/log once per boot; reuse the wake-time measurement for telemetry.
BatteryReading bootReading();
}
