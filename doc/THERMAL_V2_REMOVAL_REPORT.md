# Thermal Control V2.0.0 — bước 1: xoá hệ nhiệt cũ

Nhánh `thermal-control-v2.0.0`, cắt từ `main` (e8b557e, firmware 1.1.15). Phiên bản firmware trên nhánh này: **2.0.0**.

> **Firmware này KHÔNG có bộ điều khiển nhiệt. Yêu cầu công suất sưởi cố định 0 %, lò không bao giờ được sưởi. Không nạp vào lò đang vận hành.**
> Bản cũ còn nguyên ở `main` (e8b557e) và nhánh `claude/gracious-hopper-zjvufg`.

## 1. Đã xoá gì

**Firmware — 14 file header (5 247 dòng)**

| File | Dòng | Nội dung |
|---|---:|---|
| `thermal_control.h` | 1 332 | PID (`ThermalController`), phanh khởi động (`ThermalStartupController`), relay AutoTune |
| `thermal_smart_autotune.h` | 894 | Smart AutoTune |
| `thermal_learner.h` | 873 | bộ học hồ sơ nhiệt |
| `thermal_predictive.h` | 775 | Thermal Predictive V1 (mô hình hai nút, bộ nhận dạng, bộ lập kế hoạch) |
| `thermal_adaptive_v1.h` | 275 | Adaptive Thermal V1 |
| `adaptive_thermal_balance.h` | 175 | cân bằng nhiệt thích nghi, tự làm mát |
| `thermal_profile.h` / `thermal_launch_plan.h` | 174 / 174 | hồ sơ nhiệt, kế hoạch khởi động (bản ghi NVS) |
| `thermal_predictive_store.h` | 162 | lưu mô hình dự đoán (NVS) |
| `thermal_observer.h` | 159 | bộ quan sát |
| `thermal_profile_storage.h` | 107 | lưu hồ sơ nhiệt (NVS) |
| `thermal_assist.h` | 69 | cầu nối PID ↔ học |
| `adaptive_persistence.h` / `adaptive_model.h` | 51 / 27 | lưu mô hình adaptive (NVS) |

**Trong các file còn lại (~860 dòng):** các hàm `startAutoTune`, `cancelAutoTune`, `updateAutoTune`, `updateAdaptiveBalance`, `trackAdaptiveEnergy`, `publishThermalLearning`, `offerLaunchPlan`, `loadLaunchPlan`, `offerPredictiveModel`…; khối tính công suất trong `updateHeatingAndOutputs`; các lời gọi `pid_.reset()` rải khắp bộ điều khiển; thành viên `pid_`, `startupHeat_`, `autotune_`, `thermalV1_`, `adaptiveThermal_`, `thermalPredictive_`, `launchPlan_`…; luật kiểm tra "Kp/Ki/Kd không đồng thời bằng 0" (`mayapPidHasAuthority`); `LedCode::AutoTune`; macro `MAYAP_SMART_THERMAL`, `MAYAP_ADAPTIVE_OBSERVER_ONLY`, `MAYAP_PREDICTIVE_EVALS_PER_CYCLE`; hằng số `AUTOTUNE_*`, `PID_D_FILTER_TAU_SEC`, `THERMAL_PID_BETA`; trường telemetry của hệ cũ trong `MachineRuntime` / `PidMonitorData`.

**HMI:** màn Smart AutoTune và hộp xác nhận, màn "Hồ sơ nhiệt", mục "Tự chỉnh PID", 28 trong 34 hàng PID Monitor (còn 6), các mục menu Kp/Ki/Kd, Cong suat tune, Bien do tune, Tu can bang nhiet, Smart Thermal, Nhiet du doan.

**Web:** thẻ "Smart AutoTune", nút tự dò/huỷ tự dò, công tắc "Tự cân bằng nhiệt", ô Kp/Ki/Kd, ô relay cũ, 13 trong 18 ô PID Monitor, bảng tên giai đoạn/lý do AutoTune; lệnh `autotune_start` / `autotune_cancel` và `tech_request kind=autotune` không còn được nhận.

**Kiểm thử/CI:** 56 file test/công cụ (≈ 8 400 dòng) chỉ phục vụ hệ cũ (mô phỏng PID, AutoTune, Adaptive, Predictive, lò ảo, lưới 1 248 ca, A/B, nghiệm thu), cùng các job `sim-gates`, `sim-ab`, bước `oven_sim`, biến thể build `MAYAP_SMART_THERMAL` và artifact `SMART-ON-TEST-ONLY`.

## 2. Còn lại gì (giữ nguyên, không đụng)

