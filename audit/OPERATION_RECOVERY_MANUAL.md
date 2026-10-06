# MAYAP INDUSTRIAL v3.4.0 — Phân tích cơ chế tự phục hồi & Tài liệu vận hành

> Tài liệu này được xây dựng bằng cách đọc trực tiếp source code firmware hiện tại
> (`MAYAP_INDUSTRIAL_v3_4_0.ino`, `machine_control.h`, `hmi.h`, `network_service.h`,
> `transaction_bridge.h`, `cloud_alert_link.h`, `config.h`). Mọi khẳng định "ĐÃ CÓ" đều
> kèm theo File/Function/dòng code cụ thể. Không có nội dung nào trong tài liệu
> được suy đoán từ tên hàm hoặc comment — hành vi thực tế của code là căn cứ duy nhất.
> Không có môi trường chạy thử phần cứng thật trong quá trình viết tài liệu này — mọi
> kết luận là STATIC (đọc code), không phải RUNTIME (đã thực thi/đo đạc thật).

---

## 1. Tóm tắt firmware hiện tại

Firmware chạy trên ESP32-S3 (FreeRTOS), chia làm 4 task tĩnh (static task, không cấp
phát heap khi chạy):

| Task | File/hàm | Vai trò | Có gắn Watchdog (Task WDT)? |
|---|---|---|---|
| `mayap_ctrl` (core 1, ưu tiên 5) | `controlTask()` — `MAYAP_INDUSTRIAL_v3_4_0.ino:86` | Vòng lặp điều khiển máy (`Machine.update()`) — nhiệt độ, đảo trứng, an toàn, fault | Có (`subscribeCurrentTaskToWdt`, dòng 88) |
| `mayap_supervisor` (core 1, ưu tiên 6) | `supervisorTask()` — dòng 160 | Giám sát nhịp tim (heartbeat) của control/HMI, tự reset chip nếu control "treo" | Có (dòng 162) |
| `mayap_hmi` (core 0, ưu tiên 2) | `hmiTask()` — dòng 134 | Vẽ LCD, đọc nút xoay, buzzer | Không gắn WDT (không thấy `subscribeCurrentTaskToWdt` trong hàm này) |
| `mayap_network` (core 0, ưu tiên 1) | `networkTask()` — dòng 145 | Wi-Fi, MQTT (web realtime), Cloud Push | Không gắn WDT |

Thiết bị dùng cảm biến nhiệt/ẩm SHT qua Modbus RTU/RS485 (`SHT485Industrial`,
`machine_control.h:2160`), RTC DS3231 qua I2C (`RtcDs3231`, dòng 818), EEPROM ngoài
AT24C32 qua I2C (`ExternalEeprom24xx`, dòng 1477), LCD đơn sắc U8G2 (ST7567) qua I2C
(`hmi.h`), động cơ đảo trứng 2 chiều với 2 công tắc hành trình, relay nhiệt 2 tầng
(contactor tổng + SSR PID), quạt tuần hoàn/thông gió, đèn, còi báo.

Chế độ AUTO/MANUAL là **một công tắc vật lý duy nhất** đấu vào GPIO
(`InputChannel::Auto`, `machine_control.h:1972`), có debounce phần cứng — không phải
nút bấm chọn trên màn hình.

---

## 2. Bảng tổng hợp cơ chế tự phục hồi

| Thiết bị/chức năng | Có phát hiện lỗi? | Có tự phục hồi? | Cách tự phục hồi | Số lần retry | Timeout | Nếu phục hồi thành công | Nếu không phục hồi | Mã lỗi |
|---|---|---|---|---|---|---|---|---|
| LCD/I2C bus | **ĐÃ CÓ** | **ĐÃ CÓ** | Probe địa chỉ I2C định kỳ; nếu mất, bit-bang xung SCL để giải phóng bus (recover I2C bus), `Wire.begin()` lại, `lcd.begin()` lại | Không giới hạn số lần (retry vô hạn mỗi `LCD_RETRY_INTERVAL_MS`) | Health-check mỗi 5000 ms; retry mỗi 3000 ms khi mất | Hiện toast "LCD DA TU PHUC HOI" trên màn hình, vẽ tiếp bình thường | **CHƯA CÓ** — không có mã lỗi (E-code), không có cảnh báo nào khác ngoài log Serial (chỉ khi bật `MAYAP_DIAGNOSTIC_SERIAL`) | Không có |
| EEPROM cấu hình/mẻ ấp | **ĐÃ CÓ** | **ĐÃ CÓ** | Retry I/O từng giao dịch; probe định kỳ; reconnect + đọc lại 2 lần để xác nhận ổn định trước khi đưa dữ liệu vào RAM | I/O: `EEPROM_IO_RETRIES` lần/giao dịch; xác minh khi phục hồi: `EEPROM_RECOVERY_VERIFY_COUNT` (2) lần đọc phải khớp nhau | Health-check 5000 ms; reconnect mỗi 3000 ms | Xoá cờ lỗi, ghi log "CONNECTION RESTORED", nếu đang chạy mẻ thì ghi lại checkpoint ngay | Sau 3 lần lỗi liên tiếp (`EEPROM_FAILURE_LATCH_COUNT`) mà không đang chạy mẻ → khoá cứng | 301 (MAT EEPROM, sau khi suy giảm), 302 (EEPROM SUY GIAM, cảnh báo sớm) |
| RTC (đồng hồ thời gian thực) | **ĐÃ CÓ** | **CHƯA ĐỦ / CÓ MỘT PHẦN** | Tự "sửa" giờ bằng đồng hồ bóng (shadow clock, ước lượng từ `millis()` khi ESP32 chưa từng mất điện) khi phát hiện OSF hoặc dữ liệu không hợp lệ, rồi ghi lại vào RTC + xoá cờ OSF | Tối đa `RTC_AUTO_REPAIR_MAX_ATTEMPTS` (3) lần, cách nhau `RTC_AUTO_REPAIR_RETRY_MS` (3000 ms), chỉ khi có ≥ `RTC_AUTO_REPAIR_CONFIRM_READS` (2) lần đọc lỗi liên tiếp xác nhận | Đọc mỗi 1000 ms; coi là "kẹt" nếu giá trị không đổi > 4000 ms | Ghi log "[RTC] AUTO REPAIR OK", xoá cờ lỗi | Nếu ESP32 cũng vừa mất điện (không có "đồng hồ bóng" đáng tin) → không tự sửa được, giữ nguyên cảnh báo | 306 (LOI RTC, chỉ là Warning, không khoá gì) |
| **Pin RTC sắp hết** (câu hỏi riêng của người dùng) | **CHƯA CÓ** | **CHƯA CÓ** | — | — | — | — | Không tồn tại: DS3231 chỉ có thể báo "OSF" (Oscillator Stopped Flag — đồng hồ ĐÃ dừng, nghĩa là pin ĐÃ hết hoặc đã mất hoàn toàn cả 2 nguồn), không có cách đo mức pin còn lại để **cảnh báo trước** | Không có mã lỗi riêng cho "pin yếu"; chỉ có 306 khi RTC đã sai/mất hẳn |
| Sensor nhiệt/ẩm (SHT qua RS485) | **ĐÃ CÓ** | **ĐÃ CÓ** | Tự retry từng chu kỳ đọc (Modbus), tự đánh dấu "mất kết nối" sau nhiều chu kỳ lỗi liên tiếp, tự nhận lại ngay khi có khung dữ liệu hợp lệ tiếp theo | `ATTEMPTS_PER_CYCLE` = 2 lần/chu kỳ đọc; mất kết nối sau `OFFLINE_AFTER_FAILED_CYCLES` = 2 chu kỳ lỗi liên tiếp | Phản hồi Modbus: 200 ms; dữ liệu cũ (stale) sau 6500 ms; chu kỳ đọc 2000 ms | Sự kiện `SensorRestored`, PID reset để tránh giật, tiếp tục điều khiển | Nếu vẫn mất/timeout/ngoài dải giá trị hợp lý → giữ trạng thái an toàn (xem PHẦN 4/9) | 101 (MAT CAM BIEN), 102 (CAM BIEN SAI), 103 (CAM BIEN BAT THUONG) |
| Giá trị cảm biến "đứng yên" (stuck/frozen value) | **CHƯA CÓ** | **CHƯA CÓ** | — | — | — | — | Không có kiểm tra "giá trị không đổi trong X phút = cảm biến hỏng". Firmware chỉ có kiểm tra khác: từ chối một cú **tụt nhiệt đột ngột** (> 1.5°C giữa 2 lần đọc) cho tới khi được xác nhận lại 3 lần liên tiếp (`sensorSuspect_`, `machine_control.h:3464`) — đây là kiểm tra "hợp lý hoá tức thời", KHÔNG phải kiểm tra "đứng yên lâu dài" | 103 (chỉ áp dụng cho trường hợp tụt đột ngột, không áp dụng cho giá trị đứng yên) |
| Task điều khiển bị "treo" (deadlock/vòng lặp vô hạn) | **ĐÃ CÓ** | **CHƯA ĐỦ / CÓ MỘT PHẦN** | `supervisorTask` phát hiện qua heartbeat + thời gian 1 chu kỳ quá dài, ép output về an toàn (`mayapSafeOutputsEarly()`), rồi **khởi động lại toàn bộ chip** (`esp_restart()`) | Không "thử lại tại chỗ" — hành động phục hồi DUY NHẤT là reset cứng | Heartbeat timeout: `CONTROL_HEARTBEAT_TIMEOUT_MS`; số chu kỳ chậm liên tiếp cho phép: `CONTROL_CYCLE_TRIP_COUNT` | Sau khi reset, boot lại từ đầu, tự phục hồi mẻ ấp từ EEPROM nếu hợp lệ (PHẦN 3 mục Batch) | Không áp dụng — hành động luôn là reset, không có "thất bại vĩnh viễn" ở tầng này | Không có mã lỗi riêng cho sự kiện trip — chỉ có log Serial "[SUPERVISOR] TRIP..."; sau khi reset thì `AbnormalReset` (303) được đánh dấu ở lần boot kế tiếp nếu lý do reset là panic/WDT |
| Wi-Fi (kết nối trạm STA) | **ĐÃ CÓ** | **ĐÃ CÓ** | `BackoffTimer` lùi-và-đợi độc lập theo từng kênh (Wi-Fi/MQTT/Cloud Push riêng biệt) | Không giới hạn số lần — lùi dần rồi giữ ở mức trần, **không bao giờ "bỏ cuộc"** | Các bước lùi: 1,2,4,8,16,30,60 giây + jitter ngẫu nhiên ≤ 500 ms | Kết nối lại bình thường, `staBackoff.onSuccess()` đưa bước lùi về 0 | Không có trạng thái "thất bại vĩnh viễn" — máy vẫn tiếp tục vận hành cục bộ (an toàn/nhiệt/đảo không phụ thuộc mạng) | Không có mã lỗi cứng cho mất Wi-Fi vĩnh viễn; HMI/web chỉ hiển thị trạng thái online/offline |
| MQTT (web realtime) / Cloud Push | **ĐÃ CÓ** | **ĐÃ CÓ** | Cùng cơ chế `BackoffTimer` độc lập (`mqttBackoff`, `cloudBackoff`) | Như trên | Như trên | Kết nối lại, tiếp tục đồng bộ | Như trên | Không có |
| Động cơ đảo trứng (motor) | **ĐÃ CÓ** | **CHƯA ĐỦ / CÓ MỘT PHẦN** | Phát hiện timeout hành trình / công tắc hành trình kẹt / lệnh xung đột → khoá đảo, cần xác nhận (ACK) người vận hành; sau ≥ 3 lần lỗi đảo liên tiếp không có lần thành công xen giữa → khoá "kiểm tra cơ khí", CHỈ gỡ được qua Test Mode xác nhận cả 2 công tắc hành trình | 3 lần lỗi liên tiếp (`TURN_FAULT_STREAK_LIMIT`) mới nâng lên mức khoá cơ khí | Timeout đảo theo cấu hình (`turnMaxRunSec`) | Không tự phục hồi — luôn cần thao tác người vận hành (ACK thường hoặc vào Test Mode) | Khoá đảo, không cho đảo tiếp | 201, 202, 203, 204 (đều **latching** — cần ACK), 205 (khoá đặc biệt — chỉ gỡ qua Test Mode) |
| Nút bấm/công tắc vật lý (Auto, Heater, Fan, Light, Turn L/R, Limit L/R) | **ĐÃ CÓ** | Không áp dụng (không phải "lỗi" mà là input) | Debounce phần cứng: `INPUT_DEBOUNCE_MS` (thường), `LIMIT_DEBOUNCE_MS` (công tắc hành trình, thường ngắn hơn để phản ứng nhanh) | — | — | — | — | — |

