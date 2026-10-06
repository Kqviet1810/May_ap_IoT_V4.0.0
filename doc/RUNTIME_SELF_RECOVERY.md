# MAYAP 1.0.0 — Runtime Self-Recovery

Tiếp nối commit Adaptive Staged Boot `d5a0233c6ed39e517938f09336cc1d06425e6c19` trên nhánh `feat/adaptive-staged-boot`. Không thay Boot Manager, thứ tự startup, Recovery Level 0–3, thời điểm BOOT_SUCCESS, logo hoặc dòng trạng thái boot.

## Nguyên tắc và quyền sở hữu

Ladder chung: **retry tại chỗ → reinit subsystem → isolate có thời hạn → controlled restart nếu đủ điều kiện**. Không phải mọi lỗi đều được đi đến restart: mất cảm biến, thiếu thiết bị I2C, mất Wi-Fi/Internet hoặc máy chủ từ chối kết nối chỉ retry/isolate tại chỗ.

Supervisor gửi cờ yêu cầu, owner task tự đóng/reinit tài nguyên. Không xóa/tạo lại task đang chạy, không đóng socket từ task khác, không cưỡng bức mở khóa mutex/TLS gate. Stack, priority, core affinity và WDT của bản Adaptive Boot giữ nguyên. Task chưa được staged startup cho chạy không bị giám sát runtime.

## Ladder từng subsystem

| Subsystem | Retry | Reinit | Isolate / thử lại | ESP restart mới |
|---|---|---|---|---|
| I2C LCD/RTC/EEPROM | Giữ retry và timeout thiết bị | Supervisor giữ mutex, `Wire.end()`, nhả SDA/SCL, tối đa 9 xung SCL, STOP, `Wire.begin()` cùng pin/clock/timeout, probe 3 địa chỉ | Cooldown 30 s; thiết bị tự đọc/khởi tạo và xác minh lại theo logic cũ | Không, kể cả thiếu/short thiết bị |
| RS485/UART | Giữ nguyên Modbus parser, CRC, timeout, 2 attempts/cycle | Sau ≥6 cycle lỗi và cooldown ≥30 s: purge RX hữu hạn, reset parser, DE/RE receive, `serial.end()/begin()` cùng cấu hình | Sau ≥3 reinit liên tiếp chưa có frame tốt: nghỉ poll 30 s rồi thử lại | Không vì sensor/UART mất dữ liệu |
| Wi-Fi | Giữ reconnect/backoff | Sau ≥6 failure hoặc offline ≥5 phút: đợi owner I/O quiesce, disconnect không xóa credential, WIFI_OFF, chờ bằng state machine 500 ms, WIFI_STA, reconnect | Cooldown ≥120 s kể cả yêu cầu supervisor; sau ≥3 deep cycle chưa kết nối, nghỉ STA 120 s rồi thử tiếp | Chỉ nếu **owner networkTask không tiến triển**, theo điều kiện phía dưới |
| MQTT | Giữ backoff/connect và toàn bộ protocol/topics/transactions | Đóng TCP/TLS socket khi connect lỗi; khi supervisor yêu cầu, owner đóng socket, disconnect, phục hồi buffer nếu cần, reconnect theo backoff | Task pause I/O 30 s nếu đã đến bước isolate; giữ mailbox/outbox, replay và ACK | Chỉ nếu owner không tiến triển kéo dài |
| Cloud | Giữ queue/backoff | HTTP/TLS session được `http.end()/client.stop()` khi lỗi/kết thúc; owner recovery đặt lại trạng thái đăng ký và backoff | Pause I/O 30 s rồi đăng ký/retry; không xóa identity, PIN, command key hay outbox | Chỉ nếu owner không tiến triển kéo dài |
| OTA | Giữ deadline Internet OTA 120 s và mọi xác nhận/interlock/hash/chữ ký | Owner end/re-register ArduinoOTA; upload đang tiến triển được giữ. Session ArduinoOTA không có progress ≥60 s chỉ được abort bởi chính owner khi xử lý yêu cầu recovery | Pause service 30 s; giữ yêu cầu Internet OTA đã được nhận, metadata và màn hình báo lỗi | Chỉ nếu owner thật sự kẹt kéo dài; successful OTA vẫn restart theo cơ chế cũ |

