#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>
#include <audio_feedback.h>
#include <tracker_app.h>
#include <board_pins.h>
#include <battery.h>
#include <lorawan_storage.h>
#if __has_include(<lorawan.h>)
#include <lorawan.h>
#else
#include <lorawan.example.h>
#endif

namespace {
SX1262 radio = new Module(Board::radioCs, Board::radioDio1, Board::radioReset, Board::radioBusy);
// Preserve the deployed channel-0-only configuration (923.2 MHz).
LoRaWANBand_t loraBand = [] {
    LoRaWANBand_t band = AS923;
    band.txFreqs[1].freq = 0;
    return band;
}();
LoRaWANNode node(&radio, &loraBand);
LoRaStorage storage;
bool ready = false;
bool radioReady = false;
uint8_t deviceIdentity[32];

int16_t initializeRadio()
{
    SPI.begin(Board::radioSck, Board::radioMiso, Board::radioMosi, Board::radioCs);
    // SX1262 with DIO3-controlled 1.8 V TCXO; DIO2 controls RF switch.
    const int16_t state = radio.begin(923.2, 125.0, 9, 5,
        RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 14, 8, 1.8);
    radioReady = state == RADIOLIB_ERR_NONE;
    return state;
}

void buildIdentity()
{
    memcpy(deviceIdentity, &LoRaCredentials::joinEui, 8);
    memcpy(deviceIdentity + 8, &LoRaCredentials::devEui, 8);
    memcpy(deviceIdentity + 16, LoRaCredentials::appKey, 16);
}
}

void sleep_lora_radio()
{
    // Shutdown/OTA can be entered before the normal radio setup. Initialize
    // the PHY only so sleep is sent reliably; do not join or change storage.
    if (!radioReady) {
        const int16_t state = initializeRadio();
        Serial.printf("[LoRa] Radio init before sleep: %d\n", state);
        if (state != RADIOLIB_ERR_NONE) {
            ready = false;
            Serial.println("[LoRa] Radio sleep skipped: init failed; low sleep current is not assured");
            return;
        }
    }

    // Restore the original warm-sleep baseline for current measurements.
    // LoRaWAN session/counters in RTC are not affected.
    const int16_t state = radio.sleep();
    Serial.printf("[LoRa] Radio warm sleep: %d\n", state);
    if (state != RADIOLIB_ERR_NONE)
        Serial.println("[LoRa] Radio sleep command failed; low sleep current is not assured");

    // Detach only LoRa's SPI2 bus. ADXL362 uses a separate SPI3 bus and must
    // keep running for fall wake. Keep NSS pulled HIGH so it cannot float LOW
    // and wake the SX1262; do not change reset or interrupt pins here.
    SPI.end();
    pinMode(Board::radioSck, INPUT);
    pinMode(Board::radioMosi, INPUT);
    pinMode(Board::radioMiso, INPUT);
    pinMode(Board::radioCs, INPUT_PULLUP);
    Serial.println("[LoRa] SPI released: SCK/MOSI/MISO=INPUT, CS=INPUT_PULLUP");
    ready = false;
    radioReady = false;
}

uint32_t lora_wait_ms()
{
    return ready ? node.timeUntilUplink() : 0;
}

void lora_forget_rtc_session()
{
    LoRaStorage::clearRtcSession();
    Serial.println("[LoRa] User power cycle: RTC session cleared; OTAA join required");
}

