# Adaptive Staged Boot — MAYAP 1.0.0

Nhánh: `feat/adaptive-staged-boot`, từ baseline `25fd4e6` trên `main`.

## Boot flow

`setup()` áp safe outputs trước mọi I/O, đọc chẩn đoán reset, tạo mutex/I2C,
khởi tạo WDT với nguyên timeout/panic và mở preview LCD. Preview chỉ vẽ logo;
encoder, buzzer và task HMI chỉ bắt đầu ở stage HMI.

`loop()` chạy state machine với nhịp nhường CPU 10 ms. Mỗi bước local có dwell
150 ms bằng thời gian, không dùng `delay()` để đợi splash, mạng hay recovery.

| Stage | Công việc |
|---|---|
| Safe outputs | Tắt các ngõ ra theo helper fail-safe hiện có; trạng thái “Kiểm tra phần cứng” |
| Storage | Identity NVS, RTC/AT24C32; “Khởi tạo bộ nhớ” |
| Sensor / Machine | Toàn bộ `Machine.begin()` còn lại: safety journal, ATtiny, input, sensor, config/reminders, recovery mẻ; “Khởi tạo cảm biến” |
| HMI | Encoder, buzzer, snapshot config/runtime và task HMI; “Khởi tạo điều khiển” |
| Control / Safety | Metadata rollback local, task control/PID/safety và Supervisor; “Khởi tạo an toàn” |
| Local settle | Chỉ task local chạy, tối thiểu 3 giây heartbeat khỏe liên tục |
| Wi-Fi | Tạo task Wi-Fi; chính task này gọi `mayapNetworkBegin()`; “Kết nối mạng” |
| MQTT | Tạo task MQTT và cấu hình client trong chính owner task; “Kết nối máy chủ” |
| Cloud | Tạo task Cloud riêng |
| OTA | Tạo task OTA riêng, giữ maintenance interlock và xác nhận vật lý hiện có |
| Running | Gỡ riêng boot coordinator khỏi TWDT; control/Supervisor tiếp tục được TWDT giám sát |

Các task giữ nguyên stack tĩnh, core, priority và chu kỳ runtime. Task mạng được
tạo một lần, không create/delete lặp lại. MQTT vẫn keepalive 30 s, socket timeout
5 s, TLS connection timeout 5 s, handshake timeout 8 s; các subscription QoS1,
backoff, TLS admission, topics và payload không đổi.

Ngân sách *admission* Wi-Fi/MQTT là 1 s ở Level 0; không yêu cầu đã kết nối mới
cho bước sau chạy. Khoảng cách tối thiểu giữa từng dịch vụ là 1 s ở Level 0,
3 s ở các recovery level. Các timeout/kết nối/reconnect thật vẫn chạy trong
owner task với thuật toán cũ. TLS guard hiện có tiếp tục tránh các transient
TLS operation chạy cùng lúc.

## LCD và Home

`boot_assets.h` là bitmap 1-bit từ đúng PNG người dùng gửi (`doc/boot-logo-source.png`),
logo đặt giữa chiều ngang (69×45 px). Bên dưới chỉ có một dòng trạng thái;
8 bitmap chữ tiếng Việt có dấu tránh phụ thuộc font Unicode lớn. Không thêm
version, progress bar hay dòng thông tin phụ vào splash.

Home được giải phóng bằng đồng hồ local, không đọc Wi-Fi/MQTT/Cloud:

- Level 0/1: sau 4,5 s task local khỏe, hiện “Hệ thống sẵn sàng” 0,5 s rồi thoát splash.
- Level 2/3: sau 3 s local khỏe, hiện trạng thái sẵn sàng 0,5 s rồi thoát splash trước mạng.
- Giữ trần splash 6 s hiện có tính từ `hmiBegin()`, để lỗi sensor/LCD không khóa giao diện.
- Các màn alarm, xác nhận phục hồi mẻ và turning hiện có vẫn có quyền xuất hiện khi splash kết thúc.
- Ở recovery, các bước mạng xảy ra nền sau Home nên không kéo splash trở lại để hiển thị chúng.

“Hệ thống sẵn sàng” nghĩa là giao diện/local có thể dùng; khác với chứng nhận
`BOOT_SUCCESS`. Cảm biến lỗi vẫn vào giao diện để xử lý và các interlock hiện có
vẫn khóa nhiệt; không được dùng dòng sẵn sàng để suy ra heater được phép chạy.

