# MVP bring-up: Web ↔ Cloudflare broker ↔ ESP32

Kiến trúc: `Web (MQTT.js/WSS)` → `broker Durable Object` ← `ESP32 (WSS)`. Chỉ ESP32 + Cloudflare + Web, **không có máy chủ thứ tư**.
ESP32 chạy TLS + WebSocket + MQTT 3.1.1 **hoàn toàn trong task `mayap_mqtt`** (static, ghim core 0): `WiFiClientSecure` + `mqtt_ws.h` (khung WebSocket có mask, bounded) + `mqtt_wire.h`; không esp-mqtt, không `esp_websocket_client`, không task ẩn, không hack linker/Kconfig. Bộ đệm tĩnh: gửi 2128 B, nhận 2120 B.
Topic/QoS/retain theo [MQTT_CONTRACT.md](MQTT_CONTRACT.md); Transaction V2 theo
[TRANSACTION_V2_SPEC.md](TRANSACTION_V2_SPEC.md). Auth Web là **token ký theo (thiết bị, tài khoản)**, sống 1 giờ, broker kiểm không cần D1; token của máy này vô dụng với máy khác.
Auth ESP32 dùng **mật khẩu riêng từng máy** = `HMAC-SHA256(MQTT_DEVICE_SECRET, "mayap-mqtt-device:v1\n<deviceId>")`: Worker trả trong `/api/device/register` (trường `mqtt_password`), firmware lưu NVS, broker tính lại để kiểm (không D1, không mật khẩu dùng chung).

## 1. Deploy broker (Worker riêng, không đụng Worker Push/account)

```bash
cd cloudflare
npx wrangler deploy -c wrangler-broker.toml
npx wrangler secret put BROKER_DEVICE_SECRET           -c wrangler-broker.toml   # suy ra mật khẩu từng ESP32 (cùng giá trị MQTT_DEVICE_SECRET của Worker account)
npx wrangler secret put BROKER_WEB_TOKEN_SECRET        -c wrangler-broker.toml   # ký/kiểm token Web (cùng giá trị ở Worker account)
```

Không có hai secret này broker từ chối mọi kết nối (fail closed). Ghi lại host
`mayap-mqtt-broker.<account>.workers.dev`.

## 2. Worker account (cấp quyền cho Web)

Thêm vào `[vars]` của `cloudflare/wrangler.toml` (không phải secret):

```toml
MQTT_BROKER_URL = "wss://mayap-mqtt-broker.<account>.workers.dev/mqtt"
```

và secret **trùng với `BROKER_WEB_TOKEN_SECRET`** của broker:

```bash
npx wrangler secret put MQTT_WEB_TOKEN_SECRET
npx wrangler secret put MQTT_DEVICE_SECRET      # cùng giá trị BROKER_DEVICE_SECRET
```

`POST /api/mqtt-session` (đã đăng nhập Google) trả thông tin broker cho mọi thành viên của máy và
**grant điều khiển đã ký** chỉ cho `owner/operator`. `viewer` chỉ xem.

## 3. Firmware (Arduino IDE)

Không cần file cấu hình riêng: host broker được track trong `mqtt_transport.h`
(`MAYAP_BROKER_HOST`, ghi đè bằng `-D` nếu dùng broker khác) và mật khẩu MQTT của máy
nằm trong NVS sau lần `/api/device/register` đầu tiên (Worker cần secret
`MQTT_DEVICE_SECRET`, cùng giá trị `BROKER_DEVICE_SECRET` của broker). Tải source từ GitHub, build, nạp là chạy.
Chưa đăng ký Cloud → Serial in `[MQTT] idle: waiting for the per-device credential ...` (máy vẫn chạy bình thường).
Board: ESP32-S3, PSRAM **Disabled**, đúng FQBN trong `build-firmware.yml`. Dùng bản DEV
(`MAYAP_DIAGNOSTIC_SERIAL=1`) để thấy `[HEAP] free/min/largest` và `[MQTT] ...` trên Serial.

Máy cần đã đăng ký Cloud (có `command_key` trong NVS) và đồng hồ hợp lệ (NTP) — firmware từ chối
grant khi `time()` chưa hợp lệ.

## 4. Kiểm trên máy thật (ghi lại kết quả)

| # | Việc | Dấu hiệu đạt |
|---|------|--------------|
| 1 | Presence | Serial `[MQTT] connected MAP-…`; Web `data-connection=online`; rút nguồn/Wi-Fi → Web chuyển offline (LWT) |
| 2 | Snapshot | Web mở tab thì nhiệt độ cập nhật ~1 s; đóng/ẩn tab thì giãn ra |
| 3 | Bật/tắt đèn | Bấm nút Đèn → máy đổi trạng thái → Web hiện BẬT/TẮT sau ACK `APPLIED` |
| 4 | Lưu cấu hình | Sửa SV ở form nhanh → máy lưu EEPROM → Web nhận `config/reported` |
| 5 | ACK | Lệnh bị từ chối hiện "Máy từ chối: …"; không có ACK cuối thì Web báo "chưa chắc chắn" (không bao giờ coi PUBACK là đã thực hiện) |

Số liệu cần ghi: `[HEAP] free/min/largest` sau 10 phút online; Flash/Static RAM từ log compile.

## 5. Kiểm tự động không cần phần cứng

```bash
node --test tests/*.test.cjs                       # toàn bộ test (có policy guard + /api/mqtt-session)
E2E_CHROMIUM=/opt/pw-browsers/chromium node tools/e2e/run_mvp.cjs   # Web thật + workerd thật + emulator thiết bị
```

`tools/e2e/device_emulator.cjs` là **emulator giao thức** (đọc từ `transaction_bridge.h`), không phải
firmware: nó chứng minh Web/broker/định dạng HMAC, không chứng minh bộ điều khiển hay heap trên ESP32.
