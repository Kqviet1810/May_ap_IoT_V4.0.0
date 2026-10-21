# Lò chậm, cách nhiệt tốt: kẹt dưới SP, Adaptive không học, AutoTune hỏng — nguyên nhân, cách sửa, bằng chứng mô phỏng

Ngày 2026-10-15. **Mọi số liệu trong tài liệu này là MÔ PHỎNG** (mã firmware thật chạy trên máy tính, chỉ cái lò là mô hình). Chưa có kiểm định trên lò thật.
"Lò số" của người dùng được suy ra từ nhật ký PID chiều 15/10, **không phải đo trực tiếp**: "đạt" chỉ có nghĩa "nếu lò thật giống mô hình này".

## 1. Hiện tượng đo được trên lò thật

* PV (đầu dò 0,1 °C) đứng ở 37,2–37,4 °C với SP 37,5 °C, dù bật hay tắt Adaptive/Smart; SSR chỉ ~2 % trong khi PID yêu cầu ~10 %. Đặt lại để lên "đứng ở .4, có lúc xuống .3 rồi kéo lên, nhất quyết không lên .5".
* Đặt SP từ 37,3 lên 40: PID yêu cầu 40 % mà 1 phút sau PV vẫn chưa nhúc nhích (trễ đáp ứng lớn).
* Adaptive hiện "Gain 0,030 / Delay 45 s" (giá trị khởi tạo) mãi; Smart AutoTune báo `MODEL_INVALID`, "tune gần như vô dụng".

## 2. Lò số của người dùng (từ số liệu chiều 15/10)

| Thông số | Giá trị | Ghi chú |
|---|---|---|
| Độ lợi Kh | 0,0133–0,0144 °C/s ở 100 % | suy ra từ độ dốc; heater dưới 16 kW |
| Trễ biểu kiến | 200–240 s | ≈ trễ chết 150 s + trễ bậc 1 50 s |
| Công suất giữ SP (hold) | ≈ 1,9 % của dàn 16 kW (~300 W) | phòng 28 °C, SP 37,5 °C |
| Hằng số nguội | ≈ 626 phút | suy ra: τ = (SP − phòng) / (hold × Kh) |
| Đầu dò | 0,1 °C | lượng tử hóa bậc thang |

Cấu hình mẫu: `tools/oven_sim/oven.slow-incubator.json` (chạy bằng `tools/oven_sim.py`, xem `doc/OVEN_SIMULATION.md`).

## 3. Nguyên nhân gốc (bốn cơ chế độc lập trên cùng một "lò")

### 3.1 Phanh khởi động quá mạnh cho lò này, và hold không bao giờ được học → PV kẹt dưới SP

`ThermalStartupController` tính nhiệt "đang bay" từ số giây heater đã BẬT: `0,12 °C` cho mỗi giây BẬT (hằng số của buồng nhẹ 16 kW, `16·1,35/180000`).
Lò này chỉ tăng ~`0,0133 °C` mỗi giây BẬT ở 100 %, tức phanh mạnh gấp **~9–10 lần** thực tế. Trần công suất `cap = 100·(còn lại + 0,05)/0,5` ép duty xuống đúng hold
và PV đứng cách SP khoảng `hold × 240 s × 0,13 °C/s`. Thiết kế gốc chữa bằng *học hold* rồi trừ vào phần "dư", nhưng bộ học chỉ mở khi heater đã TẮT đủ lâu (`coastCleared_`);
ở 2 % duty chuỗi xung không bao giờ để heater tắt đủ lâu → hold không được học → lệch vĩnh viễn. Chỉnh Kp/Ki **không** cứu được (phanh mới là thứ giới hạn).

### 3.2 Mỗi lần đầu dò nhảy 0,1 °C, phanh đóng và tích phân PID bị xóa → PID chạy chỉ-P

Khi số đọc nhảy lên một bậc 0,1 °C, độ dốc ước lượng vọt (0,005 °C/s) và phanh đóng trần về 0 trong 1–3 phút. Giới hạn tích phân của PID tính theo chính trần tạm thời đó
(`integralLimit = maxOut + …`, `maxOut` = trần), nên tích phân đã tích lũy bị cắt về 0 (đã quan sát: 0,586 → 0 ngay mẫu phanh đóng). Vòng lặp chạy "chỉ P", cân bằng ở sai lệch `hold / Kp`
(≈ 1,9 / 12 ≈ 0,15 °C với Kp do AutoTune chọn). **Đã thử sửa trong PID** (giới hạn tích phân theo công suất tối đa cấu hình, hoặc sàn 3–12 %): vòng tune cho kết quả rất tốt,
nhưng với PID mặc định/của người dùng (Kp 35, Ki 0,1) số ca trượt trong ma trận lò chậm tăng từ 1 lên 9–12 / 60 và dao động tăng — việc xóa tích phân hiện đang vô tình làm
"giảm chấn" cho gain mạnh. **Không đổi PID.** Thay vào đó cho tích phân khỏi phải gánh hold (§4, feed-forward từ hold do AutoTune đo).

