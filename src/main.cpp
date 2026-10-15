#include <Arduino.h>
#include <audio_feedback.h>
#include <board_pins.h>
#include <debug_mode.h>
#include <deep_sleep.h>
#include <fall_detection.h>
#include <gnss.h>
#if __has_include(<lorawan.h>)
#include <lorawan.h>
#else
#include <lorawan.example.h>
#endif
#include <tracker_app.h>
#include <device_button.h>
#include <ota_manager.h>
#include <battery.h>
#include <esp_system.h>

// Check the configuration of the prebuilt framework, not a build-flag override.
static_assert(CONFIG_ESP_IPC_TASK_STACK_SIZE >= 2048,
              "Use the pinned Arduino framework with the ESP32-S3 IPC stack fix");

// Both modes use real deep sleep and the same button/radio lifecycle.
// Debug only shortens the normal interval and enables simulated GPS/logging.
constexpr bool LORA_DEBUG = false;
constexpr uint32_t LORA_DEBUG_INTERVAL_MS = 15000;
// Echo raw NMEA for GNSS diagnostics; this does not disable acquisition timeouts.
constexpr bool GNSS_DEBUG = false;
// Wait only on power-on / external RESET, never on deep-sleep wake or crash reset.
// The EN reset button can report POWERON, so it cannot be separated from power-on.
// This is independent of LORA_DEBUG so real GNSS/sleep behavior can be tested.
constexpr bool USB_MONITOR_WAIT = true;
constexpr uint32_t USB_MONITOR_WAIT_MS = 5000;

namespace {
void waitForUsbMonitor()
{
#if ARDUINO_USB_CDC_ON_BOOT
    if (!USB_MONITOR_WAIT) return;
    const esp_reset_reason_t reason = esp_reset_reason();
    if (reason != ESP_RST_POWERON && reason != ESP_RST_EXT) return;
    const uint32_t started = millis();
    while (!Serial && millis() - started < USB_MONITOR_WAIT_MS) {
        delay(10);
    }
    if (Serial) delay(100); // Let the host finish opening the reconnected port.
#endif
}
}

void setup()
{
    Serial.begin(115200);
    setCpuFrequencyMhz(80);
    Board::begin();
    AudioFeedback::begin();
    // Button initialization can emit logs or enter OTA/shutdown.
    waitForUsbMonitor();
    TrackerApp::beginWatchdog();
    DeviceButton::begin();
    if (DeviceButton::startedFromPowerOff()) lora_forget_rtc_session();
    if (DeviceButton::otaPending()) OtaManager::run(AppConfig::reportIntervalMs);

    Serial.println("\n=== ESP32-S3 Tracker Initializing ===");
    Serial.printf("[BOOT] Reset reason: %d\n", int(esp_reset_reason()));

    Serial.printf("LoRa debug: %s\n", LORA_DEBUG ? "ON (real deep sleep)" : "OFF");
    DebugMode::begin(LORA_DEBUG, LORA_DEBUG_INTERVAL_MS);
    DeepSleep::logWakeReason();
    Battery::bootReading();
    setup_fall_detection(LORA_DEBUG);
    if (!LORA_DEBUG) {
        setup_gnss(GNSS_DEBUG);
    }
}

void loop()
{
    TrackerApp::runCycle();
}
