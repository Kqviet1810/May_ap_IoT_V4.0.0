# MAYAP V4 — Ổn định kết nối realtime (firmware 1.1.4 / Web 1.1.9): lỗi đã chứng minh, sửa theo 4 lô, bằng chứng, nghiệm thu

Phiên bản: firmware **1.1.4**, Web **1.1.9**, broker (Durable Object) có thay đổi → triển khai **broker → Web → nạp firmware 1.1.4**.
Trạng thái: **host/workerd/Chromium đã chạy; trên mạch ESP32 thật `NOT TESTED`** (mục 8). CI xanh không phải bằng chứng phần cứng.
Giữ nguyên: chỉ ESP32 + Web + Cloudflare (không broker bên thứ ba); không đổi điều khiển nhiệt/SSR/đảo/quạt/HMI/an toàn; TLS `contexts_max=1`, `overlap=0`.

## 1. Bằng chứng gốc (log mạch 1.1.3, hai lần chạy)

Thiết bị có **hai chế độ** trong cùng một lần chạy: ~10 phút đầu rớt link 6 lần (tổng 198,8 s, 32 % thời gian), 35 phút sau 0 lần dù có lúc bật/tắt đèn 109 lần/phút. Đường truyền chỉ gây ra **bốn lần ghi đứng 5 s**; mã biến mỗi lần đó thành 20–98 s:

| Lần mất link | Thời gian | Giải thích bằng mã (khớp từng giây với hằng số) | Mục sửa |
|---|---|---|---|
| 119 → 217 s | 98,4 s | ghi đứng 5 s → lõi đóng socket; chờ cố định 15 s; một lần nối thất bại (bước 30 s); supervisor REINIT giả → `Recover` phạt lần hai (bước 60 s) | S1 S2 S3 |
| 378 → 400 s | 22,3 s | ghi đứng 5 s + chờ 15 s + bắt tay 6,8 s | S1 S2 |
| 504 → 528 s | 24,4 s | như trên | S1 S2 |
| 528 → 548 s | 20,6 s | **REINIT giả giết link khỏe vừa nối 57 ms** | S3 |
| 578 → 597 s | 19,7 s | ghi đứng 5 s + chờ 15 s + bắt tay | S1 S2 |
| 600 → 613 s | 13,4 s | **kiểm tra firmware đầu tiên (10 phút sau khi bật nguồn) đóng MQTT để mở HTTPS** | S4 |

Cũng từ log: `minEver` heap rơi 130 → 25 KB trong 111 s thao tác (ngưỡng tự đóng MQTT 16 KB, chưa kích hoạt); `refusedBulk` 0 → 403 trong 2 phút khi chỉnh cấu hình (báo cáo `config/reported` bị cắt dở và dựng lại từ đầu); Wi-Fi lên mạng 29,8 s thay vì 7,9 s mà không có lý do ngắt trong log.

**Chưa chứng minh (nói thẳng):**
* Vì sao ~10 phút đầu đường truyền xấu (RTT 1,0–1,3 s) rồi tốt lên. Giả thuyết "ISP giờ cao điểm" khớp dữ liệu nhưng **chưa được kiểm chứng**; ba phép thử không cần code đã đề xuất (khởi động nguội lúc khác giờ, `ping -t` tới ESP32, hotspot 4G). Bản 1.1.4 **không giả định** nó đúng: nó làm cho cùng một cú đứng 5 s không còn giết link và nối lại trong giây, và in đủ dữ liệu để phân xử lần sau.
* REINIT giả được **tái hiện bằng test host** (`beat` mới hơn `now` 5 ms → `Reinit` cho service khỏe). Việc đó gây đúng hai sự kiện 145,961 s và 528,311 s vẫn là suy luận; log mới (`silent=… now= beat=`) sẽ chốt.
* Đã **rút lại** hai giả thuyết trước: CPU bắt tay P-384 (bắt tay trọn gói chỉ 2,3–2,7 s khi đường tốt) và nhiễu relay (109 lần bật/tắt mỗi phút không rớt).

## 2. Lô 1 — Kết nối firmware

