// Compile the complete production dispatcher, trip state and audio policy.
const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const root = path.resolve(__dirname, '..');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tracker-trip-flow-'));
try {
    fs.writeFileSync(path.join(dir, 'debug_web.h'), `
#pragma once
#define TRACKER_WEB_DEBUG 1
#include "${root}/include/debug_web.h"
namespace DebugWeb {
inline bool webOn=false;
inline int simulatedSleeps=0;
inline bool active(){return webOn;}
inline void publish(const State&){}
inline void joined(bool){}
inline void transmission(int16_t,bool,uint8_t){}
inline void telemetry(float,float,uint8_t,uint8_t){}
inline void note(const char*){++simulatedSleeps;}
}
`);
    for (const name of ['battery.h', 'board_pins.h', 'debug_mode.h', 'deep_sleep.h',
        'device_button.h', 'fall_detection.h', 'ota_manager.h', 'power_mode.h', 'gnss.h'])
        fs.writeFileSync(path.join(dir, name), '#pragma once\n');
    fs.writeFileSync(path.join(dir, 'lorawan.h'), '#pragma once\n#include <lorawan.example.h>\n');
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
enum class Event { Sos, SosAcknowledged, Fall, LoRaJoining, LoRaConnected, Error };
inline void startLoop(Event) {}
inline void stopLoop(Event) {}
inline int successSounds=0, errorSounds=0;
inline void playAndWait(Event event) {
    if(event==Event::LoRaConnected) ++successSounds;
    if(event==Event::Error) ++errorSounds;
}
inline void play(Event) {}
}
`);
    fs.writeFileSync(path.join(dir, 'test.cpp'), `
