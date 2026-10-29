# Điều khiển nhiệt V3 — bản 2.4.0: đo tải thật, mốc E115 theo đáy

Sự cố ngày 29/10 (mẻ trứng, thêm khay trứng lạnh lúc nhiệt đang lên 36,4 °C): nhiệt tụt xuống 33,3 °C, rồi **không lên lại**, kẹt quanh 35 °C trong 20 phút và máy báo **E115 – thanh nhiệt không nóng** (cắt thanh nhiệt, bật quạt) dù thanh nhiệt hoàn toàn tốt.

## 1. Nguyên nhân (đã dựng lại từng dòng, không đoán)

Chạy lại **đúng mã firmware** trên log CSV của máy cho thấy lệnh công suất của máy khớp mã (sai lệch trung bình ≈ 1 %, PV dự đoán sai 0,05 °C). Có hai lỗi **độc lập**, chồng lên nhau:

| # | Lỗi | Vì sao gây ra sự cố |
|---|---|---|
| 1 | Mức giữ nhiệt (`pHold`) cố định ≈ 18 %. Mô hình "năng lượng đang bay" của V3 tính `PV dự đoán = PV + kAir·Σ(công suất đã lệnh − pHold)`. | Khay trứng lạnh to bằng số trứng đang ấp hút gần hết nhiệt: giữ nhiệt thật cần **55–60 %**, không phải 18 %. Mô hình tin rằng phần công suất trên 18 % sẽ nâng nhiệt thêm +2 °C (không bao giờ tới), nên bóp công suất còn **58–75 %** trong khi lò còn thấp hơn SP tới 3,5–4 °C. Lò chỉ nhích 0,1 °C/phút. |
| 2 | E115 so PV với **nhiệt độ lúc mở cửa sổ đo** (36,39 °C) và mốc này chỉ có thể đi lên. | Sau cú tụt xuống 33,3 °C, thanh nhiệt đang kéo nhiệt lên (33,3 → 35,0 °C trong 18 phút) nhưng **không bao giờ cao hơn mốc cũ 0,3 °C**, nên 900 s năng lượng full-power dồn lại và E115 cắt thanh nhiệt đúng lúc lò cần nó nhất. |

"PV dự đoán" còn đó vì nó là phanh cần thiết của một lò có độ trễ 100–220 s (không phanh trước thì vọt lố). Nó **bất ổn** vì đầu vào của nó (`pHold`) là một con số đoán, không phải số đo: sai 40 % thì dự đoán sai hàng °C.

## 2. Thiết kế mới (một khối, nhiều chỗ nối với nhau)

`thermal_v3.h` (bộ điều khiển) và `heater_response_watch.h` (E115) — hai khối **không đọc nhau** (E115 vẫn là lớp bảo vệ độc lập).

1. **Mức giữ nhiệt học từ cân bằng năng lượng đo được**, mọi lúc (không chỉ gần SP):
   `hold = công suất đã tới lò (lệnh trễ một thời gian truyền) − độ dốc PV / độ lợi lò`, cửa sổ 4 phút. Khay trứng lạnh, cửa, xe khác… đều hiện ra thành một mức giữ khác — **không có tham số "số trứng"**. Học bị chặn từng bước: lên ≤ 0,5 %/s (τ 120 s), xuống nhanh hơn (τ 40 s, ≤ 1,5 %/s) vì mức giữ quá cao gây vọt lố; chỉ đọc khi không có cửa/quạt hút/giữ dòng và khi **cả cửa sổ công suất (kể cả độ trễ truyền) thuộc lần chạy hiện tại** (sau một khoảng nghỉ công suất nhảy từ 0 lên đầy ngay trong cửa sổ, cân bằng sẽ đọc ra mức giữ ≈ 0); trần 85 %; phần "giảm nhiệt" chỉ được nâng mức giữ tối đa 30 % so với công suất đã tới lò.
