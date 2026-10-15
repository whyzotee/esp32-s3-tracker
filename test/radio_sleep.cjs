// Exercise the production PHY initialization/sleep functions without OTAA or hardware.
const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const root = path.resolve(__dirname, '..');
const source = fs.readFileSync(path.join(root, 'src/lora_wan.cpp'), 'utf8');
function extract(signature) {
    const start = source.indexOf(signature);
    if (start < 0) throw new Error(`Missing function: ${signature}`);
    const body = source.indexOf('{', start);
    let depth = 1, end = body + 1;
    for (; depth && end < source.length; ++end) {
        if (source[end] === '{') ++depth;
        if (source[end] === '}') --depth;
    }
    return source.slice(start, end);
}
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tracker-radio-sleep-'));
try {
    fs.writeFileSync(path.join(dir, 'test.cpp'), `
#include <cassert>
#include <cstdint>
#include <vector>
#include <utility>
constexpr int RADIOLIB_ERR_NONE = 0;
constexpr int RADIOLIB_SX126X_SYNC_WORD_PRIVATE = 0x12;
constexpr int INPUT=1, INPUT_PULLUP=2;
std::vector<std::pair<int,int>> pinModes;
void pinMode(int pin, int mode) { pinModes.emplace_back(pin, mode); }
namespace Board { constexpr int radioSck=9, radioMiso=11, radioMosi=10, radioCs=8; }
struct SpiMock {
    int begins = 0, ends = 0;
    void end() { ++ends; }
    void begin(int sck, int miso, int mosi, int cs) {
        assert(sck==9 && miso==11 && mosi==10 && cs==8); ++begins;
    }
} SPI;
struct RadioMock {
    int begins=0, sleeps=0, initResult=0, sleepResult=0;
    bool retained=true;
    int16_t begin(double, double, int, int, int, int, int, double tcxo) {
        assert(tcxo == 1.8); ++begins; return initResult;
    }
    int16_t sleep(bool retain=true) { ++sleeps; retained=retain; return sleepResult; }
} radio;
struct SerialMock {
    template<class... T> void printf(const char *, T...) {}
    void println(const char *) {}
} Serial;
bool ready=false, radioReady=false;
${extract('int16_t initializeRadio()')}
${extract('void sleep_lora_radio()')}
int main() {
    // Normal initialized radio: no extra begin, original warm-sleep baseline.
    ready=true; radioReady=true;
    sleep_lora_radio();
    assert(radio.begins==0 && radio.sleeps==1 && radio.retained);
    assert(SPI.ends==1);
    const std::vector<std::pair<int,int>> expectedPins={{9,INPUT},{10,INPUT},{11,INPUT},{8,INPUT_PULLUP}};
    assert(pinModes==expectedPins);
    assert(!ready && !radioReady);
    // Shutdown before normal setup: initialize only the PHY, then sleep.
    radio={}; SPI={}; pinModes.clear();
    sleep_lora_radio();
    assert(SPI.begins==1 && radio.begins==1 && radio.sleeps==1 && radio.retained);
    assert(SPI.ends==1 && pinModes==expectedPins);
    // Failed PHY init must not issue a sleep command on an unusable radio.
    radio={}; SPI={}; pinModes.clear(); radio.initResult=-2; ready=true;
    sleep_lora_radio();
    assert(radio.begins==1 && radio.sleeps==0 && !ready && !radioReady);
    assert(SPI.ends==0 && pinModes.empty());
    // A failed sleep command must not leave the application ready to transmit.
    radio={}; radio.sleepResult=-2; ready=true; radioReady=true;
    sleep_lora_radio();
    assert(radio.sleeps==1 && !ready && !radioReady);
    assert(SPI.ends==1 && pinModes==expectedPins);
}
`);
    const binary = path.join(dir, 'test');
    cp.execFileSync('c++', ['-std=c++17', path.join(dir, 'test.cpp'), '-o', binary]);
    cp.execFileSync(binary);
    console.log('Radio warm-sleep, pre-init shutdown and failure-path tests passed');
} finally {
    fs.rmSync(dir, { recursive: true, force: true });
}
