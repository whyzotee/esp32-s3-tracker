#pragma once
// Copy to lorawan.h (gitignored) and set credentials before deployment.
#include <stdint.h>
#include <stddef.h>
#include <telemetry.h>


struct LoRaResult {
    int16_t code;
    bool sessionSaved;
    bool acknowledged;
    uint8_t downlinkPort;
    size_t downlinkLength;
    uint8_t downlink[255];

    // Status 0 succeeds locally after TX/RX completes; receipt is not guaranteed.
    // Other statuses require a network ACK, not just an unrelated downlink.
    bool delivered(uint8_t status) const {
        return code >= 0 && (status == 0 || acknowledged);
    }
};

bool setup_lora_wan_app();
// Forces OTAA without resetting durable DevNonce storage.
void lora_forget_rtc_session();
LoRaResult send_lora_telemetry(const Telemetry &sample);
bool lora_device_info_due();
LoRaResult send_lora_device_info(const Telemetry &sample);
uint32_t lora_wait_ms();
void sleep_lora_radio();

// EUIs use the SAME displayed order as ChirpStack, not reversed.
namespace LoRaCredentials {
constexpr uint64_t joinEui = 0x0000000000000000ULL;
constexpr uint64_t devEui = 0x0000000000000000ULL;
// LoRaWAN 1.0.4 AppKey: 16 bytes in displayed order.
constexpr uint8_t appKey[16] = {0};
}