### 3.3 Adaptive không học / mất hồ sơ

* Chỉ học từ các bước duty ≥ 15 %; ở trạng thái giữ nhiệt không có bước nào → hồ sơ đứng ở giá trị khởi tạo (đúng như màn hình hiện Gain 0,030 / Delay 45 s).
* Bộ phát hiện sai lệch mô hình coi "gia tốc nhanh đã cũ" so với "gia tốc chậm đang trôi" là bằng chứng đổi lò (đã biết từ ca `full165`, chỉ được vá cho Smart) → hạ hồ sơ về `LEARNING` oan.
* Khi Adaptive tự học trên lò này, độ lợi bị thiên thấp (~0,0046 so với thật 0,0133) và trễ bị kẹp ở 160 s (lưới trễ của bộ học). Chưa sửa (§6); đó là lý do hồ sơ nên đến từ AutoTune.

### 3.4 AutoTune hỏng trên lò chậm

| Giới hạn cũ | Hậu quả với lò này |
|---|---|
| lưới trễ ≤ 190 s | trễ 200–260 s bị kẹp ở 190 s → độ lợi thấp hơn thật 5–16 % |
| kích thích ≤ 12 phút, xả nhiệt ≤ 8 phút | heater yếu cần > 20 phút mới thấy 2 °C → `MODEL_INVALID` / `NO_RESPONSE` |
| relay cần 4 chu kỳ trong 80 phút | lò nguội ~10 giờ không thể dao động → `RELAY_FAILED` (21 trong 24 lò thử) |
| kiểm chứng ≤ 150 phút, tổng ≤ 270 phút | chu kỳ tích phân τI = 4(τc+θ) ≈ 2000 s cần cửa sổ lâu hơn |
| hold đo ở cân bằng trong cửa sổ 200 s | một bậc đầu dò 0,1 °C trên 200 s = ±4 điểm duty > hold 1,5–3 % → hold = 0 (24/24 lò thử) |
| không có chẩn đoán | không biết vì sao từ chối |

## 4. Những gì đã sửa (và vì sao an toàn)

| Nơi | Thay đổi | An toàn |
|---|---|---|
| `thermal_control.h` `ThermalStartupController::decide()` | Với lò trễ dài (> 80 s) gần SP mà heater chưa "cleared": đo hold từ duty THỰC trung bình khi PV phẳng ≥ 6 phút trên đường trung bình chậm (95 %, chỉ tăng) + bộ tích phân hold chậm 0,2 %/phút, **chặn trên ở 1,5 × duty đo được + 2 điểm**; hạ hold vẫn nhanh (3 %/phút, tỷ lệ với mức vọt) | chỉ nới phanh qua đúng cơ chế "hold" của thiết kế gốc; High 38,2 / Emergency 39,0, OutputArbiter, contactor không đổi; nhánh cũ giữ nguyên cho lò trễ ngắn |
| `thermal_learner.h` | `detectMismatch()`: "gia tốc nhanh đã cũ" không còn là bằng chứng đổi lò ở mọi chế độ (trước chỉ Smart). Cờ `holdMeasured_` (bật khi AutoTune gieo hồ sơ có hold đo, tắt khi mismatch/degrade/reset) | chỉ làm bộ phát hiện bớt báo oan; thay đổi thật vẫn làm PV lệch và làm mới gia tốc nhanh |
| `thermal_adaptive_v1.h`, `thermal_assist.h` | Feed-forward hold từ **1 %** (thay vì 3 %) **chỉ khi hold là hold do AutoTune đo** (`HoldFfMeasuredMinPct`); hold học trực tuyến vẫn cần ≥ 3 % | bản không phân biệt (1 % cho mọi hold) làm Smart vọt tới High khi mở cửa trong mô phỏng vì gia tốc tự học bị thiên thấp → bị loại |
| `thermal_smart_autotune.h` | lưới trễ → 360 s; giới hạn kích thích/xả động (chỉ khi trễ > 150 s); không dùng relay khi nguội quá chậm (≥ 900 s mỗi chu kỳ, và CHỈ khi hold đo ở bước tiến sát SP đáng tin: phần hiệu chỉnh độ trôi ≤ max(1 điểm, 25 % hold đo) — một lò nặng còn đang nhích lên đọc hold thấp gấp đôi và từng bị bỏ relay oan, xem §5.5) hoặc relay hết giờ → tiếp tục bằng đáp ứng bậc; **hold đo bằng cửa sổ dài theo độ lợi** (tối đa 40 phút, hồi quy bình phương tối thiểu; chỉ dùng khi giá trị đo ở cửa sổ 200 s nhỏ hơn 2,5 × nhiễu lượng tử của đầu dò, tức không có ý nghĩa; nếu có ý nghĩa thì đi đúng đường cũ); kiểm chứng ≤ 300 phút, tổng ≤ 480 phút; `SmartTune::judge()` tách ra để kiểm thử | tiêu chí chấp nhận KHÔNG đổi (vọt ≤ 0,30, MAE ≤ 0,12, P95 ≤ 0,18, dao động ≤ 0,30, biên độ phân giải đầu dò); mọi nghi ngờ vẫn là FAILED và giữ PID/hồ sơ cũ |
| `machine_control.h` `updateAutoTune()` | một dòng serial `[TUNE] COAST-CHECK…` mỗi lần kích thích (không in theo chu kỳ) | chỉ đọc |
| Công cụ/kiểm thử | `tests/thermal-slow-oven.cpp` (hồi quy kín), `tools/oven_sim/autotune_family.py`, `oven.slow-incubator.json`, tùy chọn `oven_sim` (`--start`, `--kp/--ki/--kd`, `--seed*`, `--debug`), 503 kiểm tra đơn vị AutoTune | không vào firmware |

