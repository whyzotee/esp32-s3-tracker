const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const assert = require('assert');
const root = path.resolve(__dirname, '..');
const source = fs.readFileSync(path.join(root, 'src/gnss.cpp'), 'utf8');
const start = source.indexOf('void setGnssSearchLed(');
const end = source.indexOf('\n}', start) + 2;
assert(start >= 0 && end > start);
const acquisition = source.slice(source.indexOf('void gnssWorker(void *)'), source.indexOf('bool readSnapshot'));
assert(acquisition.includes('setGnssSearchLed(command.started, searching && !snapshot.qualified);'));
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
void acquire() {
    const uint32_t started=now;
    setGnssSearchLed(started,true);
    assert(led==HIGH);
    now+=499; setGnssSearchLed(started,true); assert(led==HIGH);
    now+=1; setGnssSearchLed(started,true); assert(led==LOW);
    now+=500; setGnssSearchLed(started,true); assert(led==HIGH);
    now+=500; setGnssSearchLed(started,true); assert(led==LOW);
    setGnssSearchLed(started,false); assert(led==HIGH);
}
int main() {
    acquire();
    now=UINT32_MAX-100;
    acquire(); // Wrap-safe blink and restore on completion/cancellation.
}
`);
    const binary = path.join(dir, 'test');
    cp.execFileSync('c++', ['-std=c++17', path.join(dir, 'test.cpp'), '-o', binary]);
    cp.execFileSync(binary);
    console.log('GPS task LED cadence, completion restoration and millis wraparound tests passed');
} finally {
    fs.rmSync(dir, { recursive: true, force: true });
}
