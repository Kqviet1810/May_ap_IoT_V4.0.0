# Quy trình nghiệm thu lò thật MAYAP V4 (duy nhất)

Áp dụng cho **một** Release Candidate xác định bằng SHA. Mọi bước dùng firmware sinh từ đúng SHA đó (đối chiếu SHA-256 từ `SHA256SUMS.txt` của artifact và `MAYAP_BUILD_ID` in khi khởi động).
Người thực hiện: kỹ thuật viên điện có chuyên môn + người giám sát nhiệt; luôn có người đứng cạnh công tắc HEATER trong mọi bước có cấp nhiệt.
**Khi một phép thử an toàn FAIL: dừng nghiệm thu, HEATER OFF, không tăng công suất, báo người phụ trách.** Không có ngoại lệ.
Cấm: cố ý nối tắt SSR, contactor, thermostat hay bất kỳ cơ cấu bảo vệ nào; cấm thử lỗi công suất cao. Nếu cần kiểm một lớp bảo vệ, kiểm riêng thiết bị đó với tải heater cô lập.
Chi tiết đo không nhiệt: `audit/smart-thermal/phase9/ESP32_NO_HEAT_CHECKLIST.md`. Danh sách chặn xuất xưởng: `PRODUCTION_RELEASE_READINESS.md` mục 7.

## Dụng cụ và hồ sơ
Đầu dò tham chiếu độc lập ≥ 2 (chứng chỉ còn hạn, sai số ≤ 0.05 °C) + data logger, cạnh SHT30 và ≥ 6 vị trí trong buồng; máy phân tích logic (GPIO1 SSR, GPIO14 master, GPIO13 quạt hút, GPIO21 quạt tuần hoàn); đồng hồ điện áp/clamp đúng cấp an toàn; USB-serial ghi log (bản PILOT); biên bản mẫu có chỗ ký ở mỗi bước.
Biên bản ghi: SHA RC, SHA-256 file nạp, cờ `MAYAP_SMART_THERMAL`, `MAYAP_BUILD_ID`, ngày, người làm, người giám sát.

## Bước 1 — An toàn điện và lớp ngắt nhiệt độc lập (kỹ thuật viên có chuyên môn)
1. Chuỗi công suất đúng thứ tự: cầu chì nhiệt → thermostat cơ độc lập (cảm biến riêng, cắt thẳng cuộn contactor) → contactor an toàn → SSR → điện trở. Đo thông mạch, ảnh, số hiệu.
2. Cách điện, tiếp địa, dòng từng nhóm điện trở, cân pha — ghi số đo.
3. **Kiểm chứng riêng thiết bị ngắt quá nhiệt độc lập**, tải heater cô lập: đưa cảm biến thermostat vào bể/nguồn nhiệt có nhiệt kế chuẩn, ghi nhiệt độ cắt, xác nhận cuộn contactor nhả (đo bằng đồng hồ). Ngưỡng cắt phải thấp hơn Emergency 39.0 °C và ghi vào hồ sơ. Cầu chì nhiệt: kiểm sự hiện diện và định mức, không thử phá hủy.
4. Khả năng ngắt công suất heater thật: với nguồn heater cấp có điều kiện và tải được nối qua đồng hồ, xác nhận điện áp/dòng ở đầu tải bằng 0 khi contactor nhả. Việc này không dùng lỗi cố ý.
**Đạt khi** mọi số đo đúng thiết kế và ngưỡng cắt độc lập đã được kiểm chứng. FAIL → dừng.

## Bước 2 — Cô lập heater, trạng thái đầu ra, cảm biến, watchdog (không nhiệt)
1. Nguồn heater cách ly có khóa/thẻ; **kiểm chứng không còn điện bằng thiết bị đo** (thử đồng hồ trên nguồn đã biết có điện, đo không điện, thử lại), clamp 0 A, nhấc dây tải khỏi SSR.
2. Chạy toàn bộ `ESP32_NO_HEAT_CHECKLIST.md` với **hai bản** cùng SHA: cờ 0 (PILOT) và cờ 1 (artifact `SMART-ON-TEST-ONLY`, chỉ khi tải cô lập; không có nghĩa cho phép Smart ON điều khiển heater).
3. Đạt khi: chu kỳ 5 ms/jitter thật, stack, heap, TWDT, mất điện ngẫu nhiên, trạng thái chân khi boot, đường ngắt High và Emergency đều đạt tiêu chí đã duyệt; heater không bao giờ lên ON trái inhibit.
**FAIL ở bất kỳ mục → dừng; chưa được sang bước 3.**

