# ESP32-S3 LoRaWAN tracker

The radio uses RadioLib 7.7.1 with SX1262, OTAA (Over-The-Air Activation), AS923, Class A,
unconfirmed uplinks on port 2. No Heltec activation license or Serial
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

## Debug walking track

Set these constants in `src/main.cpp`:

```cpp
constexpr bool LORA_DEBUG = true;
constexpr uint32_t LORA_DEBUG_INTERVAL_MS = 15000;
```

The first uplink is attempted after initialization and join. Both modes complete
the Class A radio transaction, save the session in RTC and enter real deep sleep.
Debug normally sleeps for 15 seconds; boot and radio time add to the TX interval.
Initialization/storage/radio failures sleep before retrying (normally 60 seconds).
GPS is not required. Debug uses the production DR3 (SF9/BW125), with synthetic coordinates
starting at 13.756300, 100.501800. Each step moves at 0.9–1.5 m/s with a gradual
random turn of up to 30.5 degrees. At 15 seconds this is about 13.5–22.5 metres.
Deep-sleep wakes continue the track from RTC, including time spent asleep.
Reset or power loss starts the simulated track again. This is a free walking simulation,
not a route constrained to roads. Its timestamp is elapsed simulation time, not UTC.
Serial labels it `[SIMULATED WALK]`; the existing payload has no simulation bit,
so gateway/application data must be treated as test data while debug is enabled.

Set `LORA_DEBUG = false` for deployment: acquire GPS, send at DR3 (SF9/BW125),
complete both receive windows, then deep sleep for 15 minutes (or longer if the
MAC duty-cycle timer requires it). Button GPIO0 and fall GPIO7 use the
original low/high wakeup polarities. Both modes validate button holds and handle
real sensor events; an EXT0 wake alone does not mean SOS.

### Button and soft power-off

Short-click actions are selected on debounced release. Power-off and OTA holds
start immediately once their three-second threshold is reached.

| Current state | Hold before release | Action |
| --- | --- | --- |
| On | Three short presses, each under 1 second, within 1.5 seconds | Queue SOS (status 1) |
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
ESP32 bootloader instead. Reset/power loss clears the RTC soft-off state.

### Audible feedback

GPIO4 drives a short, distinct buzzer tone while the tracker is awake. It is
silenced before every deep sleep and does not hold the device awake.

| Event | Tone |
| --- | --- |
| Power on after a 1–4 second hold | Three rising notes |
| Power off after a 3-second hold | Three falling notes, then sleeps after button release |
| SOS queued by three short presses | Three high, evenly-spaced beeps |
| ADXL362 suspected fall | Four urgent high beeps; final beep is longer |
| LoRaWAN OTAA Join is in progress | Two low beeps, pause, then repeats until success or failure |
| LoRaWAN session joined or restored | Three rising confirmation notes |
| Local Wi-Fi OTA/AP mode starts | Alternating low/high notes, then one high confirmation note |
| Enters deep sleep | One low confirmation beep |

The buzzer uses a small background task, so SOS, fall and LoRa connection
melodies do not block GNSS or radio work. The shutdown melody is the exception:
firmware waits for it to complete before entering deep sleep.

### Local Wi-Fi OTA update

