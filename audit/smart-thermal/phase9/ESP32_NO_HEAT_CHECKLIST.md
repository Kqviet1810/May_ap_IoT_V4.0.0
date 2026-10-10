# Checklist đo trên chip ESP32-S3 — KHÔNG CẤP NHIỆT (bản 2, theo quyết định của người phụ trách)

**Trạng thái: kế hoạch, chưa có phép đo nào được thực hiện, chưa nạp firmware nào.** Mọi mục là HARDWARE REQUIRED.
Người vận hành tự build và tự nạp (USB). Mọi ngưỡng ghi "(đề xuất)" là đề xuất để bạn duyệt, không phải hằng số của firmware.
Thay đổi so với bản 1: (a) tách "mất giao tiếp" khỏi "kẹt giá trị hợp lệ"; (b) đo chu kỳ 5 ms và jitter thật, không nhầm với thời gian thực thi; (c) tách đường ngắt High và Emergency;
(d) sửa phần OTA theo cơ chế cập nhật hiện hành; (e) cô lập nguồn heater bằng thiết bị đo, bỏ mọi thử lỗi công suất lớn.

## 0. Cô lập nguồn heater — điều kiện bắt buộc, không chỉ dựa vào lệnh SSR OFF
Lệnh SSR OFF, GPIO1 ở mức thấp, hay contactor "đã nhả" **không** phải bằng chứng cô lập. Phải có cả hai lớp (a) và (b) và ghi lại số đo vào biên bản trước **mỗi** phiên đo:
| # | Điều kiện | Cách xác minh (thiết bị đo phù hợp) |
|---|---|---|
| 0.1 | Cầu dao/cách ly nguồn của nhánh sưởi ở OFF, khóa và gắn thẻ (LOTO) bởi người có chuyên môn điện | ảnh khóa + thẻ, tên người thực hiện |
| 0.2 | **Kiểm chứng không còn điện** trên đầu ra nhánh sưởi và trên cả hai phía của SSR và contactor an toàn bằng đồng hồ/bút thử điện áp đúng cấp (CAT phù hợp), theo thứ tự: thử thiết bị trên nguồn đã biết có điện → đo "không điện" → thử lại trên nguồn đã biết có điện | số đo V từng pha-pha, pha-đất, ghi vào biên bản |
| 0.3 | Dòng bằng không: kẹp dòng (clamp) trên từng dây nhánh sưởi | ghi 0 A trước và sau phiên đo |
| 0.4 | Tháo dây tải sưởi khỏi đầu ra SSR (hoặc nhấc điện trở khỏi busbar), bọc đầu dây; điện trở không có đường tới nguồn nào | ảnh, đo thông mạch hở |
| 0.5 | Thermostat cơ độc lập và cầu chì nhiệt giữ nguyên trong mạch (không bắc cầu) | ảnh + số hiệu |
| 0.6 | Chỉ cấp phần điều khiển (3.3 V/24 V) và cuộn dây contactor/relay phụ trợ. Quạt và cơ cấu đảo trứng: chỉ chạy khi không có người ở gần phần quay | kiểm bằng mắt |
| 0.7 | Mọi đầu ra công suất cao khác (siren, quạt) có thể cho chạy; **không** có đường điện nào tới điện trở sưởi | – |
| 0.8 | Bản nạp xác định được: SHA commit, giá trị `MAYAP_SMART_THERMAL` (0 hoặc 1), SHA-256 của `.bin`. Bản cờ=1 dán nhãn "SMART ON – CHỈ THỬ KHI TẢI SƯỞI ĐÃ CÔ LẬP"; **bản vận hành luôn là cờ=0** | biên bản |

Cho phép đo hai cấu hình cờ=0 và cờ=1 khi và chỉ khi 0.1–0.4 đã đạt; **không** có nghĩa Smart ON được phép điều khiển heater thật.
**Không thực hiện thử lỗi công suất lớn** (SSR dính dẫn, SSR+contactor cùng dính, ép nhiệt quá ngưỡng bằng nguồn nhiệt công suất). Phản ứng với vượt ngưỡng chỉ được thử ở mức tín hiệu: làm ấm riêng đầu dò SHT30 bằng nguồn nhiệt công suất nhỏ (ví dụ chai nước ấm ≤ 45 °C, súng hơi ấm ở xa), tải sưởi vẫn cô lập.

## 1. Thiết bị đo
Máy phân tích logic hoặc oscilloscope ≥ 4 kênh (GPIO1 SSR, GPIO14 heat-master, GPIO13 quạt hút, GPIO21 quạt tuần hoàn), USB-serial ghi log, nguồn lab đo dòng cho ESP32, nút reset ngoài,
relay hẹn giờ ngẫu nhiên để ngắt nguồn điều khiển, nhiệt kế/đầu dò tham chiếu cạnh SHT30, đồng hồ đo điện áp/clamp cho mục 0.