- Mọi lớp bảo vệ: Nhiệt cao (High), Khẩn cấp (Emergency), nhả contactor tổng, chặn SSR, cảm biến (hợp lý/đóng băng/mất/nhiễu), E115 "không đáp ứng nhiệt", dao động/tốc độ nhiệt bất thường.
- Bộ chia xung SSR (`heater_burst_scheduler.h`), bộ phân xử đầu ra `OutputArbiter`, quy tắc khởi động đầu ra, post-cool, quạt tuần hoàn/quạt hút, lịch thông gió, tạo ẩm, đảo trứng.
- Bộ lọc cảm biến, hiệu chuẩn, ATtiny, EEPROM/NVS, lịch sử bản ghi kỹ thuật (PIN, quyền Web, yêu cầu Web chờ xác nhận ở HMI).
- Lược đồ cấu hình 13 và mọi bản ghi EEPROM giữ nguyên bố cục: các trường `kp ki kd pidCycleSec controlMode autotune* adaptiveThermalBalanceEnabled smartThermalEnabled predictiveThermalEnabled` vẫn được lưu/đọc nhưng **không còn tác dụng** (V2 quyết định tái dụng hay bỏ).
- `thermal_program.h`, `egg_cooling.h`, `heat_latch.h`, `output_budget.h`: mô-đun thiết kế chưa nối vào đường điều khiển.
- Giao thức log (`MAYAP-AI-LOG`) giữ nguyên số cột; các cột của hệ cũ luôn bằng 0 (V2 sẽ định nghĩa lại).
- Công suất tối đa (`maxHeaterPower`) vẫn có ở HMI/Web — là một trong ba giá trị người dùng của V2.

## 3. Những chỗ đã can thiệp để xoá

| Vị trí | Việc đã làm |
|---|---|
| `machine_control.h` `updateHeatingAndOutputs()` | Thay toàn bộ khối tính công suất bằng **một khe quyết định**: `commandedPower = 0`. Giữ nguyên các cổng cho phép (`normalMasterPermit`, `normalSsrPermit`), bộ chia xung và arbiter |
| `machine_control.h` lịch thông gió | Bỏ phần báo "còn bao lâu" cho bộ nhiệt; giữ nguyên quyết định bật/tắt quạt hút |
| `machine_control.h` an toàn | Bỏ nhánh `autotune_.running()` trong điều kiện cảnh báo nhiệt và quạt hút; logic High/Emergency không đổi |
| `machine_control.h` lệnh/yêu cầu Web | `AutoTuneStart/Cancel` trả "AUTO TUNE DA GO BO"; `submitTechRequest` chỉ nhận kind `Config` |
| `config.h` | Bỏ kiểu/trường/hằng số của hệ cũ; giữ id lệnh `AutoTuneStart`/`AutoTuneCancel` để không đổi số thứ tự; version 2.0.0 |
| `hmi.h` | Bỏ màn/hộp xác nhận/nhánh nhập; nhóm menu "PID/GIA NHIET" → "GIA NHIET" (chỉ còn trần công suất); 37 → 29 mục trong menu; hàng PID Monitor 34 → 6 |
| `transaction_bridge.h` | Bỏ `adaptiveThermal`, `autoTune*`, các hàng `pidMon` của hệ cũ; bỏ `autotune_start/cancel`; bỏ kiểm tra PID khác 0 |
| `MAYAP_INDUSTRIAL_v1_0_0.ino` | Bỏ 4 lời gọi `service()` của kho NVS hệ cũ |
| `app.js` / `index.html` | Gỡ thẻ/nút/ô nhập/bảng tên liên quan |
| `tests/runtime-preservation.json` | Gỡ ghim file/hàm đã xoá; ghim lại `config.h`, `class MachineController`, `startBatch` sau khi xem xét (ghi chú `release200ReviewNote`) |
| CI | `reliability-checks.yml`: job `thermal` → `heater-path` (chạy `tools/test_heater_path.py`), bỏ `sim-gates`, `sim-ab`; `build-firmware.yml`: bỏ `oven_sim`, biến thể Smart, artifact Smart-ON |
| Công cụ test | `tools/test_thermal_control.py` → `tools/test_heater_path.py`: chỉ còn arbiter, bộ chia xung SSR, E115, bộ nối cấu hình/EEPROM, bộ lọc cảm biến, chương trình SP |

## 4. RAM / Flash trước và sau

Đo bằng chính bước "Measure firmware size" của CI (arduino-cli, ESP32-S3, cùng FQBN), không ước tính.

| Build | Chỉ tiêu | Trước (main e8b557e) | Sau (nhánh V2) | Chênh |
|---|---|---:|---:|---:|
| DEV (có lệnh serial chẩn đoán) | RAM tĩnh (Global variables) | 167 224 B | _điền sau CI_ | |
| DEV | Flash (Sketch) | 1 479 529 B | _điền sau CI_ | |
| PROD | RAM tĩnh | 166 304 B | _điền sau CI_ | |
| PROD | Flash | 1 445 025 B | _điền sau CI_ | |

Ngưỡng CI: Flash ≤ 1 480 000 B, RAM tĩnh ≤ 167 500 B (trước khi xoá DEV chỉ còn 471 B dư Flash và 276 B dư RAM). RAM tĩnh là phần cố định lúc biên dịch; RAM heap còn lại lúc chạy phải đo trên lò thật (không đo được ở CI).

## 5. Kiểm tra sau khi xoá

Xem mục kết quả trong báo cáo cuối của phiên làm việc và CI của PR (build, reliability: web, uplink, heater-path, host).
