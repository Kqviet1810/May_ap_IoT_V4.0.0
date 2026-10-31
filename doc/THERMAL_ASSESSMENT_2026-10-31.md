# Đánh giá toàn bộ hệ nhiệt (31/10/2026) và phương án khắc phục tận gốc

Mục đích: nói thẳng hệ nhiệt đang ở đâu, vì sao cứ phải vá, và việc cần làm để dừng vá. Mỗi nhận định ghi rõ là **đã đo**, **suy ra** hay **chưa chứng minh**.

## 1. Kết luận

* Hệ nhiệt **giữ nhiệt ổn định tốt khi đã chạy dài**: log đêm 31/10 (3,3 giờ, mẻ chạy liền) có 85 % thời gian trong ±0,1 °C và 97 % trong ±0,2 °C, đỉnh +0,10 (đã đo).
* Hệ nhiệt **không đáng tin ở ba chỗ**: lúc **lên nhiệt gần ngưỡng** (xuất phát từ 34,8–35,3), lúc **khởi động lại sát ngưỡng**, và sau **mở cửa kèm khay lạnh**. Đó là những lúc bộ điều khiển phải dựa vào số mặc định thay vì số đo.
* Gốc của việc này **không nằm ở một luật thiếu**: bộ điều khiển hiện chạy trên số đoán (hold 18 %, độ lợi 4e-4, trễ 150 s) trong phần lớn các lần khởi động thật, và cơ chế học của nó không tự sửa được số đoán sai. Mỗi bản sửa (2.4.0 → 2.5.7) đẩy thêm một luật vào quanh cái lõi đó.

## 2. Hệ nhiệt gồm những lớp nào, mỗi lớp ra sao

| Lớp | Việc của nó | Tình trạng (bằng chứng) |
|---|---|---|
| Cảm biến + lọc | đọc 0,1 °C, lọc trung vị và IIR, đọc an toàn riêng | Hoạt động; độ phân giải 0,1 °C giới hạn mọi thứ phía sau (xem 4.4) |
| Bộ điều khiển `thermal_v3.h` | cho % công suất heater | Tốt ở trạng thái ổn định; yếu khi dùng số mặc định (3.1) |
| Bảo vệ | Báo cao, Khẩn cấp, chặn heater, E115 heater không đáp ứng | Tách khỏi bộ điều khiển; test host đạt (không phải điểm yếu) |
| Quạt hút theo xu hướng `vent_line.h` | hút khi nhiệt không giảm | Chạy đúng trong log 31/10 (bật 09:10, tắt theo thời gian tối đa); chưa có thử nghiệm so sánh có/không quạt trên máy thật |
| Lưu học NVS `thermal_learn.h` (2.5.5) | nhớ số đã học qua khởi động lại | **Chưa từng chạy trên máy của anh**: chỉ lưu khi đã nhận dạng lò, mà log 31/10 `identified = 0`, `learn_saved = 0` |
| Còi | báo cao/khẩn cấp | 2.5.6, chưa nghe thử trên phần cứng |

## 3. Bằng chứng

### 3.1 Log thật của máy (MAP-441BF6E051D0, 24–31/10, mẻ đang chạy)
Mười tệp CSV (có hai cặp trùng); nhiều phiên bản firmware khác nhau, nên **không so tuyệt đối** giữa các ngày.

| Khởi động từ | Lên tới SV−0,1 | Đỉnh trong 40 phút sau đó |
|---|---|---|
| 29,8 – 32,3 °C (4 lần, nguội) | 28 – 57 phút | **+0,1** cả 4 lần |
| 33,9 °C | 19 phút | +0,2 |
| 34,8 °C (31/10) | 19 phút | +0,2 (thành Báo cao 37,7) |
| 35,2 °C (24/10) | 15 phút | **+0,3** |
| 36,7 °C (31/10 đêm) | 6 phút | +0,1 rồi ổn định 85 %/97 % |
| 35,3 °C (29/10, cửa mở + khay lạnh) | không lên tới SV | — |

Đọc được: lên từ nguội vọt ít (+0,1); **khởi động từ gần ngưỡng (≥ 33,9 °C) vọt +0,2 đến +0,3**. Mẫu nhỏ (8 lần), nhưng khớp với điều anh phản ánh.

### 3.2 Log chẩn đoán 31/10 (đầy đủ cột)
* `identified = 0`, `kair_e5 = 40`, `tau_s = 150`, `hold_pct` 18 → 20 → 17,5 trong suốt mẻ: **bộ điều khiển chạy hoàn toàn bằng số mặc định**.
* Hold thật ở chiều nóng chỉ vài phần trăm (hồi quy thô: ~4 %; sau khi Báo cao cắt heater 14,5 phút, nhiệt chỉ tụt 0,2 °C). **Suy ra, chưa đo trực tiếp.**
* Heater quay lại với đúng 17,5 % cũ sau khi bị cắt: đã sửa ở 2.5.7.