2. **Mô hình chỉ được phanh**, không bao giờ tự cộng công suất mà PV đo không đòi hỏi. Thêm **phanh động lượng đo được** (độ dốc PV 40 s × thời gian truyền) phòng khi mô hình sai tải.
3. **Nhận dạng đúng chỗ**: thời gian truyền chỉ đo khi lò *thật sự* khởi động (cách SP ≥ 2 °C, PV đứng yên, ≥ 80 % công suất) và không bao giờ < 90 s; độ lợi lò chỉ lấy từ đoạn leo mà công suất tới lò ≥ 90 % trần. Máy khởi động lại giữa mẻ (cập nhật, mất điện) dùng *giá trị mặc định nghiêng về phía lò nhanh–độ lợi cao* (phía mà số sai làm lò dao động) và chỉ học mức giữ trong vùng không phụ thuộc vào hai số chưa đo (PV gần phẳng, công suất không đổi). Khi nhận dạng đổi độ dài vòng đệm "năng lượng đang bay", vòng đệm được dựng lại từ lịch sử lệnh thật và vị trí ghi quay về 0.
4. **Độ lợi vùng xa SP theo lò**: `kp = min(30, 1/(kAir·pipeLen))` %/°C. 30 là đúng cho lò chậm như lò của bạn (kAir·pipeLen ≈ 0,035); lò có khối khí nhỏ, trễ dài từng dao động ±1 °C.
5. **Cửa**: chặn công suất `pHold + 6 %` tối đa 300 s mỗi lần, chỉ đặt lại khi độ dốc đã chậm (không kẹt hai lần).
6. **E115 (`HeaterResponseWatch`)**: mốc so sánh là **nhiệt độ thấp nhất thấy từ đầu cửa sổ** (theo PV xuống). Mọi thứ khác giữ nguyên: điều kiện yêu cầu nhiệt, cộng thời gian SSR dẫn thật (mỗi bước ≤ 1 s), 900 s, tăng ≥ 0,3 °C, mã lỗi, mức Stop/giữ. Thanh nhiệt hỏng thật vẫn nhảy lỗi sau đúng 900 s năng lượng full-power không có phản hồi.

## 3. Kiểm chứng (mô phỏng + log thật; chưa có kiểm chứng phần cứng)

* **Lò mô phỏng khớp log 29/10** (`tests/thermal-sim.h::u29Oven()`): điều khiển bằng lệnh công suất ghi trong log, PV mô phỏng lệch **0,10 °C RMS** (xấu nhất 0,28 °C) trong 31 phút. Bản **2.3.4** trên lò đó: E115 nhảy sau **1374 s** kể từ lúc mở cửa (máy thật: 1445 s), kẹt ở 34,8 °C. Bản **2.4.0**: không nhảy, về SP ±0,3 °C sau 34 phút, vọt lố 0,17 °C, công suất ≥ 91 % khi lò còn thấp hơn SP > 2,5 °C. Lò đó khởi động lại (thanh nhiệt tắt 0–600 s, mất hết điều đã học) giữa lúc phục hồi, lúc đã phục hồi hoặc lúc ổn định: không E115, vọt ≤ 0,19 °C, lệch cuối ≤ 0,12 °C (`testReboot29`).
* **Chính log của máy** (`tests/fixtures/real-logs/…csv`, `tests/thermal-replay.cpp`): quy tắc E115 cũ nhảy ở 1873 s (máy: ~1890 s); quy tắc mới chưa bao giờ quá 353 s trong 900 s; cùng log đó nhưng nhiệt độ giữ phẳng (thanh nhiệt "chết") thì quy tắc mới **vẫn nhảy** ở 1873 s.
* **148 lò** (khối khí 0,25–1,8 MJ/K, công suất 14–22 kW, trễ 40–200 s, tải trứng, thanh nhiệt có đuôi, cảm biến trễ), `tests/thermal-closed-loop.cpp`. **Mọi số dưới đây do chính bài test này in ra**, mỗi dòng của 2.4.0 kèm một dòng của 2.3.4 chạy trên đúng các lò đó (`THERMAL_FULL_FAMILY=1` chạy cả 148 lò; CI chạy mẫu 35 lò). Máy thật chạy bộ điều khiển liên tục từ lúc bật nguồn nên lần lên nhiệt đầu luôn đi sau một khoảng nghỉ: các dòng "sau nghỉ" mô phỏng đúng việc đó.

