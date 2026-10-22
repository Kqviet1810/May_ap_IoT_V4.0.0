# Nghiên cứu thay mới hệ nhiệt — lò ảo hai nút, trần vật lý, nguyên mẫu dự đoán

Trạng thái: **nghiên cứu trên mô phỏng, chưa đụng firmware, chưa kiểm chứng trên lò thật.**
Mọi con số dưới đây là của mô phỏng. Chúng không phải kết quả đo trên lò.

## 1. Vì sao phải làm lại (kết luận từ số liệu lò của người dùng)

Thông số người dùng cung cấp: buồng 2,5 m × 2 m, 4 thanh nhiệt × 5 kW (firmware đang ghi 16 kW,
**chưa đối chiếu được**), quạt tuần hoàn cánh 1,5 m / động cơ 2 kW đặt giữa hai khoang, 2 quạt hút
(mỗi khoang một quạt), 4 xe trứng, khoảng 10.000 quả.

Vật lý suy ra từ đó (giả định, cần số đo để xác nhận):

- Khối trứng khoảng 600 kg ⇒ nhiệt dung khoảng 1,9 MJ/°C. Không khí cộng thép/khay chỉ khoảng 0,3–0,8 MJ/°C.
  Lò có **hai nút nhiệt**: không khí (nhanh) và trứng (chậm). Cảm biến chỉ đo không khí.
- Trứng trao đổi nhiệt với không khí qua hA khoảng 1000–1700 W/°C ⇒ hằng số thời gian trứng khoảng 20–30 phút.
- Quạt tuần hoàn 2 kW và nhiệt phôi (giữa kỳ ấp trở đi) cùng nằm trong buồng ⇒ **hold chỉ còn vài %**
  (khớp với hold 1,7–2,6 % người dùng đọc được) và **lò không làm nguội được ngoài quạt hút**.
- Hệ quả: sau khi vọt, toàn bộ khối (khoảng 2,4 MJ/°C) giữ năng lượng thừa; chỉ xả được cỡ vài trăm W ⇒
  hồi về SP mất 20–30 phút hoặc hàng giờ. Đây là đúng triệu chứng người dùng mô tả.

Nguồn hỗ trợ định tính: embryo heat nâng nhiệt trứng trên nhiệt máy từ giữa kỳ ấp, và truyền nhiệt phụ thuộc
tốc độ gió (French 1997 PMID 9037699; "Quantification of the heat exchange of chicken eggs" PMID 15782902).
Các hằng số cụ thể (hA, nhiệt phôi mỗi quả) **chưa có nguồn**, được quét theo dải.

## 2. Công cụ

- `tests/thermal-adaptive-sim.h`: `Plant` có thêm nút trứng tùy chọn (`eggCap`, `hA`, `fanW`, `eggW`,
  `eggStart`, `eggWEnd`, `eggRampS`, `heaterW`). Mặc định tắt: các cổng cũ không đổi
  (đã chạy `test_thermal_adaptive`, `test_thermal_control`, `test_smart_autotune --smart-thermal`,
  `test_thermal_sensor_path`, `check_release_sync`, `check_reliability`: đều đạt).
- `tools/thermal-research/baseline_single_node.cpp`: mốc firmware 1.1.14 trên lò một nút cũ (36 lò × 7 kịch bản).
- `tools/thermal-research/twin_firmware_baseline.cpp`: firmware 1.1.14 **thật** chạy trên lò ảo hai nút (72 cấu hình).
- `tools/thermal-research/mpc_prototype.cpp`: nguyên mẫu điều khiển dự đoán (chỉ trên PC, chưa phải code firmware).
- `audit/thermal-twin/*.csv`: số liệu thô.

Lưới 72 cấu hình: Cf ∈ {0,3; 0,5; 0,8} MJ/°C, hA ∈ {1000; 1700} W/°C, nhiệt quạt ∈ {1500; 2000} W,
nhiệt phôi cuối kỳ ∈ {0; 1500} W, trễ ∈ {20; 60; 120} s. Trứng 1,9 MJ/°C. Sưởi 20 kW. Tổn thất chọn sao cho hold cuối kỳ = 3 %.

## 3. Kết quả

### 3.1 Lò một nút cũ (mô hình trước đây): firmware đã ở mức sàn
Vọt tối đa 0,05–0,10 °C ở khởi động, giữ nhiệt, kích 0,1 °C, mở cửa. Điểm yếu: nhiệt phòng đổi, tải nhẹ.
Mô phỏng này **không tái hiện được** cú vọt người dùng thấy ⇒ không dùng để đo cải thiện.

