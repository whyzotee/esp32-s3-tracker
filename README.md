# ESP32-S3 LoRaWAN tracker

The radio uses RadioLib 7.8.1 with SX1262, OTAA (Over-The-Air Activation), AS923, Class A,
confirmed uplinks for every event on port 2. No Heltec activation license or Serial
provisioning dialogue is used. All debugging output is sent via Serial Monitor (115200 baud).

Target: [Heltec Wireless Shell V3](https://heltec.org/project/wireless-shell-v3/)
(HTIT-Wsh_V3), on the custom PCB shown in `docs/schematic`.
The build uses the framework's existing `heltec_wireless_shell_v3` variant;
custom peripheral wiring stays in `include/board_pins.h`.
Pin mapping and upload instructions: [docs/HardwareV3.md](docs/HardwareV3.md).
ADXL362 wiring, thresholds and testing: [docs/ADXL362.md](docs/ADXL362.md).
Sensor events override debug's normal status with status 2, while debug
coordinates remain simulated. Audible feedback uses the GPIO4 buzzer in both
debug and production mode; see [Audible feedback](#audible-feedback).

Application-side ChirpStack MQTT topics and actual test data:
[docs/RealTest.md](docs/RealTest.md).

## Trip flow and application commands

On power-up or reset, the device starts **soft-off**. Hold the button for
1–4 seconds to turn it on. Soft-off does not physically disconnect the battery.

```text
Off → Button power-on → OTAA Join → Qualified initial GPS fix → Await Start → Travelling
                                                                                ↓
Off ← Application End Trip command ←─────────────────────────────────────────────┘
```

GNSS is powered before Join, allowing satellite acquisition to begin while LoRa
connects. However, GPS parsing/fix qualification and radio transactions remain
sequential; no parallel GNSS task is added. If no fix qualifies, the device
reports preparation status with `initial_fix_ready=false` and retries using the
existing acquisition timeouts. A Start command received before the initial fix
is remembered, but travelling begins only after a fix qualifies. `LORA_DEBUG`
provides simulated positions for testing this flow, not real GPS fixes.

During preparation and while awaiting Start, uplinks use
`AppConfig::reportIntervalMs` to open RX1/RX2 for commands, even if Low Power is
enabled. During a trip, the configured normal or Low Power interval applies.
GPS acquisition, TX/RX processing and airtime limits can extend the actual
reporting period beyond the configured interval.

### All downlink commands

FPort identifies an application-data channel within LoRaWAN; it is not a GPIO
or TCP/UDP port. JSON `data` must contain **Base64-encoded command bytes**,
not a hexadecimal string.

| Command | FPort | Hex bytes | Base64 (`data`) | Device action |
| --- | ---: | --- | --- | --- |
| Start Trip | 11 | `01` | `AQ==` | Remember Start; begin travelling once the initial fix is ready |
| End Trip | 11 | `02` | `Ag==` | Clear trip state, audio and pending events; enter soft-off until button power-on |
| Off Route | 11 | `03` | `Aw==` | Repeat the off-route alarm during a trip; route detection is not performed on the device |
| Route Clear | 11 | `04` | `BA==` | Stop the off-route alarm without cancelling SOS/Fall |
| Low Power ON | 10 | `01 00 0F` | `AQAP` | Use `AppConfig::lowPowerIntervalMs` during a trip |
| Low Power OFF | 10 | `00 00 00` | `AAAA` | Return to `AppConfig::reportIntervalMs` |

FPort 11 requires **exactly 1 byte**; FPort 10 requires **exactly 3 bytes**.
Unknown commands and incorrect lengths are ignored. Repeated Start/End/Route
commands are idempotent. `03` is ignored before a trip starts. The application
must detect route deviations and send `04` when the device returns to the route.
Cancelling SOS with the button does not clear the off-route alarm.

MQTT topic:

```text
application/<APPLICATION_ID>/device/<lowercase_dev_eui>/command/down
```

Start Trip example for the MQTT integration:

```json
{
  "devEui": "<lowercase_dev_eui>",
  "confirmed": false,
  "fPort": 11,
  "data": "AQ=="
}
```

Use the same JSON structure for every command, replacing `fPort` and `data`
according to the table. Here, `confirmed:false` applies to the downlink; firmware
uplinks are always confirmed. Commands are queued and delivered after a subsequent
uplink, not immediately during sleep or GPS acquisition. Successful queuing or a
Gateway TX ACK does not prove the device applied a command; check the next uplink's
flags. End Trip powers off immediately after reception, so this protocol has no
subsequent uplink confirming End. A soft-off device sends no uplinks to receive
Start; the user must turn it on with the button first.

### Uplink and decoder

FPort **2**, **15-byte** payload, always confirmed. The payload length is unchanged:

| Byte offset | Format | Meaning |
| --- | --- | --- |
| 0 | uint8 | Event/status from the table below |
| 1–4 | float32 little-endian | Latitude |
| 5–8 | float32 little-endian | Longitude |
| 9 | bit flags | See the flag definitions below |
| 10 | uint8 | Firmware major |
| 11 | uint8 | Firmware minor |
| 12–13 | uint16 little-endian | Battery mV |
| 14 | uint8 | Battery percent |

| Status | Recommended decoder event | Meaning |
| ---: | --- | --- |
| 0 | `Normal Update` | Routine report during a trip |
| 1 | `SOS` | Three short button presses |
| 2 | `Fall Detected` | Qualified low-acceleration followed by impact sequence |
| 3 | `I am OK` | One short press cancels SOS/Fall and confirms the user is safe |
| 4 | `Trip Preparation` | Acquiring initial GPS or awaiting application Start |

Flags in byte 9:

| Bit | Mask | Name | Meaning when set |
| ---: | --- | --- | --- |
| 0 | `0x01` | `gps_fresh` | Position qualified in this acquisition cycle |
| 1 | `0x02` | `low_power_mode` | Low Power enabled |
| 2 | `0x04` | `debug_simulation` | Simulated position |
| 3 | `0x08` | `initial_fix_ready` | Qualified initial position available, or simulated readiness in debug |
| 4 | `0x10` | `battery_valid` | Battery reading valid |
| 5 | `0x20` | `low_battery` | Low battery according to hysteresis thresholds |
| 6 | `0x40` | `trip_active` | Trip started |
| 7 | `0x80` | `off_route` | Application requested an off-route alarm |

**Update the ChirpStack codec/backend to support statuses 3/4 and the new flags.**
Bit 3 was previously used for `vext_held_on`; do not decode it with that name for
this firmware. SOS/Fall/I-am-OK reports immediately use the latest RTC-cached
position. Without a cached position, coordinates are 0,0 and `gps_fresh=false`.
Trip state, coordinates and pending events remain in RTC without additional flash
writes and are lost when battery power is removed.

### QoS, audible alerts and cancellation

- Every uplink requests a LoRaWAN ACK. An ACK produces the success melody;
  a missing ACK keeps the working tone and schedules a retry after
  `AppConfig::retryInitialMs` (60 seconds), or a longer radio-required wait.
  Retries are not sent in a tight loop.
- ACK confirms LoRaWAN network reception through a gateway;
  **it does not confirm that the application created an alert**.
- SOS/Fall reports retry until acknowledged, but the alarm continues until
  **one short button press** confirms the user is safe. Cancellation stops retries
  of the old event and queues status 3 until acknowledged. A new emergency before
  successful I-am-OK delivery takes priority and supersedes the older OK report.
- SOS and Fall share one melody; off-route uses another alarm pattern.
  Audio priority is SOS/Fall > off-route > preparation/awaiting Start/missing ACK.
  Success melodies do not interrupt an emergency alarm awaiting cancellation.
- Repeating audio **keeps the MCU awake**, including while awaiting Start or a
  missing ACK. These states consume more power than deep sleep; the 90 µA sleep
  measurement does not apply. Normal active-trip operation returns to deep sleep
  after an ACK when no alarm is active.
- Awake waits continue servicing buttons and the sensor instead of blocking all
  processing with a long delay. The startup Serial/USB five-second wait has been
  removed, so native USB monitoring may miss the earliest logs.

The application should close an alert explicitly on status 3, not treat a Normal
Update as confirmation of safety. Missing ACKs can produce duplicate events;
the backend must deduplicate according to its own policy.

## Debug walking track

Set these constants in `src/main.cpp`:

```cpp
constexpr bool LORA_DEBUG = true;
constexpr uint32_t LORA_DEBUG_INTERVAL_MS = 15000;
```

Both modes start soft-off, then prepare and wait for the application Start command.
They save sessions in RTC and use real deep sleep only during an active trip with
ACK received and no ongoing alarm. Other states use a serviced awake wait.
Debug normally uses a 15-second active-trip interval; radio limits still apply.
GPS is not required in debug. Debug uses the production DR2 (SF10/BW125), with synthetic coordinates
starting at 13.756300, 100.501800. Each step moves at 0.9–1.5 m/s with a gradual
random turn of up to 30.5 degrees. At 15 seconds this is about 13.5–22.5 metres.
Deep-sleep wakes continue the track from RTC, including time spent asleep.
Reset or power loss starts the simulated track again. This is a free walking simulation,
not a route constrained to roads. Its timestamp is elapsed simulation time, not UTC.
Serial labels it `[SIMULATED WALK]`; `debug_simulation` marks synthetic positions.

Set `LORA_DEBUG = false` for deployment: acquire GPS, send at DR2 (SF10/BW125),
complete receive windows, then use the configured report interval (or longer if
the MAC duty-cycle timer requires it). Button GPIO0 and fall GPIO7 use the
original low/high wakeup polarities. Both modes validate button holds and handle
real sensor events; an EXT0 wake alone does not mean SOS.

### Button and soft power-off

Short-click actions are selected on debounced release. Power-off and OTA holds
start immediately once their three-second threshold is reached.

| Current state | Hold before release | Action |
| --- | --- | --- |
| On | Three short presses, each under 1 second, within 1.5 seconds | Queue SOS (status 1) |
| SOS/Fall alarm active | One short press | Stop the alarm and queue I am OK (status 3) |
| On | Hold 3 seconds or longer | Select soft power-off immediately; it sleeps after the button is released |
| On | Two short presses, then hold the third press for 3 seconds | Start local Wi-Fi OTA mode immediately; release is not required |
| On | Any other hold | No button event |
| Off | Hold 1–4 seconds | Power on |
| Off | Any other hold | Return to off sleep, without starting GPS/LoRa |

The initial button wake counts as the first SOS click when it has already been
released by the time button monitoring starts. Soft-off disables timer and fall wakeups and waits only for GPIO0. It is deep
sleep, not a physical battery disconnect; always-powered circuits still draw current.
After a valid power-on hold, the tracker deliberately discards its RTC LoRaWAN
session and performs a fresh OTAA join. This gives the user a physical recovery
path when the network session needs to be re-established; durable DevNonce data
is retained in NVS.
Hold time starts when firmware can read the button after wake, so allow a little
extra time for boot. Holding GPIO0 during reset/power connection can enter the
ESP32 bootloader instead. Reset/power loss starts in soft-off, awaiting a power-on hold.

### Audible feedback

GPIO4 drives state feedback. Continuous alarm/working states keep the MCU awake;
the buzzer is silenced before deep sleep.

| Event | Tone |
| --- | --- |
| Power on after a 1–4 second hold | Three rising notes |
| Power off after a 3-second hold | Three falling notes, then sleeps after button release |
| SOS or confirmed suspected fall | Same three high beeps with a pause, repeated until one short cancel press |
| Off-route command during trip | Four high beeps with a pause, repeated until Route Clear |
| Joining / acquiring initial GPS / awaiting Start / no ACK | Two low beeps, pause, repeats |
| LoRaWAN uplink ACK received, with no active alarm | Three rising confirmation notes |
| Local Wi-Fi OTA/AP mode starts | Alternating low/high notes, then one high confirmation note |
| Enters deep sleep | One low confirmation beep |

The buzzer uses a small background task, so SOS, fall and LoRa connection
loops do not block GNSS or radio work. Success and shutdown melodies are bounded
foreground confirmations; firmware waits for shutdown audio before entering sleep.

### Local Wi-Fi OTA update

While the tracker is on, press S2 / GPIO0 twice shortly, then hold the third
press for three seconds. OTA/AP mode starts immediately at the three-second
threshold; releasing the button is not required. A third short press instead
keeps the normal three-click SOS action.
The tracker stops its radio and GNSS, starts the password-protected Wi-Fi hotspot
`Tracker-OTA-XXXXXX`, and prints its name and address to Serial Monitor. Connect
to that hotspot using the default password `TrackerOTA123` and open
[http://192.168.4.1](http://192.168.4.1). Change `OtaManager::apPassword` in
`include/ota_manager.h` to an 8-63 character ASCII password, then rebuild and upload.
Upload the
PlatformIO application image:

```text
.pio/build/heltec_wifi_lora_32_V3/firmware.bin
```

The page stays available for 15 minutes after its last request or upload activity.
When the timeout expires, Wi-Fi turns off and the tracker returns to normal timed
deep sleep. A successful upload restarts immediately into the new image.

The update is selected only after the complete firmware image has been received and
verified by Arduino `Update`. An interrupted or failed upload leaves the installed
firmware selected. This project intentionally does not enable bootloader rollback:
after a successful upload, the new firmware is the firmware booted on restart.

The OTA hotspot requires its Wi-Fi password; the upload page has no separate login.
Anyone connected to the hotspot can upload firmware while this mode is enabled, so do
not leave it active in an untrusted area. Flash the first OTA-capable firmware using
the wired method, and use stable J4/external power for every update; the known
battery-path reset problem can interrupt an update.

A background task debounces the button during GPS acquisition and radio blocking
calls. Power-off is applied at the next safe service point, not midway through RF.
SOS/Fall stay pending across deep sleep until a valid LoRaWAN ACK is received.
An unrelated downlink or a nonnegative send result alone does not clear these events.
Without an ACK, the device waits awake with the working tone for the retry interval (60 seconds),
or longer if required by LoRaWAN airtime limits, before another send cycle; no
application-level immediate retry loop is added. RadioLib handles any network-configured
NbTrans repetitions within its bounded send call. Normal is also confirmed.
If fall and SOS are pending together, fall is sent first; the other event remains pending.
No extra NVS writes are used; pending events can be lost on power loss. ACK confirms
network reception, not Backend alert creation. Retries can produce duplicate application
events, which the Backend must account for; the payload has no unique event ID.

### Application-controlled Low Power Mode

Report intervals are configured in `include/tracker_app.h`. A valid application downlink on
FPort `10` containing exactly `01 00 0F` (Base64 `AQAP`) changes the next
active-trip interval to `lowPowerIntervalMs`. The `00 00 00` command (Base64 `AAAA`)
returns it to `reportIntervalMs`. See the complete command table above.
Other ports, commands and payload lengths are ignored; repeated commands are safe.

The mode is retained in RTC across deep sleep with no NVS writes. Reset or power
loss returns to normal mode. Low Power uses the configured low-power interval in
debug too; preparation/waiting for Start always polls at the normal interval.
Button and fall wakeups remain enabled in Low Power Mode.

This Class A device receives queued commands in the receive windows after an
uplink, not while sleeping. An API success only confirms broker acceptance; it
does not prove the device has applied the command. The 15-byte uplink
reports `low_power_mode` in flags. The configured interval is wait/sleep time; GPS acquisition
and radio processing add to the actual time between reports.

## Pinned library versions

- Arduino-ESP32 3.3.12 / ESP-IDF 5.5.5 via pioarduino 55.03.312-1
  (PlatformIO Core 6.2.0 or newer). This includes the IPC task stack fix for
  ESP32-S3; startup compilation checks that the framework uses at least 2048 bytes.
- [RadioLib 7.8.1](https://github.com/jgromes/RadioLib/releases/tag/7.8.1)
- [TinyGPSPlus 1.1.0](https://github.com/mikalhart/TinyGPSPlus/blob/master/library.properties)

Exact versions are pinned in `platformio.ini` for reproducible builds.

Before building with an older PlatformIO installation, upgrade its Core:

```bash
~/.platformio/penv/bin/python -m pip install --upgrade platformio
```

## Code layout

- `src/main.cpp`: debug flag, interval, and startup order.
- `include/board_pins.h`: custom V3 pin map.
- `src/board_pins.cpp`: shared VEXT power and inactive actuator levels.
- `src/debug_mode.cpp`: walking simulation and Serial monitor debug diagnostics.
- `src/device_button.cpp`: debounced holds, pending SOS and RTC soft-off state.
- `src/audio_feedback.cpp`: GPIO4 audible state feedback.
- `src/telemetry.cpp`: real/cached GPS readings and the 15-byte encoder.
- `src/tracker_app.cpp`: wake-cycle orchestration, event priority, downlink dispatch and retry timing.
- `src/trip_control.cpp`: RTC trip state, FPort 11 commands, latched alarms and I-am-OK delivery state.
- `src/ota_manager.cpp`: local Wi-Fi OTA portal, update handoff and rollback confirmation.
- `src/lorawan.cpp`: radio, OTAA activation, packet exchange and session persistence calls; no sleep scheduling.
- `src/deep_sleep.cpp`: the only owner of ESP32 deep-sleep entry, wake sources and peripheral shutdown.
- `src/lorawan_storage.cpp`: RTC session storage and durable OTAA nonce storage.
- `src/fall_detection.cpp`: ADXL362 SPI event detection and wakeup handling.

`main.cpp` initializes components, then calls `TrackerApp::runCycle()`.
The application calls `DeepSleep::timed()` after handling button actions; the
button module calls `DeepSleep::powerOff()` after recording its RTC off state.
Both paths share peripheral shutdown. The sensor driver only arms its own EXT1
wakeup; it never enters sleep. The radio returns a `LoRaResult` and does not
interpret application commands, acknowledge sensor events or manage buttons.
The application services buttons and fall detection during awake waits for trip/alert feedback.

Normal GNSS acquisition accepts three distinct one-second GGA fixes spanning at
least two seconds, with at least six satellites and HDOP <= 2.00. Data must be
fresh, and implausible jumps restart the stability check. Thresholds are in
`include/tracker_app.h`. Acquisition still times out after 120 seconds before the
first accepted fix, or 30 seconds afterwards. Timeout retains the last accepted
RTC location without setting the fresh-GNSS flag. SOS and fall events use the
cached location immediately. Quality filtering does not guarantee metre-level
accuracy and may increase awake time in poor reception.

With `LORA_DEBUG = false`, VEXT is enabled for GPS acquisition: L76L VCC
uses this shared rail. VEXT is switched off during deep sleep. ADXL362 remains
on always-on 3.3 V. Debug skips GNSS initialization; VEXT stays off.

## OTAA Provisioning and Session Persistence

Device credentials (JoinEUI, DevEUI, AppKey) and the radio API reside in
`include/lorawan.h` (gitignored; copy from `include/lorawan.example.h`).
Keep the API declarations in both headers synchronized when changing the radio API.
Session/nonce storage remains separate in `lorawan_storage.h` and `lorawan_storage.cpp`.

Session state and frame counters are stored in CRC-checked RTC memory, retained
across deep sleep. Regular uplinks, including debug uplinks, do not write NVS.

- **DevNonce reservation**: Before transmitting a Join-Request, the next DevNonce is committed
  to NVS, preventing DevNonce reuse per LoRaWAN 1.0.4 specifications.
- **Session restore**: After a timed/event deep-sleep wake, the joined session is restored
  from RTC without transmitting another Join-Request, preserving battery life and gateway
  bandwidth. A user soft-off followed by a valid power-on hold intentionally forces a new join.
- **After a successful join**: Updated nonce state is saved to NVS. A successful
  join therefore makes two NVS writes; a failed join normally makes one.
- **Power loss or reset**: Power-on, software resets and watchdog resets require a
  new OTAA join. Old session counters are never restored from flash.
- **Interrupted uplink or invalid RTC**: Rejoin using durable nonce state instead
  of resuming a possibly stale session. A gateway with a working downlink is required.
- **Migration**: Existing `lorawan-otaa/state` records retain their nonce history;
  their old sessions are ignored. The next join replaces the record with nonce-only
  state (the legacy record size is retained for compatibility).
- **Invalid NVS or changed credentials**: Transmission stops without automatically
  erasing nonce history. Do not erase NVS to fix a join error without coordinating
  device reprovisioning with the network server.

RTC is not persistent across power loss. Saving nonces to RTC alone would risk
DevNonce reuse, so the small number of NVS writes around joins is intentional.
Keep RTC memory powered during deep sleep (the ESP32 default for `RTC_DATA_ATTR`).
Run `node test/session_recovery.cjs` for the host-side storage lifecycle tests.

The 15-byte payload contains status, coordinates, diagnostic flags, firmware
version and battery telemetry. ChirpStack's envelope `time` is the authoritative
event timestamp.
Latitude and longitude are little-endian float32.

Low battery is reported in flags bit 5 (`0x20`) without changing the 15-byte
payload or Normal/SOS/Fall event codes. It enters at <=15% and clears at >=20%,
configured in `include/tracker_app.h`. State is retained in RTC across deep sleep,
not in NVS. An invalid ADC reading preserves the internal state but reports
`low_battery: null`; it must not be treated as battery recovery. Battery percentage
is a voltage-based estimate. Low battery does not change reporting intervals,
play an alarm, or power off the tracker. Applications should alert on transitions
from a valid non-low reading to a valid low reading (or the first valid low reading).
Update the server decoder below to expose this field.

## Payload decoder (ChirpStack / TTN)

Copy the JavaScript below into the device's **Payload formatters → Uplink** page.
For ChirpStack, use the device profile's JavaScript codec with `decodeUplink(input)`.
Select **Custom JavaScript formatter** and save. Use FPort `2` and the 15-byte
decrypted LoRaWAN application payload, not a raw radio packet.
The `decodeUplink(input)` interface follows [The Things Stack documentation](https://www.thethingsindustries.com/docs/integrations/payload-formatters/javascript/uplink/).

| Byte offset (zero-based) | Data |
| --- | --- |
| 0 | Status: 0 = normal, 1 = SOS, 2 = suspected fall, 3 = I am OK, 4 = preparation/waiting |
| 1–4 | Latitude: float32 little-endian |
| 5–8 | Longitude: float32 little-endian |
| 9 | Flags: 0 fresh GNSS, 1 low power, 2 simulation, 3 initial fix ready, 4 battery valid, 5 low battery, 6 trip active, 7 off-route |
| 10, 11 | Firmware major, minor version |
| 12, 13 | Battery voltage in millivolts, uint16 little-endian |
| 14 | Estimated Li-ion battery percentage, 0–100 |

```javascript
function decodeUplink(input) {
    var bytes = input.bytes;
    if (input.fPort !== 2) {
        return { errors: ["Expected FPort 2"] };
    }
    if (!bytes || bytes.length !== 15) {
        return { errors: ["Expected exactly 15 payload bytes"] };
    }

    // IEEE 754 float32, little-endian; compatible with ES5.1 formatters.
    function float32LE(offset) {
        var bits = (bytes[offset] | (bytes[offset + 1] << 8) |
            (bytes[offset + 2] << 16) | (bytes[offset + 3] << 24)) >>> 0;
        var sign = (bits >>> 31) ? -1 : 1;
        var exponent = (bits >>> 23) & 255;
        var fraction = bits & 0x7fffff;
        if (exponent === 255) return NaN;
        if (exponent === 0) return sign * fraction * Math.pow(2, -149);
        return sign * (1 + fraction / 8388608) * Math.pow(2, exponent - 127);
    }

    var latitude = float32LE(1);
    var longitude = float32LE(5);
    if (!isFinite(latitude) || !isFinite(longitude) ||
        Math.abs(latitude) > 90 || Math.abs(longitude) > 180) {
        return { errors: ["Invalid latitude/longitude"] };
    }

    var events = ["Normal Update", "SOS", "Fall Detected", "I am OK", "Trip Preparation"];
    var warnings = [];
    var firmwareVersion = (bytes[10] || bytes[11])
        ? bytes[10] + "." + bytes[11] : null;
    var flags = bytes[9];
    var batteryMv = bytes[12] | (bytes[13] << 8);
    if (bytes[0] > 4) warnings.push("Unknown status code");
    if (latitude === 0 && longitude === 0) {
        warnings.push("Coordinates are 0,0; GPS fix may be unavailable");
    }
    return {
        data: {
            status: bytes[0],
            event: events[bytes[0]] || "Unknown",
            latitude: latitude,
            longitude: longitude,
            battery_mv: (flags & 16) ? batteryMv : null,
            battery_percent: (flags & 16) ? bytes[14] : null,
            firmware_version: firmwareVersion,
            flags: firmwareVersion === null ? null : {
                gps_fresh: !!(flags & 1),
                low_power_mode: !!(flags & 2),
                debug_simulation: !!(flags & 4),
                initial_fix_ready: !!(flags & 8),
                battery_valid: !!(flags & 16),
                low_battery: (flags & 16) ? !!(flags & 32) : null,
                trip_active: !!(flags & 64),
                off_route: !!(flags & 128)
            }
        },
        warnings: warnings,
        errors: []
    };
}
```

Test with FPort `2` and this hexadecimal payload:

```text
02000060410000C942300103740E0F
```

Expected `data` output:

```json
{
  "status": 2,
  "event": "Fall Detected",
  "latitude": 14,
  "longitude": 100.5,
  "battery_mv": 3700,
  "battery_percent": 15,
  "firmware_version": "1.3",
  "flags": {
    "gps_fresh": false,
    "low_power_mode": false,
    "debug_simulation": false,
    "initial_fix_ready": false,
    "battery_valid": true,
    "low_battery": true,
    "trip_active": false,
    "off_route": false
  }
}
```

`Fall Detected` retains the original event name for compatibility, but indicates
a suspected fall based on low acceleration followed by impact, not a confirmed human fall. This script omits
the original decoder's `altitude: 10` because the firmware does not transmit altitude.
With debug enabled, coordinates are simulated. `flags.debug_simulation` identifies
them. Button/fall events may use previously cached coordinates rather than a fresh
fix; check `flags.gps_fresh`.

## Verification before handoff

### Tracking watchdog

`TrackerApp::beginWatchdog()` adds a `tracker-cycle` user to ESP32's hardware-backed
Task WDT before peripheral initialization. The framework's original 5-second
timeout, idle-task subscriptions and panic/reboot behavior are not reconfigured;
the Interrupt WDT is unchanged. A one-second heartbeat on the existing ESP timer
service feeds only this user while elapsed tracking time is below
`AppConfig::trackingWatchdogMs` (300 seconds). GNSS/radio calls cannot extend this
budget. A progressing serviced awake-wait loop renews the budget every 60 seconds
to allow long Low Power waits with continuous alarms. At the deadline the heartbeat stops feeding and the hardware Task WDT
expires within approximately its next 5 seconds, rather than a timer callback
calling `esp_restart()`. If the timer-service task itself stalls, the hardware
watchdog also stops receiving this user's heartbeat and can expire independently.
No new monitoring task is created. A mutex serializes feeding and user deletion.
The heartbeat and tracker subscription are removed immediately before deep sleep
(including button power-off) and on entry to OTA. Existing framework watchdogs and
OTA's inactivity timeout remain active as appropriate; not every fault is covered.
Boot logs include the ESP reset reason. Unexpected reset takes the existing safe
soft-off path; power-on then rejoins OTAA, preserving durable DevNonce history.
The 300-second budget must be validated on hardware against GNSS acquisition,
network RX delays and configured NbTrans repetitions. Host tests simulate the
lifecycle/deadline only; real watchdog reset and OTA upload must be tested on a
device. This does not guarantee retention of RTC events across abnormal resets.

Build using `pio run`. Flash separately, then check gateway/network-server live
data: `sendReceive=0` means no downlink and no confirmed-uplink ACK, not proof of gateway receipt.
Verify consecutive simulated positions, approximately 15-second TX intervals,
and increasing frame counters after reset and power interruption. Finally test
with debug disabled, real GPS, timer wakeup, button wakeup and fall wakeup.
