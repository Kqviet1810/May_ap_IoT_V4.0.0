# HMI và quản lý PID – MAYAP V4 (NÂNG CAO)

Tài liệu mô tả cơ chế **mã kỹ thuật 4 số**, menu **NÂNG CAO**, **Quyền Web PID**, **Bản ghi cũ** và **PID Monitor**.
Mọi kết quả kiểm thử bên dưới là **mô phỏng / host / CI**. Phần cứng thật (LCD, encoder, EEPROM I²C, mất điện thật) **NOT TESTED**.

## 1. Menu HMI

`CÀI ĐẶT CHUNG` = NHIỆT ĐỘ (ngưỡng vận hành, không đổi), ĐẢO TRỨNG, HỆ THỐNG, QUẠT HÚT, TẠO ẨM, **NÂNG CAO**, CHẾ ĐỘ TEST, THOÁT.

`NÂNG CAO` (cần mã kỹ thuật): PID / Gia nhiệt · Smart AutoTune · PID Monitor · Bảo vệ nhiệt · Hiệu chuẩn (bù nhiệt, bù ẩm) ·
Hồ sơ nhiệt · Bản ghi cũ · Quyền Web (ẨN/HIỆN) · Đổi mã kỹ thuật · Thoát (lưu).

## 2. Mã kỹ thuật

* Đúng 4 chữ số, nhập bằng núm xoay: xoay chọn 0–9, nhấn ngắn xác nhận từng số, giữ để lùi/thoát. Chữ số đã nhập hiển thị dấu chấm.
* Dùng chung HMI và Web, **độc lập** với mã PIN ghép nối thiết bị. Chỉ HMI đặt/đổi được.
* Lưu: muối ngẫu nhiên 16 B + HMAC-SHA256 lặp 256 vòng, hai ô EEPROM luân phiên (CRC + số thứ tự) tại 0x3000/0x3080. Không có mã mặc định trong mã nguồn, không ghi log.
* Sai quá 5 lần: khóa 60 s, tăng dần tới 15 phút. Bộ đếm **dùng chung** HMI + Web và lưu bền (khởi động lại không xóa).
* Phiên HMI chỉ nằm trong RAM: tự khóa khi thoát, sau 120 s không thao tác (giữ phiên 30 s/lần khi đang dùng) và khi khởi động lại.
* Giới hạn trung thực: 4 chữ số = 10⁴ khả năng. Đây là cổng truy cập (chống thao tác nhầm, chống thử mã qua giao diện); **không** chống được người có thể đọc trực tiếp chip EEPROM.

## 3. Quyền Web PID

* Mặc định **ẨN** (lưu bền). ẨN: Web không thấy mục Kỹ thuật PID, mã/yêu cầu kỹ thuật bị firmware từ chối (`WEB_HIDDEN`).
* HIỆN: Web phải nhập đúng mã kỹ thuật (kiểm trên ESP32). Phiên Web tối đa 600 s, mất khi tắt quyền, mất mạng hoặc khởi động lại.
* Web **không tự lưu PID**. Mỗi yêu cầu đổi thông số kỹ thuật / chạy Smart AutoTune chỉ được **đỗ** lại; HMI hiện giá trị cũ → mới và hỏi CÓ/KHÔNG.
  Chỉ khi CÓ, firmware kiểm tra lại toàn bộ điều kiện an toàn (không có mẻ/phục hồi mẻ, không AutoTune, không lỗi bộ nhớ/nhật ký an toàn, PID hợp lệ) rồi mới thực thi.
  60 s không trả lời = tự hủy. Giữ nút = KHÔNG.
* Tắt quyền trên HMI bất kỳ lúc nào: phiên + yêu cầu đang chờ bị thu hồi ngay.
* `config/set` từ Web **không** đổi được trường kỹ thuật (kể cả gửi lệnh trực tiếp qua MQTT): bridge từ chối `TECH_VIA_HMI_APPROVAL`, controller kiểm lại lần nữa.

## 4. Sửa trên HMI và lưu một lần

Trong NÂNG CAO, sửa nhiều thông số: chỉ ghi vào bản nháp RAM (không ghi EEPROM khi xoay). Giữ nút để thoát = **tự lưu một lần, không hỏi**.
Không có thay đổi = không ghi. Lưu lỗi: báo lỗi rõ, giữ nguyên bản nháp để thử lại. PID mới chỉ áp dụng sau khi firmware xác nhận đã lưu.
Đang ấp / chờ phục hồi mẻ: các thông số này bị khóa như trước.

## 5. Bản ghi cũ (chỉ HMI)

* 3 bản gần nhất (1 mới nhất, 3 cũ nhất) trong vùng EEPROM trống 0x3100.., mỗi bản một ô có CRC + số thứ tự; mỗi lần lưu ghi **đúng một ô** (ô cũ nhất hoặc ô hỏng) rồi đọc lại, nên mất điện giữa chừng chỉ mất bản đang ghi.
  Không đụng cấu hình A/B, dữ liệu mẻ, lịch sử nhiệt.
