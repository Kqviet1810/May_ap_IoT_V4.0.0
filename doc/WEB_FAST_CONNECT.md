> **V3 LEGACY:** This document describes the pre-V4 realtime stack. The V4 clean network baseline has no WebSocket/DeviceHub runtime and no MQTT implementation yet.

> Lịch sử tối ưu kết nối MQTT ở baseline. Transport hiện hành dùng [Cloudflare WebSocket/DeviceHub](CLOUDFLARE_REALTIME_MIGRATION.md); cache, UX và transaction semantics được giữ.

# Web 12.1.10: cache first and stable MQTT lifecycle

Based on main `61e7c27d42d139a32750665040fed7036b4e8d3b`; branch `feat/web-fast-connect`.

## Audit and startup

Previously Web waited for `/api/device/mqtt-session` and key import, subscribed to seven output topics, then requested runtime, full config, reminders and log replay together. Device presence, broker reconnect and telemetry silence shared an offline label. MQTT packet liveness and eight independent browser leases already existed on main. Wi-Fi was actually forced to PERFORMANCE on main, rather than flapping between SAVE and PERFORMANCE.

Now the browser reads per-device `localStorage` key `mayap.web.v10.runtime.v1.<deviceId>` before connecting and renders the last runtime, outputs, faults and receive time synchronously. It persists only presence/runtime/bootId/revision/timestamps; no full config, pairing secrets or control keys are added to this cache. Writes coalesce at five seconds and flush on hidden/pagehide. A slower tab cannot overwrite a newer tab's cache. Removing a device removes its runtime cache. There is no journal or firmware storage change.

Broker credentials retain the existing `config.js` RAM-only handling. A fresh document normally needs one bounded provisioning HTTP request because credentials do not persist. Available RAM credentials are reused. WSS connects as soon as provisioning supplies credentials, while control key import finishes independently. The dashboard's first batched SUBSCRIBE is presence, bootstrap, snapshot and ACK; its runtime session sync does not request heavy reports. A returned tab renders from RAM immediately and reuses a healthy socket.

For an installed PWA, public same-origin HTML/JS/CSS prefer fresh network responses, falling back to this release's cached shell after 250ms when the network is slow. Background refresh remains alive so updates are still retrieved. `config.js` is part of the public shell, allowing its RAM-only credential/security wrapper to load offline. HTTP error pages never replace cached code; Cloudflare auth/API responses are never cached by this path. This timeout only controls shell delivery; no control/safety deadline changes.

Full config is requested upon opening Settings, opening Batch configuration, or focusing a quick parameter. Config inputs remain readonly until real config is available, preventing a default value becoming an accidental patch. Chart/history subscribes only when Batch is opened; reminders and event backlog are requested when their details are expanded. Explicitly requested missing reports use normal lease renewals for bounded-cadence retry. Log replay is limited to three session attempts per connection. Older firmware ignores scope and retains its original sync behavior; updated firmware retains full sync for older Web clients that omit scope.

## Retained bootstrap

Topic: `mayap/v1/<deviceId>/bootstrap`, retained QoS 0 firmware publish (Web subscribes QoS 1). It contains `v`, `proto`, `fw`, `bootId`, `revision`, `publishedAt`, `temperature`, `humidity`, `machineState`, `batchRunning`, `heaterOn`, `circulationFanOn`, `ventFanOn`, `humidifierOn`, `lightOn`, `sirenOn`, `humidifierInstalled`, `alarmMask`, `faultCode`, `faultCount`, `faultSeverity`. No full config/history or command grants.

Production serializer measures JSON plus topic/framing before sending and enforces a 512-byte total budget. The worst-value fixture is 455 bytes. Temperature changes are quantized to 0.1°C and humidity to 1% for change detection; outputs/state/revision/fault summary also trigger updates. Successful unchanged hints receive a 30-second heartbeat. Changed hints and failed sends are limited to one attempt per two seconds, coalesced into the existing snapshot service. Idle cadence remains six seconds; active full snapshots remain 400ms and unretained. Broker reconnection forces a fresh hint/runtime publication. No HTTP enters this path.

Bootstrap is always a hint, never live or a source of control boot identity. A retained full snapshot is also rejected as live. A bootstrap with older/unknown time cannot replace a newer browser cache, and no bootstrap can replace a full snapshot on the current connection. `publishedAt=0` means device time is unavailable; Web explicitly labels its age unknown. Full live snapshots establish the current bootId and supersede hints. A new boot invalidates old config; an old-boot config report cannot replace the new runtime.

## Connection and control states

| State | Web behavior |
| --- | --- |
| Cache / bootstrap | Last readings and faults, marked stored with timestamp; syncing; control disabled |
| Connecting broker | Connecting label; never invent device offline |
| Broker connected, no sample | Waiting for device |
| Live sample + online presence | Online; control enabled only with a valid RAM grant |
| Snapshot older than 8s | Degraded; presence/config packets cannot mask runtime staleness |
| Runtime older than 30s | Remains degraded; command UI disabled |
| Current-connection presence `online:false` | Device offline immediately |

Timestamp is compact under the existing device heading; tooltip and accessible label include full receive time/source. This preserves the mobile dashboard's available height. Stored readings cannot reconcile pending transactions or open recovery confirmation prompts.