| # | Thay đổi | File | Test |
|---|---|---|---|
| S1 | **TX không chặn.** Không còn `net.write()` chờ cả `socket_timeout` rồi lõi đóng socket. Trước mỗi ghi kiểm tra `select()` ghi được trên `net.fd()`. Ba lớp lưu lượng: **Control** (PUBACK/PINGREQ/PONG — xếp hàng 6 gói ≤96 B, ghi khi socket thông), **Reliable** (từ chối, nơi gọi giữ và thử lại), **Droppable** (snapshot/log — bỏ). Link chỉ coi là chết khi **không ghi được ≥25 s và broker im ≥10 s** (`tx-stall`), không phải 5 s | `mqtt_transport.h` (`Tx`, `txReadyProbe`, `sendFrame`, `flushControl`, `serviceTxStall`) | `mqtt-transport.cpp` mục 20: socket đầy cửa sổ không treo task, không giết link |
| S2 | **Nối lại tính bằng giây.** Thang "equal jitter": nửa đến đủ của trần 1/2/4/8/15/30/60 s (lần đầu **0,5–1 s** thay vì 15 s cố định). Reset thang chỉ sau khi link sống ≥30 s (`STABLE_UP_MS`) → link nối được rồi chết ngay vẫn leo thang, không đập broker. Dùng `millis()` mới tại thời điểm lỗi (trước đây `now` cũ) | `mqtt_transport.h` (`Retry`, `onStable`) | mục 15 (viết lại), 7, 9 |
| S3 | **REINIT giả.** `ServiceWatch` tính tuổi bằng số không dấu: `beat` đọc sau `now` → tràn ~4,29 tỷ → "stale". Nay tính hiệu có dấu (`silence()`), âm = 0. `mayapMqttTransportRecover` nối lại ngay và **không phạt** (supervisor yêu cầu không phải lỗi mạng). Log `[SERVICE-RECOVERY] … silent= now= beat=` | `runtime_recovery_policy.h`, `service_recovery.h`, `mqtt_transport.h` | `runtime-recovery.cpp` (mới: `beat` trước/sau `now`, quanh rollover), `mqtt-transport.cpp` mục 9 |
| S4 | **Kiểm tra firmware không còn đóng MQTT khi Web đang dùng.** `MayapFirmwareCheck::busyProbe()` (Web có phiên realtime) → hoãn, tối đa 6 giờ quá hạn (`MAX_DEFER_MS`) để tab mở nhiều ngày không chặn vĩnh viễn | `firmware_check_policy.h`, `transaction_bridge.h` | `firmware-check.cpp` |
| S5 | **DNS đúng.** `WiFi.hostByName` trả mã lỗi khác 0 khi thất bại — lõi cũ vẫn quay số `0.0.0.0` và báo "Generic error". Nay: thất bại → log `dns failed`, quên IP; thành công → nhớ IP, truyền hostname riêng cho SNI/chứng chỉ (`net.connect(ip, port, host, CA, …)`), bỏ pha DNS ở lần nối sau. `setConnectionTimeout(10000)`, `setHandshakeTimeout(20)` chỉ để chống treo | `mqtt_transport.h` (`connectClient`) | mục 22 |
| S6 | **Log chẩn đoán** (mục 7) và chống spam `FALLBACK` (≥5 s hoặc đổi nguyên nhân) | `mqtt_transport.h`, `cloud_alert_link.h`, `network_service.h` | mục 23 |

## 3. Lô 2 — Giảm tải và heap

* **`config/reported` một báo cáo mỗi lần lưu, chịu được cửa sổ đầy.** Trạng thái báo cáo có thể **tiếp tục** (`resumable`): bỏ qua phần đã gửi, kiểm tra còn chỗ (`bulkHasRoom` / `bulkSlotsFree`) **trước** khi dựng JSON; gom các lần lưu liên tiếp trong 300 ms (`CONFIG_SETTLE_MS`). Hết "dựng 38 trường rồi bỏ" → bớt cả CPU lẫn phân mảnh heap.
* **Ack "received" mang tính thông tin**: khi cửa sổ QoS1 ≥6/8, bỏ chúng (đếm vào `shedAck`), **không bao giờ bỏ ack kết thúc** (APPLIED/REJECTED). Mỗi lần bật/tắt đèn bớt 1 PUBLISH QoS1 khi nghẽn.
* Spam `FALLBACK` 10 Hz lúc bật máy: giới hạn tốc độ.
* File: `transaction_bridge.h`, `mqtt_transport.h`, `cloud_alert_link.h`. Test: `mqtt-transport.cpp` mục 21; các test `runtime-transactions`, `runtime-online-isolation` giữ nguyên PASS.

