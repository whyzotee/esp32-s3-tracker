#pragma once
#include <stdint.h>

namespace AppConfig {
constexpr uint32_t reportIntervalMs = 1 * 60 * 1000;
constexpr uint32_t lowPowerIntervalMs = 15 * 60 * 1000;
constexpr uint32_t retryInitialMs = 60 * 1000;
constexpr uint32_t eventRetryMs = 15 * 1000;
constexpr uint32_t firstGpsTimeoutMs = 120 * 1000;
constexpr uint32_t gpsTimeoutMs = 30 * 1000;
constexpr uint8_t gpsMinSatellites = 6;
constexpr uint16_t gpsMaxHdopHundredths = 200; // HDOP <= 2.00, not an accuracy in metres.
constexpr uint8_t gpsRequiredFixes = 3; // Distinct one-second GGA epochs.
constexpr uint32_t gpsMaxFixAgeMs = 2000;
constexpr double gpsMaxTrackingSpeedMps = 15.0;
constexpr double gpsJumpToleranceM = 20.0;
constexpr uint32_t batteryTopResistorOhms = 750000;
constexpr uint32_t batteryBottomResistorOhms = 360000;
constexpr uint8_t lowBatteryEnterPercent = 15;
constexpr uint8_t lowBatteryExitPercent = 20;
constexpr uint8_t uplinkPort = 2;
constexpr uint8_t uplinkDataRate = 2; // SF10
}
