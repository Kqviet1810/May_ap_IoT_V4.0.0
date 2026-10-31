# Điều khiển nhiệt V3 — bản 2.5.1: quạt hút là phương án cuối, log có cột chẩn đoán

Bản này không đổi luật điều khiển nhiệt (`thermal_v3.h` giữ nguyên như 2.5.0). Nó đổi **luật bật quạt hút khi quá nhiệt** và **thêm cột chẩn đoán vào log PID**, theo log chạy thật ngày 31/10 (lò không tải, chỉ khay trứng rỗng, 10:16–13:36 giờ Việt Nam, trưa nóng).

## 1. Điều đọc được từ log 31/10 (có thể kiểm lại bằng file CSV)

| # | Hiện tượng | Nguyên nhân (mức chắc chắn) |
|---|---|---|
| 1 | Nhu cầu giữ nhiệt rơi từ 15 % (10:46) xuống 1 % (12:45) trong khi PV đứng đúng 37,50 | Có bằng chứng: ba đoạn PV phẳng 11–22 phút ở công suất không đổi (15,4 %, 2,1 %, 1,1 %) — công suất đó chính là nhu cầu lúc ấy, sai số dưới 1 %. Bộ điều khiển đo đúng, không "ép" xuống. Nhiệt độ phòng không được đo nên đây là suy luận. |
| 2 | 13:20–13:36, PV bám 37,6 (SP+0,1) với heater 0 % mà quạt quá nhiệt không bật | Luật 2.5.0 đòi "PV ở trên ngưỡng **liên tục** một phút", chỉ cần một mẫu rơi dưới ngưỡng là đếm lại. Cảm biến chỉ có bậc 0,1 °C: khi nhiệt độ thật nằm giữa hai bậc, số đọc nhấp nháy 37,5 ↔ 37,6 và luật không bao giờ thấy "đủ một phút". Replay luật cũ trên PV đã ghi cho thấy nó đáng ra bật lúc 13:22:44, máy thật không bật, nên số đọc thô (`safetyTemp`) khác PV đã lọc trong log — log không có cột này để phân xử. |
| 3 | 12:22–12:41, PV trung bình 37,434 (thấp 0,07) trong 19,5 phút khi nhu cầu tăng 2 → 5 % | Giả thuyết (chưa kiểm vòng kín): luật chống kẹt cần sai lệch ≥ 0,095 liên tục, số đọc nhấp nháy nên bộ đếm về 0. **Bản này không sửa** — mục này đang chờ số liệu. |
| 4 | Sau 3 trong 4 lần quạt hồ sơ tắt, PV tăng thêm +0,07 đến +0,1 trong 1–2 phút | Chưa rõ là nhiệt thật hay hiệu ứng vị trí cảm biến. Cần phép thử có kiểm soát (bật quạt qua Web 5 phút khi PV phẳng). **Bản này không bù trước.** |
| 5 | Cú tụt 13:05 (−0,4 °C trong 90 giây) rồi hồi phục về 37,5 trong 5 phút 40 giây, công suất cao nhất 19 %, không vọt | Bộ điều khiển hoạt động đúng: PV dự đoán đã phanh ở 13:09:24 khi PV đo mới 37,38. Chỉ đúng cho lò không tải. |

## 2. Thay đổi

### Luật quạt quá nhiệt (`vent_line.h`, `config.h`, `machine_control.h`)

Quạt là **phương án cuối**, không phải bộ điều chỉnh, và không bao giờ cắt heater.

* **Quyết định trên giá trị trung bình** của vài chục giây số đọc, không trên "liên tục trên ngưỡng". Số đọc R nghĩa là nhiệt độ thật nằm trong R ± nửa bậc, nên ngưỡng "quá" là `ventOnTemp − nửa bậc`; trung bình các số đọc vượt ngưỡng đó đúng lúc nhiệt độ thật vượt.
* **Cửa sổ quan sát** `W = clamp(1,2 × τ, 150 s, 260 s)` (τ là độ trễ truyền nhiệt do bộ điều khiển đo; chưa đo thì 180 s). Heater đã có thời gian giảm; một cú cắt không thể hiện ra trước τ. Nếu trung bình giảm ≥ 0,05 °C thì mở cửa sổ mới từ mức thấp hơn (bộ điều khiển đang kéo xuống: không quạt).
* **Bật quạt** khi một trong bốn điều kiện (và quạt đã nghỉ ≥ 5 phút):
  * kẹt: hết cửa sổ mà trung bình không giảm;
  * nhanh: trung bình vượt thêm 0,1 °C trong 20 giây;
  * dự báo: bộ điều khiển dự báo vượt ngưỡng thêm 0,2 °C trong lúc PV đã vượt (nhiệt dư đang tới);
  * heater chạm sàn: trung bình heater ≤ 3 % trong 60 giây và PV đã vượt ≥ 150 giây (không còn gì để giảm).
  Dự báo của bộ điều khiển còn **hoãn** hai điều kiện kẹt và sàn: khi nó nói nhiệt độ sẽ xuống dưới ngưỡng (cú cắt đang đi tới cảm biến), luật chờ tối đa hai cửa sổ. Điều kiện nhanh và dự báo không bao giờ bị hoãn.
