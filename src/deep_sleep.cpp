#include <Arduino.h>
#include <audio_feedback.h>
#include <deep_sleep.h>
#include <app_config.h>
#include <board_pins.h>
#include <device_button.h>
#include <fall_detection.h>
#include <gnss.h>
#include <lora_wan.h>
#include <power_mode.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>

namespace {
void armButton()
{
    rtc_gpio_pullup_en(Board::button);
    rtc_gpio_pulldown_dis(Board::button);
    esp_sleep_enable_ext0_wakeup(Board::button, 0);
}

[[noreturn]] void enter()
{
    sleep_lora_radio();
    stop_gnss();
    AudioFeedback::playAndWait(AudioFeedback::Event::DeepSleep);
    Board::prepareSleep();
    Serial.flush();
    esp_deep_sleep_start();
    while (true) delay(1000);
}
}

namespace DeepSleep {
void logWakeReason()
{
    switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT0: Serial.println("[WAKE] Button"); break;
    case ESP_SLEEP_WAKEUP_EXT1: Serial.println("[WAKE] Fall sensor"); break;
    case ESP_SLEEP_WAKEUP_TIMER: Serial.println("[WAKE] Timer"); break;
    default: Serial.println("[WAKE] Power-on or reset"); break;
    }
    Serial.printf("[WAKE] Low Power: %s; configured report sleep: %lu ms\n",
                  PowerMode::enabled() ? "ON" : "OFF",
                  static_cast<unsigned long>(PowerMode::reportInterval()));
}

[[noreturn]] void timed(uint32_t durationMs, uint32_t minimumMs)
{
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    // Keep pending events in RTC and retry by timer if EXT1 cannot be armed.
    if (!prepare_fall_detection_sleep()) {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
        durationMs = min(durationMs, AppConfig::eventRetryMs);
    }
    if (DeviceButton::sosPending())
        durationMs = min(durationMs, AppConfig::eventRetryMs);
    durationMs = max(durationMs, minimumMs);
    armButton();
    esp_sleep_enable_timer_wakeup(uint64_t(durationMs) * 1000);
    Serial.printf("[SLEEP] Timer: %lu ms; button/fall wake enabled when available\n",
                  (unsigned long)durationMs);
    enter();
}

[[noreturn]] void powerOff()
{
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    armButton();
    Serial.println("[POWER] Off: waiting for a 1-4 second button hold to turn on");
    enter();
}
}
