> V3 LEGACY ARCHIVE — historical architecture only. Do not use for the V4 runtime.

# MAYAP — Máy ấp trứng thông minh

> **Baseline release candidate hiện tại: 1.1.2.** Kiến trúc realtime hiện tại là **Cloudflare WebSocket + SQLite Durable Objects (`DeviceHub`)**. MQTT broker/HiveMQ/EMQX và credential realtime dùng chung toàn fleet không còn nằm trong runtime hiện hành.
>
> `release-manifest.json` là nguồn phiên bản phát hành. Việc manifest ghi `1.1.2` **không đồng nghĩa firmware đã được OTA/phát hành tới mọi máy**; rollout vẫn phải đi qua build, ký số, commissioning và xác nhận tại HMI.

## Phiên bản hiện hành trên `main`

| Thành phần | Phiên bản |
|---|---:|
| Release / baseline mã nguồn | 1.1.2 |
| ESP32 firmware | 1.1.2 |
| HMI firmware | 1.0.0 |
| Web PWA | 1.1.6 |
| Web storage schema | 10 |
| ATtiny protocol | 4 |
| ESP32 Arduino core CI | 3.3.11 |
| Arduino CLI CI | 1.5.1 |
| Node CI | 24 |

Các giá trị này được khai báo tại `release-manifest.json` và được CI đối chiếu bằng `tools/check_release_sync.py`. Không sửa riêng một version mà không cập nhật manifest/checker tương ứng.

## Source-of-truth

Khi tài liệu cũ mâu thuẫn với code hiện hành, dùng thứ tự ưu tiên sau:

1. **Code đang chạy + `release-manifest.json`**: `MAYAP_INDUSTRIAL_v1_0_0/`, `app.js`, `realtime_transport.js`, `cloudflare/src/`, workflow CI/deploy.
2. **Tài liệu migration/review hiện hành**: `doc/CLOUDFLARE_REALTIME_MIGRATION.md`, `doc/REALTIME_REVIEW_FIXES.md`, `doc/ADAPTIVE_STAGED_BOOT.md`, `doc/RUNTIME_SELF_RECOVERY.md`.
3. Các tài liệu tên `V3_8_x` được giữ để truy vết lịch sử/hardening. Chúng **không còn là nguồn chuẩn cho realtime** nếu vẫn mô tả MQTT/HiveMQ.

Mốc audit phát hành 1.0.0 vẫn được lưu tại `doc/RELEASE_1_0_0.md`.

## Kiến trúc hiện hành

```text
Web PWA / Workers Static Assets
          |
          | WSS (ticket theo phiên)
          v
Cloudflare Worker ---- D1 account / ownership / Ghi chú / Nhắc nhở / Push / OTA metadata
          |
          v
SQLite Durable Object: DeviceHub (1 hub / 1 máy)
          ^                         ^
          | WSS                     | WSS
          |                         |
       Browser                   ESP32-S3
                                    |
                                    +-- PID / heater / safety / turning / batch
                                    +-- HMI / RTC / AT24C512 / recovery
                                    +-- HTTPS heartbeat + alarm/Push
                                    |
                                    +-- pulse-width protocol v4 --> ATtiny13A
```

### Nguyên tắc điều khiển

- **ESP32 là controller duy nhất.** Cloudflare/DeviceHub chỉ xác thực, định tuyến và giới hạn phiên; không điều khiển heater hay state machine.
- Mất Internet/Cloudflare/Web không làm mất điều khiển cục bộ: PID, heater safety, đảo trứng, batch, HMI, alarm và recovery vẫn chạy trên ESP32.
- `DeviceHub` gửi `forwarded` chỉ có nghĩa frame đã được chuyển tới socket thiết bị. **Chỉ terminal ACK đã xác minh từ ESP32 mới là kết quả thao tác.**
- Command/config/history giữ transaction V2 với `bootId`, `clientId`, sequence, nonce, expiry, HMAC, replay protection và exact retry.
- TLS verification là bắt buộc; production không được dùng `setInsecure()`.

