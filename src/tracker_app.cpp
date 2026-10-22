#include <tracker_log.h>
#include <Arduino.h>
#include <audio_policy.h>
#include <tracker_app.h>
#include <battery.h>
#include <debug_mode.h>
#include <debug_web.h>
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
#include <gnss.h>
#include <trip_control.h>

namespace {
void publishDebugState(bool force = false)
{
    if (!DebugWeb::active()) return;
    static uint32_t lastUpdate = 0;
    if (!force && uint32_t(millis() - lastUpdate) < 1000) return;
    lastUpdate = millis();
    const BatteryReading battery = Battery::bootReading();
    DebugWeb::State state;
    state.phase = uint8_t(TripControl::phase());
    state.gps = uint8_t(gnss_status());
    state.initialFix = TripControl::initialFixReady();
    state.alarm = TripControl::alarmActive();
    state.offRoute = TripControl::offRoute();
    state.lowPower = PowerMode::enabled();
    state.batteryValid = battery.valid;
    state.batteryMv = battery.millivolts;
    state.batteryPercent = battery.percent;
    state.radioWaitMs = lora_wait_ms();
    DebugWeb::publish(state);
}

void serviceGps()
{
    service_gnss();
    switch (gnss_status()) {
    case GnssStatus::FixReady: TripControl::markInitialFix(); break;
    case GnssStatus::Unavailable:
    case GnssStatus::TimedOut: TripControl::preparationFailed(); break;
    case GnssStatus::Searching: break;
    }
}

uint32_t reportInterval()
{
    if (TripControl::phase() != TripControl::Phase::Travelling)
        return AppConfig::reportIntervalMs;
    if (PowerMode::enabled()) return PowerMode::reportInterval();
    return DebugMode::enabled() ? DebugMode::interval() : PowerMode::reportInterval();
}

void serviceOta()
{
    if (DeviceButton::otaPending()) OtaManager::run(reportInterval());
}

void sleepFor(uint32_t durationMs, uint32_t minimumMs = 0)
{
    DeviceButton::service();
    serviceOta();
    AudioPolicy::refresh();
    const bool wouldSleep = !TripControl::keepAwake();
    if (wouldSleep && !DebugWeb::active()) DeepSleep::timed(durationMs, minimumMs);
    if (wouldSleep) DebugWeb::note("Report sleep simulated; Wi-Fi stays active");
    // Continuous sound needs an awake MCU. Never bypass the radio wait.
    const uint32_t waitMs = max(durationMs, minimumMs);
    TrackerLog.printf("[WAIT] Awake for %lu ms; trip/alarm/ACK feedback active\n",
                  static_cast<unsigned long>(waitMs));
    const uint32_t started = millis();
    uint32_t heartbeat = started;
    while (uint32_t(millis() - started) < waitMs) {
        DeviceButton::service();
        if (!DebugMode::enabled()) serviceGps();
        serviceOta();
        update_fall_detection(false);
        AudioPolicy::refresh();
        publishDebugState();
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
    TrackerLog.printf("[APP] Downlink: port=%u bytes=%u payload(hex)=",
                  result.downlinkPort, unsigned(result.downlinkLength));
    for (size_t i = 0; i < result.downlinkLength; ++i)
        TrackerLog.printf("%s%02X", i == 0 ? "" : " ", result.downlink[i]);
    TrackerLog.println();
    if (result.downlinkLength == 0) {
        TrackerLog.println("[APP] No application payload; no Low Power command applied");
        return;
    }
    if (TripControl::handleDownlink(result.downlinkPort, result.downlink, result.downlinkLength)) {
        if (result.downlink[0] == 7) {
            DeviceButton::acknowledgeSos();
            acknowledge_fall_detection();
            AudioPolicy::refresh();
            TrackerLog.println("[TRIP] SOS/Fall resolved by application; pending emergency reports cleared");
        }
        TrackerLog.printf("[TRIP] Command=%u phase=%u initial_fix=%s off_route=%s\n",
                      result.downlink[0], unsigned(TripControl::phase()),
                      TripControl::initialFixReady() ? "yes" : "no",
                      TripControl::offRoute() ? "yes" : "no");
        if (TripControl::endRequested()) DeviceButton::powerOff();
    } else if (PowerMode::handleDownlink(result.downlinkPort, result.downlink, result.downlinkLength)) {
        TrackerLog.printf("[APP] Low Power %s: interval %lu ms (RTC only)\n",
                      PowerMode::enabled() ? "enabled" : "disabled",
                      static_cast<unsigned long>(reportInterval()));
    } else {
        TrackerLog.printf("[APP] Ignored downlink: port=%u bytes=%u\n",
                      result.downlinkPort, unsigned(result.downlinkLength));
    }
}

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
    if (!debug) serviceGps();
    if (debug || (sample.flags & TelemetryFlags::gpsFresh) != 0)
        TripControl::markInitialFix();
    return sample;
}
}

