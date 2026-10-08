# MAYAP V4 — MQTT Primary + Cold Standby Alarm (2026-10-08)

Ưu tiên: **an toàn điều khiển > ổn định RAM > cảnh báo tin cậy > realtime > tốc độ.**

## Mô hình
| Kênh | Vai trò | Khi nào hoạt động | Giữ gì khi rảnh |
|---|---|---|---|
| MQTT/WSS/TLS tới broker Durable Object | Kênh chính duy nhất: realtime Web, lệnh, trạng thái, **mọi cảnh báo** (`.../alarm`, QoS1) và heartbeat (`.../heartbeat`) | Luôn, khi có Wi-Fi + thông tin xác thực | 1 socket TLS (~43 kB heap) |
| HTTPS Emergency Fallback → Worker `POST /api/device/alarms` | Chỉ cảnh báo khi MQTT **không** chuyển được | Xem điều kiện dưới | **Không gì cả**: client tạo theo từng request, hủy ngay; không task riêng |
| HTTPS đăng ký / đặt lại PIN / OTA (kiểm tra 24 h, nạp sau xác nhận HMI) | Cung cấp, bảo trì | Hiếm, theo yêu cầu | Không gì cả |

**Hai kênh không bao giờ giữ TLS cùng lúc** (`network_io_guard.h`): một phiên HTTPS bị từ chối khi socket MQTT đang tồn tại và yêu cầu chủ MQTT đóng;
chỉ sau khi MQTT trả bộ nhớ (`stopClient`: `net.stop()` **trước**, rồi mới `mayapSetMqttTlsResident(false)`; thứ tự này được kiểm trên mọi đường đóng: yield, CONNACK bị từ chối, TLS lỗi, hết giờ bắt tay, mất Wi-Fi, áp lực heap, mất link, `Recover`, drain) và kiểm tra heap ≥ 73 728 B / khối liền ≥ 24 576 B thì HTTPS mới bắt đầu.
Ngược lại MQTT không thể bắt tay TLS khi HTTPS đang giữ lease. Bộ đếm `contexts_max` (phải =1), `overlap_violations` (=0), `overlap_denied` có trong dòng log `[TLS-GUARD]`.

## Khi nào fallback thức (`alarm_fallback_policy.h`) — thích nghi, có giới hạn
Chủ MQTT công bố tình trạng link (`mqtt_uplink.h` `Health`: Up / Closed có chủ đích / Connecting / Down, số lần lỗi, 3 lần mất gần nhất, thời điểm nhận byte cuối,
thời điểm thử nối kế tiếp, RTT PUBACK làm mượt). Task Cloud phân loại rồi quyết định:

| Phân loại | Điều kiện (bằng chứng **vận chuyển**, không phải biên nhận) | Cảnh báo nghiêm trọng | Cảnh báo thường |
|---|---|---|---|
| Healthy | link Up **và** broker còn gửi byte (≤ 20 s) — bất kể PUBACK trễ/thiếu | MQTT (không bao giờ HTTPS) | MQTT |
| Probing | link "Up" nhưng im lặng > 20 s: một PINGREQ thăm dò đã gửi | chờ tối đa 4 s cho phán quyết | chờ |
| HalfOpen | im lặng > 20 s **và** thăm dò không nhận được byte nào trong 4 s | **HTTPS ngay** (sau khi MQTT nhả bộ nhớ) | 20 s |
| Recovering | đóng có chủ đích/đang nối, lần thử kế tiếp ≤ 6 s | chờ tối đa 6 s | chờ 60 s |
| Down | link mất, thử nối thất bại, hoặc lần thử kế tiếp > 6 s | **HTTPS ngay** | chờ 20 s |
| Flapping | ≥ 3 lần mất trong 2 phút | HTTPS ngay | 20 s |
| BrokerNotStoring | *chỉ nghiêm trọng*: link sống nhưng broker không lưu cùng cảnh báo suốt ≥ 60 s qua ≥ 3 lần thử | HTTPS (phương án cuối, thang back-off nghiêm trọng, ghi rõ lý do) | không bao giờ |

**PUBACK trễ/thiếu không còn là bằng chứng link xấu.** Nó chỉ (1) giữ sự kiện trong outbox và thử lại qua MQTT bằng back-off riêng của sự kiện (cùng
`event_id` nên broker dedupe), (2) yêu cầu chủ MQTT gửi một PINGREQ thăm dò để có bằng chứng độc lập, (3) tăng bộ đếm `INGEST_TIMEOUT`. Không còn `suspect()`;
không còn đường logic từ một lần PUBACK quá hạn tới việc đóng socket MQTT khỏe. Mọi lần MQTT bị đóng có chủ đích đều ghi lý do và bằng chứng:
`[MQTT] closed on purpose (cloud-tls) why=alarm:half-open-confirmed rxAge=… probe=unanswered …`, và bộ đếm `[MQTT-STAT] closes radio/mem/cloud-tls/iso/lost`.

