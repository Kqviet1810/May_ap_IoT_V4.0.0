# Rà soát MAYAP — 30/09/2026

Nguồn: main tại f9ef4ec; firmware/HMI 4.0.0, ATtiny protocol 4, web 12.1.6. Bản sửa nâng web lên 12.1.7. Chưa tạo release mới hay nạp vào thiết bị.

## E503 chớp trên HMI khi bắt đầu/kết thúc mẻ

Thông tin vận hành: một lần bấm START/STOP làm HMI hiện E503 ngay rồi tự hết. Tìm thấy hai đường cảnh báo trung gian còn sót trong bản sửa cũ:

- Tracker chỉ tạo chuyển trạng thái khi giá trị mong muốn đổi. Lệnh ngược cũ vẫn đang chạy/đợi có thể đổi trạng thái ATtiny dù giá trị mong muốn mới bằng giá trị cũ.
- Một frame trả cả batch và activity. ACK của BATCH_START/BATCH_END xác nhận mẻ, nhưng bit bảo vệ tải phụ có thể chưa được điều chỉnh bởi ACTIVITY_ON/OFF. Bản cũ có thể báo E503 từ trường đối ứng trước khi lệnh hiệu chỉnh của nó hoàn tất.

Đã tái hiện bằng updateAttinyLink thật trích từ firmware: START trả batch=1/activity=1 trong khi đích activity=0; STOP trả batch=0/activity=1 trong khi đích activity=0. Kiểm thử thất bại trước bản sửa. Ca riêng tái hiện ACTIVITY_ON cũ còn trong hàng đợi lúc START đến.

Bản sửa đánh dấu cần đối soát khi có lệnh ngược hoặc trường đối ứng còn lệch trong quá trình chuyển. Gửi lệnh hiệu chỉnh tương ứng, xác nhận bằng phản hồi chính lệnh đó. Giữ giới hạn 8 giây; retry không gia hạn vô tận. Lệch thật sau lệnh đúng, lỗi truyền và lệch ở trạng thái ổn định vẫn báo lỗi. Không tắt E503/E501/E502.

Bổ sung log [ATTINY-SYNC]: fault=1 là batch, 2 là activity, 3 là cả hai; có expected/reported/pending và lệnh vừa hoàn tất. E503 ghi thêm mask này trong detail hiện có, không đổi schema lưu trữ.

Giới hạn: đã chứng minh các đường lỗi phần mềm; chưa có trace GPIO/log lúc lỗi trên máy của người dùng nên chưa khẳng định đây là toàn bộ nguyên nhân phần cứng.

## Cảnh báo Node.js trong ảnh

Ảnh là cảnh báo upload-artifact@v4 dùng Node 20, không phải lỗi biên dịch ESP32. Main đã dùng upload-artifact@v6, khai báo runtime node24. Không cần đổi action thêm lần nữa. Log cũ không tự thay đổi; đánh giá bằng build mới của commit hiện tại.

