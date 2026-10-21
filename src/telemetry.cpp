#include <tracker_log.h>
#include <Arduino.h>
#include <tracker_app.h>
#include <gnss.h>
#include <telemetry.h>
#include <cmath>

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
    if (status == 1 || status == 2 || status == 3)
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
            TrackerLog.println("[GPS] No fresh fix; using RTC cached location");
        }
    }

    return sample;
}

void encode_telemetry(const Telemetry &sample, uint8_t *payload)
{
    // Signed normalized int24 LE: +/-90 latitude, +/-180 longitude.
    // Reserve -8388608 for invalid coordinates; valid values use +/-8388607.
    const auto coordinate = [](double degrees, double range, uint8_t *out) {
        const int32_t value = std::isfinite(degrees) && std::abs(degrees) <= range
            ? int32_t(std::lround(degrees * 8388607.0 / range)) : -8388608;
        const uint32_t bits = uint32_t(value);
        out[0] = bits & 0xff;
        out[1] = (bits >> 8) & 0xff;
        out[2] = (bits >> 16) & 0xff;
    };
    payload[0] = sample.status;
    coordinate(sample.lat, 90.0, payload + 1);
    coordinate(sample.lon, 180.0, payload + 4);
    payload[7] = sample.flags;
    payload[8] = sample.batteryPercent;
}

void encode_device_info(const Telemetry &sample, uint8_t *payload)
{
    payload[0] = FirmwareVersion::major;
    payload[1] = FirmwareVersion::minor;
    const uint16_t millivolts = (sample.flags & TelemetryFlags::batteryValid) ? sample.batteryMv : 0;
    payload[2] = millivolts & 0xff;
    payload[3] = millivolts >> 8;
}