---

## 3. Chi tiết LCD recovery

**File:** `hmi.h`. **Biến trạng thái:** `lcdReady`, `lastLcdRetryAt`, `lastLcdHealthCheckAt`, `lastLcdFaultLogAt` (dòng 496, 558-560).

| Bước | Function | Dòng | Logic |
|---|---|---|---|
| Khởi tạo | `beginLcd()` | 2886 | Khoá I2C mutex → nếu tự sở hữu bus thì bit-bang giải phóng bus (`recoverI2cBusUnlocked()`, dòng 2861) → `Wire.begin()` → probe địa chỉ LCD (`probeLcdUnlocked()`) → nếu probe OK mới `lcd.begin()` thật sự |
| Giải phóng bus I2C bị kẹt | `recoverI2cBusUnlocked()` | 2861 | Nếu SDA đang bị kéo LOW (thiết bị treo giữa chừng một giao dịch), tạo tối đa 9 xung clock trên SCL để buộc thiết bị nhả SDA — kỹ thuật I2C bus recovery chuẩn |
| Vòng dịch vụ định kỳ | `serviceLcd(now)` | 2905 | Nếu `!lcdReady`: mỗi `LCD_RETRY_INTERVAL_MS` (3000 ms) thử `beginLcd()` lại. Nếu thành công → hiện toast "LCD DA TU PHUC HOI". Nếu `lcdReady`: mỗi `LCD_HEALTH_CHECK_MS` (5000 ms) probe lại để phát hiện mất kết nối |
| Log lỗi (chỉ log, không alarm) | dòng 2917-2921 | Mỗi `LCD_FAULT_LOG_INTERVAL_MS` (30000 ms) in ra Serial "LCD 0x3F van mat" nếu bật `MAYAP_DIAGNOSTIC_SERIAL` |

**Trả lời trực tiếp các câu hỏi:**
- Phát hiện mất kết nối: **ĐÃ CÓ** (probe I2C định kỳ + tại lúc vẽ).
- Timeout/kiểm tra phản hồi: **ĐÃ CÓ** (`Wire.setTimeOut(I2C_TIMEOUT_MS)` = 25 ms, dòng 2892).
- Tự khởi tạo lại: **ĐÃ CÓ** (`beginLcd()` gọi lại toàn bộ).
- Tự retry: **ĐÃ CÓ**, không giới hạn số lần, chu kỳ 3 giây.
- Reset bus để phục hồi: **ĐÃ CÓ** (`recoverI2cBusUnlocked()`).
- Nếu phục hồi thì tiếp tục chạy: **ĐÃ CÓ** (`dirty = true`, vẽ lại toàn bộ màn hình).
- Nếu không phục hồi được thì báo mã lỗi: **CHƯA CÓ** — không có `FaultCode` nào cho LCD trong `machine_control.h` (đã grep toàn bộ file, không có kết quả). Việc mất LCD chỉ được ghi vào log Serial nội bộ, không xuất hiện trên web/HMI/Cloud Push dưới bất kỳ hình thức nào khác (vì chính LCD là kênh hiển thị bị mất, nhưng web/app cũng không được báo).

---

## 4. Chi tiết EEPROM recovery

**File:** `machine_control.h`. **Class:** `ExternalEeprom24xx` (dòng 1477 — tầng I/O vật lý), `PersistentStore` (dòng 1611 — tầng bản ghi có CRC/schema).

### Tầng I/O vật lý (`ExternalEeprom24xx`)
- `begin()`, `readBytes()`, `writeBytes()` đều tự retry `EEPROM_IO_RETRIES` lần, nghỉ `EEPROM_RETRY_GAP_MS` giữa các lần (dòng 1479-1505).
- Ghi xong luôn `waitWriteCompleteLocked()` — poll ACK cho tới khi EEPROM báo sẵn sàng hoặc hết `EEPROM_WRITE_TIMEOUT_MS` (dòng 1586).

### Tầng bản ghi có xác thực (`PersistentStore`)
- Mỗi bản ghi (cấu hình, mẻ ấp) có `magic + schema + size + sequence + payload + crc` (CRC32 tính bằng `mcCrc32`).
- **Double-buffer A/B**: mỗi lần lưu ghi vào slot đối diện với slot đang dùng, đọc lại ngay (`readRecord`) để xác minh CRC + `sequence` + nội dung khớp — nếu xác minh thất bại thì **coi như lưu thất bại**, KHÔNG cập nhật cache (dòng 1678-1692, 1721-1734). Đây là cơ chế chống hỏng dữ liệu khi mất điện giữa chừng lúc ghi (torn write).
- Khi đọc, nếu cả 2 slot không hợp lệ ở schema hiện tại (V1), tự động rơi xuống các schema cũ hơn (Legacy V6→V5→V4→V3) để **không mất cấu hình người dùng đã chỉnh** khi nâng cấp firmware (dòng 1794-1899) — đây là cơ chế "phục hồi dữ liệu" theo đúng nghĩa (không phải chỉ là default trắng).
- Nếu không slot nào hợp lệ ở bất kỳ schema nào → coi là EEPROM trống/hỏng, firmware **tự ghi giá trị mặc định vào EEPROM** (`begin()`, dòng 3085-3091: `config_ = MachineConfig{}; ... configLoaded_ = store_.saveConfig(...)`).

### Vòng giám sát/tự kết nối lại (chạy trong `update()`)
- **Health-check** (dòng 3259-3268): mỗi `EEPROM_HEALTH_CHECK_MS` (5000 ms), nếu đang "ready" thì probe nhẹ; probe lỗi → `store_.markOffline()` + `latchStorageFault("HEALTH CHECK")`.
- **Reconnect** (dòng 3270-3356): nếu đang lỗi/suy giảm, mỗi `EEPROM_RECONNECT_PERIOD_MS` (3000 ms) thử `store_.reconnect()`.
  - Nếu đang chạy mẻ (`batchRunning_ || resumePending_`): **không load lại cấu hình cũ từ EEPROM** (tránh phá cấu hình RAM đang dùng để điều khiển) — chỉ ghi lại cấu hình RAM hiện tại vào EEPROM để khôi phục khả năng checkpoint.
  - Nếu máy đang dừng: đọc lại **2 lần** (`EEPROM_RECOVERY_VERIFY_COUNT`), phải khớp nhau tuyệt đối mới tin dùng (chống trường hợp module vừa cắm lại chưa ổn định điện áp).
