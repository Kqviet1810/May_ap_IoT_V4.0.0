# Nhiệt — bản 2.5.7: khởi động lại sau khi bảo vệ cắt heater không vọt lại

## Hiện tượng (log 31/10, máy MAP-441BF6E051D0, ngưỡng của chủ máy: SV 37,5 / Báo cao 37,7)

* Lên nhiệt lần đầu: 08:47 → 09:07 nhiệt chạm 37,5; heater đang ~18–20 %; vài phút sau nhiệt vọt lên 37,6 rồi 37,7 (Báo cao).
* Báo cao cắt heater 09:10:16 → 09:24:46 (14,5 phút): nhiệt chỉ tụt từ 37,7 xuống 37,5 dù heater 0 % và quạt hút chạy. Nghĩa là lò gần như **không mất nhiệt** lúc đó.
* Heater trở lại lúc 09:24:56 với đúng **17,5 %** như trước khi bị cắt, và nhiệt lại bắt đầu nhích lên.

## Nguyên nhân (đọc từ log mở rộng + mã)

1. Bộ điều khiển chưa đo được lò: `identified = 0` suốt mẻ, độ lợi `kAir = 4e-4` và trễ 150 s là số mặc định, **hold mặc định 18 %** (cột `hold_pct` đi 18 → 20 → 17,5).
2. Hold thật của lò trong buổi chiều nóng chỉ vài phần trăm (ước tính thô từ log: ~4 %). Hold sai làm mô hình đánh giá thiếu lượng nhiệt đang trên đường tới.
3. Bộ học hold không sửa được: hold và độ lợi nằm chung một phương trình; độ lợi mặc định cao gấp khoảng 2–4 lần nên bộ học thấy "hold = 18 % là đúng".
4. Trong lúc bảo vệ cắt heater, bộ điều khiển ở trạng thái "đóng băng": không học gì và hold giữ nguyên 18 % dù 14 phút đo được rằng lò gần như không mất nhiệt. Heater quay lại với hold sai ấy.

## Sửa (chỉ trong `thermal_v3.h`; `machine_control.h` không đổi)

Khi heater bị giữ tắt bởi bảo vệ (mất quyền chạy, không phải lệnh giữ của quạt hút) và số đọc còn hợp lệ:

* **F4 — vọt do chính mình:** ở chu kỳ bộ điều khiển mất quyền chạy mà nhiệt đang ≥ SV + 0,15 °C, hold giữ lại **60 %** (một lần cho mỗi lần cắt). Nhiệt vừa vọt là bằng chứng hold quá lớn.
* **F1 — heater tắt mà nhiệt không giảm:** cứ mỗi 3 phút, nếu nhiệt còn ≥ SV − 0,05 và không giảm quá một bước cảm biến, hold giảm dần (hằng số thời gian 180 s). Đây chính là quy tắc đã có sẵn cho lệnh giữ của quạt hút, nay áp dụng cả khi bảo vệ cắt. Nhiệt giảm thật thì hold không đổi và các luật cũ (Anti-Stuck, bộ học) nâng hold lại khi cần.

Đã thử và **bỏ** (đều đo trên bench và test vòng kín, không nới test nào):

* Hạ hold khi nhiệt đứng trên SV đang chạy: làm chậm hồi phục sau mở cửa của log thật 29/10 (test 1 của `thermal-closed-loop` không còn đạt).
* Phanh động lượng thứ hai trên độ dốc 2 phút: không thêm lợi ích so với F1, và làm vòng kín với quạt hút xấu đi.

## Đo

Bench `tools/thermal-research/fleet-bench.cpp` (lò ảo hai họ, 10 tình huống, có mô hình "Báo cao 37,7 cắt heater" bằng `HIGHCUT=37.7`; `FLEET_STRIDE=n` lấy mỗi n lò):

| | 2.5.6 | 2.5.7 |
|---|---|---|
| Tỉ lệ đạt trung bình (53 lò, mỗi 4 lò) | 64,2 % | 92,8 % |
| Số lần Báo cao cắt heater (53 lò × 10 tình huống) | 2763 | 307 |

Đợt thứ hai trên tập lò khác (70 lò, mỗi 3 lò): đạt trung bình **59,3 % → 91,3 %**, số lần Báo cao cắt heater **3832 → 405**. Khi **không** có Báo cao cắt heater (bench thường, không `HIGHCUT`) kết quả của 2.5.6 và 2.5.7 giống hệt nhau (đạt 91,6 % cả hai, 70/70 lò không kém hơn): hai luật chỉ hoạt động khi bảo vệ đã cắt heater. Đỉnh của lần lên nhiệt đầu không đổi (trung vị +0,20 °C so với SV). Bench là bằng chứng cấu trúc, không phải phép đo một lò thật; lò thật và log chiều 31/10 sau khi nâng cấp mới là phép thử cuối.

## Kiểm chứng

* `tests/thermal-overshoot.cpp` chạy đúng mã `thermal_v3.h`: cắt lúc vọt giữ 60 %; heater tắt 14 phút khi nhiệt đứng ở 37,6 thì hold về dưới 5 % và khởi động lại chỉ xin vài phần trăm; nhiệt đang giảm thì hold không đổi; cắt khi chưa vọt, lệnh giữ của quạt hút và số đọc không hợp lệ không đổi gì.
* Các bộ test hiện có đều đạt không đổi ngưỡng: `thermal-v3`, `thermal-closed-loop` (kể cả hồi phục sau mở cửa của log 29/10), `thermal-replay` (log thật), `heater-response-watch`, `vent-line`, `thermal-learn`.

## Giới hạn

* Lần lên nhiệt đầu của mẻ vẫn dùng hold mặc định 18 % cho đến khi gặp một lần cắt hoặc tự học; bản này sửa **cái vòng lặp vọt – cắt – quay lại với hold sai**, chưa làm hold mặc định tự đúng ngay từ đầu. Lần vọt đầu tiên có thể vẫn chạm Báo cao ở ngưỡng sát SV + 0,2.
* Cần log mới sau nâng cấp để xác nhận trên lò thật.