Mọi đường HTTPS còn cần: Wi-Fi lên **và** máy đã đăng ký **và** back-off cho phép. Hai thang back-off **tách biệt**: nghiêm trọng 5 s, 15 s, 30 s, 1, 2, 5 phút (+≤5 s jitter);
thường 30 s, 1, 2, 5, 10 phút. Một lần thành công chỉ đặt khoảng cách 1 s (cảnh báo sau đó là thật). Thang thường dài không bao giờ làm chậm cảnh báo nghiêm trọng mới.
MQTT chuyển được lại → fallback ngủ, hai thang về đầu. Trong một giờ Cloudflare/Internet hỏng: tối đa ~17 lần HTTPS (không bão kết nối lại), không mất cảnh báo.

**Không hứa 0,2 s.** Các chặng đo riêng (log `[ALARM-TIMING]` + `[CLOUD] event=... age=...` + Worker `since_received_ms`/`device_age_ms`):
(1) phát hiện lỗi → vào outbox (do điều khiển/HMI, không đổi); (2) quyết định chuyển kênh: 0 ms khi MQTT đã xác nhận mất, ≈ 4 s khi phải thăm dò link im lặng, 20 s cảnh báo thường;
PUBACK trễ **không** chuyển kênh; (3) HTTPS được chấp nhận: MQTT phải nhả bộ nhớ trước (mô hình ≈ 2,5 s bắt tay+gửi); (4) Worker nhận (`received_at`, `device_age_ms`);
(5) Push do dịch vụ push nhận (`push.sent`), không phải "đã hiển thị". Số liệu mô phỏng là mô hình, **chưa đo trên mạch**.

**Beacon HTTPS khi MQTT hỏng kéo dài.** Heartbeat chạy trên MQTT; nếu broker/DO hỏng, Worker sẽ thấy `last_seen` cũ và (khi đang ấp) báo `DEVICE_OFFLINE` nghiêm trọng dù máy bình thường.
Sau 90 s không có uplink MQTT nào được PUBACK **và transport bị xác nhận hỏng** (Down/Flapping/HalfOpen; Healthy, Probing, Recovering không bao giờ beacon), tick heartbeat 60 s gửi một `POST /api/device/heartbeat` nhỏ qua HTTPS — thực tế mỗi ~2 phút,
dưới ngưỡng offline 180 s của Worker, cùng cổng TLS độc quyền, dừng ngay khi MQTT chuyển được lại.

### Độc lập của đường HTTPS với broker Durable Object
Kiểm tra: HTTPS fallback không đi qua DO (`/api/device/alarm|alarms` nằm trong Worker chính; xác thực bằng `device_key` băm riêng, khác mật khẩu MQTT; `event_id` + biên nhận `durable`
từ D1; cùng `recordAlarmEvent` nên chống trùng chéo kênh, cooldown và thứ tự raise/recover giống hệt). Rủi ro còn lại dùng chung là **Worker chính + D1 + edge Cloudflare**;
chia thêm một Worker "tối thiểu" vẫn dùng chung D1, Push và edge, nhưng thêm bí mật phải cấu hình/triển khai và một bề mặt lỗi phiên bản → **không tách** (rủi ro không giảm đáng kể).
Biên nhận `durable`/`stored:true` nghĩa là sự kiện và việc push đã nằm trong D1 — **không** nghĩa là đã gửi Push. Tiến trình push có ở `GET /api/device/<id>/status?alarms=1`
(`push.recipients/sent/pending/gone/expired`; `sent` = dịch vụ push đã nhận).

### Giới hạn outbox (không mất âm thầm)
Outbox 16 mục, cảnh báo thường bị từ chối (đếm `outboxDropped`, log) khi còn 4 chỗ cuối để dành cho nghiêm trọng; nghiêm trọng đầy 16 thì bị từ chối **có đếm**, cạnh lỗi được giữ lại ở
bộ theo dõi lỗi (`cloud_fault_events`) và gửi lại khi outbox có chỗ; các chuyển trạng thái trung gian vượt giới hạn đệm được gộp thành đồng bộ trạng thái cuối. Giới hạn đã biết: một đợt
lỗi dài > 64 cạnh liên tiếp khi mất mạng hàng giờ chỉ giữ trạng thái cuối cùng của từng lỗi, không giữ từng cạnh trung gian.

