# Smart Thermal — Phase 0 (baseline, kiến trúc) và Phase 1 (toàn vẹn safety của simulator)

Cơ sở: `main` @ `8d51432c16265b53960c349fa6c2f0c327f7c8f8` (đây là HEAD mới nhất; nhánh làm việc `claude/busy-maxwell-5bgqby` được tạo từ đúng commit này, chưa có thay đổi firmware).
Chỉ mô phỏng host. **Chưa thử trên phần cứng. Chưa sửa firmware, oracle, ngưỡng.**

## Phase 0 — Baseline tái hiện

Chạy lại `tools/test_thermal_adaptive.py --full` và `tools/test_smart_autotune.py --sanitize` trên `main`; so từng dòng CSV với bằng chứng WSL2 của người kiểm định.

| Bộ | Kết quả tái hiện | So với báo cáo kiểm định |
|---|---|---|
| Ma trận 788 — baseline V4 | 186/788, High 1, Emergency 0 | khớp |
| Ma trận 788 — Adaptive V1 | 509/788, High 0, Emergency 0, 387/489 REACHABLE | khớp |
| Ma trận 2160 — baseline | 319/2160 (High 33, Emergency 17) | khớp |
| Ma trận 2160 — Adaptive V1 | 1242/2160 (High 33, Emergency 17), 1090/1535 REACHABLE | khớp |
| Thông gió 63×3 | Adaptive 0/0, tail MAE 0,102 (baseline 0,376), windup 5,6 (48,9) | khớp |
| Smart AutoTune 540 | 231 chấp nhận, 309 từ chối, `ACCEPTED_BAD=0`, High/Emergency 0/0 | khớp |
| CSV `matrix`, `legacy788`, `vent`, `faults`, `hwchange`, `smart full` | **giống hệt từng dòng** | 0 sai khác |

SHA-256 các CSV gốc: `doc/thermal/data/baseline-8d51432.sha256`.

## Phase 0 — Kiến trúc thực tế (đọc từ mã)

```
SHT485 → ActualSensorFilter (IIR, lượng tử hóa)  → temperature_ / rawTemperature_
 → updateAlarms(): safetyTemp = max(raw, filtered) → High (xác nhận 1 s) / Emergency (ngay) → faults_
 → updateHeatingAndOutputs():
     ThermalObserver → ThermalLearner (profile) → AdaptiveV1.update() (feed-forward + chính sách tích phân + StartupHint)
     ThermalStartupController (FullHeat/Approach/SoftLanding/Hold, đỉnh dự đoán, trần công suất)  ← ĐÃ CÓ cơ chế dự đoán nhiệt dư
     ThermalController (PID) → adaptive authority limiter → HeaterBurstScheduler → OutputArbiter → SSR (GPIO1) + contactor tổng
 E115 (HeaterNotHeating) và SensorFrozen tính trong updateAlarms từ cảm biến và thời gian SSR thực sự BẬT
```

Hệ quả cho kế hoạch: bộ dự đoán nhiệt dư **đã tồn tại** (`ThermalStartupController`); Phase 2 là nâng cấp/chuẩn hóa nó, không phải viết mới từ đầu.

## Phase 1 — Phát hiện: simulator cũ dùng nhiệt THẬT để tự bảo vệ

`tests/thermal-adaptive-sim.h` (dòng 256–257, 261):

```
h->highTemperatureActive_ = temp >= HighC;       // temp = nhiệt THẬT của plant
h->emergencyActive_       = temp >= EmergencyC;
h->sample(static_cast<float>(temp), static_cast<float>(fed));   // "raw" cũng là nhiệt thật
```

Firmware thật (`MachineController::updateAlarms`) tính `safetyTemp = max(rawTemperature_, temperature_)` — tức **chỉ từ cảm biến**. Vì vậy mọi kết quả "0 High/Emergency" của ma trận 788 không chứng minh gì về trường hợp cảm biến sai. Oracle gốc **không bị sửa**; harness mới đứng cạnh nó.

### Harness mới

`tests/thermal-sensor-integrity.cpp` + `tools/test_thermal_integrity.py`. Điều khiển chạy qua **đúng mã production**: lộ trình gia nhiệt (`updateHeatingAndOutputs`), khối trip High/Emergency thật (cắt từ `updateAlarms`), `ConditionTimer`, khối kế toán năng lượng E115 thật, và quy tắc SensorFrozen (20 phút không đổi VÀ ≥ max(60 s, 450 s) SSR bật). Chuỗi: nhiệt thật → cảm biến (độ phân giải, lỗi) → bộ lọc production → bộ điều khiển; trip chỉ thấy giá trị cảm biến.

