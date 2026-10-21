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
const compact=[...Buffer.from('023ee913777747300f','hex')];
result=context.decodeUplink({fPort:2,bytes:compact});
assert(Math.abs(result.data.latitude-14)<0.000006);
assert(Math.abs(result.data.longitude-100.5)<0.000011);
assert.equal(result.data.battery_percent,15);
assert.equal(result.data.battery_mv,null);
assert.equal(result.data.firmware_version,null);
assert.equal(result.data.flags.low_battery,true);
for(let status=0;status<=4;status++) {
    compact[0]=status; compact[7]=255;
    result=context.decodeUplink({fPort:2,bytes:compact});
    assert(Object.values(result.data.flags).every(v=>v===true));
    assert.equal(result.errors.length,0);
}
compact[7]=32;
assert.equal(context.decodeUplink({fPort:2,bytes:compact}).data.flags.low_battery,null);
for(const [lat,lon] of [[-90,-180],[-13.7,-100.7],[0,0],[90,180]]) {
    const encoded=[0];
    for(const [degrees,range] of [[lat,90],[lon,180]]) {
        const value=Math.round(degrees*8388607/range);
        encoded.push(value&255,value>>8&255,value>>16&255);
    }
    encoded.push(16,100);
    result=context.decodeUplink({fPort:2,bytes:encoded});
    assert(Math.abs(result.data.latitude-lat)<=90/8388607/2+1e-12);
    assert(Math.abs(result.data.longitude-lon)<=180/8388607/2+1e-12);
}
compact[3]=0x80; compact[2]=compact[1]=0;
assert(context.decodeUplink({fPort:2,bytes:compact}).errors.length);
result=context.decodeUplink({fPort:3,bytes:[1,3,116,14]});
assert.equal(result.data.message_type,'device_info');
assert.equal(result.data.firmware_version,'1.3');
assert.equal(result.data.battery_mv,3700);
assert.equal(context.decodeUplink({fPort:3,bytes:[1,3,0,0]}).data.battery_mv,null);
assert(context.decodeUplink({fPort:3,bytes:[1]}).errors.length);
for (const [hex, base64] of [['01','AQ=='], ['02','Ag=='], ['03','Aw=='], ['04','BA=='],
    ['05','BQ=='], ['06','Bg=='],
    ['01000f','AQAP'], ['000000','AAAA']]) {
    assert.equal(Buffer.from(base64, 'base64').toString('hex'), hex);
    assert(readme.includes(base64));
}
console.log('README decoder, all event/flag mappings and downlink Base64 tests passed');
