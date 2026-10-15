# Changelog

All notable firmware changes are documented here.

## [Unreleased]

### Changed

- Accept GNSS locations only after three distinct GGA epochs with at least six
  satellites, HDOP <= 2.00, fresh data and no implausible jump. Retain cached
  coordinates on timeout without setting the fresh-GPS flag; urgent events
  interrupt acquisition and continue to use cached coordinates.
- Pin pioarduino 55.03.312-1 (Arduino-ESP32 3.3.12 / ESP-IDF 5.5.5)
  and RadioLib 7.8.1; require PlatformIO Core 6.2.0 or newer.

### Fixed

- Use the framework's ESP32-S3 IPC stack fix (2048 bytes instead of 1024)
  to address the suspected cause of the intermittent `ipc1` stack-canary panic.
  Reject builds with a framework IPC stack smaller than 2048 bytes.

## [1.2] - 2026-09-25

### Added

- Distinct GPIO4 buzzer melodies for power on, power off, SOS, suspected fall,
  OTAA Join progress, successful LoRaWAN session setup, and local Wi-Fi OTA/AP mode.

### Changed

- Removed the temporary VEXT deep-sleep override; VEXT is always disabled during deep sleep.

## [1.1] - 2026-09-25

### Added

- Battery voltage in millivolts and estimated battery percentage in the
  LoRaWAN uplink payload.

### Changed

- Uplink payload is 15 bytes. Application decoders must use the updated codec.
