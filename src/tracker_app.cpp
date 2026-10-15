#include <Arduino.h>
#include <tracker_app.h>
#include <battery.h>
#include <board_pins.h>
#include <debug_mode.h>
#include <deep_sleep.h>
#include <device_button.h>
#include <fall_detection.h>
#if __has_include(<lorawan.h>)
#include <lorawan.h>
#else
#include <lorawan.example.h>
#endif
#include <ota_manager.h>
#include <power_mode.h>
#include <telemetry.h>

namespace {
uint32_t reportInterval()
{
    if (PowerMode::enabled()) return PowerMode::reportInterval();
    return DebugMode::enabled() ? DebugMode::interval() : PowerMode::reportInterval();
}

[[noreturn]] void startOtaIfRequested()
{
    if (DeviceButton::otaPending()) OtaManager::run(reportInterval());
    // Only called when an OTA request exists.
    while (true) delay(1000);
}

[[noreturn]] void sleepFor(uint32_t durationMs, uint32_t minimumMs = 0)
{
    DeviceButton::service();
    if (DeviceButton::otaPending()) startOtaIfRequested();
    DeepSleep::timed(durationMs, minimumMs);
}

void handleDownlink(const LoRaResult &result)
{
    if (result.code <= 0) return;
    Serial.printf("[APP] Downlink: port=%u bytes=%u payload(hex)=",
                  result.downlinkPort, unsigned(result.downlinkLength));
    for (size_t i = 0; i < result.downlinkLength; ++i)
        Serial.printf("%s%02X", i == 0 ? "" : " ", result.downlink[i]);
    Serial.println();
    if (result.downlinkLength == 0) {
        Serial.println("[APP] No application payload; no Low Power command applied");
        return;
    }
    if (PowerMode::handleDownlink(result.downlinkPort, result.downlink, result.downlinkLength)) {
        Serial.printf("[APP] Low Power %s: interval %lu ms (RTC only)\n",
                      PowerMode::enabled() ? "enabled" : "disabled",
                      static_cast<unsigned long>(reportInterval()));
    } else {
        Serial.printf("[APP] Ignored downlink: port=%u bytes=%u\n",
                      result.downlinkPort, unsigned(result.downlinkLength));
    }
}
}

namespace TrackerApp {
[[noreturn]] void runCycle()
{
    DeviceButton::service();
    if (DeviceButton::otaPending()) startOtaIfRequested();
    const bool debug = DebugMode::enabled();
    update_fall_detection(debug);
    DebugMode::update(fall_detection_pending());

    if (!setup_lora_wan_app()) {
        DebugMode::message("LoRa init/join failed");
        sleepFor(AppConfig::retryInitialMs);
    }
    if (DeviceButton::otaPending()) startOtaIfRequested();
    if (lora_wait_ms() > 0)
        sleepFor(max(uint32_t(1000), lora_wait_ms()), lora_wait_ms());

    const uint8_t status = fall_detection_pending() ? 2 : (DeviceButton::sosPending() ? 1 : 0);
    Telemetry sample = debug ? DebugMode::walking_sample(status) : read_telemetry(status);

    // Acquisition can block. Capture events that arrived before transmitting.
    update_fall_detection(false);
    DeviceButton::service();
    if (DeviceButton::otaPending()) startOtaIfRequested();
    // An event may arrive while normal telemetry is waiting for GNSS. Rebuild
    // from RTC-cached coordinates so the urgent packet cannot retain a 0,0
    // normal sample with only its status byte changed.
    if (fall_detection_pending() && sample.status != 2)
        sample = read_telemetry(2);
    else if (DeviceButton::sosPending() && sample.status != 1)
        sample = read_telemetry(1);

    if (PowerMode::enabled()) sample.flags |= TelemetryFlags::lowPowerMode;
    if (debug) sample.flags |= TelemetryFlags::debugSimulation;
    const BatteryReading battery = Battery::bootReading();
    sample.batteryMv = battery.millivolts;
    sample.batteryPercent = battery.percent;
    if (battery.valid) sample.flags |= TelemetryFlags::batteryValid;
    if (battery.valid && battery.low) sample.flags |= TelemetryFlags::lowBattery;
    // Log the final sample only. A button/fall event can occur while a normal
    // GPS sample is being acquired and replace it with an urgent packet.
    Serial.printf("[TX] LAT: %.6f, LON: %.6f, STATUS: %u\n",
                  sample.lat, sample.lon, sample.status);

    const LoRaResult result = send_lora_telemetry(sample);
    DebugMode::result(result.code);
    handleDownlink(result);
    if (result.delivered(sample.status)) {
        if (sample.status == 2) acknowledge_fall_detection();
        if (sample.status == 1) DeviceButton::acknowledgeSos();
    }

    const uint32_t interval = !result.delivered(sample.status) || !result.sessionSaved
        ? AppConfig::retryInitialMs : reportInterval();
    const uint32_t minimumSleep = max(lora_wait_ms(),
        result.delivered(sample.status) ? uint32_t(0) : AppConfig::retryInitialMs);
    sleepFor(max(interval, minimumSleep), minimumSleep);
}
}
