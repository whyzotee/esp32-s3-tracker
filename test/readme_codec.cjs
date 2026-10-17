// Keep the copy/paste README decoder and command encodings executable.
const assert = require('assert');
const fs = require('fs');
const vm = require('vm');
const readme = fs.readFileSync('README.md', 'utf8');
const code = readme.match(/```javascript\n(function decodeUplink[\s\S]*?)\n```/);
assert(code, 'README decoder missing');
const context = {};
vm.createContext(context);
vm.runInContext(code[1], context);
const bytes = [...Buffer.from('02000060410000C942300103740E0F', 'hex')];
let result = context.decodeUplink({fPort: 2, bytes});
assert.equal(result.data.latitude, 14);
assert.equal(result.data.longitude, 100.5);
assert.equal(result.data.battery_mv, 3700);
assert.equal(result.data.battery_percent, 15);
assert.equal(result.data.flags.low_battery, true);
for (const [status, event] of ['Normal Update', 'SOS', 'Fall Detected', 'I am OK', 'Trip Preparation'].entries()) {
    bytes[0] = status;
    bytes[9] = 0xff;
    result = context.decodeUplink({fPort: 2, bytes});
    assert.equal(result.data.event, event);
    assert.equal(result.warnings.length, 0);
    assert(Object.values(result.data.flags).every(v => v === true));
}
assert(context.decodeUplink({fPort: 11, bytes}).errors.length);
assert(context.decodeUplink({fPort: 2, bytes: bytes.slice(1)}).errors.length);
for (const [hex, base64] of [['01','AQ=='], ['02','Ag=='], ['03','Aw=='], ['04','BA=='],
    ['05','BQ=='], ['06','Bg=='],
    ['01000f','AQAP'], ['000000','AAAA']]) {
    assert.equal(Buffer.from(base64, 'base64').toString('hex'), hex);
    assert(readme.includes(base64));
}
console.log('README decoder, all event/flag mappings and downlink Base64 tests passed');