| Kịch bản | 2.3.4 | 2.4.0 |
|---|---|---|
| Mở cửa + khay trứng lạnh bằng trứng đang có, lò đã ổn định | **103/148 lò bị E115 cắt**; có lò kẹt lệch SP tới 7,1 °C; vọt tới 1,7 °C | 0 lò bị cắt; về ±0,3 °C và ở lại: trung vị 24 phút, 90 % số lò ≤ 35 phút, chậm nhất 83 phút; vọt ≤ 0,27 °C |
| Cùng sự kiện ngay trong lần lên nhiệt đầu | **108/148 lò bị cắt** | 0 lò bị cắt; trung vị 21 phút, chậm nhất 90 phút; vọt ≤ 0,35 °C |
| Khay chỉ bằng nửa số trứng (ổn định / đang lên nhiệt) | 9 / 22 lò bị cắt | 0 / 0 |
| Lên nhiệt lần đầu, trứng lạnh 28 °C | tới SP − 0,3 °C chậm nhất 89 phút; vọt tới 1,43 °C; 30 phút cuối còn dao động đỉnh–đỉnh tới 2,23 °C | chậm nhất 38 phút; vọt ≤ 0,41 °C; dao động ≤ 0,34 °C |
| Lên nhiệt lần đầu, lò trống | chậm nhất 40 phút; vọt tới 1,06 °C; dao động tới 1,46 °C | chậm nhất 31 phút; vọt ≤ 0,25 °C; dao động ≤ 0,23 °C |
| Lên nhiệt lần đầu sau khi máy đã chạy không tải 1 / 2 / 5 phút | như trên (chậm nhất 89 phút; vọt tới 1,41 °C) | chậm nhất 38 phút; vọt ≤ 0,40 °C |
| Cửa mở 2 / 5 / 10 phút | vọt tới 2,18 / 1,60 / 1,59 °C | ≤ 0,42 / 0,73 / 1,34 °C |
| Chuyển SP +0,5 °C | vọt tới 1,50 °C | ≤ 0,08 °C, đến nơi ≤ 864 s |
| Quạt hút 3×5 phút | vọt tới 2,02 °C | vọt ≤ 0,06 °C, tụt ≤ 1 °C rồi về |
| Máy khởi động lại giữa lúc đang phục hồi (thanh nhiệt tắt 45 s) | 30 phút cuối còn lệch tới 6,4 / 7,0 °C (khay ×0,5 / ×1,0); vọt tới 1,61 °C | vọt ≤ 0,53 °C, lệch cuối ≤ 0,69 / 0,91 °C |
| 24 giờ vận hành thường (quạt hút mỗi 40 phút, cửa 2 phút mỗi 5 giờ, đổi SP) | không lò nào bị cắt nhưng bằng chứng E115 đã lên **722/900 s**; vọt tới 2,16 °C | bằng chứng E115 cao nhất 341/900 s; vọt ≤ 0,53 °C |
| **Thanh nhiệt chết thật** | 148/148 nhảy E115, chậm nhất 7478 s | 148/148 nhảy E115, chậm nhất **4780 s** |

## 4. Giới hạn đã biết (viết ra để không tệ đi âm thầm)

* **Vài phút đầu sau khi thêm khay lạnh công suất chưa đầy**: trong 123/148 lò rơi > 2,5 °C dưới SP sau khay ×1,0, lệnh có lúc < 90 % (thấp nhất 18 %; bản 2.3.4: có lúc 0 %). Lý do: mức giữ nhiệt mới chỉ được học lên với tốc độ ≤ 0,5 %/s, luật tỉ lệ (30 %/°C) chỉ bão hòa khi sai lệch ≈ 3 °C, và với lò khối khí nhỏ–trễ dài `kp` còn nhỏ hơn. Hậu quả: phục hồi chậm hơn mức vật lý cho phép (số liệu ở bảng), nhưng không E115 và không vọt. Lò khớp log 29/10 với đúng sự kiện hôm đó: ≥ 91 %. Hướng cải tiến đã nghĩ tới (chưa làm): nâng mức giữ nhanh hơn khi mô hình hứa PV lên mà PV không lên.
* **Cửa mở 5–10 phút**: yêu cầu R2.over.door (vọt ≤ 0,3 °C) đạt trên mẫu CI 35 lò với cửa 2 và 5 phút (≤ 0,11 °C) nhưng trên cả 148 lò có lò vọt 0,42 (2 phút), 0,73 (5 phút), 1,34 °C (10 phút); bản cũ 2,18 / 1,60 / 1,59 °C.
* **Lên nhiệt lần đầu với trứng lạnh**: nhanh hơn 2.3.4 nhiều lần nhưng chưa đạt yêu cầu R1 của chủ máy (≤ 1,3× giới hạn vật lý + 120 s + 720 s mẻ đầu) ở lò nặng nhất: vượt tối đa 857 s trên 148 lò (252 s trên mẫu 35 lò); bản 2.3.4: 3103 s.
* **Khởi động lại giữa mẻ** với lò *nhanh, trễ ngắn* (3/35 lò mẫu): nhiệt thấp hơn SP 0,3–0,9 °C trong nhiều giờ, vì giá trị mặc định nghiêng về phía lò chậm. Không lưu tham số học vào NVS (chưa làm; sẽ xoá giới hạn này nếu làm).
* **Cảm biến 0,01 °C** (khảo sát ngoài CI): 6/245 ca mô phỏng (3 lò có `kAir·τ ≥ 0,06`: khối khí nhỏ + trễ dài) còn dao động đỉnh–đỉnh tới 0,9 °C (bản 2.3.4: tới 2,5 °C). Lò của bạn (`kAir·τ ≈ 0,026`) dao động ≤ 0,15 °C với cả hai độ phân giải.
* Mô phỏng không chứng minh được máy thật. Lỗi cơ khí/điện (SSR, contactor, đặt cảm biến) nằm ngoài bài toán này.

