#include <tracker_log.h>
#include <debug_web.h>

#if TRACKER_WEB_DEBUG
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <debug_web_page.h>
#if __has_include(<lorawan.h>)
#include <lorawan.h>
#else
#include <lorawan.example.h>
#endif
#include <atomic>
#include <cmath>

namespace {
constexpr size_t eventCapacity = 64;
struct Event { uint32_t ms; char message[160]; };
struct View {
    DebugWeb::State state;
    bool hasState = false, joined = false, txSeen = false, acknowledged = false;
    bool telemetrySeen = false;
    bool gpsSeen = false, locationValid = false;
    uint8_t gpsStatus = 0;
    uint32_t satellites = 0, gpsAt = 0;
    int16_t txCode = 0;
    uint8_t port = 0, status = 0, flags = 0;
    float latitude = 0, longitude = 0;
    uint32_t txAt = 0, stateAt = 0;
    Event events[eventCapacity]{};
    size_t next = 0, count = 0;
} view;
portMUX_TYPE viewLock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> running{false};
TaskHandle_t webTask = nullptr;
SemaphoreHandle_t stopped = nullptr;
WebServer server(80);
char pendingLine[160]{};
size_t pendingLength = 0;

void appendLocked(const char *message)
{
    Event &event = view.events[view.next];
    event.ms = millis();
    snprintf(event.message, sizeof(event.message), "%s", message);
    view.next = (view.next + 1) % eventCapacity;
    if (view.count < eventCapacity) ++view.count;
}

void quoted(String &json, const char *value)
{
    json += '"';
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(value); *p; ++p) {
        if (*p == '"' || *p == '\\') { json += '\\'; json += char(*p); }
        else if (*p >= 0x20) json += char(*p);
        else {
            char escaped[7];
            snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
            json += escaped;
        }
    }
    json += '"';
}
const char *boolean(bool value) { return value ? "true" : "false"; }
const char *phaseName(uint8_t phase)
{
    switch (phase) { case 0: return "Preparing"; case 1: return "Waiting"; case 2: return "Travelling"; }
    return "Unknown";
}
const char *gpsName(uint8_t status)
{
    switch (status) {
    case 0: return "Unavailable"; case 1: return "Searching";
    case 2: return "FixReady"; case 3: return "TimedOut";
    }
    return "Unknown";
}

String statusJson()
{
    View copy;
    portENTER_CRITICAL(&viewLock);
    copy = view;
    portEXIT_CRITICAL(&viewLock);
    const auto &s = copy.state;
    String json;
    json.reserve(16000);
    json = "{\"status\":{\"debug\":true,\"simulated_sleep\":true,\"uptime_ms\":";
    json += millis();
    json += ",\"free_heap_bytes\":"; json += ESP.getFreeHeap();
    json += ",\"state_available\":"; json += boolean(copy.hasState);
    json += ",\"state_updated_ms\":"; json += copy.stateAt;
    json += ",\"phase\":"; quoted(json, phaseName(s.phase));
    json += ",\"gps\":"; quoted(json, gpsName(copy.gpsSeen ? copy.gpsStatus : s.gps));
    json += ",\"gps_updated_ms\":"; json += copy.gpsSeen ? String(copy.gpsAt) : String("null");
    json += ",\"satellites\":"; json += copy.gpsSeen ? String(copy.satellites) : String("null");
    json += ",\"gps_location_valid\":"; json += copy.gpsSeen ? boolean(copy.locationValid) : "null";
    json += ",\"initial_fix_ready\":"; json += boolean(s.initialFix);
    json += ",\"alarm\":"; json += boolean(s.alarm);
    json += ",\"off_route\":"; json += boolean(s.offRoute);
    json += ",\"low_power\":"; json += boolean(s.lowPower);
    json += ",\"battery_mv\":"; json += s.batteryValid ? String(s.batteryMv) : String("null");
    json += ",\"battery_percent\":"; json += s.batteryValid ? String(s.batteryPercent) : String("null");
    json += ",\"radio_joined\":"; json += boolean(copy.joined);
    json += ",\"radio_wait_ms\":"; json += s.radioWaitMs;
    json += ",\"last_tx_ms\":"; json += copy.txSeen ? String(copy.txAt) : String("null");
    json += ",\"last_tx_code\":"; json += copy.txSeen ? String(copy.txCode) : String("null");
    json += ",\"last_tx_port\":"; json += copy.txSeen ? String(copy.port) : String("null");
    json += ",\"network_ack\":"; json += copy.txSeen ? boolean(copy.acknowledged) : "null";
    json += ",\"latitude\":";
    json += copy.telemetrySeen && std::isfinite(copy.latitude) ? String(copy.latitude, 6) : String("null");
    json += ",\"longitude\":";
    json += copy.telemetrySeen && std::isfinite(copy.longitude) ? String(copy.longitude, 6) : String("null");
    json += ",\"telemetry_status\":"; json += copy.telemetrySeen ? String(copy.status) : String("null");
    json += ",\"telemetry_flags\":"; json += copy.telemetrySeen ? String(copy.flags) : String("null");
    json += "},\"events\":[";
    for (size_t i = 0; i < copy.count; ++i) {
        const Event &event = copy.events[(copy.next + eventCapacity - copy.count + i) % eventCapacity];
        if (i) json += ',';
        json += "{\"ms\":"; json += event.ms; json += ",\"message\":";
        quoted(json, event.message); json += '}';
    }
    json += "]}";
    return json;
}

