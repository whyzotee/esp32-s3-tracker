// Host regression of the actual ADXL362 driver against a register-level SPI mock.
const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'adxl362-test-'));
const root = path.resolve('.');
fs.mkdirSync(path.join(dir, 'driver'));
fs.writeFileSync(path.join(dir, 'audio_feedback.h'), `
#pragma once
namespace AudioFeedback {
enum class Event { Fall };
inline void play(Event) { ++toneCalls; }
}
`);
fs.writeFileSync(path.join(dir, 'Arduino.h'), `
#pragma once
#include <cstdint>
#include <cstddef>
#include <cassert>
#define RTC_DATA_ATTR
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLDOWN 2
#define INPUT_PULLUP 3
#define LOW 0
#define HIGH 1
using gpio_num_t = int;
constexpr int GPIO_NUM_0=0, GPIO_NUM_7=7;
constexpr int ESP_OK=0, ESP_EXT1_WAKEUP_ANY_HIGH=1;
inline uint32_t now=0;
inline uint64_t wakeMask=0;
inline int wakeResult=0;
inline int toneCalls=0;
inline void (*delayHook)()=nullptr;
inline void pinMode(int,int) {}
inline void digitalWrite(int,int) {}
inline int digitalRead(int) { return LOW; }
inline void tone(int pin,int frequency,unsigned long duration) {
    assert(pin==4 && frequency==2700 && duration==2000); ++toneCalls;
}
inline void noTone(int) {}
inline uint32_t millis() { return now; }
inline void delay(int n) { now+=n; if(delayHook) delayHook(); }
inline int esp_sleep_enable_ext1_wakeup(uint64_t mask,int mode) {
    assert(mode==ESP_EXT1_WAKEUP_ANY_HIGH); wakeMask=mask; return wakeResult;
}
inline struct { void println(const char*) {} template<class... T> void printf(const char*,T...) {} } Serial;
`);
fs.writeFileSync(path.join(dir, 'driver/rtc_io.h'), `
inline void rtc_gpio_deinit(int) {}
inline void rtc_gpio_pulldown_en(int) {}
inline void rtc_gpio_pullup_dis(int) {}
`);
fs.writeFileSync(path.join(dir, 'SPI.h'), `
#pragma once
#include <Arduino.h>
#include <deque>
#define HSPI 1
#define MSBFIRST 1
#define SPI_MODE0 0
struct SPISettings { SPISettings(int speed,int order,int mode) {
    assert(speed==1000000 && order==MSBFIRST && mode==SPI_MODE0);
} };
inline uint8_t regs[256]={};
inline bool disconnected=false;
inline int failWrite=-1;
inline int statusReads=0;
inline std::deque<uint8_t> fifo;
class SPIClass {
    int phase=0; uint8_t command=0, address=0;
public:
    SPIClass(int bus) { assert(bus==HSPI); }
    void begin(int sck,int miso,int mosi,int cs) {
        assert(sck==37 && miso==34 && mosi==35 && cs==33);
    }
    void beginTransaction(SPISettings) { phase=0; }
    void endTransaction() {}
    uint8_t transfer(uint8_t value) {
        if(phase++==0) { command=value; return 0; }
        if(command==0x0D) {
            if(fifo.empty()) return 0;
            const auto result=fifo.front(); fifo.pop_front(); return result;
        }
        if(phase==2) { address=value; return 0; }
        if(disconnected) return 0xFF;
        if(command==0x0A) {
            if(address!=failWrite) {
                regs[address]=value;
                if(address==0x28 && value==0) fifo.clear();
                if(address>=0x20 && address<=0x2E) regs[0x0B]&=~0x80;
            }
            ++address; return 0;
        }
        assert(command==0x0B);
        uint8_t result=regs[address];
        if(address==0x0C) result=(fifo.size()/2)&0xFF;
        if(address==0x0D) result=(fifo.size()/2)>>8;
        if(address==0x0B) { ++statusReads; regs[address]&=~0x30; }
        ++address; return result;
    }
};
`);
fs.writeFileSync(path.join(dir, 'test.cpp'), `
#include "${root}/src/fall_detection.cpp"
void resetSensor() {
    for(auto &r:regs) r=0;
    regs[0]=0xAD; regs[1]=0x1D; regs[2]=0xF2;
    disconnected=false; failWrite=-1; pending=false; wakeMask=0; wakeResult=0;
    fifo.clear(); resetClassifier(); toneCalls=0; delayHook=nullptr;
}
void sample(int x,int y,int z) {
    const int values[]={x,y,z};
    for(int axis=0;axis<3;++axis) {
        const uint16_t word=(uint16_t(values[axis])&0x3FFF)|(axis<<14);
        fifo.push_back(word&255); fifo.push_back(word>>8);
    }
}
void low(int n=FALL_DURATION_SAMPLES) { for(int i=0;i<n;++i) sample(0,0,0); }
int main() {
    resetSensor();
    regs[0x0B]=0x80; // ERR_USER_REGS is expected before first configuration write.
    assert(setup_fall_detection());
    assert((regs[0x0B]&0x80)==0);
    regs[0x0B]=0x20; // A hardware latch alone must NOT produce an alert.
    assert(setup_fall_detection());
    assert(!fall_detection_pending());
    assert(regs[0x23]==93 && regs[0x24]==0); // <=375mg at +/-8g, 4mg/LSB.
    assert(regs[0x25]==8 && regs[0x26]==0);
    assert(regs[0x27]==4 && regs[0x2A]==0x20 && regs[0x2B]==0);
    assert(regs[0x2C]==0x82 && regs[0x2D]==2 && regs[0x28]==2);
    // Historical FIFO survives a simulated wake before configuration clears it.
    low(); sample(0,0,800); regs[0x0B]=0x20;
    assert(setup_fall_detection());
    assert(fall_detection_pending());
    assert(!prepare_fall_detection_sleep());
    acknowledge_fall_detection();
    assert(prepare_fall_detection_sleep());
    assert(wakeMask==(1ULL<<7));
    low(); sample(-800,0,0); regs[0x0B]=0x20; // Negative impact, blocking TX/GPS history.
    assert(!prepare_fall_detection_sleep());
    assert(fall_detection_pending());
    acknowledge_fall_detection();
    disconnected=true;
    assert(!prepare_fall_detection_sleep());
    disconnected=false;
    regs[0x0B]=0x80; // Register protection error.
    assert(!prepare_fall_detection_sleep());
    regs[0x0B]=0;
    wakeResult=-1;
    assert(!prepare_fall_detection_sleep());
    resetSensor(); failWrite=0x2A;
    assert(!setup_fall_detection());
    resetSensor(); disconnected=true;
    assert(!setup_fall_detection());
    assert(prepare_fall_detection_sleep()); // Missing sensor: no wake armed.
    assert(wakeMask==0);
    resetSensor(); assert(setup_fall_detection());
    // Walking / swinging with no low vector interval; impacts alone are rejected.
    sample(0,0,250); sample(800,0,0); now+=1000;
    update_fall_detection(false); assert(!pending && toneCalls==0);
    // Free fall without impact times out silently, even if hardware latch is set.
    low(); regs[0x0B]=0x20; now+=1000;
    update_fall_detection(false); assert(!pending && toneCalls==0);
    // Too short a low interval, then impact: reject.
    low(7); sample(0,0,800); now+=1000;
    update_fall_detection(false); assert(!pending);
    // Individual axes below threshold but vector above it must not qualify.
    for(int i=0;i<8;++i) sample(75,75,75);
    sample(0,0,800); now+=1000;
    update_fall_detection(false); assert(!pending);
    // Impact before the low interval must not qualify.
    sample(0,0,800); low(); now+=1000;
    update_fall_detection(false); assert(!pending);
    // Impact after 1 second of samples must not qualify.
    low(); for(int i=0;i<51;++i) sample(0,0,250);
    sample(0,0,800); now+=1000;
    update_fall_detection(false); assert(!pending);
    // Partial XYZ data / temperature tags must not fabricate a sequence.
    fifo.push_back(0); fifo.push_back(0x80);
    low(); sample(0,0,749); now+=1000;
    update_fall_detection(false); assert(!pending);
    // Confirmed sequence: vector impact can span multiple axes.
    low(); sample(550,550,0); regs[0x0B]=0x20; now+=1000;
    update_fall_detection(true);
    assert(fall_detection_pending());
    assert(toneCalls==1);
    now+=1000;
    update_fall_detection(true);
    assert(toneCalls==1); // One sound per pending event.
    // A wake before impact must still catch samples arriving during confirmation.
    acknowledge_fall_detection(); resetClassifier(); low(); now+=1000;
    delayHook=[] { delayHook=nullptr; sample(0,-800,0); };
    update_fall_detection(false); assert(pending && toneCalls==2);
    // Preserve XYZ alignment and low interval across separate FIFO reads.
    acknowledge_fall_detection(); resetClassifier();
    low(4); now+=1000; update_fall_detection(false); assert(!pending);
    low(4); sample(0,0,800); now+=1000;
    update_fall_detection(false); assert(pending);
    // An overflow breaks the old interval: missing samples cannot be combined.
    acknowledge_fall_detection(); resetClassifier();
    low(4); now+=1000; update_fall_detection(false);
    low(4); sample(0,0,800); regs[0x0B]=0x08; now+=1000;
    update_fall_detection(false); assert(!pending);
}
`);
try {
    cp.execFileSync('c++', ['-std=c++17', '-I'+dir, '-I'+path.join(root,'include'),
        path.join(dir,'test.cpp'), '-o', path.join(dir,'test')], {stdio:'inherit'});
    cp.execFileSync(path.join(dir,'test'), [], {stdio:'inherit'});
    console.log('ADXL362 FIFO/configuration/fall confirmation/sleep tests passed');
} finally {
    fs.rmSync(dir, {recursive:true, force:true});
}
