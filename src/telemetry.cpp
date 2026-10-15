#include <Arduino.h>
#include <firmware_version.h>
#include <gnss.h>
#include <telemetry.h>
#include <cstring>

extern int32_t rtc_lat;
extern int32_t rtc_lon;

namespace {
void useRtcLocation(Telemetry &sample)
{
    sample.lat = rtc_lat / 1e6f;
    sample.lon = rtc_lon / 1e6f;
}
}

Telemetry read_telemetry(uint8_t status)
{
    Telemetry sample{};
    sample.status = status;
    if (status == 1 || status == 2)
    {
        useRtcLocation(sample);
    }
    else
    {
        if (get_location())
        {
            sample.flags |= TelemetryFlags::gpsFresh;
            useRtcLocation(sample);
        }
        else
        {
            useRtcLocation(sample);
            Serial.println("[GPS] No fresh fix; using RTC cached location");
        }
    }

    return sample;
}

void encode_telemetry(const Telemetry &sample, uint8_t *payload)
{
    static_assert(sizeof(float) == 4, "Payload requires float32");
    memset(payload, 0, TELEMETRY_PAYLOAD_SIZE);
    payload[0] = sample.status;
    // ESP32 stores IEEE-754 floats in little-endian byte order.
    memcpy(payload + 1, &sample.lat, 4);
    memcpy(payload + 5, &sample.lon, 4);
    payload[9] = sample.flags;
    payload[10] = FirmwareVersion::major;
    payload[11] = FirmwareVersion::minor;
    payload[12] = sample.batteryMv & 0xFF;
    payload[13] = sample.batteryMv >> 8;
    payload[14] = sample.batteryPercent;
}