void worker(void *)
{
    server.on("/", HTTP_GET, [] {
        server.sendHeader("Cache-Control", "no-store");
        server.send_P(200, "text/html; charset=utf-8", DebugWeb::page);
    });
    server.on("/api/status", HTTP_GET, [] {
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json", statusJson());
    });
    server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
    server.begin();
    uint32_t notification = 0;
    for (;;) {
        xTaskNotifyWait(0, UINT32_MAX, &notification, 0);
        if (notification) break;
        server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    running = false;
    xSemaphoreGive(stopped);
    vTaskDelete(nullptr);
}
}

namespace DebugWeb {
void begin()
{
    if (webTask) return;
    stopped = xSemaphoreCreateBinary();
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "Tracker-Debug-%06llX",
             static_cast<unsigned long long>(LoRaCredentials::devEui >> 40));
    const IPAddress ip(192, 168, 4, 1), subnet(255, 255, 255, 0);
    if (!stopped || !WiFi.mode(WIFI_AP) || !WiFi.softAPConfig(ip, ip, subnet) ||
        !WiFi.softAP(ssid, apPassword)) {
        TrackerLog.println("[WEB DEBUG] AP startup failed; continuing without web debug");
        if (stopped) vSemaphoreDelete(stopped);
        stopped = nullptr;
        WiFi.mode(WIFI_OFF);
        return;
    }
    running = true;
    if (xTaskCreatePinnedToCore(worker, "debug-web", 20480, nullptr, 1, &webTask, 0) != pdPASS) {
        running = false;
        webTask = nullptr;
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        vSemaphoreDelete(stopped);
        stopped = nullptr;
        TrackerLog.println("[WEB DEBUG] Worker unavailable");
        return;
    }
    note("Debug AP started; report sleep is simulated");
    TrackerLog.printf("[WEB DEBUG] Connect to %s, then open http://192.168.4.1\n", ssid);
}

void stop()
{
    if (!webTask) return;
    xTaskNotify(webTask, 1, eSetBits);
    // Do not let OTA reuse Wi-Fi, or sleep cut its power, before HTTP has stopped.
    // A slow HTTP client may delay this acknowledgement by the server timeout.
    xSemaphoreTake(stopped, portMAX_DELAY);
    webTask = nullptr;
    vSemaphoreDelete(stopped);
    stopped = nullptr;
}
bool active() { return running.load(); }
void note(const char *message)
{
    if (message) TrackerLog.printf("[WEB DEBUG] %s\n", message);
}
void recordSerial(const uint8_t *bytes, size_t length)
{
    // Capture before AP startup too. Bounded work, no heap or network calls.
    // Short chunks keep the cross-core critical section bounded for raw NMEA.
    for (size_t offset = 0; offset < length; ) {
        const size_t end = offset + (length - offset > 64 ? 64 : length - offset);
        portENTER_CRITICAL(&viewLock);
        for (; offset < end; ++offset) {
            const char byte = char(bytes[offset]);
            if (byte == '\r') continue;
            if (byte == '\n') {
                pendingLine[pendingLength] = 0;
                appendLocked(pendingLine);
                pendingLength = 0;
            } else {
                pendingLine[pendingLength++] = byte ? byte : '?';
                if (pendingLength == sizeof(pendingLine) - 1) {
                    pendingLine[pendingLength] = 0;
                    appendLocked(pendingLine);
                    pendingLength = 0;
                }
            }
        }
        portEXIT_CRITICAL(&viewLock);
    }
}
void publish(const State &state)
{
    if (!active()) return;
    portENTER_CRITICAL(&viewLock);
    view.state = state;
    view.hasState = true;
    view.stateAt = millis();
    portEXIT_CRITICAL(&viewLock);
}
void joined(bool success)
{
    if (!active()) return;
    portENTER_CRITICAL(&viewLock);
    view.joined = success;
    portEXIT_CRITICAL(&viewLock);
}
void transmission(int16_t code, bool acknowledged, uint8_t port)
{
    if (!active()) return;
    portENTER_CRITICAL(&viewLock);
    view.txSeen = true; view.txCode = code; view.acknowledged = acknowledged;
    view.port = port; view.txAt = millis();
    portEXIT_CRITICAL(&viewLock);
}
void telemetry(float latitude, float longitude, uint8_t status, uint8_t flags)
{
    if (!active()) return;
    portENTER_CRITICAL(&viewLock);
    view.telemetrySeen = true; view.latitude = latitude; view.longitude = longitude;
    view.status = status; view.flags = flags;
    portEXIT_CRITICAL(&viewLock);
}
void gpsProgress(uint8_t status, uint32_t satellites, bool locationValid)
{
    if (!active()) return;
    portENTER_CRITICAL(&viewLock);
    view.gpsSeen = true; view.gpsStatus = status; view.satellites = satellites;
    view.locationValid = locationValid; view.gpsAt = millis();
    portEXIT_CRITICAL(&viewLock);
}
}
#endif