`MAYAP_SMART_THERMAL` vẫn mặc định 0, **nhưng bản mặc định không còn "giống hệt cũ"** với lò trễ dài (> 80 s): bộ học hold chậm trong `ThermalStartupController::decide()` và bộ chống "gia tốc nhanh đã cũ" trong bộ phát hiện sai lệch KHÔNG nằm sau cờ (lò thật chạy cờ 0 và kẹt dưới SP nếu thiếu chúng). Lò trễ ≤ 80 s đi nhánh cũ không đổi. Hệ quả cần biết: khi PV đang đứng ở SP, một thay đổi lò không làm PV nhúc nhích (ví dụ độ trễ/nhiệt dung đổi nhẹ) không còn bị bắt bằng "gia tốc cũ"; bộ phát hiện hold và hạn 90 phút vẫn có. Không đổi lược đồ EEPROM, giao thức, bảo vệ nhiệt, PID.

## 5. Kết quả mô phỏng (trước = main, sau = nhánh này)

### 5.1 Lò số người dùng, `tools/oven_sim.py --config tools/oven_sim/oven.slow-incubator.json` (81 lò quanh giá trị danh định + 3 kịch bản)

| Bộ điều khiển | Đạt trước | Đạt sau | High trước → sau |
|---|---|---|---|
| PID cũ (V4) | 23/81 | **57/81** | 0 → 0 |
| Adaptive V1 | 69/81 | 69/81 | 1 → 1 (cùng một lò góc, đã có trước) |
| Smart Thermal | 67/81 | **70/81** | 0 → 0 |

Khởi động từ nhiệt phòng trên lò danh định: PID cũ **không đạt trước (MAE 0,349 °C, kẹt dưới SP)** → đạt (MAE 0,047); Adaptive và Smart đạt cả trước và sau. Mở cửa (tụt 1,5 °C) và đổi SP +1 °C: tất cả đạt.
(PID cũ đổi SP +1 °C: không đạt trước (MAE 0,349) → đạt, vọt 0,18 °C.)

### 5.2 Hồi quy kín `tests/thermal-slow-oven.cpp` (47 lần chạy, 139 kiểm tra)

Hold 1,5 / 1,9 / 3 %, khởi động ấm 37,0 và 36,5 °C (có/không PID của người dùng Kp 35 Ki 0,1, hồ sơ khởi tạo cũ) và khởi động nguội 6 giờ, ba chế độ: không High/Emergency, vọt ≤ 0,30 (PID cũ nguội ≤ 0,45, là hành vi đã có),
đạt SP ≤ 2 giờ, MAE ≤ 0,12 / P95 ≤ 0,20 (ấm), MAE ≤ 0,15 (nguội). **Trên mã main bài kiểm thử này FAIL** (lò không đạt SP trong 2 giờ).

### 5.3 Smart AutoTune trên 24 lò số kiểu người dùng (`tools/oven_sim/autotune_family.py --kh 0.0133,0.0144 --dead 120,150,180,210 --lag 50 --hold 0.015,0.019,0.03 --res 0.1`)

| | Chấp nhận | ACCEPTED_BAD | sai số độ lợi | sai số trễ | sai số hold | thời gian tune |
|---|---|---|---|---|---|---|
| main | **0/24** (21 `RELAY_FAILED`, 3 `POWER_LIMITED`) | 0 | 4–16 % | tới 70 s (kẹp 190 s) | 100 % (đo = 0) | 0,7–2 giờ |
| nhánh này | **24/24** | **0** | ≤ 6,5 % (TB 4,0 %) | ≤ 1 s | ≤ 0,24 điểm | 4,3–6,1 giờ |

