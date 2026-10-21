#pragma once

// Host peripheral tests exercise the real callers with their existing Serial mock.
#if defined(TRACKER_LOG_HOST_TEST)
#define TrackerLog Serial
#else
#include <Arduino.h>
#include <debug_web.h>
#if TRACKER_WEB_DEBUG
class TrackerConsole : public Print {
public:
    using Print::write;
    size_t write(uint8_t byte) override;
    size_t write(const uint8_t *buffer, size_t length) override;
    void flush();
};
extern TrackerConsole TrackerLog;
#else
// No wrapper, buffers or Wi-Fi work in production/disabled builds.
#define TrackerLog Serial
#endif
#endif
