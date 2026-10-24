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

## 6. Kết quả mô phỏng (G7 → G8)

Tiêu chí tiêu chuẩn (các kịch bản S1…S9 mặc định) **không đo mục tiêu của G8**, nên chỉ kiểm không tệ đi:

| | A+D+E (88 lò, dev) G7 → G8 | B (hạt mới 20261225) + C (held-out) G7 → G8 |
|---|---|---|
| Cổng an toàn (High / Emergency / unsafe / E115) | 968/968 → 968/968 | High 890/891 → **891/891**; C8cal High 29/33 → 30/33; còn lại không đổi |
| Tiêu chí không đạt (tổng) | 436 → 439 | 573 → 573 |
| R2.over (vọt ≤ 0,2) | 549 → 540 /616 | 509 → 500 /567 |
| R7.spstep.settle | 71 → 76 /88 | 60 → 64 /81 |
| R5.door | 66 → 67 /88 | 50 → 53 /79 |
| R4.tail | 83 → 85 /88 | 67 → 70 /81 |

Kịch bản đo đúng cái G8 sửa (bộ giữ riêng B và C, G7 → G8, tỷ lệ % thời gian cuối trong ±0,1 °C):

| Kịch bản | B | C |
|---|---|---|
| S15a thông gió tự động 10 % chu kỳ | 82,3 → 88,7; High 2 → 0 | 85,1 → 85,7; High 2 → 0 |
| S15b thông gió 25 % | 65,0 → 86,9; High 4 → 0 | 69,7 → 89,1; High 3 → 1 |
| S15c thông gió 50 % | 37,0 → **86,3**; High 3 → 0 | 32,1 → **90,8**; High 3 → 0 |
| S14b nhu cầu tăng chậm 20 % | 85,8 → 92,8 | 86,8 → 91,8 (một ca High mới) |
| S11b cửa 5 phút | 97,6 → 98,5; High 0 → 0 | 89,2 → 91,4; High 0 → 0 |
| S13a cửa hé 15 phút | 94,2 → **87,7**; High 5 → 5 | 96,9 → **93,5**; High 8 → 9 |

Trên tập dev E: S14a/b/c giữ nhiệt 86 → 95, 81 → 90, 71 → 83 %; S15c 30 → 94 %.

**Cái giá và giới hạn:**
* Ca cửa hé lâu (S13a/b/c) kém đi chút ít ở độ ổn định sau khi đóng cửa, số ca High không đổi hoặc +1; dạng này luôn là điểm yếu (xem G7).
* Hai lò E20/E35 (cảm biến trễ 90 s) ở cửa 5 phút vẫn nằm sát vạch High 38,2 °C ở cả G7 và G8; lệch vài phần trăm độ có thể đẩy qua.
* S14b ở bộ C có thêm một ca High nhỏ (vọt +0,70).
* Khối V2 chỉ mô hình hóa quạt mà **lịch thông gió tự động theo chu kỳ báo trước**. Quạt không báo trước (thông gió theo danh sách giờ, vạch nhiệt, Web) được bỏ qua như G7: học tổn thất từ một lần chạy lẻ không đáng tin (thử trong mô phỏng làm kém đi 10 điểm ở kịch bản quạt theo giờ S6a, nên đã loại). Sau mỗi lần quạt đổi trạng thái khối nghỉ học mức nhu cầu 60 s.
* Mô phỏng chưa có khối nhiệt chậm của thép xe; bản này chưa thử trên lò thật.

## 7. Cách thử trên lò (bổ sung cho `doc/FIELD_TRIAL_V2.md`)

1. Lên nhiệt từ nguội, mở PID Monitor trước vài phút. Cột `trust_pct` ở CSV phải ở 100 % khi PV đứng yên đúng SP; nếu nó tụt dưới 50 % kéo dài trong khi PV lệch SP, đó là dấu hiệu mô hình sai mức nhu cầu (ghi lại, gửi log).
2. Giữ ổn định ≥ 90 phút, xem PV có còn kẹt dưới SP > 0,1 °C quá 10 phút không (trước đây: kẹt 17+ phút ở 0,1–0,4 °C).
3. Bật thông gió tự động (chu kỳ 40 phút), chạy ≥ 2 chu kỳ; so PV lúc quạt chạy và sau khi quạt dừng.
4. Mở cửa 5 phút rồi đóng, như các lần trước.
