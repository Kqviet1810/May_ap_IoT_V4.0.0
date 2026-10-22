> **Đã gỡ khỏi firmware 2.0.0 (Thermal Control V2.0.0).** Tài liệu này mô tả hệ nhiệt cũ (≤ 1.1.15) đã bị xoá khỏi nhánh `thermal-control-v2.0.0`; chỉ còn giá trị lịch sử. Mã nguồn cũ nằm ở `main` (e8b557e) và nhánh `claude/gracious-hopper-zjvufg`. Xem `doc/THERMAL_V2_REMOVAL_REPORT.md`.

# Mô phỏng lò cho firmware hiện tại (chạy trên máy tính)

Công cụ `tools/oven_sim.py` chạy **đúng mã điều khiển của firmware** (PID cũ, Adaptive V1, Smart Thermal, bộ chia burst, `OutputArbiter`)
trên máy tính, nối với một lò ảo. Chỉ **lò** là mô hình (bậc 1 + trễ vận chuyển + trễ heater). Đây là **mô phỏng**, chưa phải kiểm định vật lý:
nó cho biết "nếu lò có các thông số này thì firmware đạt hay không", không chứng minh lò thật có đúng các thông số đó.

## Cài một lần (Windows)
1. Python 3.8+ (python.org). Không cần thêm thư viện.
2. MSYS2 (msys2.org) → mở **MSYS2 UCRT64** → `pacman -S mingw-w64-ucrt-x86_64-gcc` → thêm `C:\msys64\ucrt64\bin` vào PATH.

## Chạy
```
python tools\oven_sim.py --config tools\oven_sim\oven.example.json --out oven-report
```
Lần đầu biên dịch mất 15–60 giây, các lần sau dùng lại. Mở `oven-report\report.html` bằng trình duyệt (kèm `report.md`, `results.csv`, `trace_*.csv`).
Mỗi ca 6 giờ lò ảo chạy khoảng 0,3 giây; 243 ca + 9 kịch bản mất khoảng 20 giây trên máy chủ thử.

## Đo thông số lò (file `oven.json`, chép từ `oven.example.json`)
| Khóa | Cách đo |
|---|---|
| `kh_c_per_min` | Lò rỗng, đóng cửa, heater chạy 100% liên tục ~20 phút, ghi nhiệt mỗi phút; lấy độ dốc (°C/phút) ở đoạn đầu |
| `cool_tau_min` | Tắt heater, ghi nhiệt mỗi 5 phút; τ = thời gian để chênh với nhiệt phòng giảm còn 37% |
| `dead_s` | Từ lúc bật heater đến lúc cảm biến bắt đầu tăng (giây) |
| `ambient_c`, `setpoint_c`, `resolution_c` | Nhiệt phòng, nhiệt đặt, độ phân giải cảm biến (0,1 hoặc 0,01) |
| `door_drop_c` | Mở cửa 30 giây, đo nhiệt tụt bao nhiêu |
| `uncertainty` | Khoảng bất định của Kh, τ, trễ, nhiệt phòng; công cụ chạy lưới 3×3×3×N quanh giá trị danh nghĩa |

Chỉ cần Kh và τ là suy ra được công suất giữ nhiệt: `hold = (SP − phòng) / (τ · Kh)`. Báo cáo cảnh báo nếu Kh gần vùng nguy hiểm
(≥ 0,08 °C/s) hoặc lò không đủ công suất (hold > 80%).

## Tiêu chí đạt (cùng bộ kiểm thử firmware)
Không chạm High 38,2 / Emergency 39,0; vọt lố ≤ 0,30 °C; MAE ≤ 0,10 °C; P95 ≤ 0,15 °C; dao động ≤ 0,25 °C (30 phút cuối); ổn định trong hạn.
Lớp `HEAT_LIMITED` = lò vật lý không đủ công suất tới setpoint, không tính là lỗi bộ điều khiển (có cột riêng).

