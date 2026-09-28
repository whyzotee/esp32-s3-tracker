#include <Arduino.h>
#include <audio_feedback.h>
#include <device_button.h>
#include <board_pins.h>
#include <deep_sleep.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

namespace {
RTC_DATA_ATTR bool poweredOff = false;
RTC_DATA_ATTR bool pendingSos = false;
RTC_DATA_ATTR bool pendingOta = false;
bool startedFromOff = false;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
bool held = false;
bool pendingOff = false;
uint8_t clickCount = 0;
uint32_t firstClickAt = 0;
bool otaHoldCandidate = false;

[[noreturn]] void sleepOff()
{
    poweredOff = true;
    pendingSos = false;
    pendingOta = false;
    AudioFeedback::playAndWait(AudioFeedback::Event::PowerOff);

    // GPIO0 is also the active-low EXT0 wake source.  Entering deep sleep
    // while it is still held would wake the ESP32 immediately.  The power-off
    // decision has already been made at the hold threshold; wait here so the
    // user can release at any convenient time rather than having to time it.
    Serial.println("[POWER] Off selected; release button to finish shutdown");
    while (digitalRead(Board::button) == LOW) delay(10);
    delay(40); // release debounce before arming EXT0
    DeepSleep::powerOff();
}

void monitor(void *)
{
    bool stable = digitalRead(Board::button) == LOW;
    bool previous = stable;
    uint32_t changedAt = millis();
    uint32_t pressedAt = changedAt;
    for (;;) {
        bool sosQueued = false;
        bool otaQueued = false;
        bool raw = digitalRead(Board::button) == LOW;
        uint32_t now = millis();
        if (raw != previous) { previous = raw; changedAt = now; }
        portENTER_CRITICAL(&lock);
        // Keep awake throughout debounce, not only after a confirmed press.
        held = raw || stable;
        if (raw != stable && uint32_t(now - changedAt) >= 40) {
            stable = raw;
            if (stable) {
                pressedAt = changedAt;
                // A third press after two short clicks can either be another
                // short click (SOS) or a 3-second hold (OTA).
                otaHoldCandidate = clickCount == 2 &&
                    uint32_t(changedAt - firstClickAt) <= DeviceButton::sosTripleClickWindowMs;
            }
            else {
                const uint32_t heldMs = uint32_t(changedAt - pressedAt);
                auto action = DeviceButton::classify(false, heldMs);
                if (action == DeviceButton::Action::PowerOff) pendingOff = true;
                else if (DeviceButton::isSosClick(heldMs)) {
                    if (clickCount == 0 || uint32_t(changedAt - firstClickAt) >
                                               DeviceButton::sosTripleClickWindowMs) {
                        clickCount = 1;
                        firstClickAt = changedAt;
                    } else if (++clickCount >= 3) {
                        pendingSos = true;
                        clickCount = 0;
                        sosQueued = true;
                    }
                } else {
                    // A non-click press cancels a partial SOS sequence.
                    clickCount = 0;
                }
                otaHoldCandidate = false;
                held = false;
            }
        }
        if (stable && otaHoldCandidate && !pendingOta &&
            uint32_t(now - pressedAt) >= DeviceButton::otaSequenceHoldMs) {
            pendingOta = true;
            clickCount = 0;
            otaHoldCandidate = false;
            otaQueued = true;
        }
        if (stable && !otaHoldCandidate && !pendingOff &&
            uint32_t(now - pressedAt) >= DeviceButton::powerOffMinMs) {
            pendingOff = true;
        }
        portEXIT_CRITICAL(&lock);
        if (sosQueued) {
            Serial.println("[BUTTON] SOS queued (three clicks)");
            AudioFeedback::play(AudioFeedback::Event::Sos);
        }
        if (otaQueued) Serial.println("[BUTTON] OTA mode queued (two clicks + 3-second hold)");
        delay(10);
    }
}
}

namespace DeviceButton {
void begin()
{
    rtc_gpio_deinit(Board::button);
    pinMode(Board::button, INPUT_PULLUP);
    if (poweredOff) {
        // Radio and GPS have not been initialized. Measure until debounced release.
        const uint32_t start = millis();
        uint32_t releasedAt = start;
        bool releasing = false;
        while (true) {
            const uint32_t now = millis();
            if (digitalRead(Board::button) == LOW) releasing = false;
            else {
                if (!releasing) { releasing = true; releasedAt = now; }
                if (uint32_t(now - releasedAt) >= 40) break;
            }
            delay(10);
        }
        if (classify(true, uint32_t(releasedAt - start)) != Action::PowerOn) sleepOff();
        poweredOff = false;
        startedFromOff = true;
        Serial.println("[POWER] On");
        AudioFeedback::playAndWait(AudioFeedback::Event::PowerOn);
    }
    held = digitalRead(Board::button) == LOW;
    // A released EXT0 wake press is the first SOS click. If it is still held,
    // monitor() starts a normal power-off or OTA hold timer immediately.
    clickCount = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0 && !held) ? 1 : 0;
    firstClickAt = millis();
    if (xTaskCreate(monitor, "button", 2048, nullptr, 2, nullptr) != pdPASS) {
        // Do not deploy without working button handling.
        while (digitalRead(Board::button) == LOW) delay(10);
        sleepOff();
    }
    service();
}
void service()
{
    while (true) {
        portENTER_CRITICAL(&lock);
        const bool pressed = held;
        const bool turnOff = pendingOff;
        const bool startOta = pendingOta;
        portEXIT_CRITICAL(&lock);
        // OTA starts at its hold threshold. Power-off is selected at its
        // threshold, then sleepOff() waits internally for GPIO0 to release
        // before arming that same pin as its EXT0 wake source.
        if (startOta) return;
        if (turnOff) sleepOff();
        if (!pressed && digitalRead(Board::button) == HIGH) return;
        delay(10);
    }
}
bool sosPending()
{
    portENTER_CRITICAL(&lock);
    bool value = pendingSos;
    portEXIT_CRITICAL(&lock);
    return value;
}
void acknowledgeSos()
{
    portENTER_CRITICAL(&lock);
    pendingSos = false;
    portEXIT_CRITICAL(&lock);
}
bool otaPending()
{
    portENTER_CRITICAL(&lock);
    bool value = pendingOta;
    portEXIT_CRITICAL(&lock);
    return value;
}
void acknowledgeOta()
{
    portENTER_CRITICAL(&lock);
    pendingOta = false;
    portEXIT_CRITICAL(&lock);
}
bool startedFromPowerOff()
{
    return startedFromOff;
}
}
