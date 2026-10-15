#include <power_mode.h>
#include <tracker_app.h>
#include <esp_attr.h>

namespace {
RTC_DATA_ATTR uint32_t lowPowerMarker = 0;
constexpr uint32_t enabledMarker = 0x4C505731;
constexpr uint8_t enableCommand[] = {0x01, 0x00, 0x0F};
constexpr uint8_t disableCommand[] = {0x00, 0x00, 0x00};

bool matches(const uint8_t *data, const uint8_t (&command)[3])
{
    return data[0] == command[0] &&
           data[1] == command[1] &&
           data[2] == command[2];
}
}

namespace PowerMode {
bool enabled() { return lowPowerMarker == enabledMarker; }
uint32_t reportInterval()
{
    return enabled() ? AppConfig::lowPowerIntervalMs : AppConfig::reportIntervalMs;
}
bool handleDownlink(uint8_t port, const uint8_t *data, size_t length)
{
    if (port != 10 || data == nullptr || length != sizeof(enableCommand)) return false;

    if (matches(data, enableCommand)) {
        lowPowerMarker = enabledMarker;
        return true;
    }
    if (matches(data, disableCommand)) {
        lowPowerMarker = 0;
        return true;
    }
    return false;
}
}
