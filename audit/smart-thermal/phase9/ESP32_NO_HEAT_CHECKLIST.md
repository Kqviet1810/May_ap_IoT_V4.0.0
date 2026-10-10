# Checklist đo trên chip ESP32-S3 — KHÔNG CẤP NHIỆT

**Trạng thái: kế hoạch, chưa có phép đo nào được thực hiện.** Mọi mục dưới đây đều là HARDWARE REQUIRED. Không nạp firmware nào trong phiên làm việc này;
người vận hành tự build và tự nạp sau khi bạn quyết định. Mọi ngưỡng "đạt" ghi (đề xuất) là đề xuất để bạn duyệt, không phải hằng số của firmware.

## 0. Điều kiện an toàn bắt buộc (trước mọi phép đo)
| # | Điều kiện | Cách kiểm |
|---|---|---|
| 0.1 | **Phía tải của SSR và contactor an toàn được tháo/hở**: điện trở sưởi không có điện (cầu dao nhánh sưởi khóa ở OFF, gắn thẻ). Chỉ phần điều khiển (3.3 V/24 V cuộn dây) được cấp | đo thông mạch + ảnh |
| 0.2 | Thermostat cơ độc lập và cầu chì nhiệt giữ nguyên trong mạch (không bắc cầu) | ảnh + ghi số hiệu |
| 0.3 | Buồng không có trứng, không có người ở gần phần cơ khí quay (đảo trứng) khi thử đầu ra đảo | kiểm bằng mắt |
| 0.4 | Bản nạp xác định được: SHA commit + giá trị `MAYAP_SMART_THERMAL` (0 hoặc 1) + cờ build ghi trong biên bản. Hai bản được dựng cùng SHA, chỉ khác cờ | tên file + SHA-256 của `.bin` |
| 0.5 | Không bật Smart Thermal trên máy thật: bản cờ=1 chỉ chạy trong khuôn khổ checklist này, tải sưởi vẫn hở | – |

## 1. Phần cứng đo cần có
Máy phân tích logic hoặc oscilloscope ≥ 4 kênh (GPIO1 SSR, GPIO14 heat-master, GPIO13 quạt hút, GPIO21 quạt tuần hoàn), nguồn lab có đo dòng cho ESP32, USB-serial ghi log,
bộ đo điện áp 3V3, nút reset ngoài, bộ cấp nguồn có thể ngắt ngẫu nhiên (relay hẹn giờ), đầu dò tham chiếu cạnh SHT30.

## 2. Số đo có sẵn trong firmware (không cần sửa mã)
Khi `MAYAP_DIAGNOSTIC_SERIAL=1` (mặc định, `config.h:53`) firmware in mỗi `TASK_STACK_MONITOR_MS` = 60 s:
```
[TASK] stack ctrl=… hmi=… sup=… net=… mqtt=… cloud=… ota=… loop=… bytes ctrl=<last>/<max>us hmi=<last>/<max>us
[HEAP] free=… min=… largest=…
[TLS-GUARD] contexts_max=… overlap_violations=… overlap_denied=… mqtt_resident=…
```
Lệnh serial (gõ `SERIAL` để bật): `HEAP`, `POWER` (lý do reset, bộ đếm reset-storm, journal NVS), `STATUS`, `FAULT LIST`, `LOG`, `DIAG FAST`.
Hằng số tham chiếu: chu kỳ tác vụ điều khiển `CONTROL_TASK_PERIOD_MS` = 5 ms; ngưỡng chu kỳ chậm `CONTROL_CYCLE_TRIP_US` = 400 000 µs; ngăn xếp điều khiển 8192 B, HMI 10240 B, supervisor 4096 B,
network 8192 B, MQTT/cloud/OTA 12288 B; TWDT 5 s; cổng heap TLS: free ≥ 73728 B và khối lớn nhất ≥ 24576 B.

