// Host regression: node test/telemetry.cjs (requires c++).
const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const telemetry = fs.readFileSync('src/telemetry.cpp', 'utf8');
const debug = fs.readFileSync('src/debug_mode.cpp', 'utf8');
const encoder = telemetry.slice(telemetry.indexOf('void encode_telemetry'));
const walk = debug.slice(debug.indexOf('Telemetry walking_sample'), debug.indexOf('void message'));
const program = `
#include <telemetry.h>
#include <tracker_app.h>
#include <cstring>
#include <cassert>
#include <cmath>
constexpr double DEG_TO_RAD = 3.141592653589793 / 180;
uint32_t clockMs = 0;
uint32_t millis() { return 0; } // Simulate uptime restarting on every wake.
uint64_t simulationClockMs() { return clockMs; }
#define RTC_DATA_ATTR
uint32_t esp_random() { return 3050; }
struct { template<class... T> void printf(const char*, T...) {} } Serial;
${encoder}
${walk}
int main() {
    Telemetry s{};
    s.status = 2; s.lat = 14; s.lon = 100.5; s.batteryMv = 3700; s.batteryPercent = 15;
    uint8_t payload[TELEMETRY_PAYLOAD_SIZE];
    memset(payload, 0xff, sizeof(payload));
    encode_telemetry(s, payload);
    const uint8_t expected[] = {2,0,0,96,65,0,0,201,66,0,
                                FirmwareVersion::major, FirmwareVersion::minor,
                                116,14,15};
    assert(sizeof(payload) == sizeof(expected));
    assert(memcmp(payload, expected, sizeof(expected)) == 0);
    s.flags = TelemetryFlags::batteryValid | TelemetryFlags::lowBattery;
    encode_telemetry(s, payload);
    assert(sizeof(payload)==15 && payload[9]==0x30 && payload[0]==2);
    auto a = walking_sample(0);
    assert(fabs(a.lat - 13.7563) < 0.00001);
    clockMs = 15000;
    auto b = walking_sample(2);
    double distance = (b.lat - a.lat) * 111320;
    assert(distance > 13.3 && distance < 22.7);
    assert(b.status == 2);
    clockMs = 30120;
    auto c = walking_sample(0);
    assert(c.status == 0);
}
`;
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tracker-telemetry-'));
const binary = path.join(dir, 'test');
cp.execFileSync('c++', ['-std=c++17', '-Iinclude', '-x', 'c++', '-', '-o', binary], {input: program});
cp.execFileSync(binary);
console.log('Payload compatibility and simulated walking tests passed');
