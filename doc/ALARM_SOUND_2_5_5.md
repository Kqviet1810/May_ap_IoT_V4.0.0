# Còi và buzzer — bản 2.5.5: kêu đúng việc, tắt ngay khi hết lỗi

## Vì sao

* Nhóm bit "Hệ thống" gộp khoảng 25 mã: mã chẩn đoán (RAM thấp, xu hướng lưu...) kêu cùng nhịp nhanh với mã quan trọng (heater không đáp ứng).
* Lỗi "chốt" kêu tiếp sau khi điều kiện đã hết, đến khi bấm ACK.
* Còi Emergency còn kêu khoảng 30 giây sau khi nhiệt đã hạ (phải chờ điều kiện thoát khóa heater).
* Khi chủ máy đặt ngưỡng Báo cao / Khẩn cấp sát nhau và sát SV (ví dụ 37,7 / 37,8 trên SV 37,5), chỉ lệch 0,2 °C là còi to, dù heater và quạt đã xử lý.

## Nguyên tắc

1. **Âm thanh bám theo điều kiện lỗi đang xảy ra, không bám theo bản ghi lỗi.** Lỗi chốt đã hết điều kiện thì im ngay (bản ghi vẫn hiện để ACK).
2. **Ngưỡng bảo vệ và tiếng kêu tách nhau.** Ngưỡng Báo cao / Khẩn cấp vẫn làm đúng việc của chúng (cắt heater, nhả contactor tổng, ép quạt). Chỉ **tiếng kêu** không còn bị ép theo ngưỡng.

## Phân loại 41 mã

| Loại | Âm thanh | Mã |
|---|---|---|
| **Còi** | còi ngoài + buzzer | E112 (theo giọng riêng bên dưới), E304 |
| **Cảnh báo** (cắt heater) | buzzer đến khi ACK hoặc hết lỗi; sau ACK, còn lỗi thì nhắc lại mỗi 15 phút (trước đây 5 phút) | E101, E102, E103, E104, E115, E130, E301, E303, E313, E314 |
| **Nhắc** | 3 bíp ngắn khi xuất hiện, nhắc thưa mỗi 30 phút khi còn lỗi, không cần ACK | E110, E120, E121, E132, E133, E134, E135, E136, E137, E138, E201–E205, E502 |
| **Im** | chỉ hiện ở HMI, Web, push | E113, E114, E302, E305, E306, E315, E401, E402, E403, E404, E501, E503 |
| **Riêng** | giọng riêng, xem dưới | E111 (Báo cao), E112 (Khẩn cấp) |

Khác với trước: E138 (mẻ quá hạn chờ xác nhận) không còn bật còi ngoài, chỉ nhắc 30 phút một lần; E113, E114, E403 không còn làm buzzer kêu qua bit "Nhiệt cao"; mã chẩn đoán không còn kêu theo nhóm "Hệ thống".

## Hai mã cao nhiệt

**E111 Báo cao** (heater đã cắt, quạt đã ép chạy): 3 bíp khi xuất hiện rồi **im**, không cần ACK. Chỉ bíp lại khi nhiệt tăng thêm ≥ 0,2 °C so với lần bíp trước, hoặc 10 phút không hạ ≥ 0,1 °C; không bao giờ dày hơn 5 phút một lần. ACK thì im đến khi lỗi hết. Hết lỗi thì không còn gì để tắt.

**E112 Khẩn cấp** (heater bị nhả contactor tổng):
* Vừa chạm ngưỡng: **3 bíp ngắn**, chưa có còi (heater và quạt đang xử lý).
* **Còi ngoài + buzzer (Temporal-3)** khi một trong ba điều kiện: nhiệt ≥ max(ngưỡng Khẩn cấp, SV + 0,8 °C); hoặc đã ở trên ngưỡng 180 giây mà chưa hạ; hoặc đã tăng thêm 0,3 °C kể từ lúc chạm ngưỡng. Với ngưỡng mặc định 39,0 thì còi kêu ngay như trước.
* Còi liên tục 60 giây đầu, sau đó 5 giây kêu / 25 giây nghỉ.
* **ACK tại máy** (từ xa vẫn bị chặn): im khi nhiệt đang hạ; cứ 2 phút kiểm tra một lần, nếu không hạ thêm ≥ 0,1 °C thì kêu lại; tăng ≥ 0,2 °C so với lúc ACK thì kêu lại ngay; tối đa im 10 phút.
* Nhiệt xuống dưới ngưỡng − 0,1 °C trong 2 giây: **tắt ngay**, dù khóa heater còn giữ thêm cho hết trễ. Chỉ kêu lại nếu nhiệt lại vượt ngưỡng + 0,1 °C.
* Còi độc lập của ATtiny khi mất điện giữ nguyên.

## Kiểm chứng

`tests/alarm-sound.cpp` chạy đúng mã `alarm_sound.h`: ngưỡng sát (note rồi còi sau 180 giây, hoặc lập tức khi tăng 0,3 hay chạm SV + 0,8), ngưỡng mặc định (còi ngay), nhịp 60 giây liên tục rồi 5/25, tắt trong 2 giây khi nguội và không kêu lại khi chỉ lơ lửng quanh ngưỡng, ACK im khi đang hạ / kêu lại khi đứng hoặc tăng / tối đa 10 phút, mất cảm biến thì im, tràn đồng hồ 32 bit; Báo cao một lần rồi chỉ khi xấu đi, ACK; nhắc 30 phút. Các bộ test host đang có chạy qua mã MachineController thật đều đạt, dấu vân tay `runtime-preservation.json` ghim lại có ghi chú xem xét.

## Giới hạn

* Phân loại là quyết định vận hành của chủ máy; sửa trong `faultSoundClass()` (`machine_control.h`).
* Chưa nghe thử trên phần cứng và chưa biên dịch ESP32 trong môi trường làm việc (CI làm bước này).
* Web vẫn hiển thị theo bản ghi lỗi (không đổi); push từ Worker không đổi.