Có thể tái tạo bitmap bằng Pillow:

```text
python tools/generate_boot_assets.py --font /path/to/arial.ttf --preview boot-preview.png
```

## Boot Diagnostic

`esp_reset_reason()` được lấy ngay sau safe outputs. Live snapshot dùng
`RTC_DATA_ATTR`; hai backing slot dùng `RTC_NOINIT_ATTR`, magic/version,
sequence và checksum. Lý do cần backing noinit: ESP-IDF nêu rõ `RTC_DATA_ATTR`
giữ qua deep sleep còn `RTC_NOINIT_ATTR` giữ qua restart. Xem
[định nghĩa thuộc tính RTC của Espressif](https://github.com/espressif/esp-idf/blob/v5.5.1/components/esp_common/include/esp_attr.h).

Lưu reset reason hiện tại/trước, stage hiện tại/trước, consecutive failed boots,
planned restart reason hiện tại/trước, detail, recovery level và boot-completed
flag. Mỗi lần boot xóa planned marker của lần hiện tại sau khi chép sang thông
tin lần trước. Chỉ cập nhật RTC khi đổi stage, chuẩn bị restart, xác nhận success
hoặc clear failures; không ghi EEPROM/NVS cho diagnostic này.

Mọi `esp_restart()`/`ESP.restart()` trực tiếp đi qua `mayapRestart(reason, detail)`.
ArduinoOTA tự restart trong thư viện nên `onEnd()` ghi marker trước khi trả về. (Đã gỡ ArduinoOTA ngày 2026-10-08; restart do Internet OTA vẫn ghi marker qua `mayapRestart(RestartReason::InternetOta)`.)
Supervisor ghi thêm marker **trước** fallback wait, vì TWDT có thể reset trước
khi tới lệnh restart. Reset reason thực tế luôn là nguồn phân loại; marker còn
sót không biến panic/WDT/brownout thành restart OTA có chủ đích.

- OTA LAN, OTA Internet đã verify và rollback: không tăng failed-boots khi reset thực tế là `ESP_RST_SW`.
- Fatal init, lỗi WDT API, control heartbeat/deadline, HMI fatal, health restart, panic/WDT/brownout: tăng failed-boots nếu record RTC còn hợp lệ.
- Panic/WDT sau `BOOT_SUCCESS` vẫn tăng: tránh crash-loop runtime liên tục được coi là boot sạch.
- `ESP_RST_POWERON` hoặc record không hợp lệ: bắt đầu counters mới. Deep sleep đã hoàn tất boot không tăng.
- Magic intentional restart cũ của PowerManager chỉ giữ cho ba đường OTA/rollback như baseline; không áp cho restart do lỗi.

Log `[BOOT-DIAG]`, `[BOOT-STAGE]`, `[BOOT_SUCCESS]`, `[BOOT-RECOVERY]` đi qua
Serial gate hiện có (LOG/EXIT), không in credential.

## Adaptive Recovery

| Level | Khi nào | Trì hoãn mạng |
|---|---|---|
| 0 | 0 failed boots | Sau 3 s local task khỏe liên tục; service gap 1 s |
| 1 | 1 failed boot | Sau 8 s; service gap 3 s |
| 2 | 2 failed boots | Home trước; sau 10 s local task khỏe mới Wi-Fi → MQTT → Cloud → OTA |
| 3 | ≥3 failed boots | Local-only ban đầu; sau 45 s local task khỏe thử từng dịch vụ, gap 3 s |

Level 3 **không tắt** sensor, PID, heater safety, turning, alarm, HMI, RTC, EEPROM,
ATtiny hoặc Supervisor. Không đổi connectivity mode đã lưu; chỉ trì hoãn tạo
task mạng. Khi được admitted, các task dùng mode và reconnect/backoff hiện có.
Nếu operator chọn Offline, các dịch vụ vẫn được admitted nhưng không kết nối Internet.

Task heartbeat, cycle budget và system-trip latch quyết định local stability cho
admission. Sensor/LCD lỗi không cản operator dùng mạng để chẩn đoán sau khoảng
trì hoãn, nhưng **không** cho đánh dấu thành công hoặc xóa chuỗi lỗi.

`BOOT_SUCCESS` chỉ được ghi sau 25 s liên tục: control/HMI/Supervisor có heartbeat,
không slow cycle/system trip, sensor usable và LCD ready. Bất kỳ lần kiểm tra
không khỏe nào đặt lại đồng hồ liên tục. Không cần Internet, không đánh dấu ở
cuối `setup()`, và ở Level 3 có thể chứng nhận local trước khi bật mạng.

Sau 10 phút đáp ứng cùng điều kiện và đã tới Running, xóa failed boots và đưa
diagnostic level về 0. Không khởi động lại hay dựng lại các task đang chạy.

Supervisor chỉ giám sát task local đã admitted; sau admission giữ nguyên tất cả
ngưỡng heartbeat, slow-cycle, thao tác latch/suspend/safe outputs và fallback.
Network task công nhận OTA chưa admitted là không có socket cần quiesce, giúp
captive portal không timeout sai trong thời gian staged admission. Sau khi OTA
được admitted, handshake portal/OTA hiện có tiếp tục nguyên trạng.

## File thay đổi

- `MAYAP_INDUSTRIAL_v1_0_0.ino`: coordinator, task admission/owner init, heartbeat flags, restart reasons.
- `boot_policy.h`: stages, timing/recovery, reset classification, checksum, stability clock.
- `boot_diagnostic.h`: RTC slots, snapshot, boot/restart API.
- `boot_assets.h`, `hmi.h`: logo/status, early display preview, local Home release.
- `machine_control.h`: tách duy nhất RTC/store begin khỏi begin còn lại; thêm hai startup flags. `update()` và toàn bộ PID/safety/batch/recovery runtime giữ nguyên.
- `network_service.h`: giữ requested connectivity mode lúc network begin bị trì hoãn.
- `ota_update.h`, `ota_web_update.h`, `ota_rollback.h`: reasoned restart/marker.
- `tests/boot-policy.cpp`, `tests/staged-boot.test.cjs`: host policy và integration regression.
- Hai workflow build/reliability: chạy test policy có ASan/UBSan và nhận nhánh mới.
- `tools/generate_boot_assets.py`, PNG nguồn, README và tài liệu này.

## Validation và rủi ro còn lại

Kiểm tra CI bao gồm 35 Node tests, toàn bộ static checker, JavaScript syntax,
test PID/autotune và test policy C++, build ESP32 core 3.3.11 / CLI 1.5.1 /
U8g2 2.36.19 / PubSubClient 2.8.0 / ArduinoJson 7.4.3 và build ATtiny13A.
Kết quả cụ thể của lượt chạy nằm trong báo cáo bàn giao.

Các giới hạn cần test trên máy thật trước phát hành:

1. Chưa đo timing/power/current trên ESP32-S3 + LCD + ATtiny thật. Cần cold boot, OTA restart, watchdog injection, I2C lỗi, sensor lỗi, AP mất, broker mất và reset ở từng stage.
2. RTC RAM không bền qua mất nguồn; brownout sâu có thể mất record. Vì không ghi EEPROM/NVS liên tục nên không hứa escalation qua mọi lần mất điện thật. Safety journal/recovery mẻ cũ tiếp tục xử lý mất điện độc lập.
3. Stage là breadcrumb cấp ứng dụng; không nhận diện crash trước `setup()` hoặc lỗi bootloader/partition/flash. Hai slot giữ bản trước khi copy bị ngắt, nhưng stage cuối có thể cũ một bước.
4. Các hàm init local vẫn là I/O đồng bộ bounded từ baseline (NVS, I2C, UART, LCD); không biến mọi giao dịch thành coroutine. WDT bootstrap giữ nguyên timeout, nên cần đo worst-case thời gian EEPROM/NVS trên phần cứng lỗi.
5. Cloud alert/provisioning và OTA mạng được trì hoãn theo recovery level. Các sự kiện local/HMI/ATtiny vẫn chạy; không hứa gửi mọi alert ngắn chỉ tồn tại trong thời gian Cloud chưa được admitted.
6. Task OTA cùng owner với rollback nên yêu cầu rollback phát sinh trước admission có thể phải chờ đến stage OTA. Metadata/menu rollback local được cache từ stage ControlSafety.
7. Logo và dấu tiếng Việt ở LCD 128×64 cần kiểm tra tương phản/khả năng đọc trực tiếp. “Kiểm tra phần cứng” mô tả bước safe outputs/I2C, không phải chứng nhận self-test relay/contactor.
8. Không thay phần cứng safety độc lập, commissioning hay interlock bằng `BOOT_SUCCESS`.
