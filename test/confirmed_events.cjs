// Exercise the production send function and delivery policy without RF hardware.
const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const root = path.resolve(__dirname, '..');
const source = fs.readFileSync(path.join(root, 'src/lorawan.cpp'), 'utf8');
const signature = 'static LoRaResult sendPayload(';
const start = source.indexOf(signature);
if (start < 0) throw new Error('Missing send function');
const app = fs.readFileSync(path.join(root, 'src/tracker_app.cpp'), 'utf8');
if (!app.includes('if (result.delivered(sample.status) && TripControl::revision() == sentRevision)') ||
    !app.includes('!result.delivered(sample.status) || !result.sessionSaved'))
    throw new Error('Pending clearing and retry must both use delivery policy');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tracker-confirmed-'));
try {
    fs.writeFileSync(path.join(dir, 'test.cpp'), `
#include <cassert>
#include <cstring>
#include <lorawan.example.h>
constexpr int RADIOLIB_ERR_UNKNOWN=-1, RADIOLIB_ERR_NONE=0;
namespace AppConfig { constexpr uint8_t uplinkDataRate=2, uplinkPort=2, deviceInfoPort=3; }
namespace DeviceButton { void servicePowerOff() {} }
struct { bool responsive=false; } radioHal;
struct LoRaWANEvent_t { uint8_t fPort=0; bool confirming=false; };
struct SerialMock {
    template<class... T> void printf(const char*, T...) {}
    void println(const char*) {}
} Serial;
struct NodeMock {
    int16_t code=0, drCode=0;
    bool ack=false, confirmed=false;
    int sends=0; uint8_t lastPort=0;
    int16_t setDatarate(uint8_t) { return drCode; }
    int16_t sendReceive(const uint8_t*, size_t length, uint8_t port,
        uint8_t* data, size_t* size, bool confirm, void*, LoRaWANEvent_t* event) {
        assert((length==9 && port==2) || (length==4 && port==3));
        ++sends; confirmed=confirm; lastPort=port;
        event->confirming=ack; event->fPort=10;
        data[0]=1; *size=1;
        return code;
    }
} node;
struct StorageMock {
    bool reserve=true, save=true;
    int saves=0;
    bool reserveUplink(NodeMock&) { return reserve; }
    bool saveUplink(NodeMock&) { ++saves; return save; }
} storage;
namespace LoRaStorage { uint32_t nextCounter(NodeMock&) { return 7; } }
bool ready=true;
bool primaryTelemetryAcknowledged=false,deviceInfoAttempted=false;
void encode_telemetry(const Telemetry&, uint8_t* payload) {
    memset(payload, 0, TELEMETRY_PAYLOAD_SIZE);
}
void encode_device_info(const Telemetry&, uint8_t* payload) { memset(payload,0,DEVICE_INFO_PAYLOAD_SIZE); }
${source.slice(start)}
int main() {
    for (uint8_t status : {uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(3), uint8_t(4)}) {
        for (int16_t code : {int16_t(-4), int16_t(0), int16_t(1), int16_t(2)}) {
            for (bool ack : {false, true}) {
                ready=true; node={}; storage={}; node.code=code; node.ack=ack;
                Telemetry sample{}; sample.status=status;
                auto result=send_lora_telemetry(sample);
                assert(node.confirmed==(status!=0) && node.sends==1);
                assert(result.acknowledged==(status!=0 && code>0 && ack));
                assert(result.delivered(status)==(code>=0 && (status==0 || (code>0 && ack))));
                assert(result.downlinkLength==(code>0 ? 1u : 0u));
                assert(storage.saves==1); // Save counters even when ACK is missing.
            }
        }
    }
    ready=true; node={}; storage={}; storage.reserve=false;
    Telemetry sos{}; sos.status=1;
    auto result=send_lora_telemetry(sos);
    assert(node.sends==0 && !result.delivered(1));
    ready=true; node={}; storage={}; node.code=1; node.ack=true; storage.save=false;
    result=send_lora_telemetry(sos);
    assert(result.delivered(1) && !result.sessionSaved && !ready);
    ready=true; node={}; storage={}; node.code=1; node.ack=true;
    primaryTelemetryAcknowledged=false; deviceInfoAttempted=false;
    assert(!lora_device_info_due());
    send_lora_telemetry(sos); assert(!lora_device_info_due());
    sos.status=4; send_lora_telemetry(sos); assert(lora_device_info_due());
    node.code=0; node.ack=false; send_lora_device_info(sos);
    assert(node.lastPort==3 && node.confirmed && !lora_device_info_due()); // Metadata remains confirmed.
}
` .replace('#include <cassert>', '#include <cassert>\n#include <initializer_list>'));
    const binary = path.join(dir, 'test');
    cp.execFileSync('c++', ['-std=c++17', '-DTRACKER_LOG_HOST_TEST', '-DTrackerLog=Serial' , '-DTRACKER_WEB_DEBUG=0', '-I', path.join(root, 'include'), path.join(dir, 'test.cpp'), '-o', binary]);
    cp.execFileSync(binary);
    console.log('Periodic unconfirmed sends, confirmed events, ACK gating and counter-save tests passed');
} finally {
    fs.rmSync(dir, { recursive: true, force: true });
}