## 3. Các phép đo
Mỗi phép đo chạy **hai lần: bản cờ=0 và bản cờ=1** cùng SHA, cùng bo, cùng cấu hình; đạt khi bản cờ=1 không tệ hơn bản cờ=0 ngoài dung sai ghi ở cột "đề xuất".

### 3.1 Thời gian thực của vòng điều khiển
| # | Phép đo | Cách làm | Đề xuất tiêu chí đạt |
|---|---|---|---|
| T1 | Chu kỳ điều khiển (last/max µs) khi nhàn rỗi, 30 phút | đọc `[TASK] … ctrl=<last>/<max>us` | max cờ=1 ≤ 1.2 × max cờ=0 và ≪ 400 000 µs; ghi cả hai |
| T2 | Jitter chân ra: chu kỳ 5 ms thật sự đo bằng analyzer trên một chân do tác vụ điều khiển lái (ví dụ GPIO14 khi bật/tắt thủ công trong thử nghiệm cho phép) hoặc trên log `ctrl last` | analyzer, ≥ 10 000 chu kỳ | không có chu kỳ vắng mặt ≥ 400 ms; phân phối ghi lại (P50/P99/max) |
| T3 | Chi phí tính toán của Smart Thermal: chạy kịch bản cấp-lệnh-sưởi giả lập (SP cao hơn nhiệt buồng nhiều độ; SSR **hở tải**) để vào pha FullHeat → Approach, rồi ép các nhánh learner bằng cách đưa đầu dò ấm tạm bằng nguồn nhiệt bên ngoài (bình nước ấm) | so `ctrl max` giữa hai bản trong cùng kịch bản | chênh `ctrl max` ≤ 100 µs hoặc ≤ 20 % (đề xuất, cần duyệt) |
| T4 | Chu kỳ điều khiển khi Wi-Fi/MQTT/HTTPS hoạt động mạnh, HMI quay encoder liên tục, ghi NVS (lưu profile) | cùng log, tương quan thời điểm | không vượt `CONTROL_CYCLE_TRIP_US`; `controlTripCycleCount` = 0 |
| T5 | Mất Wi-Fi, router khởi động lại, reconnect liên tục (đã có mô phỏng host) | tắt/bật AP 20 lần | vòng điều khiển không đổi; không reset |

### 3.2 Bộ nhớ và ngăn xếp
| # | Phép đo | Tiêu chí (đề xuất) |
|---|---|---|
| M1 | `[TASK] stack ctrl` còn lại sau 24 h chạy với đủ tải mạng/HMI | ≥ 25 % ngăn xếp (≥ 2048 B với 8192 B); bản cờ=1 không thấp hơn bản cờ=0 quá 256 B |
| M2 | `[HEAP] free/min/largest` sau 24 h và sau 10 lần OTA thử + reconnect | min ≥ 73728 B hoặc ≥ mức gate TLS hiện hành; largest ≥ 24576 B; không xu hướng giảm đều (rò rỉ) |
| M3 | Kích thước build: **đã đo bằng CI** (xem mục 4) | – |
| M4 | Bộ nhớ NVS/EEPROM: số lần ghi profile/giờ khi cờ=1 chạy giữ SP 2 h không tải sưởi (với đầu dò ở nhiệt phòng) | số lần ghi ≤ giới hạn đã duyệt trong `thermal_profile_storage.h`; không ghi khi dữ liệu không đổi |

### 3.3 Watchdog, reset và nguồn
| # | Phép đo | Cách làm | Tiêu chí |
|---|---|---|---|
| W1 | TWDT: chặn tác vụ điều khiển 6 s bằng công cụ gỡ lỗi (build thử nghiệm riêng, không phải build giao) | quan sát reset và đầu ra | đầu ra về an toàn trước/trong lúc reset; `POWER` báo đúng lý do |
| W2 | Mất điện ngẫu nhiên ≥ 200 lần (10 ms–5 s), kể cả đúng lúc ghi NVS | relay hẹn giờ ngẫu nhiên | không lần nào GPIO1/GPIO14 lên mức "ON" trước khi firmware cho phép; journal NVS không hỏng; hồ sơ A/B+CRC khôi phục |
| W3 | Brown-out: hạ 3V3 chậm bằng nguồn lab | quan sát | reset sạch, không đầu ra treo |
| W4 | Trạng thái chân khi reset/boot: GPIO1, GPIO14 ở mức an toàn từ lúc cấp điện đến khi chương trình chạy (strapping, pull-down ngoài) | analyzer từ t = 0 | OFF liên tục (ghi thời gian từ cấp điện đến điều khiển được) |

