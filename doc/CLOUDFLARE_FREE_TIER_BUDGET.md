# Ngân sách Cloudflare bản miễn phí (backend sau 3.1.2: cảnh báo theo sự cố, giám sát mất kết nối do broker)

Ghi lại vì sao một máy từng dùng 92 % quota D1 mỗi ngày, cái gì đã đổi, và cách đo để biết còn chạy thêm được bao nhiêu máy.

## Giới hạn miễn phí cần giữ dưới (tra ngày 19/11/2026, kiểm tra lại trên trang giá của Cloudflare)
| Tài nguyên | Giới hạn mỗi ngày | Quá giới hạn thì |
|---|---|---|
| D1 dòng đọc | 5.000.000 | truy vấn báo lỗi đến 00:00 UTC |
| D1 dòng ghi | 100.000 (mỗi chỉ mục được ghi tính thêm một dòng) | như trên |
| Yêu cầu Worker | 100.000 | như trên |
| Yêu cầu Durable Objects | 100.000 (tin nhắn WebSocket vào tính 1/20, mỗi lần báo thức DO tính một) | như trên |
| Thời gian chạy DO | 13.000 GB-giây (DO ngủ đông không bị tính khi rảnh) | như trên |

Khi D1 trả lỗi thì push báo động và đăng nhập cùng hỏng, nên mục tiêu thiết kế là **dưới 50 % mỗi chỉ số**, và đặt thông báo Cloudflare ở 50 % và 80 %.

## Nguyên nhân gốc đã tìm ra (một máy, đo bằng D1 Insights)
1. Cron chạy mỗi phút và lần nào cũng xóa dữ liệu quá hạn bảy ngày. `alarm_events` chỉ có chỉ mục theo `expires_at`, còn lệnh dọn lọc theo `created_at`, nên mỗi phút quét cả bảng (khoảng 1.500 dòng) hai lần: 4,38 triệu dòng mỗi ngày, 88 % tổng đọc. Chi phí này tăng tuyến tính theo số máy.
2. Khoảng 190 sự kiện cảnh báo mỗi ngày cho một máy, 78 % là E111 và E110 bật rồi tắt liên tục, mỗi sự kiện sinh một push cho từng người nhận.
3. Heartbeat mỗi phút ghi D1 và gọi một yêu cầu Worker (1.417 lần mỗi máy mỗi ngày), và một lần báo thức DO để chuyển tiếp.
4. Bảng `push_subscriptions` có khoảng 160 dòng chết, bị quét mỗi phút.

## Đã đổi trong backend (Worker và broker, sau firmware 3.1.2; không đổi firmware, không đổi Web)
- **Chỉ mục:** `alarm_events(created_at)` thay cho `alarm_events(expires_at)` (`expires_at` luôn bằng `created_at` cộng 1 giờ), nên số chỉ mục không tăng và không thêm dòng ghi. Migration 0010.
- **Cron tách làm hai:** mỗi phút chỉ kiểm tra mất kết nối (dự phòng), đẩy các push đến hạn và đóng các việc quá một giờ. Dọn dẹp (sự kiện quá 7 ngày, challenge Google, phiên đăng nhập đã hết hạn một tuần, đăng ký push không còn nhận được, `alarm_log` quá 90 ngày vào 03:00 UTC) chạy ở phút đầu mỗi giờ, và một lỗi dọn dẹp không bao giờ chặn việc kiểm tra mất kết nối.
- **Mô hình sự cố (off-delay kiểu ISA-18.2):** lần ACTIVE đầu của một cảnh báo **luôn được đẩy ngay**, mọi mức độ. RESOLVED của nó chờ 5 phút (critical, system) hoặc 10 phút (warning, info). ACTIVE lại của cùng cảnh báo trong lúc chờ nghĩa là sự cố chưa kết thúc: RESOLVED đang chờ bị hủy và không ai bị báo thêm. Lịch sử vẫn ghi mọi lần chuyển. Cảnh báo bật tắt 76 lần mỗi ngày thành một ACTIVE và một RESOLVED cho mỗi sự cố.
- **Giám sát mất kết nối do broker (thay cho cron đọc `last_seen`):** broker biết socket của máy. Khi socket mất và không có socket mới thay thế, broker đếm 180 giây kể từ gói tin cuối của máy rồi báo Worker **một lần**; Worker nâng cảnh báo `DEVICE_OFFLINE` mức critical (khi đang ấp) đúng như trước, nhưng đúng lúc đủ 180 giây (cron cũ báo trong khoảng 180–240 giây tùy phút). Máy trở lại thì heartbeat đầu tiên kết thúc cảnh báo ngay. Heartbeat đều đặn không còn đi D1: broker chỉ chuyển tiếp heartbeat đầu của mỗi kết nối, khi cờ batch đổi, hoặc mỗi 10 phút. Cron giữ vai trò **lưới an toàn** với ngưỡng 15 phút (khi broker không báo được).

Tác dụng lên ngân sách: bỏ khoảng 1.400 lần ghi D1, 1.400 yêu cầu Worker và 1.400 lần báo thức DO mỗi máy mỗi ngày; số dòng đọc không còn phụ thuộc kích thước `alarm_events`.

## Cách đo sau khi triển khai (D1 Insights, khoảng 1 giờ, và Metrics theo ngày)
- Hai lệnh xóa `alarm_events` / `alarm_deliveries` phải đọc dưới vài chục dòng mỗi lần chạy, và truy vấn trạng thái cảnh báo của Web không còn quét cả bảng.
- `UPDATE devices SET last_seen…` chỉ còn vài chục lần mỗi ngày mỗi máy.
- Số yêu cầu `MqttBrokerDO` mỗi ngày mỗi máy (Workers & Pages, Durable Objects): ghi lại để nhân lên số máy.
- Đếm sự kiện còn lại: `SELECT alarm_type, COUNT(*) FROM alarm_events GROUP BY alarm_type ORDER BY 2 DESC;`

## Chưa làm
- Bản snapshot 1 giây khi có Web đang xem (firmware `REALTIME_SNAPSHOT_ACTIVE_INTERVAL_MS`) vẫn là nguồn chính của yêu cầu DO khi xem Web lâu; đổi cần cập nhật OTA.
- Nguyên nhân E111 bật tắt khi nhiệt đo chỉ khoảng 37,5 °C (cần ngưỡng "Nhiệt cao" đang đặt trên máy).
- Trần số sự kiện mỗi máy mỗi ngày.
