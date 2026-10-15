// Host test of the actual sleep coordinator with mocked hardware calls.
const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tracker-sleep-'));
try {
    fs.mkdirSync(path.join(dir, 'driver'));
    fs.writeFileSync(path.join(dir, 'mocks.h'), `
#pragma once
#include <cstdint>
#include <algorithm>
#include <vector>
using std::min;
using std::max;
enum { ESP_SLEEP_WAKEUP_ALL, ESP_SLEEP_WAKEUP_EXT0, ESP_SLEEP_WAKEUP_EXT1,
       ESP_SLEEP_WAKEUP_TIMER };
inline std::vector<int> calls;
inline bool sensorReady = true, sos = false, ext1 = false;
inline uint64_t timer = 0;
struct Sleeping {};
struct SerialMock {
    void println(const char *) {}
    template<class... T> void printf(const char *, T...) {}
    void flush() { calls.push_back(6); }
};
inline SerialMock Serial;
inline void delay(int) {}
namespace Board {
constexpr int button = 0;
inline void prepareSleep() { calls.push_back(5); }
}
namespace AudioFeedback {
enum class Event { DeepSleep };
inline void playAndWait(Event) { calls.push_back(4); }
}
namespace DeviceButton { inline bool sosPending() { return sos; } }
namespace TrackerApp { inline void endWatchdog() { calls.push_back(8); } }
namespace PowerMode {
inline bool enabled() { return false; }
inline uint32_t reportInterval() { return 60000; }
}
inline bool prepare_fall_detection_sleep() { ext1 = sensorReady; return sensorReady; }
inline void sleep_lora_radio() { calls.push_back(2); }
inline void stop_gnss() { calls.push_back(3); }
inline int esp_sleep_get_wakeup_cause() { return ESP_SLEEP_WAKEUP_TIMER; }
inline void esp_sleep_disable_wakeup_source(int source) {
    if (source == ESP_SLEEP_WAKEUP_ALL) { timer = 0; ext1 = false; calls.push_back(0); }
    if (source == ESP_SLEEP_WAKEUP_EXT1) ext1 = false;
}
inline void rtc_gpio_pullup_en(int) {}
inline void rtc_gpio_pulldown_dis(int) {}
inline void esp_sleep_enable_ext0_wakeup(int pin, int level) {
    if (pin != 0 || level != 0) throw 1;
    calls.push_back(1);
}
inline void esp_sleep_enable_timer_wakeup(uint64_t value) { timer = value; }
[[noreturn]] inline void esp_deep_sleep_start() { calls.push_back(7); throw Sleeping{}; }
`);
    for (const name of ['Arduino.h', 'audio_feedback.h', 'board_pins.h', 'device_button.h',
        'fall_detection.h', 'gnss.h', 'lorawan.h', 'power_mode.h', 'esp_sleep.h', 'driver/rtc_io.h']) {
        fs.writeFileSync(path.join(dir, name), '#include <mocks.h>\n');
    }
    fs.writeFileSync(path.join(dir, 'test.cpp'), `
#include <cassert>
#include "deep_sleep.cpp"
void reset() { calls.clear(); timer = 99; ext1 = true; sos = false; sensorReady = true; }
void checkShutdown() {
    assert((calls == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 8, 7}));
}
int main() {
    reset();
    try { DeepSleep::timed(15000); } catch (Sleeping &) {}
    assert(timer == 15000000 && ext1); checkShutdown();
    reset(); sos = true;
    try { DeepSleep::timed(60000, 60000); } catch (Sleeping &) {}
    assert(timer == 60000000 && ext1); checkShutdown();
    reset(); sensorReady = false; sos = true;
    try { DeepSleep::timed(208000, 208000); } catch (Sleeping &) {}
    assert(timer == 208000000 && !ext1); checkShutdown();
    reset();
    try { DeepSleep::timed(3600000); } catch (Sleeping &) {}
    assert(timer == 3600000000ULL && ext1); checkShutdown();
    reset(); sensorReady = false;
    try { DeepSleep::timed(900000); } catch (Sleeping &) {}
    assert(timer == 15000000 && !ext1); checkShutdown();
    reset(); sos = true;
    try { DeepSleep::timed(3600000); } catch (Sleeping &) {}
    assert(timer == 15000000 && ext1); checkShutdown();
    reset();
    try { DeepSleep::powerOff(); } catch (Sleeping &) {}
    assert(timer == 0 && !ext1); checkShutdown();
}
`);
    const root = path.resolve(__dirname, '..');
    const binary = path.join(dir, 'test');
    cp.execFileSync('c++', ['-std=c++17', `-I${dir}`,
        `-I${path.join(root, 'include')}`, `-I${path.join(root, 'src')}`,
        path.join(dir, 'test.cpp'), '-o', binary]);
    cp.execFileSync(binary);
    console.log('Deep-sleep timer, event retry, power-off and shutdown-order tests passed');
} finally {
    fs.rmSync(dir, { recursive: true, force: true });
}