[Action chính thức](https://github.com/actions/upload-artifact/blob/v6/action.yml).

## Kết nối web–ESP32

- Trước đây subscribeAll đăng ký QoS 0. Cờ nâng QoS 1 trong mqttTask có thể thuộc kết nối cũ nếu reconnect thành công cùng vòng lặp. Nay bốn kênh ghi đăng ký QoS 1 ngay mỗi clean session. Lỗi gửi SUBSCRIBE đóng kết nối và backoff. PubSubClient không cung cấp SUBACK trong interface này; chưa phát hiện được broker từ chối quyền truy cập topic.
- Backoff lấy lại millis sau handshake thất bại, tránh deadline tính từ thời điểm quá cũ gây retry liên tục.
- Web yêu cầu sync khi snapshot cũ trên 90 giây, kể cả cấu hình đã đủ.
- Web theo dõi packet MQTT, kể cả PINGRESP. Socket báo connected nhưng im lặng quá 90 giây được tạo lại. Visibility/pageshow phục hồi sẽ đồng bộ subscription/session. ESP32 offline không gây reconnect nếu broker vẫn phản hồi.
- Cache tăng 12.1.7; sửa APP_SHELL trỏ PDF hướng dẫn 1.2 đã xóa sang PDF 1.3 hiện có.

Chưa có log RSSI/broker/router thực tế để kết luận mọi lần mất kết nối đều do phần mềm.

## Arduino OTA theo mỗi reset

> **Đã gỡ bỏ ngày 2026-10-08.** ArduinoOTA (nạp qua LAN từ Arduino IDE), mật khẩu `MAYAP_OTA_PASSWORD` và mDNS không còn trong firmware: nạp trực tiếp chỉ bằng cáp USB, cập nhật từ xa chỉ qua OTA Internet có chữ ký + xác nhận HMI. Phần dưới giữ lại như bằng chứng lịch sử của thiết kế cũ.

1. Reset vật lý EN hoặc cấp nguồn mở cửa sổ 30 phút tính từ uptime, không tính từ lúc Wi-Fi nối.
2. Chỉ mở khi ONLINE, Wi-Fi đã nối, máy rảnh và interlock bảo trì cho phép.
3. Một lần bắt đầu upload đã xác thực tiêu thụ lượt, kể cả upload thất bại. Sai mật khẩu trước khi bắt đầu upload không tiêu thụ lượt.
4. Hết 30 phút chưa nạp thì ArduinoOTA.end đóng dịch vụ/socket. Reconnect, đổi mode và recovery không gia hạn hay mở lại.
5. Upload đã bắt đầu trước hạn được hoàn tất an toàn. Reset tự động do OTA/watchdog/recovery không mở lượt mới. Muốn nạp tiếp phải reset vật lý/cấp nguồn lại.
6. Quy tắc áp dụng dịch vụ Arduino OTA LAN. OTA Internet có chữ ký và xác nhận HMI dùng cơ chế riêng hiện có.

Bật tính năng bằng GitHub Repository Secret MAYAP_OTA_PASSWORD, tối đa 63 ký tự; hoặc điền macro đó trong build_secrets.h khi compile local. Không có secret thì LAN OTA tắt hoàn toàn. Không commit file đã điền mật khẩu. PR compile dùng mật khẩu giả; artifact thử trên máy phải lấy từ build nhánh dùng credential thật của repository.

[ArduinoOTA core 3.3.11 đã đối chiếu](https://github.com/espressif/arduino-esp32/blob/3.3.11/libraries/ArduinoOTA/src/ArduinoOTA.cpp).

## Đánh giá hoạt động và tính năng hữu ích

Đã đọc luồng start/stop/resume, prestart/homing/turning, output interlock, PID/autotune, hậu làm mát, lỗi cảm biến/RTC/EEPROM, stop-intent journal, supervisor và OTA. Giữ các lớp bảo vệ; không đổi schema mẻ hay setpoint nhiệt để chữa E503.

Những tính năng đã có nên tận dụng: phục hồi có kiểm soát sau mất điện, tùy chọn yêu cầu xác nhận, khóa đảo khi cần kiểm tra cơ khí, báo mẻ quá hạn, nhắc việc theo ngày, lịch sử nhiệt và tự kiểm tra còi tùy chọn. Audit cũ trong thư mục audit không phải chứng nhận cho firmware hiện hành.

Đề xuất tiếp theo, chưa tự thêm vào bản sửa:

| Ưu tiên | Tính năng | Lợi ích/điều kiện |
|---|---|---|
| Cao | Chẩn đoán web: RSSI, nguyên nhân/số lần mất Wi-Fi/MQTT, thời điểm và bản build | Phân biệt mạng yếu, broker, firmware và bản nạp cũ |
| Cao | Cảm biến nước cạn, phản hồi quạt/tải | Phát hiện tải không thực sự hoạt động; cần phần cứng |
| Trung bình | Xuất lịch sử mẻ/nhật ký và sao lưu cấu hình | Bảo trì, so sánh chất lượng các mẻ |
| Trung bình | Số trứng, số nở, preset theo loài | Đánh giá tỷ lệ nở; preset cần kiểm chứng vận hành |
| Trung bình | Cảm biến nhiệt độc lập thứ hai và báo chênh lệch | Phát hiện lệch cảm biến/không đều nhiệt; cần phần cứng |

Các đề xuất phần cứng không thay thermostat/thermal fuse độc lập đã nêu trong tài liệu dự án.

## Kiểm thử và chạy thử máy

- 57/57 kiểm thử Node qua, gồm socket chết, snapshot cũ, HMAC/ACK, giao dịch và invariant điều khiển.
- Host C++ đã qua I2C/UART, network recovery, OTA, 448 ca GPIO/decoder, 200 chuyển mẻ, trạng thái trung gian và 100 lần đăng ký MQTT. Dùng mã thật trích từ firmware với HAL giả, không phải đo trên ESP32.
- Release sync, ATtiny protocol, reliability checker và git diff --check qua.
- Biên dịch ESP32-S3 core 3.3.11 / CLI 1.5.1, flash 8 MB, default_8MB, không PSRAM; credential giả chỉ để xác minh compile. Không dùng binary local này để vận hành.
- Bổ sung ca ACTIVITY_OFF trả sai vẫn phải báo E503. Một số lần chạy EXE local bị Windows Application Control chặn; kiểm tra CI Linux cho commit cuối.
- Hai fingerprint được cập nhật có chủ đích: attiny_state_sync.h và MachineController, vì đây là phần sửa E503. Các fingerprint bảo vệ nhiệt, bus, schema và giao dịch khác giữ nguyên.

Trên máy thật: START/STOP bằng HMI với quạt đang chạy và khi tải nghỉ; lặp nhiều lần. Ghi [ATTINY], [ATTINY-BUS], [ATTINY-SYNC] nếu E503 còn xuất hiện. Kiểm tra khóa màn hình/quay lại web, mất mạng/nối lại, OTA tại phút 29/30, nạp lỗi, nạp thành công và reset vật lý. Chưa thực hiện các thử nghiệm này vì không có kết nối thiết bị.
