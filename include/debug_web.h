#pragma once
#include <stdint.h>
#include <stddef.h>

// Set to 1 here, or pass -DTRACKER_WEB_DEBUG=1 in PlatformIO build_flags.
// Disabled builds contain no debug HTTP server and never start its Wi-Fi AP.
#ifndef TRACKER_WEB_DEBUG
#define TRACKER_WEB_DEBUG 1
#endif

namespace DebugWeb {
struct State {
    uint8_t phase = 0; // TripControl::Phase
    uint8_t gps = 0;   // GnssStatus
    bool initialFix = false, alarm = false, offRoute = false, lowPower = false;
    bool batteryValid = false;
    uint16_t batteryMv = 0;
    uint8_t batteryPercent = 0;
    uint32_t radioWaitMs = 0;
};

#if TRACKER_WEB_DEBUG
// Development AP only. Change this password before use in a shared environment.
constexpr char apPassword[] = "TrackerDebug123";
static_assert(sizeof(apPassword) >= 9 && sizeof(apPassword) <= 64,
              "Debug AP password must contain 8-63 characters");
void begin();
void stop();
bool active();
void publish(const State &state);
void joined(bool success);
void transmission(int16_t code, bool acknowledged, uint8_t port);
void telemetry(float latitude, float longitude, uint8_t status, uint8_t flags);
void note(const char *message);
void recordSerial(const uint8_t *bytes, size_t length);
void gpsProgress(uint8_t status, uint32_t satellites, bool locationValid);
#else
inline void begin() {}
inline void stop() {}
inline bool active() { return false; }
inline void publish(const State &) {}
inline void joined(bool) {}
inline void transmission(int16_t, bool, uint8_t) {}
inline void telemetry(float, float, uint8_t, uint8_t) {}
inline void note(const char *) {}
inline void recordSerial(const uint8_t *, size_t) {}
inline void gpsProgress(uint8_t, uint32_t, bool) {}
#endif
}