"ACCEPTED_BAD" = AutoTune chấp nhận nhưng chạy Adaptive 3 giờ với gain+hồ sơ vừa tune vẫn trượt vọt ≤ 0,30 / MAE ≤ 0,12 / P95 ≤ 0,18 / dao động ≤ 0,30 hoặc chạm High/Emergency.

**Cuộc sống 12 giờ sau tune** (hồ sơ AutoTune + gain vừa tune, Adaptive; 24 lò × {không sự kiện, mở cửa tụt 1,5 °C, đổi SP +1 °C} = 72 lần chạy):

| | đạt tiêu chí `oven_sim` (MAE ≤ 0,10, P95 ≤ 0,15, dao động ≤ 0,25, vọt ≤ 0,30) | vọt tối đa | MAE tối đa | P95 tối đa | dao động tối đa | High |
|---|---|---|---|---|---|---|
| feed-forward tắt (quy tắc cũ 3 %) | 49/72 | 0,070 | 0,152 | 0,224 | 0,208 | 0 |
| **nhánh này** | **72/72** | **0,066** | **0,047** | **0,063** | **0,111** | **0** |

### 5.4 Ma trận AutoTune 36 + 540 lò (`tools/test_smart_autotune.py`, cờ 0 và cờ 1 giống hệt nhau)

| | chấp nhận trước | chấp nhận sau | ACCEPTED_BAD | High / Emergency khi tune | sau tune |
|---|---|---|---|---|---|
| Mini 36 | 16 | **20** | 0 | 0 / 0 | 0 / 0 |
| Đầy đủ 540 | 231 | **293** | 0 | 0 / 0 | 0 / 0 |

Không có lò nào bị mất so với main (0 lò đã chấp nhận mà nay bị từ chối); 62 lò mới được chấp nhận (36 trước đây `RELAY_FAILED`, 26 `VALIDATION_FAILED`).
Sàn hồi quy trong `tests/thermal-smart-autotune-baseline.json` nâng từ 16/225 lên 19/285 (có ghi chú), `fullAcceptedBadMax` vẫn 0.

### 5.5 Một hồi quy bắt được ở vòng kiểm cuối (đã sửa)

Bản đầu của bước "bỏ relay khi nguội quá chậm" quyết định theo hold đo ở bước tiến sát SP. Trên lò nặng (1,6 MJ/K, hold 6–14 %) PV còn đang nhích lên ở thời điểm đo nên hold đọc thấp (6,97 % so với thật 14,25 % ở ca `full63`) → bị coi là "nguội quá chậm" → bỏ relay oan, hold cuối sai tới 7,3 điểm trên 8 lò mà main từng đo chính xác.
Nay chỉ bỏ relay khi hold đo đáng tin (phần hiệu chỉnh độ trôi ≤ max(1 điểm, 25 % hold đo)); không thì chạy relay như cũ (relay hết giờ vẫn chuyển sang bỏ relay). Kết quả: mọi lò được chấp nhận ở cả hai bản có hold lệch ≤ 0,04 điểm so với main
(27 lò trong số đó tune lâu hơn 16–40 phút do cửa sổ hold dài với đầu dò 0,1 °C); trong 62 lò mới chấp nhận, 50 có hold lệch ≤ 0,3 điểm, 11 lệch thấp tới 4,8 điểm (phía an toàn: tích phân gánh phần thiếu, và bước kiểm chứng vòng kín vẫn phán xét), 1 lệch cao 0,5 điểm.
Kết quả trên 24 lò kiểu người dùng không đổi (24/24, cùng thời gian tune, cùng Kp/Ki, 72/72 sau tune).

### 5.6 Các cổng mô phỏng của dự án

Chạy cục bộ trên mã cuối của nhánh, cùng chuỗi lệnh với `reliability-checks.yml` (g++ trên Linux, `--sanitize` ở các bộ có cờ này). Tất cả pass:

