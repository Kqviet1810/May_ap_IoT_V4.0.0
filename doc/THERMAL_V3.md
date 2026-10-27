# Thermal V3 — điều khiển nhiệt đơn giản, chỉ dùng số đo

Mã: `MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h` (khoảng 330 dòng, C++11 thuần) + `thermal_v3_adapter.h`.
**Chỉ kiểm chứng bằng mô phỏng và CI; chưa thử trên lò thật.** Mọi số dưới đây là mô phỏng, không phải đảm bảo cho lò thật.

## 1. Vì sao viết lại
Log lò thật 24/10 (09:04–09:22): công suất giữ phẳng 20 % từ 36,0 °C tới đúng 37,5 °C, sau đó PV còn trôi thêm +0,3 °C lên 37,8 °C
dù yêu cầu đã về 0. Bộ điều khiển trước đó tin vào PV *dự đoán* từ một mô hình học; mô hình sai (độ lợi bị học nhỏ lại, mức giữ học lớn
quá trần) nên dự đoán chỉ cao hơn PV 0,1 °C trong khi thực tế còn đi thêm 0,3 °C. Bài học: không để một mô hình thắng số đo thật,
và không vá từng triệu chứng.

## 2. Nguyên tắc
* Không có PV dự đoán. Khối chỉ dùng: PV, độ dốc PV trên 90 s, và công suất chính nó đã ra lệnh.
* Mô hình về lò chỉ là 4 số đo được (`struct Model`):
  | số | ý nghĩa | đo thế nào |
  |---|---|---|
  | `k` [°C/s trên 1 %] | độ lợi heater | xung thử 25 %: (độ dốc có công suất − độ dốc nền) / công suất |
  | `delay` [s] | tuổi của công suất tạo ra độ dốc hiện tại | thời gian từ lúc cấp công suất tới khi PV trả lời (≥ 0,2 °C trên đường nền) |
  | `tc` [s] | thời gian trôi sau khi cắt | mỗi lần cắt công suất: (PV còn tăng thêm) / (độ dốc lúc cắt) |
  | `hold` [%] | công suất giữ SP | −độ dốc nền / k lúc đầu; sau đó cân bằng năng lượng: công suất đứng sau độ dốc − độ dốc / k |
* Mô hình nằm trong RAM suốt đời bộ điều khiển (máy không gọi `reset()`); `exportModel()/importModel()` để ghi NVS sau này.
* Một vào, một ra: một hàm `step()` nhận một `Input`, trả một số công suất. Lớp bảo vệ độc lập (High, Emergency, E115, đường quạt hút)
  nằm ngoài và chỉ có quyền hạ hoặc chặn.

## 3. Quy trình lên nhiệt
1. **Nhìn 90 s**, heater tắt: đo độ dốc nền.
2. **Xung thử 25 %** (an toàn cho cả lò nhanh): dừng khi PV tăng 0,6 °C trên đường nền hoặc sau 7 phút. Ra `k`, `delay`, `hold` ban đầu.
   Chỉ chạy khi còn cách SP ≥ 2 °C và chưa có mô hình. Gần SP hơn thì vào luật ngay với giá trị tiên nghiệm thận trọng.
3. **Cắt về 0 và đo độ trôi** → `tc`.
4. **Luật chạy:**
   * dưới SP: `u = hold + (SP − PV − 0,05) × 0,8 / (k × tc)` — công suất mà nếu giữ nguyên sẽ đưa PV tới đúng SP;
   * quy tắc dừng: độ dốc × tc × 1,25 là quãng PV còn đi; khi nó ≥ khoảng còn lại dưới SP, công suất trên mức sàn (50 % của `hold`)
     được giảm dần về sàn (về 0 ở gấp đôi khoảng còn lại). Trên SP mà PV còn tăng: công suất 0;
   * trên SP: gain gấp 3.
5. **Giữ:** `hold` đi theo cân bằng năng lượng (hằng số thời gian 3·tc), chỉ khi PV cách SP ≤ 1,5 °C, độ dốc ≤ 6 mK/s và công suất không đổi quá 12 %.
   PV dẹt dưới SP > 2 phút thì `hold` tăng 1,5 %/phút.

## 4. Kết quả mô phỏng (`python3 tools/thermal-v3/benchmark_v3.py`, chạy cả trong CI)
21 lò (12 lò chuẩn A + 9 lò khớp log 24/10, có/không trễ cảm biến 40 s) × 5 kịch bản. Tiêu chí đặt trong `bench_v3.cpp`.

| Kịch bản | vọt lớn nhất / trung vị | giữ trong ±0,1 °C giờ cuối (thấp nhất / trung vị) | lên tới SP−0,3 (lâu nhất / trung vị) |
|---|---|---|---|
| nguội 33 °C | 0,35 / 0,13 °C | 69 % / 100 % | 114 / 36 phút |
| ấm 36,5 °C | 0,22 / 0,16 °C | 100 % / 100 % | 59 / 22 phút |
| mở cửa 5 phút (900 W/K) | 0,54 / 0,17 °C | 79 % / 100 % | — ; phục hồi ≤ 7,1 phút |
| quạt hút 5 phút/30 phút | 0,38 / 0,15 °C | 30 % / 100 % | — ; phục hồi ≤ 2,9 phút |
| nhiệt phòng +4 °C | 0,53 / 0,15 °C | 31 % / 100 % | — ; phục hồi ≤ 1,7 phút |

Các lò khớp log 24/10 (U1–U6, U1L–U3L): vọt 0,10–0,23 °C, giữ trong ±0,1 °C đúng 100 % giờ cuối ở kịch bản nguội và ấm.

## 5. Giới hạn đã biết
* Lò có trễ rất lớn (A7: 180 s + lò 50 s) là ca yếu nhất: lên nhiệt 114 phút, và sau quạt hút hoặc nhiệt phòng đổi nó còn dao động ±0,2–0,3 °C
  (30–50 % thời gian trong ±0,1 °C ở giờ cuối). Lò A6 cũng yếu ở kịch bản quạt hút.
* Mô phỏng của chúng tôi là lò hai nút tự dựng. Khối nhiệt chậm thật (thép xe, trứng) có thể khác. Phải thử thật.
* Cắt sớm có giá: lên nhiệt lần đầu khoảng 20–60 phút (ước lượng mô phỏng), lâu hơn bản nhanh nhưng vọt.
* Không còn trạng thái "mở cửa" riêng: độ dốc âm sau một lần cắt giống hệt mở cửa, nên bản thử đầu bị nhầm và kẹp công suất khi phục hồi.
  Luật tự phục hồi mở cửa; kịch bản "door" trong benchmark kiểm điều đó.
* Mô hình chưa lưu NVS: mất điện thì đo lại (xung thử + một lần cắt). Có sẵn `exportModel()/importModel()`.

## 6. Đọc log
`pred_c` bây giờ bằng PV (không còn dự đoán). Trạng thái: 1 climb (bắt đầu hoặc còn xa SP), 2 track, 3 frozen (không permit hoặc quạt hút giữ heater).
`trust_pct` = 100 khi đã đo được độ lợi, 53 khi còn dùng giá trị tiên nghiệm. PID Monitor: `gain` = k×1000, `delay` = tc, `holdPct` = mức giữ.
