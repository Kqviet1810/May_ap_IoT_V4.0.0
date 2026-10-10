# Báo cáo nghiệm thu cuối — Smart Thermal MAYAP V4 (PR #14)

Cơ sở: commit `3db417c` trên nhánh `claude/gracious-hopper-zjvufg`. **Mã sản xuất (`MAYAP_INDUSTRIAL_v1_0_0/`) không đổi trong đợt này.**
Không merge main, không deploy, không flash, không bật Smart Thermal. `MAYAP_SMART_THERMAL` vẫn mặc định 0. Oracle, High 38.2, Emergency 39.0, SP, baseline: không đổi.

## 1. SOFTWARE VERIFIED (đã kiểm chứng bằng host/CI)
| Hạng mục | Kết quả |
|---|---|
| CI trên `ef58910` | `reliability` và `build` đều thành công |
| Kích thước build thật (arduino-cli, core esp32 3.3.11) | mặc định 1 432 549 B / RAM 160 904 B; cờ=0 giống hệt mặc định (+0/+0); cờ=1 +52 B Flash, +0 B RAM; trong ngân sách 1 480 000 / 164 000 |
| A/B tái lập baseline đóng băng sau khi sửa harness | 788, 2160, holdout: A và B giống từng dòng (`True` cả ba); không có hồi quy an toàn |
| Smart AutoTune 540, **cờ=0** | cổng PASS: mini ACCEPTED_BAD=0, full ACCEPTED_BAD=0, High/Emergency=0/0; 231/540 chấp nhận |
| Smart AutoTune, rollback | 471 kiểm tra đơn vị PASS; 84 lần cắt tức thời, 12 điểm mất điện: PID/profile cũ nguyên vẹn; cả 309 ca bị từ chối khôi phục PID/profile cũ (harness dừng nếu vi phạm, không lần nào dừng) |
| PRACTICAL / normal subset | tái dựng khớp đúng cả 7 số tham chiếu (xem mục 5); tính cho A/B/C |
| Phân loại 48 ca B PASS → C FAIL | xem mục 3 |
| Phân loại 18 High / 3 Emergency | xem mục 4 |

## 2. Smart AutoTune 540 với MAYAP_SMART_THERMAL=1 — **phát hiện**
Cờ biên dịch riêng không đổi kết quả vì harness ép chế độ điều khiển theo từng kịch bản; đã thêm `--smart-thermal` (biên dịch với cờ=1 **và** chạy oracle 3 giờ sau-tune ở chế độ Smart). Kết quả (cờ=1):
* chấp nhận 231/540, rejected 309, High/Emergency trong tune 0/0, sau tune 0/0;
* **ACCEPTED_BAD = 1** (`full165`: eff 0.7, 1.6 MJ/K, loss 180, dead 30, lag 3, ambient 28, res 0.1). Cờ=0: 0.
* Nguyên nhân (truy vết + tắt từng phần): hold fast-track của learner Smart. Khoảng t≈7300 s xuất hiện **một sự kiện lệch Kh** (hệ số tăng ước lượng 0.0115 so với thật 0.0070), assist feed-forward về 0 trong ≈1800 s, PV thấp hơn SP ≈0.2 °C → P95 0.193 > 0.18 và `settle = -1`. Không có sự kiện an toàn (High/Emergency = 0). Tắt hold fast-track thì ca này hết lỗi.
* Cổng `tools/test_smart_autotune.py --smart-thermal` do đó **FAIL có chủ đích (exit 1)**; không đưa vào CI. Đây là điều kiện chặn đề xuất bật cờ=1 (D1).
Bằng chứng: `autotune/summary-flag0.md`, `autotune/summary-flag1-smart-oracle.md`, `autotune/full540-flag*.csv`.

## 3. 48 ca Adaptive PASS → Smart FAIL (2160)
Phân rã bằng cách tắt từng phần Smart (`regress48-classification.csv`, `ablation-2160.csv`):
| Nguyên nhân | Số ca | Ghi chú |
|---|---|---|
| Khởi động Smart | 30 | 10 ca nặng (MAE > 0.5 °C): PV kẹt dưới SP hàng giờ |
| Hold fast-track | 14 | phần lớn sát ngưỡng (MAE 0.12–0.13 so với 0.10) |
| Chỉ lỗi khi cả hai bật | 3 | |
| Lỗi với từng phần riêng | 1 | |
Mức độ: 24 sát ngưỡng (≤ 1.5× giới hạn), 14 vừa, 10 nặng. 44/48 ở độ phân giải 0.1. Ở 27/48, chính Adaptive V1 chỉ thoát vòng kẹt sau ≥ 4000 s → đây là **điểm yếu sẵn có** (vòng khởi động hãm cân bằng ≈3 °C dưới SP khi chưa biết hold), Smart đẩy cân bằng đủ lệch để learner không tạo được cửa sổ hold phẳng.
Đóng góp từng phần trên 2160 ca (PASS / High / Emergency): Adaptive 1242/33/17; Smart đầy đủ 1397/18/3; chỉ hold fast-track 1402/33/17; chỉ khởi động 1227/18/3; bỏ nhánh prior 1404/30/16; bỏ nhánh gain quan sát 1395/21/4.
Thử các sửa nhỏ để xử lý vòng kẹt (giới hạn theo độ dốc, kéo dài chân trời, giới hạn thời gian 600/1200/2400 s): **không cải thiện** hoặc làm giảm an toàn → không áp dụng. **Không sửa mã sản xuất.**

