# Kế hoạch nghiệm thu (commissioning) — lò TRỐNG, Smart Thermal

**Tài liệu kế hoạch. Chưa có bước nào được thực hiện trên máy thật.** Mọi con số "đạt" trong mô phỏng chỉ là điều kiện để *được phép*
bắt đầu các phép thử dưới đây, không thay thế chúng. Người thực hiện: kỹ thuật viên điện có chuyên môn + người giám sát nhiệt; luôn có người
đứng cạnh công tắc HEATER trong suốt các bước có cấp nhiệt. Không đặt trứng vào lò cho tới khi Giai đoạn 6 đạt.

Cờ firmware dùng trong tài liệu này: `MAYAP_SMART_THERMAL` (mặc định 0). Mọi phép thử Giai đoạn 0–3 chạy với cờ **= 0** (hành vi hiện hành); cờ
chỉ được bật ở Giai đoạn 4 trở đi, trên một bản build ghi rõ SHA, và quay lại 0 là phương án lui (rollback) duy nhất cần thiết.

## Giai đoạn 0 — Điều kiện tiên quyết (không cấp nhiệt)
| # | Kiểm tra | Đạt khi |
|---|---|---|
| 0.1 | Chuỗi công suất theo `doc/SAFETY_HARDWARE_REQUIREMENTS.md`: cầu chì nhiệt → thermostat cơ độc lập (cảm biến riêng, cắt thẳng cuộn contactor) → contactor an toàn → SSR → điện trở | đúng thứ tự, đo thông mạch, ghi ảnh + số hiệu |
| 0.2 | Thermostat cơ độc lập: ngưỡng cắt đặt và kiểm chứng bằng nguồn nhiệt ngoài + nhiệt kế chuẩn | cắt ở ≤ ngưỡng ghi trong hồ sơ; **thấp hơn Emergency 39.0 °C** |
| 0.3 | Đo điện: cách điện, tiếp địa, dòng từng nhóm điện trở (8 điện trở / 16 kW, 2 SSR chung GPIO1), cân pha | trong dung sai thiết kế; ghi dòng thực |
| 0.4 | Cảm biến tham chiếu: ≥ 2 đầu dò chuẩn đã hiệu chuẩn (chứng chỉ còn hạn) + data logger, đặt cạnh đầu dò SHT30 và ở ≥ 6 vị trí (góc trên/dưới, giữa khay, gần cửa, gần quạt, gần điện trở) | sai lệch tham chiếu ≤ 0.05 °C |
| 0.5 | Offset SHT30 do người vận hành hiệu chuẩn bằng nhiệt kế chuẩn (firmware không tự chỉnh offset) | ghi offset; offset âm không được hạ nhiệt độ an toàn (đã có trong mô phỏng; kiểm lại bằng hiển thị) |
| 0.6 | Build firmware: `arduino-cli compile` (core esp32 3.3.11, cờ như `build-firmware.yml`) và đọc **Flash / RAM tĩnh** so với ngưỡng CI (Flash ≤ 1 480 000 B, RAM ≤ 164 000 B) với cờ 0 và cờ 1 | cả hai trong ngưỡng; chênh lệch ghi lại. (Chưa đo được trong môi trường mô phỏng: máy build không tải được core ESP32) |
| 0.7 | Chạy lại bộ kiểm thử host trên đúng SHA sẽ nạp: `tools/test_thermal_control.py --sanitize`, `test_thermal_adaptive.py --full`, `test_smart_autotune.py`, `test_thermal_sensor_path.py --sanitize`, `ab_smart_thermal.py` | tất cả xanh; lưu log |

