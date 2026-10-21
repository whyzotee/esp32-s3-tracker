#include <tracker_log.h>

#if TRACKER_WEB_DEBUG && !defined(TRACKER_LOG_HOST_TEST)
TrackerConsole TrackerLog;
size_t TrackerConsole::write(uint8_t byte) { return write(&byte, 1); }
size_t TrackerConsole::write(const uint8_t *buffer, size_t length)
{
    Serial.write(buffer, length);
    // Keep web logs available even with USB disconnected (Serial.write may
    // return zero). Capture never performs HTTP, allocates or waits for clients.
    DebugWeb::recordSerial(buffer, length);
    return length;
}
void TrackerConsole::flush() { Serial.flush(); }
#endif
