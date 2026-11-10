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

## Độ tin cậy của kết quả
* Bước tự kiểm: 3 ca chuẩn so với kết quả đã chạy trên Linux (`tools/oven_sim/reference.json`); báo `OK` hoặc `LECH`.
  Windows/Linux có thể lệch rất nhỏ ở chữ số cuối do thư viện toán; dung sai MAE 0,002 °C.
* Mô hình chưa hiệu chuẩn: hãy chạy thí nghiệm bước công suất trên lò thật và so với `trace_*.csv` để biết sai lệch mô hình.
* Không chạy thử lỗi công suất cao trên lò thật và không thay thế kiểm tra cách ly heater bằng dụng cụ đo.
