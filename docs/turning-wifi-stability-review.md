> **V3 LEGACY:** This document describes the pre-V4 realtime stack. The V4 clean network baseline has no WebSocket/DeviceHub runtime and no MQTT implementation yet.

# Turning / Wi-Fi stability review

Branch: `codex/turning-wifi-stability`; base: `main` `412be80` (2026-10-05).
No merge, deployment, OTA release or physical flashing performed.

## Turning

Latest main already contains `3137f60`: homing projects the schedule from the
persisted anchor, manual reanchor saves immediately, and interval/enable edits
reproject instead of starting a fresh interval. Those fixes are retained.

One remaining production defect was reproduced: with a persisted epoch and an
invalid RTC, `scheduleNextTurnFromAnchor()` intentionally leaves `nextTurnAt_ = 0`
while waiting. `updateTurning()` nevertheless considered zero an expired deadline
and started an automatic movement. It now returns until a real deadline exists.
The existing bounded RTC fallback is retained; unknowable outage duration cannot
be reconstructed without a working RTC. No EEPROM schema/address changes.

Production-method HAL tests cover remaining/overdue power loss, WDT/reboot millis
origins, mid-travel homing, Manual -> Auto with/without movement, manual reanchor
OFF/ON and persistence, interval shortening/lengthening, RTC invalid/recovery and
bounded fallback, homing, millis wrap, sensor/high/emergency/turn inhibit and
last-three-day lockdown. Motor/limit safety implementations remain unchanged.

Before fix: invalid-RTC test failed (motor deadline admitted while waiting).
After fix: tests PASS. Temporary mutations of historical homing reanchor and the
zero-deadline guard each make the tests fail, without modifying production files.

## Wi-Fi

Previously every driver disconnect sample immediately became the UI/fault
snapshot; foreground/background browser activity also selected modem sleep.

* Stable presentation: continuous loss for 4000 ms before offline; reconnect
  requires at least 750 ms and three fresh associated samples. Wrap-safe.
* Raw I/O availability: immediate; WebSocket/HTTPS/OTA use this accessor.
* Actual STA association is sampled by networkTask, including during owner drain
  and isolation. RadioGate closure or Internet/WebSocket failure does not itself
  mean Wi-Fi loss. Stable state never admits network I/O.
* Explicit OFFLINE/unconfigured publishes immediately. Existing owner-drained
  radio stop, reconnect, backoff, portal and deep-recovery transaction retained.
* **[Cập nhật 2026-10-08: xem doc/NETWORK_MQTT_PRIMARY.md — PERFORMANCE cố định chỉ còn trong 15 phút đầu và khi còn dùng Web; sau 15 phút không dùng Web mới modem sleep, một quy tắc theo thời gian do networkTask áp dụng.]**
* Fixed `WIFI_PS_NONE`, applied exclusively by networkTask and reapplied after
  association/recovery. The realtime power mailbox and SAVE/PERFORMANCE policy
  are removed. Failed driver application retries without log flooding.
* Edge logs: `[WIFI-RAW] associated/available/state`, `[WIFI-STABLE]`,
  `[WIFI-RECOVERY]`. No credentials logged.

Before fix: sub-second driver flap test failed. A later integration test also
caught Online owner drain being incorrectly classified as Wi-Fi loss.
After fix: both PASS. Mutation tests reject both regressions.

Fault injection includes sub-second drops, four-second sustained loss, router
reboot/one positive sample, 1000 reconnect cycles, eight-hour loss, Internet-only
failure, owner-drain with STA still associated, skipped station service, immediate
actual WebSocket admission/closure during UI grace, millis wrap and fixed power
application/retry. Existing real RadioGate tests additionally cover 30,000
admission races, busy TLS/socket owner closure and 24-hour owner stalls.

## Verification

| Check | Result |
| --- | --- |
| New production turning + Wi-Fi tests, ASan/UBSan and mutation proofs | PASS |
| Node account/protocol/security/push/Web tests | 183/183 PASS |
| Runtime bus/network/OTA/Tiny/cloud/WebSocket/transactions/isolation + regression proofs, ASan/UBSan | PASS |
| AT24C512 page/readback/write-cycle/suspension/power-cut regression, ASan/UBSan | PASS |
| Runtime recovery, boot policy, existing epoch schedule tests, ASan/UBSan | PASS |
| Release synchronization, ATtiny protocol, static reliability and protected fingerprints | PASS |
| Real workerd + native Chromium | PASS |
| Browser connection, service worker, themes, mobile/layout/notes UX | PASS |
| Full thermal regression, ASan/UBSan | PASS (test command exit 0; performance limits below) |
| Real ESP32-S3 Arduino 3.3.11 build, no PSRAM, linked IRAM/DRAM check | PASS |

Final firmware: 1,396,521 bytes flash (41%); globals 176,248 bytes (53%).
No PID/Adaptive/PDM/sensor/thermal safety/ATtiny/transaction/Cloud API changes.
Protected manifest changes are limited to the reviewed turning guard, one raw
status declaration and one OTA admission accessor; no blanket refresh.

Thermal performance qualification remains incomplete on main: control-only matrix
0/1536 target PASS, 1536 target FAIL; actual Phase 1 heating route 186/788 target
PASS, 602 target FAIL, High=0, Emergency=0. Existing Adaptive comparison reports
High 10->9 and Emergency 8->8. These results are not qualifications of this patch
or physical accuracy claims; thermal code and acceptance thresholds were retained.

## Changed files

Firmware: `machine_control.h`, `network_service.h`, new `wifi_stable_state.h`,
`config.h` (declaration only), `MAYAP_INDUSTRIAL_v1_0_0.ino`, `transaction_bridge.h`,
`web_realtime_policy.h`, `cloud_alert_link.h`, `ota_update.h`, `ota_web_update.h`.
The Cloud/OTA files only switch network I/O admission to the raw snapshot.

Tests: `runtime-turning.cpp`, `runtime-wifi-state.cpp`, `runtime-ota.cpp`,
`runtime-cloud-alert.cpp`, `runtime-stability.cpp`, `runtime-web-connect.cpp`,
`runtime-preservation.json`. Tools: `test_turning_scheduler.py`,
`test_wifi_state.py`, `test_runtime_buses.py`, `check_reliability.py`.
CI: `.github/workflows/reliability-checks.yml`; this review document.

## Remaining risks / review before main merge

* Host fault injection cannot prove ESP32 driver/radio timing, real control
  deadlines, motor travel, switch wiring or EEPROM behavior under physical power
  loss. Bench-test a running batch, mid-travel power cut, overdue restart,
  Manual -> Auto and interval change with a valid RTC.
* With an invalid RTC, elapsed power-off time is unknowable; the retained bounded
  fallback is not equivalent to an accurate epoch-based remaining-time estimate.
* Stable UI deliberately keeps Wi-Fi connected during the four-second grace;
  commands still lose admission immediately. Internet/Online unavailable is a
  separate status, not a reason to reboot or declare STA disconnected.
* Check weak Wi-Fi, router reboot, wrong portal password/success/cancel, Internet
  outage during TLS/HTTPS and repeated reconnect on the physical unit. Confirm
  uninterrupted PID/turning/HMI and no reset, plus OFFLINE radio shutdown.
* Fixed awake Wi-Fi can draw more current than modem sleep. Verify supply margin
  on the installed board; network subsystem isolation cannot prevent a physical
  brownout from an inadequate supply.
* Existing thermal simulation target failures above remain outside this scope.