### 3.4 Đầu ra và đường an toàn (không sưởi)
| # | Phép đo | Tiêu chí |
|---|---|---|
| S1 | Thứ tự và độ trễ: lệnh SSR OFF → heat-master rời sau 120 ms khi High/Emergency (ép bằng đưa đầu dò ấm vượt 38.2 °C/39.0 °C **bằng nguồn nhiệt ngoài**, tải sưởi hở) | khớp thiết kế: SSR OFF trước, master nhả sau ≈ 120 ms, hai quạt chạy; clear sau trễ 10 s / 0.2 °C |
| S2 | Rút/chập/mở cảm biến SHT30, cảm biến kẹt giá trị | SSR OFF + master nhả ≤ 1 chu kỳ; quạt theo logic hiện có |
| S3 | Số lần đóng cắt của GPIO1/GPIO14 trong 1 giờ lệnh sưởi giả lập | ghi lại; so với giới hạn 1800/giờ của `OutputArbiter` (cảnh báo) |
| S4 | Heater-master: pickup 500 ms, min OFF 3000 ms | analyzer, ≥ 100 lần |

### 3.5 Cảm biến và bus
| # | Phép đo | Tiêu chí |
|---|---|---|
| B1 | SHT30: độ phân giải thực, nhiễu ở nhiệt phòng 30 phút (σ, bước lượng tử) – thông số đầu vào của mọi mô phỏng | ghi σ và bước; đối chiếu `resolution` 0.1/0.01 đã mô phỏng |
| B2 | Tuổi mẫu cảm biến và thời gian từ mẫu đến quyết định đầu ra (end-to-end) | ghi lại; so với giả định mô phỏng |
| B3 | Va chạm bus I²C/RS485 khi HMI + Wi-Fi tải cao | không lỗi bus kéo dài; không `sensorUsable=0` kéo dài |

## 4. Đã có sẵn từ CI (SOFTWARE VERIFIED, chưa phải đo trên chip)
Build thật bằng arduino-cli (core ESP32 3.3.11) trên commit `ef58910`, từ log của job `build`:

| Biến thể | Flash | RAM tĩnh | So với bản mặc định |
|---|---|---|---|
| mặc định (không cờ) | 1 432 549 / 1 480 000 B | 160 904 / 164 000 B | – |
| `MAYAP_SMART_THERMAL=0` (tường minh) | 1 432 549 B | 160 904 B | +0 B / +0 B (đồng nhất kích thước với mặc định) |
| `MAYAP_SMART_THERMAL=1` | 1 432 601 B | 160 904 B | +52 B Flash / +0 B RAM |

Dư địa: Flash 47 451 B (cờ 0) và 47 399 B (cờ 1); RAM 3 096 B. Kích thước là bằng chứng ngân sách, **không** phải bằng chứng thời gian thực hay ngăn xếp.

## 5. Điều kiện để đề xuất bước tiếp theo
Chỉ khi T1–T5, M1–M2, W1–W4, S1–S4 đạt trên **cả hai bản** thì mới đề xuất giai đoạn 1–2 của `COMMISSIONING_TEST_PLAN.md` (cấp nhiệt có hạn chế) cho bản cờ=0. Cờ=1 chỉ được đề xuất sau khi mục
"BLOCKED/PENDING" trong `FINAL_ACCEPTANCE_REPORT.md` được bạn duyệt.