- **Cơ chế 2 tầng lỗi** (`latchStorageFault()`, dòng 5146): 2 lần lỗi đầu chỉ là "suy giảm" (`StorageDegraded`, không khoá gì nếu không đang chạy mẻ — nếu đang chạy mẻ thì tiếp tục điều khiển hoàn toàn từ RAM); đến lần thứ 3 liên tiếp (`EEPROM_FAILURE_LATCH_COUNT`) mà máy KHÔNG đang chạy mẻ mới nâng lên "mất hẳn" (`StorageUnavailable`, khoá nhiệt).

**Trả lời trực tiếp:**
- Kiểm tra EEPROM phản hồi: **ĐÃ CÓ** (probe địa chỉ I2C).
- Dữ liệu hợp lệ (CRC/checksum): **ĐÃ CÓ** (CRC32 + magic + schema + size).
- Retry đọc: **ĐÃ CÓ**.
- Tự khởi tạo lại: **ĐÃ CÓ** (`reconnect()`).
- Tự ghi giá trị mặc định: **ĐÃ CÓ**, nhưng chỉ khi EEPROM hoàn toàn trống/hỏng (không phải khi chỉ mất kết nối tạm thời).
- Phục hồi dữ liệu (không phải chỉ default): **ĐÃ CÓ** (đường nâng cấp schema cũ → mới giữ nguyên giá trị người dùng).
- Không thể phục hồi thì báo lỗi nào: `StorageUnavailable` (301, Stop, khoá SSR + nhả contactor nhiệt tổng) hoặc `StorageDegraded` (302, Warning, không khoá gì nếu đang chạy mẻ).

---

## 5. Chi tiết RTC recovery

**File:** `machine_control.h`, class `RtcDs3231` (dòng 818).

- **Kiểm tra hoạt động:** đọc thanh ghi thời gian + thanh ghi trạng thái (chứa OSF) mỗi `RTC_READ_PERIOD_MS` (1000 ms). Đọc lỗi 3 lần liên tiếp (`failedReads_ >= 3`) → coi là "offline" (dòng 837-841).
- **Kiểm tra OSF (Oscillator Stopped Flag):** đọc từ thanh ghi 0x0F, bit 0x80 (`readOsf()`, dòng 1099-1107). OSF=1 nghĩa là dao động thạch anh của RTC đã từng dừng hoàn toàn — xảy ra khi **cả nguồn chính lẫn pin backup đều mất** tại một thời điểm nào đó. Đây là dấu hiệu **gián tiếp và chỉ báo sau khi đã xảy ra** cho "hết pin/pin đã tháo", KHÔNG phải phép đo mức điện áp pin còn lại.
- **Kiểm tra thời gian hợp lệ:** năm phải trong khoảng `RTC_VALID_YEAR_MIN..MAX` (2024-2099); ngoài ra còn kiểm tra "đồng hồ có đang chạy tiến" — nếu giá trị epoch đọc được không thay đổi trong hơn `RTC_STUCK_TIMEOUT_MS` (4000 ms) thì coi là không hợp lệ (`clockAdvancing`, dòng 868-870) — đây chính là kiểm tra "kẹt giá trị" (stuck) áp dụng riêng cho RTC (không có tương đương cho sensor nhiệt/ẩm).
- **Tự sửa (auto-repair, `tryAutoRepair()`, dòng 1015-1030):** khi phát hiện OSF hoặc dữ liệu không hợp lệ liên tiếp ≥ `RTC_AUTO_REPAIR_CONFIRM_READS` (2) lần, và còn dưới `RTC_AUTO_REPAIR_MAX_ATTEMPTS` (3) lần thử: tính giờ ước lượng từ "đồng hồ bóng" (`estimatedShadowEpoch()` — epoch RTC hợp lệ gần nhất + số giây trôi qua theo `millis()` của chính ESP32, chỉ tin nếu khoảng cách ≤ `RTC_AUTO_REPAIR_MAX_GAP_SEC` = 24 giờ) rồi **ghi ngược lại vào RTC** và xoá cờ OSF.
  - Đây LÀ tự phục hồi thật (không chỉ retry đọc) — nhưng **có điều kiện tiên quyết**: chỉ hoạt động nếu bản thân ESP32 chưa từng mất điện kể từ lần cuối RTC còn đúng (đồng hồ bóng sống trong RAM, mất khi ESP32 reset). Nếu cả ESP32 và RTC cùng mất điện → không có gì để tự sửa, RTC giữ nguyên trạng thái lỗi.
- **Không có** cơ chế đo/báo trước "pin RTC sắp yếu". Không có thanh ghi/ADC nào được đọc cho mục đích này trong code.

**Trả lời câu hỏi trước đó của người dùng ("đã có tính năng xác nhận RTC sắp hết pin để báo chưa"):**
> **CHƯA CÓ.** Firmware chỉ phát hiện được khi RTC **đã** mất dữ liệu/dừng dao động (OSF=1, tức là hỏng rồi), báo lỗi 306 "LOI RTC" (mức Warning, không khoá gì). Không có cơ chế dự đoán/cảnh báo sớm "pin sắp hết" trước khi sự cố xảy ra, vì phần cứng DS3231 không cung cấp số đo điện áp pin cho firmware đọc.

---

## 6. Chi tiết Sensor recovery

**Cảm biến đang dùng:** 1 cảm biến nhiệt độ + độ ẩm (SHT) qua Modbus RTU trên RS485
(`SHT485Industrial`, `machine_control.h:2160`), giao tiếp bán song công (DE/RE pin
điều khiển hướng truyền).

| Kiểm tra | Trạng thái | Bằng chứng |
|---|---|---|
| Mất kết nối | **ĐÃ CÓ** | `online_ = false` sau `OFFLINE_AFTER_FAILED_CYCLES` (2) chu kỳ lỗi liên tiếp (`failAttempt()`, dòng 2417-2430), hoặc khi dữ liệu quá cũ (`updateFreshness()`, dòng 2431-2436) |
| Giá trị vượt giới hạn vật lý | **ĐÃ CÓ** | `decodeFrame()` (dòng 2361-2384) loại khung nếu `temp`/`hum` ngoài `TEMP_MIN/MAX_X10` (-40..60 °C), `HUM_MIN/MAX_X10` (0..100 %) → tính là `rangeErrors_`, không cập nhật giá trị |
| CRC sai | **ĐÃ CÓ** | `crc16(response_,7) != received` → `crcErrors_`, loại khung |
| Timeout phản hồi | **ĐÃ CÓ** | `RESPONSE_TIMEOUT_MS` = 200 ms (`WaitResponse` state, dòng 2217-2219) |
| Retry đọc | **ĐÃ CÓ** | `ATTEMPTS_PER_CYCLE` = 2 lần/chu kỳ, cách nhau `RETRY_GAP_MS` = 80 ms |
| Giá trị "đứng yên" (stuck/frozen) | **CHƯA CÓ** | Không có kiểm tra so sánh giá trị hiện tại với N lần đọc trước để phát hiện "cảm biến không đổi dù môi trường phải đổi". Bộ lọc median+IIR (dòng 2385-2408) chỉ để làm mượt nhiễu, không phát hiện đứng yên |
| Kiểm tra hợp lý tức thời (plausibility) | **ĐÃ CÓ** — nhưng khác "stuck" | Từ chối một mẫu nếu nhiệt độ tụt đột ngột > `SENSOR_MAX_DOWN_STEP_C` (1.5 °C) so với giá trị chấp nhận gần nhất, cho tới khi có `SENSOR_PLAUSIBILITY_CONFIRM_SAMPLES` (3) mẫu liên tiếp xác nhận giá trị mới (`processSensor()`, dòng 3460-3491) |
| Tự khởi tạo lại | Không cần — giao tiếp là polling liên tục không trạng thái "đã init 1 lần rồi thôi", mỗi chu kỳ đọc là độc lập |
| Tự nhận lại khi phục hồi | **ĐÃ CÓ** | `completeCycleSuccess()` (dòng 2409-2416): có khung hợp lệ đầu tiên sau khi mất → phát sự kiện `Restored`; điều khiển (`sensorUsable_`) chỉ bật lại sau `SENSOR_RECOVERY_GOOD_SAMPLES` (3) mẫu tốt liên tiếp (dòng 3514-3516), tránh bật nhiệt lại ngay khi dữ liệu vừa "chớp nháy" phục hồi |
| Trạng thái an toàn khi mất cảm biến | **ĐÃ CÓ** | Theo bảng `faultDescriptor`: `SensorLost/Invalid/Suspect` đều có `inhibitSsr=true, dropHeatMaster=true, forceCirculationFan=true` — cắt nhiệt (cả 2 tầng: SSR và contactor tổng), ép bật quạt tuần hoàn, **không** khoá đảo trứng (trứng vẫn được đảo bình thường dù mất cảm biến) |

**Mã lỗi:** 101 `MAT CAM BIEN` (mất kết nối/transport), 102 `CAM BIEN SAI` (dữ liệu ngoài dải), 103 `CAM BIEN BAT THUONG` (đang trong giai đoạn nghi ngờ do bước nhảy đột ngột — `sensorSuspect_`).

Có ân hạn khởi động (`SENSOR_STARTUP_GRACE_MS` = 20000 ms, dòng 3078): trong 20 giây đầu sau khi bật máy, nếu cảm biến chưa sẵn sàng thì **chưa báo lỗi 101** (dòng 4198-4200, biến `sensorGrace`) — tránh báo giả trong lúc cảm biến/bus vừa khởi động.

---

## 7. Các cơ chế recovery khác (đã xác nhận trong code)