## 4. Lô 3 — Broker và Web

* **Broker: presence có ân hạn (`PRESENCE_GRACE_MS`, mặc định 75 s).** Thiết bị đóng socket đột ngột → broker **không** phát `offline` ngay mà phát retained `presence {online:false, state:"reconnecting", since, graceUntil}` và đặt DO alarm; nếu thiết bị quay lại trong ân hạn thì `offline` **không bao giờ** được công bố; hết hạn mà vắng thì mới phát `offline` + LWT thật. DISCONNECT chủ động vẫn tức thì. `PRESENCE_GRACE_MS=0` tắt tính năng. Alarm DO hợp nhất với hạn chót ân hạn (không thêm lần đánh thức cố định). File: `cloudflare/src/broker/broker-do.js`; hợp đồng: `doc/MQTT_CONTRACT.md`.
* **Web: trạng thái "KẾT NỐI LẠI"** (không phải "NGOẠI TUYẾN") giữ dữ liệu cuối cùng, khoá các form chỉnh nhiệt/cấu hình (không có "thành công giả"), nhưng **không khoá nút đèn**: một cú bấm được **giữ tối đa 8 s** (`HELD_COMMAND_MS`) và gửi ngay khi máy trở lại; hết 8 s thì bỏ với thông báo rõ ràng. Hết ân hạn → "NGOẠI TUYẾN", nút khoá. File: `app.js`, `index.html`, `sw.js`.
* Tương thích: Web cũ + broker mới → vẫn thấy offline như trước (không có trạng thái mới); Web mới + broker cũ → không có `state` nên cũng offline. Không có kết hợp nào hỏng.

## 5. Lô 4 — Khởi động nhanh

* `NETWORK_CONNECT_TIMEOUT_MS` 20 s → **12 s**; thêm `NETWORK_DRIVER_GAVE_UP_MS` (4 s): khi driver đã báo `STA_DISCONNECTED` của chính lần thử này thì không chờ hết timeout. Thử lại nhanh hơn, không đổi thuật toán phục hồi sâu.
* `STA_STABLE_MS` 10 s → **3 s** trước khi bắt tay TLS (vẫn chặn Wi-Fi chớp tắt).
* Log lý do ngắt Wi-Fi `[WIFI] sta disconnected reason=%u rssi=%d` (đăng ký `WiFi.onEvent` một lần) → lần chậm 29,8 s tới sẽ có nguyên nhân.
* File: `config.h`, `network_service.h`, `mqtt_transport.h`.

## 6. Không làm — và vì sao

| Hạng mục | Lý do |
|---|---|
| Hộp thư lệnh ở broker (TTL, giao lại khi thiết bị nối lại) | Lệnh là *toggle* và có liên quan an toàn: giao muộn có thể đảo ngược ý định người dùng. Thay bằng cú bấm đèn được giữ 8 s ở Web (người dùng còn nhìn màn hình). Cần quyết định riêng nếu muốn mở rộng |
| Lưu BSSID/kênh Wi-Fi để nối nhanh | Bằng chứng diễn đàn Espressif/arduino-esp32: lợi ích không ổn định, rủi ro kẹt AP cũ khi đổi router. Chưa có số đo trên mạch |
| Gộp lần lưu cấu hình xuống flash (debounce) | Nằm trong `MachineController` thuộc phạm vi được bảo vệ (`runtime-preservation.json`) và đổi ngữ nghĩa lưu; không thuộc yêu cầu "không đụng nhiệt/an toàn" |
| Lọc cảnh báo "SENSOR LOST" tự xoá trong 55 ms lúc bật máy | Cần bạn quyết định (câu hỏi đã nêu, chưa có trả lời) |
| TLS session resumption | Chỉ có ý nghĩa nếu đo trên mạch thấy CPU là phần lớn; dữ liệu hiện tại nói ngược lại |
| Xử lý phần cứng (snubber/ferrite/dời anten) | Không có bằng chứng cần thiết sau khi yếu đi giả thuyết relay; chỉ làm nếu sau 1.1.4 vẫn còn `tx-stall` |