## 2. Số đo có sẵn trong firmware (không cần sửa mã) và những gì chúng THỰC SỰ đo
Khi `MAYAP_DIAGNOSTIC_SERIAL=1` (mặc định) mỗi `TASK_STACK_MONITOR_MS` = 60 s firmware in:
```
[TASK] stack ctrl=… hmi=… sup=… net=… mqtt=… cloud=… ota=… loop=… bytes ctrl=<last>/<max>us hmi=<last>/<max>us
[HEAP] free=… min=… largest=…
[TLS-GUARD] contexts_max=… overlap_violations=… overlap_denied=… mqtt_resident=…
```
**`ctrl=<last>/<max>us` là thời gian THỰC THI của một lần `Machine.update()`** (hiệu hai lần đọc `esp_timer_get_time()` quanh lệnh gọi; xem `MAYAP_INDUSTRIAL_v1_0_0.ino`).
Nó **không** phải chu kỳ 5 ms và **không** phản ánh jitter của lịch chạy (`vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(CONTROL_TASK_PERIOD_MS))`). Không dùng nó để kết luận về chu kỳ hay jitter.
Lệnh serial (gõ `SERIAL` để bật): `HEAP`, `POWER`, `STATUS`, `FAULT LIST`, `LOG`, `DIAG FAST`.
Hằng số tham chiếu: `CONTROL_TASK_PERIOD_MS` = 5; `CONTROL_CYCLE_TRIP_US` = 400 000 µs (ngưỡng "chu kỳ chậm", tính trên thời gian thực thi); ngăn xếp điều khiển 8192 B, HMI 10240 B, supervisor 4096 B,
network 8192 B, MQTT/cloud/OTA 12288 B; TWDT 5 s; cổng heap TLS: free ≥ 73728 B và khối lớn nhất ≥ 24576 B.

## 3. Các phép đo (mỗi phép chạy ở cả bản cờ=0 và bản cờ=1, cùng SHA, cùng bo)

### 3.1 Thời gian thực — tách "thực thi" khỏi "chu kỳ/jitter"
| # | Phép đo | Cách làm | Tiêu chí (đề xuất) |
|---|---|---|---|
| T0 | Tần số tick FreeRTOS của core đang dùng | đọc `CONFIG_FREERTOS_HZ` trong sdkconfig của core esp32 3.3.11 trong biên bản build; `pdMS_TO_TICKS(5)` chỉ bằng 5 tick khi 1000 Hz | ghi lại; nếu ≠ 1000 Hz thì dừng và báo (chu kỳ danh định 5 ms sai) |
| T1 | **Thời gian thực thi** `Machine.update()` (last/max µs) khi nhàn rỗi 30 phút | log `[TASK] ctrl=` | cờ=1 max ≤ 1.2 × cờ=0 max và ≪ 400 000 µs; ghi cả hai |
| T2 | **Chu kỳ 5 ms và jitter thật** (khoảng cách giữa hai lần bắt đầu vòng điều khiển) | Firmware sản xuất **không có chân nào đổi trạng thái mỗi vòng**, nên cần **bản đo chuyên dụng không phải bản vận hành** đảo một chân GPIO dự phòng ở đầu mỗi vòng điều khiển (hoặc ghi mốc `esp_timer`), đo bằng analyzer ≥ 10 000 chu kỳ. Bản đo này **cần bạn duyệt** vì thêm mã đo; không thêm vào bản vận hành | P50/P99/max của chu kỳ ghi lại; không có chu kỳ trễ ≥ `CONTROL_CYCLE_TRIP_US`; so sánh cờ=0/cờ=1 |
| T3 | Chi phí tính toán của Smart Thermal: chạy tình huống đòi heater (SP cao hơn nhiệt buồng nhiều độ) với tải sưởi **cô lập** để vào FullHeat → Approach; làm ấm riêng đầu dò để đi qua các nhánh learner | so T1 giữa hai bản trong cùng tình huống | chênh `ctrl max` ≤ 100 µs hoặc ≤ 20 % (đề xuất, cần duyệt) |
| T4 | Thực thi và (qua bản đo T2) chu kỳ khi Wi-Fi/MQTT/HTTPS hoạt động mạnh, HMI quay encoder liên tục, ghi NVS | cùng log / analyzer | `controlTripCycleCount` = 0; chu kỳ không bị kéo dài quá mức |
| T5 | Mất Wi-Fi, router khởi động lại, reconnect liên tục | tắt/bật AP 20 lần | vòng điều khiển không đổi, không reset |