| Cổng | Kết quả |
|---|---|
| `tools/test_thermal_control.py --sanitize` | pass (kể cả `thermal-slow-oven`: 47 lần chạy, 139 kiểm tra; `thermal-smart-autotune-unit`: 503 kiểm tra; `thermal-adaptive-unit`: 755 kiểm tra) |
| `tools/test_thermal_adaptive.py` (788 ca) | pass; Adaptive V1 **519/788** (main 509), baseline V4 186/788, High 0 và Emergency 0 ở cả hai; thông gió/đổi phần cứng/lỗi cảm biến: V1 High/Emergency = 0 |
| `tools/test_smart_autotune.py` cờ 0 và `--smart-thermal` | pass; mini 20/36, đầy đủ 293/540, ACCEPTED_BAD = 0, High/Emergency khi tune và sau tune = 0 |
| `tools/ab_smart_thermal.py` | "no safety regression": hàng A/B tái tạo đúng từng byte theo `baseline-slow-oven`; Smart Thermal High/Emergency = 0 ở thông gió, đổi phần cứng, lỗi, chạy dài |
| `tools/test_thermal_sensor_path.py --sanitize`, `tools/test_tech_access.py --sanitize` | pass |
| `tools/oven_sim.py --strict`, `check_release_sync.py`, `check_attiny_protocol.py`, `check_reliability.py` | pass |
| `node --test tests/*.test.cjs` | 284/284 pass (gồm `runtime-recovery`: 0 mục ghim lệch sau khi re-pin có ghi chú review `slowOvenThermalReviewNote`) |

Đây là mô phỏng trên PC; chưa có số đo RAM/Flash/CPU của ESP32 (CI `build-firmware.yml` trên nhánh sẽ cho).

## 6. Giới hạn còn lại (đọc trước khi tin)

* **Chưa kiểm trên lò thật.** Mọi thứ phụ thuộc vào việc lò số khớp lò thật; `[TUNE] COAST-CHECK` và `[TUNE-VALIDATE]` trên lò thật sẽ cho biết.
* AutoTune lò này mất ~4,3–6,1 giờ (kiểm chứng phải ≥ 3 × τI); đó là cái giá của việc không nhận một lò mà không chứng minh được. Nếu AutoTune từ chối (an toàn), PID và hồ sơ cũ giữ nguyên.
* **Khi chưa có AutoTune**, Adaptive tự học gia tốc thiên thấp và trễ bị kẹp ở 160 s trên lò này (lưới trễ của bộ học). Một lò Adaptive góc (Kh 0,8 °C/phút, τ 313 phút, trễ 210+50 s, phòng 25 °C, không AutoTune) vẫn có thể dao động tới ~38,4 °C sau ~3 giờ
  (đã có trước thay đổi này, lớp SAFETY_LIMITED, 1/81 lò). Mở rộng lưới trễ của bộ học cần thêm RAM lịch sử (vòng 352 s) và kiểm chứng riêng; chưa làm.
