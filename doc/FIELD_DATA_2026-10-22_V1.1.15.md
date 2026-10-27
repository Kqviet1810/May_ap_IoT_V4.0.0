# Số liệu lò thật: mẻ đầu sau khi nạp V1.1.15 (đọc từ 7 ảnh chụp PID Monitor Web)

Nguồn: 7 ảnh chụp trang PID Monitor của Web do chủ lò gửi (`Downloads.rar`, ~1 giờ đầu của một mẻ). Đây là **số đọc từ ảnh** (sai số đọc khoảng ±0,1 °C và ±1 %), không phải log số.
Web không có các dòng "Du doan" (chỉ có trên HMI), nên **chưa biết bộ dự đoán có đang lái hay không**.

SP = 37,30 °C. Mốc thời gian lấy từ nhãn "N phút gần nhất" của biểu đồ (ảnh chụp lúc N phút sau khi bắt đầu mẻ).

| t (phút) | PV | Sai số | PID P+I+D | Giới hạn Adaptive | Yêu cầu cuối | SSR TB 60 s | Hold FF | Heater gain | Delay (s) | Hold power | Profile conf. |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 8 | 34,18 | 3,12 | 78,1 % | **0–2 %** | 2,4 % | 10,4 % | 3,9 | 0,004 | 114 | 16,9 % | 12 % |
| 16 | 35,30 | 2,00 | 9,2 % | **0–0 %** | **0,0 %** | 0,6 % | 3,9 | 0,003 | 124 | 16,9 % | 20 % |
| 26 | 36,20 | 1,10 | 5,0 % | 0–39 % | 8,9 % | 11,1 % | 3,9 | 0,002 | 55 | 16,9 % | 26 % |
| 30 | 36,50 | 0,80 | 3,7 % | 0–21 % | 7,6 % | 6,1 % | 3,9 | 0,002 | 55 | 0,0 % | 45 % |
| 40 | 37,00 | 0,30 | 1,5 % | 0–28 % | 5,4 % | 4,8 % | 3,9 | 0,002 | 55 | 2,1 % | 55 % |
| 51 | 37,40 | −0,10 | −0,1 % | 0–21 % | 3,8 % | 3,3 % | 3,9 | 0,002 | 55 | 3,8 % | 45 % |
| 54 | 37,50 | −0,20 | 0,0 % | 0–100 % | 0,0 % | 0,2 % | 0,0 | 0,002 | 55 | 3,8 % | 45 % |

## Điều đọc được từ số liệu

1. **PV bắt đầu ~34,2 °C, tụt xuống ~33,6 °C trong vài phút đầu** (heater đang 60 % rồi về 0 trong ~2 phút), rồi nằm phẳng. Sau đó tăng chậm: 33,6 → 37,0 trong ~40 phút, tốc độ khoảng 0,08–0,1 °C/phút. **Chạm SP−0,3 °C ≈ 40 phút**; vọt khoảng +0,2 °C (37,5 với SP 37,3) lúc ~54 phút.
2. **Giới hạn Adaptive kẹp công suất khi PV còn cách SP 2–3 °C:** ở ảnh 8 phút, PID đòi 78 % nhưng giới hạn 0–2 % nên yêu cầu cuối chỉ 2,4 %. Ở ảnh 16 phút, giới hạn 0–0 % nên yêu cầu cuối 0 % trong khi PV còn thấp hơn SP 2 °C. Trong mã (`thermal_control.h`), giới hạn trên của PID là `min(maxOut, hold_ff + authority)` với `maxOut` là trần do bộ phanh khởi động đưa ra, nên phanh khởi động là thủ phạm gần nhất. Cũng cần đối chiếu với hold FF học được chỉ 3,9 %.
3. **Các giá trị học được của hệ cũ không ổn định:** Hold power 16,9 % → 0 → 2,1 → 3,8 %; Heater gain 0,004 → 0,002; Delay 114 → 124 → 55 s; Profile confidence 12 → 55 → 45 %. Công suất giữ thực tế ở cuối mẻ ~3–8 % (SSR TB 60 s: 3–5 %).
4. **Công suất yêu cầu có dạng dốc tuyến tính** (0 → ~55 % trong vài phút, ảnh 16 phút) rồi nhiều xung nhọn: dấu hiệu tích phân PID cộng dồn bị kìm bởi giới hạn, rồi xả khi giới hạn mở.
5. Quạt hút (dải xám) chạy ở cuối ảnh 54 phút.

## Chưa biết, cần thêm
- Dòng HMI "Du doan: che do / sai so mh / ly do / lui/ph / Chu ky DK us" (để biết bộ dự đoán ở trạng thái nào và CPU).
- Phiên bản firmware đang chạy.
- Nhiệt độ phòng, ảnh hưởng của đợt nạp trứng (tụt 0,6 °C đầu mẻ có thể là hiệu ứng nạp trứng nguội hay mở cửa).
