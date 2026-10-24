# Thermal V2.0.1 — ứng viên phát hành (rc), ghi chú

**2.0.3:** Web: thẻ **Nâng cao** gộp "Nâng cao trên máy" và "Kỹ thuật PID" (ô sửa được sáng, ô chỉ xem tối, 2–4 cột); tile **Quạt hút** (đổi tên từ "Thông gió") hiện *vì sao bật* (Khẩn cấp / Quá nhiệt cao / Quá nhiệt / Bảo vệ / Lỗi cảm biến / Từ Web / Thông gió / Thông gió theo giờ); ô đảo trứng ghi "ĐANG ĐẢO TRÁI ◀ / PHẢI ▶". Máy: ngưỡng bật/tắt quạt hút chỉnh được theo bước 0,05 °C (cách SP tối thiểu 0,05 °C, bật cao hơn tắt tối thiểu 0,05 °C); **Cập nhật tự động** (HMI Nâng cao → Điều khiển Web, hàng thứ 7, mặc định BẬT, lưu cùng bản ghi mã kỹ thuật, bản ghi cũ đọc là BẬT): khi có firmware mới và máy rảnh (không mẻ, không kiểm tra, công tắc nhiệt tắt) trong 60 giây liên tục thì máy tự tải, kiểm SHA-256 + chữ ký như đường HMI rồi nạp; có mẻ đang chạy thì **không** tự cập nhật. Chi tiết: `doc/WEB_OUTPUT_CONTROL.md`.

**2.0.2 (G8):** sửa theo log lò thật 24/10 — trọng số tin cậy cho dự đoán (luật bám PV đo khi dự đoán không khớp), tích phân theo PV đo khi kẹt dưới SP, lọc nhiễu dự đoán từ độ dốc, quạt hút thành đầu vào của khối V2 (học tổn thất do quạt, bù trước theo lịch thông gió tự động), cột `trust_pct` trong CSV, hai trạng thái `recover` / `degraded`. Chi tiết, số liệu và cái giá: `doc/THERMAL_V2_G8_DESIGN.md`. Chưa thử lò thật.

**2.0.1 (G7):** sửa theo log lò thật 23/10 — dự đoán neo vào nhu cầu đo được, học nhiệt tồn, phanh lần đầu, giới hạn đuổi khuyết (cửa), chạy ngầm khi quạt hút chặn sưởi. Chi tiết, số liệu và cái giá: `doc/THERMAL_V2_G7_DESIGN.md`. Web: ô đảo trứng chia đôi khi được phép và AUTO tắt; thẻ "Nâng cao trên máy" (đọc/ghi); log 30 phút (CSV có cột `v2_state`).

Trạng thái: **chưa thử lò thật.** Mô phỏng + biên dịch CI. Không gắn tag, không tạo Release (chờ bạn).

Ứng viên = commit ghi ở cuối tệp này, artifact do CI `Build & release firmware` (PR #33) dựng.

Gồm: bộ điều khiển `thermal_v2.h` (học trễ/độ lợi/mô hình tổn thất, luật dự đoán + PI, bộ canh phản hồi, bộ quan sát trứng khi cảm biến ≤ 0,02 °C), bộ chuyển đổi `thermal_v2_adapter.h` (chạy 100 ms sau tác vụ 5 ms), PID Monitor 9 mục, LOG AI schema 2, E113 không nổ khi leo nhiệt đúng kỳ vọng. Lớp bảo vệ High/Emergency/cảm biến/E115/contactor KHÔNG đổi.

Giới hạn đã biết (chi tiết `doc/THERMAL_V2_G2_DESIGN.md` §7c, §11, §12 và `tools/thermal-v2/g*_results.txt`): R1.reach.restart không đạt (không lưu đường đã học); vọt/dải ±0,1 °C ở khay lạnh lớn và quạt vent; ca High hiếm ở lò trễ dài khối khí nhỏ; lỗi cảm biến đứng/trôi/nhảy thấp và SSR hàn dính cần phần cứng; slot batch fit chưa đo trên ESP32.

Thử lò thật: `doc/FIELD_TRIAL_V2.md`.
