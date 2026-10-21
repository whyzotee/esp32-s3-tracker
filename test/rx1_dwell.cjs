// Compile the actual band initializer: uplink-only setDwellTime(false) must not
// leave the AS923 RX1 DR2 floor enabled when the server answers at DR0/SF12.
const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const source = fs.readFileSync(path.join(__dirname, '../src/lorawan.cpp'), 'utf8');
const initializer = source.match(/LoRaWANBand_t loraBand = \[\] \{[\s\S]*?\}\(\);/);
if (!initializer) throw new Error('Missing production band initializer');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tracker-rx1-'));
try {
    fs.writeFileSync(path.join(dir, 'test.cpp'), `
#include <cassert>
struct LoRaWANBand_t {
    struct Channel { unsigned freq; } txFreqs[2];
    unsigned dwellTimeDn;
    unsigned dwellTimeUp;
};
const LoRaWANBand_t AS923 = {{{923200000}, {923400000}}, 400, 400};
${initializer[0]}
int main() {
    assert(AS923.dwellTimeDn == 400);
    assert(AS923.dwellTimeUp == 400);
    assert(loraBand.dwellTimeDn == 0);
    assert(loraBand.dwellTimeUp == 0);
    assert(loraBand.txFreqs[0].freq == 923200000);
    assert(loraBand.txFreqs[1].freq == 0);
    for (unsigned dr : {0u, 1u, 2u, 3u}) {
        unsigned rx1 = dr;
        if (loraBand.dwellTimeDn && rx1 < 2) rx1 = 2;
        assert(rx1 == dr);
    }
}
`.replace('#include <cassert>', '#include <cassert>\n#include <initializer_list>'));
    cp.execFileSync('c++', ['-std=c++17', path.join(dir, 'test.cpp'), '-o', path.join(dir, 'test')]);
    cp.execFileSync(path.join(dir, 'test'));
} finally {
    fs.rmSync(dir, {recursive: true, force: true});
}
