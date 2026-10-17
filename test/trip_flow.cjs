// Exercise the production cycle dispatcher against the real trip state machine.
const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const root = path.resolve(__dirname, '..');
const source = fs.readFileSync(path.join(root, 'src/tracker_app.cpp'), 'utf8');
function extract(signature) {
    const start = source.indexOf(signature);
    if (start < 0) throw new Error(signature);
    let end = source.indexOf('{', start) + 1, depth = 1;
    for (; depth; ++end) {
        if (source[end] === '{') ++depth;
        if (source[end] === '}') --depth;
    }
    return source.slice(start, end);
}
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tracker-trip-flow-'));
try {
    fs.writeFileSync(path.join(dir, 'Arduino.h'), `
#pragma once
#include <cstdint>
#define RTC_DATA_ATTR
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
`);
    fs.writeFileSync(path.join(dir, 'audio_feedback.h'), `
#pragma once
namespace AudioFeedback {
enum class Event { Sos, Fall, LoRaJoining, LoRaConnected };
inline void startLoop(Event) {}
inline void silence() {}
inline void playAndWait(Event) {}
}
`);
    fs.writeFileSync(path.join(dir, 'test.cpp'), `
#include <cassert>
#include <algorithm>
#include <functional>
#include <tracker_app.h>
#include <lorawan.example.h>
#include "trip_control.cpp"
using std::max;
bool sos=false, fall=false, goodFix=true, joined=true;
uint32_t slept=0, minimum=0, radioWait=0;
int acquisitions=0;
Telemetry sent{};
LoRaResult response{};
std::function<void()> afterSend;
struct PoweredOff {};
struct { template<class... T> void printf(const char*,T...) {} void println(const char* = "") {} } Serial;
namespace DeviceButton {
void service() { if (TripControl::takeCancellation()) fall=false; }
bool sosPending() { return sos; }
bool otaPending() { return false; }
void acknowledgeSos() { sos=false; }
[[noreturn]] void powerOff() { TripControl::reset(); throw PoweredOff{}; }
}
namespace DebugMode {
bool enabled() { return false; }
void update(bool) {}
void message(const char*) {}
void result(int) {}
Telemetry walking_sample(uint8_t status) { Telemetry s{}; s.status=status; return s; }
}
namespace PowerMode {
bool enabled() { return false; }
bool handleDownlink(uint8_t,const uint8_t*,size_t) { return false; }
}
namespace Battery {
struct Reading { uint16_t millivolts=4000; uint8_t percent=75; bool valid=true, low=false; };
Reading bootReading() { return {}; }
}
using BatteryReading = Battery::Reading;
void update_fall_detection(bool) {}
bool fall_detection_pending() { return fall; }
void acknowledge_fall_detection() { fall=false; }
bool setup_lora_wan_app() { return joined; }
uint32_t lora_wait_ms() { return radioWait; }
Telemetry read_telemetry(uint8_t status) {
    Telemetry s{}; s.status=status; s.lat=13; s.lon=100;
    if (status==0 || status==4) { ++acquisitions; if (goodFix) s.flags|=TelemetryFlags::gpsFresh; }
    return s;
}
LoRaResult send_lora_telemetry(const Telemetry &s) { sent=s; if(afterSend) afterSend(); return response; }
uint32_t reportInterval() { return AppConfig::reportIntervalMs; }
void startOtaIfRequested() { assert(false); }
void sleepFor(uint32_t value,uint32_t floor=0) { slept=value; minimum=floor; }
${extract('void handleDownlink(const LoRaResult &result)')}
${extract('uint8_t nextStatus()')}
${extract('Telemetry makeSample(uint8_t status, bool debug)')}
namespace TrackerApp {
void beginWatchdog() {}
${extract('void runCycle()')}
}
void reset() {
    TripControl::reset(); sos=fall=false; goodFix=joined=true;
    acquisitions=0; slept=minimum=radioWait=0; afterSend={};
    response={}; response.code=1; response.acknowledged=response.sessionSaved=true;
}
void command(uint8_t value) { assert(TripControl::handleDownlink(11,&value,1)); }
int main() {
    reset(); TrackerApp::runCycle();
    assert(sent.status==4 && acquisitions==1 && TripControl::phase()==TripControl::Phase::Waiting);
    assert(sent.flags & TelemetryFlags::initialFixReady);
    assert(!(sent.flags & TelemetryFlags::tripActive));
    TrackerApp::runCycle(); assert(acquisitions==1); // Ready waits do not re-block on GPS.
    command(1); TrackerApp::runCycle(); assert(sent.status==0 && (sent.flags & TelemetryFlags::tripActive));
    sos=true; TripControl::raiseAlarm(); TrackerApp::runCycle();
    assert(sent.status==1 && !sos && TripControl::alarmActive()); // ACK clears delivery, not audible alarm.
    assert(TripControl::cancelAlarm()); TrackerApp::runCycle();
    assert(sent.status==3 && !TripControl::wellbeingPending() && !TripControl::alarmActive());
    reset(); command(1); TrackerApp::runCycle();
    assert(sent.status==0 && acquisitions==1); // Early Start uses the initial fix once, not two searches.
    reset(); goodFix=false; command(1); TrackerApp::runCycle();
    assert(sent.status==4 && TripControl::phase()==TripControl::Phase::Preparing);
    reset(); fall=true; TripControl::raiseAlarm(); response.code=0; response.acknowledged=false;
    TrackerApp::runCycle(); assert(sent.status==2 && fall && minimum>=AppConfig::retryInitialMs);
    response.code=1; response.acknowledged=true; TrackerApp::runCycle();
    assert(!fall && TripControl::alarmActive());
    reset(); sos=fall=true; TripControl::raiseAlarm(); TrackerApp::runCycle();
    assert(sent.status==2 && !fall && sos); TrackerApp::runCycle(); assert(sent.status==1 && !sos);
    reset(); sos=true; TripControl::raiseAlarm();
    afterSend=[] { assert(TripControl::cancelAlarm()); sos=false; };
    TrackerApp::runCycle(); assert(TripControl::wellbeingPending());
    afterSend={}; TrackerApp::runCycle(); assert(sent.status==3 && !TripControl::wellbeingPending());
    reset(); response.downlinkPort=11; response.downlinkLength=1; response.downlink[0]=2;
    try { TrackerApp::runCycle(); assert(false); } catch(PoweredOff&) {}
    assert(!TripControl::initialFixReady());
    reset(); joined=false; TrackerApp::runCycle(); assert(slept==AppConfig::retryInitialMs);
    reset(); radioWait=208000; TrackerApp::runCycle(); assert(slept==208000 && minimum==208000);
}
`);
    const binary = path.join(dir, 'test');
    cp.execFileSync('c++', ['-std=c++17', `-I${dir}`, `-I${root}/include`,
        `-I${root}/src`, path.join(dir, 'test.cpp'), '-o', binary]);
    cp.execFileSync(binary);
    console.log('Production trip cycle, initial fix, SOS/Fall/OK priority, retry and End Trip tests passed');
} finally {
    fs.rmSync(dir, {recursive:true, force:true});
}
