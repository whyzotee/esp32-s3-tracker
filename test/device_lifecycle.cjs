// Execute the complete power-off coordinator with a held button and fake sleep.
const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const root = path.resolve(__dirname,'..');
const dir = fs.mkdtempSync(path.join(os.tmpdir(),'tracker-lifecycle-'));
try {
    fs.writeFileSync(path.join(dir,'Arduino.h'), `
#pragma once
#include <cassert>
#include <vector>
constexpr int LOW=0;
inline std::vector<int> calls;
inline int holds=5;
inline bool silent=false;
inline int digitalRead(int) { return holds ? LOW : 1; }
inline void delay(int ms) {
    assert(silent); // Must already be silent throughout release wait.
    if(ms==10) --holds; else assert(ms==40);
}
inline struct { void println(const char*) {} } Serial;
`);
    fs.writeFileSync(path.join(dir,'audio_feedback.h'), `
#pragma once
namespace AudioFeedback { inline void shutdown() { calls.push_back(1); silent=true; } }
`);
    fs.writeFileSync(path.join(dir,'board_pins.h'),'#pragma once\nnamespace Board { constexpr int button=0; }\n');
    fs.writeFileSync(path.join(dir,'trip_control.h'), `
#pragma once
namespace TripControl { inline void reset() { assert(silent);calls.push_back(2); } }
`);
    fs.writeFileSync(path.join(dir,'fall_detection.h'), `
#pragma once
inline void acknowledge_fall_detection() { calls.push_back(3); }
`);
    fs.writeFileSync(path.join(dir,'deep_sleep.h'), `
#pragma once
struct Sleeping {};
namespace DeepSleep {
[[noreturn]] inline void powerOff() { assert(holds==0 && silent);calls.push_back(4);throw Sleeping{}; }
}
`);
    fs.writeFileSync(path.join(dir,'test.cpp'), `
#include <Arduino.h>
#include "device_lifecycle.cpp"
int main() {
    assert(!DeviceLifecycle::shuttingDown());
    try { DeviceLifecycle::powerOff(); assert(false); } catch(Sleeping&) {}
    assert(DeviceLifecycle::shuttingDown());
    assert((calls==std::vector<int>{1,2,3,4}));
}
`);
    const binary=path.join(dir,'test');
    cp.execFileSync('c++',['-std=c++17', '-DTRACKER_LOG_HOST_TEST', '-DTrackerLog=Serial' , '-DTRACKER_WEB_DEBUG=0','-I'+dir,'-I'+path.join(root,'include'),
        '-I'+path.join(root,'src'),path.join(dir,'test.cpp'),'-o',binary]);
    cp.execFileSync(binary);
    console.log('Power-off gate, audio-first ordering and silent held-button shutdown tests passed');
} finally { fs.rmSync(dir,{recursive:true,force:true}); }