* **Tắt quạt** khi trung bình về ≤ ngưỡng tắt trong 20 giây và quạt đã chạy ≥ 3 phút; chạy tối đa 10 phút (còn cao sau đó thì đường Cao/Khẩn cấp lo, không bị giới hạn).
* **Bảo vệ relay:** nghỉ ≥ 5 phút; **thùng token** 3 lần ngay, rồi 1 lần mỗi 30 phút (trung bình 2 lần/giờ). Một lần bật chỉ tốn token khi quạt đang dừng: nếu quạt hồ sơ đang chạy thì luật tiếp quản không tốn gì; nếu khối hồ sơ sắp chạy trong 5 phút thì chờ khối đó (trừ khi vượt quyết liệt).
* **Mã lý do** (`VentLine::Why`): 1 kẹt, 2 nhanh, 3 heater chạm sàn, 4 dự báo; cộng bit trạng thái: đang quan sát, đang chạy, hết ngân sách, chờ khối hồ sơ, chạy quá 10 phút. `ventReason` hiện có cho Web vẫn là 3 ("quá nhiệt"), nên Web 1.1.26 hiển thị như cũ.

### Log PID có cột chẩn đoán (`pid_log.h`, `transaction_bridge.h`, Web 1.1.27)

Mỗi mẫu 10 giây mang thêm: số đọc an toàn (`safety_c`), hold đã học (`hold_pct`), hệ số gia nhiệt (`kair_e5`), độ trễ τ (`tau_s`), tích phân (`integral_pct`), lý do và trạng thái đường quạt (`vent_why`, `vent_line`), cờ cửa (`door`), đã đo τ (`identified`), số token (`fan_tokens`), `vent_reason`. 17 cột cũ giữ nguyên vị trí; cột mới thêm ở cuối.

Máy gửi phần chẩn đoán trong **gói riêng xen kẽ** với gói số liệu cũ (khung tin không lớn hơn trước). Web cũ bỏ qua gói này; Web 1.1.27 ghép theo chỉ số mẫu và để trống cột mới nếu máy cũ. Hệ quả: thu đủ 30 phút log mất gấp đôi thời gian (khoảng 2 phút → 4 phút), giữ thẻ PID Monitor mở lâu hơn.

## 3. Kiểm chứng

**Test đơn vị** `tests/vent-line.cpp` (chạy đúng mã `vent_line.h`, 13 nhóm): số đọc đứng trên ngưỡng một bậc và không giảm → bật sau cửa sổ (≈ 190 s với τ = 150 s); số đọc đang giảm → không bật; số đọc nhấp nháy 37,5/37,6 (nhiệt thật 37,57) → bật, còn nhiệt thật 37,53 → không bao giờ; vượt thêm 0,1 → bật sau ≈ 27 s; heater ở sàn → bật sau ≈ 165 s (thay vì 275 s khi heater còn làm việc); dự báo cao → bật sau ≈ 20 s, dự báo thấp → hoãn tới hết cửa sổ thứ hai, nhưng vượt nhanh không bị hoãn; chạy tối thiểu 180 s, tối đa 600 s, nghỉ 300 s; sáu giờ liên tục quá nhiệt → 14 lần bật (3 + 1 mỗi 30 phút); quạt hồ sơ đang chạy → tiếp quản không tốn token, khối hồ sơ sắp tới → chờ; mất cảm biến; tràn đồng hồ 32 bit; cảm biến 0,01 °C. Các bộ test khác giữ nguyên, **không nới**: `thermal-v3`, `thermal-closed-loop` (kể cả lò khớp log 29/10), `thermal-replay`, `heater-response-watch`, `runtime-*`, `test_heater_path`, `test_tech_access`, `check_reliability`, `node --test tests/*.test.cjs` (301 đạt, 0 lỗi). Dấu vân tay `config.h` và `MachineController` trong `tests/runtime-preservation.json` được ghim lại có ghi chú xem xét (`release251ReviewNote`).

**Bench 210 lò ngẫu nhiên × 8 tình huống** (`tools/thermal-research/fleet-bench.cpp`, so sánh bằng `fleet-summary.py`; A = luật 2.5.0, B = luật mới; hai tình huống mới: phòng nóng thêm 7 °C trong 2 giờ làm hold rơi còn vài %, và hồ sơ thông gió 70 %). Ngưỡng quạt 37,6 / 37,5 như máy thật.

