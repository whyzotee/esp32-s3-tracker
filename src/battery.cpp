#include <tracker_log.h>
#include <Arduino.h>
#include <tracker_app.h>
#include <battery.h>
#include <board_pins.h>

namespace {
RTC_DATA_ATTR bool lowBattery = false;
constexpr uint8_t sampleCount = 16;
constexpr uint16_t minimumValidMv = 3000;
constexpr uint16_t maximumValidMv = 4400;

uint8_t percentage(uint16_t millivolts)
{
    // Typical resting Li-ion/LiPo discharge curve. The reported voltage is the
    // primary measurement; percentage is intentionally only an estimate.
    struct Point { uint16_t mv; uint8_t percent; };
    constexpr Point curve[] = {
        {4200, 100}, {4150, 95}, {4100, 90}, {4000, 75}, {3900, 55},
        {3800, 35}, {3700, 15}, {3600, 5}, {3300, 0},
    };
    if (millivolts >= curve[0].mv) return 100;
    if (millivolts <= curve[sizeof(curve) / sizeof(curve[0]) - 1].mv) return 0;
    for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
        if (millivolts >= curve[i].mv) {
            const Point high = curve[i - 1];
            const Point low = curve[i];
            return low.percent + uint32_t(millivolts - low.mv) *
                (high.percent - low.percent) / (high.mv - low.mv);
        }
    }
    return 0;
}
}

namespace Battery {
BatteryReading bootReading()
{
    // Ordinary RAM is reset after deep sleep, so every wake gets a new reading.
    static const BatteryReading reading = read();
    return reading;
}

BatteryReading read()
{
    // Arduino-ESP32 3.x registers the pin as an ADC channel on its first
    // analog read. Set the default attenuation first, then initialize before
    // applying the per-pin setting (the reverse order logs an ADC error).
    analogSetAttenuation(ADC_11db);
    analogReadMilliVolts(Board::batteryAdc);
    analogSetPinAttenuation(Board::batteryAdc, ADC_11db);

    // The divider has a high source impedance. Discard the first conversion and
    // average following readings so the ADC sample capacitor can settle.
    analogReadMilliVolts(Board::batteryAdc);
    uint32_t adcMvTotal = 0;
    for (uint8_t i = 0; i < sampleCount; ++i) {
        adcMvTotal += analogReadMilliVolts(Board::batteryAdc);
        delay(2);
    }
    const uint32_t adcMv = adcMvTotal / sampleCount;
    const uint32_t batteryMv = adcMv *
        (AppConfig::batteryTopResistorOhms + AppConfig::batteryBottomResistorOhms) /
        AppConfig::batteryBottomResistorOhms;

    BatteryReading result{};
    if (batteryMv < minimumValidMv || batteryMv > maximumValidMv) {
        TrackerLog.printf("[BAT] Invalid ADC reading: %lu mV at GPIO%d\n",
                      static_cast<unsigned long>(batteryMv), Board::batteryAdc);
        return result;
    }
    result.millivolts = static_cast<uint16_t>(batteryMv);
    result.percent = percentage(result.millivolts);
    result.valid = true;
    lowBattery = Battery::classifyLow(lowBattery, result.percent, result.valid);
    result.low = lowBattery;
    TrackerLog.printf("[BAT] %u mV (%u%%), ADC=%lu mV\n", result.millivolts,
                  result.percent, static_cast<unsigned long>(adcMv));
    TrackerLog.printf("[BAT] Low Battery: %s\n", result.low ? "ON" : "OFF");
    return result;
}
}