Visibility enters WARM for **300 seconds (five minutes)** and flushes cache. VISIBLE renews the existing 45s lease every 3s; WARM renews every 15s, caps each lease to the time remaining and suppresses heavy report sync. At the deadline the page becomes IDLE, sends inactive and stops lease/grant renewal, while retaining a healthy MQTT socket. BFCache pagehide follows WARM; real unload/navigation sends inactive. A return evaluates wall and monotonic timestamps before VISIBLE activation, so throttled timers, backwards clock adjustments and 32-bit timestamp boundaries cannot restart the budget. pageshow/online/visibility are coalesced over 250ms. Internal navigation only adds requested subscriptions. SUBSCRIBE is single flight per device/connection and validates SUBACK. MQTT.js owns ordinary connection retry. Normal foreground broker silence beyond max(90s, twice keepalive) replaces a supposedly connected dead client. After background suspension, old receive time alone is insufficient: one bounded presence SUBSCRIBE probes broker liveness via SUBACK/PINGRESP/any incoming packet. A healthy broker reuses the socket even with an offline ESP32; only an unanswered probe replaces the silent socket. MQTT.js continues ordinary disconnected-socket retries itself. Probe deadlines delivered very late after another OS suspension recheck rather than declaring failure. ESP32 snapshot staleness alone never resets a healthy broker. Stale client callbacks cannot change the current connection.

Control grants are prefetched after live connection and renewed when at most 60s remain, during both VISIBLE and WARM, single flight with existing HTTP timeout/backoff. Five-minute grants normally need one renewal near the end of a full warm interval. IDLE does not maintain grants; resume immediately prepares an expired grant in the background and displays "Đang chuẩn bị quyền điều khiển…" while command buttons remain disabled. A valid grant and fresh live runtime allow immediate control. Valid grants are reused; keys remain in RAM. Missing/expired grants reject immediately on click instead of requesting HTTP. Bad/expired grants and denied auth do not enable control. Existing protected signing, anti-replay, identical-envelope retry, bounded transaction timeout, signed ACK verification and terminal reconciliation bodies retain their fingerprints.

Command path: UI -> local HMAC over bootId/expiry/clientId/sequence/nonce/body -> MQTT QoS 1 WSS -> HiveMQ -> ESP32 MQTT/TLS -> existing controller queue -> signed received/completed ACK -> browser HMAC verification and transaction handling. Neither `/sign-mqtt` nor `/mqtt-session` is awaited per click. Cloudflare continues pairing, session checks, proactive grants, push and OTA.

## Wi-Fi and task ownership

Eight existing client leases are ORed. While any lease is active, use PERFORMANCE. When the last lease ends, keep PERFORMANCE for 25 seconds; a returning lease resets this grace. Only then use SAVE. During WARM an active hidden browser keeps PERFORMANCE. After 300s, IDLE releases its lease and the existing 25s grace applies only after every browser lease expires. Lost inactive packets fall back to the bounded browser TTL, then grace. If the OS freezes JS/network completely, renewal cannot be guaranteed: the last lease expires after at most 45s (less near the warm deadline), then grace; resume restores active and requests stale runtime without fabricating live data. A new active lease restores PERFORMANCE on the next MQTT service iteration. No `delay`, I/O or timer is added to controlTask. Driver mode changes are applied only when needed and rechecked after radio reset; driver errors invalidate the cache and retry on later service iterations. Disconnected radio recovery remains awake.

The audited main already has a dedicated **mqttTask**, which owns `mayapWebLinkUpdate` and all MQTT I/O; networkTask owns STA/portal/NTP. This existing task split is preserved, and stale comments are corrected. PID, safety, turning, alarms, HMI, EEPROM, ATtiny and batch recovery are unchanged.

## Validation and latency targets

Run `node --test tests/*.test.cjs`, the three reliability/release/protocol scripts, and `python tools/test_runtime_buses.py --sanitize --check-regression` on Linux. The new runtime suite uses the actual production ArduinoJson serializer, session handler and timing policies. The full existing protected runtime/transaction regression remains enabled. CI builds ESP32-S3 and ATtiny. Browser QA: `MAYAP_PLAYWRIGHT=<package path> node tools/test_web_connection.cjs <out>` and existing `tools/test_web_experience.cjs`, using a localhost server and isolated transports only.

Targets, not measured hardware promises: cache render immediately; retained/socket resume under 500ms; live startup roughly 0.5–2s on a normal network; click-to-ESP32 roughly 100–500ms; terminal ACK usually under 1s. Fresh-page credential provisioning, network/TLS latency, broker delivery, phone suspension and DTIM may add latency. Existing safety/expiry/transaction deadlines are preserved. Mock browser timing is not radio or Internet timing.

Still verify on real Android/iOS/PC with deployed new Web + firmware: cold open vs tab return, BFCache/socket suspension, broker/device disconnect independently, poor Wi-Fi/mobile network, background at 30/120/179/180/299/300s and beyond, several tabs/browsers, SAVE wakeup latency and signed command/ACK timing. No production machine commands, OTA, deployment or merge are part of these automated tests.
