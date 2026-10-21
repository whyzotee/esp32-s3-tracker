#include <tracker_log.h>
#include <Arduino.h>
#include <tracker_app.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <mutex>

static_assert(CONFIG_ESP_TASK_WDT_EN && CONFIG_ESP_TASK_WDT_INIT && CONFIG_ESP_TASK_WDT_PANIC,
              "Tracking requires the framework hardware Task WDT with panic/reboot enabled");
static_assert(CONFIG_ESP_TASK_WDT_TIMEOUT_S >= 3,
              "Tracking heartbeat needs a Task WDT timeout of at least 3 seconds");
static_assert(AppConfig::trackingWatchdogMs > AppConfig::firstGpsTimeoutMs,
              "Tracking watchdog must allow first GNSS acquisition");

namespace {
esp_timer_handle_t trackingWatchdog = nullptr;
esp_task_wdt_user_handle_t trackingUser = nullptr;
int64_t trackingStartedUs = 0;
std::mutex trackingWatchdogMutex;

void feedTrackingWatchdog(void *)
{
    // Synchronize with deletion on the Arduino task; no print/flush in callback.
    const std::lock_guard<std::mutex> lock(trackingWatchdogMutex);
    if (trackingUser != nullptr && esp_timer_get_time() - trackingStartedUs <
        int64_t(AppConfig::trackingWatchdogMs) * 1000) {
        ESP_ERROR_CHECK(esp_task_wdt_reset_user(trackingUser));
    }
    // Past the deadline, or if this timer task stalls, no heartbeat reaches
    // the hardware-backed Task WDT. Its normal timeout/panic triggers recovery.
}

}

// Keep the public entry points stable; watchdog ownership is independent of
// trip scheduling and peripheral acquisition.
namespace TrackerApp {
void beginWatchdog()
{
    const std::lock_guard<std::mutex> lock(trackingWatchdogMutex);
    if (trackingWatchdog != nullptr) return;
    esp_timer_create_args_t config{};
    config.callback = feedTrackingWatchdog;
    config.dispatch_method = ESP_TIMER_TASK;
    config.name = "tracker-wdt-feed";
    ESP_ERROR_CHECK(esp_timer_create(&config, &trackingWatchdog));
    trackingStartedUs = esp_timer_get_time();
    ESP_ERROR_CHECK(esp_task_wdt_add_user("tracker-cycle", &trackingUser));
    ESP_ERROR_CHECK(esp_timer_start_periodic(trackingWatchdog, 1000000));
    TrackerLog.printf("[WDT] Hardware Task WDT armed; tracking budget: %lu ms\n",
                  static_cast<unsigned long>(AppConfig::trackingWatchdogMs));
    // Never extend trackingStartedUs from acquisition/radio loops.
}

void endWatchdog()
{
    const std::lock_guard<std::mutex> lock(trackingWatchdogMutex);
    if (trackingWatchdog == nullptr) return;
    const esp_err_t state = esp_timer_stop(trackingWatchdog);
    if (state != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(state);
    ESP_ERROR_CHECK(esp_timer_delete(trackingWatchdog));
    trackingWatchdog = nullptr;
    ESP_ERROR_CHECK(esp_task_wdt_delete_user(trackingUser));
    trackingUser = nullptr;
}
}