| Chức năng | File/Function | Cơ chế |
|---|---|---|
| Watchdog phần cứng (ESP-IDF Task WDT) | `.ino:232-238` | Timeout 5000 ms (`CONTROL_WDT_TIMEOUT_MS`), `trigger_panic = true` — nếu task không "cho ăn" (`esp_task_wdt_reset()`) đúng hạn, chip tự panic/reset |
| Giám sát chéo giữa các task (Supervisor) | `supervisorTask()`, `.ino:160-211` | So `controlHeartbeatMs`/`hmiHeartbeatMs` với thời gian hiện tại; nếu control "treo" (mất nhịp tim hoặc 1 chu kỳ quá dài lặp lại `CONTROL_CYCLE_TRIP_COUNT` lần) → ép an toàn output → chờ `SUPERVISOR_RESTART_FALLBACK_MS` (7000 ms) → `esp_restart()` |
| Phục hồi mẻ ấp sau mất điện/reset bất thường | `Machine.begin()`, `machine_control.h:3104-3149` | Đọc bản ghi mẻ từ EEPROM (`wasRunning=1`) → `resumePending_ = true`; nếu mất điện thật và `autoResumeOnPowerLoss=false` → yêu cầu xác nhận CO/HUY trên HMI/web trước khi tiếp tục; nếu bật `autoResumeOnPowerLoss` hoặc reset do WDT/panic (không phải mất điện) → tự áp lại không cần hỏi |
| Chặn tự phục hồi khi có lệnh DỪNG/HUỶ đang chờ | dòng 3106-3120 | Nếu có "tombstone" trong NVS nội bộ đánh dấu người dùng vừa bấm DỪNG/HUỶ mà EEPROM chưa kịp xác nhận, firmware **cố tình không** tự áp lại mẻ dù bản ghi cũ còn đó — tránh mẻ đã dừng bị "sống lại" ngoài ý muốn |
| Interlock đầu ra 2 chiều đảo trứng | `OutputArbiter` (đã xác nhận ở phiên trước) | Không bao giờ cho 2 chiều đảo bật đồng thời; có dead-time bắt buộc giữa 2 lần đổi chiều |
| Wi-Fi / MQTT / Cloud Push | `network_service.h`, `transaction_bridge.h`, `cloud_alert_link.h` | Mỗi kênh có `BackoffTimer` độc lập (1-2-4-8-16-30-60 giây + jitter), không giới hạn số lần thử, không có trạng thái "bỏ cuộc vĩnh viễn" |
| Cơ chế ACK (xác nhận) cho lỗi latching | `FaultManager::set()`/`acknowledge()`, dòng 497-543 | Lỗi có `latching=true`: khi điều kiện hết (`condition=false`) **KHÔNG tự xoá** — chỉ xoá khi người vận hành bấm ACK (`acknowledge()`) VÀ điều kiện đã hết. Lỗi `latching=false`: tự xoá ngay khi điều kiện hết, không cần ACK |

---

## 8. Danh sách toàn bộ Error Code

Cột "Loại phục hồi trước khi báo" phân biệt rõ 2 hành vi theo yêu cầu: **"tự động dò/retry trước rồi mới báo"** vs **"phát hiện là báo ngay, không có bước tự thử nào ở giữa"**.

| Mã | Tên hiển thị (LCD) | Mức độ | Latching (cần ACK)? | Nguyên nhân | Loại phục hồi trước khi báo | Interlock áp dụng |
|---|---|---|---|---|---|---|
| 101 | MAT CAM BIEN | Stop | Không | Mất giao tiếp Modbus với cảm biến SHT, hoặc dữ liệu quá cũ | **Có retry trước**: 2 lần/chu kỳ, 2 chu kỳ lỗi liên tiếp mới báo | Cắt SSR + contactor nhiệt, ép quạt tuần hoàn |
| 102 | CAM BIEN SAI | Stop | Không | Khung Modbus hợp lệ nhưng giá trị ngoài dải vật lý cho phép | Báo ngay khi có khung ngoài dải (không có bước thử lại số trước) | Như trên |
| 103 | CAM BIEN BAT THUONG | Stop | Không | Nhiệt độ tụt đột ngột > 1.5 °C chưa được xác nhận | **Có xác nhận trước**: cần 3 mẫu liên tiếp mới quyết định chấp nhận/từ chối | Như trên |
| 110 | NHIET DO THAP | Warning | Không | Nhiệt độ dưới ngưỡng cấu hình | Báo ngay theo ngưỡng, không có bước thử | Không khoá gì |
| 111 | NHIET DO CAO | Stop | Không | Nhiệt độ vượt ngưỡng cao | Báo ngay | Cắt SSR (giữ contactor), ép cả 2 quạt, khoá đảo |
| 112 | QUA NHIET KHAN | Emergency | **Không** (tự xoá khi nhiệt hạ) | Nhiệt độ vượt ngưỡng khẩn cấp | Báo ngay | Cắt SSR + nhả contactor tổng ngay, ép cả 2 quạt, khoá đảo |
| 113 | NHIET DO BIEN THIEN NHANH | Warning | Không | Tốc độ đổi nhiệt vượt ngưỡng | Báo ngay (cảnh báo chẩn đoán sớm) | Không khoá gì |
| 114 | NHIET DO KHONG ON DINH | Warning | Không | Nhiệt dao động vượt biên độ cho phép | Báo ngay | Không khoá gì |
| 115 | THANH NHIET KHONG NONG | Warning | Không | SSR đang bật nhưng nhiệt không tăng đủ sau thời gian quy định | Có theo dõi thời gian trước khi kết luận (không báo tức thì khi vừa bật) | Không khoá gì (chỉ cảnh báo) |
| 120 | DO AM THAP | Warning | Không | Độ ẩm dưới ngưỡng | Báo ngay | Không khoá gì |
| 121 | DO AM CAO | Warning | Không | Độ ẩm vượt ngưỡng | Báo ngay | Không khoá gì |
| 130 | TAT CONG TAC NHIET | Stop | Không | Công tắc vật lý cho phép nhiệt bị tắt giữa mẻ | Báo ngay | Cắt SSR + nhả contactor |
| 132 | CAN CHUYEN SANG AUTO | Warning | Không | Đang cố áp lại mẻ nhưng công tắc AUTO chưa bật | Báo ngay | Không khoá gì (chỉ chặn hành động Resume) |
| 133 | AUTO BI TAT GIUA ME | Stop | Không | Công tắc AUTO bị gạt sang MANUAL trong lúc đang chạy mẻ | Có trễ chống nhiễu (`AUTO_LOST_ALARM_DELAY_MS`) trước khi báo | Khoá đảo (vẫn giữ điều khiển nhiệt) |
| 134 | TU DONG DAO BI TAT | Stop | Không | Cấu hình `turningEnabled` bị tắt giữa mẻ | Báo ngay | Khoá đảo |
| 135 | CHO XAC NHAN AP LAI | Warning | Không | Màn hình "Áp lại mẻ cũ" chờ quá `RESUME_CONFIRM_ALERT_MS` (15 phút) không ai thao tác | Chờ đủ thời gian mới báo (không phải ngay khi hiện màn hình) | Không khoá gì (chỉ nhắc) |
| 136 | ME QUA HAN AP | Warning | Không | Số ngày ấp thực tế vượt `totalIncubationDays` cấu hình | Báo ngay khi qua ngưỡng | Không khoá gì |
| 201 | LOI 2 HANH TRINH | Stop | **Có** | Cả 2 công tắc hành trình cùng báo tích cực (xung đột vật lý) | Báo ngay | Khoá đảo |
| 202 | DAO QUA THOI GIAN | Stop | **Có** | Motor đảo chạy quá `turnMaxRunSec` mà không tới công tắc hành trình | Có timeout theo dõi trước khi báo | Khoá đảo |
| 203 | HANH TRINH BI KET | Stop | **Có** | Công tắc hành trình không nhả sau khi đã dừng lệnh đảo | Có theo dõi trước khi báo | Khoá đảo |
| 204 | XUNG DOT LENH DAO | Stop | **Có** | Lệnh đảo trái/phải xung đột nhau | Báo ngay | Khoá đảo |
| 205 | CAN KIEM TRA CO KHI DAO | Stop | Đặc biệt (xem PHẦN 3/13) | 3 lần lỗi đảo (202/203) liên tiếp không có lần thành công xen giữa | Đếm chuỗi lỗi trước khi nâng cấp cảnh báo | Khoá đảo hoàn toàn |
| 301 | MAT EEPROM | Stop | **Có** | EEPROM mất kết nối/lỗi ≥ 3 lần liên tiếp khi máy không chạy mẻ | **Có retry + reconnect trước** (xem PHẦN 4) | Cắt SSR + nhả contactor |
| 302 | EEPROM SUY GIAM | Warning | Không | EEPROM lỗi 1-2 lần liên tiếp (chưa tới ngưỡng "mất hẳn") | Có retry trước | Không khoá gì (nếu đang chạy mẻ, tiếp tục từ RAM) |
| 303 | RESET BAT THUONG | Stop | **Có** | Chip khởi động lại do panic/WDT/brownout (không phải do người dùng chủ động) | Không có bước "thử trước" — phát hiện ngay ở lần boot kế tiếp qua `esp_reset_reason()` | Cắt SSR + nhả contactor cho tới khi ACK |
| 304 | XUNG DOT OUTPUT | Emergency | **Có** | Phát hiện 2 đầu ra loại trừ nhau cùng bật (an toàn phần cứng cấp thấp nhất) | Báo ngay | Cắt toàn bộ nhiệt, ép quạt, khoá đảo |
| 305 | RELAY QUA NHIEU | Warning | **Có** | Relay đóng/cắt vượt tần suất cho phép trong khoảng thời gian | Có đếm tần suất trước khi báo | Không khoá output (chỉ cảnh báo hao mòn relay) |
| 306 | LOI RTC | Warning | Không | RTC mất kết nối/OSF/dữ liệu không hợp lệ | **Có tự sửa trước** (xem PHẦN 5) — chỉ báo khi tự sửa cũng thất bại hoặc chưa đủ điều kiện tự sửa | Không khoá gì |
| 313 | CHUA XOA DU LIEU ME | Stop | Không | Đã dừng/huỷ mẻ nhưng EEPROM chưa xác nhận ghi `wasRunning=0` | Có retry ghi trước | Cắt SSR + nhả contactor |
| 314 | LOI NHAT KY AN TOAN | Stop | **Có** | NVS nội bộ (safety journal) ghi lỗi | Báo ngay khi ghi thất bại | Cắt SSR + nhả contactor, cấm bắt đầu/phục hồi mẻ |
| 315 | MAT NHAT KY ME | Warning | Không | Log chi tiết nhiệt/đảo của mẻ không ghi được | Báo ngay | Không khoá gì (không ảnh hưởng an toàn) |

