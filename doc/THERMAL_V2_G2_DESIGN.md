# Thermal V2.0.0 — thiết kế đã chốt ở G2 (thay cho §2–§3 của `THERMAL_V2_CONSTANTS.md`)

Nhánh `thermal-control-v2.0.0`. Mã: `MAYAP_INDUSTRIAL_v1_0_0/thermal_v2.h` (C++ thuần, RAM ~12 KB, không NVS). Thử trên lò ảo: `tools/thermal-v2/` (**chỉ mô phỏng**, không phải bằng chứng cho lò thật).

## 1. Vì sao thiết kế G0 phải đổi
Đặc tả G0 dùng `u_cb = [UA(PV−Tamb) + hA(PV−Te)]/Pmax·100` và nhận dạng k từ độ dốc leo nhiệt. Khi code, hai điều không thành:
1. Firmware **không có** Tamb, UA, hA, Pmax. Các số này phải học.
2. Nhận dạng k trong lúc leo nhiệt bị lệch 20–40 % vì không biết nhu cầu lúc đó (nhu cầu và k không tách được từ một độ dốc). Thời gian đến lần tăng 0,2 °C đầu tiên cũng chứa cả thời gian dâng của lag, không chỉ độ trễ vận chuyển.

Kiểm bằng phân tích ngoại tuyến trên dữ liệu rig (hồi quy bình phương tối thiểu với đúng độ trễ/lag) cho thấy cấu trúc mô hình đúng và k ra đúng (±3 %). Mọi sai lệch trước đó đến từ **độ trễ/lag sai** khi dựng lại công suất đến không khí. Từ đó có thiết kế dưới đây.

## 2. Mô hình bộ điều khiển tin về lò
`d(PV)/dt = k·P_air − [c0 + cp·(PV−P0) − ce·(Te−P0) − c3·w(t)]`, P0 = 35 °C chỉ để căn tâm.
- `P_air` = công suất đã giao thật (từ bộ chia xung) sau trễ vận chuyển `td` và lag `τ` (dựng lại mỗi giây từ lịch sử).
- Nhu cầu `D(PV) = 100·[…]/a`, a = 100·k. Tương đương `[UA(PV−Tamb)+hA(PV−Te)]/Pmax·100`: tổn thất tăng theo PV, trao đổi với trứng theo PV−Te.
- Trứng: một trạng thái `Te' = (PV−Te)/τe` chạy song song cho 3 ứng viên τe ∈ {1000, 1500, 2200} s; lệch nhiệt độ ban đầu `δ = c3/ce` suy giảm theo `w = exp(−t/τe)` (trứng nóng sau mất điện).

## 3. Học gì, khi nào
| Giai đoạn | Việc | Vì sao |
|---|---|---|
| 0–90 s | **Nhìn**: heater tắt | độ dốc PV = −k·nhu cầu; PV tự tăng = trứng ấm |
| Sau đó | **Thăm dò công suất an toàn** `P = 12 + 0,8·(chỗ trống − 1 °C)/(k_max·T_max)` (≥ 20 %) cho tới khi thấy tăng 0,2 °C (độ trễ) | trước đó bộ điều khiển mù; năng lượng cấp trong lúc mù không được phép đẩy khí quá SP kể cả ở lò nhanh nhất của họ lò |
| Lần cấp điện đầu + 600 s | **Batch fit** (khả năng tiếp tục, 1 ứng viên = 3 lần gọi): quét `td` 20–260 s (bước 10) × `τ` {25,35,45,55} × `τe` {1000,1500,2200}; mỗi ứng viên giải ridge 5 ẩn (k, c0, cp, ce, c3) | tìm trễ/lag/τe trên lịch sử 960 s, giải phần còn lại bằng bình phương tối thiểu có tiên nghiệm |
| +1500 s | Batch lần 2, lấy kết quả lần 1 làm tiên nghiệm | một mức giữ nhiệt tĩnh không được phép lật lại điều mà lần leo nhiệt đã đo |
| Liên tục | RLS bốn tham số theo độ dốc 60 s, cổng loại mẫu bất thường 4σ, **k đóng băng** giữa hai batch | giữ hệ số tổn thất theo thời gian; k không tách được khỏi tổn thất khi giữ tĩnh |
| Liên tục | **Neo mức**: trong đoạn PV phẳng, công suất đến = nhu cầu (không phụ thuộc k); mức mô hình kéo dần về đó, cùng lượng đó rút khỏi I | triệt sai lệch xác lập mà không cần biết k |

## 4. Luật điều khiển
`u = clamp(FF + K·(SP − PV_pred) + I, 0, cap)`.
- `FF = D(SP) − (1−β)·ha·(SP−Te)`, β = 0,85.
- `PV_pred = PV + k·(E_transit − D(PV_mid)·T_transit)`, `T_transit = td + τ`; nhu cầu tính tại giữa quỹ đạo dự đoán.
- `Kc = 1/(3·k·θ)` (SIMC, τc = 2θ), θ = td + τ/2 + 6 s; K = Kc dưới SP, 3·Kc trên SP; `τI = 6θ`; tích phân có điều kiện, |I| ≤ 30.
- **Chế độ xa**: dự đoán thiếu hụt > `max(0,25 °C, 1,5·σ_pred)` ⇒ full công suất cho tới khi thiếu hụt < một nửa ngưỡng; `σ_pred` tính từ độ bất định tương đối của k và ma trận hiệp phương sai của mô hình nhu cầu ⇒ ngân sách vọt là hệ quả thiết kế, không phải hằng số chỉnh tay.
- E115: khi `e115Frac ≥ 0,4` và PV < SP, cộng thêm tối đa 25 điểm theo tỉ lệ (không nhảy 100 %).
- **Bộ giới hạn cuối** (độc lập với bộ học, chỉ dùng PV đo và hằng số cố định của RG144): chỉ hoạt động khi PV ≥ SP, hạ u để không chạm đường Emergency − 0,4 °C. Không bảo vệ đường High (38,2 °C) — đường vent chỉ cách SP 0,5 °C, dùng hằng số xấu nhất sẽ chậm mọi lần lên nhiệt.

