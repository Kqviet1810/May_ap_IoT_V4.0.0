# Còi và buzzer — bản 2.5.5/2.5.6: kêu đúng việc, tắt ngay khi hết lỗi

## Vì sao

* Nhóm bit "Hệ thống" gộp khoảng 25 mã: mã chẩn đoán (RAM thấp, xu hướng lưu...) kêu cùng nhịp nhanh với mã quan trọng (heater không đáp ứng).
* Lỗi "chốt" kêu tiếp sau khi điều kiện đã hết, đến khi bấm ACK.
* Còi Emergency còn kêu khoảng 30 giây sau khi nhiệt đã hạ (phải chờ điều kiện thoát khóa heater).
* Khi chủ máy đặt ngưỡng Báo cao / Khẩn cấp sát SV (ví dụ 37,7 trên SV 37,5) cần quy tắc rõ ràng cho tiếng kêu: 2.5.5 hạ Báo cao xuống 3 bíp, 2.5.6 theo đúng yêu cầu chủ máy: vượt 0,2 là còi to ngay, hạ 0,2 thì ngừng.

## Nguyên tắc

1. **Âm thanh bám theo điều kiện lỗi đang xảy ra, không bám theo bản ghi lỗi.** Lỗi chốt đã hết điều kiện thì im ngay (bản ghi vẫn hiện để ACK).
2. **Hành động bảo vệ không đổi.** Ngưỡng Báo cao / Khẩn cấp vẫn cắt heater, nhả contactor tổng, ép quạt như trước; tiếng kêu theo đường ngưỡng và ngừng khi hạ 0,2 °C.

## Phân loại 41 mã

| Loại | Âm thanh | Mã |
|---|---|---|
| **Còi** | còi ngoài + buzzer | E111, E112 (giọng riêng bên dưới), E304 |
| **Cảnh báo** (cắt heater) | buzzer đến khi ACK hoặc hết lỗi; sau ACK, còn lỗi thì nhắc lại mỗi 15 phút (trước đây 5 phút) | E101, E102, E103, E104, E115, E130, E301, E303, E313, E314 |
| **Nhắc** | 3 bíp ngắn khi xuất hiện, nhắc thưa mỗi 30 phút khi còn lỗi, không cần ACK | E110, E120, E121, E132, E133, E134, E135, E136, E137, E138, E201–E205, E502 |
| **Im** | chỉ hiện ở HMI, Web, push | E113, E114, E302, E305, E306, E315, E401, E402, E403, E404, E501, E503 |
| **Riêng** | còi to ngay khi vượt ngưỡng, ngừng khi hạ 0,2 °C | E111 (Báo cao), E112 (Khẩn cấp) |

Khác với trước: E138 (mẻ quá hạn chờ xác nhận) không còn bật còi ngoài, chỉ nhắc 30 phút một lần; E113, E114, E403 không còn làm buzzer kêu qua bit "Nhiệt cao"; mã chẩn đoán không còn kêu theo nhóm "Hệ thống".

## Hai mã cao nhiệt (bản 2.5.6: kêu to ngay, hạ 0,2 °C thì ngừng)

Yêu cầu của chủ máy: **vượt 0,2 °C là còi to ngay, hạ 0,2 °C thì ngừng.** Với ngưỡng của chủ máy (SV 37,5; Báo cao 37,7 = SV + 0,2; Khẩn cấp khoảng 37,8) E111 và E112 dùng **cùng một giọng**:

* **Vừa chạm ngưỡng: còi ngoài + buzzer (Temporal-3) kêu ngay**, không còn "3 bíp trước, còi sau" như 2.5.5.
* Còi liên tục 5 phút đầu, sau đó 5 giây kêu / 25 giây nghỉ (một số đọc kẹt trên ngưỡng hàng giờ không thành tiếng ồn liên tục).
* **Nhiệt hạ xuống ngưỡng − 0,2 °C trong 2 giây: tắt ngay**, dù khóa heater/quạt còn giữ thêm cho hết trễ. Lơ lửng giữa ngưỡng − 0,2 và ngưỡng thì không kêu chập chờn: chỉ kêu lại khi nhiệt chạm lại ngưỡng.
* **ACK tại máy** (HMI hoặc lệnh serial; ACK từ xa của Khẩn cấp vẫn bị chặn): im khi nhiệt đang hạ; cứ 2 phút kiểm tra một lần, không hạ thêm ≥ 0,1 °C thì kêu lại; tăng ≥ 0,2 °C so với lúc ACK thì kêu lại ngay; tối đa im 10 phút.
* Mất cảm biến: im (lỗi cảm biến có giọng riêng).
* Còi độc lập của ATtiny khi mất điện giữ nguyên.

Hành động bảo vệ **không đổi**: cắt heater, nhả contactor tổng, ép quạt hút chạy, ngưỡng và trễ của chúng. Chỉ tiếng kêu bám theo đường ngưỡng.

Hệ quả cần biết: với ngưỡng sát SV (37,5 / 37,7) một cú vọt 0,2 °C do cửa hoặc nhiễu sẽ làm còi to kêu, đúng như yêu cầu. Nếu thấy ồn, nới Báo cao lên (ví dụ SV + 0,4).

## Kiểm chứng

`tests/alarm-sound.cpp` chạy đúng mã `alarm_sound.h`: chạm ngưỡng là còi ngay; hạ 0,2 °C trong 2 giây thì tắt (chưa đủ 0,2 hoặc chưa đủ 2 giây thì chưa tắt); không kêu chập chờn quanh ngưỡng; 5 phút liên tục rồi 5/25; ACK im khi đang hạ / kêu lại khi đứng hoặc tăng / tối đa 10 phút; mất cảm biến thì im; ngưỡng Khẩn cấp cùng giọng; tràn đồng hồ 32 bit; nhóm "Nhắc" 30 phút. Các bộ test host đang có chạy qua mã MachineController thật đều đạt, dấu vân tay `runtime-preservation.json` ghim lại có ghi chú xem xét (`release256ReviewNote`).

## Giới hạn

* Phân loại là quyết định vận hành của chủ máy; sửa trong `faultSoundClass()` (`machine_control.h`).
* Chưa nghe thử trên phần cứng và chưa biên dịch ESP32 trong môi trường làm việc (CI làm bước này).
* Web vẫn hiển thị theo bản ghi lỗi (không đổi); push từ Worker không đổi.
