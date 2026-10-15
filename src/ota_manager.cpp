#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <audio_feedback.h>
#include <board_pins.h>
#include <deep_sleep.h>
#include <device_button.h>
#include <gnss.h>
#if __has_include(<lorawan.h>)
#include <lorawan.h>
#else
#include <lorawan.example.h>
#endif
#include <ota_manager.h>

namespace {
constexpr uint32_t idleTimeoutMs = 15UL * 60UL * 1000UL;
constexpr uint32_t rebootDelayMs = 1000;
WebServer server(80);
uint32_t lastActivityAt = 0;
uint32_t rebootAt = 0;
bool updateOk = false;
String apName;

void touch()
{
    lastActivityAt = millis();
}

String page(const String &message)
{
    String result(F("<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"));
    result += F("<title>Tracker OTA</title><style>body{font-family:sans-serif;max-width:36rem;margin:2rem auto;padding:0 1rem}input,button{font:inherit;margin:.5rem 0}button{padding:.5rem 1rem}</style></head><body><h1>Tracker OTA</h1><p>");
    result += message;
    result += F("</p><form method=\"POST\" action=\"/update\" enctype=\"multipart/form-data\"><input type=\"file\" name=\"firmware\" accept=\".bin\" required><br><button type=\"submit\">Upload firmware</button></form><p>Upload the PlatformIO firmware.bin file. Do not close this page during upload.</p></body></html>");
    return result;
}

void handleRoot()
{
    touch();
    server.send(200, "text/html", page("Ready. The updater closes after 15 minutes without activity."));
}

void handleUploadBody()
{
    touch();
    HTTPUpload &upload = server.upload();
    switch (upload.status) {
    case UPLOAD_FILE_START:
        updateOk = Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
        if (!updateOk) Update.printError(Serial);
        Serial.printf("[OTA] Upload started: %s\n", upload.filename.c_str());
        break;
    case UPLOAD_FILE_WRITE:
        if (updateOk && Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            updateOk = false;
            Update.printError(Serial);
        }
        break;
    case UPLOAD_FILE_END:
        if (updateOk) updateOk = Update.end(true);
        if (!updateOk) Update.printError(Serial);
        Serial.printf("[OTA] Upload %s: %u bytes\n", updateOk ? "complete" : "failed",
                      static_cast<unsigned>(upload.totalSize));
        break;
    case UPLOAD_FILE_ABORTED:
        updateOk = false;
        Update.abort();
        Serial.println("[OTA] Upload aborted");
        break;
    default:
        break;
    }
}

void handleUploadDone()
{
    touch();
    if (!updateOk) {
        server.send(400, "text/html", page("Upload failed. The installed firmware is unchanged."));
        return;
    }
    server.send(200, "text/html", page("Upload complete. Restarting into the new firmware..."));
    rebootAt = millis();
}

bool startAp()
{
    const uint64_t mac = ESP.getEfuseMac();
    char name[32];
    snprintf(name, sizeof(name), "Tracker-OTA-%06llX",
             static_cast<unsigned long long>(mac & 0xFFFFFFULL));
    apName = name;

    if (!WiFi.mode(WIFI_AP)) return false;
    const IPAddress apIp(192, 168, 4, 1);
    const IPAddress subnet(255, 255, 255, 0);
    if (!WiFi.softAPConfig(apIp, apIp, subnet)) {
        Serial.println("[OTA] Failed to configure fixed AP IP");
        return false;
    }
    if (!WiFi.softAP(apName.c_str(), OtaManager::apPassword)) return false;

    server.on("/", HTTP_GET, handleRoot);
    server.on("/update", HTTP_POST, handleUploadDone, handleUploadBody);
    server.onNotFound([] {
        touch();
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
    });
    server.begin();
    touch();
    Serial.printf("[OTA] AP ready: %s at http://%s/ (password protected)\n", apName.c_str(),
                  WiFi.softAPIP().toString().c_str());
    return true;
}

void stopAp()
{
    server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    digitalWrite(Board::statusLed, HIGH);
}
}

namespace OtaManager {
[[noreturn]] void run(uint32_t sleepIntervalMs)
{
    DeviceButton::acknowledgeOta();
    sleep_lora_radio();
    stop_gnss();
    Board::setVext(false);
    Serial.println("[OTA] Starting local Wi-Fi updater");
    AudioFeedback::play(AudioFeedback::Event::OtaMode);
    if (!startAp()) {
        Serial.println("[OTA] AP start failed; returning to sleep");
        DeepSleep::timed(sleepIntervalMs);
    }

    for (;;) {
        server.handleClient();
        digitalWrite(Board::statusLed, (millis() / 500U) % 2U == 0U ? HIGH : LOW);
        if (rebootAt != 0 && uint32_t(millis() - rebootAt) >= rebootDelayMs) {
            Serial.println("[OTA] Rebooting into uploaded firmware");
            Serial.flush();
            ESP.restart();
        }
        if (uint32_t(millis() - lastActivityAt) >= idleTimeoutMs) {
            Serial.println("[OTA] Idle timeout; returning to timed sleep");
            stopAp();
            DeepSleep::timed(sleepIntervalMs);
        }
        delay(2);
    }
}
}