* Tích phân PID bị xóa mỗi lần phanh đóng (§3.2) vẫn còn: với gain do AutoTune chọn (Kp ≈ 12–18), nếu hold < 1 % hoặc hồ sơ không còn "hold đo" (sau mismatch, sau khởi động lại với hồ sơ từ flash) thì PV có thể đỗ dưới SP cỡ `hold / Kp` (≤ 0,15 °C trong mô phỏng).
* Lò cần hold rất lớn (≥ 8 %) mà khởi động từ trạng thái ấm *đang nguội* có thể không bao giờ thấy "lần tăng đầu tiên" nên không học hold (đã có trước; lò người dùng ~2 % không thuộc loại này).
* `holdPower_` của bộ điều khiển khởi động có thể bị học quá cao lúc leo nhiệt lần đầu (tới ~19 % trên lò 1,9 %, đã có trước): phanh yếu hơn khi hồ sơ Adaptive bị rút; vọt khi đổi SP ≤ 0,18 °C trong mô phỏng.
* **Tham chiếu A/B của dự án đã đổi** (cần chủ dự án xem và đồng ý): bộ khóa `audit/smart-thermal/baseline/` bất biến được giữ nguyên; `audit/smart-thermal/baseline-slow-oven/` là tham chiếu mới mà `tools/ab_smart_thermal.py` tái tạo từng byte cho A (PID cũ) và B (Adaptive). So với bộ cũ: 66 hàng FAIL→PASS, **8 hàng PASS→FAIL** (tất cả sát ngưỡng, không hàng nào chạm High/Emergency; bảng đầy đủ trong `README.md` của thư mục đó), số hàng chạm High/Emergency không tăng. Tái tạo: `python3 tools/ab_smart_thermal.py --write-reference`.
* Bộ học hold chậm chỉ tăng (95 % duty đo + bộ tích phân 0,2 %/phút chặn ở 1,5 × duty đo + 2 điểm) và chỉ giảm khi PV vượt SP hoặc dốc lên nhanh: một tải tạm thời kéo dài lúc PV phẳng dưới SP (trứng lạnh, cửa mở lâu) có thể "đóng đinh" hold cao hơn thật trong một lúc. Thử riêng (tải thêm 20–110 W/K trong 1 hoặc 3 giờ rồi bỏ): High/Emergency = 0 mọi lần; vọt vượt 0,30 °C ở đúng một ca (Baseline, 60 W/K, 3 giờ: 0,312 so với 0,248 của mã cũ), Adaptive 40 W/K 1 giờ 0,20 so với 0,087; còn lại bằng hoặc tốt hơn, và MAE từ ~0,35 xuống ~0,04.
* Cờ `holdMeasured_` còn đến khi có mismatch/degrade/reset, kể cả sau khi đổi SP (lúc đó `holdPowerPct` bị ghi đè bằng ước lượng trực tuyến đầu tiên và áp dụng ngưỡng feed-forward 1 % thay vì 3 %). Ảnh hưởng bị chặn: feed-forward tối đa 2,85 %, dưới sàn quyền hạn 5 điểm. Bộ đếm "phẳng" `flatMs_` không bị xóa khi cổng nhánh đóng (đếm cũ tiếp tục sau đó, thiên về hold thấp = phía an toàn); thử xóa nó đổi 5 hàng ma trận A/B (thêm 1 hàng PASS→FAIL ở lò SAFETY_LIMITED) nên không đổi vào lúc này.
* Sau `reset()` của bộ điều khiển khởi động khi đang ở SP trên lò ấm chậm, bộ học hold chỉ chạy lại sau một lần tăng nhiệt thật (điều kiện `firstRiseAt_` có từ trước).
* Cửa sổ hold dài của AutoTune tính hạn `ApproachMaxLongMs` từ đầu pha tiến sát SP, không từ lúc chuyển sang cửa sổ dài: nếu lần cân bằng đầu tiên mất hơn ~80 phút thì cửa sổ dài không kịp xong và AutoTune dừng an toàn bằng `ApproachTimeout` (power = 0, PID/hồ sơ cũ giữ nguyên). Với đầu dò 0,1 °C trên lò nặng cửa sổ dài thêm 16–40 phút tune (27 lò trong ma trận 540).
* Bỏ relay làm mất kiểm tra biên độ lợi (Ku): bước kiểm chứng vòng kín là thẩm phán duy nhất cho lò đó (chủ ý).
* Cần đo trên ESP32: RAM tĩnh (ước tính +~150 B), Flash (+~3 KB), thời gian CPU. Ngân sách CI: Flash ≤ 1.480.000, RAM ≤ 164.000; bản main hiện 1.413.373 / 160.448 (profile production); CI trên nhánh sẽ báo số thật.

## 7. Việc cần làm trên lò thật (lò trống, nguội, có người trực — xem `doc/SMART_AUTOTUNE_V1.md`)

1. Nạp firmware (cờ `MAYAP_SMART_THERMAL=0`), bật AUTO + HEATER, giữ nguyên Kp/Ki đang dùng; xem PV tiến tới 37,5 và đứng đó (không còn kẹt 37,2–37,4).
2. Chạy AutoTune ở nhiệt độ phòng (cần SP − PV ≥ 2,5 °C). Có người trực, tay sẵn sàng tắt HEATER. Gửi toàn bộ dòng `[TUNE] …`, đặc biệt `COAST-CHECK` (một lần mỗi kích thích) và `[TUNE-VALIDATE]`.
3. Sau SUCCESS giữ SP 37,5 °C ít nhất 2 giờ, ghi PV, nhiệt kế chuẩn, duty, hồ sơ Adaptive.
4. Không thử lỗi công suất cao; không dùng Smart Thermal cho vận hành.

## 8. Chạy lại

```
python3 tools/oven_sim.py --config tools/oven_sim/oven.slow-incubator.json --out /tmp/slow --jobs 2
python3 tools/oven_sim/autotune_family.py --kh 0.0133 --dead 150 --lag 50 --hold 0.019 --res 0.1
python3 tools/test_thermal_control.py --only thermal-slow-oven,thermal-smart-autotune-unit
python3 tools/test_smart_autotune.py        # 36 + 540 lò, cổng ACCEPTED_BAD = 0
python3 tools/test_thermal_adaptive.py      # 788 ca
```

## 2026-10-20 addendum: hold measured over a window

A field log (strong heater, delay ~60-90 s, hold 4-6 %, 0.1 C probe) showed PV 0.5 C under SP for ~1000 s and a hold of 0.3 %. Cause: the
brake counted the heat that merely replaces the losses (the real hold) as surplus "in flight" (about +1 C of coming overshoot) because the
hold was never learned. The flat-PV measurement required six minutes of uninterrupted per-sample flatness, which a 0.1 C probe never
allows. It is now a window test: the slow PV average moved less than 0.02 C over one six minute window.

Simulation (tests/thermal-slow-oven.cpp, 6 new runs; same plant before the fix: MAE 0.72 C, never reached SP): MAE 0.04-0.09 C, SP reached.
788-case matrix: 186 -> 185 PASS (one borderline cold-start row), total MAE slightly better, High/Emergency 0. Not validated on a real oven.

