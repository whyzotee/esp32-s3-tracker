#pragma once

#include <stdint.h>
#include <app_config.h>

struct BatteryReading {
    uint16_t millivolts = 0;
    uint8_t percent = 0;
    bool valid = false;
    bool low = false; // Meaningful only when valid is true.
};

namespace Battery {
static_assert(AppConfig::lowBatteryEnterPercent < AppConfig::lowBatteryExitPercent,
              "Low battery recovery threshold must exceed entry threshold");
constexpr bool classifyLow(bool wasLow, uint8_t percent, bool valid)
{
    return !valid ? wasLow : (wasLow ? percent < AppConfig::lowBatteryExitPercent
                                   : percent <= AppConfig::lowBatteryEnterPercent);
}
BatteryReading read();
// Sample/log once per boot; reuse the wake-time measurement for telemetry.
BatteryReading bootReading();
}
