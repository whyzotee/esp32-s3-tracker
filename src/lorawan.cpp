#include <tracker_log.h>
#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>
#include <tracker_app.h>
#include <board_pins.h>
#include <battery.h>
#include <lorawan_storage.h>
#include <device_button.h>
#if __has_include(<lorawan.h>)
#include <lorawan.h>
#else
#include <lorawan.example.h>
#endif

namespace {
// Run shutdown on the radio's owning task, not concurrently from the button
// worker. Short clicks must not stall RX timing. Never re-enter SPI mid-command.
class ButtonRadioHal : public ArduinoHal {
public:
    ButtonRadioHal() : ArduinoHal(SPI) {}
    bool responsive = false;
    void spiBeginTransaction() override {
        inTransaction = true;
        ArduinoHal::spiBeginTransaction();
    }
    void spiEndTransaction() override {
        ArduinoHal::spiEndTransaction();
        inTransaction = false;
    }
    void yield() override {
        ArduinoHal::yield();
        poll();
    }
    void delay(RadioLibTime_t ms) override {
        if (!responsive || inTransaction) {
            ArduinoHal::delay(ms);
            return;
        }
        // Keep the original deadline instead of adding a button debounce wait
        // to each RX-window delay. Microsecond radio timings are untouched.
        const RadioLibTime_t start = ArduinoHal::millis();
        poll();
        for (;;) {
            const RadioLibTime_t elapsed = ArduinoHal::millis() - start;
            if (elapsed >= ms) break;
            const RadioLibTime_t remaining = ms - elapsed;
            ArduinoHal::delay(remaining > 10 ? 10 : remaining);
            poll();
        }
    }
private:
    bool inTransaction = false;
    void poll() {
        if (!responsive || inTransaction) return;
        DeviceButton::servicePowerOff();
    }
};
ButtonRadioHal radioHal;
// Preserve the RX preamble timeout behavior independently of diagnostics.
class TrackerRadio : public SX1262 {
public:
    explicit TrackerRadio(Module *module) : SX1262(module), modPtr(module) {}
    int16_t stageMode(RadioModeType_t mode, RadioModeConfig_t *config) override {
        const int16_t rc = SX1262::stageMode(mode, config);
        if (mode == RADIOLIB_RADIO_MODE_RX && modPtr) {
            const uint8_t stopPreamble = RADIOLIB_SX126X_STOP_ON_PREAMBLE_ON;
            modPtr->SPIwriteStream(RADIOLIB_SX126X_CMD_STOP_TIMER_ON_PREAMBLE, &stopPreamble, 1);
        }
        return rc;
    }
private:
    Module *modPtr = nullptr;
};
TrackerRadio radio(new Module(&radioHal, Board::radioCs, Board::radioDio1, Board::radioReset, Board::radioBusy));
// Preserve the deployed channel-0-only configuration (923.2 MHz).
LoRaWANBand_t loraBand = [] {
    LoRaWANBand_t band = AS923;
    band.txFreqs[1].freq = 0;
    // Match the deployed ChirpStack no-dwell configuration. Without
    // this, AS923 defaults clamp RX1 to DR2 (SF10) and reject DR0 uplinks/joins
    // (ToA > 400 ms). Use only where regional/network regulations permit no dwell limit.
    band.dwellTimeDn = 0;
    band.dwellTimeUp = 0;
    return band;
}();
LoRaWANNode node(&radio, &loraBand);
LoRaStorage storage;
bool ready = false;
bool radioReady = false;
RTC_DATA_ATTR bool primaryTelemetryAcknowledged = false;
RTC_DATA_ATTR bool deviceInfoAttempted = false;
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
        TrackerLog.printf("[LoRa] Radio init before sleep: %d\n", state);
        if (state != RADIOLIB_ERR_NONE) {
            ready = false;
            TrackerLog.println("[LoRa] Radio sleep skipped: init failed; low sleep current is not assured");
            return;
        }
    }

    // Restore the original warm-sleep baseline for current measurements.
    // LoRaWAN session/counters in RTC are not affected.
    const int16_t state = radio.sleep();
    TrackerLog.printf("[LoRa] Radio warm sleep: %d\n", state);
    if (state != RADIOLIB_ERR_NONE)
        TrackerLog.println("[LoRa] Radio sleep command failed; low sleep current is not assured");

    // Detach only LoRa's SPI2 bus. ADXL362 uses a separate SPI3 bus and must
    // keep running for fall wake. Keep NSS pulled HIGH so it cannot float LOW
    // and wake the SX1262; do not change reset or interrupt pins here.
    SPI.end();
    pinMode(Board::radioSck, INPUT);
    pinMode(Board::radioMosi, INPUT);
    pinMode(Board::radioMiso, INPUT);
    pinMode(Board::radioCs, INPUT_PULLUP);
    TrackerLog.println("[LoRa] SPI released: SCK/MOSI/MISO=INPUT, CS=INPUT_PULLUP");
    ready = false;
    radioReady = false;
}

