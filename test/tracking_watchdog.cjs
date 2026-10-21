// Test hardware Task WDT subscription/heartbeat lifecycle with mocked ESP-IDF.
const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const assert = require('assert');
const root = path.resolve(__dirname, '..');
const source = fs.readFileSync(path.join(root, 'src/tracking_watchdog.cpp'), 'utf8');
assert(!source.includes('esp_task_wdt_reconfigure'));
assert(!source.includes('esp_task_wdt_init'));
assert(!source.includes('esp_restart('));
function extract(signature) {
    const start = source.indexOf(signature);
    assert(start >= 0);
    let end = source.indexOf('{', start) + 1, depth = 1;
    for (; depth; ++end) {
        if (source[end] === '{') ++depth;
        if (source[end] === '}') --depth;
    }
    return source.slice(start, end);
}
const main = fs.readFileSync(path.join(root, 'src/main.cpp'), 'utf8');
assert(main.indexOf('TrackerApp::beginWatchdog();') < main.indexOf('DeviceButton::begin();'));
const ota = fs.readFileSync(path.join(root, 'src/ota_manager.cpp'), 'utf8');
assert(ota.indexOf('TrackerApp::endWatchdog();') < ota.indexOf('DeviceButton::acknowledgeOta();'));
const sleep = fs.readFileSync(path.join(root, 'src/deep_sleep.cpp'), 'utf8');
assert(sleep.indexOf('TrackerApp::endWatchdog();') < sleep.indexOf('esp_deep_sleep_start();'));
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tracker-wdt-'));
try {
    fs.writeFileSync(path.join(dir, 'test.cpp'), `
#include <cassert>
#include <stdexcept>
#include <mutex>
#include <tracker_app.h>
#define ESP_ERROR_CHECK(call) do { if ((call) != 0) throw std::runtime_error("WDT API error"); } while(0)
using esp_timer_handle_t = void*;
using esp_err_t = int;
constexpr int ESP_TIMER_TASK=0, ESP_ERR_INVALID_STATE=-2;
struct esp_timer_create_args_t {
    void (*callback)(void*)=nullptr;
    int dispatch_method=0;
    const char* name=nullptr;
};
esp_timer_create_args_t configured;
esp_timer_handle_t trackingWatchdog=nullptr;
using esp_task_wdt_user_handle_t=void*;
esp_task_wdt_user_handle_t trackingUser=nullptr;
std::mutex trackingWatchdogMutex;
int64_t trackingStartedUs=0;
uint64_t now=0, armedAt=0, duration=0;
int creates=0, starts=0, deletes=0, restarts=0, error=0;
bool active=false, subscribed=false, timerService=true, expired=false;
uint64_t lastFeed=0;
int feeds=0;
int64_t esp_timer_get_time() { return now; }
int esp_task_wdt_add_user(const char*, esp_task_wdt_user_handle_t* handle) {
    *handle=reinterpret_cast<void*>(2); subscribed=true; lastFeed=now; return 0;
}
int esp_task_wdt_reset_user(esp_task_wdt_user_handle_t handle) {
    assert(subscribed && handle==trackingUser); ++feeds; lastFeed=now; return 0;
}
int esp_task_wdt_delete_user(esp_task_wdt_user_handle_t) { subscribed=false; return 0; }
int esp_timer_create(const esp_timer_create_args_t* config, esp_timer_handle_t* handle) {
    if (error) return error;
    ++creates; configured=*config; *handle=reinterpret_cast<void*>(1); return 0;
}
int esp_timer_start_periodic(esp_timer_handle_t, uint64_t us) {
    ++starts; active=true; armedAt=now; duration=us; return 0;
}
int esp_timer_stop(esp_timer_handle_t) { active=false; return 0; }
int esp_timer_delete(esp_timer_handle_t) { ++deletes; return 0; }
void advance(uint64_t us) {
    assert(us%1000000==0);
    for (; us; us-=1000000) {
        now+=1000000;
        if (active && timerService) configured.callback(nullptr);
        if (subscribed && !expired && now-lastFeed>=5000000) { expired=true; ++restarts; }
    }
}
struct { template<class... T> void printf(const char*, T...) {} } Serial;
${extract('void feedTrackingWatchdog(void *)')}
namespace TrackerApp {
${extract('void beginWatchdog()')}
${extract('void endWatchdog()')}
}
int main() {
    TrackerApp::endWatchdog(); assert(deletes==0);
    TrackerApp::beginWatchdog();
    assert(creates==1 && starts==1 && duration==1000000 && subscribed);
    advance(120000000); assert(restarts==0); // First GNSS timeout is allowed.
    TrackerApp::beginWatchdog(); assert(starts==1); // Does NOT extend a stuck cycle.
    advance(180000000); assert(restarts==0 && lastFeed==299000000);
    advance(5000000); assert(restarts==1); // Hardware timeout after feeding stops.
    TrackerApp::endWatchdog(); assert(deletes==1 && !active);
    advance(900000000); assert(restarts==1);
    TrackerApp::beginWatchdog(); assert(creates==2 && active);
    expired=false; timerService=false;
    advance(5000000); assert(restarts==2); // Timer service stalls: hardware still expires.
    TrackerApp::endWatchdog(); TrackerApp::endWatchdog(); assert(deletes==2);
    feedTrackingWatchdog(nullptr); // Queued callback after cancellation must not feed freed user.
    advance(900000000); assert(restarts==2); // Sleep/OTA remove tracker subscription.
    error=-1;
    try { TrackerApp::beginWatchdog(); assert(false); } catch (const std::runtime_error&) {}
    assert(creates==2 && trackingWatchdog==nullptr); // Fail visibly rather than run unprotected.
}
`);
    const binary = path.join(dir, 'test');
    cp.execFileSync('c++', ['-std=c++17', '-DTRACKER_LOG_HOST_TEST', '-DTrackerLog=Serial' , '-DTRACKER_WEB_DEBUG=0', '-I', path.join(root, 'include'), path.join(dir, 'test.cpp'), '-o', binary]);
    cp.execFileSync(binary);
    console.log('Tracking watchdog lifecycle, deadline simulation and OTA/sleep integration checks passed');
} finally {
    fs.rmSync(dir, { recursive: true, force: true });
}