* Nội dung: Kp/Ki/Kd, trần công suất, chu kỳ SSR, cân bằng nhiệt (Adaptive), ngưỡng chẩn đoán, relay tune, bù nhiệt/ẩm.
  **Không** gồm: mã kỹ thuật, quyền Web, dữ liệu mẻ, ngưỡng an toàn độc lập (High/Emergency/…).
  Hồ sơ nhiệt đã học (Adaptive Thermal V1) **không** nằm trong bản ghi này.
* Xem bằng núm xoay (dấu `!` = khác cấu hình hiện tại); nhấn → CÓ/KHÔNG khôi phục; giữ = quay lại. Khôi phục thành công cũng lưu cấu hình bị thay thế thành bản ghi mới nhất (hoàn tác được).
* Web không xem/xuất/xóa/khôi phục được.

## 6. PID Monitor

HMI (chỉ đọc, 4 dòng/trang, xem được khi đang ấp) và Web (đầy đủ + 2 đồ thị: nhiệt độ/SP, công suất yêu cầu/SSR thực tế + dải quạt hút).
Số liệu: PV, SP, sai số, hiệu chỉnh PID (P+I+D, đã loại FF), Hold FF, Vent FF, giới hạn Adaptive, yêu cầu cuối, SSR thực tế TB 60 s, Kp/Ki/Kd, Profile Confidence, Heater Gain/Delay, Coast, Hold Power, Vent Gain.
Không giả lập: thiếu dữ liệu hiển thị `—`. Gói MQTT chỉ kèm `pidMon` (~200 B) khi một tab Web đang mở mục này (cờ `mon` trong lease).

## 7. An toàn vận hành

Không đổi High/Emergency, E115, OutputArbiter, bảo vệ độc lập, thuật toán Adaptive Thermal V1/Smart AutoTune (chỉ thêm telemetry chỉ đọc và một bản ghi hoàn tác trước khi lưu kết quả tune).
HMI/Web/EEPROM không chạy trong vòng nhiệt ngoài các lần ghi cấu hình vốn có (thêm tối đa một ô lịch sử khi áp dụng thay đổi kỹ thuật). `MachineRuntime` chỉ mang số vô hướng; dữ liệu nặng nằm ở một bản dùng chung (`MayapTech::Detail`) để tiết kiệm RAM.
Mất Wi-Fi/Cloudflare/HMI: yêu cầu Web hết hạn, điều khiển cục bộ tiếp tục.

## 8. Kiểm thử

| Nhóm | Công cụ | Nội dung |
|---|---|---|
| Lõi mã/ring | `tests/tech-access.cpp` | SHA/HMAC vectors, PIN đúng/sai, khóa, phiên, cổng Web, ghi dở, ring 3 ô |
| Controller thật | `tests/tech-controller.cpp` (`tools/test_tech_access.py`) | Hàm cắt trực tiếp từ `machine_control.h`: yêu cầu Web CÓ/KHÔNG/hết giờ/mất mạng, thu hồi, kiểm lại an toàn, không đổi = không ghi, hoàn tác, khôi phục, mất điện giữa lúc ghi |
| Bridge MQTT thật | `tests/runtime-transactions.cpp` | ẨN/khóa/hợp lệ/ngoài giới hạn/bận, PIN chỉ là chữ số, lệnh HMI-only không với tới |
| HMI | `tests/hmi-navigation.test.cjs` | nhóm menu, nhập PIN, nháp + lưu một lần, duyệt yêu cầu Web, bản ghi cũ, PID Monitor |
| Bảo mật nguồn | `tests/tech-access.test.cjs` | không bypass `config/set`, không log PIN, EEPROM không chồng lấn |
| Web | `tests/web-experience.test.cjs`, `tests/protocol-v2.test.cjs` | cổng ẨN/HIỆN, chỉ gửi trường đổi, ngân sách gói, không số liệu giả |

Chưa kiểm trên phần cứng: tốc độ vẽ LCD 128×64 khi xoay nhanh, núm xoay thật, ghi EEPROM thật/mất điện thật, đo stack task, thời gian một lần ghi lịch sử trong vòng điều khiển.

## 9. Kích thước firmware (CI PILOT, run 37887810763)

| | Flash | RAM tĩnh |
|---|---|---|
| Baseline trước nhánh này (sau Smart AutoTune) | 1,451,089 B | 162,120 B |
| Sau HMI/PID management | 1,474,669 B (+23,580) | 163,552 B (+1,432) |

Ngân sách mềm Flash cũ 1,460,000 B bị vượt 14,669 B nên đã nâng lên **1,480,000 B** (còn dư ~5,3 KB); ngân sách RAM 164,000 B giữ nguyên và chỉ còn dư **448 B**.
Giới hạn cứng phân vùng app là 3,342,336 B (Flash dùng ~44 %). Chưa đo dư stack các task trên phần cứng (NOT TESTED); nếu cần thêm tính năng, RAM là ràng buộc chính.