uint32_t lora_wait_ms()
{
    return ready ? node.timeUntilUplink() : 0;
}

void lora_forget_rtc_session()
{
    ready = false;
    primaryTelemetryAcknowledged = false;
    deviceInfoAttempted = false;
    LoRaStorage::clearRtcSession();
    TrackerLog.println("[LoRa] User power cycle: RTC session cleared; OTAA join required");
}

bool setup_lora_wan_app()
{
    // Awake trip/alarm loops keep the live MAC and its airtime timer intact.
    if (ready) return true;
    ready = false;
    buildIdentity();

    int16_t state = initializeRadio();
    TrackerLog.printf("[LoRa] Radio init: %d\n", state);
    if (state != RADIOLIB_ERR_NONE)
        return false;
    state = node.beginOTAA(LoRaCredentials::joinEui, LoRaCredentials::devEui, nullptr, LoRaCredentials::appKey);
    if (state != RADIOLIB_ERR_NONE)
    {
        TrackerLog.printf("[LoRa] OTAA init failed: %d\n", state);
        return false;
    }

    // Populate version/mode/plan/key checksum before storage operations
    node.getBufferNonces();

    LoRaStorage::Restore restore = storage.begin(node, deviceIdentity);
    if (restore == LoRaStorage::Restore::Error)
    {
        TrackerLog.println("[LoRa] Nonce storage invalid or identity changed; join disabled (no automatic reset)");
        return false;
    }

    if (restore == LoRaStorage::Restore::Session)
    {
        radioHal.responsive = true;
        DeviceButton::servicePowerOff();
        state = node.activateOTAA();
        radioHal.responsive = false;
        DeviceButton::servicePowerOff();
        if (state == RADIOLIB_LORAWAN_SESSION_RESTORED || state == RADIOLIB_ERR_NONE)
        {
            TrackerLog.printf("[LoRa] Session restored from RTC (FCntUp: %lu)\n",
                          (unsigned long)LoRaStorage::nextCounter(node));
            ready = true;
        }
        else
        {
            TrackerLog.printf("[LoRa] Session restore activation failed: %d; will re-join\n", state);
            restore = LoRaStorage::Restore::JoinRequired;
        }
    }

    if (restore == LoRaStorage::Restore::Fresh || restore == LoRaStorage::Restore::JoinRequired)
    {
        TrackerLog.printf("[LoRa] Initiating OTAA Join (DevEUI: %016llX)...\n",
                      (unsigned long long)LoRaCredentials::devEui);
        if (!storage.reserveJoin(node))
        {
            TrackerLog.println("[LoRa] Failed to reserve DevNonce; aborting join");
            return false;
        }

        node.setDwellTime(false);
        node.scanGuard = 200;
        const int16_t drState = node.setDatarate(AppConfig::uplinkDataRate);
        if (drState != RADIOLIB_ERR_NONE) {
            TrackerLog.printf("[LoRa] Cannot set Join DR%u: %d\n", AppConfig::uplinkDataRate, drState);
        } else {
            TrackerLog.printf("[LoRa] Join PHY configured: DR%u\n", AppConfig::uplinkDataRate);
        }

        radioHal.responsive = true;
        DeviceButton::servicePowerOff();
        state = node.activateOTAA();
        radioHal.responsive = false;
        DeviceButton::servicePowerOff();
        if (state == RADIOLIB_LORAWAN_NEW_SESSION)
        {
            TrackerLog.println("[LoRa] OTAA Join successful (new session)");
            ready = storage.saveJoined(node);
        }
        else
        {
            TrackerLog.printf("[LoRa] OTAA Join failed: %d\n", state);
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
    // Allow slow-DR uplinks only where local regulations permit no dwell limit.
    node.setDwellTime(false);
    node.scanGuard = 200;
    // RadioLib validates the requested DR against the session's dwell-time limits.
    state = node.setDatarate(AppConfig::uplinkDataRate);
    if (state != RADIOLIB_ERR_NONE) {
        TrackerLog.printf("[LoRa] Cannot set DR%u: %d; check AS923 dwell-time settings\n",
                      AppConfig::uplinkDataRate, state);
        return false;
    }

    TrackerLog.printf("[LoRa] PHY configured: DR%u; ADR=OFF; local uplink dwell limit=OFF; band downlink dwell default=OFF\n",
                  AppConfig::uplinkDataRate);
    return ready;
}

static LoRaResult sendPayload(const uint8_t *payload, size_t length, uint8_t port, bool confirmed)
{
    LoRaResult result{};
    result.code = RADIOLIB_ERR_UNKNOWN;
    if (!ready) return result;
    // Same PHY settings in debug and production.
    result.code = node.setDatarate(AppConfig::uplinkDataRate);
    if (result.code != RADIOLIB_ERR_NONE) {
        TrackerLog.printf("[LoRa] Cannot set TX DR%u: %d; transmission skipped\n",
                      AppConfig::uplinkDataRate, result.code);
        return result;
    }

    if (!storage.reserveUplink(node)) {
        TrackerLog.println("[LoRa] Could not reserve frame counter; transmission skipped");
        result.code = RADIOLIB_ERR_UNKNOWN;
        ready = false;
        return result;
    }

    TrackerLog.printf("[LoRa] TX port=%u bytes=%u FCnt=%lu confirmed=%s DR=%u\n",
                  port, unsigned(length),
                  (unsigned long)LoRaStorage::nextCounter(node),
                  confirmed ? "yes" : "no", AppConfig::uplinkDataRate);

    LoRaWANEvent_t event{};
    result.downlinkLength = sizeof(result.downlink);
    radioHal.responsive = true;
    DeviceButton::servicePowerOff();
    result.code = node.sendReceive(payload, length, port,
                                   result.downlink, &result.downlinkLength, confirmed,
                                   nullptr, &event);
    radioHal.responsive = false;
    DeviceButton::servicePowerOff();
    result.acknowledged = confirmed && result.code > 0 && event.confirming;
    result.downlinkPort = event.fPort;
    if (result.code <= 0) result.downlinkLength = 0;
    result.sessionSaved = storage.saveUplink(node);
    if (!result.sessionSaved) {
        TrackerLog.println("[LoRa] Session save failed");
        ready = false;
    }
    TrackerLog.printf("[LoRa] sendReceive=%d (0=no downlink, 1/2=RX window, negative=error)\n",
                  result.code);
    if (confirmed)
        TrackerLog.printf("[LoRa] Uplink ACK: %s\n",
                      result.acknowledged ? "received" : "missing; retry required");
    else
        TrackerLog.printf("[LoRa] Unconfirmed send: %s; network ACK not requested\n",
                      result.code >= 0 ? "completed" : "failed");
    return result;
}

LoRaResult send_lora_telemetry(const Telemetry &sample)
{
    uint8_t payload[TELEMETRY_PAYLOAD_SIZE];
    encode_telemetry(sample, payload);
    const LoRaResult result = sendPayload(payload, sizeof(payload), AppConfig::uplinkPort,
                                        sample.status != 0);
    if (result.acknowledged && (sample.status == 0 || sample.status == 4))
        primaryTelemetryAcknowledged = true;
    return result;
}

bool lora_device_info_due()
{
    return ready && primaryTelemetryAcknowledged && !deviceInfoAttempted;
}

LoRaResult send_lora_device_info(const Telemetry &sample)
{
    // Best-effort, once per user power-on. Never let metadata retries starve
    // tracking or emergency traffic; durable MAC counters still apply.
    deviceInfoAttempted = true;
    uint8_t payload[DEVICE_INFO_PAYLOAD_SIZE];
    encode_device_info(sample, payload);
    return sendPayload(payload, sizeof(payload), AppConfig::deviceInfoPort, true);
}

