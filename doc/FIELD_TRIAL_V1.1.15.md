# Chạy thử V1.1.15 trên lò (hướng dẫn một trang)

Bản này đã kiểm chứng **trên mô phỏng**, chưa trên lò thật. Mục đích mẻ thử: xem nó có giữ nhiệt tốt như mô phỏng không, và lấy số liệu để hiệu chỉnh.

## Trước khi chạy
- Chạy lúc không có trứng quý (hoặc lò thử). Có người đứng cạnh trong giờ đầu.
- Công tắc **HMI → NÂNG CAO → "Nhiet du doan"** để **BẬT** (mặc định BẬT). Mẻ so sánh: để TẮT (= hệ cũ như 1.1.14).
- Không đổi ngưỡng High/Emergency. Chúng không phụ thuộc bản này.

## Cần đọc ở PID Monitor (HMI → PID MONITOR, cuộn xuống các dòng "Du doan")
| Dòng | Ý nghĩa | Bình thường |
|---|---|---|
| Du doan: che do | TAT / DANG HOC / DIEU KHIEN / LUI VE PID | DANG HOC khoảng 10–15 phút đầu, sau đó DIEU KHIEN |
| Du doan: sai so mh | sai số mô hình khớp lịch sử 90 phút | < 0,10 °C |
| Du doan: loi gan | sai số trong vài phút gần nhất | < 0,15 °C |
| Du doan: yeu cau | công suất nó đề nghị (%) | gần "Du doan: hold" khi giữ nhiệt |
| Du doan: hold | công suất mô hình cho là đủ giữ SP (%) | 10–30 % với lò điển hình |
| Du doan: tre/qt | độ trễ và quán tính phần tử mô hình học được (s) | cùng bậc độ trễ thật (vài chục giây) |
| Du doan: k / a | ghép nối không khí–trứng và tổn thất (%/°C) | ổn định, không nhảy mạnh |
| Du doan: trung | nhiệt độ trứng ước lượng | gần SP khi đã giữ lâu |
| Du doan: ly do | lý do gần nhất mô hình mất tin cậy: OK / THIEU DL / SAI LON / CAM BIEN / QUA SP / MH KEM | OK |
| Du doan: lui/ph | số lần lùi về PID và số phút bộ dự đoán đã điều khiển | 0 lần; số phút tăng đều |
| Chu ky DK us | thời gian một chu kỳ điều khiển (lần cuối / lớn nhất) | lớn nhất ≪ 5000 µs; **ghi lại số này** (CPU ESP32 chưa từng đo) |

## Khi nào tắt công tắc "Nhiet du doan"
- PV vượt SP hơn **1 °C** (High/Emergency vẫn bảo vệ độc lập, nhưng hãy tắt để so sánh).
- PV kẹt dưới SP hơn **0,3 °C** quá **20 phút** trong lúc "DIEU KHIEN".
- "LUI VE PID" xuất hiện từ **2 lần** trở lên.
- "Chu ky DK us" lớn nhất vượt **50 000 µs** (50 ms).

## Cần ghi lại gửi cho tôi
1. Ảnh (hoặc chép tay) các dòng trên ở: phút 5, 15, 30, 60 sau khi bắt đầu mẻ; và 5 phút sau một lần mở cửa.
2. Thời điểm PV chạm 37,2 °C (hoặc SP − 0,3).
3. PV cao nhất và thấp nhất trong 1 giờ giữ nhiệt (từ màn hình chính).
4. Mô tả lò: số xe/trứng, công suất heater, độ trễ cảm nhận được nếu biết.

## Nhớ
- "Lên nhiệt chậm" ở mẻ đầu **vẫn còn** (khởi động chưa được sửa). Đừng đánh giá hỏng vì điều đó.
- Điều cần xem là giữ nhiệt quanh SP và hồi phục sau mở cửa.
