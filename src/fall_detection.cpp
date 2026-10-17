#include <fall_detection.h>
#include <SPI.h>
#include <audio_feedback.h>
#include <trip_control.h>
#include <driver/rtc_io.h>
#include <math.h>

namespace {
SPIClass sensorBus(HSPI); // Dedicated SPI3; LoRa uses the global FSPI/SPI2 bus.
bool available = false;
bool diagnosticMode = false;
RTC_DATA_ATTR bool pending = false;
uint32_t lastPoll = 0;
uint32_t lastLog = 0;
bool fallBuzzerPlayed = false;
constexpr uint8_t STATUS = 0x0B;
constexpr uint8_t INACT = 0x20;
constexpr uint8_t REGISTER_ERROR = 0x80;
constexpr uint32_t DEBUG_SAMPLE_INTERVAL_MS = 250;
constexpr uint8_t FILTER_CONFIG = 0x82; // +/-8g, 50 Hz. FIFO history ~= 3.4 seconds.
constexpr uint16_t THRESHOLD_RAW = FALL_THRESHOLD_MG / 4;
static_assert(FALL_DURATION_SAMPLES > 0 && FALL_IMPACT_WINDOW_MS >= FALL_SAMPLE_MS,
              "Fall confirmation requires nonzero sample durations");
uint16_t lowSamples = 0;
uint16_t impactSamplesLeft = 0;
uint8_t nextAxis = 0;
int16_t axes[3] = {};
void readRegisters(uint8_t reg, uint8_t *data, size_t length);

void resetClassifier()
{
    lowSamples = impactSamplesLeft = 0;
    nextAxis = 0;
}

void classifySample(int16_t x, int16_t y, int16_t z)
{
    if (pending) return;
    // Convert to mg before comparing vector magnitude (not individual axes).
    const int32_t mx = int32_t(x) * 4, my = int32_t(y) * 4, mz = int32_t(z) * 4;
    const int64_t squared = int64_t(mx) * mx + int64_t(my) * my + int64_t(mz) * mz;
    if (impactSamplesLeft) {
        if (squared >= int64_t(FALL_IMPACT_MG) * FALL_IMPACT_MG) {
            pending = true;
            resetClassifier();
            Serial.println("[ADXL362] FALL: low acceleration followed by impact; status=2 queued");
            return;
        }
        --impactSamplesLeft;
    }
    if (squared <= int64_t(FALL_THRESHOLD_MG) * FALL_THRESHOLD_MG) {
        if (lowSamples < FALL_DURATION_SAMPLES && ++lowSamples == FALL_DURATION_SAMPLES)
            impactSamplesLeft = FALL_IMPACT_WINDOW_MS / FALL_SAMPLE_MS;
    } else {
        lowSamples = 0;
    }
}

void readFifo()
{
    uint8_t count[2];
    readRegisters(0x0C, count, 2);
    const uint16_t entries = uint16_t(count[0]) | (uint16_t(count[1] & 0x03) << 8);
    if (!entries || entries > 512) return;
    sensorBus.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(Board::accelCs, LOW);
    sensorBus.transfer(0x0D); // FIFO command has no register-address byte.
    for (uint16_t i = 0; i < entries; ++i) {
        const uint8_t lo = sensorBus.transfer(0);
        const uint16_t word = uint16_t(lo) | (uint16_t(sensorBus.transfer(0)) << 8);
        const uint8_t axis = word >> 14;
        // Resynchronize after stream overflow / partial XYZ sets; never mix axes.
        if (axis == 0 && nextAxis != 0) resetClassifier();
        if (axis != nextAxis || axis > 2) {
            resetClassifier();
            continue;
        }
        axes[axis] = (word & 0x0800) ? int16_t((word & 0x0FFF) | 0xF000) : int16_t(word & 0x0FFF);
        if (++nextAxis == 3) {
            nextAxis = 0;
            classifySample(axes[0], axes[1], axes[2]);
        }
    }
    digitalWrite(Board::accelCs, HIGH);
    sensorBus.endTransaction();
}

void readRegisters(uint8_t reg, uint8_t *data, size_t length)
{
    sensorBus.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(Board::accelCs, LOW);
    sensorBus.transfer(0x0B); // ADXL362 register-read command.
    sensorBus.transfer(reg);
    for (size_t i = 0; i < length; ++i) data[i] = sensorBus.transfer(0);
    digitalWrite(Board::accelCs, HIGH);
    sensorBus.endTransaction();
}

bool devicePresent()
{
    uint8_t id[3];
    readRegisters(0x00, id, sizeof(id));
    if (diagnosticMode && !available) {
        Serial.printf("[FALL DEBUG] Device ID: %02X %02X %02X (expected AD 1D F2)\n",
                      id[0], id[1], id[2]);
    }
    return id[0] == 0xAD && id[1] == 0x1D && id[2] == 0xF2;
}

bool writeRegister(uint8_t reg, uint8_t value)
{
    sensorBus.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(Board::accelCs, LOW);
    sensorBus.transfer(0x0A);
    sensorBus.transfer(reg);
    sensorBus.transfer(value);
    digitalWrite(Board::accelCs, HIGH);
    sensorBus.endTransaction();
    uint8_t actual = 0;
    readRegisters(reg, &actual, 1);
    if (diagnosticMode && actual != value) {
        Serial.printf("[FALL DEBUG] Register 0x%02X write failed: expected=0x%02X actual=0x%02X\n",
                      reg, value, actual);
    }
    return actual == value;
}

bool readInterrupt(bool allowUnconfigured = false, bool verifyDevice = true)
{
    // SPI has no ACK: check identity to catch common disconnected-bus faults.
    if (verifyDevice && !devicePresent()) return false;
    uint8_t source = 0;
    readRegisters(STATUS, &source, 1); // Clears ACT/INACT hardware latches.
    if (source & REGISTER_ERROR) {
        if (diagnosticMode) {
            Serial.printf("[FALL DEBUG] STATUS=0x%02X ERR_USER_REGS=1 (%s)\n",
                          source, allowUnconfigured ? "expected before configuration" : "unexpected");
        }
        return allowUnconfigured;
    }
    uint8_t config[2];
    readRegisters(0x2C, config, 2);
    if (config[0] != FILTER_CONFIG || config[1] != 0x02) return true;
    if (source & 0x08) resetClassifier(); // Missing samples must not bridge an old candidate.
    readFifo();
    if (impactSamplesLeft && !pending) {
        // A bounded confirmation period; no tone/uplink until impact is verified.
        // Service the FIFO while waiting, including an impact just after CPU wake.
        const uint32_t start = millis();
        while (impactSamplesLeft && !pending && uint32_t(millis() - start) < FALL_IMPACT_WINDOW_MS) {
            delay(10);
            readFifo();
        }
        if (!pending) {
            resetClassifier();
            Serial.println("[ADXL362] Suspected fall rejected: no qualifying impact");
        }
    }
    return true;
}
}