---

## 9. Flow xử lý lỗi (theo đúng source code, không dùng khuôn mẫu chung)

### Sensor mất kết nối (101/102/103)
```text
Modbus poll (mỗi 2000 ms)
     |
Đọc thất bại (timeout/CRC/format)?
     |-- CHUA het 2 lan thu trong 1 chu ky -> cho 80 ms, thu lai
     |
Het 2 lan thu / 2 chu ky loi lien tiep
     |
online_ = false  --> sensorUsable_ = false (neu qua thoi gian an han khoi dong)
     |
FaultCode 101 duoc bao (khong latching)
     |
OutputArbiter: cat SSR + nha contactor tong, ep quat tuan hoan
(TRUNG VAN DUOC DAO BINH THUONG - khong bi khoa)
     |
Modbus poll van tiep tuc chay ngam
     |
Co khung hop le dau tien tro lai
     |
sensorUsable_ CHUA bat lai ngay - can 3 mau lien tiep "tot" (goodSensorStreak_)
     |
Du 3 mau tot -> sensorUsable_ = true -> FaultCode 101 tu xoa (khong can ACK)
     |
PID reset (tranh giat nhiet do vua mat mau)
```

### EEPROM mất kết nối trong lúc đang chạy mẻ
```text
Health-check probe (moi 5000 ms) / thao tac doc-ghi that bai
     |
latchStorageFault()
     |
Dang chay me (batchRunning_ || resumePending_)?
     |-- CO: KHONG doc lai config tu EEPROM (RAM la nguon dung), chi cho "suy giam"
     |        (StorageDegraded, khong khoa gi, van dieu khien binh thuong tu RAM)
     |-- KHONG: neu du 3 lan loi lien tiep -> StorageUnavailable (khoa SSR+contactor)
     |
Reconnect moi 3000 ms
     |
store_.reconnect() thanh cong?
     |-- KHONG -> giu nguyen trang thai loi, cho vong sau
     |-- CO -> dang chay me: ghi lai config RAM vao EEPROM de xac minh module on dinh
     |          -> thanh cong: xoa co loi, ghi checkpoint me ngay
     |          -> that bai: store_.markOffline(), cho vong sau
```

### RTC lỗi/OSF
```text
Doc RTC (moi 1000 ms)
     |
OSF=1 hoac du lieu khong hop le?
     |-- KHONG -> valid_ = true, cap nhat gio binh thuong
     |-- CO -> valid_ = false, tang invalidReadConfirm_
     |
Du 2 lan xac nhan lien tiep VA con duoi 3 lan thu?
     |-- CHUA du dieu kien / da het 3 lan thu -> KHONG tu sua, giu FaultCode 306
     |-- Du dieu kien -> tinh estimatedShadowEpoch() (dua tren millis() cua ESP32)
              |
         Co "dong ho bong" dang tin (chua qua 24h ke tu lan RTC dung cuoi)?
              |-- KHONG (vd ESP32 cung vua mat dien) -> tu sua that bai, giu loi
              |-- CO -> ghi gio uoc luong nguoc vao RTC, xoa co OSF
                        -> FaultCode 306 tu xoa (khong can ACK)
```

### Motor đảo lỗi lặp lại → khoá cơ khí (205)
```text
Loi dao (TurnTimeout=202 hoac TurnLimitStuck=203) duoc latch
     |
turnFaultStreak_ += 1  (chi tang cho 2 ma loi nay, reset ve 0 moi khi co 1 lan dao
                         THANH CONG xen giua - completeTurn())
     |
turnFaultStreak_ >= TURN_FAULT_STREAK_LIMIT (3)?
     |-- CHUA -> chi bao 202/203 binh thuong, ACK la het (loai bo qua che do dao thu)
     |-- DU -> TurnMechanicalCheckRequired (205) duoc bat, KHOA dao hoan toan
              |
         ACK thuong (AlarmAck) co xoa duoc 205 khong? -> KHONG
              |
         Chi co 1 duong xoa: vao Test Mode, xac nhan CA 2 cong tac hanh trinh
         deu bao Success rieng le
              |
         testLimitVerifiedLeft_ VA testLimitVerifiedRight_ deu true?
              |-- CO -> 205 tu xoa, turnFaultStreak_ reset ve 0
              |-- CHUA -> giu khoa
```
> Lưu ý: `enterTestMode()` đã có sẵn điều kiện từ chối vào Test Mode khi
> `batchRunning_` — nghĩa là để gỡ khoá 205, bắt buộc phải **dừng mẻ** trước.
> Đây là đánh đổi thiết kế có chủ đích (đã ghi nhận trong audit trước), không
> phải thiếu sót.

### Task điều khiển "treo" (Supervisor trip)
```text
Supervisor doc controlHeartbeatMs moi chu ky (SUPERVISOR_TASK_PERIOD_MS)
     |
elapsedMs(now, ctrlBeat) > CONTROL_HEARTBEAT_TIMEOUT_MS
   HOAC so chu ky cham lien tiep >= CONTROL_CYCLE_TRIP_COUNT?
     |-- KHONG -> binh thuong, "an" WDT cua chinh supervisor, ngu tiep
     |-- CO -> mayapLatchSystemTrip() (co toan cuc, OutputArbiter tu day
              chi chap nhan trang thai an toan)
              -> vTaskSuspend(controlTaskHandle)
              -> mayapSafeOutputsEarly() lien tuc trong SUPERVISOR_RESTART_FALLBACK_MS
              -> esp_restart()  (KHONG co buoc "thu lai tai cho")
```

---

## 10. Những điểm firmware còn thiếu cơ chế recovery

| Chức năng | Hiện trạng | Mức độ | Đề xuất (chỉ đề xuất, không tự sửa code) |
|---|---|---|---|
| LCD mất kết nối vĩnh viễn | **CHƯA CÓ** cảnh báo ra ngoài (chỉ có Serial log nội bộ, chỉ khi bật debug) | Trung bình (không ảnh hưởng an toàn nhiệt/đảo, nhưng người vận hành tại chỗ sẽ "mù" hoàn toàn nếu không có web/app mở song song) | Thêm 1 `FaultCode` cho "LCD mất kết nối kéo dài", đẩy qua alarm hệ thống + Cloud Push để người vận hành biết dù không đứng cạnh máy |
| Cảnh báo pin RTC sắp yếu (trước khi mất) | **CHƯA CÓ** | Thấp/Trung bình (RTC mất giờ không ảnh hưởng an toàn nhiệt tức thời, nhưng ảnh hưởng tính đúng của lịch mẻ ấp/log) | Vì DS3231 không đo được mức pin, có thể cân nhắc theo dõi "số lần OSF xảy ra" hoặc đơn giản là nhắc định kỳ theo tuổi thọ pin (thời gian) trong tài liệu bảo trì, thay vì phần cứng |
| Kiểm tra "giá trị cảm biến đứng yên" (stuck/frozen) | **CHƯA CÓ** | Trung bình (một cảm biến hỏng kiểu "đứng yên ở giá trị hợp lệ" — ví dụ kẹt ở 30°C — sẽ không bị bắt bởi range-check lẫn plausibility-step-check hiện tại) | Thêm kiểm tra: nếu giá trị đọc được không đổi (dao động < ngưỡng rất nhỏ) trong khoảng thời gian dài bất thường so với đặc tính nhiệt của lò ấp, nâng cảnh báo riêng |
| Task HMI/Network "treo" | **CHƯA CÓ** giám sát riêng | Thấp/Trung bình (2 task này không gắn Task WDT của ESP-IDF, và Supervisor hiện chỉ theo dõi `controlHeartbeatMs`/`hmiHeartbeatMs` — có đọc `hmiHeartbeatMs` nhưng KHÔNG có nhánh hành động nào cho `!hmiHealthy`, chỉ log; không giám sát nhịp tim networkTask) | Cân nhắc thêm hành động thực sự (không chỉ log) khi HMI "treo", và thêm heartbeat cho networkTask |
| Mã lỗi cho sự kiện Supervisor trip | **CHƯA CÓ** | Thấp (đã có `AbnormalReset` ở lần boot kế tiếp nếu lý do reset khớp panic/WDT, nhưng bản thân sự kiện "trip" không có mã lỗi/log bền vững ngoài Serial tạm thời) | Ghi sự kiện trip vào `eventLog_`/safety journal trước khi reset để có thể truy vết sau này |

---

## 11. Kết luận (Phần phân tích)

**Đã tự phục hồi được:** LCD (detect + re-init + bus recovery), EEPROM (retry + CRC +
reconnect + verify + tự phục hồi cấu hình cũ qua nhiều schema), RTC (tự sửa giờ có
điều kiện qua đồng hồ bóng), Sensor (retry + tự nhận lại sau nhiều mẫu tốt liên
tiếp), Wi-Fi/MQTT/Cloud Push (backoff vô hạn), mẻ ấp sau mất điện/reset bất thường
(tự áp lại có điều kiện hoặc chờ xác nhận).