## 2026-10-20 second addendum (firmware 1.1.12): why the last degrees were so slow, and what changed

Field report: from 32 C the climb to 37.5 C was very slow, PV crept up and finally stopped 0.2-0.5 C under the set point, with a hold learned at 0.3 percent.
A 4608-case grid (heater 0.25-2.0 x, capacity 300-1200 kJ/C, dead time 10-120 s, lag 10-30 s, hold 3-10 percent, probe 0.01/0.1 C, start 25/32/36 C, legacy and
Adaptive) reproduced it (315 / 154 cases never reached SP in 3 h) and found four causes, all in `ThermalStartupController`:

1. **Energy brake sized for the strongest plant.** Every ON second was assumed to add 0.12 C of coming overshoot (16 kW bank on 180 kJ/C). On a 4 kW bank
   on 600 kJ/C the real figure is 0.007 C/s, so the brake held back heat that never arrives. Now the heater gain is measured (PV rise per second of ON-time older
   than the apparent delay, running maximum, trusted after a 0.8 C rise) and scales the coast by 4 x measured / assumed (floor 0.2, never above 1; withdrawn for good if PV
   passes SP + 0.15 C). The answer to "why not accelerate and then brake": the loop does, but it braked too early and too hard because it could not tell how strong
   the heater was; it can now. The reserve is 4 x because with 2 x a wrong learned hold feed-forward removed the braking of a 90 s dead-time oven (0.8 C over SP, High).
2. **"First rise" never detected on a weak or heavy oven** (slope above 0.003 C/s is never reached), so the delay estimate, the hold learner and the gain measurement stayed
   off for good. A PV rise of 0.25 C since the first heat counts as well.
3. **The hold measurement read the wrong minutes.** It averaged the last 4 minutes of duty while PV had been flat for 6; with a dead time the last minutes are heat in
   flight. It now reads the duty over the flat window shifted back by the apparent delay.
4. **A 0.1 C probe never reached HOLD**, so the integral stayed frozen as soon as PV was within 0.15 C of SP (every probe step makes the 10 s IIR slope jump to 0.005-0.01 C/s).
   Stability is now judged against the 150 s slow average too (a step is at most one probe step, a real climb of 0.002 C/s is 0.3 C away from it).

Results (simulation only, not validated on a real oven; `audit/smart-thermal/baseline-slow-oven/README.md`): grid never-reached-SP 315 -> 162 (legacy) and 154 -> 62 (Adaptive),
median rise 2484 -> 2229 s / 2016 -> 1831 s, MAE 0.223 -> 0.165 / 0.089 -> 0.067, High/Emergency 0 -> 0; 788 / 2160 / holdout A/B: no extra High or Emergency row, PASS +22 / +69 / +22.
`tests/thermal-slow-oven.cpp` gained eight runs (weak heater started under SP; 90 s dead time in a hot room); mutation checks: removing the 0.25 C rule or lowering the reserve to 2 x fails them.

Known limits (honest list):
* 162 / 62 grid cases still never reach SP: 10 percent hold with a weak heater started at 36 C. There no PV rise ever surfaces, so the gain stays unmeasured and the legacy brake
  keeps the heater at a few percent. A stall-based relief was tried and turned the 788 commissioning gate red (High crossing), so it was left out.
* With a probe lag of 60 s the loop now cycles +-0.3 C around SP - 0.2 C instead of sitting 0.5 C under SP, and the unchanged HeaterNotHeating (E115) detector trips in one such
  no-fault case (`tests/thermal-sensor-path-baseline.json`, FALSE_TRIP_NO_FAULT 4 -> 5). Four other no-fault cases with 30-60 s lag already tripped before.

## 2026-10-21 third addendum (firmware 1.1.13): why 1.1.12 still sat at 37.1 C for an hour, and the measured launch

Field report on 1.1.12: after about 53 minutes PV stood at 37.13 C with a set point of 37.5 C, the PID monitor showing Kp 25, Ki 0.1, hold 20.3 percent,
learned gain 0.002 C/s, delay 160 s, confidence 17 percent, and the heater only bursting to 100 percent now and then. The 1.1.12 simulations had passed. A grid of
realistic ovens (gain 0.008-0.025 C/s at 100 percent, dead time 100-180 s, lag 40 s, hold 12-28 percent, probe 0.01 / 0.1 C, start 29 / 32 C, `RG144`) reproduced the
stall in the 1.1.12 controller and found the causes:

1. **The brake predicted heat that was not on its way.** The coast was the ON-time of the last 160-240 s times a gain *assumed* or learned from the Adaptive profile (0.002 C/s in the
   report, 6 x too low, or a junk value on other days) minus the hold *as learned by the same brake*. With a hold of 20 percent and a delay of 160 s the in-flight heat of a normal
   hold duty is already larger than the distance to the set point, so the brake never released: PV stood still under SP and the hold was never measured (it needs PV standing near SP).
   The earlier simulations used plants (delay 30-90 s, hold 1-10 percent) where this loop closes by itself.
2. **A hint of the Adaptive profile could add to the brake of the measured model** (coast per ON-ms of an average warm-up), double counting the heat that is already followed.
3. **The PID integral wound up behind the brake** and then carried the boost into the hold, the Adaptive feed-forward (up to 95 percent on a junk learned hold) fought the launch,
   and a fixed Kp that is fine for the oven of the report rang on a lighter oven (the gain doubles when the number of egg trolleys halves).

What 1.1.13 does (`ThermalStartupController`, launch model; simulation only):

* **Heat in flight is followed per ON-second** through the measured delay and the first order lag (10 s bins, 800 s ring). The gain is measured on the launch from the arrived heat
  (PV rise plus the loss seen before the heat surfaced) and bounded from above even when nothing has been seen yet; the delay is the time to the PV turning upward.
* **Full throttle until the predicted peak is about 1 C below SP**, then the brake. With nothing known (first batch) a deliberate cut to 0 measures the drift after the cut
  (the calibration of the model) and the hold is read from the cooling of the idle chamber and from the flat PV balance.
* **The plan is stored like the tuning** (`thermal_launch_plan.h`, NVS keys `la` / `lb`, CRC, two alternating slots, read-back, at most one save per 10 minutes and only when it
  differs materially): gain, delay, drift calibration and hold. The next batch launches at once; it still re-measures the gain, because a different number of egg trolleys changes the
  thermal mass (the hold does not change with the load, the gain does). A plan that the oven does not answer is dropped for the batch.
* **The hold measured on the launch replaces the Adaptive feed-forward**, the integral restarts while the boost runs, the proportional gain is limited to 70 / (gain x (delay + lag))
  and the integral time to 4 x (delay + lag) (only ever lowers the configured gains), a restart at the set point with a known plan uses the plan instead of fitting noise, and a raised
  set point starts a new warm-up on the same oven. A stall backstop relaxes a model that predicts heat while PV has stood still for minutes.
* **The profile / scheduled vent fan waits during the warm-up** (`WARMUP_VENT_*`: PV more than 1 C under SP at the batch start, released at SP - 0.5 C, after 30 minutes or without
  a sensor). A test caught that the fan was still open for the first ~3 minutes of every batch (before the sensor filter is usable): fixed, it now covers the start.

Results (simulation only, not validated on a real oven): `RG144` (both batches, plant true gain 0.85-1.3 x the plan's): no High crossing; batch 1 (nothing known) reaches SP - 0.3 C
in a median of 2100 s (max 3550 s), batch 2 with the plan reaches SP - 1 C in a median of 780 s and SP - 0.3 C in 1150 s (physical minimum for the grid ~690 s median); a plan used on
half / double thermal mass: no High, overshoot at most 0.47 / 0.28 C. Oven of the report (gain 0.012, delay 160 s, hold 20 percent): batch 1 about 34-44 minutes, batch 2 about 12-17 minutes.
**Reaching the set point inside 10 minutes is not possible on such an oven**: with 5.4 C to climb at a net 0.01 C/s and a delay of 160 s the physical minimum is ~11 minutes to SP - 0.5 C.
788 / 2160 / holdout A/B: PASS 722 -> 1338, 1674 -> 3782, 323 -> 698; rows with High 66 -> 33, with Emergency 34 -> 4 (matrix), none in the other two.
`tests/thermal-launch.cpp` (8 ovens, batch 1 / batch 2 / half and double mass / vent profile; mutation-checked: dropping the plan fails the 20-minute check),
`tests/thermal-launch-plan.cpp` (record, wear policy, two slots, torn write at every byte).

Known limits (honest list):
* The first batch of a new oven is slower than later ones (about 30-45 minutes on the oven of the report): it spends minutes measuring (probe pulse, ramp, one deliberate cut).
* The hold stored in the plan can be wrong by a large factor on a few grid ovens (0.2 x to 3 x); it is credited at 80 percent and re-measured on every launch, and no grid case crossed High with it.
* The Adaptive gate "V1 passes at least twice the baseline" was replaced by "at least the baseline": the baseline uses the same launch model now and passes more than twice as many cases itself.
* `tests/thermal-smart-autotune-unit.cpp`: the very light plant that used to be rejected by the validation (180 kJ/C, 0.1 C probe) is accepted now, the rejection test uses a lighter one.
