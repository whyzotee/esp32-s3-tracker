#pragma once
#include <Preferences.h>
#include <RadioLib.h>

// RadioLib 7.7.1 serialization only. Re-test before changing the library version.
// RTC holds active sessions; NVS is written only around OTAA joins.
class LoRaStorage {
public:
    enum class Restore { Error, Fresh, JoinRequired, Session };
    Restore begin(LoRaWANNode &node, const uint8_t identity[32]);
    bool reserveJoin(LoRaWANNode &node);
    bool saveJoined(LoRaWANNode &node);
    bool reserveUplink(LoRaWANNode &node);
    bool saveUplink(LoRaWANNode &node);
    static uint32_t nextCounter(LoRaWANNode &node);
    // Drop only the RTC session. Durable DevNonce state in NVS is retained.
    static void clearRtcSession();

private:
    // All byte fields: deterministic layout, no compiler padding.
    struct Record {
        uint8_t version = 1;
        uint8_t phase = 0; // 0: nonces only, 1: committed session, 2: TX in flight.
        uint8_t identity[32] = {};
        uint8_t nonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE] = {};
        uint8_t session[RADIOLIB_LORAWAN_SESSION_BUF_SIZE] = {};
        uint8_t crc[4] = {};
    } current;
    static Record rtc;
    Preferences nvs;
    bool commit(Record &record);
    bool persistNonces(const Record &record);
};
