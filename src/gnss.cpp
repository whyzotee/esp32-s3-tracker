#include <tracker_log.h>
#include <gnss.h>
#include <debug_web.h>
#include <TinyGPS++.h>
#include <device_button.h>
#include <tracker_app.h>
#include <fall_detection.h>
#include <trip_control.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

RTC_DATA_ATTR int32_t rtc_lat = 0;
RTC_DATA_ATTR int32_t rtc_lon = 0;

namespace {
TinyGPSPlus GPS;
bool echoRawNmea = false;
RTC_DATA_ATTR bool hasPreviousFix = false;
TinyGPSCustom gpFixQuality(GPS, "GPGGA", 6);
TinyGPSCustom gnFixQuality(GPS, "GNGGA", 6);

// LED2 is active-high. Only the GPS worker controls its search blink.
void setGnssSearchLed(uint32_t started, bool searching)
{
    digitalWrite(Board::statusLed, searching && ((millis() - started) / 500U) % 2U != 0U
        ? LOW : HIGH);
}

struct FixStability {
    const char *reason = "waiting-GGA";
    char quality = '?';
    uint32_t lastEpoch = UINT32_MAX;
    uint32_t lastAt = 0;
    uint32_t firstAt = 0;
    uint8_t count = 0;
    double latitude = 0;
    double longitude = 0;

