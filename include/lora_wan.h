#pragma once
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

    // An unrelated downlink is not an ACK for an urgent uplink.
    bool delivered(uint8_t status) const {
        return code >= 0 && (!(status == 1 || status == 2) || acknowledged);
    }
};

bool setup_lora_wan_app();
// Forces OTAA on the next setup without resetting DevNonce storage.
void lora_forget_rtc_session();
LoRaResult send_lora_telemetry(const Telemetry &sample);
uint32_t lora_wait_ms();
void sleep_lora_radio();
