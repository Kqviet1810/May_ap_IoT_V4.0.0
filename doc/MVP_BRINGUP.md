# Bring-up: ESP32 ↔ MQTT broker ↔ Web

Kiến trúc: `ESP32 --MQTT/TLS (8883)--> broker <--MQTT/WSS-- Web`. Một broker, một không gian topic, cùng logic phiên
([broker/README.md](../broker/README.md), [MQTT_CONTRACT.md](MQTT_CONTRACT.md)); Transaction V2 theo
[TRANSACTION_V2_SPEC.md](TRANSACTION_V2_SPEC.md). Cloudflare (Worker) chỉ lo đăng nhập, D1, push, OTA và cấp
credential — **không nằm trong đường realtime** của ESP32 hay Web.

Mật khẩu ESP32 là riêng từng máy: `HMAC-SHA256(MQTT_DEVICE_SECRET, "mayap-mqtt-device:v1\n<deviceId>")`. Worker trả
`mqtt_host`, `mqtt_port`, `mqtt_password` trong `/api/device/register`; firmware lưu NVS; broker tính lại để kiểm
(không D1, không mật khẩu dùng chung). Web dùng token ký theo (thiết bị, tài khoản), sống 1 giờ.

## 1. Broker (máy chủ riêng, cần TCP trực tiếp)

Xem [broker/README.md](../broker/README.md): chạy `broker/server.js` hoặc Docker. Cần một host có chứng chỉ TLS
(Let's Encrypt) và mở cổng 8883 (ESP32) và 443 (Web). Cloudflare proxy không chuyển được TCP thô nên host này không
đặt sau proxy Cloudflare.

```bash
export BROKER_DEVICE_SECRET=<ngẫu nhiên dài>      # = MQTT_DEVICE_SECRET của Worker
export BROKER_WEB_TOKEN_SECRET=<ngẫu nhiên dài>   # = MQTT_WEB_TOKEN_SECRET của Worker
```

## 2. Worker account

- Biến repo GitHub `MQTT_BROKER_HOST` (hoặc `MQTT_BROKER_HOST` trong `cloudflare/wrangler.toml`): host của broker.
- Secret GitHub `MQTT_DEVICE_SECRET` và `MQTT_WEB_TOKEN_SECRET` (workflow deploy đẩy lên Worker; broker dùng cùng giá trị).

`POST /api/mqtt-session` (đã đăng nhập Google) trả `wss://<host>/mqtt/<deviceId>` + token cho mọi thành viên của máy và
**grant điều khiển đã ký** chỉ cho `owner/operator`. `viewer` chỉ xem.

## 3. Firmware (Arduino IDE)

Không cần file cấu hình, không cần `platform.local.txt`, không cần cờ linker: tải source, chọn board ESP32-S3
(PSRAM **Disabled**, đúng FQBN trong `build-firmware.yml`), build, nạp. Không có thư viện MQTT ngoài (client nằm trong
`mqtt_wire.h`/`mqtt_transport.h`). Máy chưa đăng ký Cloud → Serial in
`[MQTT] idle: waiting for broker host + per-device credential ...` (điều khiển cục bộ vẫn chạy bình thường).
Dùng bản DEV (`MAYAP_DIAGNOSTIC_SERIAL=1`) để thấy `[MQTT] ...`, `[HEAP] free/min/largest`.

Máy cần đã đăng ký Cloud (nhận `mqtt_*` ở lần register đầu) và đồng hồ hợp lệ (NTP): TLS cần ngày giờ thật, firmware
chờ đồng hồ trước khi kết nối.

## 4. Kiểm trên máy thật (ghi lại kết quả)

| # | Việc | Dấu hiệu đạt |
|---|------|--------------|
| 1 | Presence | Serial `[MQTT] tls+mqtt up ... task=mayap_mqtt core=0 prio=2`; Web `data-connection=online`; rút nguồn/Wi-Fi → Web chuyển offline (LWT) |
| 2 | Snapshot | Web mở tab thì nhiệt độ cập nhật ~1 s; đóng/ẩn tab thì giãn ra |
| 3 | Bật/tắt đèn | Bấm nút Đèn → máy đổi trạng thái → Web hiện BẬT/TẮT sau ACK `APPLIED` |
| 4 | Lưu cấu hình | Sửa SV ở form nhanh → máy lưu EEPROM → Web nhận `config/reported` |
| 5 | ACK | Lệnh bị từ chối hiện "Máy từ chối: …"; không có ACK cuối thì Web báo "chưa chắc chắn" (không bao giờ coi PUBACK là đã thực hiện) |
| 6 | Cô lập realtime | Rút/cắm Wi-Fi, tắt broker, đổi mạng: **không có** `[SUPERVISOR] TRIP`, không reset; `[HEAP] min` ổn định |

Số liệu cần ghi: `[HEAP] free/min/largest` sau 10 phút online; Flash/Static RAM từ log compile.

## 5. Kiểm tự động không cần phần cứng

```bash
node --test tests/*.test.cjs        # gồm broker Node (TLS+WSS), wire format firmware↔codec broker, policy guard
E2E_CHROMIUM=/opt/pw-browsers/chromium node tools/e2e/run_mvp.cjs   # Web thật + broker thật + emulator thiết bị (MQTT/TCP)
```

`tools/e2e/device_emulator.cjs` là **emulator giao thức** (đọc từ `transaction_bridge.h`), không phải firmware: nó
chứng minh Web/broker/định dạng HMAC, không chứng minh bộ điều khiển hay heap trên ESP32.