bool setup_fall_detection(bool debug)
{
    diagnosticMode = debug;
    available = false;
    rtc_gpio_deinit(FALL_INT_PIN);
    pinMode(FALL_INT_PIN, INPUT_PULLDOWN);
    pinMode(Board::accelInt2, INPUT);
    pinMode(Board::accelCs, OUTPUT);
    digitalWrite(Board::accelCs, HIGH);
    sensorBus.begin(Board::accelSck, Board::accelMiso, Board::accelMosi, Board::accelCs);
    delay(5); // Power-on register access settling.
    if (!devicePresent()) {
        Serial.println("[ADXL362] Not found (expected AD 1D F2); check SPI wiring. Fall wake disabled");
        return false;
    }
    // Read retained FIFO BEFORE standby/reconfiguration: impact may precede CPU boot.
    // ERR_USER_REGS is expected at startup until the first protected-register write.
    if (!readInterrupt(true, false) ||
        !writeRegister(0x2A, 0x00) || // INT1 off during configuration.
        !writeRegister(0x2B, 0x00) || // INT2 unused.
        !writeRegister(0x2D, 0x00) || // Standby.
        !writeRegister(0x27, 0x00) ||
        !writeRegister(0x28, 0x00) || // Clear old FIFO only after inspecting it.
        !writeRegister(0x2C, FILTER_CONFIG) ||
        !writeRegister(0x23, THRESHOLD_RAW & 0xFF) ||
        !writeRegister(0x24, THRESHOLD_RAW >> 8) ||
        !writeRegister(0x25, FALL_DURATION_SAMPLES & 0xFF) ||
        !writeRegister(0x26, FALL_DURATION_SAMPLES >> 8) ||
        !writeRegister(0x27, 0x04) || // Absolute inactivity, default (not linked) mode.
        !writeRegister(0x29, 0x00) ||
        !writeRegister(0x28, 0x02) || // Continuous stream, XYZ only, latest ~3.4 seconds.
        !readInterrupt(false, false) ||
        !writeRegister(0x2A, INACT) || // Active-high latched INT1.
        !writeRegister(0x2D, 0x02)) { // Continuous measurement; no autosleep.
        Serial.println("[ADXL362] Configuration/readback failed; fall wake disabled");
        return false;
    }
    resetClassifier();
    available = true;
    Serial.printf("[ADXL362] Ready: CS=%d SCK=%d MISO=%d MOSI=%d INT1=%d, %umg/%ums\n",
                  Board::accelCs, Board::accelSck, Board::accelMiso, Board::accelMosi,
                  int(FALL_INT_PIN), FALL_THRESHOLD_MG, FALL_DURATION_SAMPLES * FALL_SAMPLE_MS);
    Serial.printf("[ADXL362] Confirmation: impact >=%umg within %ums; range +/-8g, FIFO 50Hz\n",
                  FALL_IMPACT_MG, FALL_IMPACT_WINDOW_MS);
    if (diagnosticMode) {
        Serial.println("[FALL DEBUG] Real deep-sleep wake, status=2 uplink and buzzer enabled");
        Serial.println("[FALL DEBUG] Use a padded fixture; low acceleration alone does not send a fall alert");
    }
    return true;
}

