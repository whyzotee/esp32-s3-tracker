# Custom V3 hardware and upload

Source of truth: [MCU](schematic/MCU.jpg), [GPS and accelerometer](schematic/L76_ADXL362.jpg),
and [USB/power](schematic/JTAG.jpg). The confirmed module is
[Wireless Shell V3](https://heltec.org/project/wireless-shell-v3/), **HTIT-Wsh_V3**,
not Wireless Stick or a stock WiFi LoRa 32 development board. It has no OLED.
The installed PlatformIO platform has no separate Shell V3 board manifest, so
the project retains the `heltec_wifi_lora_32_V3` ESP32-S3/8 MB base profile and
environment name. Do not assume a separate `board_build.variant` override is
configured; inspect the current `platformio.ini`. Custom wiring
is explicitly assigned in `include/board_pins.h`. Do not enable octal PSRAM:
GPIO33–37 are used by the custom sensor bus.

## Pin map

Numbers below are ESP32 **GPIO numbers**, not U5 physical pad numbers.
Edit application pin assignments in `include/board_pins.h`.

| Signal | GPIO / supply | Notes |
| --- | --- | --- |
| GPS_TXD, L76L TX1 | 41 | ESP32 UART1 RX, 9600 baud |
| GPS_RXD, L76L RXD1 | 42 | ESP32 UART1 TX |
| L76L VCC | VEXT | Enabled by GPIO36 LOW |
| L76L V_BCKP | 3V3 | Remains powered in deep sleep |
| ADXL362 CS | 33 | R10 pulls up to 3V3 |
| ADXL362 MISO | 34 | No longer a GPS power-control output |
| ADXL362 MOSI | 35 | Do not use the stock V3 LED GPIO35 |
| ADXL362 SCLK | 37 | Dedicated HSPI, 1 MHz, mode 0 |
| ADXL362 INT1 | 7 | Active-high deep-sleep wake |
| ADXL362 INT2 | 6 | Connected but unused by driver |
| ADXL362 VS and VDDIO | 3V3 | Always on, not VEXT |
| User/BOOT switch S2 | 0 | Active-low; also bootloader entry |
| Reset switch S1 | EN | Resets the MCU, not a GPIO |
| Buzzer gate | 4 | Held LOW/off |
| Vibration gate | 5 | Held LOW/off |
| CPU_LED | 40 | Held LOW/off; use Board::statusLed, not LED_BUILTIN |
| Native USB D− / D+ | 19 / 20 | J1 data path via protection circuit |
| UART0 RX / TX | 44 / 43 | J4 programming fallback |
| SX1262 CS/SCK/MOSI/MISO | 8 / 9 / 10 / 11 | Internal Heltec V3 radio bus |
| SX1262 RESET/BUSY/DIO1 | 12 / 13 / 14 | Internal module connections |

The radio connections follow the installed Wireless Shell V3 Arduino variant;
the custom sheet explicitly labels GPIO13/14 as LoRa, but does not draw
the radio internals. Verify the fitted module revision if radio init fails.
GPS PPS drives the schematic LED, not GPIO41. GPS RESET/STANDBY/FORCE_ON
are not controlled by this firmware. Do not connect external JTAG on
GPIO39–42 while GPS/CPU_LED are in use.

The upstream variant's `LED_BUILTIN`/`LED` aliases still point to GPIO35;
do not use them on this custom PCB because GPIO35 is ADXL362 MOSI. Also pass
explicit pins to peripheral initialization instead of relying on default I2C
pins. The current application does both and does not need a custom variant.

## Power and Diagnostics

The custom board does not have an OLED display. All debugging and runtime diagnostics
are transmitted over native USB Serial at 115200 baud.

Production enables VEXT (active-low via GPIO36) for GPS acquisition (L76L VCC).
Before entering deep sleep, the firmware stops UART, floats GPS pins, and drives GPIO36
high to switch off VEXT and conserve power. The ADXL362 sensor remains continuously
powered on the always-on 3.3 V rail. In simulated debug mode (`LORA_DEBUG = true`),
GNSS UART is not started and VEXT remains off.

## Build and native USB upload (recommended)

Install the PlatformIO IDE extension in VS Code and open this project folder.
Run these commands in a PlatformIO terminal (or use Build/Upload tasks):

```bash
pio run -e heltec_wifi_lora_32_V3
pio device list
pio run -e heltec_wifi_lora_32_V3 -t upload --upload-port /dev/cu.usbmodemXXXX
pio device monitor --port /dev/cu.usbmodemXXXX --baud 115200
```

Replace the placeholder with the actual port, e.g. `COM5` on Windows or
`/dev/ttyACM0` on Linux. USB may enumerate on a different port after flashing;
run `pio device list` again. If `pio` is unavailable in a normal macOS terminal,
use the PlatformIO terminal or `~/.platformio/penv/bin/pio`.

Use a USB data cable and the correctly wired J1 USB interface. In the sheet,
J1 pin 1 is VBUS, 2 is D−, 3 is D+, and 4 is GND; verify connector orientation
on the PCB before attaching a cable. The file named `JTAG.jpg` shows USB and
power circuitry, not an external four-wire JTAG programming header.

For a blank board, sleeping firmware, or a stuck upload:

1. Hold S2 (GPIO0/BOOT).
2. Press and release S1 (EN/reset) while still holding S2.
3. Release S2 and check the newly enumerated USB port.
4. Run Upload, then press S1 without holding S2 if the application does not start.

GPIO46 must not be driven high during download-mode entry. This follows
[Espressif's ESP32-S3 boot-mode instructions](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/advanced-topics/boot-mode-selection.html).
No Heltec license key is needed. The build uses native USB CDC for `Serial`.
Do not leave a production board waiting for Serial Monitor; firmware does
not wait for a host connection.

## UART0 fallback through J4

Use a **3.3 V logic** USB-to-UART adapter, never RS-232/5 V UART logic.

| J4 pin | Board signal | Adapter connection |
| --- | --- | --- |
| 1 | 3V3 | Leave disconnected when board has its own power |
| 2 | GPIO44 / UART0 RX | Adapter TX |
| 3 | GPIO43 / UART0 TX | Adapter RX |
| 4 | GND | Adapter GND |

Power the board normally, enter bootloader mode with S2/S1 as above, then
use the same Upload command with the adapter's serial port. This schematic
does not show a DTR/RTS auto-reset circuit: manual boot entry may be required.
Application logs still use native USB by default. For UART0 application logs,
change `ARDUINO_USB_CDC_ON_BOOT=1` to `0` in `platformio.ini`, rebuild and upload;
monitor the adapter at 115200 baud. GPS remains on separate UART1.

## LoRaWAN identity when replacing the board

Regular uploads on the same board must not erase NVS. **A new board does not
inherit the old board's OTAA nonce history or RTC session.** Provision a new
device identity for the new board, or perform an explicitly coordinated identity
migration that preserves nonce history. Do not run both boards with the same
active identity and do not permanently disable frame-counter validation.
Firmware does not automatically reset a corrupt session.

## Bench verification

Attach the correct LoRa antenna before transmitting. With debug enabled,
check `[ADXL362] Ready`, plausible acceleration at rest, and simulated uplinks
with 15-second deep sleeps between normal attempts (plus boot and radio time).
Check actual reception in ChirpStack; Low Power timing and debug overrides follow
the current README and source. Button holds and soft-off follow the README behavior.
Then set `LORA_DEBUG = false`, rebuild/upload, and verify outdoor GPS fixes,
VEXT voltage while awake/asleep, timer wake, S2 wake, and ADXL362 INT1 wake
with status 2. Use a padded fixture, never a person, for free-fall tests.
Check the motor remains off and the buzzer follows the README's audio policy,
including silence after shutdown. A successful build is not electrical validation.

Host regression checks (requires Node.js and a C++ compiler; build first to
install the pinned RadioLib headers):

```bash
node test/fall_detection.cjs
node test/session_recovery.cjs
node test/telemetry.cjs
```

The sensor test uses a register-level SPI mock: configuration readback,
latched wake events, pending-event retention, missing sensor, runtime faults,
and GPIO7 sleep arming. It does not exercise a physical SPI peripheral.

Board-profile reference: [PlatformIO Heltec V3](https://docs.platformio.org/en/latest/boards/espressif32/heltec_wifi_lora_32_V3.html).