While the tracker is on, press S2 / GPIO0 twice shortly, then hold the third
press for three seconds. OTA/AP mode starts immediately at the three-second
threshold; releasing the button is not required. A third short press instead
keeps the normal three-click SOS action.
The tracker stops its radio and GNSS, starts the open Wi-Fi hotspot
`Tracker-OTA-XXXXXX`, and prints its name and address to Serial Monitor. Connect
to that hotspot and open [http://192.168.4.1](http://192.168.4.1). Upload the
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

The OTA hotspot has no password or upload authentication by design. Anyone within
Wi-Fi range can upload firmware only while this mode is deliberately enabled, so do
not leave it active in an untrusted area. Flash the first OTA-capable firmware using
the wired method, and use stable J4/external power for every update; the known
battery-path reset problem can interrupt an update.

A background task debounces the button during GPS acquisition and radio blocking
calls. Power-off is applied at the next safe service point, not midway through RF.
SOS stays pending across deep sleep until the local send succeeds (unconfirmed
uplinks do not guarantee gateway receipt). If fall and SOS are pending together,
fall is sent first and SOS retries on a short timer. No extra NVS writes are used.

### Application-controlled Low Power Mode

Normal production sleep remains 15 minutes. A valid application downlink on
FPort `10` containing exactly `01 00 0F` (Base64 `AQAP`) changes the next
production sleep to one hour. The `00 00 00` command (Base64 `AAAA`) returns it
to the normal 15-minute interval. See [MQTT contract](docs/mqtt-payload.md).
Other ports, commands and payload lengths are ignored; repeated commands are safe.

The mode is retained in RTC across deep sleep with no NVS writes. Reset or power
loss returns to the normal 15-minute mode. Low Power also sleeps for one hour in
debug, matching production behavior.
Button and fall wakeups remain enabled in Low Power Mode.

This Class A device receives queued commands in the receive windows after an
uplink, not while sleeping. An API success only confirms broker acceptance; it
does not prove the device has applied the command. The 15-byte uplink
has no Low Power status field. The one-hour interval is sleep time; GPS acquisition
and radio processing add to the actual time between reports.

## Pinned library versions

- [RadioLib 7.7.1](https://github.com/jgromes/RadioLib/releases/tag/7.7.1)
- [TinyGPSPlus 1.1.0](https://github.com/mikalhart/TinyGPSPlus/blob/master/library.properties)

Exact versions are pinned in `platformio.ini` for reproducible builds.

## Code layout

- `src/main.cpp`: debug flag, interval, and startup order.
- `include/board_pins.h`: custom V3 pin map.
- `src/board_pins.cpp`: shared VEXT power and inactive actuator levels.
- `src/debug_mode.cpp`: walking simulation and Serial monitor debug diagnostics.
- `src/device_button.cpp`: debounced holds, pending SOS and RTC soft-off state.
- `src/audio_feedback.cpp`: GPIO4 audible state feedback.
- `src/telemetry.cpp`: real/cached GPS readings and the 15-byte encoder.
- `src/tracker_app.cpp`: wake-cycle orchestration, event priority, downlink dispatch and retry timing.
- `src/ota_manager.cpp`: local Wi-Fi OTA portal, update handoff and rollback confirmation.
- `src/lora_wan.cpp`: radio, OTAA activation, packet exchange and session persistence calls; no sleep scheduling.
- `src/deep_sleep.cpp`: the only owner of ESP32 deep-sleep entry, wake sources and peripheral shutdown.
- `src/lorawan_storage.cpp`: RTC session storage and durable OTAA nonce storage.
- `src/fall_detection.cpp`: ADXL362 SPI event detection and wakeup handling.

`main.cpp` initializes components, then calls `TrackerApp::runCycle()`.
The application calls `DeepSleep::timed()` after handling button actions; the
button module calls `DeepSleep::powerOff()` after recording its RTC off state.
Both paths share peripheral shutdown. The sensor driver only arms its own EXT1
wakeup; it never enters sleep. The radio returns a `LoRaResult` and does not
interpret application commands, acknowledge sensor events or manage buttons.
There is no separate awake-only scheduling loop or unused `go_sleep()` path.

With `LORA_DEBUG = false`, VEXT is enabled for GPS acquisition: L76L VCC
uses this shared rail. VEXT is switched off during deep sleep. ADXL362 remains
on always-on 3.3 V. Debug skips GNSS initialization; VEXT stays off.

## OTAA Provisioning and Session Persistence

Device credentials (JoinEUI, DevEUI, AppKey) reside in `include/lorawan_credentials.h`
(gitignored; copy from `include/lorawan_credentials.example.h`).

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

## Payload decoder (TTN / The Things Stack)

Copy the JavaScript below into the device's **Payload formatters → Uplink** page.
Select **Custom JavaScript formatter** and save. Use FPort `2` and the 15-byte
decrypted LoRaWAN application payload, not a raw radio packet.
The `decodeUplink(input)` interface follows [The Things Stack documentation](https://www.thethingsindustries.com/docs/integrations/payload-formatters/javascript/uplink/).

| Byte offset (zero-based) | Data |
| --- | --- |
| 0 | Status: 0 = normal, 1 = SOS button hold, 2 = free-fall detected / suspected fall |
| 1–4 | Latitude: float32 little-endian |
| 5–8 | Longitude: float32 little-endian |
| 9 | Flags: bit 0 = fresh GNSS fix, bit 1 = low-power mode, bit 2 = debug simulation, bit 4 = valid battery reading |
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

    var events = ["Normal Update", "SOS", "Fall Detected"];
    var warnings = [];
    var firmwareVersion = (bytes[10] || bytes[11])
        ? bytes[10] + "." + bytes[11] : null;
    var flags = bytes[9];
    var batteryMv = bytes[12] | (bytes[13] << 8);
    if (bytes[0] > 2) warnings.push("Unknown status code");
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
                battery_valid: !!(flags & 16)
            }
        },
        warnings: warnings,
        errors: []
    };
}
```

Test with FPort `2` and this hexadecimal payload:

```text
02000060410000C942000100740E0F
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
  "firmware_version": "1.2",
  "flags": {
    "gps_fresh": false,
    "low_power_mode": false,
    "debug_simulation": false,
    "battery_valid": true
  }
}
```

`Fall Detected` retains the original event name for compatibility, but indicates
a suspected fall based on free-fall, not a confirmed human fall. This script omits
the original decoder's `altitude: 10` because the firmware does not transmit altitude.
With debug enabled, coordinates are simulated. `flags.debug_simulation` identifies
them. Button/fall events may use previously cached coordinates rather than a fresh
fix; check `flags.gps_fresh`.

## Verification before handoff

Build using `pio run`. Flash separately, then check gateway/network-server live
data: unconfirmed `sendReceive=0` means no downlink, not proof of gateway receipt.
Verify consecutive simulated positions, approximately 15-second TX intervals,
and increasing frame counters after reset and power interruption. Finally test
with debug disabled, real GPS, timer wakeup, button wakeup and fall wakeup.
