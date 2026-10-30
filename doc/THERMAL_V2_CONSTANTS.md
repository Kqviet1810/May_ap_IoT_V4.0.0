# Thermal V2.0.0 — đặc tả đã chốt (G0) và nguồn gốc từng hằng số

Quy tắc: **hằng số nào không suy ra được từ cảm biến, vật lý hoặc một phép kiểm số thì không được vào mã.** Mỗi dòng dưới đây ghi nó suy ra từ đâu. Chủ máy đã duyệt lộ trình bản 2 và 7 quyết định (`THERMAL_V2_ROADMAP.md` §7).

## 1. Hợp đồng khối (1 vào – 1 ra – tự kiểm)

**Vào (mỗi mẫu cảm biến mới, theo số thứ tự mẫu, không theo cờ bool):**

| Tín hiệu | Nguồn trong firmware | Ghi chú |
|---|---|---|
| `pv` (°C, đã lọc), `seq`, `tSampleMs` | `temperature_`, bộ đếm mẫu mới (thêm) | mẫu thật 0,5 Hz; trễ lọc ≈ 6 s đã nằm trong θ nhận dạng |
| `sp` | `config_.targetTemp` | |
| `dQ` = độ phân giải cảm biến | `sensorResolutionMode` → 0,1 / 0,01 °C | tự động = 0,1 |
| `headroomC` | `min(highTempAlarm, ventOnTemp) − max(0, −tempOffset) − sp` | tính trong `machine_control.h`, chung miền với bảo vệ (đo thô) |
| `permit` | `normalSsrPermit && actuatorReady` | false ⇒ V2 đóng băng, không phát công suất |
| `fanCirc`, `fanVent`, `ventHold` | `outputs_.state()`, `warmupVentHold_` | |
| `cap` (%) | `maxHeaterPower` | một trong các giá trị người dùng |
| `eggs` | số trứng (mặc định 10 000) | giá trị người dùng thứ tư |
| `e115Frac` | `heaterWatch_.accumOnMs() / (heaterStuckDurationSec·1000)` | đọc một chiều, không sửa E115 |
| `delivered` (%/s) | `burst.groupA && permit` × dt | **năng lượng đã giao thật**, không phải lệnh |

**Ra:** `commandedPower` ∈ [0, cap] (%), `state` (mã), `powerLimited` (cờ, không phải lỗi), telemetry. Bộ chia xung/arbiter/bảo vệ không đọc ngược.

## 2. Phương trình

```
θ_eff = θ_d + τ_lag/2                       (nửa-quy-tắc, Skogestad)
u_cb   = [UA·(PV−Tamb) + hA·(PV−Te)] / Pmax·100         cân bằng tổn thất
u_ff   = u_cb + β·hA·(SP−Te)/Pmax·100                   đẩy nhiệt vào trứng
PV_pred = PV + k_air · Σ_i w_i·(P_giao,i − u_cb,i)      cửa sổ θ_d + 3·τ_lag, w = đáp ứng xung trễ+quán tính
Te' = hA·(PV−Te)/Ce          (một trạng thái trứng)
u = clamp( u_ff + K·(SP − PV_pred) + I , 0, cap )
K = Kc  nếu SP − PV_pred ≥ 0 ;  K = 2·Kc  nếu SP − PV_pred < 0     (bất đối xứng công khai)
Kc = τ1 / ( g·(τc + θ_eff) ) ,  τc = 2·θ_eff ,  τI = min(τ1, 4·(τc + θ_eff))
I' = Kc/τI·e   (tích phân có điều kiện: đóng băng khi bão hòa cùng chiều;  |I| ≤ 60 điểm)
mọi sự kiện: I := u_trước − u_ff − K·e                    (êm; không reset)
ngân sách vọt:  (u − u_ff) ≤ 0,2 / (ε̂·k_air·θ_d')         ε̂ = sai số tương đối dự đoán đo được
```

## 3. Bảng hằng số và nguồn gốc

