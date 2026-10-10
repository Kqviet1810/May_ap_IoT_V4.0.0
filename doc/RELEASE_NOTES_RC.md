# Ghi chú Release Candidate — MAYAP V4 (PR #14)

**Đây là Release Candidate để thử nghiệm, không phải bản Production đã nghiệm thu.** Phiên bản mã: firmware 1.1.4 (không đổi số), định danh bằng RC SHA / `MAYAP_BUILD_ID`.

## Hành vi mặc định (cờ `MAYAP_SMART_THERMAL=0`)
Không thay đổi so với baseline hiện hành: PID, Adaptive Thermal V1, bảo vệ High 38.2 °C / Emergency 39.0 °C, thứ tự và cực tính SSR–contactor, fail-safe, OTA. Cờ 0 có Flash/RAM giống hệt bản không đặt cờ (CI so sánh).

## Thay đổi trong PR #14
* **Smart Thermal (mặc định TẮT):** hãm khởi động theo nhiệt lượng đang bay, hold hội tụ nhanh, cổng "ước lượng gain nhanh đã cũ không phải bằng chứng đổi plant" (sửa lỗi AutoTune `full165`: ACCEPTED_BAD 1 → 0 với cờ ON).
* **Cổng kiểm thử/CI:** sensor-path (cảnh báo/E115/E104 chạy trên nhiệt độ được báo cáo), A/B/C so với baseline đóng băng, Smart AutoTune OFF **và ON**, biến thể build cờ 0/1 với ngân sách Flash/RAM, artifact RC kèm `BUILD_INFO.txt` và `SHA256SUMS.txt`.
* **Tài liệu:** kế hoạch nghiệm thu một quy trình, checklist đo ESP32 không nhiệt, hướng dẫn nâng cấp/rollback, bảng chặn xuất xưởng.

## Giao diện (sau RC)
* HMI màn hình ngủ: chỉ nhiệt độ, chữ số lớn nhất vừa màn hình, nằm giữa; giờ giữ ở góc trái trên. Độ ẩm không còn hiện ở màn hình ngủ.
* HMI chỉnh thời gian ngủ: cùng bố cục màn chỉnh thông số (tiêu đề, nhãn, giá trị lớn, dòng hướng dẫn).
* Web 1.1.12: nút ba gạch thay thanh tab dưới **chỉ** khi màn thấp (chiều cao ≤ 600 px); màn cao hơn giữ thanh tab dưới như trước.

## Không có trong RC
D2–D6 (latch contactor, ngân sách đóng cắt, chương trình nhiệt, làm mát trứng, lưu chương trình) chỉ là module thiết kế, không được firmware include. `PriorMinError` giữ 3.5.

## Giới hạn đã biết
* Chưa đo thời gian thực/ngăn xếp/heap trên chip; chưa nghiệm thu lò thật.
* Cảm biến sai hợp lệ (kẹt/trôi trong dải) chỉ bị chặn bởi thermostat cơ độc lập; firmware không đảm bảo phát hiện.
* Contactor có thể đóng lại sau High khi SSR dính (High không chốt); cần biện pháp được duyệt trước khi vận hành có trứng.
* Mô phỏng (không phải bằng chứng máy thật): Smart ON còn 18 High / 3 Emergency trong miền Kh ≥ 0.133 °C/s; Adaptive baseline (cờ 0) có 33 / 17 trong cùng miền.
