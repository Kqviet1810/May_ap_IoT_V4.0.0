> **V3 LEGACY:** This document describes the pre-V4 realtime stack. The V4 clean network baseline has no WebSocket/DeviceHub runtime and no MQTT implementation yet.

# Online isolation from local control

Online is auxiliary. Handled Wi-Fi/DNS/TLS/WebSocket/HTTPS/service failures never call the controller restart API. `networkTask` is the sole Wi-Fi driver/STA/AP/reconnect/power-policy owner; realtime presence/diagnostics use cached IP/RSSI snapshots. Arduino auto-reconnect is disabled so it cannot silently bypass the transaction.

The shared `RadioGate` closes admission atomically. Realtime, Cloud and OTA finish their admitted operation, close resident sockets/listeners and acknowledge closure. Radio changes require all admitted owners' ACKs and no active owner. Busy=0 alone is insufficient. A stuck owner keeps radio mutation blocked; other Online services park and the controller continues. Tasks are not forcibly deleted while holding lwIP/heap/flash locks.

Portal and STA startup/recovery share this gate. Portal success hands over the already initialized STA. Failure/cancel/timeout does not reopen admission before radio cleanup. Failed portal UI is not an exclusive-radio request. An ongoing OTA upload withholds its drain ACK; a verified successful OTA can still reboot intentionally.

The portal owner also closes its own HTTP/DNS listeners before disabling AP, including the successful Wi-Fi-test path. Fault injection rejects the previous AP-first order as well as idle-only closure checks.

Heap critical/sustained-pressure warnings pause Online rather than requesting a controller restart. A 16 KiB instantaneous reserve triggers the same pause; reopening requires 72 KiB free and a 24 KiB allocation block. Memory-pressure flags are nonblocking atomics on the control path. Existing heap fault thresholds remain diagnostic. Optional task-create failure degrades its service; Wi-Fi/realtime initialization readiness has a finite boot-stage timeout, and Cloud/OTA admission cannot hold boot forever.

HMI distinguishes lost/weak Wi-Fi and Wi-Fi connected but realtime unavailable, with bounded notices and recovery messages. PID, output arbitration, sensor, turning, RTC/EEPROM and local watchdog trip behavior remain unchanged.

## Validation and limits

Actual production gate, service policy, radio recovery, portal state machine, heap handler and WebSocket transport are extracted/compiled in host fault injection. Tests cover active HTTPS during quiesce, DNS/TLS/open-socket loss, delayed drain ACK, successful/failed/cancelled/timed-out portal, immediate reconnect after portal exit, 12-hour virtual outage and 24-hour virtual owner stalls. Breaking the closure-ACK check makes the regression fail. This is source/state-machine evidence, not a wall-clock ESP32 deadline measurement.

The protected controlTask remains on core 1 at priority 5, at the existing 5 ms period. It neither enters nor waits for the Online gate. HMI now runs on core 1 at priority 2, below control and supervisor, so a core-0 network-task stall does not also starve the local display/input heartbeat. All Online tasks remain on core 0. Full local thermal/output, buses, watchdog and transaction regressions remain required.

## Remaining hardware limits

No software can guarantee availability through arbitrary ESP-IDF driver/lwIP panic, heap corruption, interrupts disabled by a faulty driver, brownout, or a real local control/WDT failure. These existing safety resets remain intentional. Generic NVS/flash writes (including saving Wi-Fi credentials/provisioning) and intentional OTA share ESP32 flash hardware and can briefly stall flash-backed code; host tests cannot certify those physical latencies. Hardware commissioning must record control-cycle maximum/heartbeat while disconnecting Wi-Fi during TLS and HTTPS, submitting correct/wrong portal credentials, flapping the router and leaving WAN unavailable for hours. No safety threshold is relaxed to conceal a deadline failure.

## Changed files

- Firmware: `MAYAP_INDUSTRIAL_v1_0_0.ino`, `online_isolation.h`, `service_recovery.h`, `network_service.h`, `transaction_bridge.h` (cached IP/RSSI only), `machine_control.h` (heap health only), `hmi.h` (bounded Online notices only).
- Regression: `tests/runtime-network.cpp`, `tests/runtime-online-isolation.cpp`, `tests/runtime-websocket.cpp`, `tests/runtime-recovery.test.cjs`, `tests/runtime-preservation.json` (four narrowly reviewed fingerprints).
- Tooling/documentation: `tools/test_runtime_buses.py`, `tools/check_reliability.py`, this document.

The existing thermal simulator reports 0/1536 acceptance-target passes, although its hard regression checks pass. Those existing thermal performance limits are outside this Online-only patch; thermal algorithms and acceptance thresholds have not been modified.