bool setup_lora_wan_app()
{
    ready = false;
    buildIdentity();

    int16_t state = initializeRadio();
    Serial.printf("[LoRa] Radio init: %d\n", state);
    if (state != RADIOLIB_ERR_NONE)
        return false;
    state = node.beginOTAA(LoRaCredentials::joinEui, LoRaCredentials::devEui, nullptr, LoRaCredentials::appKey);
    if (state != RADIOLIB_ERR_NONE)
    {
        Serial.printf("[LoRa] OTAA init failed: %d\n", state);
        return false;
    }

    // Populate version/mode/plan/key checksum before storage operations
    node.getBufferNonces();

    LoRaStorage::Restore restore = storage.begin(node, deviceIdentity);
    if (restore == LoRaStorage::Restore::Error)
    {
        Serial.println("[LoRa] Nonce storage invalid or identity changed; join disabled (no automatic reset)");
        return false;
    }

    if (restore == LoRaStorage::Restore::Session)
    {
        state = node.activateOTAA();
        if (state == RADIOLIB_LORAWAN_SESSION_RESTORED || state == RADIOLIB_ERR_NONE)
        {
            Serial.printf("[LoRa] Session restored from RTC (FCntUp: %lu)\n",
                          (unsigned long)LoRaStorage::nextCounter(node));
            ready = true;
        }
        else
        {
            Serial.printf("[LoRa] Session restore activation failed: %d; will re-join\n", state);
            restore = LoRaStorage::Restore::JoinRequired;
        }
    }

    if (restore == LoRaStorage::Restore::Fresh || restore == LoRaStorage::Restore::JoinRequired)
    {
        Serial.printf("[LoRa] Initiating OTAA Join (DevEUI: %016llX)...\n",
                      (unsigned long long)LoRaCredentials::devEui);
        AudioFeedback::startLoop(AudioFeedback::Event::LoRaJoining);
        if (!storage.reserveJoin(node))
        {
            Serial.println("[LoRa] Failed to reserve DevNonce; aborting join");
            AudioFeedback::stopLoop(AudioFeedback::Event::LoRaJoining);
            return false;
        }

        state = node.activateOTAA();
        AudioFeedback::stopLoop(AudioFeedback::Event::LoRaJoining);
        if (state == RADIOLIB_LORAWAN_NEW_SESSION)
        {
            Serial.println("[LoRa] OTAA Join successful (new session)");
            ready = storage.saveJoined(node);
        }
        else
        {
            Serial.printf("[LoRa] OTAA Join failed: %d\n", state);
            return false;
        }
    }

    // DevStatusAns uses the LoRaWAN battery scale, not percent:
    // 1 = empty, 254 = full, 255 = unavailable (0 means external power).
    // Refresh after activation/restoration so every wake answers with its reading.
    const BatteryReading battery = Battery::bootReading();
    const uint8_t batteryLevel = battery.valid
        ? static_cast<uint8_t>(1 + (uint32_t(battery.percent) * 253 + 50) / 100)
        : 255;
    node.setDeviceStatus(batteryLevel);

    node.setADR(false);
    // Allow the 15-byte SF10 uplink where local regulations permit no dwell limit.
    node.setDwellTime(false);
    // RadioLib validates the requested DR against the session's dwell-time limits.
    state = node.setDatarate(AppConfig::uplinkDataRate);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LoRa] Cannot set DR%u: %d; check AS923 dwell-time settings\n",
                      AppConfig::uplinkDataRate, state);
        return false;
    }

    if (ready) AudioFeedback::play(AudioFeedback::Event::LoRaConnected);
    return ready;
}

LoRaResult send_lora_telemetry(const Telemetry &sample)
{
    LoRaResult result{};
    result.code = RADIOLIB_ERR_UNKNOWN;
    if (!ready) return result;
    // Same PHY settings in debug and production.
    result.code = node.setDatarate(AppConfig::uplinkDataRate);
    if (result.code != RADIOLIB_ERR_NONE) return result;

    uint8_t payload[TELEMETRY_PAYLOAD_SIZE];
    encode_telemetry(sample, payload);
    if (!storage.reserveUplink(node)) {
        Serial.println("[LoRa] Could not reserve frame counter; transmission skipped");
        result.code = RADIOLIB_ERR_UNKNOWN;
        ready = false;
        return result;
    }

    const bool confirmed = sample.status == 1 || sample.status == 2;
    Serial.printf("[LoRa] TX port=%u bytes=%u FCnt=%lu status=%u confirmed=%s\n",
                  AppConfig::uplinkPort, unsigned(sizeof(payload)),
                  (unsigned long)LoRaStorage::nextCounter(node), sample.status,
                  confirmed ? "yes" : "no");

    LoRaWANEvent_t event{};
    result.downlinkLength = sizeof(result.downlink);
    result.code = node.sendReceive(payload, sizeof(payload), AppConfig::uplinkPort,
                                   result.downlink, &result.downlinkLength, confirmed,
                                   nullptr, &event);
    result.acknowledged = confirmed && result.code > 0 && event.confirming;
    result.downlinkPort = event.fPort;
    if (result.code <= 0) result.downlinkLength = 0;
    result.sessionSaved = storage.saveUplink(node);
    if (!result.sessionSaved) {
        Serial.println("[LoRa] Session save failed");
        ready = false;
    }
    Serial.printf("[LoRa] sendReceive=%d (0=no downlink, 1/2=RX window, negative=error)\n",
                  result.code);
    if (confirmed)
        Serial.printf("[LoRa] Uplink ACK: %s\n",
                      result.acknowledged ? "received" : "missing; event remains pending");
    return result;
}