## 7. Log chẩn đoán mới — cách đọc

| Dòng log | Ý nghĩa |
|---|---|
| `[MQTT] link lost reason=<tx-fail\|frame\|protocol\|socket-closed\|read-fail\|broker-close\|no-puback-silent\|tx-stall\|silent> (tx= fatal= open= inflight= rxAge= brokerClose=<code> '…' refused= dropped= ctrlQ= stall= up=)` | Lý do thật của mỗi lần mất link; `tx-stall` = đường lên đứng ≥25 s + broker im ≥10 s |
| `[MQTT] wss+mqtt up <ms> … dns= tls= ws= mqtt=` | Thời gian từng pha → trả lời "DNS, TLS, WS hay MQTT chậm" |
| `[MQTT] dns failed host=… after …ms` / `[MQTT] tls connect failed … err= ip= dns= tls= heap= largest=` | Lỗi bắt tay có đủ ngữ cảnh |
| `[MQTT-TX] refused= dropped= ctrlQueued= shedAck= writeMax= loopMax= heapLow= rssi=` | Sức khỏe đường lên mỗi chu kỳ thống kê; `writeMax`/`loopMax` bắt cú đứng, `rssi` thay cho phỏng đoán Wi-Fi |
| `[MQTT-STAT] closes radio= mem= cloud-tls= iso= lost= \| qos1 expired= refusedBulk= refusedAck= \| probes= answered= rttEwma=` | Tổng hợp đóng/mất; `cloud-tls` phải **không tăng** khi Web đang dùng |
| `[SERVICE-RECOVERY] <svc> <action> silent=…ms now=… beat=…` | Chốt giả thuyết REINIT giả |
| `[WIFI] sta disconnected reason=<n> rssi=<dBm>` | Lý do Wi-Fi ngắt (mã theo `wifi_err_reason_t`) |

## 8. Kết quả kiểm thử (host/workerd/Chromium — KHÔNG phải phần cứng)

| Bộ test | Kết quả |
|---|---|
| `tests/mqtt-transport.cpp` (host, ASan+UBSan, -Werror) — 23 mục, gồm TX không chặn, thang nối lại, DNS, shed ack, probe vòng lặp | PASS |
| `tests/runtime-recovery.cpp`, `boot-policy.cpp`, `turn-schedule.cpp`, `firmware-check.cpp` | PASS |
| `tools/test_runtime_buses.py --sanitize` (network, alert, fallback, transactions, online isolation…) | PASS (sau khi bổ sung `staDisconnectAt` vào khung `runtime-network.cpp`) |
| `tools/test_wifi_state.py`, `test_turning_scheduler.py`, `test_single_eeprom.py`, `test_thermal_control.py` (--sanitize) | PASS (không động tới nhiệt) |
| `node --test tests/*.test.cjs` | **231/231** (gồm 6 test GRACE mới của broker) |
| `tools/e2e/firmware_wss_interop.cjs` (firmware transport thật ↔ DO broker workerd) | **9/9** (mất link đột ngột = `reconnecting`, nối lại trong ân hạn thì không bao giờ `offline`; 40 lệnh → 40 ack) |
| `tools/e2e/run_mvp.cjs` (Chromium + broker thật + giả lập thiết bị) | **18/18** (W1b ân hạn: nút đèn dùng được, form khoá, cú bấm giữ được giao khi trở lại; W1e hết ân hạn → offline; W6 gia hạn token 0/332 mẫu không online) |
| `tools/e2e/uplink_resilience.cjs` (Worker chậm/chết, 20 thiết bị) | **18/18** (20 thiết bị: 40/40 cảnh báo đến Worker đúng một lần, lời gọi Worker bị chặn trên) |
| `tools/test_web_experience.cjs` (Chromium) | PASS |
| `check_release_sync`, `check_attiny_protocol`, `check_reliability`, `git diff --check` | OK |