## Giai đoạn 1 — Bảo vệ và an toàn (cấp nhiệt tối đa 10 % công suất, có người trực, nắp đóng)
Thực hiện từng phép thử trong mục "Kiểm thử nghiệm thu máy thật" của `doc/SAFETY_HARDWARE_REQUIREMENTS.md` **và** các phép thử bổ sung rút ra từ
bộ `thermal-sensor-path` (các giới hạn của một cảm biến duy nhất):
| # | Phép thử | Kết quả mong đợi |
|---|---|---|
| 1.1 | Rút/chập/mở mạch cảm biến SHT30 | SSR OFF + contactor nhả ≤ 1 chu kỳ điều khiển; quạt theo logic hiện có |
| 1.2 | Cảm biến kẹt thấp (giả lập bằng điện trở/đầu giả) khi heater đang bật | **thermostat cơ phải cắt**; ghi nhiệt độ buồng đo bằng tham chiếu ở thời điểm cắt. (Mô phỏng: firmware chỉ dừng nhờ E115/E104 sau 15–20 phút, nhiệt buồng có thể lên 41–78 °C → bảo vệ cơ là lớp duy nhất) |
| 1.3 | SSR dính dẫn (đấu giả lập: nối tắt SSR) | contactor nhả khi chạm High; **ghi số lần contactor đóng/nhả** (mô phỏng: High tái kích hoạt 10–25 lần, buồng 40–60 phút trên High) → xem quyết định D3 trong `FINAL_REPORT.md` |
| 1.4 | SSR **và** contactor cùng dính | thermostat cơ / cầu chì nhiệt cắt nguồn heater (firmware không thể) |
| 1.5 | Ép nhiệt vượt High (nguồn nhiệt ngoài lên đầu dò) | contactor nhả, hai quạt chạy; clear sau trễ 10 s / 0.2 °C |
| 1.6 | Treo/reset ESP32 giữa lúc đóng/nhả contactor, mất điện nhiều lần, reset ngay lúc ghi EEPROM/NVS | đầu ra về an toàn; không heater ON khi khởi động lại; hồ sơ A/B+CRC khôi phục |
| 1.7 | Mất Wi-Fi/MQTT/Cloudflare, Wi-Fi reconnect liên tục | vòng nhiệt không đổi (đo jitter chu kỳ điều khiển) |
| 1.8 | Đo thời gian chu kỳ điều khiển trên chip (5 ms), stack high-water, heap tối thiểu, largest free block, loop latency khi HMI/Wi-Fi bị kích | số đo thật trên ESP32-S3; mô phỏng host không thay được |

## Giai đoạn 2 — Đặc tính nhiệt lò trống (công suất giới hạn dưới giám sát)
| # | Phép đo | Cách làm | Ghi lại |
|---|---|---|---|
| 2.1 | Gia nhiệt bậc thang | từ nhiệt độ phòng, SSR 10 % → 20 % → 30 % (giới hạn `maxHeaterPower`), mỗi bậc đến khi tốc độ tăng ổn định | độ trễ chết, hằng số thời gian, **hệ số tăng nhiệt Kh (°C/s ở 100 %)**, so với `Kh` mà learner ước lượng |
| 2.2 | Nhiệt dư (coast) | cắt heater ở nhiều mức nhiệt/duty khác nhau | `coast rise`, thời gian đạt đỉnh, so với mô hình 0.12 °C/ON-giây của startup legacy |
| 2.3 | Tổn thất | giữ SP, đo duty cân bằng ở 3 nhiệt độ phòng | `hold %`; kiểm tra tỉ lệ duty ≤ 30 % (nếu > 30 %: lò thuộc lớp POWER_LIMITED) |
| 2.4 | Tuần hoàn | tắt/bật quạt tuần hoàn và đo đồng đều nhiệt (6 vị trí) | chênh lệch không gian; ảnh hưởng của quạt lên nhiệt đầu dò |
| 2.5 | Thông gió | một sự kiện quạt hút ở mỗi mức (nếu có thể điều chỉnh) trên lò trống | độ sụt, thời gian hồi, `ventCoolingGain`; thử cả khi nhiệt phòng > SP nếu điều kiện cho phép |
| 2.6 | Đối chiếu mô phỏng | nhập các số đo vào mô hình `Plant` (eff/capacity/loss/dead/lag) và chạy lại `ab_smart_thermal.py` cho đúng lò | lò thuộc lớp REACHABLE / SAFETY_LIMITED / HEAT_LIMITED nào; có nằm trong miền đã mô phỏng không (eff 0.5–2.0, 180 kJ/°C – 1.6 MJ/°C, dead 0–120 s) |