**Chưa tự phục hồi được (cần người vận hành/kỹ thuật viên):** Motor đảo khi lỗi
latching (201-204, cần ACK) và đặc biệt khoá cơ khí 205 (chỉ gỡ qua Test Mode);
EEPROM mất hẳn (301, cần kỹ thuật kiểm tra phần cứng); Reset bất thường (303, cần
ACK xác nhận đã kiểm tra máy); Xung đột output (304, sự cố an toàn cấp cao nhất);
Task điều khiển treo (chỉ có 1 hành động duy nhất: reset cứng toàn bộ, không có
"thử lại tại chỗ").

**Chỉ báo E-code, không có bước tự thử ở giữa:** 102 (giá trị ngoài dải), 111
(nhiệt cao), 112 (khẩn cấp), 120/121 (ẩm thấp/cao), 130 (công tắc nhiệt), 134 (tắt
đảo tự động), 201/204 (xung đột hành trình/lệnh đảo), 303 (reset bất thường), 304
(xung đột output), 314 (lỗi nhật ký an toàn).

**Có bước tự thử/theo dõi trước khi báo:** 101, 103, 113/114/115 (theo dõi theo
thời gian), 133 (có trễ chống nhiễu), 202/203 (theo dõi timeout), 301/302 (retry +
reconnect), 306 (tự sửa RTC trước).

**Cần kỹ thuật viên (không phải người vận hành):** 301 (mất EEPROM — cần kiểm tra
phần cứng/dây nối module DS3231+AT24C32), 303 (reset bất thường lặp lại — cần tìm
nguyên nhân phần cứng/nguồn điện), 304 (xung đột output — sự cố nghiêm trọng ở tầng
điều khiển đầu ra), 314 (lỗi nhật ký an toàn), 205 lặp lại nhiều lần dù đã qua Test
Mode (nghi ngờ hỏng cơ khí thật sự — công tắc hành trình/động cơ/dây đai).

**Phát hiện nhưng xử lý chưa đầy đủ (đáng chú ý khi vận hành):**
- `EmergencyTemperature` (112, mức Emergency — mức nghiêm trọng nhất trong hệ
  thống) có **`latching = false`** trong bảng `faultDescriptor` — nghĩa là khi
  nhiệt độ tự hạ xuống dưới ngưỡng khẩn cấp, cảnh báo **tự xoá ngay, không yêu cầu
  người vận hành xác nhận (ACK)** trước khi hệ thống coi như "đã qua sự cố". Đây là
  hành vi thực tế của code, không phải suy đoán — người vận hành nên biết điều
  này để chủ động kiểm tra thủ công sau mỗi lần có cảnh báo 112, dù màn hình có
  thể đã tự tắt cảnh báo.
- Task HMI có heartbeat được Supervisor đọc (`hmiHeartbeatMs`, `hmiHealthy`) nhưng
  code hiện tại chỉ dùng để **ghi log** khi đổi trạng thái, không có hành động
  phục hồi nào (không suspend, không restart) nếu HMI "treo" — khác với Control
  task (có hành động reset đầy đủ).

---

## 12. Tài liệu vận hành dành cho người không chuyên

### 12.1. Tổng quan thiết bị

Máy ấp trứng công nghiệp MAYAP tự động kiểm soát nhiệt độ, độ ẩm và chu kỳ đảo
trứng trong suốt quá trình ấp. Các bộ phận người vận hành cần biết:

- **Màn hình LCD** (trên tủ điều khiển): hiển thị nhiệt độ, độ ẩm, trạng thái mẻ
  ấp, và các cảnh báo (nếu có).
- **Nút xoay** cạnh màn hình: dùng để di chuyển menu, chọn mục, xác nhận.
- **Công tắc AUTO/MANUAL** (công tắc vật lý, không phải nút trên màn hình): gạt
  sang AUTO để máy tự vận hành theo chương trình đã cài; gạt sang MANUAL để điều
  khiển tay từng thiết bị (nhiệt, quạt, đèn, đảo trái/phải).
- **Các công tắc tay khác** (khi ở MANUAL): bật/tắt nhiệt, quạt tuần hoàn, đèn,
  đảo trái, đảo phải.
- **Đèn báo/còi báo**: kêu/nhấp nháy khi có cảnh báo cần chú ý.
- **Web/App trên điện thoại** (nếu máy đang kết nối mạng): xem trạng thái từ xa,
  nhận thông báo đẩy khi có sự cố.

### 12.2. Quy trình khởi động

**Trước khi bật máy:**
- Kiểm tra nguồn điện đã cắm chắc chắn, không có dấu hiệu cháy/hở dây.
- Kiểm tra cảm biến nhiệt/ẩm (đầu dò gắn trong buồng ấp) không bị rơi/lỏng.
- Kiểm tra cơ cấu đảo trứng di chuyển tự do, không bị vướng vật cản.
- Kiểm tra công tắc AUTO/MANUAL đang ở vị trí phù hợp với ý định vận hành.
- Kiểm tra tất cả cửa/nắp buồng ấp đã đóng kín.

**Bật máy:**
1. Bật nguồn tổng. Màn hình LCD sẽ sáng lên (có thể mất vài giây nếu module màn
   hình đang tự dò lại kết nối — đây là bình thường, máy vẫn đang khởi động các bộ
   phận khác song song).
2. Quan sát màn hình chính: nhiệt độ, độ ẩm, giờ hiện tại phải hiển thị hợp lý.
3. Nếu màn hình hiện cảnh báo ngay khi khởi động (ví dụ mất cảm biến, lỗi RTC), xem
   PHẦN 12.6 bên dưới để tra mã lỗi.

**Khi máy khởi động thành công**, người vận hành cần thấy: nhiệt độ/độ ẩm hiển thị
số thực (không phải "--"), giờ đúng thực tế, không có cảnh báo đỏ nào đang treo
trên màn hình.

### 12.3. Chế độ AUTO

**AUTO dùng để làm gì?** Máy tự động giữ nhiệt độ/độ ẩm theo cài đặt, tự đảo trứng
theo chu kỳ đã hẹn, tự cảnh báo khi có bất thường.

**Khi nào được phép dùng AUTO?** Sau khi đã kiểm tra đầy đủ theo mục "Trước khi bật
máy" ở trên và đã cài đặt xong nhiệt độ/độ ẩm mục tiêu, chu kỳ đảo, số ngày ấp.

**Cách chuyển sang AUTO:** Gạt công tắc vật lý AUTO/MANUAL sang vị trí AUTO. Không
có bước "chọn trên màn hình" — đây là công tắc cứng.

**Khi ở AUTO, máy tự làm gì (theo đúng logic firmware):**
```text
Cong tac AUTO dang bat?
     |
Cam bien nhiet/am co du lieu tin cay?
     |-- KHONG -> tam cat nhiet (ca 2 tang), ep quat tuan hoan, VAN TIEP TUC DAO
     |-- CO -> dieu khien nhiet theo PID huong ve nhiet do dat
              -> den chu ky da hen -> thuc hien dao trung (neu khong bi khoa boi
                 loi hanh trinh/lenh xung dot)
              -> lien tuc kiem tra cac dieu kien an toan (nhiet cao/thap qua muc,
                 do am ngoai nguong, cong tac nhiet tat, RTC/EEPROM...)
```

### 12.4. Chế độ MANUAL

**MANUAL dùng để làm gì?** Điều khiển tay từng thiết bị (nhiệt, quạt, đèn, đảo)
khi cần can thiệp trực tiếp (vệ sinh, kiểm tra, hiệu chỉnh).

**Khi nào dùng?** Khi bảo trì, kiểm tra thiết bị, hoặc khi KHÔNG đang trong một mẻ
ấp cần tự động hoàn toàn.

**Cách chuyển sang MANUAL:** Gạt công tắc vật lý sang MANUAL.

**Điều khiển từng output (đều là công tắc vật lý riêng, theo `InputChannel` trong
firmware):** Heater (nhiệt), Fan (quạt tuần hoàn), Light (đèn), Turn Left/Turn
Right (đảo trái/phải).

**Chức năng bị khoá bởi interlock/an toàn khi đang có mẻ chạy hoặc có lỗi:**
- Nếu đang chạy một mẻ ấp và gạt sang MANUAL: máy phát cảnh báo `133 AUTO BI TAT
  GIUA ME` và **khoá đảo trứng** (không cho đảo tay trong khi mẻ vẫn coi là đang
  chạy) — vẫn giữ điều khiển nhiệt để trứng không bị nguội đột ngột.
- Nếu 2 công tắc hành trình đang xung đột (`201`) hoặc đã bị khoá cơ khí (`205`):
  đảo trái/phải bị khoá bất kể AUTO hay MANUAL, cho tới khi xử lý xong theo PHẦN
  12.6.

### 12.5. Chuyển đổi AUTO ↔ MANUAL

| Trạng thái hiện tại | Thao tác | Điều kiện | Kết quả |
|---|---|---|---|
| AUTO (đang chạy mẻ) | Gạt sang MANUAL | Không có điều kiện chặn phần cứng — công tắc luôn gạt được | Máy phát cảnh báo `133`, khoá đảo trứng, vẫn giữ điều khiển nhiệt |
| MANUAL | Gạt sang AUTO | Nếu đang có mẻ chờ "áp lại" (resume) mà công tắc AUTO đang tắt | Máy sẽ báo `132 CAN CHUYEN SANG AUTO` và không tự áp lại mẻ cho tới khi gạt sang AUTO |
| MANUAL | Gạt sang AUTO | Bình thường (không có mẻ chờ áp lại) | Máy vào AUTO ngay, tiếp tục/bắt đầu điều khiển tự động |

