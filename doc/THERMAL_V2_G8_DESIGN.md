# Thermal V2 — G8: sửa theo log lò thật 24/10/2026 (kẹt dưới SP, dự đoán không bám thực tế, quạt hút)

Firmware 2.0.2. **Chỉ kiểm chứng trong mô phỏng + CI; chưa thử lò thật.** Mọi số dưới đây là mô phỏng, không phải đảm bảo cho lò thật.

## 1. Log lò thật nói gì (`mayap-…-20261024T014802.csv`, lò 16–20 kW, xe đầy, chưa trứng)

| Quan sát | Số đo |
|---|---|
| Lên nhiệt từ 31,7 °C | 15,5 phút tới 37,49; đỉnh vọt chỉ **+0,10 °C** (bản 2.0.0: +0,40 và +0,70) |
| Sau khi vào SP | PV tụt xuống 37,11–37,40 và **kẹt 17+ phút** với yêu cầu trung bình 14,8 % |
| Dự đoán lúc kẹt | cao hơn PV trung bình **+0,12…+0,14 °C** liên tục, đỉnh nhảy 37,6 khi PV 37,2–37,4, độ lệch chuẩn 0,075–0,11 °C |
| Quạt hút | tắt suốt khoảng kẹt (chỉ chạy 3 phút ở 01:25 do vạch nhiệt và lúc 01:47:53 do thông gió tự động vừa bật) |
| Khi thả chạy ngầm (01:28:11) | yêu cầu vọt 100 % trong ~30 s |

## 2. Nguyên nhân (từ log + mã) và cách sửa

| # | Nguyên nhân | Sửa (công tắc `V2_Fn`, chỉ dùng trên máy chủ) |
|---|---|---|
| A1/A2 | Luật dùng `e = SP − PV_dự_đoán` cho cả P và I: khi dự đoán ≈ SP mà PV thấp hơn 0,3 °C thì sai số bằng 0 và tích phân đứng yên. Không có vòng đối chiếu dự đoán với PV đo | **F11** trọng số tin cậy `w` ∈ [0,1]: `PV_dùng = PV + w·(dự đoán − PV)`; `w` giảm khi, lúc không khí đứng yên và công suất đến ổn định, dự đoán lệch PV kéo dài (EMA 300 s). Chỉ áp dụng trong dải PV ∈ [SP−0,5; SP+0,3] và luôn tôn trọng dự đoán nếu nó báo vượt SP > 0,15 °C. Far mode và phanh vẫn dùng dự đoán nguyên bản |
| A3 | Độ dốc 60 s dùng làm nhu cầu đo (F1): cảm biến nhảy 0,1 °C cho độ dốc giả ~2 mK/s, `D_MIN = −60 %` biến thành +0,3 °C dự đoán | **F10C** cận dưới độ tin cậy của độ dốc (trừ 3 lần sai số chuẩn của đường hồi quy 60 s) |
| A4 | `ASYM = 20`: nhiễu ±0,15 quanh SP bị khuếch đại thành lệch trung bình thấp | (đã thử giảm khi PV chưa quá SP; mặc định giữ 20, xem mục 5) |
| A5/A6 | Tích phân kẹt và mức nhu cầu học chậm | **F10A** tích phân theo PV đo nhanh hơn 8 lần khi kẹt (lệch ≥ 0,1 °C, độ dốc không đóng được lệch trong 15 phút, đã chạm SP một lần, công suất đến chưa vượt mức giữ +15 %) sau 7 phút; cổng slowUp 0,1× mở khi đã kẹt |
| C1–C3 | V2 không có đầu vào quạt hút. Thông gió theo biểu đồ làm tổn thất tăng mà V2 chỉ học qua độ dốc, chậm vài phút | **F12** đầu vào `ventFan` + thời gian tới lúc quạt bật/tắt (từ biểu đồ). Học riêng `dFan` (tổn thất do quạt, 0…60 %) khi quạt chạy, không trộn vào mức nhu cầu của lò. Bù trước một thời gian trễ: mô hình nhu cầu dùng trạng thái quạt tại lúc công suất yêu cầu bây giờ sẽ tới nơi (cho FF) và tỷ lệ quạt chạy trong tầm nhìn dự đoán (cho dự đoán). RLS và quan sát trứng nghỉ khi quạt chạy |

## 3. Kiến trúc: một vào, một ra, một quyết định

Khối quyết định nhiệt vẫn là **một hàm**: nhận một `Input`, trả **một số** công suất 0…cap % (kèm nhãn trạng thái và số tin cậy chỉ để hiển thị, không đọc ngược). Lớp "quản lý" nằm **bên trong** khối này:

* `trust_` (F11) quyết định luật tin dự đoán bao nhiêu; `stuckS_` (F10A) quyết định có tích phân theo PV đo không; các trạng thái `Recover` (7) và `Degraded` (8) chỉ là nhãn để người xem log biết khối đang ở chế độ nào.
* **Không** có kênh ra thứ hai. Lớp bảo vệ (High, Emergency, cảm biến, E115, contactor, SSR cấm, vạch quạt hút) không đổi, không đọc `trust_` hay `dFan_`, và vẫn chỉ có quyền chặn.
* Quyền tăng công suất vẫn chỉ thuộc V2. Quản lý không có quyền vượt cap, không bỏ qua `permit`.

## 4. Log và giao diện

* Mẫu log PID (`spare`): 4 bit thấp = trạng thái V2 (0…8), 4 bit cao = độ tin cậy dự đoán (0…15). CSV thêm cột `trust_pct`.
* Trạng thái mới trên PID Monitor: `recover`, `degraded`.
* Dòng đếm ở thẻ PID Monitor nay ghi đúng: số mẫu trang đã nhận (có thể lớn hơn 180 vì trang giữ cả phần cũ), máy giữ 30 phút gần nhất.

## 5. Điều đã thử và bỏ / để lại

* **Bỏ khỏi mặc định:** F10B (không xả tích phân khi PV dưới SP): đẩy một ca High ở cửa 5 phút (lò E20/E35, đã sát vạch 38,2). F10D (neo mức có trừ độ dốc trong cửa sổ "phẳng") và F10E (cổng slowUp chỉ cho sai lệch lớn): không đổi kết quả đo được.
* **Giảm `ASYM`:** chưa làm mặc định, vì giảm bảo vệ vọt; chưa có lợi ích đo được tách riêng khỏi F10C.
* **Khối nhiệt chậm** (thép xe/vách): chưa làm. Mô hình vẫn một khối trứng; đây là nguyên nhân khả dĩ của việc nhu cầu giữ đổi 43 → 15 % trong 20 phút ở log 24/10. Chỉ nên làm nếu log lò thật sau bản này cho thấy còn lệch.

## 6. Kết quả mô phỏng

(Điền sau khi lưới cuối chạy xong; xem `tools/thermal-v2/g8_results.txt`.)