Lỗi tiêm vào (cảm biến vẫn "hợp lệ" về giao thức): kẹt ở 30°C, kẹt ở giá trị vừa đọc, lệch −3°C, trôi −1°C/phút (tối đa −8), kẹt cao 45°C. 9 plant (khối lượng 180 kJ/°C → 1,6 MJ/°C × công suất heater 0,5/1/2) × 2 thời điểm (khởi động 600 s và ổn định 5400 s). SP 37,5°C, 16 kW danh định.

### Kết quả (72 kịch bản cảm biến đọc THẤP; dữ liệu `doc/thermal/data/sensor-integrity-8d51432.csv`)

| Lỗi | Số ca | Nhiệt thật ≥ 39,0°C (Emergency) | Firmware **không bao giờ** cắt |
|---|---:|---:|---:|
| Kẹt ở 30°C | 18 | 14 | 0 |
| Kẹt ở giá trị vừa đọc | 18 | 10 | 6 |
| Lệch −3°C | 18 | 16 | **15** |
| Trôi −1°C/phút | 18 | 15 | 6 |
| **Tổng** | **72** | **55 (76%)** | **27 (24 trong số đó nhiệt thật ≥ 39°C)** |

* Khi cảm biến phát hiện được, độ trễ trung vị **sau khi nhiệt thật đã vượt Emergency là ~967 s** (E115 cần 900 s SSR bật; SensorFrozen cần 1200 s). Nhiệt thật đỉnh tới **94,3°C** (plant nhẹ, heater gấp đôi); trung bình ~17 MJ nhiệt được cấp trong lúc plant đã trên Emergency.
* Lỗi lệch −3°C làm plant **ổn định vĩnh viễn ở ~40,6°C** (trên Emergency) trong khi firmware tin là 37,5°C — và không có cơ chế nào trong phần mềm nhận ra. Đây chính là hậu quả của một offset hiệu chuẩn âm quá lớn hoặc một cảm biến lệch, không chỉ của hỏng hóc.
* Chiều ngược lại **an toàn**: cảm biến kẹt cao 45°C → Emergency trip ngay (18/18), plant không vượt 37,9°C.
* Kiểm soát (không lỗi) khớp mô phỏng gốc.

### Kết luận Phase 1

1. Với **một** cảm biến, phần mềm hiện tại **không** bảo vệ được trước cảm biến đọc thấp: nó chỉ thấy lỗi sau khi nhiệt thật đã vượt ngưỡng nhiều phút. Đây không phải lỗi của thuật toán điều khiển; đó là giới hạn kiến trúc, đúng như báo cáo kiểm định cảnh báo. **Bắt buộc có bảo vệ quá nhiệt độc lập bằng phần cứng** (cảm biến riêng, cắt riêng) trước thử công suất đầy đủ. Mô phỏng không thể thay thế.
2. Có thể thu hẹp (không thể xóa) khoảng hở bằng phần mềm: kiểm tra hợp lý năng lượng–độ tăng nhiệt dựa trên profile đã học (độ lợi gia nhiệt đã biết thì SSR bật N giây phải tăng ≥ x°C; không tăng → lỗi sau vài chục giây thay vì 900 s). Đây là việc của Phase 2/3 và **không** thay cho phần cứng. `tools/test_thermal_integrity.py` giữ "ratchet" 72/55/24: các con số này chỉ được giảm.
3. **Phát hiện phụ (chưa điều tra, dữ liệu có thật):** plant `cap=180 kJ/°C, eff=0,5` (REACHABLE theo phân loại) — mô phỏng gốc cho overshoot −1,807°C / MAE 2,0°C và trong harness mới E115 báo `HeaterNotHeating` **giả** sau ~5500 s dù không có lỗi. Cần xem đây là hồi quy điều khiển hay giới hạn mô hình (Phase 3).
4. Giới hạn của harness này: lag cảm biến chưa mô hình hóa (độ trễ đọc bằng 0 ngoài bộ lọc IIR); chưa mô phỏng nhiễu; SensorSuspect (phát hiện nhảy) và các bất thường khác của `updateAlarms` chưa nằm trong chuỗi; không mô phỏng SSR bị chập (cần cảm biến dòng/nhiệt độc lập).

## Cách chạy lại

```
python3 tools/test_thermal_integrity.py --report-dir /tmp/mayap-thermal-integrity
python3 tools/test_thermal_adaptive.py --full --report-dir /tmp/mayap-adaptive-full
python3 tools/test_smart_autotune.py --sanitize --report-dir /tmp/mayap-smart
```
