# MAYAP V4 clean network baseline

This repository starts at the V3 `main` commit `52f10af1722c453f5cf3270f5387b74d2a0f6c32`. The cleanup branch removes the custom WebSocket and DeviceHub runtime. This is Phase 1 only: there is no MQTT client or broker connection yet.

| Component | Baseline version |
| --- | --- |
| Release / baseline mã nguồn | 1.1.4 |
| ESP32 firmware | 1.1.4 |
| HMI firmware | 1.0.0 |
| Web PWA | 1.1.9 |

## Current architecture

- The ESP32-S3 controls PID, heater, thermal safety, turning, batch, HMI, RTC, EEPROM/history, ATtiny, alarms and recovery locally. Those sources are retained from the baseline.
- The reserved `mqttTask` reports offline and performs no network I/O. `transaction_bridge.h` retains bounded runtime/config mailboxes, signed V2 application validation, replay fences, terminal result cache, command/config completion handoff, history and event log. No publisher is attached in this phase.
- Cloud HTTPS APIs retain provisioning, OTA, heartbeat, alarms and Push. The Worker retains Google accounts, sessions, ownership/roles, claims, notes and reminders. Existing D1 tables remain intact. `telemetry_history` is legacy storage for read compatibility.
- The Web PWA retains UI, account features and Transaction V2 semantics. Remote controls remain disabled while the new transport is absent; no browser realtime connection is attempted.

## Validation

The CI build targets ESP32-S3 N8 with PSRAM disabled, and runs host, Web and Worker tests. `tools/build_web_assets.py` stages public assets. Hardware, actual Cloud connectivity and live device command delivery need a later phase.

## Protocol documentation

The transport-neutral application contract is documented in [doc/TRANSACTION_V2_SPEC.md](doc/TRANSACTION_V2_SPEC.md). The V4 source and this README define the current baseline.