## Bước 3 — Gia nhiệt lò trống có giám sát, công suất giới hạn (bản cờ 0)
Điều kiện: Bước 1–2 đạt và ký. Lò trống, nắp đóng, không trứng, đầu dò tham chiếu đặt, giới hạn dừng ghi rõ.
1. Đặt `maxHeaterPower` ban đầu 10 %, SP thấp (ví dụ 30 °C). Cấp nguồn heater, bật HEATER dưới giám sát.
2. Tăng bậc 10 → 20 → 30 % chỉ khi bậc trước ổn định và mọi tham chiếu < 37.0 °C.
3. **Giới hạn dừng (HEATER OFF ngay):** bất kỳ tham chiếu ≥ 38.0 °C; chênh SHT30 so tham chiếu > 0.5 °C ở trạng thái ổn định; heater ON khi đáng lẽ inhibit; bất kỳ lỗi/báo động bất thường; tốc độ tăng nhiệt vượt dự kiến của bậc công suất.
**Đạt khi** không chạm giới hạn dừng và các đầu ra đúng như thiết kế.

## Bước 4 — Đặc tính nhiệt thật
Ghi: Kh (°C/s ở 100 %, từ bậc thang công suất), dead time, coast rise sau khi cắt, duty giữ nhiệt (hold) ở SP, phân bố nhiệt tại ≥ 6 vị trí (kể cả có/không quạt tuần hoàn), so sánh với `Kh`/delay/hold mà learner ước lượng.
**Dừng và báo** nếu Kh đo được > 0.12 °C/s (miền mà mô phỏng thấy High/Emergency; xem readiness mục 5) hoặc nhiệt tham chiếu lệch SHT30 > 0.3 °C ở trạng thái ổn định.
Đối chiếu số đo vào mô hình mô phỏng (`tools/ab_smart_thermal.py` theo plant đo được) để xếp lò vào lớp REACHABLE / SAFETY_LIMITED / HEAT_LIMITED.

## Bước 5 — Khởi động nguội, khởi động nóng, giữ nhiệt, tác động quạt
Khởi động nguội ×3, khởi động nóng (33–36 °C) ×2, giữ SP 37.5 °C ≥ 2 giờ, sự kiện quạt hút từng mức, mở cửa 30 s, đổi SP 37.5 → 37.0 → 37.5. Ghi overshoot tham chiếu, MAE, thời gian settle. Đạt khi không chạm High và nằm trong oracle gốc (overshoot ≤ 0.30, MAE ≤ 0.10, P95 ≤ 0.15, ripple ≤ 0.25 theo tham chiếu).
AutoTune (Smart AutoTune V1) chỉ chạy khi baseline đạt, lò trống/nguội, nắp đóng, có người trực, theo `doc/SMART_AUTOTUNE_V1.md`; `maxHeaterPower` đặt đúng giới hạn đã duyệt; FAIL không đổi PID/profile cũ.

## Bước 6 — Vận hành khi mất Wi-Fi/MQTT, mất nguồn, khởi động lại
Rút Wi-Fi/ngắt MQTT/Cloudflare: vòng nhiệt không đổi. Mất nguồn điều khiển 10–60 s ×3 khi đang giữ nhiệt và khi đang gia nhiệt: khởi động lại về trạng thái an toàn, **heater không bật ngoài ý muốn**, mẻ/profile khôi phục theo thiết kế. Reset giữa lúc ghi NVS/EEPROM ×≥ 3. Kiểm lại HMI hiển thị đúng lý do reset (`POWER`).

## Bước 7 — Chạy dài hạn lò trống và biên bản
Giữ SP 37.5 °C ≥ 12 giờ rồi ≥ 72 giờ thật (72 giờ mô phỏng không thay thế), có người/cảnh báo từ xa, đầu dò tham chiếu liên tục. Ghi: MAE, không `THERMAL_MODEL_MISMATCH` giả, không E115/E104, số lần ghi profile, heap/stack, số lần đóng cắt contactor. Lập **biên bản nghiệm thu** liên kết SHA RC, SHA-256 file, cờ, số đo và chữ ký từng bước.
Chỉ khi biên bản đạt và bạn chấp thuận: mới đề xuất đặt trứng thử với bản cờ 0, kèm phương án dừng HEATER trong tầm tay 72 giờ đầu.

## Bước 8 — Smart ON (kiểm định riêng, không thuộc nghiệm thu baseline)
Chỉ sau khi Bước 1–7 đạt cho **bản cờ 0** và bạn cho phép bằng văn bản: lặp lại Bước 3–7 với bản cờ 1 sinh từ chính SHA, trên lò trống, công suất giới hạn, so sánh trực tiếp với cờ 0 (4.1–4.7 của `audit/smart-thermal/COMMISSIONING_TEST_PLAN.md`). Smart ON bị chặn nếu còn ACCEPTED_BAD > 0 hoặc có High/Emergency mới. Cho đến lúc đó **không có phép nào cho Smart ON điều khiển heater thật**.

## Ký xác nhận
| Bước | Kết quả (PASS/FAIL/N-A) | Người thực hiện | Người giám sát | Ngày |
|---|---|---|---|---|
| 1 | | | | |
| 2 | | | | |
| 3 | | | | |
| 4 | | | | |
| 5 | | | | |
| 6 | | | | |
| 7 | | | | |
| 8 | | | | |
