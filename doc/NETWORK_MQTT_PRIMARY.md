# MAYAP V4 — MQTT Primary + Cold Standby Alarm (2026-10-08)

Ưu tiên: **an toàn điều khiển > ổn định RAM > cảnh báo tin cậy > realtime > tốc độ.**

## Mô hình
| Kênh | Vai trò | Khi nào hoạt động | Giữ gì khi rảnh |
|---|---|---|---|
| MQTT/WSS/TLS tới broker Durable Object | Kênh chính duy nhất: realtime Web, lệnh, trạng thái, **mọi cảnh báo** (`.../alarm`, QoS1) và heartbeat (`.../heartbeat`) | Luôn, khi có Wi-Fi + thông tin xác thực | 1 socket TLS (~43 kB heap) |
| HTTPS Emergency Fallback → Worker `POST /api/device/alarms` | Chỉ cảnh báo khi MQTT **không** chuyển được | Xem điều kiện dưới | **Không gì cả**: client tạo theo từng request, hủy ngay; không task riêng |
| HTTPS đăng ký / đặt lại PIN / OTA (kiểm tra 24 h, nạp sau xác nhận HMI) | Cung cấp, bảo trì | Hiếm, theo yêu cầu | Không gì cả |

**Hai kênh không bao giờ giữ TLS cùng lúc** (`network_io_guard.h`): một phiên HTTPS bị từ chối khi socket MQTT đang tồn tại và yêu cầu chủ MQTT đóng;
chỉ sau khi MQTT trả bộ nhớ (`stopClient` → `mayapSetMqttTlsResident(false)`) và kiểm tra heap ≥ 73 728 B / khối liền ≥ 24 576 B thì HTTPS mới bắt đầu.
Ngược lại MQTT không thể bắt tay TLS khi HTTPS đang giữ lease. Bộ đếm `contexts_max` (phải =1), `overlap_violations` (=0), `overlap_denied` có trong dòng log `[TLS-GUARD]`.

## Khi nào fallback thức (`alarm_fallback_policy.h`)
Tất cả đều phải đúng: MQTT không chuyển được (link xuống hoặc không PUBACK) **và** cảnh báo cũ nhất đã chờ ≥ 10 s (nghiêm trọng) / 60 s (còn lại) **và**
Wi-Fi lên **và** máy đã đăng ký **và** back-off cho phép (30 s, 1, 2, 5, 10 phút + ≤5 s jitter; chỉ tính khi có request HTTPS thật).
MQTT chuyển được lại → fallback ngủ và back-off về đầu. Heartbeat **không** có fallback HTTPS (MQTT im lặng = "không liên lạc được", không mở TLS thứ hai).

Worker fallback (`/api/device/alarm|alarms`) độc lập với broker DO: xác thực bằng `device_key` riêng (khác khóa MQTT), `event_id` chống trùng
(cùng id qua MQTT rồi HTTPS hoặc ngược lại chỉ ra một dòng), biên nhận `durable`, retry theo back-off.

## Wi-Fi tiết kiệm (`wifi_power_policy.h`, chỉ networkTask chạm driver)
PERFORMANCE 15 phút đầu sau khi bật nguồn và khi còn dùng Web (mỗi `session` đang mở, lệnh, yêu cầu config/history làm mới mốc 15 phút);
quá 15 phút không dùng Web và không có cảnh báo trong 2 phút gần nhất → `WIFI_PS_MIN_MODEM`. Hoạt động đầu tiên → PERFORMANCE ngay.
Khác chính sách cũ đã gỡ (đảo theo lease/lỗi): chỉ một quy tắc chậm theo thời gian; kill-switch là hằng `WEB_IDLE_MS`.

## Lỗi mạng không được ảnh hưởng điều khiển
Không có đường restart nào từ MQTT/TLS/Cloudflare/fallback (service isolation chỉ tạm dừng dịch vụ online). Điều khiển nhiệt/đảo trứng chạy ở task tĩnh
core 1 không dùng heap TLS; fallback thất bại chỉ tăng back-off. Hộp thư uplink, outbox cảnh báo và bộ đệm sự kiện lỗi đều có kích thước cố định.

## OTA
Task OTA chỉ còn việc HTTPS ngắn (kiểm tra mỗi 24 h + jitter, lần đầu 10 phút sau khi bật nguồn; nạp sau khi xác nhận HMI và đã dừng mẻ). Mỗi phiên chạy sau khi MQTT
đã đóng và trả `mayapReleaseCloudTlsYield()` khi xong để MQTT nối lại. ArduinoOTA đã gỡ. Stack 12 288 B giữ nguyên: đỉnh stack khi nạp thật chưa đo trên mạch,
nên chưa gộp/thu nhỏ task (xem báo cáo).

## Kiểm chứng
Host: `tests/runtime-alarm-fallback.cpp` (đường gửi thật + guard thật + mô hình heap: min free ≥ 50 kB, `contexts_max`=1), `tests/mqtt-transport.cpp` (cờ resident trên mọi đường đóng/lỗi),
`tests/runtime-stability.cpp` (loại trừ TLS), `tests/alarm-delivery.test.cjs` (broker thật → Worker thật → D1, trùng giữa hai kênh), `tests/runtime-wifi-state.cpp` (chính sách nguồn).
**Chưa đo trên mạch:** free heap/min heap/largest block/stack watermark thực tế — firmware in `[HEAP]`, `[TASK] stack` và `[TLS-GUARD]` mỗi 60 s (bản chẩn đoán/PILOT).