## Web nhận cảnh báo khi mất realtime (Web 1.1.7)
Realtime lên: dữ liệu và lỗi lấy từ snapshot realtime như cũ. Realtime mất: sau 4 s (không giật khi nối lại nhanh) Web hỏi Worker qua HTTPS (`GET /api/device/<id>/status?alarms=1`, cùng phiên
tài khoản) mỗi 20 s, **chỉ khi realtime đang mất**; không thêm socket/TLS nào trên ESP32. Ba trạng thái tách biệt: **MẤT REALTIME** (Web không có link, máy vẫn báo về máy chủ), **MÁY MẤT KẾT NỐI**
(Worker không nghe được máy > 180 s trên mọi kênh), **MÁY ĐANG CÓ LỖI** (banner liệt kê lỗi đang hoạt động theo máy chủ). Web không tải được máy chủ → không kết luận gì về máy. Realtime trở lại
→ huỷ poll, bỏ bản sao máy chủ, bỏ qua câu trả lời đến muộn (không cảnh báo cũ/trùng); bản sao quá 60 s không được hiển thị.

## Wi-Fi (`wifi_power_policy.h`, chỉ networkTask chạm driver)
Mặc định **PERFORMANCE** (`WIFI_PS_NONE`) mọi lúc. Tiết kiệm (ECO) nằm sau cờ biên dịch `MAYAP_WIFI_ECO` (**mặc định 0**) cho đến khi thử trên mạch: khi bật, PERFORMANCE 15 phút đầu và khi còn dùng Web,
quá 15 phút không Web và 2 phút không cảnh báo → `WIFI_PS_MIN_MODEM`, hoạt động đầu tiên → PERFORMANCE ngay. Đổi chế độ chỉ gọi `esp_wifi_set_ps` (không ngắt/kết nối lại, MQTT và cảnh báo giữ nguyên);
`tools/test_wifi_state.py` chạy cả hai cấu hình ECO=0 và ECO=1.

## Lỗi mạng không được ảnh hưởng điều khiển
Không có đường restart nào từ MQTT/TLS/Cloudflare/fallback (service isolation chỉ tạm dừng dịch vụ online). Điều khiển nhiệt/đảo trứng chạy ở task tĩnh
core 1 không dùng heap TLS; fallback thất bại chỉ tăng back-off. Hộp thư uplink, outbox cảnh báo và bộ đệm sự kiện lỗi đều có kích thước cố định.

## OTA
Task OTA chỉ còn việc HTTPS ngắn (kiểm tra mỗi 24 h + jitter, lần đầu 10 phút sau khi bật nguồn; nạp sau khi xác nhận HMI và đã dừng mẻ). Mỗi phiên chạy sau khi MQTT
đã đóng và trả `mayapReleaseCloudTlsYield()` khi xong để MQTT nối lại. ArduinoOTA đã gỡ. Stack 12 288 B giữ nguyên: đỉnh stack khi nạp thật chưa đo trên mạch,
nên chưa gộp/thu nhỏ task (xem báo cáo).

## Kiểm chứng
Host: `tests/runtime-alarm-fallback.cpp` (đường gửi thật + guard thật + mô hình heap: min free ≥ 50 kB, `contexts_max`=1), `tests/mqtt-transport.cpp` (cờ resident trên mọi đường đóng/lỗi),
`tests/runtime-stability.cpp` (loại trừ TLS), `tests/alarm-delivery.test.cjs` (broker thật → Worker thật → D1, trùng giữa hai kênh), `tests/runtime-wifi-state.cpp` (chính sách nguồn, ECO 0/1). Kịch bản `runtime-alarm-fallback`: broker khoẻ, link mất, đóng có chủ đích ≤/> 6 s, không PUBACK (có/không mẫu RTT), half-open, không lưu, flapping, Worker/D1/DNS/Internet hỏng (mã -1/500/503), Wi-Fi chập chờn, báo đồng thời có thứ tự, outbox đầy, MQTT hồi phục, beacon; `tests/web-experience.test.cjs` (Web mất realtime/resync).
**Chưa đo trên mạch:** free heap/min heap/largest block/stack watermark thực tế — firmware in `[HEAP]`, `[TASK] stack` và `[TLS-GUARD]` mỗi 60 s (bản chẩn đoán/PILOT).
