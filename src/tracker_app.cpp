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
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <mutex>

static_assert(CONFIG_ESP_TASK_WDT_EN && CONFIG_ESP_TASK_WDT_INIT && CONFIG_ESP_TASK_WDT_PANIC,
              "Tracking requires the framework hardware Task WDT with panic/reboot enabled");
static_assert(CONFIG_ESP_TASK_WDT_TIMEOUT_S >= 3,
              "Tracking heartbeat needs a Task WDT timeout of at least 3 seconds");
static_assert(AppConfig::trackingWatchdogMs > AppConfig::firstGpsTimeoutMs,
              "Tracking watchdog must allow first GNSS acquisition");

namespace {
esp_timer_handle_t trackingWatchdog = nullptr;
esp_task_wdt_user_handle_t trackingUser = nullptr;
int64_t trackingStartedUs = 0;
std::mutex trackingWatchdogMutex;

void feedTrackingWatchdog(void *)
{
    // Synchronize with deletion on the Arduino task; no print/flush in callback.
    const std::lock_guard<std::mutex> lock(trackingWatchdogMutex);
    if (trackingUser != nullptr && esp_timer_get_time() - trackingStartedUs <
        int64_t(AppConfig::trackingWatchdogMs) * 1000) {
        ESP_ERROR_CHECK(esp_task_wdt_reset_user(trackingUser));
    }
    // Past the deadline, or if this timer task stalls, no heartbeat reaches
    // the hardware-backed Task WDT. Its normal timeout/panic triggers recovery.
}

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
void beginWatchdog()
{
    const std::lock_guard<std::mutex> lock(trackingWatchdogMutex);
    if (trackingWatchdog != nullptr) return;
    esp_timer_create_args_t config{};
    config.callback = feedTrackingWatchdog;
    config.dispatch_method = ESP_TIMER_TASK;
    config.name = "tracker-wdt-feed";
    ESP_ERROR_CHECK(esp_timer_create(&config, &trackingWatchdog));
    trackingStartedUs = esp_timer_get_time();
    ESP_ERROR_CHECK(esp_task_wdt_add_user("tracker-cycle", &trackingUser));
    ESP_ERROR_CHECK(esp_timer_start_periodic(trackingWatchdog, 1000000));
    Serial.printf("[WDT] Hardware Task WDT armed; tracking budget: %lu ms\n",
                  static_cast<unsigned long>(AppConfig::trackingWatchdogMs));
    // Never extend trackingStartedUs from acquisition/radio loops.
}

void endWatchdog()
{
    const std::lock_guard<std::mutex> lock(trackingWatchdogMutex);
    if (trackingWatchdog == nullptr) return;
    const esp_err_t state = esp_timer_stop(trackingWatchdog);
    if (state != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(state);
    ESP_ERROR_CHECK(esp_timer_delete(trackingWatchdog));
    trackingWatchdog = nullptr;
    ESP_ERROR_CHECK(esp_task_wdt_delete_user(trackingUser));
    trackingUser = nullptr;
}

[[noreturn]] void runCycle()
{
    beginWatchdog(); // Idempotent: setup already armed it before peripheral init.
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
