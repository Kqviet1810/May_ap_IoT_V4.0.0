# Điều khiển nhiệt V3 — bản 2.5.0: quạt hút không cắt heater, luật quạt bảo vệ relay

Hai log ngày 30/10 (bản 2.4.0 sáng, 2.4.1+ chiều) cho thấy hai lỗi khác nhau, và một giả định sai về cái quạt.

## 1. Nguyên nhân (đọc từ log và mã, có thể kiểm lại)

| # | Hiện tượng | Nguyên nhân |
|---|---|---|
| 1 | 30/10 chiều: PV chạm 37,6 (một bậc cảm biến), heater bị cắt về 0 trong 2 phút (14:13:55–14:15:54), PV tụt 37,3 và nằm đó 20 phút. | 2.4.1 chặn heater (`ventHeaterBlock_`) suốt thời gian đường quạt hoạt động và trừ `pHold` khi quạt chạy mà PV không giảm. Ngưỡng bật quạt (SV+0,05) nhỏ hơn nửa bậc cảm biến nên một cú nhích 0,05 °C thật là đủ bật. Cú cắt hai phút đến cảm biến sau độ trễ truyền nhiệt, nên PV còn tụt tiếp sau khi heater đã chạy lại. |
| 2 | Hai quạt 45 W cánh 300 mm. | Không thể coi quạt là cơ cấu hạ nhiệt: trong log, khi quạt chạy 3–4 phút mà heater giữ nguyên thì PV không đổi (14:07 và 04:28 ngày 30/10). Phần cắt heater mới hạ được PV. Còn bù trước +12 % khi quạt chạy (V3 gốc) là bơm thêm nhiệt đúng lúc đang quá nhiệt. |
| 3 | Relay 16 A của quạt. | Luật cũ bật/tắt theo ngưỡng 20 s xác nhận và giữ tối thiểu 60 s: một PV dao động quanh ngưỡng bật tắt quạt nhiều lần. |

## 2. Thay đổi

**Phía máy** (`vent_line.h` mới, `machine_control.h`, `config.h`):

* Đường quạt **không cắt heater**. Heater chỉ bị giữ bởi hàng rào an toàn đã có: PV ≥ max(ngưỡng quạt, SV+0,3), nhả khi thấp hơn 0,1 °C.
* Quạt bật khi PV ở/trên ngưỡng quạt **60 s mà không giảm quá 0,05 °C**, hoặc vượt ngưỡng thêm **0,1 °C trong 20 s**.
* Quạt chạy **tối thiểu 3 phút**, nghỉ **tối thiểu 5 phút**, **tối đa 6 lần mỗi giờ**. High/Emergency ép quạt bằng đường riêng (`ventFanForceOn`) và không bị giới hạn.
* Mã nằm trong một lớp thuần C++ (`VentLine`) mà test chạy đúng mã đó: `tests/vent-line.cpp`.

**Bộ điều khiển** (`thermal_v3.h`): là bộ điều khiển 2.4.0 (đo tải, E115 mốc theo đáy), **bỏ phần 2.4.1** (không cắt lệnh theo quạt, không trừ hold theo quạt, giữ nguyên sàn dự đoán) và **một thay đổi**: luật chống kẹt (Anti-Stuck) chờ PV thấp hơn SP **một bậc cảm biến** thay vì ba bậc. Với ba bậc, PV đứng ở SP−0,2 vừa quá xa cho thành phần P, vừa chưa đủ để luật này chạy (log 30/10: 37,3 trong 20 phút).

**Phiên bản**: 2.5.0 (`config.h`, `README.md`, `release-manifest.json`); `tests/runtime-preservation.json` đã ghim lại có ghi chú xem xét.

## 3. Kiểm chứng

Mô phỏng 210 lò ngẫu nhiên có kiểm soát (trễ truyền 41–195 s, hold 2–24 %, công suất 14–22 kW, có lò chậm và trễ cảm biến) × 6 tình huống. Công cụ: `tools/thermal-research/fleet-bench.cpp` (xem đầu file). Quạt yếu (100 W), V4 = 2.4.2.