### 3.3 Bench lò ảo (210 lò, hai họ, 10–11 tình huống)
* Tái hiện đúng hướng của log thật (đỉnh +0,2 từ gần ngưỡng).
* **Cảnh báo về độ tin cậy của chính bench:** bench cho 91–93 % lò ảo chạm +0,2 khi **lên từ nguội**, nhưng log thật cho **+0,1 cả 4 lần**. Bench **quá bi quan** ở lên nhiệt từ nguội, nghĩa là số bench chưa phải thước đo tuyệt đối. Nó đúng hơn ở xuất phát gần ngưỡng.
* Lò ảo chỉ được "khớp" với **một** log thật (29/10). Phần còn lại là họ lò ngẫu nhiên.

### 3.4 Những gì chưa có
* Không có phép thử vòng kín trên lò thật cho từng thay đổi; mỗi lỗi chỉ lộ ra khi mẻ thật chạy, thường ngày hôm sau.
* Không có thử nghiệm có/không quạt hút trên máy thật; không có nghe thử còi.

## 4. Vì sao cứ phải vá: các nguyên nhân gốc

### 4.1 Bộ điều khiển chạy bằng số đoán trong đa số lần khởi động
Nhận dạng (đo trễ và độ lợi) chỉ chạy khi lên nhiệt với ≥ 80 % công suất từ dưới SV ít nhất 2 °C. Khởi động từ 34,8 °C chỉ chạy khoảng 51 % nên **không bao giờ nhận dạng**; mọi luật phía sau (phanh, bộ học hold, lưu NVS) làm việc trên số đoán.

### 4.2 Hold và độ lợi nằm chung một phương trình
Bộ học hold suy ra hold từ "công suất đến lò trừ độ dốc chia độ lợi". Độ lợi mặc định cao gấp khoảng 2–4 lần thật nên bộ học luôn thấy "hold 18 % là đúng". Không luật nào thêm vào ngoài có thể sửa chỗ này trừ khi **đo được độ lợi hoặc đo được hold trực tiếp**.

### 4.3 Vá chồng vá
* 22 commit chạm `thermal_v3.h` trong 6 ngày (26–31/10); lõi đã đổi hướng nhiều lần (V3 → "chỉ đo" → khôi phục → 2.4.0 → 2.5.x).
* 31 hằng số điều chỉnh trong một tệp 660 dòng, cộng bộ quạt, bộ lưu, bộ chặn heater.
* Các luật va nhau: trong 2.5.7 mình thử hạ hold khi đứng trên SV (làm hỏng test hồi phục sau mở cửa của log 29/10) và phanh động lượng 2 phút (làm xấu vòng kín với quạt). Phải bỏ cả hai. Mỗi lần thử mất hàng giờ và chỉ còn lại một luật nhỏ.
* Một số bản sửa trước đó (luật quạt 2.5.1, lưu học 2.5.5) **chưa có bằng chứng lợi ích trên máy thật**; lưu học chưa từng được kích hoạt.

### 4.4 Yêu cầu và vật lý đang rất sát nhau
Ngưỡng Báo cao của anh là SV + 0,2 °C, tức **hai bước của cảm biến 0,1 °C**. Với trễ 100–180 s và nhiệt tồn, một bộ điều khiển không biết số của lò khó đảm bảo đỉnh dưới +0,2. Không có cách nào "vá" để chắc chắn dưới 0,2; chỉ có cách **đo lò, hoặc tiến vào SV chậm theo kiểu không phụ thuộc mô hình**.

### 4.5 Khoảng trống kiểm chứng
Không có công cụ nào trả lời "bản này có tốt hơn trên lò của anh không" trước khi anh chạy mẻ. Bench chưa được hiệu chuẩn với log thật (3.3).

## 5. Phương án tận gốc

Nguyên tắc: **ngừng thêm luật; thay nguồn gây sai (số đoán) bằng số đo; tiến vào SV theo cách không cần biết mô hình; học có thử và có đường lui; kiểm chứng trên lò thật trước khi bật.**

### P1. Bộ nhớ mô hình lò có độ tin cậy
Giữ `{độ lợi K, trễ τ, hold H}` cùng nguồn gốc của từng số (mặc định / đo khi lên nhiệt nguội / đo khi dò / lấy từ bản lưu) và độ tin cậy. Mọi quyết định bám vào độ tin cậy: tin thấp thì thận trọng (tiến chậm), tin cao thì nhanh.

