# Thermal V2 — G7: sửa theo log lò thật (23/10/2026)

Firmware 2.0.1. **Chỉ kiểm chứng trong mô phỏng + CI; chưa thử lò thật.** Mọi số dưới đây là mô phỏng.

## 1. Log lò thật nói gì (lò 20 kW, xe đầy, chưa trứng, cảm biến 0,01 °C)

| Quan sát | Số đo |
|---|---|
| Lên nhiệt, cửa đóng | cắt ở 37,64 → đỉnh **37,90** (+0,40); dự đoán lúc đó 37,44 |
| Mở cửa 5 phút rồi đóng | cắt ở 37,60 → đỉnh **38,20** (+0,70); dự đoán lúc đó 37,52 |
| Trôi tự nhiên (heater 0 %, quạt hút bật) | −0,008 °C/phút |
| Độ chững khi sưởi | dài nhất 80 s, thường 20–30 s |
| Trễ lò | khoảng 100–110 s |
| So dự đoán với PV thật (3 phút) | không lệch; (5 phút) hụt +0,4…+0,5 °C |
| Bị khoá | quạt hút bật 31 phút → V2 đứng yên, `pred_c` đóng băng |

Nguyên nhân (từ log + mã): (1) dự đoán chỉ dùng mô hình, mức nhu cầu của mô hình cao hơn thực và hụt nhiệt tồn; (2) tích phân nạp khi cửa mở, xả chậm; (3) "far" nhấp nháy 20 ↔ 100 %; (4) cắt công suất do lớp quạt hút ở SP + 0,1, không phải do V2; (5) V2 bị khoá khi quạt hút bật.

## 2. Lò số mở rộng (tập E, chỉ để phát triển, không phải held-out)

`tools/thermal-v2/plants.json` → `E` (36 lò, hạt 4343, khoảng RỘNG, không khớp riêng lò nào): phần công suất đến chậm (nhiệt tồn của phần tử sưởi, 0–40 %, 60–240 s), trễ cảm biến 0–90 s. Kịch bản mới `S11a/b/c`: cửa giữ mở 2/5/10 phút = tổn thất kéo dài 1 kW/K. Tiêu chí mới `R2.over.door` (≤ 0,3 °C). Lò A9/A10 của tập A (20 kW, 1,2 MJ/K) cho cùng vọt +0,4 như log thật.

Trên bộ điều khiển cũ (V2.0.0), tập E: **High 46/504**, Emergency 2/504, vọt khi lên nhiệt trung vị +0,35 °C, p90 +0,59.

## 3. Các thay đổi (mỗi cái sau một công tắc `V2_Fn`, mặc định bật; xem `thermal_v2.h`)

| | Việc | Ghi chú |
|---|---|---|
| F1 | Dự đoán đỉnh = max(mô hình, **cân bằng năng lượng với nhu cầu đo được**: `D_meas = P_đến − slope/k`) | trừ biên nhiễu của độ dốc |
| F2 | Học nhiệt tồn ở **mỗi lần cắt công suất** (thực/ dự kiến), hệ số kẹp 0,3–2,0 | `coastGain` |
| F3 | **Phanh**: lần tiếp cận đầu, khi đỉnh dự đoán ≥ SP−0,2 cắt 0 % tới khi qua đỉnh (cửa sổ = 1,5×trễ + kiên nhẫn, ≤ 300 s) | **chi phí: lên nhiệt chậm thêm 20–35 %** |
| F4 | Tích phân rò khi đỉnh > SP; mức mô hình hạ ngay (cổng 25 % nới xuống), tăng chậm 0,1× khi có dấu hiệu nhiễu | **bỏ** "học sai lệch lặp lại" (làm cửa tệ hơn) |
| F5 | "Far" theo độ hụt làm mượt, dwell 30 s, ngưỡng thoát rộng | hết nhấp nháy 20↔100 % |
| F6 | **Giới hạn đuổi**: khuyết sâu ≥ 0,6 °C mà PV phẳng/giảm đều và công suất đến đã ≥ hold + 15 % → u ≤ min(P_đến + 8, hold + phần thừa) | nhường chỗ khi E115 > 20 % |
| F7 | **Chạy ngầm** khi quạt hút (ngưỡng nhiệt) chặn sưởi: mô hình + neo mức vẫn chạy, lệnh = 0, tích phân giữ | adapter giữ nhịp 100 ms |
| F8 | Giới hạn dưới SP: u ≤ hold + khoảng còn lại tới SP+0,1 / (k·60 s) | chỉ khi còn ≥ 0,3 °C dưới SP; **bỏ** luật "không tăng khi trên SP" (gây dao động) |
| F9 | Kiên nhẫn đo từ đường lên; thăm dò dùng trễ ĐÃ ĐO | ảnh hưởng nhỏ |