### 3.2 Firmware 1.1.14 trên lò hai nút theo thông số người dùng (72 cấu hình)
| Kịch bản | Vọt (med/p90/max °C) | Ổn định (s, med/p90/max) | Chạm High |
|---|---|---|---|
| Mẻ đầu, trứng lạnh | 0,51 / 0,75 / 1,66 | 3329 / 14279 / 14399 | 180 |
| Nhiệt phòng +5 °C | 0,31 / 0,70 / 0,70 | hồi 239 / 7199 / 7199 | 189 |

Mẻ đầu ổn định sau trung vị 55 phút (người dùng quan sát khoảng 60 phút). Cơ chế: trứng lạnh hút nhiệt, mô hình
một nút nghĩ lò đã đủ nóng, ngắt sưởi sớm, PV vọt rồi tụt rồi lặp.

### 3.3 Trần vật lý (bộ điều khiển biết đúng mô hình và trạng thái)
Mẻ đầu ổn định trung vị khoảng 10 phút, vọt tối đa 0,43 °C, 0 lần chạm High.

### 3.4 Nguyên mẫu chỉ thấy nhiệt độ không khí (ước lượng Kalman 4 trạng thái: Ta, Te, w, d)
So cặp trên cùng 72 lò (kịch bản T3–T6 chỉ 44 lò do firmware không đưa ra kế hoạch ở các lò còn lại):

| Kịch bản | Firmware 1.1.14 | Nguyên mẫu (mô hình đúng) |
|---|---|---|
| Mẻ đầu: ổn định med/p90/max | 55 / 238 / 240 phút | 10 / 28 / 43 phút |
| Mẻ đầu: vọt med/p90/max | 0,51 / 0,75 / 1,66 | 0,14 / 0,36 / 0,62 |
| Mẻ đầu: chạm High | 180 | 0 |
| Nhiệt phôi tăng 0→1500 W trong 8 giờ: vọt max | 0,39 | 0,06 |
| Nhiệt phòng +5 °C: hồi p90 | 7199 s | 580 s |
| Nhiệt phòng +5 °C: chạm High | 189 | 0 |
| Mở cửa −3 °C: hồi med | 179 s | 170 s (giới hạn vật lý) |
| Kích 37,4→37,5 | tương đương | tương đương |

### 3.5 Độ nhạy với mô hình sai (điểm yếu thật)
| Niềm tin sai về hA, Ce, UA, Cf | Mẻ đầu ổn định med/p90 | Ghi chú |
|---|---|---|
| −30 % cả bốn | 17 / 63 phút | vẫn tốt hơn firmware |
| +30 % cả bốn | 11 / 38 phút | vẫn tốt hơn firmware |
| lệch hỗn hợp (−30 % hA/Cf, +30 % Ce/UA) | 112 / 235 phút | **bằng hoặc tệ hơn firmware**, 17 lần chạm High |

Kết luận: **mô hình cố định không đủ**. Muốn dùng được trên lò lạ phải có nhận dạng online các tham số.
Phần này chưa làm.

## 4. Giới hạn đã biết (không bao che)

1. Lò ảo do tôi dựng từ thông số và giả định. Chưa có số liệu thật để hiệu chỉnh.
2. Chỉ đo nhiệt độ không khí ⇒ nhiệt phòng đổi và nhiệt trứng tỏa không tách được khi PV đã ổn định.
   Bộ ước lượng chỉ tách được nhờ thang thời gian khác nhau. Có thể sai kéo dài. Đã thêm rào "không mở quạt hút khi PV dưới SP".
3. Nguyên mẫu chạy trên PC bằng mô hình lò của chính nó, chưa tích hợp bộ chia xung SSR, bộ lọc cảm biến, vòng an toàn.
4. Cú rớt nhiệt khi mở cửa gần như do vật lý (hồi phục không cải thiện được); cải thiện chủ yếu ở mẻ đầu, đổi nhiệt phôi và nhiệt phòng.
5. Quạt hút trong mô phỏng chỉ có một công suất 200 W/°C (giả định). Chưa biết lưu lượng thật.
6. Firmware ghi tổng sưởi 16 kW, người dùng nói 4 × 5 kW = 20 kW. Cần xác nhận.

## 5. Việc tiếp theo (chưa làm)

1. Bộ ghi dữ liệu trên lò thật (S0) để hiệu chỉnh lò ảo và kiểm chứng.
2. Nhận dạng online hA, Ce, UA, trễ (RLS hoặc tương đương), thử lại bảng 3.5.
3. Tích hợp vào đường điều khiển thật, chạy bóng, rồi mới cho điều khiển; PID là phương án lùi.
4. Đọc tiếp bằng sáng chế Honeywell/Watlow, sách *Advanced PID Control* khi có tài liệu.