## 5. Quyết định về ngưỡng vent
Giữ nguyên cấu hình sản phẩm (`ventOnTemp` 38,0 °C, High 38,2 °C, Emergency 39,0 °C). Không đổi ngưỡng để làm đẹp kết quả mô phỏng; mọi đánh giá chạy với đúng giá trị sản xuất. Hệ quả: vọt trên 0,5 °C kích hoạt vent và làm PV dao động (kịch bản S3).

## 6. Đã thử và bỏ (kèm lý do, để không lặp lại)
| Thử | Kết quả |
|---|---|
| Bộ theo dõi sai số nhanh song song với I | R5.door 0/12 |
| Chuyển giao bumpless I ↔ mô hình theo mọi cập nhật | I trôi về −23, luật mất tác dụng; chỉ giữ chuyển giao ở neo mức |
| Dự đoán dùng độ dốc PV đo thay cho mô hình | dao động khi công suất đổi trong cửa sổ trễ |
| Bù I vào nhu cầu của bộ dự đoán | thêm vòng hồi tiếp dương, R3 xấu đi |
| Quan sát trạng thái trứng từ innovation của độ dốc | cho cả cửa mở/lòng vent lẫn thay xe trứng nên làm sai Te; R5.door 1/12 → 7/12 sau khi chặn, vẫn kém hơn khi tắt. Tắt (`EGG_OBSERVER=false`) |
| Vùng chết trên tác động tỉ lệ vì cảm biến 0,1 °C | kém hơn |
| Giới hạn công suất theo năng lượng đang bay (hằng số xấu nhất) | làm chậm mọi lần lên nhiệt, R1 từ 24/24 xuống 4/24 |

## 7. Kết quả set A cuối G2 (mô phỏng; số liệu trong `tools/thermal-v2/g2_results.txt`)
Đạt: bốn cổng an toàn 132/132 (không Emergency, không E115, không High, không lệnh sưởi không an toàn), R1.reach 23/24, R2.high 132/132, R4 12/12 (tail và under), R5.door 12/12, R5.tray 24/24, R7.spstep 12/12.
Chưa đạt: R2.over 54/84, R3.band01.cold 19/48, R3.band01.warm 7/12, R3.band03 23/24, R1.reach.restart 0/12 (viết đúng đặc tả; 12/12 nếu nới +720 s), R7.spstep.settle 11/12. CPU.slot và R6 chưa đánh giá (G4/G5).

## 7b. Thay đổi sau rà soát độc lập (3 lượt)
- Cổng kiểm tính hợp lý kết quả batch fit (mép lưới, k ngoài khoảng, rms) + thử lại 3 lần/300 s; không chấp nhận thì giữ tiên nghiệm.
- `UNIDENTIFIED_CAP`: chưa có batch fit được chấp nhận thì công suất bị chặn ở mức thăm dò an toàn (đổi lại: R1.reach.restart không đạt trong 40 phút vì không có NVS).
- Kiểm tra đầu vào (NaN/Inf, dt, sp/pv bất thường) và làm sạch đầu ra; sửa lỗi batch kích hoạt hai lần, lệch vòng đệm, công suất thăm dò, thành viên chưa khởi tạo.
- Test đơn vị 21 kiểm tra (`run_grid.py unit`, chạy trong CI).
- Đã thử và hoàn nguyên: giữ lịch sử qua khoảng mất permit ngắn, cờ khoảng hợp lệ (S3/emergency xấu đi); trần mềm ở đường vent (chậm leo nhiệt); `LIM_THETA_S` 300 (không sửa được ca ngoài họ, làm R3.band03 và R7 xấu hơn) → giữ 180.

## 7c. Giới hạn thật (không khắc phục được trong G2)
1. Lượng tử hoá cảm biến 0,1 °C + đường vent chỉ cách SP 0,5 °C: vọt >0,5 °C kích hoạt vent; R2.over/R3.band01 bị chặn bởi phân giải, không phải bởi luật.
2. Khởi động lại không có NVS: phải học lại ~10 phút trước khi công suất mở hết ⇒ R1.reach.restart. Hướng: lưu tham số học vào NVS (G5) hoặc nới tiêu chí.
3. Lò ngoài họ (trễ 250 s, ví dụ `X_td250`) ở S3 vẫn chạm High/Emergency khi mất permit lặp lại: bộ học không đủ dữ liệu, bộ giới hạn cuối chỉ có hằng số RG144.
4. Mất permit lặp lại làm batch không chạy được ⇒ chạy bằng tiên nghiệm bảo thủ.
5. `ha` và `τe` nhận dạng yếu; nhiễu bước của trứng (đổi xe) không mô hình hoá.
6. Set A là tập thiết kế; bằng chứng tổng quát hoá chỉ có ở B/C (G3). Rig chưa có nhiễu cảm biến/jitter.

## 8. Việc đo ở G5
`CPU.slot` ≤ 2 ms trên ESP32-S3: ước tính theo số phép tính, mỗi lần gọi ≤ ~0,5 ms (một ứng viên batch = 3 lần gọi: dựng lại đường heater ~960 bước float; hàng hồi quy + phương trình chuẩn; giải 5×5 double). Phải đo thật bằng `esp_timer` ở G5.