| | V4 (2.4.2) | 2.5.0 |
|---|---|---|
| Tổng sai lệch tích lũy 6 tình huống (trung vị) | 180 | **119** |
| Số lò thắng V4 | | **100 %** |
| Tỷ lệ đạt trung bình (tiêu chí bên dưới) | 36 % | **93 %** |
| Lên nhiệt lần đầu, trung vị / p90 | 31 / 40 phút | 31 / 40 phút |
| Vọt lần đầu ≤ +0,5 °C | 100 % | 100 % |
| Lò treo ở SP+0,1 quá 10 phút sau một cú vượt | 3 | 1 |
| ±0,2 °C liên tục trong 30 phút | | 69/210 lò (33 %) |
| ±0,2 °C liên tục trong 45 phút | | 140/210 lò (67 %) |
| Quạt bật: trung vị / tối đa (250 phút) | | 4 / 10 |

Tiêu chí "đạt": trở lại ±0,1 °C trong 40 phút (quạt lịch, cú vượt, khởi động lại), 60 phút (mở cửa 3 phút), 90 phút (khay lạnh); với quạt, cú vượt và khởi động lại PV không rơi quá 1 °C.

Theo tình huống (đạt %, V4 → 2.5.0): lên nhiệt rồi giữ 61 → 97; quạt lịch 33 → 95; vượt 1 bậc 42 → 85; mở cửa 3 phút 24 → 94; cửa + khay lạnh 8 → 93; khởi động lại 49 → 92.

Các bộ test đã có, **không nới**: `tests/thermal-v3.cpp`, `tests/thermal-closed-loop.cpp` (kể cả lò khớp log 29/10), `tests/thermal-replay.cpp`, `tests/heater-response-watch.cpp`, `tools/test_heater_path.py`, `tests/runtime-recovery.test.cjs`.

Luật quạt chạy lại trên PV thật của bảy log (24/10, 27/10 ×3, 29/10, 30/10 ×2) cho số lần bật bằng hoặc ít hơn các lần bật đã ghi (ví dụ 27/10 chiều: 3 so với 5; 29/10: 0 so với 2) và thời gian chạy ngắn hơn. Đây là so sánh trên PV đã ghi, không phải mô phỏng vòng kín.

## 4. Giới hạn (nói thẳng)

* **30 phút ổn định không đạt cho mọi lò.** Với quạt yếu, 33 % lò ổn định ±0,2 °C trong 30 phút, 67 % trong 45 phút; với quạt mạnh còn 8 % và 30 %. Lò trong mô phỏng chạm SP (không khí) trung vị 31 phút; riêng trứng chạm 37,2 °C với heater 100 % liên tục cần trung vị 27 phút. Phần dư địa còn lại (không khí có thể chạm SP trong khoảng 10 phút) chưa được dùng; cần một thay đổi riêng ở pha lên nhiệt.
* Hai luật nhỏ đã thử trên nền 2.4.0 và **không giữ** vì không thêm lợi ích đo được: mở rộng băng tích phân (±1,0 / 1,5 / 2,0: 69 → 71 / 72 / 67 lò) và trừ hold khi quá SP (số lò treo đã là 0).
* Lò mô phỏng không phải lò thật; bộ lò ngẫu nhiên chỉ có hold 2–24 %, còn máy thật giữ khoảng 30 %. Hiệu quả của quạt thật chưa đo được (log chỉ cho một cận trên khoảng 1–5 % công suất heater).
* Cú vượt do quán tính lớn (hold 2–4 %, trễ >150 s) không controller nào hạ được bằng cách cắt heater; chỉ phòng ngừa được.
* Bộ nhớ học (`pHold`, kAir, τ) vẫn mất khi khởi động lại; sau khởi động lại giữa mẻ bộ điều khiển học lại.
* **Bằng chứng mô phỏng không phải bằng chứng phần cứng**: log PID của mẻ kế tiếp là bài nghiệm thu thật.
