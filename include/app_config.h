#pragma once
#include <stdint.h>

namespace AppConfig {
constexpr uint32_t reportIntervalMs = 1 * 60 * 1000;
constexpr uint32_t lowPowerIntervalMs = 15 * 60 * 1000;
constexpr uint32_t retryInitialMs = 60 * 1000;
constexpr uint32_t eventRetryMs = 15 * 1000;
constexpr uint32_t firstGpsTimeoutMs = 120 * 1000;
constexpr uint32_t gpsTimeoutMs = 30 * 1000;
constexpr uint32_t batteryTopResistorOhms = 750000;
constexpr uint32_t batteryBottomResistorOhms = 360000;
constexpr uint8_t uplinkPort = 2;
constexpr uint8_t uplinkDataRate = 2; // SF10
}
