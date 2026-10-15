// Host regression: production battery sampling/state and README payload decoder.
const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const assert = require('assert');
const vm = require('vm');
const source = fs.readFileSync('src/battery.cpp', 'utf8').replace(/^#include .*$/gm, '');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tracker-low-battery-'));
try {
    const program = `
#include <battery.h>
#include <cassert>
#include <stddef.h>
#define RTC_DATA_ATTR
constexpr int ADC_11db=0;
namespace Board { constexpr int batteryAdc=2; }
uint32_t adc=1208;
void analogSetAttenuation(int) {}
void analogSetPinAttenuation(int,int) {}
uint32_t analogReadMilliVolts(int) { return adc; }
void delay(int) {}
struct { template<class... T> void printf(const char*, T...) {} } Serial;
${source}
int main() {
    static_assert(!Battery::classifyLow(false,16,true), "entry boundary");
    static_assert(Battery::classifyLow(false,15,true), "entry boundary");
    static_assert(Battery::classifyLow(true,19,true), "hysteresis");
    static_assert(!Battery::classifyLow(true,20,true), "recovery boundary");
    static_assert(Battery::classifyLow(true,100,false), "invalid preserves low");
    static_assert(!Battery::classifyLow(false,0,false), "invalid preserves normal");
    auto reading=Battery::read(); assert(reading.valid && reading.percent==19 && !reading.low);
    adc=1200; reading=Battery::read(); assert(reading.valid && reading.percent==15 && reading.low);
    adc=0; reading=Battery::read(); assert(!reading.valid && !reading.low);
    adc=1208; reading=Battery::read(); assert(reading.valid && reading.percent==19 && reading.low);
    adc=1209; reading=Battery::read(); assert(reading.valid && reading.percent==20 && !reading.low);
    adc=1200; auto first=Battery::bootReading();
    adc=1300; auto cached=Battery::bootReading();
    assert(first.low && cached.low && first.millivolts==cached.millivolts);
}
`;
    const binary = path.join(dir, 'test');
    cp.execFileSync('c++', ['-std=c++17', '-Iinclude', '-x', 'c++', '-', '-o', binary], {input: program});
    cp.execFileSync(binary);
    const readme = fs.readFileSync('README.md', 'utf8');
    const decoder = readme.match(/```javascript\s*([\s\S]*?function decodeUplink[\s\S]*?)```/);
    assert(decoder, 'README decoder exists');
    const context = {};
    vm.createContext(context);
    vm.runInContext(decoder[1], context);
    const bytes = Array.from(Buffer.from('02000060410000C942300103740E0F', 'hex'));
    let result = context.decodeUplink({fPort:2, bytes});
    assert.equal(result.data.flags.low_battery, true);
    assert.equal(result.data.status, 2);
    bytes[9]=16; result=context.decodeUplink({fPort:2, bytes});
    assert.equal(result.data.flags.low_battery, false);
    bytes[9]=32; result=context.decodeUplink({fPort:2, bytes});
    assert.equal(result.data.flags.low_battery, null);
    console.log('Low battery thresholds, hysteresis, invalid readings, wake cache and decoder tests passed');
} finally {
    fs.rmSync(dir, {recursive:true, force:true});
}
