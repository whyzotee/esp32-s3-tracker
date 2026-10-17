// Host tests of the real trip state machine and alarm/delivery separation.
const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tracker-trip-'));
try {
    fs.writeFileSync(path.join(dir, 'Arduino.h'), `
#pragma once
#include <cstdint>
#define RTC_DATA_ATTR
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
`);
    fs.writeFileSync(path.join(dir, 'audio_feedback.h'), `
#pragma once
namespace AudioFeedback {
enum class Event { Sos, Fall, LoRaJoining };
inline int last = -1;
inline void startLoop(Event e) { last = int(e); }
inline void silence() { last = -1; }
}
`);
    fs.writeFileSync(path.join(dir, 'test.cpp'), `
#include <cassert>
#include "trip_control.cpp"
using namespace TripControl;
bool command(uint8_t value) { return handleDownlink(11, &value, 1); }
int main() {
    reset();
    assert(phase()==Phase::Preparing && keepAwake());
    refreshAudio(); assert(AudioFeedback::last==int(AudioFeedback::Event::LoRaJoining));
    uint8_t malformed[]={1,0};
    assert(!handleDownlink(10,malformed,1));
    assert(!handleDownlink(11,nullptr,1));
    assert(!handleDownlink(11,malformed,0));
    assert(!handleDownlink(11,malformed,2));
    assert(!command(0) && !command(5));
    assert(!command(3)); // Off-route is meaningful only during a trip.
    assert(command(1) && command(1)); // Start can queue before initial GPS fix.
    assert(phase()==Phase::Preparing);
    markInitialFix(); assert(phase()==Phase::Travelling);
    setAcknowledged(false); assert(keepAwake());
    setAcknowledged(true); assert(!keepAwake());
    refreshAudio(); assert(AudioFeedback::last==-1);
    assert(command(3) && offRoute() && keepAwake());
    refreshAudio(); assert(AudioFeedback::last==int(AudioFeedback::Event::Fall));
    raiseAlarm(); const auto oldAlarm=revision();
    refreshAudio(); assert(AudioFeedback::last==int(AudioFeedback::Event::Sos));
    setAcknowledged(true); assert(alarmActive()); // Network ACK never cancels sound.
    assert(cancelAlarm() && !alarmActive() && wellbeingPending());
    assert(takeCancellation() && !takeCancellation());
    assert(!cancelAlarm()); // Another plain click does not create another OK report.
    acknowledgeWellbeing(oldAlarm); assert(wellbeingPending());
    acknowledgeWellbeing(revision()); assert(!wellbeingPending());
    refreshAudio(); assert(AudioFeedback::last==int(AudioFeedback::Event::Fall));
    assert(command(4) && !offRoute() && !keepAwake());
    raiseAlarm(); assert(cancelAlarm()); const auto oldOk=revision();
    raiseAlarm(); assert(!wellbeingPending()); assert(cancelAlarm());
    acknowledgeWellbeing(oldOk); assert(wellbeingPending()); // Old ACK cannot clear a newer cancellation.
    acknowledgeWellbeing(revision()); assert(!wellbeingPending());
    assert(command(2) && endRequested());
    reset(); assert(!endRequested() && !alarmActive() && !wellbeingPending());
    markInitialFix(); assert(phase()==Phase::Waiting && keepAwake());
    setAcknowledged(true); assert(keepAwake()); // Await application Start, even after successful ACK.
    assert(command(1) && phase()==Phase::Travelling && !keepAwake());
}
`);
    const root = path.resolve(__dirname, '..');
    const binary = path.join(dir, 'test');
    cp.execFileSync('c++', ['-std=c++17', `-I${dir}`, `-I${root}/include`,
        `-I${root}/src`, path.join(dir, 'test.cpp'), '-o', binary]);
    cp.execFileSync(binary);
    console.log('Trip command validation, initial fix gating, alarm cancellation and stale ACK tests passed');
} finally {
    fs.rmSync(dir, {recursive:true, force:true});
}
