#include <gnss.h>
#include <TinyGPS++.h>
#include <device_button.h>
#include <tracker_app.h>
#include <fall_detection.h>

TinyGPSPlus GPS;

namespace {
bool echoRawNmea = false;
RTC_DATA_ATTR bool hasPreviousFix = false;
TinyGPSCustom gpFixQuality(GPS, "GPGGA", 6);
TinyGPSCustom gnFixQuality(GPS, "GNGGA", 6);

// LED2 is active-high. Restore the normal awake indicator on every exit path,
// including a fix, timeout, or an SOS/Fall/OTA interruption.
struct GnssSearchLed {
    uint32_t started = millis();
    GnssSearchLed() { digitalWrite(Board::statusLed, HIGH); }
    void update() const {
        digitalWrite(Board::statusLed,
                     ((millis() - started) / 500U) % 2U == 0U ? HIGH : LOW);
    }
    ~GnssSearchLed() { digitalWrite(Board::statusLed, HIGH); }
};

struct FixStability {
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
            count = 0;
            lastEpoch = epoch;
            return false;
        }
        if (epoch == lastEpoch) return false; // GP/GN must not double-count an epoch.
        if (count > 0) {
            const uint32_t gap = now - lastAt;
            const double allowed = AppConfig::gpsJumpToleranceM +
                AppConfig::gpsMaxTrackingSpeedMps * gap / 1000.0;
            if (gap > AppConfig::gpsMaxFixAgeMs ||
                TinyGPSPlus::distanceBetween(latitude, longitude, lat, lon) > allowed)
                count = 0;
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

void printGnssStatus(uint32_t elapsedMs)
{
    Serial.printf("[GPS] elapsed=%lus chars=%lu checksum_ok=%lu checksum_fail=%lu "
                  "satellites=%lu location=%s\n",
                  static_cast<unsigned long>(elapsedMs / 1000),
                  static_cast<unsigned long>(GPS.charsProcessed()),
                  static_cast<unsigned long>(GPS.passedChecksum()),
                  static_cast<unsigned long>(GPS.failedChecksum()),
                  static_cast<unsigned long>(GPS.satellites.isValid()
                      ? GPS.satellites.value() : 0),
                  GPS.location.isValid() ? "valid" : "invalid");
}

void printGnssFailure()
{
    if (GPS.charsProcessed() == 0) {
        Serial.println("[GPS] No UART data: check VEXT, L76L power, GPIO41 and baud rate");
    } else if (GPS.passedChecksum() == 0) {
        Serial.println("[GPS] UART data received but no valid NMEA: check baud rate and signal integrity");
    } else {
        Serial.println("[GPS] No fix met quality/stability requirements: check antenna and sky view");
    }
}
}

RTC_DATA_ATTR int32_t rtc_lat = 0;
RTC_DATA_ATTR int32_t rtc_lon = 0;

void setup_gnss(bool rawNmeaDebug)
{
    echoRawNmea = rawNmeaDebug;
    Board::setVext(true);
    // RX-only avoids driving the L76-L 2.8 V UART domain from a 3.3 V GPIO.
    Serial1.begin(9600, SERIAL_8N1, Board::gnssRx, -1);
    pinMode(Board::gnssTx, INPUT);
    Serial.printf("[GPS] UART1 RX=%d TX=disabled baud=9600 raw=%s\n",
                  Board::gnssRx, echoRawNmea ? "ON" : "OFF");
}

void stop_gnss()
{
    Serial1.end();
    pinMode(Board::gnssRx, INPUT);
    pinMode(Board::gnssTx, INPUT);
    // Board::prepareSleep turns off the GPS supply afterwards.
}

bool get_location()
{
    GnssSearchLed searchLed;
    const uint32_t timeout = !hasPreviousFix
        ? AppConfig::firstGpsTimeoutMs : AppConfig::gpsTimeoutMs;

    const uint32_t start = millis();
    uint32_t lastStatus = start;
    FixStability stability;
    Serial.printf("[GPS] Acquisition timeout=%lu ms previous_fix=%s\n",
                  static_cast<unsigned long>(timeout), hasPreviousFix ? "yes" : "no");

    while (millis() - start < timeout)
    {
        searchLed.update();
        DeviceButton::service();
        update_fall_detection(false);
        if (DeviceButton::sosPending() || DeviceButton::otaPending() ||
            fall_detection_pending()) return false;
        while (Serial1.available())
        {
            const char byte = static_cast<char>(Serial1.read());
            const bool complete = GPS.encode(byte);
            if (echoRawNmea) Serial.write(byte);
            if (complete && stability.observe()) {
                rtc_lat = stability.latitude * 1e6;
                rtc_lon = stability.longitude * 1e6;
                hasPreviousFix = true;
                Serial.printf("[GPS] Fix acquired: LAT=%.6f LON=%.6f satellites=%lu\n",
                              stability.latitude, stability.longitude,
                              static_cast<unsigned long>(GPS.satellites.value()));
                return true;
            }
        }
        delay(1);

        const uint32_t now = millis();
        if ((now - lastStatus) >= 1000)
        {
            lastStatus = now;
            printGnssStatus(now - start);
        }
        if ((now - start) >= timeout)
        {
            printGnssFailure();
            break;
        }
    }

    return false;
}