## 4. 18 High / 3 Emergency còn lại (`high-emergency-18.csv`)
Cả 18 ca đều ở **hiệu suất heater 2.0 trên khối lượng nhẹ nhất 180 kJ/K** (Kh thật 0.178 °C/s, góc cực đoan của miền mô phỏng).
* **10 ca có sai lệch ban đầu 2.5 °C (ambient 35)**: chỉ ≈19–21 s bật heater trong 180 s đầu vẫn vượt 38.2. **Phát biểu trước đây "giới hạn vật lý" là sai**: prior chỉ bật khi sai lệch ≥ `PriorMinError` = 3.5 °C; hạ xuống 2.0 làm High 18 → 8 (Emergency vẫn 3).
* **5 ca sai lệch 9.5–12.5 °C** và **3 ca sai lệch 12.5–27.5 °C với dead time 60–120 s**: bật 62–166 s trước khi thấy đáp ứng đầu; prior hãm theo dự đoán đỉnh = SP, không có biên dưới High, nên sai số gain/trễ ≈ 10–30 % đủ vượt 38.2.
* Hiệu chỉnh duy nhất đo được bằng số liệu thật là Kh của lò (Giai đoạn 2.1 của kế hoạch nghiệm thu).
Ứng viên đã đo (PENDING APPROVAL, không áp dụng): `PriorMinError` 3.5 → 2.0: 2160 ca PASS 1397 → 1391, High 18 → 8, Emergency 3; 788 ca PASS 546 → 542, High/Emergency 0/0; holdout 282 → 282. Thêm gain ×1.3: 2160 ca High 5 / Emergency 1 nhưng 788 ca xuất hiện 1 High + 1 Emergency → loại. Kéo dài cửa sổ prior 240 s: PASS 1356 → loại.

## 5. PRACTICAL và normal subset (RECONSTRUCTED, `tools/practical_subsets.py`)
Định nghĩa gốc không có trong workspace; tái dựng bằng tìm kiếm có hệ thống và chỉ chấp nhận khi **khớp đúng** các số tham chiếu của Adaptive V1 (594/788, 1464/2160, 45/47, 187/213, 86/96). Tìm được đúng một nghiệm cho mỗi bộ; `--verify` kiểm lại. Vẫn ghi nhãn "tái dựng", chưa được tác giả báo cáo gốc xác nhận.
| Tập | cases | STANDARD A/B/C | PRACTICAL A/B/C | High A/B/C | Emergency A/B/C |
|---|---|---|---|---|---|
| 788 | 788 | 186/509/546 | 340/594/616 | 1/0/0 | 0/0/0 |
| 2160 | 2160 | 319/1242/1397 | 710/1464/1621 | 33/33/18 | 17/17/3 |
| holdout | 360 | 45/254/282 | 127/283/310 | 0/0/0 | 0/0/0 |
| normal 788 | 47 | 10/45/47 | 31/45/47 | 0/0/0 | 0/0/0 |
| normal 2160 | 213 | 60/187/203 | 142/197/210 | 0/0/0 | 0/0/0 |
| extended (holdout) | 96 | 11/86/88 | 34/90/93 | 0/0/0 | 0/0/0 |
PRACTICAL: overshoot ≤ 0.30, MAE ≤ 0.20, P95 ≤ 0.30, không High/Emergency. Oracle STANDARD không bị sửa.

## 6. HARDWARE REQUIRED
Toàn bộ `ESP32_NO_HEAT_CHECKLIST.md` (chưa đo): chu kỳ điều khiển 5 ms và jitter, stack high-water, heap min/largest, TWDT, mất điện ngẫu nhiên, trạng thái chân khi boot, thứ tự SSR/master khi High/Emergency (tải sưởi hở), nhiễu/phân giải thật của SHT30, chi phí tính toán cờ=1 so với cờ=0. Sau đó: đặc tính lò trống (Kh, coast, hold) và mọi giai đoạn của `COMMISSIONING_TEST_PLAN.md`.

## 7. BLOCKED / PENDING (cần bạn duyệt hoặc đủ bằng chứng)
1. Bật `MAYAP_SMART_THERMAL=1`: **bị chặn** bởi ACCEPTED_BAD=1 ở mục 2 và 48 hồi quy ở mục 3.
2. Có áp dụng `PriorMinError` 2.0 (đánh đổi PASS −6/−4 lấy High −10) hay không.
3. Có xử lý hold fast-track (nguồn của ACCEPTED_BAD=1 và 14 hồi quy) hay không, sau khi có đo thực.
4. D2–D6 (E115/E104, chốt contactor, chính sách quạt, ngân sách rơ-le, chương trình nhiệt/làm mát trứng): giữ nguyên trạng thái "chưa duyệt, chưa tích hợp".
5. Xác nhận định nghĩa PRACTICAL/normal đã tái dựng với tác giả báo cáo gốc.
6. Mục tiêu ≥ 95 % trên ca REACHABLE chưa đạt (khoảng 80 % trên 2160 ca).

**Dừng tại đây để bạn quyết định có chuyển sang bước thử ESP32 không cấp nhiệt hay không.**
