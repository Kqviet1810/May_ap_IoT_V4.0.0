# Thermal V2.0.1 — ứng viên phát hành (rc), ghi chú

**2.0.1 (G7):** sửa theo log lò thật 23/10 — dự đoán neo vào nhu cầu đo được, học nhiệt tồn, phanh lần đầu, giới hạn đuổi khuyết (cửa), chạy ngầm khi quạt hút chặn sưởi. Chi tiết, số liệu và cái giá: `doc/THERMAL_V2_G7_DESIGN.md`. Web: ô đảo trứng chia đôi khi được phép và AUTO tắt; thẻ "Nâng cao trên máy" (đọc/ghi); log 30 phút (CSV có cột `v2_state`).

Trạng thái: **chưa thử lò thật.** Mô phỏng + biên dịch CI. Không gắn tag, không tạo Release (chờ bạn).

Ứng viên = commit ghi ở cuối tệp này, artifact do CI `Build & release firmware` (PR #33) dựng.

Gồm: bộ điều khiển `thermal_v2.h` (học trễ/độ lợi/mô hình tổn thất, luật dự đoán + PI, bộ canh phản hồi, bộ quan sát trứng khi cảm biến ≤ 0,02 °C), bộ chuyển đổi `thermal_v2_adapter.h` (chạy 100 ms sau tác vụ 5 ms), PID Monitor 9 mục, LOG AI schema 2, E113 không nổ khi leo nhiệt đúng kỳ vọng. Lớp bảo vệ High/Emergency/cảm biến/E115/contactor KHÔNG đổi.

Giới hạn đã biết (chi tiết `doc/THERMAL_V2_G2_DESIGN.md` §7c, §11, §12 và `tools/thermal-v2/g*_results.txt`): R1.reach.restart không đạt (không lưu đường đã học); vọt/dải ±0,1 °C ở khay lạnh lớn và quạt vent; ca High hiếm ở lò trễ dài khối khí nhỏ; lỗi cảm biến đứng/trôi/nhảy thấp và SSR hàn dính cần phần cứng; slot batch fit chưa đo trên ESP32.

Thử lò thật: `doc/FIELD_TRIAL_V2.md`.
