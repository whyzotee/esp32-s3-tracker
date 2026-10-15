#pragma once

#include <stdint.h>

namespace OtaManager {
// Wi-Fi AP password: 8-63 ASCII characters. Change before building/uploading.
constexpr char apPassword[] = "TrackerOTA123";
static_assert(sizeof(apPassword) - 1 >= 8 && sizeof(apPassword) - 1 <= 63,
              "OTA Wi-Fi password must contain 8-63 characters");

// Stops tracker peripherals, hosts the local updater, then restarts or sleeps.
[[noreturn]] void run(uint32_t sleepIntervalMs);
}
