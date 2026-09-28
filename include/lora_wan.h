#pragma once
#include <stdint.h>
#include <stddef.h>
#include <telemetry.h>

struct LoRaResult {
    int16_t code;
    bool sessionSaved;
    uint8_t downlinkPort;
    size_t downlinkLength;
    uint8_t downlink[255];
};

bool setup_lora_wan_app();
// Forces OTAA on the next setup without resetting DevNonce storage.
void lora_forget_rtc_session();
LoRaResult send_lora_telemetry(const Telemetry &sample);
uint32_t lora_wait_ms();
void sleep_lora_radio();
