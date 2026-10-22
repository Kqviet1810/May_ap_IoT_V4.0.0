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

## 9. G3b — độ phân giải cảm biến và bộ quan sát trứng (mô phỏng)
- Cảm biến thật: mô-đun SHT485 (SHT30 qua RS485), firmware hỗ trợ X10 (0,1 °C) và X100 (0,01 °C), chọn ở HMI "DPG cam bien" hoặc tự nhận dạng. Repo không cho biết mô-đun thật đang xuất định dạng nào ⇒ phải đọc trên máy (xem báo cáo G3b). Bộ lọc trung vị-3 + IIR 3/8 làm mịn dưới bậc cảm biến.
- Chạy tập A với bậc 0,01 (`V2_DQ=0.01`): R2.over 64/84, R3.band01 lạnh 25/48, ấm 9/12, R7.settle 12/12, bốn cổng an toàn 132/132; R5.door 9/12 (kém hơn 12/12 ở bậc 0,1). Số liệu: `tools/thermal-v2/g3b_dq001_results.txt`. ⇒ Độ phân giải giải thích phần nhỏ; phần lớn sai lệch là động học khay trứng/quạt vent và mô hình trứng (ha, Te) nhận dạng yếu.
- Thử trong G3b (đều trên tập A, hai độ phân giải, đo lại toàn lưới): neo mức nhanh hơn/rộng hơn (LEVEL_GAIN 0,10; LEVEL_FLAT_C 0,30), giữ bộ học khi innovation cùng dấu liên tiếp, trần mềm ở đường vent: không có cải thiện rõ hoặc xấu hơn ⇒ bỏ.
- Bộ quan sát trứng chỉ bật khi dQ ≤ 0,02 (`EGG_OBSERVER_FINE`): bậc 0,01 cho R2.over 74/84, band01 lạnh 32/48, R5.door 11/12, nhưng GATE.high 131/132 (A10/S5b, đỉnh 38,315 °C trong 181 s, không Emergency). Thử giảm hệ số, giảm bước, bỏ trong chế độ xa: cổng High vẫn 130–131/132. (Đã đảo ở G3c bên dưới.)

## 10. G3c — thay đổi chốt trước G4/G5 (mô phỏng)
Tập phát triển mới D (40 lò, hạt giống 4242) để tinh chỉnh cùng tập A; B (đổi hạt giống 20261105 vì đã xem hai lần) và C chỉ dùng để chấm. Hàng C8 chấm riêng (`*.C8cal`): cảm biến lệch hiệu chuẩn mà bộ điều khiển không thể thấy.
1. **Neo mức theo độ dốc** (`LEVEL_SLOPE_MODE`): nhu cầu đo = công suất đến − độ dốc/k, cập nhật cả khi PV đang di chuyển (gain 0,12 /10 s, chặn lệch >25 %). Trước đây chỉ neo khi PV phẳng ⇒ sau khay lạnh mô hình giữ nhu cầu sai hàng giờ.
2. **Cắt mạnh khi dự đoán vượt SP**: `ASYM` 3 → 20 (hệ số tỉ lệ phía trên SP), `TAUI_FACTOR` 6 → 8. Quét 1 chiều trên A+D: ASYM 3/5/8/12/20/40 → tổng hỏng 258/220/197/178/189/169 (dq 0,1).
3. **Bộ quan sát nhiệt trứng bật khi cảm biến ≤ 0,02 °C** (`EGG_OBSERVER_FINE`), deadband 0,5 mK/s, cổng 8 mK/s nhưng innovation lớn kéo dài ≥ 120 s được coi là khay lạnh (cửa tự hồi trong vài phút nên không bị nhầm), bước tối đa 1,0 °C. Với bậc 0,1 nó làm xấu (282/193 so với 169 ở deadband 2/5 mK/s) ⇒ không bật.
4. Thử và bỏ: LEVEL_FLAT_C/GAIN lớn hơn, giữ bộ học khi innovation cùng dấu, tăng gain neo khi có sự kiện (190/172 so với 169), trần mềm, KUP, BETA, Z_SIGMA, E_ON_MIN (trong nhiễu ±10).
Tổng case-criteria hỏng trên A+D: bậc 0,1: 287 → 169; bậc 0,01: 205 → 73. Bốn cổng an toàn 0 hỏng ở cả hai.
Kiểm chứng B/C (cấu hình chốt): B 0,01 → 175 hỏng/660 ca (G3 cũ: 425); C 0,01 → không có High ngoài C8; chi tiết `tools/thermal-v2/g3_results.txt`.
Độ bền: nhiễu cảm biến σ 0,05 °C và jitter slot 30 %: bộ điều khiển vẫn hữu hạn/an toàn nhưng cổng High hỏng 1–2 ca ở A+D; chỉ tiêu mềm xấu hơn (73 → 102–141).

