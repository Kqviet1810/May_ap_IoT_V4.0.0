# Điều khiển nhiệt V3 — bản 2.6.0: máy nhớ những gì đã học, quạt hút là phương án cuối

Bản 2.6.0 gồm hai phần. Phần quạt hút và log chẩn đoán đã mô tả ở `THERMAL_V3_2_5_1.md` (không đổi). Tài liệu này mô tả phần còn lại: **lưu những gì bộ điều khiển nhiệt đã học qua lần khởi động lại** (mất điện, cập nhật OTA, treo máy).

Luật điều khiển (`thermal_v3.h`) không đổi. Chỉ thêm ba hàm để nạp lại số đã học (`importOven`, `importHold`, `pipeLen`).

## 1. Vấn đề

Bộ học (`pHold`, hệ số gia nhiệt `kAir`, độ trễ `τ`) nằm trong RAM. Hủy mẻ rồi chạy lại ngay thì còn nguyên (máy vẫn có điện), nhưng mất điện, cập nhật OTA hay khởi động lại giữa mẻ đưa bộ điều khiển về giá trị mặc định (hold 18 %, τ 150 s, kAir 4e-4): vài cú vọt nhỏ +0,1 °C trước khi học lại.

## 2. Cách làm (`thermal_learn.h`, nối trong `machine_control.h`)

* **Chỉ lưu trạng thái đã chứng minh.** Sau **2 giờ ổn định liên tục**: mẻ đang chạy, heater được phép, |PV − SV| ≤ 0,2 °C, không cửa mở, không nghi ngờ đáp ứng heater, không High/Emergency, không chế độ thử, đã đo được τ, và `pHold` đứng yên (lệch ≤ 1 % trong 30 phút). Lần ghi sau chỉ khi lại có 2 giờ ổn định **và** `pHold` lệch hơn 1,5 % so với bản đã lưu: tối đa 12 lần ghi một ngày.
* **Bản ghi 32 byte** trong NVS (`mayap_thm`/`snap`): magic, phiên bản, công suất tối đa của heater, SV, `pHold`, `kAir`, `τ`, độ dài đường truyền, **thời điểm bắt đầu mẻ**, thời điểm lưu, CRC32. Ghi nguyên khối.
* **Khi bật máy:** bản ghi hỏng (CRC, phiên bản, ngoài giới hạn) bị bỏ **nguyên cả bản**; bộ điều khiển chạy từ giá trị mặc định như trước. Bản ghi tốt thì nạp `kAir`, `τ` và độ dài đường truyền — chỉ khi công suất tối đa heater vẫn như lúc lưu — làm **điểm xuất phát** của lần lên nhiệt kế tiếp (nó vẫn đo lại: không coi là đã đo).
* **`pHold` chỉ nạp khi nối lại đúng mẻ đó** (cùng thời điểm bắt đầu mẻ, SV lệch ≤ 1 °C). Mẻ mới thì `pHold` về 18 % như cũ: hold học ở ngày 18 sai ở ngày 1.
* **Xóa:** nút **"Xoa du lieu hoc nhiet"** ở HMI Nâng cao (chỉ tại máy, không xóa được khi đang ấp), khôi phục cài đặt gốc, và tự xóa khi E115 (heater không đáp ứng) xuất hiện.
* Log PID có cột `learn_src` (0 mặc định, 1 đã nạp số của lò, 2 đã nạp cả hold của mẻ đang nối lại), `learn_saved` (đã có bản lưu) và `stable_min` (số phút ổn định tích lũy, bước 4) — cột mới thêm ở cuối CSV của Web 1.1.27.

## 3. Kiểm chứng

* `tests/thermal-learn.cpp`: mã hóa khứ hồi; **mọi bit bị lật** đều bị từ chối; sai độ dài/phiên bản/ngoài giới hạn; công suất khác nhau; hold chỉ cho đúng mẻ và SV; 2 giờ ổn định mới ghi (không sớm hơn một giây), không xác nhận thì hỏi lại sau 10 phút; sau khi ghi thì 2 giờ không ghi lại, rồi chỉ khi hold lệch > 1,5 %; mọi gián đoạn (báo động, PV lệch, im lặng > 5 s) đếm lại từ 0; hold đang trôi thì không lưu; xóa thì ghi lại sau 2 giờ ổn định; tràn đồng hồ 32 bit; bộ điều khiển nạp số và chạy tiếp ở SV với hold 11,5 % → 11,9 % sau 10 phút.
* **Bench 210 lò** (`fleet-bench.cpp`, quạt 100 W; B = bản ghi của lần chạy ổn định một giờ trước; đạt = về ±0,1 °C trong 40 phút và không xuống dưới 36,5 °C):

| Tình huống | Đạt | Về ±0,1 °C (trung vị / p90) | PV thấp nhất p10 | PV cao nhất p90 | Phút ≥ SP+0,1 (trung vị) |
|---|---|---|---|---|---|
| khởi động lại 45 s, **không lưu** (2.5.0) | 85,2 % | 20,1 / 70,7 phút | 37,10 | 38,10 | 9,8 |
| khởi động lại 45 s, **có bản lưu** | **92,9 %** | 0,0 / 35,0 phút | 37,20 | **37,70** | **3,1** |
| hủy mẻ 2 phút rồi chạy lại ngay (RAM giữ nguyên) | 96,2 % | 8,3 / 18,6 phút | 37,00 | 37,60 | 1,7 |

## 4. Giới hạn (nói thẳng)

* Bản lưu trong bench lấy từ chính lần chạy mô phỏng một giờ trước, khi lò đã ổn: **lạc quan** hơn thực tế. Hold lúc mất điện có thể khác hold lúc lưu (ví dụ cửa mở, trứng mới); bộ học vẫn chạy tiếp và là người quyết định cuối.
* Dữ liệu lưu phụ thuộc đồng hồ thời gian thực (RTC) chỉ ở chỗ `savedEpoch`; thiếu RTC thì vẫn lưu và nạp số của lò, nhưng `pHold` chỉ nạp khi mẻ nối lại có thời điểm bắt đầu đúng.
* Nối lại sau mất điện **lâu** (lò nguội hẳn): số của lò vẫn dùng được, `pHold` đã học có thể cao hơn nhu cầu lúc đó; bộ học hạ xuống nhanh (hằng số 40 s) như mọi lúc.
* Chưa biên dịch cho ESP32 trong môi trường làm việc; kiểm tra này do CI làm. Việc ghi NVS chạy trong chu kỳ điều khiển (khoảng 1–5 ms mỗi lần, tối đa vài lần một ngày); chưa đo trên phần cứng.
* Mục 8 (Anti-Stuck dùng sai lệch trung bình) và mục 9 (bù trước khi quạt tắt/bật) không có trong bản này.