### I2C

Kiểm tra mỗi 1 s, lấy mutex với timeout 0 để không đứng sau giao dịch storage. Nghi bus chung treo khi đường SDA/SCL bị giữ LOW lúc đang có mutex, hoặc ít nhất 2 trong 3 thiết bị có ≥3 lỗi liên tiếp trong 10 s gần nhất. Thiếu riêng 1 thiết bị không đủ để reset cả bus. NACK do EEPROM đang hoàn tất page-write là bình thường, không tính vào lỗi chung.

Xung và STOP dùng open drain, không drive HIGH đối đầu thiết bị. Nếu SCL vẫn LOW, kết thúc xung sớm. Không loop vô hạn. Khi đường vẫn lỗi, nhường mutex và thử lại sau cooldown; không cố đọc/ghi dữ liệu hay tự clear fault. LCD nhận recovery epoch và thực hiện reinit panel theo đường runtime hiện có. HMI đã bỏ đường tự `Wire.end()/begin()` cho shared bus; boot rendering giữ nguyên.

### RS485 và heater

Reinit xảy ra ở sensor owner sau một cycle đã thất bại hoàn toàn. Bộ đếm CRC/timeout/format, filter, freshness, event và các mẫu đã nhận không bị làm giả thành “healthy”. Sau UART reinit không có dữ liệu mới và sensor vẫn offline cho đến frame hợp lệ.

Toàn bộ `MachineController`, trong đó có `processSensor()`, heater/PID lockout, ngưỡng `SENSOR_RECOVERY_GOOD_SAMPLES=3`, plausibility/frozen detection và các fault latch, không đổi. Frame đầu tiên sau reconnect không tự cho heater vượt qua cổng 3 mẫu tốt. Recovery không clear lỗi cơ khí, emergency temperature, heater nghiêm trọng hay sensor không đáng tin cậy.

### Service heartbeat và restart

Heartbeat Network/MQTT/Cloud/OTA lần lượt cho phép 30/60/120/180 s không tiến triển. Cloud cập nhật heartbeat sau session HTTP hoàn tất; OTA cập nhật khi có dữ liệu flash được ghi hoặc callback progress, tránh hiểu upload đang tiến triển thành task chết. Owner vẫn beat khi offline/pause; thiếu Internet không phải thiếu heartbeat.

1. Lần đầu heartbeat quá hạn: yêu cầu owner reinit.
2. Sau thêm 60 s chưa có recovery ACK và heartbeat vẫn quá hạn: gửi isolate, pause I/O 30 s khi owner có thể nhận yêu cầu.
3. Sau thêm tổng cộng 300 s từ yêu cầu reinit đầu tiên, nếu heartbeat vẫn quá hạn, không có ACK mới: mới đề nghị controlled restart. Như vậy thời gian tối thiểu từ heartbeat cuối là khoảng 330/360/420/480 s tương ứng Network/MQTT/Cloud/OTA.
4. Chỉ thực hiện đường restart mới khi `mayapFirmwareMaintenanceReady()` cho biết máy đang rảnh/an toàn theo cổng hiện có. Latch system trip, suspend control, safe outputs, rồi `mayapRestart(HealthMonitor, "<SERVICE> runtime unresponsive")`. Adaptive Boot Manager đã có nhận reason và tự xử lý diagnostic/recovery level.

Máy đang chạy mẻ, chờ phục hồi mẻ, đảo/test/autotune hoặc còn output/fault làm maintenance chưa an toàn **không bị reboot chỉ vì service mạng kẹt**. Giữ local control và chờ điều kiện an toàn/owner phục hồi. Task thật sự deadlock không thể tự reinit khi chưa giành lại execution; isolate là cờ ngừng I/O mới, không cam kết cưỡng bức hủy một I/O đang kẹt. WDT và các trip control/HMI/health cũ vẫn có quyền hành động theo ngưỡng cũ.