> Nếu gạt công tắc mà máy không phản ứng ngay: đợi tối đa vài trăm mili-giây (thời
> gian chống dội công tắc/debounce) rồi kiểm tra lại màn hình.

### 12.6. Xử lý Error Code

> Bảng đầy đủ 25 mã lỗi tại PHẦN 8. Dưới đây là mẫu diễn giải cho các mã người vận
> hành gặp thường xuyên nhất, theo đúng hành vi firmware đã xác nhận.

#### 101 — Mất cảm biến
**Ý nghĩa:** Máy không nhận được tín hiệu hợp lệ từ đầu dò nhiệt độ/độ ẩm.
**Trước khi báo:** Firmware đã tự thử đọc lại 2 lần mỗi chu kỳ, trong 2 chu kỳ liên
tiếp (khoảng vài giây) trước khi kết luận mất kết nối.
**Người vận hành làm:**
1. Kiểm tra đầu dò/dây tín hiệu RS485 có bị lỏng, đứt, hoặc bị chuột cắn không.
2. Kiểm tra đầu dò có bị rơi ra khỏi buồng ấp không.
3. Không tự ý tháo/mở cảm biến nếu không được đào tạo.
4. Chờ máy tự nhận lại — không cần bấm gì, hệ thống tự động thử lại liên tục.
**Nếu vẫn còn sau khi đã kiểm tra dây/đầu nối:** liên hệ kỹ thuật.

#### 205 — Cần kiểm tra cơ khí đảo
**Ý nghĩa:** Đảo trứng đã lỗi lặp lại nhiều lần liên tiếp không có lần nào thành
công xen giữa — nghi ngờ hỏng cơ khí (công tắc hành trình, dây đai, động cơ).
**Trước khi báo:** Firmware đã tự chờ hết thời gian tối đa cho mỗi lần đảo và đếm
đủ 3 lần lỗi liên tiếp mới nâng lên mức khoá này.
**Người vận hành làm:**
1. **KHÔNG** cố ép đảo lại nhiều lần — hệ thống đã chủ động khoá để bảo vệ cơ khí.
2. Dừng mẻ ấp hiện tại (bắt buộc, vì máy yêu cầu dừng mẻ mới cho vào được Test
   Mode).
3. Vào Test Mode, thực hiện xác nhận cả 2 công tắc hành trình theo hướng dẫn trên
   màn hình.
4. Nếu cả 2 công tắc xác nhận thành công, khoá tự động gỡ.
**Nếu Test Mode cũng không xác nhận được:** liên hệ kỹ thuật — có khả năng hỏng
công tắc hành trình hoặc cơ cấu đảo thật sự.

#### 301 — Mất EEPROM
**Ý nghĩa:** Module lưu trữ cấu hình/dữ liệu mẻ ấp (EEPROM ngoài) mất kết nối 3 lần
liên tiếp trong lúc máy không chạy mẻ.
**Trước khi báo:** Đã tự retry nhiều lần ở tầng đọc/ghi, và tự thử kết nối lại định
kỳ mỗi 3 giây trước khi báo mức "mất hẳn".
**Người vận hành làm:**
1. Không tự ý mở tủ điện để kiểm tra module.
2. Ghi lại thời điểm xảy ra, báo kỹ thuật viên.
**Khi nào cần gọi kỹ thuật:** Ngay khi thấy mã này — đây là lỗi phần cứng, không
có thao tác tại chỗ nào người vận hành được phép tự sửa.

---

## 13. Troubleshooting decision tree

```text
May khong hoat dong
       |
LCD co hien thi khong?
   +---------+----------+
   KHONG               CO
    |                   |
Kiem tra nguon      Co canh bao (E-code) dang hien khong?
dien, cau chi     +--------+---------+
(khong tu y mo         CO             KHONG
tu dien)                |               |
                  Tra bang E-code   May co dang chay dung
                  (PHAN 8/12.6)     nhu ky vong khong?
                                    (nhiet/am/dao)
                                        |
                                Kiem tra cong tac AUTO/MANUAL
                                dang o vi tri nao
```

### Riêng cho LCD
```text
LCD toi den / khong ve
       |
Cho toi 3-5 giay (firmware tu retry moi 3 giay)
       |
LCD tu sang lai (co toast "LCD DA TU PHUC HOI")?
   +--------+--------+
   CO              KHONG
   |                |
Tiep tuc      Kiem tra day/cong I2C cua module LCD
van hanh      (khong tu y thao module khi may dang co dien)
              -> Neu van khong len sau vai phut: goi ky thuat
```

### Riêng cho EEPROM
```text
Man hinh bao 301 (MAT EEPROM) hoac 302 (EEPROM SUY GIAM)
       |
302 (suy giam)?
   +-----+-----+
   CO         KHONG (la 301)
   |            |
Chi la      May da tu thu ket noi lai nhieu lan that bai
canh bao       |
som, may   Goi ky thuat de kiem tra module EEPROM/day noi
van tu
thu ket
noi lai
ngam, theo
doi them
```

### Riêng cho RTC
```text
Man hinh bao 306 (LOI RTC) / gio hien thi sai
       |
May co vua mat dien hoan toan gan day khong?
   +-----+-----+
   CO           KHONG
   |             |
Firmware      Firmware co the tu sua gio dua tren dong ho
KHONG the     boi trong RAM (khong can thao tac)
tu doan       -> Cho vai giay, kiem tra lai gio tren man hinh
gio duoc      -> Neu van sai: vao menu cai dat gio thu cong,
(khong        hoac goi ky thuat neu nghi ngo hong module RTC
biet mat
dien bao
lau) -> Vao
menu cai
dat gio
thu cong
```

### Riêng cho Sensor
```text
Bao 101/102/103 (loi cam bien)
       |
101 (mat ket noi)?
   +-----+----------------+
   CO                   KHONG (102/103 - du lieu bat thuong)
   |                       |
Kiem tra day RS485,    102: gia tri vuot nguong vat ly that su
dau noi cam bien       -> kiem tra cam bien co bi hong/dat sai vi tri
-> May tu thu ket      103: dang trong giai doan xac nhan bien dong
noi lai lien tuc,      dot ngot -> cho vai giay de he thong tu xac
khong can bam gi       nhan lai (khong can can thiep neu la nhieu
                       thoang qua)
```

### Riêng cho Communication (Wi-Fi/MQTT/Cloud Push)
```text
Web/App bao "Offline" / khong nhan duoc thong bao
       |
May van dang chay binh thuong tai cho (nhiet/am/dao khong
phu thuoc mang) -> KHONG anh huong an toan truc tiep
       |
He thong tu dong thu ket noi lai theo chu ky lui-va-doi
(1-2-4-8-16-30-60 giay), khong bao gio "bo cuoc"
       |
Kiem tra Wi-Fi router/nguon mang tai cho neu can xem tu xa gap
```

### Riêng cho Auto mode / Manual mode
```text
May khong dao trung nhu ky vong
       |
Cong tac dang o AUTO hay MANUAL?
   +--------+---------+
   AUTO             MANUAL
   |                  |
Co canh bao      Day la binh thuong - o MANUAL
201/202/203/     phai bam cong tac dao tay
204/205          truc tiep, may khong tu dao
dang hien
khong?
   +---+---+
   CO     KHONG
   |        |
Tra    Kiem tra da den chu ky
bang   dao chua (theo cau hinh
E-code turnIntervalMin)
```

---

## 14. Checklist vận hành (in ra dán cạnh máy)

### Trước khi vận hành
- [ ] Kiểm tra nguồn điện, cầu chì, dây dẫn không hư hỏng
- [ ] Kiểm tra đầu dò nhiệt độ/độ ẩm gắn chắc chắn trong buồng ấp
- [ ] Kiểm tra cơ cấu đảo trứng di chuyển tự do, không vướng
- [ ] Kiểm tra cửa/nắp buồng ấp đã đóng kín
- [ ] Chọn công tắc AUTO/MANUAL đúng ý định vận hành
- [ ] Xác nhận màn hình hiển thị nhiệt độ/độ ẩm/giờ hợp lý, không có cảnh báo treo

### Khi máy báo lỗi
- [ ] Ghi lại đúng mã lỗi (E-code) hiện trên màn hình
- [ ] Tra bảng E-code (PHẦN 8/12.6) để biết mức độ và bước xử lý
- [ ] Chỉ thực hiện các thao tác được liệt kê là "người vận hành được phép làm"
      (PHẦN 15)
- [ ] Với lỗi cần ACK (xem cột "Latching" ở PHẦN 8): xác nhận (ACK) trên màn hình
      SAU KHI đã kiểm tra nguyên nhân, không ACK chỉ để "cho hết kêu"
- [ ] Kiểm tra lại mã lỗi có còn hiển thị sau khi xử lý không
- [ ] Nếu vẫn còn, hoặc thuộc nhóm "cần kỹ thuật viên" (PHẦN 11) → liên hệ kỹ thuật
      ngay, không tiếp tục vận hành

---

## 15. Quy tắc an toàn

### Người vận hành được phép làm
- Kiểm tra dây/đầu nối bên ngoài tủ điện (không tháo nắp tủ điện).
- Kiểm tra trạng thái công tắc AUTO/MANUAL, Heater/Fan/Light/Turn L/R.
- Xác nhận (ACK) cảnh báo trên màn hình sau khi đã kiểm tra nguyên nhân.
- Gạt chuyển AUTO/MANUAL theo đúng quy trình PHẦN 12.5.
- Vào Test Mode để xác nhận công tắc hành trình khi được hướng dẫn (mã lỗi 205),
  sau khi đã dừng mẻ ấp.
- Dừng/Huỷ mẻ ấp theo nút/menu trên màn hình hoặc web/app khi cần.

