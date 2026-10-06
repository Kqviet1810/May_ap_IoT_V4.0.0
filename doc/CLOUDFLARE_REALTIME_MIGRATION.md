> **V3 LEGACY:** This document describes the pre-V4 realtime stack. The V4 clean network baseline has no WebSocket/DeviceHub runtime and no MQTT implementation yet.

# Cloudflare realtime migration

## Audit and decisions

Baseline: `e285d1a712293844c7a6cdfc4a4ddd0027013d31` (V1.0.0). The baseline's 103 Node tests, sanitized host fault injection, ESP32/ATtiny builds and linked GPIO IRAM check passed in the cloud environment before this work.

The browser currently receives a shared HiveMQ credential from `account-worker.js`, after Google session/ownership checks in D1. `app.js` implements V2 grants, HMAC writes, per-tab sequence, boot fences, exact-payload retries, uncertain outcomes and signed device ACK verification. `realtime_link.h` performs independent HMAC/replay validation, then uses the existing HMI command/config/reminder queues. Terminal outcomes are cached on the ESP32. Network tasks only exchange bounded mailboxes with the control task. `device_identity.h` already creates a unique random 256-bit device key in NVS; provisioning sends a per-device command key back to the device. `cloud_alert_link.h` separately provides authenticated provisioning, heartbeat and Push. The Worker wrappers enforce provisioning limits and OTA authentication; account ownership is implemented in `account-auth.js` and `account-worker.js`.

The migration keeps those controller queues and cryptographic V2 domains. A domain string containing the historical word `mqtt` is a cryptographic protocol identifier, not a broker dependency; changing it would invalidate deployed grants/ACKs without adding protection. Recovery/boot enum slots retain their historical labels to preserve the tested state-machine ABI. The new runtime has no broker, MQTT subscriptions, retained writes, PubSubClient or MQTT.js.

One SQLite-backed `DeviceHub` per device accepts one authenticated ESP32 socket and at most eight browser sockets. The Worker authenticates device keys or short-lived browser tickets before entering the hub. The hub also checks live ownership/session before forwarding a control write. D1 remains a control-plane store; telemetry is exchanged over sockets and cached in bounded socket attachments, which survive hibernation. Commands remain signed and are acknowledged only by the ESP32. The hub never executes machine actions or treats transport forwarding as application success.

Workers Static Assets serves the existing UI with the same Worker/account origin. GitHub remains the source repository and CI/deployment authority. This uses GA Workers, SQLite Durable Objects, the WebSocket Hibernation API and Static Assets, with no Pub/Sub, VPS, R2 or beta dependency.

## Official references checked

Cloudflare documentation was read from its official `cloudflare/cloudflare-docs` production branch when the documentation site was blocked by the environment's network proxy:

- [WebSocket Hibernation](https://developers.cloudflare.com/durable-objects/best-practices/websockets/): `acceptWebSocket`, restored attachments, automatic protocol ping/pong, no timers while hibernating.
- [DO limits](https://developers.cloudflare.com/durable-objects/platform/limits/): SQLite DOs available on Free; account storage limit 5 GB; platform incoming WS cap 32 MiB. Our application cap is much smaller.
- [DO pricing](https://developers.cloudflare.com/durable-objects/platform/pricing/): message billing uses a 20:1 ratio; a quota exhaustion is an error, not an automatic reliability guarantee.
- [Static Assets](https://developers.cloudflare.com/workers/static-assets/): Worker and assets deployed together; asset-first routing with explicit Worker-first API/socket paths.
- [Workers Builds](https://developers.cloudflare.com/workers/ci-cd/builds/): GitHub integration is an available alternative to the repository's GitHub Actions deployment.
- ESP-IDF `esp_tls.h` bundled in the pinned Arduino ESP32 3.3.11: `esp_tls_conn_new_async`, `non_block`, verified CA/hostname, partial reads/writes and WANT_READ/WANT_WRITE. These APIs permit a bounded incremental transport pump on the existing dedicated network owner.

See [verified review findings and fixes](REALTIME_REVIEW_FIXES.md) for the follow-up transaction/concurrency/security hardening and exact revocation boundary.

## Cutover and evidence

Implementation, quota calculations, configuration, validation results and the hardware bench checklist are maintained below as the migration is validated. No OTA release or device rollout is part of this PR.

## Wire contract and lifecycle

Endpoints are `GET /realtime/device/MAP-…` and `GET /realtime/browser/MAP-…`.
The device sends its unique NVS key as an HTTPS upgrade bearer and its uint32
boot ID as `X-Mayap-Boot`. A browser first calls the account-authorized
`POST /api/device/realtime-session` with its per-tab client ID. Its signed ticket
expires in 60 seconds and is consumed exactly once in Durable Object storage.
The ticket is sent in `Sec-WebSocket-Protocol: mayap.v1, ticket.<ticket>`;
there is no credential in a URL or static configuration. The server selects
`mayap.v1`. Browser Origin must match `ALLOWED_ORIGIN` exactly.

Messages are UTF-8 JSON `{v:1,channel,payload}`. Channel names preserve the V2
application protocol: session, command, config/set, reminders/set,
history/request; device presence/bootstrap/snapshot/config/reported/
ack/log/history/reported. Writes retain the original signed
body, grant, grantSig and sig. Both Hub and ESP32 validate clientId, bootId,
sequence, nonce, grant expiry and HMAC; commands also have their original short
execution expiry. The Hub serializes checks per browser across async D1/crypto awaits; credits, visibility and device telemetry keep flowing independently. Generation/authorization fences guard the synchronous commit.
Its 16-entry request cache permits exact retries and refuses a different body
for one requestId. ESP32 terminal/in-flight/replay caches remain authoritative.

`forwarded` means only that the Hub sent a frame. Success remains a signed
ESP32 `completed` ACK. Lost connection/receipt or a timeout becomes UNCERTAIN;
a late verified terminal ACK can still settle it. Configuration succeeds only
after the existing controller/EEPROM readback. A transport error cannot invent
an application ACK. ACK and presence bypass the foreground telemetry filter.

Browser socket read authorization has an absolute five-minute lease, renewed
with another one-use ticket and a live session/ownership check. Revoked sessions
or ownership stop **writes immediately** through the D1 check on every write;
successful logout/session/member revocation also invalidates matching Hub sockets immediately. Arbitrary direct SQL edits have no push hook; their existing read access ends within five minutes unless paired with Hub invalidation. Viewer never receives a control
grant and cannot forward writes. Device key rotation closes the active device
socket; a live key-hash check on writes also fences a socket if invalidation fails.
Per-device command keys still derive from DEVICE_KEY_PEPPER as before. Changing
that pepper invalidates all device credentials/command keys: do not rotate it
as a routine cutover action.

The Hub holds no control state or durable telemetry history. Hibernation restores
healthy sockets from attachments, including boot, last-seen, sparse bootstrap,
lease, rate and receive-credit state. Exact replay fingerprints and sequence also persist in private DO SQLite across socket replacement. Device protocol Ping/Pong is automatic;
the browser's exact JSON ping/pong uses the hibernation auto-response API. There
are no interval timers in a Hub. Alarms bound stale device detection at 210 s
and browser read lease expiry. The existing 180 s HTTPS/Push offline threshold
and one-minute Cron remain unchanged. A disconnected device reports offline;
replacing a socket cannot let the old close event mark the replacement offline.

A visible viewer gets 1 Hz snapshots. A hidden tab can stay warm for five minutes
but sends `foreground:false`, ending telemetry streaming; controls remain subject
to the original freshness gate. Idle devices send one compact bootstrap at most
every 120 s, plus bounded state-change hints; heater PWM/temperature drift does
not create an idle stream. Active viewing refreshes the compact bootstrap at
120 s for new viewers. HTTPS heartbeats are reduced from 15 to 60 s and carry
only the existing sparse device/batch control-plane state; alarms still use the
existing independent HTTPS/Push path. D1 is never queried/written for snapshots.

Bounds: application frames 2 KiB; browser sockets 8/device; Hub attachments
4 KiB/socket (stricter than the **current official 16,384-byte limit**, not the
older 2 KiB platform assumption); forwarding pending entries 16/browser;
receive credit 16 events/browser; browser bufferedAmount admission 8 KiB;
rate 80 browser messages/10 s and 160 device application frames/10 s; ticket admissions 40/10 s, with indexed one-use nonce records instead of a 64-ticket ceiling. Async FIFO bounds: 8/browser and 16 admissions; credits/session updates bypass the FIFO.
Slow receivers close rather than accumulating unlimited outgoing frames.
ESP32 uses a fixed 8-frame TX ring, a 2 KiB message/fragment parser, async lwIP
DNS and async esp-tls with a 1 ms select budget and 15 s overall handshake
deadline. Each pump writes <=4096 bytes/4 calls and reads <=1024 bytes. Send
stall expires at 10 s, socket receive silence at 90 s. TLS CA validation and
hostname/SNI use the original host even though async DNS supplies a numeric IP.
ESP-IDF's official esp_tls_mbedtls.c confirms common_name reaches
mbedtls_ssl_set_hostname. All socket I/O remains on the existing Core 0 owner;
control/ISR receive only bounded mailboxes. Recovery/boot enum/task labels named
Mqtt are retained ABI labels with no broker implementation.

## Free-plan capacity and limits

Official pricing checked on 2026-10-02: DO Free 100,000 requests/day,
13,000 GB-s/day, 5 million storage rows read/day, 100,000 rows written/day and
5 GB storage. Incoming application WS messages use the 20:1 billing ratio;
outgoing messages and protocol pings are free. Application auto-response does
not add duration. Each setAlarm is a storage row written. Workers Free separately
limits dynamic requests to 100,000/day; Static Assets requests are asset-first.
D1 Free separately allows 5 million rows read/day, 100,000 written/day and 5 GB.
Limits are account-wide and exhaustion fails requests until quota resets.

A conservative worksheet for **30 devices, one viewer/device for one hour/day**:

| Dimension | Estimated daily use (before other account workloads) |
|---|---:|
| HTTPS device heartbeat Worker requests | 43,200 |
| Heartbeat D1 writes | ~43,200 (one device row/update) |
| Device telemetry application frames | ~129,600 |
| Browser receive-credit + 15 s session frames | ~116,000 |
| Metered application WS requests, /20 | ~12,300 |
| Device stale-check alarms | <=~21,600 |
| Ticket mint/renew and browser lease work | hundreds, plus reconnects |
| DO persistent writes, alarms + tickets | roughly 45,000–50,000 |

This is a sizing worksheet, not evidence of the customer's actual inventory or
Cloudflare account usage. Alarm/Push bursts, reconnect storms, indexes, other
Workers and other D1 workloads must be included. At 30 continuously watched
machines, 1 Hz telemetry + receive credit alone exceeds the DO Free request
quota; eight continuously watching browsers per machine also cannot fit Free.
Keep typical foreground viewing sparse and inspect Cloudflare usage. If the
actual workload is persistent dashboard viewing, reduce telemetry cadence with
the Web freshness threshold reviewed together, or revise capacity before
cutover. An operator must measure DO duration and D1 row metrics in a pilot;
local workerd and simulated weeks do not prove edge billing or WAN latency.
No paid broker or VPS is needed. Quota loss never changes autonomous machine
operation; remote controls become unavailable/uncertain and fail closed.

## Configuration and manual cutover

1. Keep this as a reviewed PR. No tag, GitHub Release, remote migration, OTA
   upload or production deploy was run by this task. V1.1.0 is the proposed code
   version; local HMI remains V1.0.0 and ATtiny protocol v4 is unchanged.
2. Verify Cloudflare account access and that the existing D1 `mayap_push` binding
   refers to the intended database. Back up that database before remote migration.
   Reuse existing account/provisioning/Push secrets; **do not replace peppers**.
   The new resource is one GA SQLite DO namespace `DEVICE_HUB`/`DeviceHub`, created
   by migration `device-hub-v1`. Static Assets is binding ASSETS. No R2/KV/beta
   resource is required. One-use tickets, an admission rate budget, expiring exact-retry records and alarms use private DO storage; telemetry is not persisted.
3. Cloudflare secrets: `DEVICE_KEY_PEPPER`, `MAYAP_SESSION_PEPPER`,
   `VAPID_PUBLIC_KEY`, `VAPID_PRIVATE_KEY`, `VAPID_SUBJECT`; optional
   `GITHUB_TOKEN` for authenticated release reads. Existing OTA verification
   public key/signing setup is unchanged. No fleet realtime secret is added.
   Remove obsolete MAYAP_MQTT_* secrets after cutover. Per-device keys already
   live in NVS and as peppered hashes in D1, never in firmware build/CI secrets.
4. Set GOOGLE_CLIENT_ID (existing GIS web client) and ALLOWED_ORIGIN to the final
   HTTPS Worker/custom-domain origin. Add that exact origin to Google Cloud's
   **Authorized JavaScript origins** and update its consent/test-user settings
   if necessary. No Google client secret is required. Web/config uses same origin.
5. Use an isolated pilot Worker + D1 for bench testing. Copy wrangler config to
   an operator-owned pilot config, set a different Worker name, D1 ID and allowed
   origin, and set its secrets separately. For an isolated bench build, change
   MAYAP_CLOUD_API_HOST only in that worktree's build_public.h and do not commit
   the bench override. Do not point the pilot at customer D1 or rotate real-device identities. Stage
   public assets with `python3 tools/build_web_assets.py`, then deploy using that
   explicit pilot config. This external resource creation was not possible here.
6. GitHub repository secrets: CLOUDFLARE_API_TOKEN (Workers Scripts Edit and D1
   Edit for this account), CLOUDFLARE_ACCOUNT_ID. Add zone Workers Routes Edit
   only if managing a custom-domain route. CI now installs pinned dependencies,
   tests actual local workerd/Chromium, builds both MCUs and checks linked ISR
   safety. Manual Worker deploy stays available. Automatic main deploy is gated
   by successful Build & release firmware at its exact SHA and repository
   variable **CLOUDFLARE_REALTIME_AUTODEPLOY=1**; enable only after commissioning
   and the coordinated cutover. It is disabled by default during migration.
7. After the bench passes, coordinate the Web/Worker origin switch with manual
   firmware commissioning. Old V1.0.0 devices speak MQTT and will not become
   WebSocket devices just because this Worker is deployed. A mixed unflashed
   fleet therefore loses remote realtime on the new UI. Autonomous local control
   continues. GitHub Pages is no longer the production host; disable its old
   publishing source or provide an intentional redirect after cutover. PWA cache,
   installed-app origin and browser Push permission do not transfer to a new
   origin: users log in, install PWA/enable Push there again. Do not run two
   automatic deploy sources (Actions and Cloudflare Builds) concurrently.
8. Roll back cloud code using a reviewed GitHub commit/deploy, and the bench via
   a wired known-good image. Never erase existing config/batch/EEPROM/NVS to test
   rollback. Old cloud/MQTT firmware and old Web must be restored together if
   reverting transport. Do not delete/recreate D1 or the DO migration history.

## Repeatable validation

Verified locally on 2026-10-02 against the proposed migration:

| Check | Result |
|---|---|
| Node account/protocol/transaction/realtime regressions | 149 passed; none skipped |
| Real workerd + native Chromium WebSocket | Passed, including credential rotation |
| Web connection/PWA and existing UX | Passed; 12 viewports, both themes |
| Sanitized PID, boot, recovery and nine runtime host targets | Passed |
| EEPROM power-cut/recovery fault injection | 827 cut points passed |
| Actual ESP transport accelerated reconnect test | 20,000 reconnects passed under ASan/UBSan |
| ESP32-S3 DEV and PROD compile; linked ISR IRAM/DRAM | Passed |
| ATtiny13A compile | Passed; 870 bytes flash, 2 bytes RAM |
| Release/protocol/reliability checks and diff formatting | Passed |

The GitHub workflows include these checks. Local results do not imply that a
remote GitHub Actions run, Cloudflare deployment or physical commissioning has
completed. Wi-Fi portal/radio recovery also cancels an in-progress WebSocket
handshake and releases TLS admission; fault injection covers this cancellation.

```bash
npx --yes pnpm@11.19.0 --dir cloudflare install --frozen-lockfile --ignore-scripts
node --test tests/*.test.cjs
python3 tools/check_release_sync.py
python3 tools/check_reliability.py
python3 tools/check_attiny_protocol.py
python3 tools/check_eeprom_history.py
python3 tools/test_runtime_buses.py --sanitize --check-regression
python3 tools/test_single_eeprom.py --sanitize
# Install Chromium once; MAYAP_CHROME can select an existing local Chromium.
(cd cloudflare && npx pnpm@11.19.0 exec playwright install --with-deps chromium)
python3 tools/test_realtime_workerd.py --browser
# Serve repo on localhost:8765 in another terminal for the UI suites:
node tools/test_web_connection.cjs /tmp/mayap-connection-qa
node tools/test_web_experience.cjs /tmp/mayap-web-qa
```

Node regression covers the original control/storage/account/transaction behavior
and new lease/ticket/replay/slow-consumer/hibernation/revocation tests. Broker-only
SUBACK/QoS tests now assert authenticated socket readiness, selected-device scope,
forwarding receipt and native liveness; transport tests run the actual new client
rather than reimplementing it in UI mocks. Source preservation fingerprints for
PID, safety/outputs, controller, storage, HMI, ATtiny, staged boot, recovery and
OTA stay unchanged. Only explicitly reviewed network config/protocol overhead
and Web transport entries changed. The retired MQTT wrapper fingerprint was
removed after real workerd and RFC6455/async TLS tests passed. Existing historical
UI QA assertions now respect the already-present account mobile header and hidden
landing; CSS and layout were not changed for this migration.

New tests include one simulated telemetry day with no D1 hot path, seven simulated
browser days, and 20,000 reconnects of the actual ESP transport under ASan/UBSan.
These are accelerated tests, not claims of physical 24/7 soak. Actual workerd
checks upgrade/auth/subprotocol tickets, one-use ticket replay, protocol Ping/Pong,
JSON auto-response, signed writes, exact retry, ACK routing, device replacement,
boot fencing and frame cap. Actual Chromium also connects directly to workerd,
signs via WebCrypto, receives a forwarding receipt, survives resume storms and
shuts down with no pending timers. Separate DOM/PWA suites cover controls,
cache-before-auth, uncertain ACK, local signing without HTTP per click,
background/idle leases, stale data and offline account gate.

## ESP32 commissioning checklist — still manual

- One isolated ESP32-S3 N8, 8 MB flash/default_8MB, PSRAM disabled; back up config,
  batch, NVS identity and EEPROM first. Use wired bench firmware, never fleet OTA.
- Confirm unique NVS key across two machines; valid CA/time/SNI handshake; invalid
  certificate, wrong host/key, expired ticket/grant and disabled ownership fail.
- Run a normal batch offline/online; compare PID/output waveforms, heater safety,
  sensor loss/high temperature, turning limits, alarm, RTC/batch restore, HMI,
  Tiny power-loss alarm and EEPROM readback with baseline. Network changes must
  not change control timing or ISR capture.
- Check light/manual turning/config/reminders/history: applied/rejected outcomes
  require signed terminal ACK; duplicate request executes once; replay/old boot/
  expired command cannot apply; lost ACK becomes uncertain and exact retry settles.
- Turn router/Internet off for 1 min, 10 min and hours; DNS failure, TLS stalled
  connect, Cloudflare maintenance/quota failure, slow uplink and dead sockets.
  The machine continues locally and restores one WS after bounded backoff.
- Suspend/resume Android/iOS/desktop, BFCache and background >5 min; multiple tabs,
  eight viewers and slow consumers; change selected machine and revoke sessions.
  Stale client/boot data cannot enable buttons or complete the wrong transaction.
- Physically trigger supported key rotation; old socket closes and cannot receive
  another command; reconnect succeeds with the new per-device key.
- Measure minimum free/largest heap, task stack high-water marks, reconnect leak,
  Core 1 control jitter and GPIO/Tiny timing under full config/history traffic,
  concurrent Cloud Push/HTTP and Wi-Fi portal recovery. Test 49-day clock wrap
  with the existing host cases plus bench diagnostics where practical.
- Soak at least 72 h with WAN interruptions and daily background/reconnect churn;
  check Cloudflare request/duration/storage/D1 quotas in the actual account.
  Verify real Push permissions/delivery and Google login on the final origin.

Remaining limits: no physical controller or edge account access in this workspace;
real WAN latency, mbedTLS handshake CPU/heap peaks, mobile OS socket policies,
72 h physical soak, Google/Push origin commissioning and actual Free-plan usage
remain operator validation. Cloudflare is a single remote-service dependency;
its outage removes remote realtime, while the existing autonomous controller,
local safety and HMI remain available.