## 11. G4 — vòng bảo vệ dưới lỗi chèn (mô phỏng, `run_grid.py protect`, kết quả: `tools/thermal-v2/g4_results.txt`)
Chèn vào lò đang ổn định (hoặc đang leo nhiệt) qua đường bảo vệ SẢN XUẤT (High/Emergency/E115/lọc cảm biến/cổng cho phép/bộ chia xung): cảm biến đứng, trôi thấp, nhảy thấp, nhảy cao, mất đường, SSR hàn dính, SSR không dẫn. Bộ điều khiển thử: `v2` và `fastest` (luôn đề nghị 100 %).
- **P1** (bắt buộc): không có lệnh sưởi nào lọt ra khi lớp bảo vệ đang chặn: 0 trên 832 ca, cả hai bộ điều khiển. ĐẠT.
- **P2** (bắt buộc cho lỗi mà các lớp một-cảm-biến nhìn thấy: nhảy cao, mất đường, SSR không dẫn): v2 không chạm High/Emergency, ĐẠT ở cả 0,01 và 0,1 °C.
- **Bộ canh phản hồi mới (`RESP_WATCHDOG`)** trong `thermal_v2.h`: công suất đã đủ thời gian tới nơi (cũ hơn td+2τ, tối đa 260 s) mà PV không nhúc nhích 0,3 °C trong khi vẫn thấp hơn SP 1 °C ⇒ rút đề nghị về 0 tới khi PV đổi. Tính từ hằng số xấu nhất, và sau khi có batch fit thì dùng td/τ/k đã học (k lấy nửa giá trị). Cảm biến đứng THẤP khi đang leo: đỉnh không khí 35–44 °C (không có bộ canh) xuống còn 35–38,6 °C (tập A), 12 lò đều dưới Emergency. Không đổi chỉ tiêu A+D (73 ở 0,01; 169 ở 0,1).
- **Không bảo vệ được bằng phần mềm một cảm biến** (đo, không chấm đạt/trượt): cảm biến đứng đúng ở SP (đỉnh 39–46 °C), cảm biến trôi thấp 3 °C (40,9 °C), nhảy thấp 4 °C (42 °C), SSR hàn dính (43 °C; chỉ contactor tổng cắt được và nhiệt trong ống dẫn vẫn đẩy qua 39 °C). Cần phần cứng: cảm biến thứ hai độc lập, cầu chì nhiệt/relay quá nhiệt cơ khí, hoặc đo dòng SSR. Production hiện chỉ phát hiện cảm biến đứng sau 20 phút (SENSOR_FROZEN_TIMEOUT_MS).
- Cờ `responseSuspect()` để G5 đưa lên telemetry và cảnh báo.

## 12. G5 — nối vào firmware (kết quả: mô phỏng + biên dịch CI; chưa chạy trên lò thật)
- `thermal_v2_adapter.h` (C++ thuần): `ThermalV2Slot::step()` nhận `ThermalV2SlotIn`, tính dt từ hiệu hai lần `millis()` (an toàn khi tràn), kẹp [0, cap], NaN → 0. Firmware (`machine_control.h::thermalV2Command`) và rig dùng CHUNG adapter; rig còn biên dịch nguyên văn `buildThermalV2SlotIn()` của firmware (make_includes.py → `actual-v2-builder.inc`), nên phần ghép đầu vào là chữ thật. Quyết định cho phép sưởi (`normalSsrPermit && actuatorReady`) vẫn do lớp bảo vệ sản xuất; bộ điều khiển không đọc được lớp bảo vệ và lớp bảo vệ không đọc công suất đề nghị.
- `dQ` lấy từ định dạng cảm biến đã khoá (X100 và native → 0,01; X10 hoặc chưa khoá → 0,1). Bộ quan sát nhiệt trứng chỉ chạy khi dQ ≤ 0,02.
- PID Monitor (HMI, 9 mục): PV dự đoán, pha học (NHIN/DO, DA NHAN DANG, DA HOC 1, DA HOC 2, kèm NGHI nếu bộ canh phản hồi cắt), thời gian slot lần cuối/lớn nhất (`esp_timer`). Web `pidMon`: thêm khóa ngắn `pr`, `ph`, `sus`, `sl`.
- LOG AI nối tiếp (đã có sẵn): schema 2, các cột bộ điều khiển mang số của V2 (xem `doc/SERIAL_LOG.md`). Đây là đường đóng vòng với lò thật ở G7: bật "LOG AI" qua USB, sao chép CSV gửi lại. **Chưa làm** vòng nhớ log 8 KB và nút tải CSV trên Web của lộ trình G5 (cần giao diện Web riêng; xếp sau, không chặn G7 vì có LOG AI).
- E113 (TEMP RATE HIGH): một lần TĂNG nhiệt trong khi SSR bật trung bình ≥ 50 % trong 60 s qua không còn kích hoạt cảnh báo; giảm nhiệt, hoặc tăng khi SSR chủ yếu tắt (SSR hàn dính), vẫn cảnh báo. Ngưỡng cắt không đổi.
- **Khởi động lại với đường nhận dạng đã lưu: KHÔNG bật.** `exportPrior()/importPrior()` có trong `thermal_v2.h` (kiểm tra bằng test đơn vị) nhưng firmware không lưu/nạp. Đo ở `tools/thermal-v2/g5_restore_prior.txt` (A+D, 0,01 °C): nạp đường của chính lò đó cho R1.reach.restart 0/52 → 52/52 nhưng High 3/52 và Emergency 1/52 (nhiệt trứng sau mất điện không biết, trước đây công suất bị chặn ở mức an toàn cho tới khi khớp); đường lệch ±30 % độ lợi (đổi thanh đốt): High 7–11/52; lò khác hẳn: High 38/52, Emergency 13/52. Một đường đã lưu sai nguy hiểm hơn là học lại mất ~12 phút ⇒ giữ nguyên "học lại sau khởi động", chấp nhận R1.reach.restart không đạt.
