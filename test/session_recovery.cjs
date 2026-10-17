// Host test: node test/session_recovery.cjs (requires c++).
const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'otaa-storage-'));
const includeDir = path.join(dir, 'include');
fs.mkdirSync(includeDir);
fs.writeFileSync(path.join(includeDir, 'esp_attr.h'), '#define RTC_DATA_ATTR\n');
fs.writeFileSync(path.join(includeDir, 'esp_system.h'), `
#pragma once
enum { ESP_RST_POWERON, ESP_RST_DEEPSLEEP };
inline int resetReason = ESP_RST_POWERON;
inline int esp_reset_reason() { return resetReason; }
`);

// Mock Preferences.h
fs.writeFileSync(path.join(includeDir, 'Preferences.h'), `
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <map>
#include <string>
#include <algorithm>

class Preferences {
public:
    inline static std::map<std::string, std::vector<uint8_t>> store;
    inline static unsigned writes = 0;
    inline static bool failWrites = false;
    bool opened = false;
    bool begin(const char*, bool) { if (opened) return false; opened = true; return true; }
    void end() { opened = false; }
    bool isKey(const char* key) { return store.find(key) != store.end(); }
    size_t getBytesLength(const char* key) {
        auto it = store.find(key);
        return it != store.end() ? it->second.size() : 0;
    }
    size_t getBytes(const char* key, void* buf, size_t maxLen) {
        auto it = store.find(key);
        if (it == store.end()) return 0;
        size_t len = std::min(maxLen, it->second.size());
        memcpy(buf, it->second.data(), len);
        return len;
    }
    size_t putBytes(const char* key, const void* val, size_t len) {
        if (failWrites) return 0;
        ++writes;
        const uint8_t* p = static_cast<const uint8_t*>(val);
        store[key] = std::vector<uint8_t>(p, p + len);
        return len;
    }
    bool remove(const char* key) {
        return store.erase(key) > 0;
    }
    void clear() {
        store.clear();
    }
};
`);

// Mock RadioLib.h
fs.writeFileSync(path.join(includeDir, 'RadioLib.h'), `
#pragma once
#include <cstdint>
#include <cstring>

#define RADIOLIB_LORAWAN_NONCES_BUF_SIZE 16
#define RADIOLIB_LORAWAN_SESSION_BUF_SIZE 256
#define RADIOLIB_LORAWAN_NONCES_VERSION 0
#define RADIOLIB_LORAWAN_NONCES_VERSION_VAL 0x0001
#define RADIOLIB_LORAWAN_NONCES_DEV_NONCE 8
#define RADIOLIB_LORAWAN_SESSION_FCNT_UP 16
#define RADIOLIB_ERR_NONE 0

class LoRaWANNode {
public:
    uint8_t nonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE] = {};
    uint8_t session[RADIOLIB_LORAWAN_SESSION_BUF_SIZE] = {};

    LoRaWANNode() {
        nonces[0] = RADIOLIB_LORAWAN_NONCES_VERSION_VAL & 0xFF;
        nonces[1] = (RADIOLIB_LORAWAN_NONCES_VERSION_VAL >> 8) & 0xFF;
    }

    uint8_t* getBufferNonces() { return nonces; }
    uint8_t* getBufferSession() { return session; }
    int16_t setBufferNonces(const uint8_t* n) {
        memcpy(nonces, n, sizeof(nonces));
        return RADIOLIB_ERR_NONE;
    }
    int16_t setBufferSession(const uint8_t* s) {
        memcpy(session, s, sizeof(session));
        return RADIOLIB_ERR_NONE;
    }
};
`);