### 3.2 Bộ nhớ và ngăn xếp
| # | Phép đo | Tiêu chí (đề xuất) |
|---|---|---|
| M1 | `[TASK] stack ctrl` còn lại sau 24 h với tải mạng/HMI | ≥ 25 % ngăn xếp (≥ 2048 B với 8192 B); cờ=1 không thấp hơn cờ=0 quá 256 B |
| M2 | `[HEAP] free/min/largest` sau 24 h có Wi-Fi/MQTT/HTTPS cloud định kỳ và lần **kiểm tra** phiên bản firmware (chỉ hỏi, không tải) | min và largest không dưới các cổng TLS hiện hành; không xu hướng giảm đều |
| M3 | Kích thước build: đã đo bằng CI (mục 4) | – |
| M4 | NVS/EEPROM: số lần ghi profile/giờ khi cờ=1 chạy giữ SP 2 h (đầu dò ở nhiệt phòng, tải sưởi cô lập) | không vượt giới hạn trong `thermal_profile_storage.h`; không ghi khi dữ liệu không đổi |

### 3.3 Watchdog, reset, nguồn điều khiển
| # | Phép đo | Cách làm | Tiêu chí |
|---|---|---|---|
| W1 | TWDT: chặn tác vụ điều khiển > 5 s trên **bản thử riêng** (không phải bản vận hành) | quan sát reset và đầu ra | đầu ra về an toàn trước/trong lúc reset; `POWER` báo đúng lý do |
| W2 | Mất điện nguồn điều khiển ngẫu nhiên ≥ 200 lần (10 ms–5 s), kể cả lúc ghi NVS | relay hẹn giờ | GPIO1/GPIO14 không lên mức "ON" trước khi firmware cho phép; journal NVS không hỏng; hồ sơ A/B+CRC khôi phục |
| W3 | Brown-out: hạ 3V3 chậm bằng nguồn lab | quan sát | reset sạch, không đầu ra treo |
| W4 | Trạng thái chân từ lúc cấp điện đến khi chạy (strapping, pull-down ngoài) | analyzer từ t = 0 | GPIO1, GPIO14 OFF liên tục; ghi thời gian từ cấp điện đến điều khiển được |

### 3.4 Cảm biến — phân biệt MẤT GIAO TIẾP với KẸT GIÁ TRỊ HỢP LỆ
| # | Tình huống | Hành vi mong đợi theo firmware hiện hành | Cách đo / tiêu chí |
|---|---|---|---|
| S1 | **Mất giao tiếp**: rút cáp/chập/hở SHT30 hoặc bus | cảm biến bị coi là mất; heater bị chặn (SSR OFF, theo logic sensor-lost) sau thời gian chờ cảm biến cấu hình (`sensorTimeoutSec` mặc định 10 s) + một vòng điều khiển | đo thời gian từ lúc mất khung đến khi GPIO1/GPIO14 về OFF bằng analyzer; so với `sensorTimeoutSec`; **không** đòi "≤ 1 chu kỳ" |
| S2 | **Kẹt giá trị hợp lệ** (khung hợp lệ, giá trị không đổi hoặc trôi chậm trong dải): ví dụ bộ giả cảm biến phát giá trị hằng | firmware **không có khả năng phát hiện trong một chu kỳ**. Cơ chế hiện có là E104 (giá trị không đổi ≥ 20 phút **và** ≥ 450 s ON thực tế) và E115 (900 s ON mà PV không lên); cả hai cần heater chạy thật | **Không có tiêu chí "phát hiện" trong thử không nhiệt.** Chỉ ghi nhận: firmware vẫn coi giá trị là hợp lệ, không báo lỗi giả. Ghi rõ đây là giới hạn của một cảm biến và là lý do bảo vệ cơ độc lập (thermostat, cầu chì nhiệt) là lớp duy nhất cho trường hợp này; không thử E104/E115 vì cần cấp nhiệt |
| S3 | Khung nhiễu/CRC sai/khung trùng lặp | theo bộ kiểm thử host `Sensor hardening` | quan sát không có `sensorUsable` giả; ghi tỷ lệ khung bị loại |

