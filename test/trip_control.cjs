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
enum class Event { Sos, SosAcknowledged, Fall, LoRaJoining, LoRaConnected, Error };
inline int last = -1;
inline int receipts = 0;
inline int errors = 0;
inline void play(Event e) { if(e==Event::Error) ++errors; else { assert(e==Event::LoRaConnected); ++receipts; } }
inline void startLoop(Event e) { last = int(e); }
inline void stopLoop(Event e) { if(last==int(e))last=-1; }
inline void playAndWait(Event e) { play(e); }
}
`);
    fs.writeFileSync(path.join(dir, 'test.cpp'), `
#include <cassert>
#include "trip_control.cpp"
#include "audio_policy.cpp"
using namespace TripControl;
bool command(uint8_t value) { return handleDownlink(11, &value, 1); }
int main() {
    reset();
    assert(phase()==Phase::Preparing && keepAwake());
    AudioPolicy::refresh(); assert(AudioFeedback::last==int(AudioFeedback::Event::LoRaJoining));
    uint8_t malformed[]={1,0};
    assert(!handleDownlink(10,malformed,1));
    assert(!handleDownlink(11,nullptr,1));
    assert(!handleDownlink(11,malformed,0));
    assert(!handleDownlink(11,malformed,2));
    assert(!command(0) && !command(8));
    assert(command(5) && command(6) && AudioFeedback::receipts==0);
    assert(!command(3)); // Off-route is meaningful only during a trip.
    assert(command(1) && command(1)); // Start can queue before initial GPS fix.
    assert(phase()==Phase::Preparing);
    markInitialFix(); assert(phase()==Phase::Travelling);
    markLoRaConnected(); AudioPolicy::refresh();
    assert(AudioFeedback::receipts==1);
    AudioFeedback::receipts=0;
    setAcknowledged(false); assert(keepAwake());
    setAcknowledged(true); assert(!keepAwake());
    AudioPolicy::refresh(); assert(AudioFeedback::last==-1);
    assert(command(3) && offRoute() && keepAwake());
    AudioPolicy::refresh(); assert(AudioFeedback::last==int(AudioFeedback::Event::Fall));
    raiseAlarm(); const auto oldAlarm=revision();
    AudioPolicy::refresh(); assert(AudioFeedback::last==int(AudioFeedback::Event::Sos));
    setAcknowledged(true); assert(alarmActive()); // Network ACK never cancels sound.
    assert(command(5) && command(6)); AudioPolicy::refresh();
    assert(AudioFeedback::receipts==0);
    assert(alarmActive() && revision()==oldAlarm && !wellbeingPending());
    AudioPolicy::refresh(); assert(AudioFeedback::last==int(AudioFeedback::Event::SosAcknowledged));
    for(int i=0;i<10;++i) {
        AudioPolicy::refresh(); assert(AudioFeedback::last==int(AudioFeedback::Event::SosAcknowledged));
    }
    assert(command(5)); AudioPolicy::refresh(); // Duplicate ACK retains the same loop.
    assert(AudioFeedback::last==int(AudioFeedback::Event::SosAcknowledged));
    assert(cancelAlarm() && !alarmActive() && wellbeingPending());
    assert(command(5) && command(6) && AudioFeedback::receipts==0);
    assert(takeCancellation() && !takeCancellation());
    AudioPolicy::refresh(); assert(AudioFeedback::receipts==1);
    AudioPolicy::refresh(); assert(AudioFeedback::receipts==1); // Once per cancellation.
    assert(!cancelAlarm()); // Another plain click does not create another OK report.
    acknowledgeWellbeing(oldAlarm); assert(wellbeingPending());
    acknowledgeWellbeing(revision()); assert(!wellbeingPending());
    AudioPolicy::refresh(); assert(AudioFeedback::last==int(AudioFeedback::Event::Fall));
    assert(command(4) && !offRoute() && !keepAwake());
    raiseAlarm(); assert(cancelAlarm()); const auto oldOk=revision();
    AudioPolicy::refresh();
    raiseAlarm(); assert(!wellbeingPending()); assert(cancelAlarm());
    acknowledgeWellbeing(oldOk); assert(wellbeingPending()); // Old ACK cannot clear a newer cancellation.
    acknowledgeWellbeing(revision()); assert(!wellbeingPending());
    assert(command(2) && endRequested());
    reset(); assert(!endRequested() && !alarmActive() && !wellbeingPending());
    markInitialFix(); assert(phase()==Phase::Waiting && keepAwake());
    setAcknowledged(true); assert(!keepAwake()); // Await Start with timed Class A polling.
    assert(command(1) && phase()==Phase::Travelling && !keepAwake());
    reset(); markLoRaConnected(); markInitialFix();
    const int confirmations=AudioFeedback::receipts;
    AudioPolicy::refresh(); assert(phase()==Phase::Waiting && AudioFeedback::last==-1);
    assert(AudioFeedback::receipts==confirmations+1);
    setAcknowledged(false); AudioPolicy::refresh(); AudioPolicy::refresh();
    assert(AudioFeedback::last==-1 && AudioFeedback::receipts==confirmations+1);
    reset(); AudioPolicy::refresh(); preparationFailed(); AudioPolicy::refresh();
    assert(AudioFeedback::errors==1 && AudioFeedback::last==-1);
    AudioPolicy::refresh(); preparationFailed(); AudioPolicy::refresh();
    assert(AudioFeedback::errors==1 && AudioFeedback::last==-1);
    assert(retryPreparation()); AudioPolicy::refresh();
    assert(AudioFeedback::last==int(AudioFeedback::Event::LoRaJoining));
    assert(!retryPreparation());
    preparationFailed(); AudioPolicy::refresh(); assert(AudioFeedback::errors==2);
    assert(retryPreparation()); markLoRaConnected(); markInitialFix(); AudioPolicy::refresh();
    const int readyConfirmations=AudioFeedback::receipts;
    assert(phase()==Phase::Waiting && !retryPreparation()); AudioPolicy::refresh();
    assert(AudioFeedback::last==-1 && AudioFeedback::receipts==readyConfirmations);
    raiseAlarm(); AudioPolicy::refresh(); assert(AudioFeedback::last==int(AudioFeedback::Event::Sos));
    reset(); // Cancellation feedback also works during initial preparation.
    raiseAlarm(); AudioPolicy::refresh(); assert(cancelAlarm());
    const int beforeCancel=AudioFeedback::receipts;
    AudioPolicy::refresh(); assert(AudioFeedback::receipts==beforeCancel+1);
    AudioPolicy::refresh(); assert(AudioFeedback::receipts==beforeCancel+1);
    assert(!cancelAlarm()); AudioPolicy::refresh();
    assert(AudioFeedback::receipts==beforeCancel+1);
    raiseAlarm(); assert(cancelAlarm()); raiseAlarm();
    AudioPolicy::refresh(); assert(AudioFeedback::receipts==beforeCancel+1);
    reset();
    const int beforeNetwork=AudioFeedback::receipts;
    const int beforeErrors=AudioFeedback::errors;
    AudioPolicy::transmissionResult(true);
    AudioPolicy::transmissionResult(false);
    assert(AudioFeedback::receipts==beforeNetwork && AudioFeedback::errors==beforeErrors);
    markLoRaConnected(); markInitialFix(); // Ready, still awaiting Start.
    AudioPolicy::transmissionResult(true); AudioPolicy::transmissionResult(true);
    assert(AudioFeedback::receipts==beforeNetwork+2);
    AudioPolicy::transmissionResult(false); assert(AudioFeedback::errors==beforeErrors+1);
    raiseAlarm(); AudioPolicy::transmissionResult(true); AudioPolicy::transmissionResult(false);
    assert(AudioFeedback::receipts==beforeNetwork+2 && AudioFeedback::errors==beforeErrors+1);
    assert(cancelAlarm()); assert(command(1) && command(3));
    AudioPolicy::transmissionResult(true); AudioPolicy::transmissionResult(false);
    assert(AudioFeedback::receipts==beforeNetwork+2 && AudioFeedback::errors==beforeErrors+1);
    assert(command(4)); AudioPolicy::transmissionResult(true);
    assert(AudioFeedback::receipts==beforeNetwork+3);
    reset(); raiseAlarm(); assert(command(6)); AudioPolicy::refresh();
    assert(AudioFeedback::last==int(AudioFeedback::Event::SosAcknowledged));
    raiseAlarm(); AudioPolicy::refresh(); // A new emergency needs its own application ACK.
    assert(AudioFeedback::last==int(AudioFeedback::Event::Sos));
    assert(!takeFeedback().applicationAcknowledged);
    reset(); markLoRaConnected(); markInitialFix(); AudioPolicy::refresh();
    for (uint8_t ack=5; ack<=6; ++ack) {
        raiseAlarm(); assert(command(ack)); AudioPolicy::refresh();
        const auto before=revision();
        assert(command(7) && !alarmActive() && revision()!=before);
        AudioPolicy::refresh(); assert(AudioFeedback::last==-1);
        assert(!wellbeingPending() && !takeCancellation());
        assert(!takeFeedback().applicationAcknowledged);
        const auto resolved=revision();
        assert(command(7) && revision()==resolved); // Duplicate resolution is harmless.
        assert(command(ack)); AudioPolicy::refresh(); assert(AudioFeedback::last==-1);
    }
    assert(command(1) && command(3)); raiseAlarm(); assert(command(7));
    AudioPolicy::refresh();
    assert(offRoute() && phase()==Phase::Travelling);
    assert(AudioFeedback::last==int(AudioFeedback::Event::Fall)); // Route alarm is independent.
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
