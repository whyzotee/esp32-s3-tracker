#include <Arduino.h>
#include <audio_feedback.h>
#include <board_pins.h>
#include <debug_mode.h>
#include <deep_sleep.h>
#include <fall_detection.h>
#include <gnss.h>
#include <lora_wan.h>
#include <tracker_app.h>
#include <device_button.h>
#include <ota_manager.h>
#include <app_config.h>

// Both modes use real deep sleep and the same button/radio lifecycle.
// Debug only shortens the normal interval and enables simulated GPS/logging.
constexpr bool LORA_DEBUG = false;
constexpr uint32_t LORA_DEBUG_INTERVAL_MS = 15000;
// Echo raw NMEA for GNSS diagnostics; this does not disable acquisition timeouts.
constexpr bool GNSS_DEBUG = false;
void setup()
{
    Serial.begin(115200);
    setCpuFrequencyMhz(80);
    Board::begin();
    AudioFeedback::begin();
    DeviceButton::begin();
    if (DeviceButton::startedFromPowerOff()) lora_forget_rtc_session();
    if (DeviceButton::otaPending()) OtaManager::run(AppConfig::reportIntervalMs);

    // Wait up to 3 seconds for USB CDC Serial monitor to connect
    uint32_t startWait = millis();
    while (!Serial && (millis() - startWait < 3000))
    {
        delay(10);
    }
    delay(500); // Allow host terminal to settle after USB re-enumeration

    Serial.println("\n=== ESP32-S3 Tracker Initializing ===");

    Serial.printf("LoRa debug: %s\n", LORA_DEBUG ? "ON (real deep sleep)" : "OFF");
    DebugMode::begin(LORA_DEBUG, LORA_DEBUG_INTERVAL_MS);
    DeepSleep::logWakeReason();
    setup_fall_detection(LORA_DEBUG);
    if (!LORA_DEBUG) {
        setup_gnss(GNSS_DEBUG);
    }
}

void loop()
{
    TrackerApp::runCycle();
}