void update_fall_detection(bool debug)
{
    if (!available || uint32_t(millis() - lastPoll) < 20) return;
    lastPoll = millis();
    if (!readInterrupt()) {
        if (uint32_t(millis() - lastLog) >= 1000) {
            lastLog = millis();
            Serial.println("[ADXL362] SPI identity/register error");
        }
        return;
    }
    if (debug && uint32_t(millis() - lastLog) >= DEBUG_SAMPLE_INTERVAL_MS) {
        lastLog = millis();
        uint8_t data[6];
        readRegisters(0x0E, data, sizeof(data));
        int16_t x = uint16_t(data[0]) | uint16_t(data[1]) << 8;
        int16_t y = uint16_t(data[2]) | uint16_t(data[3]) << 8;
        int16_t z = uint16_t(data[4]) | uint16_t(data[5]) << 8;
        const float magnitude = sqrtf(float(x) * x + float(y) * y + float(z) * z) * 0.004f;
        Serial.printf("[FALL DEBUG] x=%+.3fg y=%+.3fg z=%+.3fg |a|=%.3fg INT1=%d pending=%s\n",
                      x * 0.004, y * 0.004, z * 0.004, magnitude,
                      digitalRead(FALL_INT_PIN), pending ? "YES" : "NO");
    }

    if (pending && !fallBuzzerPlayed) {
        fallBuzzerPlayed = true;
        TripControl::raiseAlarm();
        TripControl::refreshAudio();
        Serial.println("[FALL] EVENT DETECTED; alert tone active");
    }
}

bool fall_detection_pending() { return pending; }
void acknowledge_fall_detection()
{
    pending = false;
    fallBuzzerPlayed = false;
}

bool prepare_fall_detection_sleep()
{
    if (!available) return true;
    if (!readInterrupt() || pending) return false;
    // R10 pulls CS high while asleep; sensor remains on always-powered 3V3.
    // Do not read STATUS again after arming: a new event must wake the CPU.
    rtc_gpio_pulldown_en(FALL_INT_PIN);
    rtc_gpio_pullup_dis(FALL_INT_PIN);
    return esp_sleep_enable_ext1_wakeup(1ULL << FALL_INT_PIN, ESP_EXT1_WAKEUP_ANY_HIGH) == ESP_OK;
}
