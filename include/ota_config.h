#pragma once

namespace OtaConfig {
// Wi-Fi AP password: 8-63 ASCII characters. Change before building/uploading.
constexpr char apPassword[] = "TrackerOTA123";
static_assert(sizeof(apPassword) - 1 >= 8 && sizeof(apPassword) - 1 <= 63,
              "OTA Wi-Fi password must contain 8-63 characters");
}