**Chưa thể chạy cục bộ:** biên dịch firmware thật (không có `arduino-cli`). Các API lõi dùng mới — `WiFi.hostByName`, `NetworkClientSecure::connect(IPAddress, port, host, CA, …)`, `WiFi.onEvent`/`arduino_event_info_t`, `select()` trên `fd()` — đã đối chiếu mã nguồn lõi 3.3.1; **CI `build-firmware.yml` (esp32 core 3.3.11) là bước xác nhận biên dịch**.

## 9. Nghiệm thu trên mạch (tất cả `NOT TESTED` cho tới khi bạn nạp và chạy)

| # | Phép đo | Đạt khi | Trạng thái |
|---|---|---|---|
| H1 | Nạp 1.1.4, chạy **qua mốc 10 phút** với Web mở | Không có `closed on purpose (cloud-tls)` ở ~600 s; `[MQTT-STAT] cloud-tls=0`; Web không rớt | NOT TESTED |
| H2 | Bật/tắt đèn liên tục (từ Web) 5 phút | Không `link lost`; `ctrlQ`, `refused` thấp; `shedAck` có thể tăng, ack kết thúc không mất | NOT TESTED |
| H3 | Chặn đường lên 10–15 s (AP lọc gói hoặc proxy trễ) | Không `link lost`; sau 25 s im lặng mới `reason=tx-stall` | NOT TESTED |
| H4 | Ngắt AP 5 s rồi bật lại | Nối lại <15 s; Web "KẾT NỐI LẠI" rồi online; **không** có "NGOẠI TUYẾN" nếu <75 s | NOT TESTED |
| H5 | Reset nguội → thời gian tới `wss+mqtt up` | Ghi `dns= tls= ws= mqtt=` và Wi-Fi lên; so với 42 s trước đây | NOT TESTED |
| H6 | 72 giờ chạy, 20 thiết bị | Không `[SERVICE-RECOVERY] … REINIT` giả (`silent` luôn ≥ timeout); không vượt hạn mức Cloudflare Free | NOT TESTED |
| H7 | Chỉnh SV liên tục 2 phút | `refusedBulk` không bão; heap `minEver` không rơi sát 16 KB | NOT TESTED |
| H8 | `ping -t` tới ESP32 trong 15 phút đầu (đối chiếu `[MQTT-TX] rssi=` và `[WIFI] sta disconnected reason=`) | Phân xử Wi-Fi vs Internet cho giả thuyết ISP giờ cao điểm | NOT TESTED |

**Dự đoán có thể bác bỏ:** nếu trong 1.1.4 ở mốc ~600 s vẫn thấy `closed on purpose (cloud-tls)` khi Web đang mở, thì S4 sai ở đâu đó (busy-probe không bắt được) và tôi cần log đó.

## 10. Triển khai và rollback

1. Deploy **broker** (`wrangler deploy -c wrangler-broker.toml`) — tương thích ngược; đặt `PRESENCE_GRACE_MS` nếu muốn khác 75 s.
2. Deploy **Worker + Web** (asset 1.1.9; Service Worker đổi cache).
3. Nạp **firmware 1.1.4** (không đổi định dạng cấu hình/EEPROM/giao thức ATtiny).
4. Rollback: nạp lại 1.1.3; broker/Web mới vẫn chạy với firmware cũ (broker chỉ thêm ân hạn; Web hiển thị "KẾT NỐI LẠI" khi broker có `state`).

Không deploy production nếu chưa có xác nhận của bạn; không thay đổi secret.

## 11. Nguồn tham khảo đã đối chiếu

* arduino-esp32 issue #5398 và PR #11865 (hành vi ghi chặn/`socket_timeout`, đóng socket khi ghi thất bại).
* Diễn đàn Espressif về BSSID/kênh cache khi kết nối Wi-Fi (kết luận: không chắc có lợi → không làm).
* Tài liệu AWS CRT MQTT5 / hướng dẫn "exponential backoff and jitter" (equal jitter, reset sau khi ổn định).
* Tài liệu Cloudflare Durable Objects: vòng đời, WebSocket Hibernation, alarm (ân hạn presence bằng alarm thay vì bộ đếm).
* Mã nguồn lõi arduino-esp32 3.3.1: `NetworkClientSecure::fd()`, `send_ssl_data`, `hostByName`, TCP_NODELAY.