## 4. Kết quả mô phỏng (cũ → mới, số ca đạt)

| Tiêu chí | A (12 lò) | D (60) | E (36) | B held-out (60) | C robust (21) |
|---|---|---|---|---|---|
| High (an toàn) | 167→**168**/168 | 557→**560**/560 | 458→494/504 | 825→**840**/840 | 273→293/294 |
| Emergency | 168→168 | 560→560 | 502→**504** | 836→**840** | 293→**294** |
| Vọt ≤ 0,2 | 74→**82**/84 | 228→**261**/280 | 147→205/252 | 330→**390**/420 | 114→130/147 |
| Vọt sau cửa ≤ 0,3 | 34→33/36 | 113→119/120 | 42→65/108 | 154→**170**/180 | 51→55/63 |
| Lên nhiệt (R1.reach) | 24→**11**/24 | 80→**35**/80 | 72→59/72 | 120→**59**/120 | 42→**14**/42 |
| Về tới SP (settle ±0,1) | 11→8/12 | 38→32/40 | 23→31/36 | 49→46/60 | 13→15/21 |
| Hồi sau cửa tức thì (R5.door) | 12→**9**/12 | 34→28/40 | 16→27/36 | 51→43/60 | 13→13/19 |
| Duy trì ±0,1 (R4.tail) | 12→10/12 | 40→36/40 | 29→35/36 | 58→49/60 | 20→16/21 |
| Khay lạnh (R5.tray) | 24→24 | 80→80 | 72→71/72 | 120→120 | 42→42 |
| Lên nhiệt lại sau khởi động | 0→0 | 0→0 | 0→0 | 0→0 | 0→0 |

Tập E, ca "cửa 5 phút" (S11b): vọt trung vị **+0,40 → +0,14**, p90 +0,98 → +0,46, số ca High **9 → 0 / 36**. Lên nhiệt (S1): vọt trung vị +0,35 → +0,12, p90 +0,59 → +0,19.

**Cái giá**: lên nhiệt lần đầu chậm hơn khoảng **20–35 %** (thêm 4–8 phút trên khoảng 20 phút) và hồi sau cú giảm tức thì chậm hơn; còn 10 phút cửa mở (S11c) vẫn có High ở ~5/36 lò khó. Nguyên nhân vật lý: năng lượng đã ra lệnh đến sau độ trễ; khi cửa đóng (cảm biến biết muộn 40–140 s) phần nhiệt đã cam kết vẫn tới. Chỉ giảm được bằng cách không đuổi khuyết, không xoá được.

## 5. Chưa làm / bỏ có lý do

* "Học từ sai lệch lặp lại ở cổng 25 %" (F4B): thử, làm cửa tệ hơn → tắt (`V2_F4B=0`).
* "Làm sạch mức sau phanh" (`levelClean`): đặt mức sai khi mô hình trễ sai → tắt.
* Khởi động lại giữa mẻ vẫn học lại từ đầu (R1.reach.restart 0 %): xem `RELEASE_NOTES_V2_RC.md`.
* Chưa so lại log lò thật qua bộ điều khiển (replay): log chỉ có PV, yêu cầu, SSR trung bình; công cụ chưa viết.

## 6. Công tắc thử nghiệm (chỉ máy chủ)

`V2_CXXFLAGS="-DV2_F3=0 ..." python3 tools/thermal-v2/run_grid.py grid --ctl v2 --sets E ...`. Firmware không định nghĩa các macro này.