namespace TrackerApp {
void runCycle()
{
    beginWatchdog(); // Idempotent: setup already armed it before peripheral init.
    DeviceButton::service();
    serviceOta();
    const bool debug = DebugMode::enabled();
    update_fall_detection(debug);
    DebugMode::update(fall_detection_pending());
    const bool retryingPreparation = TripControl::retryPreparation();
    TripControl::setAcknowledged(false); // A new work/TX cycle is in progress.
    AudioPolicy::refresh();
    publishDebugState(true);

    if (!debug) start_gnss_acquisition(retryingPreparation);
    if (!setup_lora_wan_app()) {
        DebugWeb::joined(false);
        DebugMode::message("LoRa init/join failed");
        TripControl::preparationFailed();
        return sleepFor(AppConfig::retryInitialMs);
    }
    TripControl::markLoRaConnected();
    DebugWeb::joined(true);
    if (!debug) serviceGps();
    publishDebugState(true);
    serviceOta();
    if (lora_wait_ms() > 0)
        return sleepFor(max(uint32_t(1000), lora_wait_ms()), lora_wait_ms());

    // Metadata uses a later legal TX slot, after the first primary ACK.
    // A pending SOS, fall or I-am-OK report always takes precedence.
    if (lora_device_info_due() && (nextStatus() == 0 || nextStatus() == 4)) {
        const BatteryReading battery = Battery::bootReading();
        Telemetry info{};
        info.batteryMv = battery.millivolts;
        if (battery.valid) info.flags |= TelemetryFlags::batteryValid;
        const LoRaResult result = send_lora_device_info(info);
        DebugWeb::transmission(result.code, result.acknowledged, AppConfig::deviceInfoPort);
        TripControl::setAcknowledged(result.acknowledged);
        handleDownlink(result);
        return sleepFor(max(reportInterval(), lora_wait_ms()), lora_wait_ms());
    }

    Telemetry sample = makeSample(nextStatus(), debug);

    // Acquisition can block. Capture events that arrived before transmitting.
    DeviceButton::service();
    update_fall_detection(false);
    serviceOta();
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
    TrackerLog.printf("[TX] LAT: %.6f, LON: %.6f, STATUS: %u\n",
                  sample.lat, sample.lon, sample.status);
    DebugWeb::telemetry(sample.lat, sample.lon, sample.status, sample.flags);
    publishDebugState(true);

    const uint32_t sentRevision = TripControl::revision();
    const LoRaResult result = send_lora_telemetry(sample);
    DebugWeb::transmission(result.code, result.acknowledged, AppConfig::uplinkPort);
    DebugMode::result(result.code);
    // For periodic status 0, local send completion is sufficient to resume sleep.
    // Keep the actual network ACK separate in diagnostics.
    TripControl::setAcknowledged(result.delivered(sample.status));
    handleDownlink(result);
    AudioPolicy::transmissionResult(result.delivered(sample.status));
    if (result.delivered(sample.status) && TripControl::revision() == sentRevision) {
        if (sample.status == 2) acknowledge_fall_detection();
        if (sample.status == 1) DeviceButton::acknowledgeSos();
        if (sample.status == 3) TripControl::acknowledgeWellbeing(sentRevision);
    }

    const bool retry = !result.delivered(sample.status) || !result.sessionSaved;
    // A saved MAC state permits retry at the next legal radio slot. Reserve the
    // longer backoff for init/storage failures rather than a missing uplink ACK.
    const uint32_t interval = retry
        ? (result.sessionSaved ? AppConfig::txRetryFloorMs : AppConfig::retryInitialMs)
        : reportInterval();
    const uint32_t radioWait = lora_wait_ms();
    const uint32_t minimumSleep = retry ? max(radioWait, interval) : radioWait;
    if (retry)
        TrackerLog.printf("[RETRY] Next attempt in %lu ms; radio_wait=%lu ms MAC_saved=%s\n",
                          static_cast<unsigned long>(minimumSleep),
                          static_cast<unsigned long>(radioWait),
                          result.sessionSaved ? "yes" : "no");
    sleepFor(max(interval, minimumSleep), minimumSleep);
}
}
