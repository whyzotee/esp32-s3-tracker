// Host test: node test/power_mode.cjs (requires c++).
const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tracker-power-'));
const root = path.resolve(__dirname, '..');
try {
    fs.writeFileSync(path.join(dir, 'esp_attr.h'), '#define RTC_DATA_ATTR\n');
    fs.writeFileSync(path.join(dir, 'test.cpp'), `
#include <cassert>
#include "power_mode.cpp"
int main() {
    const uint8_t enable[] = {0x01, 0x00, 0x0F};
    const uint8_t disable[] = {0x00, 0x00, 0x00};
    const uint8_t unknown[] = {0x01, 0x00, 0x01};
    const uint8_t legacy[] = {0x01};
    const uint8_t extra[] = {0x01, 0x00, 0x0F, 0x00};
    assert(!PowerMode::enabled());
    assert(PowerMode::reportInterval() == AppConfig::reportIntervalMs);
    assert(!PowerMode::handleDownlink(2, enable, sizeof(enable)));
    assert(!PowerMode::handleDownlink(10, unknown, sizeof(unknown)));
    assert(!PowerMode::handleDownlink(10, legacy, sizeof(legacy)));
    assert(!PowerMode::handleDownlink(10, extra, sizeof(extra)));
    assert(!PowerMode::handleDownlink(10, enable, 0));
    assert(!PowerMode::handleDownlink(10, nullptr, sizeof(enable)));
    assert(!PowerMode::enabled());
    assert(PowerMode::handleDownlink(10, enable, sizeof(enable)));
    assert(PowerMode::enabled());
    assert(PowerMode::reportInterval() == AppConfig::lowPowerIntervalMs);
    assert(PowerMode::handleDownlink(10, enable, sizeof(enable)));
    assert(!PowerMode::handleDownlink(10, unknown, sizeof(unknown)));
    assert(PowerMode::reportInterval() == AppConfig::lowPowerIntervalMs);
    assert(PowerMode::handleDownlink(10, disable, sizeof(disable)));
    assert(!PowerMode::enabled());
    assert(PowerMode::reportInterval() == AppConfig::reportIntervalMs);
    assert(PowerMode::handleDownlink(10, disable, sizeof(disable)));
    // Simulate a cold boot reinitializing RTC data.
    lowPowerMarker = 0;
    assert(PowerMode::reportInterval() == AppConfig::reportIntervalMs);
}
`);
    const binary = path.join(dir, 'test');
    cp.execFileSync('c++', ['-std=c++17', `-I${dir}`,
        `-I${path.join(root, 'include')}`, `-I${path.join(root, 'src')}`,
        path.join(dir, 'test.cpp'), '-o', binary]);
    cp.execFileSync(binary);
    console.log('Low Power command validation and interval tests passed');
} finally {
    fs.rmSync(dir, { recursive: true, force: true });
}
