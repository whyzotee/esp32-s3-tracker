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
#include <initializer_list>
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
    const auto decode = [](const uint8_t* p,double range) {
        int32_t n = p[0] | (p[1]<<8) | (p[2]<<16);
        if(n & 0x800000) n-=0x1000000;
        return n * range / 8388607.0;
    };
    assert(sizeof(payload)==9 && payload[0]==2 && payload[7]==0 && payload[8]==15);
    const uint8_t compactExpected[]={2,62,233,19,119,119,71,0,15};
    assert(memcmp(payload,compactExpected,9)==0);
    assert(fabs(decode(payload+1,90)-14)<90.0/8388607);
    assert(fabs(decode(payload+4,180)-100.5)<180.0/8388607);
    uint8_t info[DEVICE_INFO_PAYLOAD_SIZE];
    s.flags=TelemetryFlags::batteryValid;
    encode_device_info(s,info);
    const uint8_t expected[] = {FirmwareVersion::major,FirmwareVersion::minor,116,14};
    assert(sizeof(info)==sizeof(expected) && memcmp(info,expected,sizeof(info))==0);
    s.flags = TelemetryFlags::batteryValid | TelemetryFlags::lowBattery;
    encode_telemetry(s, payload);
    assert(sizeof(payload)==9 && payload[7]==0x30 && payload[0]==2);
    for(float lat : {-90.f, -13.7f, 0.f, 13.7f, 90.f}) {
      for(float lon : {-180.f,-100.7f,0.f,100.7f,180.f}) {
        s.lat=lat; s.lon=lon; encode_telemetry(s,payload);
        assert(fabs(decode(payload+1,90)-lat)<=90.0/8388607/2+1e-12);
        assert(fabs(decode(payload+4,180)-lon)<=180.0/8388607/2+1e-12);
      }
    }
    s.lat=NAN; s.lon=181; encode_telemetry(s,payload);
    assert(payload[1]==0 && payload[2]==0 && payload[3]==0x80);
    assert(payload[4]==0 && payload[5]==0 && payload[6]==0x80);
    s.flags=0; encode_device_info(s,info); assert(info[2]==0 && info[3]==0);
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
cp.execFileSync('c++', ['-std=c++17', '-DTRACKER_LOG_HOST_TEST', '-DTrackerLog=Serial' , '-DTRACKER_WEB_DEBUG=0', '-Iinclude', '-x', 'c++', '-', '-o', binary], {input: program});
cp.execFileSync(binary);
console.log('Payload compatibility and simulated walking tests passed');