### P2. Đo thay vì đoán, ở mọi kiểu khởi động
* **Nguội (xa hơn khoảng 3 °C):** như hiện nay, đầy công suất, đo K và τ.
* **Gần ngưỡng (ý của anh):**
  * Sát hoặc trên SV: công suất 0, tụt thì bơm từ từ theo tốc độ tụt đo được.
  * Dưới ngưỡng khoảng 0,5 °C: bắt đầu 5 %, mỗi bước +2 %, mỗi bước chờ ít nhất 2 phút hoặc bằng τ, đến khi nhiệt hết tụt thì chốt **đó là hold đo được**.
  * Dưới ngưỡng khoảng 2 °C: bắt đầu hold 8 %.
* **Trong khi chạy:** ước lượng bình phương tối thiểu có quên (K, H) chỉ khi có kích thích thật (khởi động, mở cửa, đổi công suất > 5 % kéo dài > τ). Mình **không** đề xuất xung thử nhỏ định kỳ vì cảm biến 0,1 °C không nhìn thấy: 1,5 % trong 4 phút chỉ nâng khoảng 0,04–0,14 °C.

### P3. Hạ cánh mềm, không phụ thuộc mô hình
Trong 0,3–0,5 °C cuối, công suất bị chặn theo `H_ước + ΔP(sai lệch)` với `ΔP` thu nhỏ khi tới gần; ở SV trở lên là 0. Đỉnh bị chặn bởi độ dốc × τ chứ không bởi độ chính xác của K. Chi phí: lên nhiệt chậm thêm khoảng 5–10 phút (bench đã cho thấy hold thấp chậm thêm 7–12 phút).

### P4. Lưu và dùng lại có thử một giờ
Đã trình bày ở tin trước: 3–4 bản lưu; lưu sau 60 phút ổn định thay vì 2 giờ; bản cũ dùng hằng số lò nguyên vẹn, hold dùng làm điểm xuất phát; 60 phút đầu theo dõi, vọt ≥ +0,15 hoặc Báo cao cắt hoặc lệch kéo dài thì bỏ bản đó, quay về P2 và ghi một bản mới; lưu cả khi chỉ đo được hold.

### P5. Kiểm chứng trước khi tin
1. **Hiệu chuẩn bench theo log thật:** khớp họ lò sao cho tái hiện đúng cả "nguội +0,1" lẫn "gần ngưỡng +0,2/+0,3" của 8 log có sẵn. Chưa làm xong bước này thì không chấp nhận số bench làm bằng chứng.
2. **Cổng nghiệm thu bằng số:** đỉnh ≤ +0,10 °C ở ≥ 95 % lò hiệu chuẩn cho mọi điểm xuất phát (sát SV, −0,5, −2, nguội); không Báo cao trong 3 giờ ổn định; thời gian lên nhiệt không dài quá khoảng 10 phút so với hiện nay; test hồi phục sau mở cửa và vòng kín hiện có vẫn đạt, không nới ngưỡng nào.
3. **Chạy bóng trên máy thật:** bản mới tính nhưng không điều khiển, ghi song song với bản đang chạy trong một mẻ; chỉ bật điều khiển khi hai bên phù hợp ở các điểm anh quan tâm.

### P6. Đóng băng
Không thêm luật vào `thermal_v3.h` cho đến khi có bench đã hiệu chuẩn (P5.1). Bản 2.5.7 là bản vá cuối.

## 6. Những thứ giữ lại
Bộ bảo vệ độc lập (Báo cao, Khẩn cấp, chặn heater, E115), luật quạt hút theo xu hướng, giữ ổn định lâu dài (đã đạt 85 %/97 %), phục hồi sau mở cửa trên log 29/10, và toàn bộ hạ tầng test.

## 7. Điều mình **không** thể hứa
* Không có phương án nào bảo đảm 100 % không vọt +0,2 ở mọi lò và mọi điều kiện; phương án trên nhắm ≥ 95 % trên lò ảo đã hiệu chuẩn và được xác nhận bằng chạy bóng trên lò thật.
* Nếu anh muốn "không bao giờ kêu còi vì dao động 0,2 °C", cần thêm một lớp: Báo cao SV + 0,4, hoặc debounce 2–3 phút cho còi. Đó là quyết định vận hành của anh, không phải của bộ điều khiển.
* Còn phải nghe thử còi và thử quạt hút trên phần cứng.

## 8. Việc cần anh quyết
1. Duyệt P1–P6 làm hướng chính, dừng vá thêm luật.
2. Cho phép mình hiệu chuẩn bench theo các log thật trong `tests`/uploads (cần anh giữ lại các tệp CSV, kèm phiên bản firmware mỗi tệp nếu nhớ).
3. Cho phép chạy bóng một mẻ trên máy MAP-441BF6E051D0.
4. Chọn: giữ Báo cao SV + 0,2 hay nới.
