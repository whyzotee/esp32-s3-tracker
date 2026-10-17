#include <Arduino.h>
#include <audio_feedback.h>
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
#include <trip_control.h>
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
    if (TripControl::phase() != TripControl::Phase::Travelling)
        return AppConfig::reportIntervalMs;
    if (PowerMode::enabled()) return PowerMode::reportInterval();
    return DebugMode::enabled() ? DebugMode::interval() : PowerMode::reportInterval();
}

[[noreturn]] void startOtaIfRequested()
{
    if (DeviceButton::otaPending()) OtaManager::run(reportInterval());
    // Only called when an OTA request exists.
    while (true) delay(1000);
}

void sleepFor(uint32_t durationMs, uint32_t minimumMs = 0)
{
    DeviceButton::service();
    if (DeviceButton::otaPending()) startOtaIfRequested();
    TripControl::refreshAudio();
    if (!TripControl::keepAwake()) DeepSleep::timed(durationMs, minimumMs);
    // Continuous sound needs an awake MCU. Never bypass the radio wait.
    const uint32_t waitMs = max(durationMs, minimumMs);
    Serial.printf("[WAIT] Awake for %lu ms; trip/alarm/ACK feedback active\n",
                  static_cast<unsigned long>(waitMs));
    const uint32_t started = millis();
    uint32_t heartbeat = started;
    while (uint32_t(millis() - started) < waitMs) {
        DeviceButton::service();
        if (DeviceButton::otaPending()) startOtaIfRequested();
        update_fall_detection(false);
        TripControl::refreshAudio();
        // Renew only from the progressing main loop, never a timer callback.
        if (uint32_t(millis() - heartbeat) >= 60000) {
            TrackerApp::endWatchdog();
            TrackerApp::beginWatchdog();
            heartbeat = millis();
        }
        delay(10);
    }
    TrackerApp::endWatchdog();
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
    if (TripControl::handleDownlink(result.downlinkPort, result.downlink, result.downlinkLength)) {
        Serial.printf("[TRIP] Command=%u phase=%u initial_fix=%s off_route=%s\n",
                      result.downlink[0], unsigned(TripControl::phase()),
                      TripControl::initialFixReady() ? "yes" : "no",
                      TripControl::offRoute() ? "yes" : "no");
        if (TripControl::endRequested()) DeviceButton::powerOff();
    } else if (PowerMode::handleDownlink(result.downlinkPort, result.downlink, result.downlinkLength)) {
        Serial.printf("[APP] Low Power %s: interval %lu ms (RTC only)\n",
                      PowerMode::enabled() ? "enabled" : "disabled",
                      static_cast<unsigned long>(reportInterval()));
    } else {
        Serial.printf("[APP] Ignored downlink: port=%u bytes=%u\n",
                      result.downlinkPort, unsigned(result.downlinkLength));
    }
}
}

namespace {
uint8_t nextStatus()
{
    if (fall_detection_pending()) return 2;
    if (DeviceButton::sosPending()) return 1;
    if (TripControl::wellbeingPending()) return 3;
    return TripControl::phase() == TripControl::Phase::Travelling ? 0 : 4;
}

Telemetry makeSample(uint8_t status, bool debug)
{
    if (status == 4 && TripControl::initialFixReady() && !debug) {
        Telemetry sample = read_telemetry(3); // Cached coordinates, no fix wait.
        sample.status = 4;
        return sample;
    }
    Telemetry sample = debug ? DebugMode::walking_sample(status) : read_telemetry(status);
    if (debug || (sample.flags & TelemetryFlags::gpsFresh) != 0)
        TripControl::markInitialFix();
    return sample;
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

void runCycle()
{
    beginWatchdog(); // Idempotent: setup already armed it before peripheral init.
    DeviceButton::service();
    if (DeviceButton::otaPending()) startOtaIfRequested();
    const bool debug = DebugMode::enabled();
    update_fall_detection(debug);
    DebugMode::update(fall_detection_pending());
    TripControl::setAcknowledged(false); // A new work/TX cycle is in progress.
    TripControl::refreshAudio();

    if (!setup_lora_wan_app()) {
        DebugMode::message("LoRa init/join failed");
        TripControl::setAcknowledged(false);
        return sleepFor(AppConfig::retryInitialMs);
    }
    if (DeviceButton::otaPending()) startOtaIfRequested();
    if (lora_wait_ms() > 0)
        return sleepFor(max(uint32_t(1000), lora_wait_ms()), lora_wait_ms());

    Telemetry sample = makeSample(nextStatus(), debug);

    // Acquisition can block. Capture events that arrived before transmitting.
    DeviceButton::service();
    update_fall_detection(false);
    if (DeviceButton::otaPending()) startOtaIfRequested();
    // An event may arrive while normal telemetry is waiting for GNSS. Rebuild
    // from RTC-cached coordinates so the urgent packet cannot retain a 0,0
    // normal sample with only its status byte changed.
    if (sample.status == 4 && nextStatus() == 0)
        sample.status = 0; // Initial fix completed a previously queued Start.
    else if (nextStatus() != sample.status)
        sample = makeSample(nextStatus(), debug);

    if (PowerMode::enabled()) sample.flags |= TelemetryFlags::lowPowerMode;
    if (debug) sample.flags |= TelemetryFlags::debugSimulation;
    if (TripControl::initialFixReady()) sample.flags |= TelemetryFlags::initialFixReady;
    if (TripControl::phase() == TripControl::Phase::Travelling)
        sample.flags |= TelemetryFlags::tripActive;
    if (TripControl::offRoute()) sample.flags |= TelemetryFlags::offRoute;
    const BatteryReading battery = Battery::bootReading();
    sample.batteryMv = battery.millivolts;
    sample.batteryPercent = battery.percent;
    if (battery.valid) sample.flags |= TelemetryFlags::batteryValid;
    if (battery.valid && battery.low) sample.flags |= TelemetryFlags::lowBattery;
    // Log the final sample only. A button/fall event can occur while a normal
    // GPS sample is being acquired and replace it with an urgent packet.
    Serial.printf("[TX] LAT: %.6f, LON: %.6f, STATUS: %u\n",
                  sample.lat, sample.lon, sample.status);

    const uint32_t sentRevision = TripControl::revision();
    const LoRaResult result = send_lora_telemetry(sample);
    DebugMode::result(result.code);
    TripControl::setAcknowledged(result.acknowledged);
    if (result.acknowledged && !TripControl::alarmActive() && !TripControl::offRoute()) {
        AudioFeedback::silence();
        AudioFeedback::playAndWait(AudioFeedback::Event::LoRaConnected);
    }
    handleDownlink(result);
    if (result.delivered(sample.status) && TripControl::revision() == sentRevision) {
        if (sample.status == 2) acknowledge_fall_detection();
        if (sample.status == 1) DeviceButton::acknowledgeSos();
        if (sample.status == 3) TripControl::acknowledgeWellbeing(sentRevision);
    }

    const uint32_t interval = !result.delivered(sample.status) || !result.sessionSaved
        ? AppConfig::retryInitialMs : reportInterval();
    const uint32_t minimumSleep = max(lora_wait_ms(),
        result.delivered(sample.status) ? uint32_t(0) : AppConfig::retryInitialMs);
    sleepFor(max(interval, minimumSleep), minimumSleep);
}
}