#include <cassert>
#include <algorithm>
#include <functional>
#include <cstring>
#include <type_traits>
#include <tracker_app.h>
#include <lorawan.example.h>
#include "trip_control.cpp"
#include "audio_policy.cpp"
using std::max;
bool sos=false, fall=false, goodFix=true, joined=true;
uint32_t slept=0, radioWait=0, now=0;
int deepSleeps=0;
uint32_t millis() { return now; }
void delay(uint32_t value) { now+=value; }
int acquisitions=0;
Telemetry sent{};
LoRaResult response{};
std::function<void()> afterSend;
struct PoweredOff {};
struct {
    template<class T, class... Rest> void printf(const char* format,T first,Rest...) {
        if constexpr (std::is_arithmetic_v<T>)
            if (strstr(format,"[WAIT]")) slept=uint32_t(first);
    }
    void println(const char* = "") {}
} Serial;
namespace DeviceButton {
void service() { if (TripControl::takeCancellation()) fall=false; }
bool sosPending() { return sos; }
bool otaPending() { return false; }
void acknowledgeSos() { sos=false; }
[[noreturn]] void powerOff() { TripControl::reset(); throw PoweredOff{}; }
}
namespace DebugMode {
bool enabled() { return false; }
uint32_t interval() { return 15000; }
void update(bool) {}
void message(const char*) {}
void result(int) {}
Telemetry walking_sample(uint8_t status) { Telemetry s{}; s.status=status; return s; }
}
namespace PowerMode {
bool enabled() { return false; }
uint32_t reportInterval() { return AppConfig::reportIntervalMs; }
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
bool infoDue=false;
int infoSends=0;
bool lora_device_info_due() { return infoDue; }
LoRaResult send_lora_device_info(const Telemetry&) { ++infoSends; infoDue=false; return response; }
void start_gnss_acquisition(bool = false) {}
void service_gnss() {}
enum class GnssStatus { Unavailable, Searching, FixReady, TimedOut };
GnssStatus gnss_status() { return goodFix && acquisitions > 0 ? GnssStatus::FixReady : GnssStatus::Searching; }
uint32_t lora_wait_ms() { return radioWait; }
Telemetry read_telemetry(uint8_t status) {
    Telemetry s{}; s.status=status; s.lat=13; s.lon=100;
    if (status==0 || status==4) { ++acquisitions; if (goodFix) s.flags|=TelemetryFlags::gpsFresh; }
    return s;
}
LoRaResult send_lora_telemetry(const Telemetry &s) { sent=s; if(afterSend) afterSend(); return response; }
struct Sleeping {};
namespace DeepSleep {
[[noreturn]] void timed(uint32_t value,uint32_t floor) {
    ++deepSleeps; assert(value>=floor); slept=value; throw Sleeping{};
}
}
namespace OtaManager {
[[noreturn]] void run(uint32_t) { assert(false); throw 1; }
}
namespace TrackerApp {
void beginWatchdog() {}
void endWatchdog() {}
}
#include "tracker_app.cpp"
void cycle() { try { TrackerApp::runCycle(); } catch (Sleeping&) {} }
void reset() {
    TripControl::reset(); sos=fall=false; goodFix=joined=true;
    DebugWeb::webOn=false; DebugWeb::simulatedSleeps=0; deepSleeps=0;
    acquisitions=0; slept=radioWait=now=0; afterSend={};
    infoDue=false; infoSends=0;
    response={}; response.code=1; response.acknowledged=response.sessionSaved=true;
}
void command(uint8_t value) { assert(TripControl::handleDownlink(11,&value,1)); }
int main() {
    reset(); cycle();
    assert(sent.status==4 && acquisitions==1 && TripControl::phase()==TripControl::Phase::Waiting);
    assert(sent.flags & TelemetryFlags::initialFixReady);
    assert(!(sent.flags & TelemetryFlags::tripActive));
    assert(deepSleeps==1); // Ready + ACK sleeps even before application Start.
    cycle(); assert(acquisitions==1); // Ready waits do not re-block on GPS.
    command(1); cycle(); assert(sent.status==0 && (sent.flags & TelemetryFlags::tripActive));
    sos=true; TripControl::raiseAlarm(); cycle();
    assert(sent.status==1 && !sos && TripControl::alarmActive()); // ACK clears delivery, not audible alarm.
    assert(TripControl::cancelAlarm()); cycle();
    assert(sent.status==3 && !TripControl::wellbeingPending() && !TripControl::alarmActive());
    reset(); command(1); cycle();
    assert(sent.status==0 && acquisitions==1); // Early Start uses the initial fix once, not two searches.
    reset(); goodFix=false; command(1); cycle();
    assert(sent.status==4 && TripControl::phase()==TripControl::Phase::Preparing);
    reset(); fall=true; TripControl::raiseAlarm(); response.code=0; response.acknowledged=false;
    cycle(); assert(sent.status==2 && fall && slept>=AppConfig::retryInitialMs);
    response.code=1; response.acknowledged=true; cycle();
    assert(!fall && TripControl::alarmActive());
    reset(); sos=fall=true; TripControl::raiseAlarm(); cycle();
    assert(sent.status==2 && !fall && sos); cycle(); assert(sent.status==1 && !sos);
    reset(); sos=true; TripControl::raiseAlarm();
    afterSend=[] { assert(TripControl::cancelAlarm()); sos=false; };
    cycle(); assert(TripControl::wellbeingPending());
    afterSend={}; cycle(); assert(sent.status==3 && !TripControl::wellbeingPending());
    reset(); response.downlinkPort=11; response.downlinkLength=1; response.downlink[0]=2;
    try { cycle(); assert(false); } catch(PoweredOff&) {}
    assert(!TripControl::initialFixReady());
    reset(); joined=false; cycle(); assert(slept==AppConfig::retryInitialMs);
    for (int event=1;event<=2;++event) {
        reset(); command(1); cycle();
        sos=event==1; fall=event==2; TripControl::raiseAlarm();
        response.code=1; response.acknowledged=false;
        response.downlinkPort=11; response.downlinkLength=1; response.downlink[0]=7;
        cycle();
        assert(!sos && !fall && !TripControl::alarmActive());
        assert(!TripControl::wellbeingPending() && TripControl::phase()==TripControl::Phase::Travelling);
        response.downlinkLength=0; response.acknowledged=true;
        cycle(); assert(sent.status==0); // No stale SOS/Fall or synthetic I-am-OK retry.
    }
    reset(); radioWait=208000; cycle(); assert(slept==208000);
    reset(); infoDue=true; cycle(); assert(infoSends==1 && acquisitions==0);
    reset(); infoDue=true; sos=true; TripControl::raiseAlarm(); cycle();
    assert(infoSends==0 && sent.status==1);
    reset(); infoDue=true; fall=true; TripControl::raiseAlarm(); cycle();
    assert(infoSends==0 && sent.status==2);
    reset(); infoDue=true; TripControl::raiseAlarm(); assert(TripControl::cancelAlarm());
    cycle(); assert(infoSends==0 && sent.status==3);
    reset(); command(1); cycle();
    assert(deepSleeps==1); // Production mode still really sleeps.
    reset(); DebugWeb::webOn=true; command(1); cycle();
    assert(deepSleeps==0 && DebugWeb::simulatedSleeps==1);
    assert(slept==AppConfig::reportIntervalMs && now>=slept);
    radioWait=208000; cycle();
    assert(deepSleeps==0 && slept==208000); // Debug never bypasses radio cooldown.
    response.downlinkPort=11;response.downlinkLength=1;response.downlink[0]=2;
    radioWait=0;
    try{cycle();assert(false);}catch(PoweredOff&){}
    assert(!TripControl::initialFixReady()); // End Trip still powers off.
    reset(); command(1); cycle();
    response={}; response.code=0; response.sessionSaved=true; // No downlink/ACK.
    const int successBefore=AudioFeedback::successSounds;
    const int errorsBefore=AudioFeedback::errorSounds;
    deepSleeps=0; cycle();
    assert(sent.status==0 && !response.acknowledged);
    assert(AudioFeedback::successSounds==successBefore+1 && AudioFeedback::errorSounds==errorsBefore);
    assert(deepSleeps==1 && slept==AppConfig::reportIntervalMs);
    assert(!TripControl::keepAwake()); // No artificial retry or awake wait for a missing ACK.
    response.code=-4; deepSleeps=0; cycle();
    assert(AudioFeedback::errorSounds==errorsBefore+1);
    assert(deepSleeps==0 && slept>=AppConfig::retryInitialMs && TripControl::keepAwake());
}
`);
    const binary = path.join(dir, 'test');
    cp.execFileSync('c++', ['-std=c++17', '-DTRACKER_LOG_HOST_TEST', '-DTrackerLog=Serial', `-I${dir}`, `-I${root}/include`,
        `-I${root}/src`, path.join(dir, 'test.cpp'), '-o', binary]);
    cp.execFileSync(binary);
    console.log('Production trip cycle, initial fix, SOS/Fall/OK priority, retry and End Trip tests passed');
} finally {
    fs.rmSync(dir, {recursive:true, force:true});
}