### 3.5 Đường ngắt High và đường ngắt Emergency (kích thích ở mức tín hiệu, tải sưởi cô lập)
Hai đường khác nhau; mỗi đường đo riêng, ghi thời điểm vượt ngưỡng (từ đầu dò tham chiếu) và thời điểm từng cạnh GPIO trên analyzer.
| # | Đường | Ngưỡng và hành vi theo thiết kế | Đo / tiêu chí |
|---|---|---|---|
| H1 | **High 38.2 °C** | tính trên `max(raw, filtered)`, **xác nhận 1 s**; SSR OFF trước, heat-master nhả sau ≈ 120 ms; **không chốt** (non-latching): gỡ khi giảm ≥ 0.2 °C và qua 10 s; hai quạt chạy | latency vượt ngưỡng → GPIO1 OFF → GPIO14 OFF; thời gian xác nhận ≈ 1 s; gỡ đúng điều kiện; ghi số lần master đóng lại khi dao động quanh ngưỡng |
| E1 | **Emergency 39.0 °C** | **tức thời** (không chờ xác nhận 1 s); SSR OFF, master nhả sau ≈ 120 ms; điều kiện gỡ chặt hơn: giảm ≥ 0.3 °C và qua 30 s | latency từ vượt 39.0 đến GPIO1/GPIO14; so với H1 (phải nhanh hơn H1 do không có 1 s xác nhận); gỡ đúng điều kiện |
| X1 | Thứ tự và độ trễ SSR → master ở cả hai đường | phân tích logic | khớp ≈ 120 ms ở cả hai; SSR luôn đi trước |
| X2 | Heat-master: pickup 500 ms, min OFF 3000 ms | analyzer ≥ 100 lần | khớp |
| X3 | Số cạnh GPIO1/GPIO14 trong 1 giờ lệnh sưởi giả lập (tải cô lập) | ghi lại | so với giới hạn 1800/giờ của `OutputArbiter` (cảnh báo) |
Chỉ thử bằng làm ấm riêng đầu dò. **Không** thử SSR dính, SSR+contactor dính, hay làm nóng buồng bằng nguồn công suất.

### 3.6 Cảm biến — đặc tính
| # | Phép đo | Tiêu chí |
|---|---|---|
| B1 | SHT30: σ nhiễu và bước lượng tử ở nhiệt phòng 30 phút | ghi lại; đối chiếu `resolution` 0.1/0.01 đã mô phỏng |
| B2 | Tuổi mẫu và độ trễ mẫu → quyết định đầu ra | ghi lại; so với giả định mô phỏng |
| B3 | Va chạm bus I²C/RS485 khi HMI + Wi-Fi tải cao | không lỗi bus kéo dài |

## 4. Cập nhật firmware (OTA) — theo cơ chế hiện hành
Cơ chế hiện hành (xem `ota_web_update.h`, `ota_rollback.h`, `firmware_update_guard.h`): **ArduinoOTA đã gỡ**; cập nhật từ xa **chỉ** qua HTTPS tới Cloudflare Worker; firmware **chỉ hỏi** có bản mới, **không tự tải**;
người vận hành phải tự bấm và xác nhận trên HMI; tải xuống tính SHA-256 trực tiếp, so với giá trị Worker trả về, khớp mới `Update.end(true)` rồi khởi động lại, sai hoặc đứt giữa chừng thì `Update.abort()` giữ nguyên firmware cũ;
bảng phân vùng hai slot (`default_8MB`) cho phép quay về **đúng một** bản trước đó. Nạp trực tiếp chỉ bằng USB.
Vì vậy, trong thử không nhiệt này:
| # | Mục | Quy định |
|---|---|---|
| O1 | Không thực hiện tải/ghi firmware qua mạng (không OTA thử nghiệm lặp) | nạp bản đo bằng USB, do người vận hành, sau khi bạn duyệt |
| O2 | Chỉ quan sát: chu kỳ **kiểm tra phiên bản** không tự tải; HMI hiện dòng cập nhật chỉ khi có bản mới; không có tải nếu không xác nhận | heap/stack ghi trong M1–M2 |
| O3 | Phân vùng: biên bản build ghi Partition Scheme `default_8MB` (hai slot OTA) | ghi lại; **không** thực hiện rollback thử nếu chưa được duyệt riêng |
| O4 | Điều kiện cho phép OTA thật (`mayapFirmwareMaintenanceReady`, trạng thái batch/heater) và việc OTA thật trên máy đang vận hành | **ngoài phạm vi thử không nhiệt**; chỉ kiểm khi có kế hoạch riêng được duyệt |

## 5. Đã có sẵn từ CI (SOFTWARE VERIFIED, chưa phải đo trên chip)
Số đo build thật của commit cuối nằm trong báo cáo bàn giao (mục Flash/RAM). Kích thước là bằng chứng ngân sách, **không** phải bằng chứng thời gian thực hay ngăn xếp.

## 6. Điều kiện để đề xuất bước tiếp theo
Chỉ khi mục 0 đạt ở mọi phiên và T0–T5, M1–M2, W1–W4, S1, H1/E1/X1–X3 đạt trên **cả hai bản** thì mới đề xuất giai đoạn tiếp theo của `COMMISSIONING_TEST_PLAN.md` cho bản cờ=0.
Cờ=1 điều khiển heater thật vẫn bị chặn cho đến khi điều kiện NO-GO trong báo cáo bàn giao được bạn gỡ.
