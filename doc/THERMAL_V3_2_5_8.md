# Nhiệt — bản 2.5.8: khởi động của lò chưa được đo

## Vấn đề
Bộ điều khiển chưa nhận dạng lò (trường hợp của máy anh: khởi động từ 34,8 °C chỉ chạy khoảng 51 % công suất nên không bao giờ nhận dạng) dùng hold mặc định 18 %. Hold thật của lò trong buổi chiều nóng chỉ vài phần trăm, nên ở lần lên nhiệt gần ngưỡng và ở lần khởi động lại sát ngưỡng nhiệt hay vọt qua SV + 0,2 (Báo cao 37,7).

## Sửa (chỉ `thermal_v3.h`)
Ở chu kỳ đầu của một bộ điều khiển **không có hold đã lưu**, hold được đặt theo khoảng cách tới SV (e₀):

| e₀ | hold ban đầu |
|---|---|
| ≤ 3 °C (từ sát SV tới dưới ngưỡng 3 °C) | **8 %** |
| > 3 °C (nguội) | 18 % như trước (lần lên nhiệt đo được lò) |

Với e₀ ≤ 1 °C có thêm **vòng dò hold**: cứ mỗi max(120 s, thời gian trễ) mà nhiệt vẫn còn giảm thì hold +2 %; dừng sau hai bước nhiệt không giảm; **nếu nhiệt đã tụt 0,15 °C dưới SV kể từ lúc bắt đầu thì trả hold về 18 % ngay** (lò đang cần nhiệt). Hold nạp từ bản ghi đã lưu không bị thay.

## Đã thử và bỏ
Hold 0 % khi sát SV và 5 % khi gần SV, đúng ý anh: giảm vọt rất mạnh trên bench (khởi động lại sát SV: ≥ +0,2 °C từ 79 % xuống khoảng 4 %) nhưng làm test "khởi động lại khi khay trứng lạnh đầy" của `tests/thermal-closed-loop.cpp` xuống 35,36 °C, thấp hơn giới hạn 35,5 °C của test. Mình không nới giới hạn. Đây là quyết định về yêu cầu của chủ máy (xem cuối tài liệu).

## Đo (bench lò ảo, 70 lò, không Báo cao cắt heater)
| Điều kiện | 2.5.7 | 2.5.8 |
|---|---|---|
| Xuất phát dưới ngưỡng 2 °C (35,5): lò chạm ≥ +0,2 | 39 % | **19 %** (≥ +0,3: 7 % → 0 %), nhưng lên 37,4 chậm hơn khoảng 10 phút |
| Xuất phát dưới ngưỡng 0,5 °C (37,0): ≥ +0,2 / ≥ +0,3 | 57 % / 43 % | **37 % / 20 %** |
| Xuất phát đúng ngưỡng (37,5): ≥ +0,2 / ≥ +0,3 | 67 % / 51 % | 60 % / 30 % |
| Khởi động lại sát SV: ≥ +0,2 / ≥ +0,3 | 79 % / 60 % | **49 % / 17 %**; hồi phục ±0,1 trung vị 23 → 15 phút; 9 % lò tụt dưới 36,9 (trước: 4 %) |
| Lên nhiệt từ nguội (31,4) | 93 % ≥ +0,2 | **không đổi** (bench quá bi quan ở mục này, xem `THERMAL_ASSESSMENT_2026-10-31.md`) |
| Các tình huống còn lại (mở cửa, quạt, đổi phòng...) | 91,6 % đạt | 91,6 % đạt, không đổi |

Kết luận thẳng: **cải thiện vừa phải, không phải lời giải.** Muốn đưa vọt khi khởi động lại về dưới 10 % cần hold ban đầu thấp hơn, và điều đó chạm giới hạn test khay trứng đầy.

## Kiểm chứng
`tests/thermal-overshoot.cpp` (nhóm 7 và 8 mới), `thermal-v3`, `thermal-closed-loop` (giữ nguyên mọi giới hạn), `thermal-replay` (log thật), `heater-response-watch`, `vent-line`, `thermal-learn` đều đạt.

## Quyết định còn mở
Nếu anh chấp nhận để lò xuống 35,3–35,4 °C trong trường hợp hiếm "khởi động lại bộ điều khiển giữa mẻ khi khay đầy trứng lạnh", có thể đặt hold ban đầu sát SV bằng 0–5 % và giảm vọt khi khởi động lại xuống dưới 10 %. Đó là thay đổi giới hạn kiểm thử, cần anh duyệt rõ ràng.