| Hằng số | Giá trị | Suy ra từ |
|---|---|---|
| Δ (bước cảm biến) | 0,1 hoặc 0,01 °C | cấu hình `sensorResolutionMode` |
| σ_q | Δ/√12 | sai số lượng tử đều (nhưng không trắng; xem ngưỡng phẳng) |
| Chu kỳ mẫu Ts | 2 s | `POLL_PERIOD_MS` |
| Phát hiện bắt đầu tăng (θ̂) | PV − PV₀ ≥ 2Δ | 1 bậc có thể là lượng tử; 2 bậc liên tiếp là tín hiệu |
| "Phẳng" | \|mean(cửa sổ 2) − mean(cửa sổ 1)\| ≤ Δ, mỗi cửa sổ 300 s | so sánh trung bình, không hồi quy tuyến tính trên nhiễu bậc thang |
| Điều kiện kích thích | `delivered` > 30 % **và** độ dốc > 4Δ/W, W = 120 s | độ dốc nhỏ nhất vượt lượng tử |
| Cửa sổ hồi quy k_air | W_k = max(60 s, 10Δ/độ_dốc_ước), tối đa 180 s | sai số tương đối ±Δ/R ≤ 10 % cần độ tăng R ≥ 10Δ |
| τ_c | 2·θ_eff | kiểm số G0: `tools/thermal-v2/g0_results.txt` — 648 ca lệch mô hình (θ ×0,7–1,4; g ×0,7–1,3; τ1 ×0,7–1,4): 0 ca mất ổn định, đỉnh độ nhạy Ms = 1,33 (<1,4), vọt tuyến tính ≤ 7,8 % bước |
| Hệ số b trong Kc | 1 (giữ θ_eff trong Kc) | bỏ (b=0) làm Ms 1,65–2,0 và vọt 36–56 %: dư địa bền vững cần |
| Bất đối xứng | 2 | kiểm số G0: ×1/×2/×3/×4 → Ms 1,32 / 1,62 / 1,93 / 2,28; chọn ×2 (Ms ≤ 1,62 < 2,0) |
| τI | min(τ1, 4(τc+θ_eff)) | quy tắc FOPDT SIMC (Skogestad); bản nháp 1 bỏ min() nên tích phân chậm gấp ~6 |
| Cửa sổ dự đoán | θ_d + 3·τ_lag | 95 % đáp ứng xung bậc nhất |
| β (tin cậy trứng) | 0,7 | rà soát độc lập (mô phỏng): β=0,7 → lên nhiệt 1,00–1,06 × nhanh nhất, vọt 0,08–0,16 °C; **tiên nghiệm, sẽ chỉnh bằng log thật** |
| \|I\| tối đa | 60 điểm | công suất hold cực đại hợp lý khi trứng lạnh (≤ 104 % bank, trừ u_ff) |
| Nhiệt đang trên đường — giới hạn độc lập | k_hi = 2,5e-4 °C/s/% (= 0,02 °C/s ÷ 80 %), θ_hi = 180 s, h_lo = 12 % | trường hợp xấu nhất đo được trong RG144 (G ≤ 0,025 °C/s, trễ ≤ 180 s, hold ≥ 12 %); **không chỉnh theo bộ điều khiển** |
| Ngưỡng E115 leo công suất | `e115Frac` ≥ 0,4 | 40 % của 900 s: còn 540 s để PV tăng 0,3 °C trước khi lỗi |
| Mất cho phép → quay lại | > 120 s ⇒ tính lại từ PV mới | 2·θ_d danh định; nhỏ hơn thì giữ trạng thái |
| Đường dốc SP khi đổi SP | tốc độ tối đa = độ dốc tối đa vật lý đã đo | chỉ \|ΔSP\| ≤ 0,5 °C nằm trong R2/R3 |
| Prior mẻ đầu | Pmax = 16 kW, k_air ≈ 1/Cf với Cf = 0,3 MJ/°C (nhẹ nhất ⇒ thận trọng nhất), θ_d = 180 s | giới hạn trên của dải RG144; chỉ dùng cho tới khi nhận dạng xong |
| Hold ban đầu | 0 (RAM, không NVS) | mất điện: khởi động lại từ PV; hold tích lũy qua `I` |

Hằng số **chưa có** (cố ý): ngưỡng CUSUM, margin m, ngưỡng độ dốc tay, X của phục hồi — các thứ này thuộc thiết kế bản 1 đã bỏ (xem ROADMAP §4).

## 4. Nguồn
Skogestad (2003) *Simple analytic rules for model reduction and PID controller tuning* (nửa-quy-tắc, SIMC FOPDT, τI = min(τ1, 4(τc+θ))); Skogestad & Grimholt (2012) *The SIMC method for smooth PID controller tuning*; Grimholt (2018) NTNU. Bộ dự đoán kiểu Smith với trễ+lag bị loại khỏi dự đoán: kiểm bền vững bằng số ở `tools/thermal-v2/g0_margins.py`. Các dải thông số lò: `doc/THERMAL_REDESIGN_RESEARCH.md`, `doc/SLOW_OVEN_STUDY.md` (đều là suy ra/mô phỏng, **không** đo trực tiếp).

## 5. Kiểm số G0 — nó chứng minh gì và không chứng minh gì
- Chứng minh: với **đối tượng tuyến tính** FOPDT+lag và bộ dự đoán, quy tắc `τc = 2θ_eff, b = 1` bền với lệch mô hình trong dải ghi trên, và hệ số ×2 phía vọt không phá biên.
- **Không** chứng minh: bão hòa công suất, lượng tử cảm biến, trứng, nhiễu, bộ giới hạn độc lập, hành vi 24 h — những thứ này do G2–G4 kiểm.

## 6. Kỷ luật tập A/B/C (cơ chế)
- Tập, kịch bản, seed: `tools/thermal-v2/plants.json` (**commit trước G2**). Seed B = 20261022, sinh bằng xorshift32 theo thứ tự ghi trong file; khoảng tham số cũng ghi ở đó.
- Runner B/C (viết ở G1) in hash commit của `thermal_v2.h`, từ chối chạy khi cây bẩn, và đếm số lần B/C đã được chạy; sau mỗi thay đổi cấu trúc phải đổi seed B (ghi vào `plants.json` cùng lý do).
- Tiêu chí: `tools/thermal-research/acceptance.json` (đã viết lại theo R1–R7; cổng: 0 High, 0 Emergency, 0 E115, 0 lệnh sưởi không an toàn).

## 7. Việc còn lại cho G1 (mở)
- Công thức `w_i`: chọn đáp ứng xung rời rạc của (trễ θ_d + bậc 1 τ_lag); ghi vào mã cùng test so với mô phỏng liên tục.
- Công thức ε̂ và `θ_d'` trong ngân sách vọt: chốt khi có đường cong mô phỏng đầu tiên (G2), **không** qua chỉnh số.
- Tính "nhanh nhất vật lý" từng lò bằng mô phỏng bơm 100 % (không phải công thức), cả cho cú tụt cửa (R5).