    bool observe()
    {
        // Evaluate only complete, checksum-valid GGA sentences. An RMC can
        // update location before satellites/HDOP are available for that epoch.
        TinyGPSCustom *quality = gpFixQuality.isUpdated() ? &gpFixQuality
            : (gnFixQuality.isUpdated() ? &gnFixQuality : nullptr);
        if (!quality) return false;
        const char fixType = quality->value()[0];
        this->quality = fixType;
        const uint32_t epoch = GPS.time.value() / 100; // HHMMSS, discard centiseconds.
        const uint32_t now = millis();
        const double lat = GPS.location.lat();
        const double lon = GPS.location.lng();
        const double hdop = GPS.hdop.hdop();
        const uint32_t satellites = GPS.satellites.value();
        const bool good = (fixType == '1' || fixType == '2') &&
            GPS.time.isValid() && GPS.location.isValid() &&
            GPS.location.age() <= AppConfig::gpsMaxFixAgeMs &&
            GPS.hdop.isValid() && GPS.hdop.age() <= AppConfig::gpsMaxFixAgeMs &&
            hdop > 0 && hdop <= AppConfig::gpsMaxHdopHundredths / 100.0 &&
            GPS.satellites.isValid() &&
            GPS.satellites.age() <= AppConfig::gpsMaxFixAgeMs &&
            satellites >= AppConfig::gpsMinSatellites;
        if (!good) {
            reason = fixType != '1' && fixType != '2' ? "fix-type" :
                !GPS.time.isValid() ? "invalid-time" :
                !GPS.location.isValid() ? "invalid-location" :
                GPS.location.age() > AppConfig::gpsMaxFixAgeMs ? "stale-location" :
                !GPS.hdop.isValid() ? "invalid-HDOP" :
                GPS.hdop.age() > AppConfig::gpsMaxFixAgeMs ? "stale-HDOP" :
                hdop <= 0 || hdop > AppConfig::gpsMaxHdopHundredths / 100.0 ? "HDOP-too-high" :
                !GPS.satellites.isValid() ? "invalid-satellites" :
                GPS.satellites.age() > AppConfig::gpsMaxFixAgeMs ? "stale-satellites" :
                "too-few-satellites";
            count = 0;
            lastEpoch = epoch;
            return false;
        }
        if (epoch == lastEpoch) return false; // GP/GN must not double-count an epoch.
        reason = "waiting-stable-fixes";
        if (count > 0) {
            const uint32_t gap = now - lastAt;
            const double allowed = AppConfig::gpsJumpToleranceM +
                AppConfig::gpsMaxTrackingSpeedMps * gap / 1000.0;
            if (gap > AppConfig::gpsMaxFixAgeMs ||
                TinyGPSPlus::distanceBetween(latitude, longitude, lat, lon) > allowed) {
                count = 0;
                reason = gap > AppConfig::gpsMaxFixAgeMs ? "fix-gap" : "position-jump";
            }
        }
        lastEpoch = epoch;
        lastAt = now;
        if (count == 0) firstAt = now;
        latitude = lat;
        longitude = lon;
        if (count < AppConfig::gpsRequiredFixes) ++count;
        // Buffered UART sentences must not qualify as several seconds of tracking.
        return count >= AppConfig::gpsRequiredFixes &&
            now - firstAt >= uint32_t(AppConfig::gpsRequiredFixes - 1) * 1000;
    }
};

// Only the GPS task reads Serial1 or accesses TinyGPS++. The main task owns
// RTC coordinates and trip state; snapshots/commands cross tasks via queues.
struct GnssCommand {
    uint32_t generation;
    uint32_t started;
    uint32_t timeout;
    bool search;
};
struct GnssSnapshot {
    uint32_t generation = 0;
    uint32_t fixAt = 0;
    uint32_t fixSequence = 0;
    int32_t latitude = 0;
    int32_t longitude = 0;
    bool qualified = false;
    bool complete = false;
};
TaskHandle_t gnssTask = nullptr;
QueueHandle_t gnssCommands = nullptr;
QueueHandle_t gnssSnapshots = nullptr;
SemaphoreHandle_t gnssStopped = nullptr;
constexpr uint32_t stopTaskBit = 1;
bool gnssRunning = false; // Main-task lifecycle state only.
bool acquiring = false;
uint32_t acquisitionStart = 0;
uint32_t acquisitionTimeout = 0;
uint32_t acquisitionGeneration = 0;
uint32_t consumedFixSequence = 0;

// Worker-only logging. No second logger or parser access on the main task.
void logGnssStatus(uint32_t elapsedMs, const FixStability &stability)
{
    TrackerLog.printf("[GPS] elapsed=%lus chars=%lu checksum_ok=%lu checksum_fail=%lu "
                  "satellites=%lu location=%s HDOP=%.2f fix_type=%c stable=%u/%u reason=%s\n",
                  static_cast<unsigned long>(elapsedMs / 1000),
                  static_cast<unsigned long>(GPS.charsProcessed()),
                  static_cast<unsigned long>(GPS.passedChecksum()),
                  static_cast<unsigned long>(GPS.failedChecksum()),
                  static_cast<unsigned long>(GPS.satellites.isValid() ? GPS.satellites.value() : 0),
                  GPS.location.isValid() ? "valid" : "invalid",
                  GPS.hdop.isValid() ? GPS.hdop.hdop() : 0.0,
                  stability.quality, stability.count, AppConfig::gpsRequiredFixes, stability.reason);
}

void logGnssTimeout()
{
    if (GPS.charsProcessed() == 0)
        TrackerLog.println("[GPS] Timeout: no UART data; check VEXT, L76L power, GPIO41 and baud rate");
    else if (GPS.passedChecksum() == 0)
        TrackerLog.println("[GPS] Timeout: no valid NMEA; check baud rate and signal integrity");
    else
        TrackerLog.println("[GPS] Timeout: no fix met quality/stability requirements; check antenna and sky view");
}

void gnssWorker(void *)
{
    FixStability stability;
    GnssCommand command{};
    GnssSnapshot snapshot;
    uint32_t lastPublish = millis();
    uint32_t lastStatus = millis();
    bool outcomeLogged = false;
    for (;;) {
        uint32_t notifications = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notifications, 0);
        if ((notifications & stopTaskBit) != 0) break;
        GnssCommand next;
        if (xQueueReceive(gnssCommands, &next, 0) == pdTRUE) {
            command = next;
            if (command.search) {
                stability = FixStability{};
                snapshot = GnssSnapshot{};
                snapshot.generation = command.generation;
                lastStatus = command.started;
                outcomeLogged = false;
                TrackerLog.printf("[GPS] Acquisition timeout=%lu ms; GPS task active\n",
                              static_cast<unsigned long>(command.timeout));
            }
        }
        const bool searching = command.search &&
            uint32_t(millis() - command.started) < command.timeout;
        snapshot.complete = command.search && !searching;
        // Bounded drain plus a real RTOS block leaves idle/radio tasks runnable.
        // Continue draining even outside a search to avoid stale UART backlogs.
        for (unsigned bytes = 0; bytes < 128 && Serial1.available(); ++bytes) {
            const char byte = static_cast<char>(Serial1.read());
            if (echoRawNmea) TrackerLog.write(byte);
            if (GPS.encode(byte) && searching && stability.observe()) {
                snapshot.latitude = stability.latitude * 1e6;
                snapshot.longitude = stability.longitude * 1e6;
                snapshot.fixAt = millis();
                ++snapshot.fixSequence;
                snapshot.qualified = true;
            }
        }
        setGnssSearchLed(command.started, searching && !snapshot.qualified);
        if (uint32_t(millis() - lastPublish) >= 20) {
            xQueueOverwrite(gnssSnapshots, &snapshot);
#if TRACKER_WEB_DEBUG
            // The GPS owner can publish progress even while the main task joins.
            DebugWeb::gpsProgress(uint8_t(snapshot.qualified ? GnssStatus::FixReady :
                snapshot.complete ? GnssStatus::TimedOut : GnssStatus::Searching),
                GPS.satellites.isValid() ? GPS.satellites.value() : 0,
                GPS.location.isValid());
#endif
            lastPublish = millis();
        }
        if (command.search && !outcomeLogged) {
            if (snapshot.qualified) {
                TrackerLog.printf("[GPS] Fix acquired: LAT=%.6f LON=%.6f satellites=%lu\n",
                              snapshot.latitude / 1e6, snapshot.longitude / 1e6,
                              static_cast<unsigned long>(GPS.satellites.value()));
                outcomeLogged = true;
            } else if (!searching) {
                logGnssStatus(millis() - command.started, stability);
                logGnssTimeout();
                outcomeLogged = true;
            } else if (uint32_t(millis() - lastStatus) >= 1000) {
                logGnssStatus(millis() - command.started, stability);
                lastStatus = millis();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    digitalWrite(Board::statusLed, HIGH);
    // No UART, GPIO or queue access after acknowledging shutdown.
    xSemaphoreGive(gnssStopped);
    vTaskDelete(nullptr);
}

bool readSnapshot(GnssSnapshot &snapshot)
{
    return gnssRunning && gnssSnapshots != nullptr &&
        xQueuePeek(gnssSnapshots, &snapshot, 0) == pdTRUE &&
        snapshot.generation == acquisitionGeneration;
}

void finishAcquisition()
{
    if (gnssRunning && acquiring) {
        const GnssCommand command{acquisitionGeneration, acquisitionStart,
                                  acquisitionTimeout, false};
        xQueueOverwrite(gnssCommands, &command);
    }
    acquiring = false;
}

}

void setup_gnss(bool rawNmeaDebug)
{
    if (gnssRunning) return;
    echoRawNmea = rawNmeaDebug;
    Board::setVext(true);
    // RX-only avoids driving the L76-L 2.8 V UART domain from a 3.3 V GPIO.
    Serial1.setRxBufferSize(1024);
    Serial1.begin(9600, SERIAL_8N1, Board::gnssRx, -1);
    pinMode(Board::gnssTx, INPUT);
    TrackerLog.printf("[GPS] UART1 RX=%d TX=disabled baud=9600 raw=%s\n",
                  Board::gnssRx, echoRawNmea ? "ON" : "OFF");
    gnssCommands = xQueueCreate(1, sizeof(GnssCommand));
    gnssSnapshots = xQueueCreate(1, sizeof(GnssSnapshot));
    gnssStopped = xSemaphoreCreateBinary();
    if (gnssCommands == nullptr || gnssSnapshots == nullptr || gnssStopped == nullptr ||
        xTaskCreatePinnedToCore(gnssWorker, "gps", 4096, nullptr, 1, &gnssTask, 0) != pdPASS) {
        TrackerLog.println("[GPS] Task/queue allocation failed; GPS acquisition disabled");
        stop_gnss();
        return;
    }
    gnssRunning = true;
    TrackerLog.println("[GPS] Dedicated task started on core 0; runs alongside LoRa");
    start_gnss_acquisition();
}

void stop_gnss()
{
    if (gnssTask != nullptr) {
        xTaskNotify(gnssTask, stopTaskBit, eSetBits);
        // Let the owner finish before closing UART or cutting peripheral power.
        xSemaphoreTake(gnssStopped, portMAX_DELAY);
        gnssTask = nullptr;
    }
    gnssRunning = false;
    acquiring = false;
    if (gnssCommands != nullptr) { vQueueDelete(gnssCommands); gnssCommands = nullptr; }
    if (gnssSnapshots != nullptr) { vQueueDelete(gnssSnapshots); gnssSnapshots = nullptr; }
    if (gnssStopped != nullptr) { vSemaphoreDelete(gnssStopped); gnssStopped = nullptr; }
    Serial1.end();
    pinMode(Board::gnssRx, INPUT);
    pinMode(Board::gnssTx, INPUT);
    // Board::prepareSleep turns off the GPS supply afterwards.
}

void start_gnss_acquisition(bool restart)
{
    if (!gnssRunning) {
        return;
    }
    if (acquiring && !restart) return;
    acquisitionTimeout = !hasPreviousFix
        ? AppConfig::firstGpsTimeoutMs : AppConfig::gpsTimeoutMs;
    acquisitionStart = millis();
    ++acquisitionGeneration;
    consumedFixSequence = 0;
    acquiring = true;
    const GnssCommand command{acquisitionGeneration, acquisitionStart, acquisitionTimeout, true};
    xQueueOverwrite(gnssCommands, &command);
}

void service_gnss()
{
    GnssSnapshot snapshot;
    if (!readSnapshot(snapshot)) return;
    if (!snapshot.qualified ||
        snapshot.fixSequence == consumedFixSequence) return;
    rtc_lat = snapshot.latitude;
    rtc_lon = snapshot.longitude;
    hasPreviousFix = true;
    consumedFixSequence = snapshot.fixSequence;
}

GnssStatus gnss_status()
{
    if (!gnssRunning) return GnssStatus::Unavailable;
    GnssSnapshot snapshot;
    if (!readSnapshot(snapshot)) return GnssStatus::Searching;
    if (snapshot.qualified) return GnssStatus::FixReady;
    return snapshot.complete ? GnssStatus::TimedOut : GnssStatus::Searching;
}

bool get_location()
{
    start_gnss_acquisition(); // Reuse the search already started before OTAA.
    if (!gnssRunning) return false;

    for (;;)
    {
        DeviceButton::service();
        update_fall_detection(false);
        if (DeviceButton::sosPending() || DeviceButton::otaPending() ||
            fall_detection_pending() || TripControl::wellbeingPending()) {
            finishAcquisition();
            return false;
        }
        service_gnss();
        GnssSnapshot snapshot;
        const bool haveSnapshot = readSnapshot(snapshot);
        if (haveSnapshot && snapshot.qualified &&
            uint32_t(millis() - snapshot.fixAt) <= AppConfig::gpsMaxFixAgeMs) {
            finishAcquisition();
            return true;
        }
        // The worker owns timeout detection/logging as well as fix detection.
        if (haveSnapshot && snapshot.complete) break;
        delay(10); // Wait for a snapshot; only the GPS task processes NMEA.
    }

    finishAcquisition();
    return false;
}