### Người vận hành KHÔNG được phép làm
- Mở tủ điện, tháo nắp bảo vệ mạch điều khiển.
- Đo điện áp/dòng điện bên trong tủ điện.
- Tháo board mạch, module cảm biến, module RTC, module EEPROM khi máy đang có
  điện.
- Cố ép output hoạt động khi hệ thống đang khoá vì lý do an toàn (ví dụ cố đảo tay
  khi đang bị khoá cơ khí 205).
- Bỏ qua/vô hiệu hoá interlock an toàn dưới bất kỳ hình thức nào.
- Thay đổi firmware, cấu hình nâng cao, hoặc can thiệp trực tiếp vào dữ liệu
  EEPROM.
- Tiếp tục vận hành khi đang có cảnh báo thuộc nhóm "cần kỹ thuật viên".

---

## 16. Phần dành cho kỹ thuật viên

### Bảng tra cứu kỹ thuật đầy đủ

| E-code | Module | Function liên quan | Điều kiện trigger | Recovery sequence | Retry/Timeout | Điểm kiểm tra khi lỗi lặp lại |
|---|---|---|---|---|---|---|
| 101/102/103 | `SHT485Industrial`, `processSensor()` | `decodeFrame()`, `failAttempt()`, `updateFreshness()` | Xem PHẦN 6 | Xem PHẦN 6/9 | 2 lần/chu kỳ, offline sau 2 chu kỳ, stale sau 6500 ms | Dây RS485 (A/B đảo cực?), điện trở terminator, nguồn cấp cảm biến, địa chỉ Modbus slave (mặc định 1) |
| 111/112 | `updateAlarms()`/output logic | Xem faultDescriptor dòng 430-432 | Ngưỡng nhiệt cấu hình | Không có bước tự thử — Warning/Stop tức thời theo ngưỡng | — | Hiệu chuẩn `tempOffset`, kiểm tra SSR có kẹt ON không (đối chiếu với `HeaterNotHeating`=115) |
| 201-204 | `machine_control.h` (Turn logic, đã xác nhận ở phiên audit trước) | `latchTurnFault()`, `updateTurnOutputs()` | Xem PHẦN 8 | Cần ACK (`AlarmAck`) | `turnMaxRunSec` (cấu hình) | Cảm biến/công tắc hành trình, dây đai, độ trễ dead-time đổi chiều |
| 205 | `latchTurnFault()`, `updateTestMode()` | Đếm `turnFaultStreak_`, xoá qua `testLimitVerifiedLeft_/Right_` | `turnFaultStreak_ >= TURN_FAULT_STREAK_LIMIT` (3) | Chỉ gỡ qua Test Mode dual-verify | `TURN_FAULT_STREAK_LIMIT`=3 | Cơ khí đảo, công tắc hành trình, có thể cần thay thế phần cứng nếu lặp lại sau khi đã xác nhận Test Mode |
| 301/302 | `ExternalEeprom24xx`, `PersistentStore`, `latchStorageFault()` | Xem PHẦN 4 | `storageFailureStreak_ >= EEPROM_FAILURE_LATCH_COUNT` (3), chỉ khi không chạy mẻ | Retry I/O + reconnect + verify 2 lần | Health-check 5000 ms, reconnect 3000 ms | Địa chỉ I2C module (AT24C32 + DS3231 chung bus), dây SDA/SCL, pull-up, nguồn 3.3V module |
| 306 | `RtcDs3231` | `tryAutoRepair()` | OSF=1 hoặc dữ liệu không hợp lệ | Tự sửa qua đồng hồ bóng (có điều kiện, xem PHẦN 5) | 3 lần thử, 3000 ms/lần | Pin CR2032 của module DS3231, tuổi thọ pin, tiếp xúc pin |
| 303 | `power_` (reset reason tracking) | `resetReasonIsAbnormal()` | `esp_reset_reason()` trả về PANIC/WDT/BROWNOUT... | Không tự phục hồi — cần ACK | — | Nguồn cấp ổn định (brownout?), log Serial trước khi reset (nếu còn), tần suất reset lặp lại |
| 304 | `OutputArbiter` (đã xác nhận phiên trước) | `forceSafe()` | 2 đầu ra loại trừ nhau cùng bật | Không tự phục hồi — cần ACK + kiểm tra phần cứng | — | Relay/SSR có kẹt tiếp điểm không, dây điều khiển đầu ra có chéo nhau không |
| 314 | Safety journal (NVS) | `safetyJournal_.begin()` | Ghi NVS thất bại | Không tự phục hồi — cần ACK | — | Vòng đời flash NVS (mòn theo số lần ghi), có thể cần format lại phân vùng NVS (thao tác kỹ thuật viên, không phải người vận hành) |

### Interlock tổng hợp (đầu ra bị ảnh hưởng theo từng lỗi)
Xem đầy đủ tại bảng PHẦN 8, cột suy ra trực tiếp từ `faultDescriptor()` — các cờ
`inhibitSsr`, `dropHeatMaster`, `inhibitsTurning`, `forceVentFan`,
`forceCirculationFan` áp dụng ngay khi lỗi được `set()` là `true`, độc lập với việc
lỗi đó có `latching` hay không.

### Reset condition (điều kiện xoá lỗi)
- **Non-latching:** tự xoá ngay khi điều kiện (`condition`) trở về `false` — không
  cần ACK. Áp dụng cho phần lớn mã lỗi Warning và cả `EmergencyTemperature` (112).
- **Latching:** cần `condition=false` **VÀ** `acknowledge()` được gọi (người vận
  hành bấm ACK trên HMI/web) — thiếu 1 trong 2 điều kiện thì lỗi vẫn "active".
- **205 (đặc biệt):** không dùng cờ `latching` chuẩn — có điều kiện xoá riêng biệt
  hoàn toàn nằm ngoài cơ chế ACK thông thường (`updateTestMode()`).

---

## 17. Bảng tổng kết cuối cùng

| Hạng mục | Đã có | Chưa có | Có một phần | Ghi chú |
|---|---|---|---|---|
| LCD recovery | ✔ (detect + retry + bus recovery + re-init) | ✔ (E-code khi không phục hồi được) | | Phục hồi kỹ thuật tốt nhưng thiếu kênh cảnh báo ra ngoài |
| EEPROM recovery | ✔ | | | CRC + dual-slot + reconnect + verify 2 lần + legacy schema fallback — đầy đủ nhất trong số 4 nhóm |
| RTC recovery | | | ✔ | Tự sửa có điều kiện (cần "đồng hồ bóng" còn sống); **không có** cảnh báo pin sắp yếu |
| Sensor recovery | ✔ (mất kết nối/CRC/range/plausibility) | ✔ (kiểm tra "đứng yên"/stuck) | | Thiếu riêng phần phát hiện giá trị đứng yên dài hạn |
| Communication recovery | ✔ | | | Backoff vô hạn, không có trạng thái "thất bại vĩnh viễn" |
| Watchdog recovery | | | ✔ | Có Task WDT + Supervisor, nhưng hành động DUY NHẤT là reset cứng toàn hệ thống, không "chữa tại chỗ"; HMI/Network task không có giám sát hành động tương đương |
| Auto fault recovery | ✔ | | | Sensor/EEPROM/RTC tự phục hồi và tự xoá cảnh báo không cần ACK khi không latching |
| Manual fault handling | ✔ | | | Interlock rõ ràng qua `faultDescriptor`, công tắc vật lý có debounce |

## Kết luận

1. **Firmware hiện tại đã tự phục hồi được:** LCD, EEPROM, Sensor (mất kết nối
   tạm thời), RTC (có điều kiện), Wi-Fi/MQTT/Cloud Push, mẻ ấp sau mất điện/reset
   (có điều kiện an toàn).
2. **Chưa tự phục hồi được:** lỗi cơ khí đảo trứng (201-205 — luôn cần con người),
   EEPROM mất hẳn (301), reset bất thường (303), xung đột output (304), lỗi nhật
   ký an toàn (314), và mọi trường hợp task điều khiển bị "treo" (chỉ có 1 phản
   ứng: reset cứng, không phải phục hồi tại chỗ).
3. **Chỉ báo E-code ngay, không có bước tự thử ở giữa:** 102, 111, 112, 120, 121,
   130, 134, 201, 204, 303, 304, 314 (xem chi tiết PHẦN 8).
4. **Cần người vận hành can thiệp (nhưng không cần kỹ thuật viên):** phần lớn ACK
   cho lỗi latching sau khi đã tự kiểm tra nguyên nhân bên ngoài (dây, đầu nối,
   công tắc), theo đúng phạm vi PHẦN 15.
5. **Cần kỹ thuật viên:** 301, 303 (nếu lặp lại), 304, 314, và 205 nếu Test Mode
   xác nhận vẫn thất bại.
6. **Lỗi được phát hiện nhưng xử lý chưa đầy đủ theo đúng nghĩa (điểm cần lưu ý
   khi vận hành, không phải bug code):** `EmergencyTemperature` (112) tự xoá cảnh
   báo ngay khi nhiệt hạ, không bắt buộc ACK — dù đây là mức độ nghiêm trọng nhất
   trong hệ thống fault. Task HMI có heartbeat được theo dõi nhưng không có hành
   động phục hồi tương ứng khi "treo".

---

*Tài liệu được tạo hoàn toàn dựa trên đọc source code tĩnh (static analysis), không
có bước biên dịch/nạp/chạy thử trên phần cứng thật. Mọi số liệu hằng số (ms, số lần
retry...) lấy trực tiếp từ `config.h` tại thời điểm viết tài liệu này; nếu firmware
được cập nhật sau này, cần đối chiếu lại các hằng số trước khi in ấn tài liệu chính
thức.*
