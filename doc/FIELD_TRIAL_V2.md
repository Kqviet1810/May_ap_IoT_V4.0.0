# Thử lò thật — Thermal V2.0.0 (G7)

Bản thử: firmware CI của commit ghi trong `RELEASE_NOTES_V2_RC.md`. **Mọi kết quả trước đây là mô phỏng.** Mục đích G7: lấy log thật để biết V2 đạt đến đâu và hiệu chỉnh những thứ mô phỏng không biết (hệ số trao đổi nhiệt với trứng, nhiễu cảm biến thật, trễ thật).

## 0. Điều kiện bắt buộc
- Có người đứng cạnh **công tắc tổng** suốt các bước 2–6. Ngưỡng dừng ngay (cắt điện tổng): bất kỳ nhiệt kế tham chiếu độc lập ≥ 38,0 °C; cảm biến SHT30 chênh tham chiếu > 0,5 °C ở trạng thái ổn định; heater bật khi đáng lẽ bị chặn; cảnh báo lạ.
- Nhiệt kế tham chiếu độc lập (≤ ±0,05 °C) đặt cạnh đầu dò SHT30.
- Máy tính nối USB-serial để ghi log.
- Lò **chưa có trứng** ở các bước 2–5.

## 1. Kiểm không sưởi (bench)
1. Nạp firmware. Vào HMI → Nâng cao → "DPG cam bien": chọn **0.01 C** (nếu báo lỗi cảm biến thì đặt lại TU DONG và cho tôi biết: mô-đun xuất 0,1).
2. Ngắt cầu dao heater (hoặc rút dây SSR/contactor). Bắt đầu mẻ AUTO, SP 37,5.
3. Kiểm: PID Monitor hiện "Pha hoc V2" = NHIN/DO rồi DA BIET; "Yeu cau cuoi" tăng khi PV < SP nhưng heater không có điện; E115 sẽ nổ sau khoảng 15 phút (đúng thiết kế).
4. Hơ nhẹ đầu dò (máy sấy tóc) tới vượt ngưỡng High/vent: kiểm quạt hút bật, báo cảnh báo; không cần tới Emergency trên thiết bị thật.

## 2. Lò rỗng, trần 25 %, SP 33 °C
Đặt công suất tối đa (maxHeaterPower) = 25 %, SP = 33 °C. Bật heater, bắt đầu mẻ. Ghi log: trong USB-serial gõ `LOG AI` (một dòng CSV mỗi giây, cột V2 xem `doc/SERIAL_LOG.md`, schema 2). Chạy 90 phút. Gửi tôi toàn bộ log + ảnh PID Monitor + số đọc nhiệt kế tham chiếu mỗi 10 phút.
Đạt: không vượt SP + 0,5 °C; "Pha hoc V2" lên DA HOC 1 sau khoảng 10–15 phút và DA HOC 2 sau khoảng 35 phút.

## 3. Nâng trần: 60 %, rồi 100 %
Lặp lại bước 2 với trần 60 % rồi 100 % (SP 33). Chú ý "Slot V2 us" (lớn nhất) trên PID Monitor: ghi lại; nếu > 3000 us báo tôi.

## 4. SP 37,5, lò rỗng, trần 100 %
Lên từ nguội tới 37,5. Đo: thời gian chạm SP − 0,3, vọt cực đại, thời gian vào ±0,1 °C. Chạy 6 giờ. Gửi log.

## 5. Nhiễu: mở cửa
Khi đã ổn định 1 giờ: mở cửa 30 s. Ghi PV và nhiệt kế tham chiếu 15 phút. Mục tiêu mô phỏng: về ±0,3 °C trong khoảng 4–6 phút.

## 6. Có trứng (chỉ khi các bước trên sạch)
Nạp một xe trứng. Chạy 24 giờ, ghi `LOG AI`. Với lô đầu, người trực lò cạnh công tắc tổng trong 2 giờ đầu.

## 7. Gửi cho tôi
CSV `LOG AI`, ảnh PID Monitor (9 mục), số đọc tham chiếu, ghi chú bất thường (cảnh báo, âm thanh còi, vent). Từ đó tôi sửa `ha`/`τe`, ngưỡng bộ quan sát trứng, độ nhiễu thật.

## 8. Lùi lại
Nếu V2 kẹt hoặc lạ: cắt công tắc tổng, nạp lại firmware 1.1.15 (còn nguyên ở `main`).
