#include <lorawan_storage.h>
#include <cstring>
#include <stddef.h>
#include <esp_attr.h>
#include <esp_system.h>

RTC_DATA_ATTR LoRaStorage::Record LoRaStorage::rtc;

namespace {
uint32_t readLE(const uint8_t *p, size_t length)
{
    uint32_t value = 0;
    for (size_t i = 0; i < length; ++i) value |= uint32_t(p[i]) << (i * 8);
    return value;
}
void writeLE(uint8_t *p, uint32_t value, size_t length)
{
    for (size_t i = 0; i < length; ++i) p[i] = value >> (i * 8);
}
void signBuffer(uint8_t *buffer, size_t length)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < length - 2; i += 2) {
        sum ^= uint16_t(buffer[i]) << 8;
        if (i + 1 < length - 2) sum ^= buffer[i + 1];
    }
    writeLE(buffer + length - 2, sum, 2);
}
uint32_t crc32(const uint8_t *p, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320U : 0);
    }
    return ~crc;
}
}

bool LoRaStorage::commit(Record &record)
{
    writeLE(record.crc, crc32(reinterpret_cast<const uint8_t *>(&record),
                             offsetof(Record, crc)), sizeof(record.crc));
    rtc = record;
    current = record;
    return true;
}

void LoRaStorage::clearRtcSession()
{
    rtc = Record{};
}

bool LoRaStorage::persistNonces(const Record &record)
{
    // Keep the legacy layout for safe migration, but never persist session keys
    // or frame counters. Flash is written only around OTAA joins.
    Record durable = record;
    durable.phase = 0;
    memset(durable.session, 0, sizeof(durable.session));
    writeLE(durable.crc, crc32(reinterpret_cast<const uint8_t *>(&durable),
                              offsetof(Record, crc)), sizeof(durable.crc));
    return nvs.putBytes("state", &durable, sizeof(durable)) == sizeof(durable);
}

LoRaStorage::Restore LoRaStorage::begin(LoRaWANNode &node, const uint8_t identity[32])
{
    if (!nvs.begin("lorawan-otaa", false)) return Restore::Error;
    current = Record{};
    if (!nvs.isKey("state")) {
        memcpy(current.identity, identity, sizeof(current.identity));
        memcpy(current.nonces, node.getBufferNonces(), sizeof(current.nonces));
        return Restore::Fresh;
    }
    if (nvs.getBytesLength("state") != sizeof(current) ||
        nvs.getBytes("state", &current, sizeof(current)) != sizeof(current) ||
        current.version != 1 || current.phase > 2 ||
        memcmp(current.identity, identity, sizeof(current.identity)) != 0 ||
        readLE(current.crc, 4) != crc32(reinterpret_cast<const uint8_t *>(&current),
                                     offsetof(Record, crc))) return Restore::Error;
    // Only a genuine deep-sleep wake may resume a session. On power-on,
    // watchdog or software reset, join again using the durable nonce floor.
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP &&
        rtc.version == 1 && rtc.phase == 1 &&
        memcmp(rtc.identity, identity, sizeof(rtc.identity)) == 0 &&
        readLE(rtc.crc, 4) == crc32(reinterpret_cast<const uint8_t *>(&rtc),
                                   offsetof(Record, crc)) &&
        readLE(rtc.nonces + RADIOLIB_LORAWAN_NONCES_DEV_NONCE, 2) >=
        readLE(current.nonces + RADIOLIB_LORAWAN_NONCES_DEV_NONCE, 2)) {
        current = rtc;
    } else {
        current.phase = 0;
    }
    if (readLE(current.nonces + RADIOLIB_LORAWAN_NONCES_VERSION, 2) !=
        RADIOLIB_LORAWAN_NONCES_VERSION_VAL) return Restore::Error;
    if (node.setBufferNonces(current.nonces) != RADIOLIB_ERR_NONE) return Restore::Error;
    // A power cut in TX/RX may lose downlink state. Rejoin with the preserved
    // nonce instead of resuming potentially stale counters/session keys.
    if (current.phase != 1) return Restore::JoinRequired;
    if (node.setBufferSession(current.session) != RADIOLIB_ERR_NONE) return Restore::Error;
    return Restore::Session;
}

bool LoRaStorage::reserveJoin(LoRaWANNode &node)
{
    Record next = current;
    memcpy(next.nonces, node.getBufferNonces(), sizeof(next.nonces));
    uint32_t nonce = readLE(next.nonces + RADIOLIB_LORAWAN_NONCES_DEV_NONCE, 2);
    // Do not let RadioLib's uint16_t nonce wrap, including its final increment.
    if (nonce >= UINT16_MAX) return false;
    writeLE(next.nonces + RADIOLIB_LORAWAN_NONCES_DEV_NONCE, nonce + 1, 2);
    signBuffer(next.nonces, sizeof(next.nonces));
    next.phase = 0;
    memset(next.session, 0, sizeof(next.session));
    // Only the persisted copy advances. The imminent join uses the current nonce.
    return persistNonces(next) && commit(next);
}

bool LoRaStorage::saveJoined(LoRaWANNode &node)
{
    Record next = current;
    memcpy(next.nonces, node.getBufferNonces(), sizeof(next.nonces));
    memcpy(next.session, node.getBufferSession(), sizeof(next.session));
    if (readLE(next.nonces + RADIOLIB_LORAWAN_NONCES_DEV_NONCE, 2) <
        readLE(current.nonces + RADIOLIB_LORAWAN_NONCES_DEV_NONCE, 2)) return false;
    next.phase = 1;
    return persistNonces(next) && commit(next);
}

uint32_t LoRaStorage::nextCounter(LoRaWANNode &node)
{
    return readLE(node.getBufferSession() + RADIOLIB_LORAWAN_SESSION_FCNT_UP, 4);
}

bool LoRaStorage::reserveUplink(LoRaWANNode &node)
{
    const uint32_t counter = nextCounter(node);
    if (counter == UINT32_MAX) return false;
    Record next = current;
    memcpy(next.nonces, node.getBufferNonces(), sizeof(next.nonces));
    memcpy(next.session, node.getBufferSession(), sizeof(next.session));
    writeLE(next.session + RADIOLIB_LORAWAN_SESSION_FCNT_UP, counter + 1, 4);
    signBuffer(next.session, sizeof(next.session));
    next.phase = 2;
    return commit(next);
}

bool LoRaStorage::saveUplink(LoRaWANNode &node)
{
    Record next = current;
    memcpy(next.nonces, node.getBufferNonces(), sizeof(next.nonces));
    memcpy(next.session, node.getBufferSession(), sizeof(next.session));
    const uint32_t reserved = readLE(current.session + RADIOLIB_LORAWAN_SESSION_FCNT_UP, 4);
    if (readLE(next.session + RADIOLIB_LORAWAN_SESSION_FCNT_UP, 4) < reserved)
        writeLE(next.session + RADIOLIB_LORAWAN_SESSION_FCNT_UP, reserved, 4);
    signBuffer(next.session, sizeof(next.session));
    next.phase = 1;
    return commit(next);
}