### Wi-Fi ownership

Network công bố radio quiesce rồi sang chu kỳ sau mới kiểm tra busy flags. MQTT đóng socket bằng owner; Cloud/Internet OTA đợi giao dịch có timeout trả về; ArduinoOTA đang upload tiếp tục pump cho đến khi hoàn tất/lỗi, chưa ACK quiesce. Chỉ đổi radio sau khi MQTT/Cloud hết busy và OTA ACK quiesce. Captive portal giữ ưu tiên, thao tác đổi SSID không bị deep recovery tranh radio. WIFI_OFF chờ 500 ms không dùng `delay(500)`.

## Các file thay đổi

- Thêm `MAYAP_INDUSTRIAL_v1_0_0/runtime_recovery_policy.h`: policy pure C++, ngưỡng/cooldown và timer wraparound.
- Thêm `MAYAP_INDUSTRIAL_v1_0_0/service_recovery.h`: heartbeat/ACK/request/isolate và handshake radio.
- Thêm `MAYAP_INDUSTRIAL_v1_0_0/i2c_supervisor.h`: supervisor shared bus.
- Sửa `.ino`: nối các owner recovery, Supervisor và I2C supervisor runtime; staged coordinator/setup giữ nguyên.
- Sửa `config.h`: chỉ 2 khai báo API I2C, không đổi constant/schema.
- Sửa `machine_control.h`: báo kết quả RTC/EEPROM và thêm recovery vào driver SHT485; không đổi `MachineController`/fault/output logic.
- Sửa `hmi.h`: report probe, nhận bus epoch, bỏ reset shared bus riêng.
- Sửa `network_service.h`, `transaction_bridge.h`, `cloud_alert_link.h`, `ota_update.h`, `ota_web_update.h`: recovery owner và heartbeat progress.
- Thêm `tests/runtime-recovery.cpp`, `runtime-buses.cpp`, `runtime-network.cpp`, `runtime-ota.cpp`, `runtime-recovery.test.cjs`, `runtime-preservation.json`; `tools/test_runtime_buses.py` trích **implementation thực** để fault injection với HAL giả.
- Sửa cả workflow build/reliability để chạy host tests với AddressSanitizer/UndefinedBehaviorSanitizer; README thêm liên kết tài liệu.

## Kiểm thử và giới hạn

Chạy toàn bộ Node regression, static checks release/ATtiny/reliability/EEPROM/secrets, JS syntax, host PID/boot/runtime và compile ESP32 + ATtiny. Manifest đối chiếu 31 file/section với commit Adaptive Boot, bao gồm nguyên `MachineController`, FaultManager, OutputArbiter, Boot Manager/coordinator/HMI boot, schema/config, protocol, ATtiny và Internet OTA validation/transaction. Đây là guard hồi quy của phạm vi yêu cầu; không thay thế thử nghiệm hardware.

HAL tests tiêm bus LOW, lock bận, lỗi nhiều/riêng thiết bị, clock tối đa 9 lần, cooldown, frame mất/sai CRC, UART isolation/reconnect, stale sample, service admission/ACK/escalation, radio handshake, portal, upload progress/abort và millis wraparound. Không mô phỏng đầy đủ scheduling 2 core hoặc điện học bus. Cần bench với dây/bus và cảm biến thật, disconnect/reconnect Wi-Fi/broker, upload OTA bị ngắt và soak chạy mẻ dài. Không thay đổi threshold an toàn để làm test qua.

`HardwareSerial.end()/begin()` cấp phát lại tài nguyên driver, `Wire.end()/begin()` và radio mode có latency phụ thuộc core/driver. Kiểm thử latency thực trên ESP32 và watchdog vẫn cần thiết. Một thiết bị short bus/nguồn yếu hoặc fault vật lý cần sửa tại máy; software không clear chúng. I2C supervisor không cướp mutex bị owner giữ; control/HMI safety và WDT vẫn xử lý owner treo theo cơ chế cũ.
