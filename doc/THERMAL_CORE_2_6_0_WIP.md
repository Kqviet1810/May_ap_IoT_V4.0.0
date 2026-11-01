# Lõi nhiệt mới (nhánh làm việc, CHƯA phát hành) — trạng thái đo được

Lõi `thermal_v3.h` được viết lại theo đề xuất đã duyệt: **một bộ nhớ tải duy nhất** (tích phân `load`), P theo sai lệch trừ phần nhiệt còn sắp lên (độ dốc đo 60 s × thời gian trễ + nhiệt đã gửi chưa tới), bù trước quạt theo lịch, bảo vệ cửa. Đã bỏ: bộ học hold, dự đoán "năng lượng đang bay" theo hold, Anti-Stuck, F1, F4, START, PROBE, `respSuspect_`, các trường vào chết. Lõi cũ không còn trong mã.

Phần máy: lưu `load` sau 30 phút ổn định (mẻ đang chạy, nạp lại khi khởi động lại đúng mẻ đó), 60 phút thử rồi hủy nếu không giữ được; điều kiện lưu không còn phụ thuộc cờ `suspect`.

## Đo trên bench 53 lò ảo (mỗi 4 lò), lõi 2.5.8 (A) so với lõi mới (B)

| Tình huống | Đạt A | Đạt B |
|---|---|---|
| lên nhiệt + giữ | 96 % | 100 % |
| quạt theo lịch | 96 % | 100 % |
| khởi động lại | 75 % | 91 % |
| ngày nóng dần | 100 % | 92 % |
| mở cửa 3 phút | 98 % | 85 % |
| mở cửa + khay trứng lạnh | 92 % | **72 %** |
| hủy mẻ rồi chạy lại ngay | 96 % | 79 % |

Ở trạng thái giữ nhiệt lò ảo, sai số RMS trung vị 0,026 °C (A: 0,054), thời gian chạm 37,4 °C trung vị 28 phút (A: 31).

## Chưa đạt

* Sau khi mở cửa cho khay trứng lạnh, 28 % lò ảo nằm lại dưới SP 0,1–0,3 °C đến hết 90 phút (tích phân gần SP chậm). Đây là đúng lỗi "lệch −0,1 kéo dài" trong log 01/11. Thêm bộ tăng tốc tích phân khi PV đứng yên cải thiện (tray 71 → 81 %) nhưng làm xấu thêm các phép thử vòng kín (5 → 14 phép không đạt).
* `tests/thermal-closed-loop.cpp`: 4 phép thử không đạt ở biên nhỏ, không nới ngưỡng nào: lò rỗng chạm SP 2858 s (giới hạn 2470 s), lệnh nhỏ nhất khi lò thấp hơn SP 2,5 °C là 89 % (cần 90 %), 24 h vận hành vọt 0,60 °C (giới hạn 0,60), thanh nhiệt hỏng bị E115 chặn lúc 6130 s (giới hạn 6000 s).
* Chưa thử trên lò thật. Chưa có bản log nào của lõi mới.
