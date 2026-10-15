const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const assert = require('assert');
const root = path.resolve(__dirname, '..');
const source = fs.readFileSync(path.join(root, 'src/gnss.cpp'), 'utf8');
const start = source.indexOf('struct GnssSearchLed {');
const end = source.indexOf('\n};', start) + 3;
assert(start >= 0 && end > start);
const acquisition = source.slice(source.indexOf('bool get_location()'));
assert(acquisition.includes('GnssSearchLed searchLed;'));
assert(acquisition.includes('searchLed.update();'));
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tracker-gnss-led-'));
try {
    fs.writeFileSync(path.join(dir, 'test.cpp'), `
#include <cassert>
#include <cstdint>
namespace Board { constexpr int statusLed=40; }
constexpr int HIGH=1, LOW=0;
uint32_t now=0;
int led=-1;
uint32_t millis() { return now; }
void digitalWrite(int pin, int state) { assert(pin==40); led=state; }
${source.slice(start, end)}
bool acquire(bool fix) {
    GnssSearchLed searchLed;
    assert(led==HIGH);
    now+=499; searchLed.update(); assert(led==HIGH);
    now+=1; searchLed.update(); assert(led==LOW);
    now+=500; searchLed.update(); assert(led==HIGH);
    now+=500; searchLed.update(); assert(led==LOW);
    return fix; // Covers both success and early-interruption return values.
}
int main() {
    assert(acquire(true)); assert(led==HIGH);
    assert(!acquire(false)); assert(led==HIGH);
    { GnssSearchLed searchLed; now+=500; searchLed.update(); assert(led==LOW); }
    assert(led==HIGH); // Normal scope exit, as on timeout.
    now=UINT32_MAX-100;
    { GnssSearchLed searchLed; now+=500; searchLed.update(); assert(led==LOW); }
    assert(led==HIGH);
}
`);
    const binary = path.join(dir, 'test');
    cp.execFileSync('c++', ['-std=c++17', path.join(dir, 'test.cpp'), '-o', binary]);
    cp.execFileSync(binary);
    console.log('GNSS search LED cadence, exit restoration and millis wraparound tests passed');
} finally {
    fs.rmSync(dir, { recursive: true, force: true });
}
