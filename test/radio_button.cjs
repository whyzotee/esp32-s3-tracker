// Execute the actual HAL override with deterministic time/SPI/button mocks.
const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const source = fs.readFileSync('src/lorawan.cpp', 'utf8');
const activations = [...source.matchAll(/state = node\.activateOTAA\(\);/g)];
if (activations.length !== 2) throw Error('Expected restore and fresh join paths');
for (const activation of activations) {
    if (!source.slice(activation.index - 100, activation.index).includes('radioHal.responsive = true;') ||
        !source.slice(activation.index, activation.index + 140).includes('radioHal.responsive = false;'))
        throw Error('Both OTAA paths must service power-off');
}
const start = source.indexOf('class ButtonRadioHal');
const end = source.indexOf('\nButtonRadioHal radioHal;', start);
if (start < 0 || end < 0) throw Error('Missing button-aware HAL');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'radio-button-'));
try {
    const binary = path.join(dir, 'test');
    cp.execFileSync('c++', ['-std=c++17', '-x', 'c++', '-', '-o', binary], { input: `
#include <cassert>
#include <cstdint>
using RadioLibTime_t = uint32_t;
int SPI=0, polls=0;
uint32_t now=0;
bool pending=false;
struct Shutdown {};
namespace DeviceButton {
void servicePowerOff() { ++polls; if(pending) throw Shutdown{}; }
}
class ArduinoHal {
public:
    explicit ArduinoHal(int&) {}
    virtual void spiBeginTransaction() {}
    virtual void spiEndTransaction() {}
    virtual void yield() { ++now; }
    virtual void delay(uint32_t ms) { now += ms; }
    uint32_t millis() { return now; }
};
${source.slice(start, end)}
int main() {
    ButtonRadioHal hal;
    hal.delay(5000); assert(now==5000 && polls==0);
    hal.responsive=true;
    hal.delay(5000); assert(now==10000 && polls>400);
    int before=polls;
    hal.spiBeginTransaction();
    hal.yield(); hal.delay(20); assert(polls==before);
    hal.spiEndTransaction();
    hal.yield(); assert(polls==before+1);
    pending=true;
    bool stopped=false;
    try { hal.delay(5000); } catch(Shutdown&) { stopped=true; }
    assert(stopped);
    stopped=false;
    try { hal.yield(); } catch(Shutdown&) { stopped=true; }
    assert(stopped);
    pending=false;
    now=UINT32_MAX-5;
    hal.delay(20); assert(now==14); // wrap-safe deadline
}
` });
    cp.execFileSync(binary);
} finally {
    fs.rmSync(dir, {recursive:true, force:true});
}
