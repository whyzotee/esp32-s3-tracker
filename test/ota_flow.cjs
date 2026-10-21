// Execute the production OTA run loop with fake network/update peripherals.
const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const source = fs.readFileSync(path.join(__dirname,'../src/ota_manager.cpp'),'utf8');
const assert = require('assert');
assert(source.includes('"Tracker-OTA-%06llX"'));
assert(source.includes('static_cast<unsigned long long>(LoRaCredentials::devEui >> 40)'));
assert(!source.includes('getEfuseMac'));
const start=source.indexOf('[[noreturn]] void run(uint32_t sleepIntervalMs)');
let end=source.indexOf('{',start)+1, depth=1;
for (;depth;++end) {
    if(source[end]==='{')++depth;
    if(source[end]==='}')--depth;
}
const dir=fs.mkdtempSync(path.join(os.tmpdir(),'tracker-ota-'));
try {
    const binary=path.join(dir,'test');
    cp.execFileSync('c++',['-std=c++17', '-DTRACKER_LOG_HOST_TEST', '-DTrackerLog=Serial' , '-DTRACKER_WEB_DEBUG=0','-x','c++','-','-o',binary],{input:`
#include <cassert>
#include <cstdint>
#include <vector>
namespace DebugWeb {void stop(){}}
constexpr int HIGH=1, LOW=0;
uint32_t now=100,lastActivityAt=100,rebootAt=0;
constexpr uint32_t idleTimeoutMs=900000,rebootDelayMs=1000;
int scenario=0,clients=0,polls=0,stops=0;
std::vector<int> calls;
struct Off{};struct Sleep{};struct Restart{};
uint32_t millis(){return now;}
void delay(int){now+=idleTimeoutMs+1;}
void digitalWrite(int,int){}
struct {void println(const char*){}void flush(){}} Serial;
struct {void handleClient(){++clients;}} server;
struct {[[noreturn]] void restart(){throw Restart{};}} ESP;
namespace TrackerApp {void endWatchdog(){calls.push_back(1);}}
namespace DeviceButton {
void acknowledgeOta(){calls.push_back(2);}
void servicePowerOff(){++polls;if(scenario==0)throw Off{};}
}
void sleep_lora_radio(){calls.push_back(3);}
void stop_gnss(){calls.push_back(4);}
namespace Board {constexpr int statusLed=40;void setVext(bool on){assert(!on);calls.push_back(5);}}
namespace AudioFeedback {
enum class Event{OtaMode};
void playAndWait(Event){calls.push_back(6);}
}
bool startAp(){calls.push_back(7);return scenario!=1;}
void stopAp(){++stops;}
namespace DeepSleep {
[[noreturn]] void timed(uint32_t interval){assert(interval==30000);throw Sleep{};}
}
namespace OtaManager {
${source.slice(start,end)}
}
void reset(int test){
    scenario=test;calls.clear();clients=polls=stops=0;
    now=lastActivityAt=100;rebootAt=0;
}
int main(){
    reset(0);
    try{OtaManager::run(30000);assert(false);}catch(Off&){}
    assert(polls==1 && clients==0);
    assert((calls==std::vector<int>{1,2,3,4,5,6,7}));
    reset(1);
    try{OtaManager::run(30000);assert(false);}catch(Sleep&){}
    assert(polls==0 && clients==0);
    reset(2);
    try{OtaManager::run(30000);assert(false);}catch(Sleep&){}
    assert(polls==2 && clients==2 && stops==1);
    reset(3);rebootAt=1;now=2000;
    try{OtaManager::run(30000);assert(false);}catch(Restart&){}
    assert(polls==1 && clients==1);
}
`});
    cp.execFileSync(binary);
    console.log('OTA peripheral/audio ordering, hold-to-off, idle timeout and reboot tests passed');
} finally {fs.rmSync(dir,{recursive:true,force:true});}