## Lò chậm, cách nhiệt tốt (cấu hình mẫu của lò người dùng)
`tools/oven_sim/oven.slow-incubator.json` mô tả lò "kẹt dưới SP" ngày 15/10: Kh 0,8 °C/phút (0,0133 °C/s), trễ chết 150 s + trễ bậc 1 50 s, hold ≈ 1,9 %, τ nguội ≈ 626 phút, phòng 28 °C, đầu dò 0,1 °C
(suy ra từ nhật ký PID, chưa đo trực tiếp — thay bằng số đo thật khi có). Báo cáo sẽ cảnh báo "Công suất giữ quá nhỏ (<2%)": đúng, đó chính là đặc điểm của lò này.
```
python tools\oven_sim.py --config tools\oven_sim\oven.slow-incubator.json --out oven-report-cham
```
Kết quả hiện tại và nguyên nhân: `doc/SLOW_OVEN_STUDY.md`.

## Chạy AutoTune thật trên lò ảo của bạn
`tools/oven_sim/autotune_family.py` chạy **Smart AutoTune của firmware** (cùng mã, cùng bộ chia xung, cùng bộ kiểm chứng) trên một họ lò ảo mô tả bằng các số đo được, rồi báo: chấp nhận hay từ chối (và vì sao),
thời gian tune, độ lợi/trễ/hold đo được so với mô hình, PID sẽ lưu, và kết quả của lần chạy 3 giờ độc lập sau tune (ACCEPTED_BAD nếu trượt).
```
python3 tools/oven_sim/autotune_family.py --kh 0.0133,0.0144 --dead 120,150,180,210 --lag 50 --hold 0.015,0.019,0.03 --res 0.1
```
`--kh` °C/s ở 100 % (độ dốc đầu của lần chạy full công suất, chia 60 nếu đo °C/phút), `--dead` giây từ lúc bật đến lúc cảm biến nhúc nhích, `--lag` thêm trễ bậc 1, `--hold` tỷ lệ duty giữ SP
(0,019 = 1,9 %), `--cap` J/K, `--amb`, `--sp`, `--res`. Mỗi danh sách được lấy tích Descartes. Chỉ là mô phỏng.

## Chạy một ca lẻ và theo dõi bên trong bộ điều khiển
Tệp nhị phân `tools/oven_sim/.build/oven_sim` (tạo sau lần chạy `oven_sim.py` đầu tiên) nhận các cặp `khóa giá_trị`:
`--eff --cap --loss --dead --lag --amb --res --noise --sp --hours --mode (0 PID cũ, 2 Adaptive, 3 Smart)`,
`--start <°C>` (lò đã ấm khi bắt đầu), `--kp --ki --kd` (PID của người dùng), `--seedGain --seedDelay --seedHold --seedCoast --seedConf` (hồ sơ đã lưu từ trước, ví dụ hồ sơ AutoTune),
`--event door|setpoint_step --eventAt <giờ> --eventValue --spBefore`, `--trace file.csv` (nhiệt độ, duty… theo thời gian), `--debug file.txt --debugFrom <s> --debugTo <s>`
(trạng thái bên trong phanh khởi động và PID: `req`, `I`, `cap`, `peak`, `holdL`, `phase`…). Dòng kết quả: `lớp,chế độ,đạt,high,emergency,vọt,mae,p95,dao_động,ổn_định_s,đạt_SP_s,năng_lượng,confidence`.

## Độ tin cậy của kết quả
* Bước tự kiểm: 3 ca chuẩn so với kết quả đã chạy trên Linux (`tools/oven_sim/reference.json`); báo `OK` hoặc `LECH`.
  Windows/Linux có thể lệch rất nhỏ ở chữ số cuối do thư viện toán; dung sai MAE 0,002 °C.
* Mô hình chưa hiệu chuẩn: hãy chạy thí nghiệm bước công suất trên lò thật và so với `trace_*.csv` để biết sai lệch mô hình.
* Không chạy thử lỗi công suất cao trên lò thật và không thay thế kiểm tra cách ly heater bằng dụng cụ đo.
