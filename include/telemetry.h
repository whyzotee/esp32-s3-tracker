#pragma once
#include <stdint.h>
#include <stddef.h>

namespace TelemetryFlags {
constexpr uint8_t gpsFresh = 1 << 0;
constexpr uint8_t lowPowerMode = 1 << 1;
constexpr uint8_t debugSimulation = 1 << 2;
constexpr uint8_t batteryValid = 1 << 4;
constexpr uint8_t lowBattery = 1 << 5;
}

struct Telemetry {
    uint8_t status = 0;
    uint8_t flags = 0;
    float lat = 0, lon = 0;
    uint16_t batteryMv = 0;
    uint8_t batteryPercent = 0;
};
constexpr size_t TELEMETRY_PAYLOAD_SIZE = 15;
Telemetry read_telemetry(uint8_t status);
void encode_telemetry(const Telemetry &sample, uint8_t *payload);
