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
Off → Button power-on → OTAA Join + GPS acquisition → Await Start → Travelling
                                                                                ↓
Off ← Application End Trip command ←─────────────────────────────────────────────┘
```

GNSS acquisition and OTAA Join run concurrently. A dedicated FreeRTOS GPS task
on core 0 (4096-byte stack, priority 1) continuously drains UART1 and exclusively
owns the NMEA parser and quality/stability checks. It publishes complete fix
snapshots through a one-slot queue. The GPS task alone logs search progress once
per second and reports the first qualified fix or timeout once per acquisition,
including while LoRa is joining. The main task alone updates RTC
coordinates/trip state and owns LoRa; GPS no longer runs inside RadioLib wait
callbacks. Shutdown signals the GPS task and waits for its acknowledgement
before closing UART1 or cutting peripheral power. Task/queue allocation failure
disables GPS safely without blocking LoRa. The GPS timeout starts before Join
and is not restarted after the airtime wait. If no fix qualifies, the device
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

For the deployed SF12 test network, the tracker AS923 band defaults to no
downlink dwell-time limit, matching ChirpStack's RX1 DR0/SF12 replies.
RadioLib's `setDwellTime(false)` disables only the uplink limit; leaving the
AS923 downlink default enabled instead clamps RX1 to DR2/SF10. This configuration
must comply with the deployment's regional requirements. After changing this
default, use the button to power off/on and establish a new OTAA session rather
than restoring an older RTC session. Do not erase stored DevNonce history.
Network MAC commands can subsequently change the dwell-time settings.

FPort identifies an application-data channel within LoRaWAN; it is not a GPIO
or TCP/UDP port. JSON `data` must contain **Base64-encoded command bytes**,
not a hexadecimal string.

| Command | FPort | Hex bytes | Base64 (`data`) | Device action |
| --- | ---: | --- | --- | --- |
| Start Trip | 11 | `01` | `AQ==` | Remember Start; begin travelling once the initial fix is ready |
| End Trip | 11 | `02` | `Ag==` | Clear trip state, audio and pending events; enter soft-off until button power-on |
| Off Route | 11 | `03` | `Aw==` | Repeat the off-route alarm during a trip; route detection is not performed on the device |
| Route Clear | 11 | `04` | `BA==` | Stop the off-route alarm without cancelling SOS/Fall |
| Acknowledge SOS | 11 | `05` | `BQ==` | Add a fourth confirmation beep to every repeating alarm phrase; the alarm continues |
| Acknowledge Fall | 11 | `06` | `Bg==` | Same audible receipt confirmation as Acknowledge SOS; the alarm continues |
| Resolve SOS/Fall | 11 | `07` | `Bw==` | Stop the shared SOS/Fall alarm and clear pending SOS/Fall reports; keep the trip running, without sending I-am-OK |
| Low Power ON | 10 | `01 00 0F` | `AQAP` | Use `AppConfig::lowPowerIntervalMs` during a trip |
| Low Power OFF | 10 | `00 00 00` | `AAAA` | Return to `AppConfig::reportIntervalMs` |

FPort 11 requires **exactly 1 byte**; FPort 10 requires **exactly 3 bytes**.
Unknown commands and incorrect lengths are ignored. Repeated Start/End/Route
commands are idempotent. `03` is ignored before a trip starts. The application
must detect route deviations and send `04` when the device returns to the route.
Cancelling SOS with the button does not clear the off-route alarm.

Application acknowledgements (`05` / `06`) are distinct from LoRaWAN ACKs.
Both use the shared SOS/Fall alarm: they do not cancel it, clear pending events,
or report `I am OK`. Cancel with a short button press, or resolve remotely with `07`.
The first application receipt latches an acknowledged alarm pattern: three SOS/Fall
beeps followed by one distinct fourth confirmation beep, repeated every phrase
until button cancellation or remote resolution. Duplicate receipts do not add extra notes. Receipts
received after cancellation are silent; a new emergency clears the latched receipt.

Resolution (`07`) applies to both SOS and Fall, with or without a prior application
acknowledgement. It is idempotent and does not end the trip or clear an independent
off-route alarm. Unlike a button cancellation, it does not declare the wearer safe
or generate an I-am-OK report. Class A delivers it in a receive window after an uplink.

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

FPort **2**, **9-byte** primary payload, always confirmed:

| Byte offset | Format | Meaning |
| --- | --- | --- |
| 0 | uint8 | Event/status from the table below |
| 1–3 | signed int24 little-endian × 90 / 8388607 | Latitude |
| 4–6 | signed int24 little-endian × 180 / 8388607 | Longitude |
| 7 | bit flags | See the flag definitions below |
| 8 | uint8 | Battery percent |

FPort **3** carries a separate **4-byte** device-info packet: firmware major,
minor, then battery mV uint16 LE. See the decoder section for scheduling and
application caching requirements. The decoder also accepts legacy 15-byte frames.

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

- Every uplink requests a LoRaWAN ACK. Once LoRa/GPS are ready, both while
  awaiting Start and during a trip, each ACK produces the success melody and
  a missing ACK produces the error melody, unless an emergency/off-route alarm
  is active. A missing ACK schedules a retry after
  `AppConfig::retryInitialMs` (60 seconds), or a longer radio-required wait.
  Retries are not sent in a tight loop.
- ACK confirms LoRaWAN network reception through a gateway;
  **it does not confirm that the application created an alert**.
- SOS/Fall reports retry until acknowledged, but the alarm continues until
  **one short button press** confirms the user is safe. Cancellation stops retries
  of the old event and queues status 3 until acknowledged. A new emergency before
  successful I-am-OK delivery takes priority and supersedes the older OK report.
- SOS and Fall share one melody; off-route uses another alarm pattern.
  Audio priority is SOS/Fall > off-route > active preparation.
  Success melodies do not interrupt an emergency alarm awaiting cancellation.
- Preparation, missing ACK and active alarms **keep the MCU awake**.
  Once ready and acknowledged, awaiting Start uses timed deep sleep between
  reports, just like normal active-trip operation without an alarm. Class A
  receives queued Start commands only in the receive windows after an uplink;
  keeping the MCU awake does not add receive windows. Readiness and per-transmission
  ACK/error confirmations are one-shot rather than repeating loops. Awake waits
  consume more power than deep sleep; the 90 µA sleep measurement does not apply.
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
GPS is not required in debug. Debug uses the shared configured uplink DR, with synthetic coordinates
starting at 13.756300, 100.501800. Each step moves at 0.9–1.5 m/s with a gradual
random turn of up to 30.5 degrees. At 15 seconds this is about 13.5–22.5 metres.
Deep-sleep wakes continue the track from RTC, including time spent asleep.
Reset or power loss starts the simulated track again. This is a free walking simulation,
not a route constrained to roads. Its timestamp is elapsed simulation time, not UTC.
Serial labels it `[SIMULATED WALK]`; `debug_simulation` marks synthetic positions.

Set `LORA_DEBUG = false` for deployment: acquire GPS, send at the configured uplink DR,
complete receive windows, then use the configured report interval (or longer if
the MAC duty-cycle timer requires it). Button GPIO0 and fall GPIO7 use the
original low/high wakeup polarities. Both modes validate button holds and handle
real sensor events; an EXT0 wake alone does not mean SOS.

### Main-project data rate

`AppConfig::uplinkDataRate` in `include/tracker_app.h` is DR2
(SF10/BW125). AS923 DR0 is SF12 and DR1 is SF11;
do not use DR12 to request SF12. ADR remains disabled. The existing local
uplink dwell-limit override remains disabled; use this only where the regional
and network configuration legally permit no uplink dwell limit. Network MAC
commands can still change session limits.

Upload only this main project, without erasing flash. Use its normal soft-power
button procedure to join/prepare. Serial prints the configured DR, requested TX
DR, `sendReceive` result, and ACK result. Check the UG67 Traffic page for the
actual uplink at SF10/BW125, then ChirpStack frames/events for DR2 and the ACK.
A successful configuration log is not proof of RF transmission or delivery.
Keep the payload, TX power, reporting interval, and gateway settings unchanged
for this comparison. MAC limits can still extend the reporting interval.

### Button and soft power-off

Short-click actions are selected on debounced release. Power-off and OTA holds
start immediately once their three-second threshold is reached.

OTAA join and uplink TX/RX waits also check pending power-off on the main task
through a button-aware RadioLib HAL. Holding the button does not have to wait
for the entire join/receive attempt to finish. Shutdown never starts inside an
SPI transaction, and ordinary short presses do not pause radio receive timing.
An interrupted radio exchange may be lost; the next user power-on starts a new
join with the next durably reserved DevNonce. Release the button after the
power-off sound to complete deep-sleep entry.

| Current state | Hold before release | Action |
| --- | --- | --- |
| On | Three short presses, each under 1 second, within 1.5 seconds | Queue SOS (status 1) |
| SOS/Fall alarm active | One short press | Stop the alarm, play one local success melody, and queue I am OK (status 3); the melody does not indicate network delivery |
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
| Application ACK during SOS/Fall | Three alarm beeps plus a fourth confirmation beep on every repeating phrase until button cancellation or remote resolution; network ACK alone does not enable it |
| Off-route command during trip | Four high beeps with a pause, repeated until Route Clear |
| Initial Join / GPS acquisition still in progress | Two low beeps, pause, repeats |
| Initial LoRa connection and GPS fix both ready | ACK confirmation melody once, then quiet while awaiting Start |
| Initial LoRa connection or GPS acquisition fails | Error melody once, then quiet; no repeating error loop |
| Automatic retry after preparation failure | Resume the same Join/GPS working melody; success or failure is reported once for the new attempt |
| Awaiting application Start | Each telemetry ACK plays success, then the timed deep-sleep beep; Web Debug simulates sleep without the sleep beep |
| LoRaWAN uplink ACK during a trip, with no active alarm | Three rising confirmation notes |
| No uplink ACK during a trip, with no active alarm | Error melody once for that transmission |
| Local Wi-Fi OTA/AP mode starts | Alternating low/high notes, then one high confirmation note |
| Enters timed/report deep sleep | One low confirmation beep |

One background task exclusively owns the buzzer GPIO and LEDC peripheral.
Commands carry a generation so cancellation also rejects a melody that the
worker has already removed from its queue but has not started. Synchronous
confirmations still run on that worker; the caller waits for completion.

Power-off closes audio admission first, interrupts the current melody, discards
queued feedback and plays the three falling PowerOff notes exactly once. The
worker then detaches LEDC and drives the buzzer LOW before acknowledging
shutdown. The device remains silent while waiting for button release and while
stopping LoRa/GPS. Soft-off does **not** append the timed-sleep beep. All sleep
paths also close audio admission before applying GPIO holds, so a late producer
cannot restart the buzzer. Audio is available again after the next boot/wake.

### Optional read-only Wi-Fi debug page

Set `TRACKER_WEB_DEBUG` to `1` in `include/debug_web.h`, or add
`-D TRACKER_WEB_DEBUG=1` to the existing PlatformIO `build_flags`, then build
and upload. The default is `0`: no debug AP, HTTP task or debug buffers are
started in normal builds. This flag is independent of `LORA_DEBUG`; keep
`LORA_DEBUG=false` to debug real GPS and tracking.

After turning the tracker on:

1. Connect to Wi-Fi `Tracker-Debug-<prefix>` (`prefix` is the first six hexadecimal digits of the uppercase LoRaWAN DevEUI, including leading zeros). Devices sharing this prefix have the same AP name.
2. Use the development password `TrackerDebug123` (configurable in
   `include/debug_web.h`; change it for shared environments).
3. Open [http://192.168.4.1](http://192.168.4.1). If the phone warns that this
   Wi-Fi has no Internet, stay connected to it.

The plain HTML page auto-refreshes once per second and has no control buttons
apart from refresh/pause. It shows trip/preparation state, live GPS progress and
satellite count, battery reading, last prepared coordinates/status/flags,
last TX return code and network ACK, radio cooldown and free heap. Events mirror
the firmware's Serial log through a shared console, on both USB/J1 and UART/J4
configurations. The bounded RAM history retains the latest 64 line chunks
(up to 159 characters each); long lines are split and older chunks are overwritten.
It also captures firmware logs before AP startup and when USB is disconnected.
ROM boot messages, panic output and libraries writing directly to hardware
Serial/ESP logging are not intercepted. State and TX timestamps distinguish stale information while main-task
operations are in progress; GPS progress is published independently by its
owning task. History is lost on reset.

When the AP is active, report sleep is **simulated with an awake wait**, so Wi-Fi
does not disconnect between reports. Radio airtime/duty-cycle waits remain
unchanged. This consumes more power and is not suitable for sleep-current or
production battery-life measurement. Long-hold power-off and End Trip still
shut the tracker down; the debug task is stopped before sleep. Entering OTA
stops the debug AP/server first, then starts the existing OTA AP on port 80.
If debug AP/task startup fails, the tracker falls back to normal sleep behavior.

GPS progress logs include HDOP, GGA fix type, stable-fix count and a rejection
reason such as `HDOP-too-high`, `stale-location`, `too-few-satellites`,
`position-jump` or `waiting-stable-fixes`. The final progress line is also
printed before timeout. Six satellites alone do not satisfy the
quality requirements (HDOP <= 2.50, fresh autonomous/DGPS fix and three stable
epochs spanning at least two seconds).

The HTTP worker runs separately on core 0 and reads synchronized snapshots;
it never calls LoRa, reads the GPS UART or changes trip state. The console tee
only copies bytes into a fixed buffer; it never sends HTTP from the radio or
GPS task. No keys or AP passwords are printed by this feature. Existing
application downlink diagnostics and optionally enabled raw NMEA are mirrored,
so the page may contain location and application payload data. HTTP is
local and unencrypted; only use this development AP with trusted clients.
Wi-Fi adds scheduling/power overhead: verify LoRa RX timing on real hardware.

### Local Wi-Fi OTA update

While the tracker is on, press S2 / GPIO0 twice shortly, then hold the third
press for three seconds. OTA/AP mode starts immediately at the three-second
threshold; releasing the button is not required. A third short press instead
keeps the normal three-click SOS action.
The tracker stops its radio and GNSS, replaces any previous audio loop with the
OTA confirmation, then starts the password-protected Wi-Fi hotspot
`Tracker-OTA-<prefix>` (the first six hexadecimal digits of the uppercase LoRaWAN DevEUI, including leading zeros; devices sharing this prefix have the same AP name) and prints its name and address to Serial Monitor. The OTA
service loop continues checking the three-second power-off hold. Connect
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
does not prove the device has applied the command. The 9-byte uplink
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
- `src/debug_web.cpp`: optional read-only debug AP/server, snapshots and bounded event history.
- `src/device_button.cpp`: debounced holds, pending SOS and RTC soft-off state.
- `src/device_lifecycle.cpp`: terminal shutdown gate, PowerOff confirmation and button-release wait.
- `src/audio_feedback.cpp`: single-worker buzzer ownership, generation cancellation and sleep barriers.
- `src/audio_policy.cpp`: preparation/alarm/off-route priority and application/network ACK feedback.
- `src/telemetry.cpp`: real/cached GPS readings and compact telemetry/device-info encoders.
- `src/tracker_app.cpp`: wake-cycle orchestration, event priority, downlink dispatch and retry timing.
- `src/tracking_watchdog.cpp`: hardware Task WDT subscription, bounded feeding and cleanup.
- `src/trip_control.cpp`: RTC trip state, FPort 11 commands, latched alarms and I-am-OK delivery state.
- `src/ota_manager.cpp`: local Wi-Fi OTA portal, update handoff and rollback confirmation.
- `src/lorawan.cpp`: radio, OTAA activation, packet exchange and session persistence calls; no sleep scheduling.
- `src/deep_sleep.cpp`: the only owner of ESP32 deep-sleep entry, wake sources and peripheral shutdown.
- `src/lorawan_storage.cpp`: RTC session storage and durable OTAA nonce storage.
- `src/fall_detection.cpp`: ADXL362 SPI event detection and wakeup handling.

`main.cpp` initializes components, then calls `TrackerApp::runCycle()`.
The application calls `DeepSleep::timed()` after handling button actions.
Both a long button hold and an application End Trip command record the RTC
off state through the button module, then use the same `DeviceLifecycle`
shutdown coordinator. It owns the terminal shutdown state and calls
`DeepSleep::powerOff()` after silencing audio and waiting for button release.
Both sleep paths share peripheral shutdown. The sensor driver only arms its own EXT1
wakeup; it never enters sleep. The radio returns a `LoRaResult` and does not
interpret application commands, acknowledge sensor events or manage buttons.
The application services buttons and fall detection during awake waits for trip/alert feedback.

TripControl contains state transitions and consumable feedback snapshots, not
audio-driver calls. The GPS driver reports `GnssStatus`; the application maps
qualified fixes and acquisition failures into trip preparation state. LoRa
remains owned by the main task while GPS acquisition runs on its existing
dedicated task. Buffered RX tracing is controlled by
`AppConfig::radioRxTrace` (currently enabled for diagnosis); disabling the
trace changes logging only, not radio parameters or RX scheduling.

### Refactor verification

Run all host regressions with `node --test test/*.cjs`, then build with
`pio run -e heltec_wifi_lora_32_V3`. The trip-flow regression compiles the whole
production dispatcher against fake peripherals. Audio regressions run the
production worker on host threads, including a forced pause after dequeue,
queued ACK cancellation, SOS/Fall/working-loop shutdown and sleep GPIO gating.

Hardware acceptance is still required: hold the power button during Join,
GPS acquisition, Await Start, TX/RX and SOS/Fall; hear only one PowerOff melody,
continue holding for several seconds and verify silence, then release and
verify deep sleep. Repeat using End Trip, verify timed sleep and subsequent
power-on, and measure the off-state current. Host tests do not establish
physical buzzer levels, radio delivery or board sleep current.

Normal GNSS acquisition accepts three distinct one-second GGA fixes spanning at
least two seconds, with at least six satellites and HDOP <= 2.50. Data must be
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

The primary FPort 2 payload is 9 bytes: status, coordinates, flags and battery
percentage. Coordinates are signed normalized int24 little-endian, scaled by
`8388607 / 90` for latitude and `8388607 / 180` for longitude. This gives steps
of approximately 1.2 m latitude and up to 2.4 m longitude at the equator, not a
guarantee of GPS accuracy. ChirpStack's envelope `time` is the event timestamp.

FPort 3 carries firmware major/minor and battery millivolts in a separate 4-byte
device-info packet. It is attempted once after the first normal/preparation
uplink ACK per user power-on, in a later legal TX slot. Emergency/OK reports
take priority. It does not retry if its ACK is lost. This extra startup packet
costs airtime; the 9-byte saving applies to subsequent primary reports, not to
the combined startup exchange. Applications must route FPort 3 separately and
cache its fields per device; primary packets omit these measurements and the
decoder returns `null` for them. Never treat those nulls as a battery failure or
overwrite cached device-info fields with them. Device-info battery millivolts
are a startup sample, not the current voltage of each primary report.

Deploy the decoder below before flashing the new firmware. It accepts both
new 9-byte and legacy 15-byte FPort 2 messages, plus FPort 3 device info. Legacy
15-byte coordinates remain float32 little-endian. Downlinks are unchanged.

Low battery is reported in flags bit 5 (`0x20`) without changing the
Normal/SOS/Fall event codes. It enters at <=15% and clears at >=20%,
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
Select **Custom JavaScript formatter** and save. Use FPort `2` and the 9-byte
decrypted LoRaWAN application payload, not a raw radio packet.
The `decodeUplink(input)` interface follows [The Things Stack documentation](https://www.thethingsindustries.com/docs/integrations/payload-formatters/javascript/uplink/).

| Byte offset (zero-based) | Data |
| --- | --- |
| 0 | Status: 0 = normal, 1 = SOS, 2 = suspected fall, 3 = I am OK, 4 = preparation/waiting |
| 1–3 | Latitude: signed int24 LE × 90 / 8388607 |
| 4–6 | Longitude: signed int24 LE × 180 / 8388607 |
| 7 | Flags: 0 fresh GNSS, 1 low power, 2 simulation, 3 initial fix ready, 4 battery valid, 5 low battery, 6 trip active, 7 off-route |
| 8 | Estimated Li-ion battery percentage, 0–100 |

The signed int24 value `-8388608` is reserved for invalid coordinates; `0,0`
still represents an unavailable initial fix as in the legacy format. All flags
and event codes are retained. FPort 3: bytes 0/1 = firmware major/minor, bytes
2/3 = battery mV uint16 LE (`0` = unavailable). Legacy FPort 2 uses status at 0,
float32 coordinates at 1/5, flags at 9, firmware at 10/11, mV at 12/13 and % at 14.

```javascript
function decodeUplink(input) {
    var bytes = input.bytes;
    if (input.fPort === 3) {
        if (!bytes || bytes.length !== 4) return { errors: ["Expected 4 device-info bytes"] };
        var infoMv = bytes[2] | (bytes[3] << 8);
        return { data: {
            message_type: "device_info",
            firmware_version: bytes[0] + "." + bytes[1],
            battery_mv: infoMv || null
        }, warnings: [], errors: [] };
    }
    if (input.fPort !== 2) {
        return { errors: ["Expected FPort 2 or 3"] };
    }
    if (!bytes || (bytes.length !== 9 && bytes.length !== 15)) {
        return { errors: ["Expected 9 compact or 15 legacy payload bytes"] };
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

    function int24LE(offset) {
        var value = bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16);
        if (value & 0x800000) value -= 0x1000000;
        return value === -8388608 ? NaN : value;
    }
    var compact = bytes.length === 9;
    var latitude = compact ? int24LE(1) * 90 / 8388607 : float32LE(1);
    var longitude = compact ? int24LE(4) * 180 / 8388607 : float32LE(5);
    if (!isFinite(latitude) || !isFinite(longitude) ||
        Math.abs(latitude) > 90 || Math.abs(longitude) > 180) {
        return { errors: ["Invalid latitude/longitude"] };
    }

    var events = ["Normal Update", "SOS", "Fall Detected", "I am OK", "Trip Preparation"];
    var warnings = [];
    var firmwareVersion = !compact && (bytes[10] || bytes[11])
        ? bytes[10] + "." + bytes[11] : null;
    var flags = bytes[compact ? 7 : 9];
    var batteryMv = compact ? null : (bytes[12] | (bytes[13] << 8));
    var batteryPercent = bytes[compact ? 8 : 14];
    if ((flags & 16) && batteryPercent > 100) return { errors: ["Invalid battery percentage"] };
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
            battery_percent: (flags & 16) ? batteryPercent : null,
            firmware_version: firmwareVersion,
            flags: !compact && firmwareVersion === null ? null : {
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

Compact example: FPort `2`, hex `023EE913777747300F` decodes to status 2,
approximately latitude 14 / longitude 100.5, battery 15% and low battery true.
The coordinate differences are quantization, not a change to the GPS filter.
FPort `3`, hex `0103740E` reports firmware 1.3 and battery 3700 mV.

Legacy compatibility example: test with FPort `2` and this 15-byte hexadecimal payload:

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