| Quạt 100 W (giống máy) | Đạt A | Đạt B | Số lần bật A: trung vị / p90 / max | B |
|---|---|---|---|---|
| lên nhiệt + giữ | 97 % | 97 % | 1 / 1 / 3 | 1 / 2 / 3 |
| quạt lịch | 96 % | 95 % | 1 / 1 / 3 | 1 / 2 / 3 |
| vượt 1 bậc | 84 % | 86 % | 2 / 2 / 4 | 2 / 3 / 4 |
| mở cửa 3 phút | 94 % | 96 % | 1 / 1 / 3 | 1 / 2 / 3 |
| cửa + khay lạnh | 92 % | 92 % | 2 / 3 / 5 | 3 / 4 / 5 |
| khởi động lại | 84 % | 85 % | 2 / 2 / 5 | 2 / 3 / 5 |
| phòng nóng lên | 100 % | 100 % | 1 / 1 / 3 | 1 / 3 / 5 |
| hồ sơ 70 % | 100 % | 100 % | 1 / 1 / 3 | 1 / 2 / 3 |
| **trung bình** | **93,4 %** | **93,9 %** | | |

Lên nhiệt lần đầu 31 / 40 phút (trung vị / p90) và vọt lần đầu +0,2 / +0,4 °C: giống hệt A. Tổng sai lệch tích lũy theo từng lò (B so với A): trung vị +0,1 %, p10 −3,7 %, p90 +3,6 %; 11 lò kém hơn trên 5 %, 13 lò tốt hơn trên 5 %. Nói cách khác, với quạt yếu **luật mới trung tính về nhiệt** và số lần bật gần bằng 2.5.0 (p90 cao hơn một lần ở vài tình huống).

Với quạt mạnh 2,5 kW (trường hợp thử độ nhạy, không phải máy thật): trung bình đạt A 68,7 %, B 66,8 %; kém nhất ở "cửa + khay lạnh" (88 % → 79 %); tổng sai lệch theo lò trung vị +0,0 %.

*Phiên bản trung gian bị loại:* bản đầu của luật mới (chưa có điều kiện hoãn theo dự báo) làm số lần bật tăng gấp đôi (trung vị 2, p90 4 mỗi 250 phút ở "lên nhiệt + giữ") và tụt xuống 56 % với quạt mạnh, vì nó bật quạt khi PV đang tự hạ nhờ hold học giảm dần. Điều kiện hoãn theo dự báo của bộ điều khiển đưa số lần bật về mức 2.5.0.

**Replay luật trên PV thật của 12 log đã ghi (13 file, một file trùng)** (mở vòng, chỉ cấp PV; PV đã chịu ảnh hưởng của luật chạy lúc ghi). Log 31/10: luật mới bật lúc 10:40, 11:14, 12:03 và **13:22:54** (heater chạm sàn, đúng chỗ máy thật không bật quạt dù PV bám 37,6 trong 8 phút); luật 2.5.0 trên cùng PV bật 4 lần với tổng 26,8 phút, luật mới 4 lần, 22,8 phút. Trên các log còn lại luật mới bật ít hơn hoặc bằng luật 2.5.0, trừ hai log 23/10 (thời 2.3.x, PV dao động mạnh): 8 so với 6–7 lần bật.

**Mục 8 (luật chống kẹt dùng sai lệch trung bình) thử trên bench, không đưa vào bản này.** Kịch bản "phòng lạnh đi 5 °C trong 2 giờ" (hold tăng): IAE trung vị 7,95 → 8,00 °C·phút; 122 lò kém hơn, 24 lò tốt hơn (ngưỡng 2 %). Không có lợi ích đo được nên giữ nguyên `thermal_v3.h`.


## 4. Giới hạn (nói thẳng)

* **Bằng chứng mô phỏng không phải bằng chứng phần cứng.** Quạt mô phỏng 100 W lấy đi rất ít nhiệt; hiệu quả của quạt thật chưa đo được. Log 31/10 là lò không tải; lò đầy trứng chưa có log.
* **Số lần bật quạt tăng so với 2.5.0** ở quạt yếu (xem bảng). Phần lớn là các lúc PV bám +0,1 trong khi heater đang giảm dần — đúng chỗ luật cũ "mù". Trong khuôn khổ ngân sách (trung bình ≤ 2 lần/giờ) nhưng không phải "ít hơn". Quạt yếu không làm PV hạ thêm nên lợi ích nhiệt của những lần bật này gần như bằng 0 trong mô phỏng.
* **Quạt mạnh (2,5 kW)**: xem bảng; luật mới kém luật 2.5.0 ở mô phỏng này. Máy này dùng hai quạt 45 W nên không phải trường hợp thật.
* Chưa làm: bù trước khi quạt tắt (mục 9) và luật chống kẹt dùng sai lệch trung bình (mục 8): hoãn theo quyết định của chủ máy.
* Gói chẩn đoán làm log chậm gấp đôi và thêm 1,4 KB RAM cho vòng log (180 mẫu × 8 byte).
* Chưa biên dịch cho ESP32 trong môi trường làm việc (không có lõi ESP32); kiểm tra này do CI làm.
