#include <tracker_log.h>
#include <Arduino.h>
#include <math.h>
#include <debug_mode.h>
#include <sys/time.h>

static uint64_t simulationClockMs()
{
    timeval time{};
    gettimeofday(&time, nullptr); // ESP32 RTC-backed time includes deep sleep.
    return uint64_t(time.tv_sec) * 1000 + time.tv_usec / 1000;
}

namespace {
bool active = false;
uint32_t sendInterval = 15000;
bool lastFallPending = false;
}

namespace DebugMode {
void begin(bool enabled, uint32_t intervalMs)
{
    active = enabled;
    sendInterval = max(intervalMs, uint32_t(1000));
    if (active) {
        TrackerLog.printf("[DEBUG] Initialized (interval: %lu ms, monitor only)\n", (unsigned long)sendInterval);
    }
}

bool enabled() { return active; }
uint32_t interval() { return sendInterval; }

Telemetry walking_sample(uint8_t status)
{
    Telemetry sample{};
    sample.status = status;
    // Synthetic starting point, not a GPS fix. Continue a walking track.
    RTC_DATA_ATTR static double walkingLat = 13.7563;
    RTC_DATA_ATTR static double walkingLon = 100.5018;
    RTC_DATA_ATTR static double heading = 0.0;
    RTC_DATA_ATTR static uint64_t lastStep = 0;
    RTC_DATA_ATTR static bool started = false;
    uint64_t now = simulationClockMs();
    if (started)
    {
        double seconds = now >= lastStep ? (now - lastStep) / 1000.0 : 0;
        double speed = 0.9 + (esp_random() % 601) / 1000.0; // 0.9–1.5 m/s
        heading += (int(esp_random() % 6101) - 3050) / 100.0 * DEG_TO_RAD;
        double distance = speed * seconds;
        walkingLat += distance * cos(heading) / 111320.0;
        walkingLon += distance * sin(heading) / (111320.0 * cos(walkingLat * DEG_TO_RAD));
    }
    started = true;
    lastStep = now;
    sample.lat = walkingLat;
    sample.lon = walkingLon;
    TrackerLog.printf("[SIMULATED WALK] LAT: %.6f, LON: %.6f, STATUS: %u\n",
                  sample.lat, sample.lon, sample.status);
    return sample;
}

void message(const char *message)
{
    if (!active) return;
    TrackerLog.printf("[DEBUG] %s\n", message);
}

void result(int16_t code)
{
    if (!active) return;
    TrackerLog.printf("[DEBUG] Radio result: %d (%s)\n", code,
                  code < 0 ? "TX/RX error" : (code == 0 ? "Sent / no downlink" : "Downlink received"));
}

void update(bool fallPending)
{
    if (!active) return;
    if (fallPending != lastFallPending) {
        lastFallPending = fallPending;
        TrackerLog.printf("[DEBUG] Fall pending: %s\n", fallPending ? "YES" : "NO");
    }
}
}