const testRunner = `
#include <cstdint>
#include <cstring>
#include <cassert>
#include <iostream>
#include <Preferences.h>
#include <RadioLib.h>
#include <lorawan_storage.h>
#include "lorawan_storage.cpp"

int main() {
    LoRaWANNode node;
    LoRaStorage storage;
    uint8_t id1[32] = {1, 2, 3};
    uint8_t id2[32] = {4, 5, 6};

    // 1. Fresh state
    auto res = storage.begin(node, id1);
    assert(res == LoRaStorage::Restore::Fresh);

    // 2. Reserve join should advance persisted DevNonce and set phase 0
    assert(storage.reserveJoin(node));

    // RadioLib increments devNonce in memory when JoinRequest is composed
    node.nonces[RADIOLIB_LORAWAN_NONCES_DEV_NONCE]++;

    // 3. Save joined session
    assert(storage.saveJoined(node));

    assert(Preferences::writes == 2);
    // 4. Only a deep-sleep wake restores the RTC session.
    resetReason = ESP_RST_DEEPSLEEP;
    res = storage.begin(node, id1);
    assert(res == LoRaStorage::Restore::Session);

    // 5. Uplink reservation and save
    uint32_t c0 = LoRaStorage::nextCounter(node);
    assert(c0 == 0);
    assert(storage.reserveUplink(node));

    // Simulate in-flight failure: reboot before saveUplink
    res = storage.begin(node, id1);
    // Phase was 2 (in-flight), so should require rejoin!
    assert(res == LoRaStorage::Restore::JoinRequired);

    // Rejoin and save
    assert(storage.reserveJoin(node));
    node.nonces[RADIOLIB_LORAWAN_NONCES_DEV_NONCE]++;
    assert(storage.saveJoined(node));

    // Normal uplink cycle
    assert(storage.reserveUplink(node));
    // Simulate RadioLib incrementing counter during TX
    node.session[RADIOLIB_LORAWAN_SESSION_FCNT_UP] = 1;
    assert(storage.saveUplink(node));

    // Check session restore again
    res = storage.begin(node, id1);
    assert(res == LoRaStorage::Restore::Session);
    assert(LoRaStorage::nextCounter(node) == 1);
    assert(Preferences::writes == 4); // two joins, zero flash writes for uplinks

    // Flash write failures must not affect normal RTC-only uplinks.
    Preferences::failWrites = true;
    for (int i = 0; i < 100; ++i) {
        assert(storage.reserveUplink(node));
        node.session[RADIOLIB_LORAWAN_SESSION_FCNT_UP]++;
        assert(storage.saveUplink(node));
        LoRaStorage wakeStorage;
        LoRaWANNode wakeNode;
        assert(wakeStorage.begin(wakeNode, id1) == LoRaStorage::Restore::Session);
        assert(LoRaStorage::nextCounter(wakeNode) == LoRaStorage::nextCounter(node));
    }
    assert(Preferences::writes == 4);

    // Cold boot never resumes a stale RTC or legacy flash session.
    resetReason = ESP_RST_POWERON;
    LoRaStorage coldStorage;
    LoRaWANNode coldNode;
    assert(coldStorage.begin(coldNode, id1) == LoRaStorage::Restore::JoinRequired);
    assert(coldNode.nonces[RADIOLIB_LORAWAN_NONCES_DEV_NONCE] == 2);
    assert(!coldStorage.reserveJoin(coldNode));
    Preferences::failWrites = false;
    assert(coldStorage.reserveJoin(coldNode));
    LoRaStorage interruptedJoin;
    LoRaWANNode retryNode;
    assert(interruptedJoin.begin(retryNode, id1) == LoRaStorage::Restore::JoinRequired);
    assert(retryNode.nonces[RADIOLIB_LORAWAN_NONCES_DEV_NONCE] == 3);

    // Identity mismatch should return Error
    res = storage.begin(node, id2);
    assert(res == LoRaStorage::Restore::Error);

    // Corrupt durable state must fail closed, without deleting nonce history.
    Preferences::store["state"][0] ^= 0xff;
    assert(storage.begin(node, id1) == LoRaStorage::Restore::Error);

    std::cout << "LoRaWAN OTAA storage lifecycle tests passed successfully." << std::endl;
    return 0;
}
`;

const binary = path.join(dir, 'test');
const sourceFile = path.join(dir, 'test.cpp');
fs.writeFileSync(sourceFile, testRunner);

const projectRoot = path.resolve(__dirname, '..');
cp.execFileSync('c++', [
    '-std=c++17',
    `-I${includeDir}`,
    `-I${path.join(projectRoot, 'include')}`,
    `-I${path.join(projectRoot, 'src')}`,
    sourceFile,
    '-o', binary
]);
const output = cp.execFileSync(binary, { encoding: 'utf8' });
console.log(output.trim());