### Tên legacy còn tồn tại

Một số tên như `mqttTask`, `MayapRecovery::Service::Mqtt` hoặc crypto domain `mayap-mqtt-write:v2` vẫn được giữ để bảo toàn ABI/state-machine/transaction đã kiểm thử. **Đó không phải bằng chứng runtime còn MQTT broker.** `mqttTask` hiện bơm native WebSocket qua `realtime_link.h` + `websocket_transport.h`.

## Phân miền thực thi ESP32

Staged boot đi theo thứ tự: **Safe outputs → Storage → Sensor/Machine → HMI → Control safety → Local settle → Wi-Fi → Realtime → Cloud → OTA → Running**.

| Task | Core | Priority | Vai trò |
|---|---:|---:|---|
| `controlTask` | 1 | 5 | State machine, PID, safety, turning, outputs |
| `supervisorTask` | 1 | 6 | Heartbeat/deadline, latch trip, safe restart |
| `hmiTask` | 0 | 2 | LCD ST7567S, encoder, HMI transaction |
| `networkTask` | 0 | 1 | Wi-Fi, portal, radio recovery |
| `mqttTask` *(legacy name)* | 0 | 2 | Native WebSocket realtime |
| `cloudTask` | 0 | 1 | HTTPS heartbeat, provisioning, alarm/Push |
| `otaTask` | 0 | 1 | ArduinoOTA, Internet OTA, rollback service |

Network/HTTPS/OTA không chạy trong `controlTask`. Task và stack được tạo tĩnh; watchdog/supervisor giám sát miền điều khiển độc lập với mạng.

## Device identity và tài khoản

Luồng người dùng chuẩn:

1. ESP32 tạo Device ID `MAP-XXXXXXXXXXXX` từ eFuse MAC.
2. Mỗi máy tạo **device key 256-bit riêng** và lưu trong NVS; không dùng fleet realtime secret.
3. Máy provision/heartbeat với Worker qua HTTPS.
4. Người dùng đăng nhập Web bằng Google và claim máy lần đầu bằng **Device ID + PIN**.
5. Worker kiểm ownership/role trước khi cấp ticket WebSocket và control grant.
6. Browser mở `GET /realtime/browser/<device>`; ESP32 mở `GET /realtime/device/<device>`.

Nếu `REQUIRE_DEVICE_INVENTORY=0`, reliability layer có thể auto-admit máy mới theo rate-limit. `=1` bật lại factory allowlist nghiêm ngặt.

## Lưu trữ cục bộ

Firmware hiện dùng **AT24C512 64 KiB tại I2C `0x50`** làm EEPROM ngoài bắt buộc:

- Config / Batch / Reminders dùng các vùng A/B cố định để giữ tương thích dữ liệu.
- Lịch sử nhiệt: **7 ngày, 5 phút/mẫu**, vùng `0x1000..0x2F7F`.
- Vùng `0x3000..0xEFFF` hiện **không được firmware sử dụng**. Backend Ghi chú cũ đã bị loại bỏ hoàn toàn; chỉ còn giao diện Web.
- Driver ghi EEPROM dùng ACK polling có timeout và retry; regression hiện kiểm cả write-cycle thực, task wake-up trễ, millis wrap và power-cut/readback.

Commit `274b0d28` đã sửa false-timeout khi task tỉnh muộn: sau khi scheduler trì hoãn, driver phải probe EEPROM thêm lần nữa trước khi kết luận hết thời gian.

## Safety

Firmware có fault manager tập trung với severity `Info / Warning / Stop / Emergency`, output arbiter và heater inhibit. Tuy nhiên software chỉ là một lớp.