Dừng và báo ngay nếu: bất kỳ tham chiếu nào vượt 38.2 °C, hoặc Kh đo được > 0.12 °C/s (lò mạnh hơn dự trù 35 % của startup legacy → đây chính là miền
mà `MAYAP_SMART_STARTUP` bảo vệ; chỉ khi đó mới có lý do bật cờ), hoặc nhiệt tham chiếu lệch đầu dò SHT30 > 0.3 °C ở trạng thái ổn định.

## Giai đoạn 3 — Smart AutoTune trên lò trống (đã được phê duyệt riêng)
Làm theo "First AutoTune on a real oven" trong `doc/SMART_AUTOTUNE_V1.md` (lò trống, nguội, nắp đóng, có người trực, nhiệt kế tham chiếu cạnh đầu dò).
Đạt khi: kết quả `[TUNE-VALIDATE] RESULT=PASS|FAIL` ghi lại đủ `res/delay/coast/period/horizon/hold/why`; **FAIL không đổi PID/hồ sơ cũ**; sau SUCCESS giữ SP 37.5 °C ≥ 2 giờ với
log PV, tham chiếu, duty, profile (gain/delay/hold/confidence/state).

## Giai đoạn 4 — Bật Smart Thermal (build cờ = 1), lò trống, SP 37.5 °C
| # | Phép thử | Tiêu chí đạt (so sánh trực tiếp với cờ = 0 trên cùng lò) |
|---|---|---|
| 4.1 | Khởi động nguội ×3 lần | quá nhiệt tham chiếu (không phải đầu dò) ≤ 0.3 °C; **không** chạm High; thời gian settle ghi lại |
| 4.2 | Khởi động nóng (lò còn ấm 33–36 °C) ×2 | như trên |
| 4.3 | Giữ ổn định 12 giờ | MAE tham chiếu ≤ 0.1 °C (oracle gốc: MAE 0.10, P95 0.15, ripple 0.25), **không có `THERMAL_MODEL_MISMATCH` giả**, không E115, số lần ghi flash của profile ghi lại |
| 4.4 | Sự kiện quạt hút (từng mức) và mở cửa 30 s | độ sụt và hồi phục không tệ hơn cờ = 0 |
| 4.5 | Mất điện 10–60 s giữa chừng ×3, reset giữa lúc ghi profile | khởi động lại về PID cơ bản + profile (nếu hợp lệ) làm hạt giống; không overshoot lớn hơn cờ = 0 |
| 4.6 | Đổi SP 37.5 → 37.0 → 37.5 | set point thay đổi được kiểm soát, không giật công suất |
| 4.7 | Thay đổi phần cứng có kiểm soát (giảm 1 nhóm điện trở nếu có thể) | learner báo mismatch trong thời gian chấp nhận được; nhiệt vẫn trong dải |
Điều kiện cho phép *tăng* `maxHeaterPower` hoặc dùng các chu trình theo mẻ: chỉ sau khi 4.1–4.5 đạt trên **ít nhất 2 lần chạy độc lập** và có chữ ký người phụ trách an toàn.

## Giai đoạn 5 — Chỉ khi đã có quy trình ấp được duyệt
Chương trình nhiệt theo ngày (`thermal_program.h`) và làm mát trứng định kỳ (`egg_cooling.h`) **không** nằm trong firmware hiện tại. Trước khi tích hợp cần: quy trình sinh học
được duyệt theo loài, đầu dò đặt trong/gần trứng để đo nhiệt trứng (không chỉ nhiệt không khí), phê duyệt riêng cho kế hoạch tích hợp ở mục 4 của `PHASE6_INCUBATION_PROGRAM.md`.

## Giai đoạn 6 — Chấp nhận để đặt trứng thử
Tất cả giai đoạn 0–4 đạt, báo cáo đối chiếu mô phỏng ↔ máy thật (2.6) được ký, bảo vệ cơ độc lập đã được kiểm chứng lại sau lần cuối thay đổi điện, và có phương án dừng (HEATER OFF)
trong tầm tay trong 72 giờ đầu của mẻ thử. **72 giờ ở đây là thời gian thật trên máy; 72 giờ mô phỏng trong báo cáo không có giá trị thay thế.**
