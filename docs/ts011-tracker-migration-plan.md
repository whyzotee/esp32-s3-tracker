# TS011 Tracker Migration Plan

Status: planned; implementation has not started.

## Summary

Add TS011-1.0.1 Relay TX support to the production tracker while preserving its
existing trip, GPS, fall detection, button and audio behavior. Use the local
[TS011-1.0.1 specification](ts011-1-0-1-relay.pdf) as the normative reference,
especially sections 3, 4, 7 and 10.2. Obtain regional radio parameters from the
matching RP002 revision; do not infer WOR or RXR parameters from the application's
uplink data rate.

Reuse the existing simulated tracker's pinned LoRa Basics Modem (LBM) integration.
Neither a successful build nor a direct-network ACK proves relay interoperability.

## Architecture and compatibility

- Keep RadioLib as the production default during development. Add a separate
  PlatformIO environment for LBM and compile exactly one radio backend per build.
- Reuse the simulator's LBM v4.9.0 pin
  (`a8ddc544043e72807cf7db532478e1dda734ae7c`), SX1262 HAL, build integration,
  buffered diagnostics and pre-transmission nonce reservation patch after review.
- Keep LoRaWAN 1.0.4 and Class A. Do not introduce Class C or require LoRaWAN 1.1.
- Preserve FPort 2 compact 9-byte telemetry, FPort 3 device information, FPort 10
  low-power commands and FPort 11 commands `01` through `07`.
- Preserve network-ACK feedback, repeating application-ACK fourth alarm note,
  button cancellation/I-am-OK, and shared remote SOS/Fall resolution (`07`).
- Keep application behavior independent of the backend. Never expose credentials
  in diagnostics or copy private simulator configuration into tracked files.

## Implementation stages

### 1. Separate the radio interface

- Move shared result types and backend interfaces out of the credentials header.
- Replace synchronous radio assumptions with explicit join, queued-transmission,
  completion and downlink events. Establish this boundary with a RadioLib adapter
  before adding LBM.
- Include transaction identity, network-ACK result, receive-window metadata and
  persistence failures in backend results. An unrelated downlink is not an ACK.
- Preserve event revision checks so completion of an older transaction cannot
  clear a newer SOS, Fall or cancellation report.

### 2. Integrate the LBM owner task

- Run the modem engine and modem API calls in one dedicated owner task; use
  bounded command/result queues. Report queue rejection explicitly and retain
  pending emergency events for retry.
- Keep GPS acquisition parallel. Process audio, web-debug and trip updates
  outside modem callbacks. Do not print logs or write NVS inside timing-critical
  WOR-to-uplink scheduling gaps.
- Let LBM handle WOR, WOR-ACK, RX1/RX2/RXR, relay synchronization, relay keys,
  WOR counters and EndDeviceConfReq/Ans.
- Distinguish relay synchronization, network ACK and application ACK in logs.
- Initially enable relay operation with direct fallback allowed; obey subsequent
  server activation and BackOff commands. Join WOR does not wait for WOR-ACK.
- Preserve report intervals subject to modem scheduling and regional airtime
  limits. Do not force transmission every 30 seconds when the stack forbids it.
- Review hardware entropy ownership against battery ADC before enabling real
  battery sampling. Verify peripheral ownership rather than blindly copying the
  simulator's entropy configuration.

### 3. Make storage and sleep safe

- Use a versioned LBM storage namespace. Never interpret RadioLib session blobs
  as LBM context.
- Migrate the existing DevNonce history conservatively before the first LBM
  join. Persist nonce reservation before WOR scheduling; fail closed on corrupt
  state, identity mismatch, exhausted counters or failed durable writes.
- Validate initial radio integration without timed deep sleep. This is a
  development stage only, not an acceptable production power-mode replacement.
- Before production deep sleep, implement and verify restoration of all required
  MAC/relay state, counters and timebase. Do not copy pointer-containing runtime
  objects directly into RTC memory.
- Invalidate relay synchronization after wake whenever its timing cannot be
  reconstructed safely. Do not reset WOR counters while reusing the same keys.
- Never sleep while a transaction or receive window remains pending. Preserve
  button/fall wake and existing shutdown/audio behavior.
- If safe restoration cannot be demonstrated, stop production rollout and record
  the blocker. Do not silently switch to permanent awake operation or rejoin on
  every timer wake.
- Power loss still requires fresh OTAA; this migration does not add persistent
  application Trip state or automatic application-side Trip recovery.

### 4. Provision and document

- Document ChirpStack's relay-capable tracker profile, relay association, matching
  regional settings and trusted-device/key updates after each fresh join.
- Keep the existing relay firmware unchanged unless interoperability testing
  identifies a specific defect. Relay-only configuration and forwarding commands
  are not application FPort 11 commands.
- Retain the production OTA partition layout. Do not copy the simulator's
  factory-only partition scheme; verify image size against both OTA slots.
- Update the README with backend selection, provisioning, ACK distinctions,
  diagnostics, measured limitations and rollback instructions.
- Do not remove the working RadioLib backend until LBM passes the release gates
  and replacement is explicitly approved.

## Verification and release gates

### Host and build checks

- Run all existing regression tests for trip, alarm, audio, payload and buttons.
- Test event ordering, duplicate completions, queue rejection, stale transaction
  results, ACK distinctions and all existing downlink commands through LBM's
  application adapter.
- Test nonce migration, interrupted writes, identity mismatch, counter
  exhaustion, reboot and power loss. Verify a rollback cannot reuse a nonce.
- Build both backend environments with web debugging enabled and disabled;
  production defaults must not change unintentionally.

### Hardware checks

- Establish direct OTAA, confirmed uplink and application downlink first.
- Verify relayed join, forwarded telemetry, forwarded application downlink and
  network ACK through RXR.
- Prove forwarding with correlated relay/server evidence and a controlled test
  without direct gateway coverage. WOR-ACK or network ACK alone is insufficient.
- Test relay absent, gateway outage/recovery, forwarding limits, fresh sessions,
  trusted-device updates and a relay restart without a tracker restart.
- Test SOS/Fall delivery, application ACK's repeating fourth note, button
  cancellation and remote resolution, including failed transmissions.
- Verify repeated sleep/wake, long-hold shutdown, OTA, GPS concurrency, battery
  readings, watchdog recovery and measured sleep current with web debugging off.
- Reject release if RX deadlines are missed, counters regress, pending emergency
  reports disappear incorrectly, or power behavior differs without documentation.

## Rollout and defaults

- Scope is tracker Relay TX, not another Relay RX implementation.
- Existing application wire formats and user-facing flow remain compatible.
- Keep LBM opt-in until every release gate passes on hardware.
- Switching backends requires fresh OTAA; cross-stack session continuity is not
  promised. Preserve the highest used DevNonce across both backends on migration
  and rollback; do not erase NVS as a routine migration step.
- No automatic flashing, credential changes, server mutations or commits.

## Documentation status

The root README is the current firmware behavior and wire-format reference.
HardwareV3.md is the wiring/upload reference. Legacy firmware descriptions,
historical MQTT examples and checklists, and unrelated application-backend
payload documentation have been removed to avoid conflicting instructions.