## 5. Kiểm tra trên máy thật (mẻ tiếp theo)

Xuất CSV PID Monitor sau lần thêm trứng/mở cửa đầu tiên và kiểm: không có E115; sau khi cửa đóng, `request_pct` lên cao khi PV còn thấp hơn SP > 2,5 °C (vài phút đầu có thể < 90 % trong lúc mức giữ đang học, xem mục 4) và PV lên đều (không chững 20 phút); `pred_c` bám sát `pv_c` (không lệch +2 °C); không có chu kỳ lên–xuống > 0,5 °C ở trạng thái ổn định. Nếu khác với điều trên thì gửi CSV: đó là chuẩn nghiệm thu duy nhất.

Chạy lại các con số ở mục 3: `g++ -std=c++11 -O2 tests/thermal-closed-loop.cpp -o /tmp/cl && THERMAL_FULL_FAMILY=1 /tmp/cl`.

## 6. Ba lỗi tìm thấy khi soát lại, sửa trước khi hợp nhất

Lỗi 1 và 2 là của chính bản này và chỉ xuất hiện ở lần lên nhiệt đầu sau một khoảng nghỉ (bộ điều khiển chạy liên tục từ lúc bật nguồn, chỉ không cấp nhiệt khi chưa bấm chạy) — kịch bản mà bộ test ban đầu chưa có. `tests/thermal-v3.cpp::testStartAfterAPause` và các dòng "sau nghỉ" của `tests/thermal-closed-loop.cpp` giữ chúng lại. Lỗi 3 có sẵn từ các bản trước, kể cả 2.3.4.

1. **Vòng đệm công suất lệch tổng**: khi nhận dạng rút ngắn vòng đệm (độ trễ đo được < 150 s) mà vị trí ghi đang nằm ngoài vòng mới, tổng chạy của "năng lượng đang bay" lệch vĩnh viễn (PV dự đoán lệch tới 0,04 °C): 28–40 % số lần bật trong mô phỏng với lò trễ 60–100 s, 0 % với lò trễ ≥ 150 s (lò khớp log 29/10 đo được 170 s nên không dính). Nay vị trí ghi quay về 0 khi dựng lại vòng.
2. **Bộ học mức giữ đọc nhầm bước nhảy công suất**: ngay sau khoảng nghỉ cửa sổ công suất chứa bước nhảy 0 → 100 %, cân bằng đọc ra mức giữ ≈ 0 (thấp nhất 0,1 % thay vì 8 %), độ lợi lò học thấp hơn 20 %, lần lên nhiệt đầu chậm thêm tới 250 s (lò khớp log 29/10: +200 s). Nay bộ học chỉ đọc khi cả cửa sổ (kể cả độ trễ truyền) thuộc lần chạy hiện tại.
3. **Đồng hồ nội bộ là `float`** (có sẵn từ các bản trước): cộng bước 0,1 s vào một số `float` lớn thì mất dần — sau khoảng 6 ngày chạy liên tục đồng hồ chạy nhanh 25 %, sau khoảng 20 ngày thì đứng yên. Mọi độ dốc (phát hiện mở cửa, chống kẹt) là hiệu của hai lần đọc đồng hồ đó: sau 3 ngày độ dốc đo lệch +6 %, sau 8 ngày −20 %, sau 20 ngày về 0 và máy **không bao giờ thấy cửa mở nữa** cho đến lần khởi động lại. Máy ấp bật liên tục 3 tuần (một mẻ) là chuyện thường. Nay đồng hồ là `double` (`testDoorPlungeAfterWeeksOfUptime`: đo đúng −20,0 mK/s sau 3, 8 và 30 ngày).