Phần cứng heater hiện tại: **GPIO1 điều khiển đồng thời cả hai SSR**, phần cứng hiện có 8 thanh nhiệt, 4 thanh mỗi bên quạt, tổng 16 kW. Firmware chỉ có **một bank 16 kW** (GPIO1 OFF = 0 kW, ON = 16 kW); công suất PID 0–100% là công suất trung bình của toàn bank. Không có GPIO điều khiển riêng SSR thứ hai. Xem [báo cáo thermal V2](audit/THERMAL_CONTROL_V2.md) trước khi thử nghiệm trên máy thật. Quantum 300 ms là candidate commissioning, chưa được xác nhận trên máy thật. PID 18/0.8/45, beta=1 chưa được đặc trưng trên buồng 12 m³. Mô phỏng chỉ so sánh control-only; vượt High/Emergency được báo riêng và không chứng minh độ chính xác thực tế. `pidCycleSec` chỉ còn là dữ liệu legacy, đã ẩn khỏi Web/HMI.

Chuỗi an toàn phần cứng yêu cầu:

```text
thermal fuse -> thermostat/thermal relay độc lập -> safety contactor -> SSR -> heater
```

Thermostat/thermal relay phải có khả năng cắt coil contactor trực tiếp, không phụ thuộc ESP32. Xem `doc/SAFETY_HARDWARE_REQUIREMENTS.md`.

## OTA

Có hai đường OTA độc lập:

- **ArduinoOTA LAN**: chỉ bật khi cấu hình `MAYAP_OTA_PASSWORD`. Điểm nhập duy nhất khi build bằng Arduino IDE là `MAYAP_INDUSTRIAL_v1_0_0/build_public.h`; để rỗng sẽ tắt OTA.
- **Internet OTA**: GitHub Release → Cloudflare Worker → ESP32. Firmware kiểm kích thước/thời gian, SHA-256 và chữ ký ECDSA trước khi flash.

Web không được tự flash máy. Operator phải xác nhận trực tiếp tại HMI; rollback cũng là thao tác local.

## Cấu trúc repo

```text
MAYAP_INDUSTRIAL_v1_0_0/   ESP32 firmware 1.1.2
ATTINY13A_POWER_ALARM/     firmware ATtiny13A protocol v4
cloudflare/                Worker + D1 + DeviceHub + Static Assets config
.github/workflows/         build / test / release / deploy
app.js                     Web application
realtime_transport.js      bounded native browser WebSocket client
protocol_v2.js             transaction/ACK protocol helpers
notes.js / notes.css       giao diện Ghi chú; dữ liệu bền vững lưu trực tiếp D1 qua API tài khoản
release-manifest.json      manifest version/toolchain
tests/                     host/browser/runtime regressions
tools/                     checker, web asset builder, QA/integration
doc/                       architecture, migration, commissioning, safety
audit/                     audit lịch sử và simulated evidence
vendor/                    browser dependency được vendored có chủ ý
```

Tên thư mục sketch `MAYAP_INDUSTRIAL_v1_0_0` là tên lịch sử của sketch. **Version runtime phải đọc từ `MAYAP_FIRMWARE_VERSION`/manifest**, không suy ra từ tên thư mục.

## Build ESP32

Board production: **ESP32-S3-WROOM-1U-N8**, flash thật 8 MB, không PSRAM.

Thiết lập bắt buộc:

- Flash Size: `8M`
- Partition Scheme: `default_8MB` / “8M with spiffs”
- PSRAM: disabled
- Không dùng `huge_app` vì không có dual OTA phù hợp dự án.

FQBN của CI:

```text
esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=default_8MB,FlashSize=8M,PSRAM=disabled
```

Build profile:

- **DEV**: diagnostic Serial ON, input simulation OFF.
- **PILOT**: `workflow_dispatch`, diagnostic Serial ON, input simulation OFF.
- **PROD**: tag `vX.Y.Z`, diagnostic Serial OFF, input simulation OFF.

Tag release phải khớp chính xác `MAYAP_FIRMWARE_VERSION`.

## CI / regression gates

`Build & release firmware` hiện kiểm tối thiểu:

1. version/tag và release manifest;
2. cấm `setInsecure()` và private signing key trong firmware;
3. contract ESP32 ↔ ATtiny;
4. reliability checker;
5. Node transaction/account/Web/DeviceHub regressions;
6. real local Cloudflare `workerd` + Chromium realtime test;
7. Web connection/UX QA;
8. PID/autotune, staged boot, runtime recovery, buses và EEPROM driver regressions;
9. compile ATtiny13A với trần 1 KiB flash / 64 B static RAM;
10. compile ESP32-S3 và kiểm linked GPIO ISR cache safety;
11. khi build tag: ký ECDSA và tạo GitHub Release.

Cloudflare dependencies được khóa bằng `cloudflare/pnpm-lock.yaml`; workflow cài bằng **pnpm 11.19.0 + `--frozen-lockfile`**.

## Cloudflare hiện hành

`cloudflare/wrangler.toml` hiện cấu hình:

- Worker: `mayap-push-worker`
- Entrypoint: `src/account-worker.js`
- Compatibility date: **`2026-10-01`**
- Flag: `nodejs_compat`
- D1 binding: `DB` → `mayap_push`
- Durable Object binding: `DEVICE_HUB` → `DeviceHub`
- Static Assets binding: `ASSETS`
- Worker-first routes: `/api/*`, `/realtime/*`
- Cron: `* * * * *`

`account-worker.js` là cửa vào account/realtime. Các endpoint vật lý được chuyển qua `reliability-wrapper.js -> security-wrapper.js -> index.js` để giữ provisioning, heartbeat, alarm/Push và OTA control-plane hiện có.

Deploy chuẩn: `.github/workflows/deploy-cloudflare-worker.yml`. Production auto-deploy chỉ chạy khi repository variable `CLOUDFLARE_REALTIME_AUTODEPLOY=1`; mặc định migration/commissioning không được coi là rollout tự động.

Chi tiết Cloudflare: `cloudflare/README.md`.

## Tài liệu nên đọc

- `doc/CLOUDFLARE_REALTIME_MIGRATION.md` — kiến trúc realtime 1.1.0, wire contract, quota, cutover.
- `doc/REALTIME_REVIEW_FIXES.md` — các finding/fix về transaction, auth, lease, concurrency.
- `doc/ADAPTIVE_STAGED_BOOT.md` — staged startup và adaptive boot recovery.
- `doc/RUNTIME_SELF_RECOVERY.md` — recovery ladder cho I2C/RS485/service/Wi-Fi.
- `doc/SAFETY_HARDWARE_REQUIREMENTS.md` — yêu cầu phần cứng an toàn.
- `doc/attiny_power_alarm.md` — bộ báo mất điện ATtiny.
- `doc/RELEASE_1_0_0.md` — mốc release 1.0.0 đã audit.

## Nguyên tắc bảo trì

- Không đưa HTTP/TLS/Cloud vào hot path điều khiển.
- Không coi `forwarded`/transport receipt là thành công điều khiển.
- Không đổi EEPROM region/protocol/crypto domain chỉ vì tên lịch sử “không đẹp”; phải đánh giá tương thích trước.
- Mọi thay đổi heater safety, provisioning, WebSocket auth, ATtiny, EEPROM hoặc OTA phải đi qua regression gate và commissioning phần cứng trước khi rollout.

### Tự cân bằng nhiệt (candidate commissioning trong 1.1.2)

Mặc định **OFF**, config cũ không tự bật. Khi ON, observer học từ GPIO1 ON-time thực; supervisor chỉ giới hạn heater authority/soft landing và yêu cầu quạt hút ON/OFF, giữ nguyên PID, SP và safety. Không nhận biết số xe hay nhiệt từng khoang. Learned model NVS chỉ là seed confidence thấp, không restore quyền điều khiển. Xem [Adaptive Thermal Balance](audit/THERMAL_CONTROL_V2.md) và chạy commissioning observer-only (`MAYAP_ADAPTIVE_OBSERVER_ONLY=1`) trước khi bật actuator trên máy thật. Không có tuyên bố độ chính xác ±0.1°C hay đều nhiệt toàn buồng.
