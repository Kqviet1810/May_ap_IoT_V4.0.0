# MAYAP V4 — Mất realtime khi có cảnh báo: nguyên nhân gốc, kiến trúc sửa, bằng chứng, nghiệm thu (2026-10-08)

Phiên bản: firmware **1.1.3**, Web **1.1.8**, broker + Worker (triển khai **Worker trước, broker sau**).
Trạng thái: **mô phỏng/CI/workerd đã chạy; trên mạch ESP32 thật `NOT TESTED`** (xem mục 9).
Nguyên tắc không đổi: chỉ ESP32 + Web + Cloudflare, không broker bên thứ ba; không đổi điều khiển nhiệt/SSR/đảo/quạt/HMI/an toàn.

## 1. Kết luận ngắn

Web "mất realtime rồi tự nối lại" khi có cảnh báo có **ba nguyên nhân độc lập**, tất cả đều đã tái hiện bằng test thật (không phải suy đoán):

| # | Nguyên nhân gốc | Cơ chế | Bằng chứng |
|---|---|---|---|
| R1 | **Một lần PUBACK cảnh báo quá hạn bị coi là transport hỏng** | PUBACK chờ D1 (≈2,4–2,6 s bình thường vì `recordAlarmEvent` chạy ~8 truy vấn D1 tuần tự) → quá 4 s → `MayapUplink::suspect()` → `classify()`=NoAck → critical đi HTTPS ngay → `MayapTlsOperation(Cloud)` bị từ chối vì MQTT đang giữ TLS → `mayapRequestCloudTlsYield` → mqttTask `closed on purpose (cloud-tls)` → LWT → Web offline | log mạch: `t=400688 durable_ack=0 route_to_ack=4001ms` → `t=401987 FALLBACK … cause=down` → `t=402005 [MQTT] closed on purpose (cloud-tls)`; lặp lại `t=480991 → 482105`. Test `runtime-alarm-fallback.cpp` cũ khẳng định chính hành vi này (cases 5/7) |
| R2 | **Broker đóng socket thiết bị bằng `1013 SLOW_CONSUMER` khi Web gửi một loạt lệnh QoS1** | Cửa sổ in-flight broker→thiết bị là 16; broker xử lý cả loạt nhanh hơn một RTT PUBACK của thiết bị → gói thứ 17 đóng socket. Cũng đúng chiều thiết bị→Web (cụm ACK/config chunk) | tái hiện bằng firmware thật + broker DO thật: 40 lệnh liên tiếp → `[MQTT] link lost (tx=0 … inflight=8 rxAge=0ms brokerClose=1013 'SLOW_CONSUMER')`, chỉ 8/40 ACK tới Web. Khớp `link lost (tx=1 … inflight=3)` và `ACK PUB FAIL` / `up inflight=8` trên mạch |
| R3 | **Web tự đóng socket khỏe để đổi token** | `renewDeviceChannel()` = `disconnectDeviceChannel()` rồi mới `fetch /api/mqtt-session` rồi mới nối lại (break-before-make); token chỉ được broker kiểm tra lúc CONNECT, socket đang sống không cần "đổi" | e2e Chromium + broker workerd, trước khi sửa: `W6 FAIL … 2 samples not online (cache)`; sau: `334 samples, 0 not online` |

Các lỗi phụ cùng nằm trên đường này, đã sửa:

* **Beacon HTTPS** (`beaconDue`) đủ điều kiện cả khi MQTT khỏe nhưng heartbeat PUBACK chưa về (`silentMs ≥ 90 s`) → có thể xin nhường TLS và đóng MQTT khỏe. Nay chỉ beacon khi transport **đã xác nhận hỏng** (Down/Flapping/HalfOpen).
* Broker xử lý `alarm` **trong** vòng xử lý gói (`await` chuỗi `_uplinkChain` + `fetch` Worker tới 5 s): một Worker chậm giữ lâu cả handler, và cuối `webSocketMessage` ghi đè attachment bằng bản đọc cũ (mất cập nhật `inflight/subs`). Nay không có `await` mạng trong đường xử lý gói; ghi attachment hợp nhất.
* Alarm DO cố định 15 s (≈5 760 lần/ngày/máy; 115 k/ngày cho 20 máy — vượt hạn mức DO Free chỉ riêng việc này). Nay theo hạn chót thật.
* `ACK PUB FAIL` lặp mỗi vòng (log tràn). Nay back-off 50 ms→1 s và log tối đa 1 lần/2 s; ACK có phần dự trữ trong cửa sổ QoS1.
* Sau khi HTTPS fallback xong MQTT phải chờ lease 15 s mới nối lại. Nay nhả quyền TLS ngay khi outbox rỗng.
* Beacon bị hoãn vì MQTT đóng rồi nối lại trước tick 60 s kế tiếp → không bao giờ gửi được; nay thử lại sau 500 ms.

Chưa chứng minh được (nói thẳng): vì sao trong đúng đợt log đó cửa sổ `inflight=8` bị kẹt ~10 s **mà link không rớt**. Giả thuyết còn lại: độ trễ phía Cloudflare, hoặc phía thiết bị khi heap ~35 kB. Bản sửa **không phụ thuộc** vào việc xác định điều đó (ACK được dự trữ chỗ, kẹt thì hết hạn thay vì đóng socket, back-off), và bản mới in đủ dữ liệu để biết ở lần chạy sau: `brokerClose=<code>`, `rxAge`, `[MQTT-STAT] qos1 expired/refusedAck/refusedBulk`, `probes/answered`, RTT PUBACK.

### Sơ đồ nguyên nhân R1 (trước khi sửa)

```
ESP32 alarm ──MQTT PUBLISH──► broker DO ──await fetch (≤5 s)──► Worker: ~8 truy vấn D1 tuần tự (2,4–2,6 s, có lúc >4 s) ──► durable ──► PUBACK
      │ chờ PUBACK 1,5–4 s (cloud_alert_link.h awaitUplink)          ▲ cooldown 15 s → durable:false (429) → KHÔNG PUBACK
      ▼ hết hạn
 MayapUplink::suspect()  ─► classify()=NoAck ─► decide()=Https (critical: 0 s grace)
      ▼
 postJson → MayapTlsOperation(Cloud) bị từ chối (MQTT đang giữ TLS) → mayapRequestCloudTlsYield()
      ▼
 mqttTask: "closed on purpose (cloud-tls)" → LWT presence{online:false} → Web "NGOẠI TUYẾN"/nối lại
```

Sau khi sửa: PUBACK đến từ DO trong ~20–60 ms; PUBACK trễ chỉ yêu cầu một PINGREQ thăm dò và thử lại qua MQTT; `suspect()` không còn tồn tại; HTTPS chỉ được phép khi transport đã được xác nhận hỏng độc lập.

### Mã lý do (đã phân biệt, không dùng chung một tín hiệu timeout)

| Mã yêu cầu | Nơi hiện thực / log |
|---|---|
| `TRANSPORT_DOWN` | `Cause::Down/Flapping/HalfOpen`; `[MQTT] link lost (… brokerClose=<code> '<reason>')` |
| `PUBACK_TIMEOUT` / `INGEST_FAILED` | `[CLOUD] no MQTT PUBACK … INGEST_TIMEOUT misses=N, link kept`; phía broker `uplink.last_error=HTTP_5xx/TIMEOUT/FETCH_ERROR/NO_INGEST` |
| `BACKPRESSURE` | thiết bị: `refusedAck/refusedBulk` + `[CFG-TX] ACK PUB FAIL (refused N … retry in …)`; broker: `overflow` và hàng đợi QoS1 `pend:*` |
| `CLOUD_TLS_YIELD` | `[MQTT] closed on purpose (cloud-tls) why=<alarm:link-down|alarm:link-flapping|alarm:half-open-confirmed|alarm:critical-starved|beacon:broker-unreachable|other>` |
| `INTENTIONAL_CLOSE` | `closed on purpose (radio|memory|cloud-tls|isolated)` + bộ đếm `[MQTT-STAT] closes radio/mem/cloud-tls/iso/lost` |
| `BROKER_STORED` / `D1_STORED` / `PUSH_*` | PUBACK alarm / phán quyết `stored|duplicate` của Worker / `push.sent|pending` ở `/status?alarms=1` |

## 2. Ranh giới trách nhiệm (bốn trạng thái độc lập)

| Trạng thái | Ai biết | Bằng chứng | Hậu quả |
|---|---|---|---|
| `MQTT_TRANSPORT_HEALTH` | chủ MQTT trên thiết bị | socket đóng/ghi lỗi, byte nhận từ broker (≤20 s), thăm dò PINGREQ có/không được trả lời trong 4 s, mã CLOSE của broker | **duy nhất** quyết định đóng MQTT / dùng HTTPS |
| `BROKER_ACCEPTANCE` | PUBACK của alarm | broker đã ghi dòng bền trong DO → **PUBACK = `BROKER_STORED`** | thiết bị xóa sự kiện khỏi outbox; thiếu PUBACK → giữ + thử lại qua MQTT, **không** đổi kênh |
| `ALARM_STORAGE` | hàng đợi trong DO → Worker/D1 | phán quyết từng sự kiện `stored/duplicate/throttled/rejected/lỗi` | `D1_STORED`; backlog quan sát được trên topic `uplink` |
| `PUSH_DELIVERY` | Worker (`alarm_deliveries`) | `pending/sent/gone/expired` (`/status?alarms=1`) | độc lập; "đã vào hàng đợi" ≠ "đã Push" |

## 3. Phương án A hay B

**Chọn B (DO lưu bền trước PUBACK; Worker/D1/Push bất đồng bộ có hàng đợi).**

| Tiêu chí | A: giữ PUBACK sau D1, chỉ tách trạng thái | B: PUBACK sau khi DO lưu |
|---|---|---|
| Độ đúng khi Worker/D1 chậm hoặc hỏng | thiết bị phải giữ sự kiện trong **RAM** (mất khi mất điện), phải thử lại qua MQTT mỗi vài giây | Cloudflare giữ sự kiện bền; thiết bị rảnh ngay |
| Cooldown 429 | thiết bị thử lại lặp, mỗi lần chờ 4 s | broker chờ đúng thời gian còn lại của cooldown |
| HOL | cùng chuỗi tuần tự: Worker chậm làm trễ ACK heartbeat/alarm khác | PUBACK/PINGRESP không phụ thuộc Worker (đo: PINGRESP 3 ms khi Worker 6 s/lần) |
| RAM ESP32 | outbox giữ lâu | không đổi, giải phóng sớm hơn |
| Quota Free | nhiều lần thử lại qua MQTT + HTTPS | batch ≤8 sự kiện/lần gọi Worker (đo 40 lần cho 100 sự kiện), heartbeat ≤ 1/30 s/máy |
| Độ phức tạp | thấp | trung bình (`uplink-queue.js` ~300 dòng, bền, có test) |

B bắt buộc đạt: ghi DO trước PUBACK ✔; PUBACK chỉ nghĩa `BROKER_STORED` ✔; hàng đợi bị chặn 64 + `overflow` đếm + từ chối PUBACK khi đầy ✔; retry/backoff 2/5/15/30/60 s ±20 % ✔; phục hồi sau DO restart ✔ (test T9: kill broker khi 3 sự kiện còn chờ → nối lại → tự giao); dedupe `deviceId+eventId` ✔ (DO theo thiết bị; Worker theo `event_id`); thứ tự `ACTIVE→RESOLVED` mỗi loại lỗi ✔; Worker ghi D1 xong nhưng mất phản hồi → `duplicate`, không push lại ✔ (T6); Worker hỏng kéo dài → backlog hiển thị (`uplink`) ✔ (T5); không polling dày (alarm theo hạn chót) ✔; không sự kiện mất âm thầm (`expired` sau 24 h, `rejected` có vòng ghi) ✔; **không thêm TLS/kết nối nào trên ESP32** ✔.

## 4. Thay đổi (tệp)

| Lớp | Tệp | Nội dung |
|---|---|---|
| Broker | `cloudflare/src/broker/uplink-queue.js` (mới) | hàng đợi bền, dedupe, thứ tự theo loại, phán quyết từng sự kiện, backoff + jitter, bảo trì/hết hạn, `status()` |
| Broker | `broker-do.js` | PUBACK sau khi ghi DO; drain trong DO alarm (alarm và heartbeat là hai luồng độc lập); alarm theo hạn chót; **hàng đợi QoS1 sau cửa sổ 16 (bền, 128)** thay cho đóng 1013; hợp nhất attachment; vòng đời socket Web = token + 2 phút; topic `uplink` |
| Broker | `acl.js` | topic `uplink` (broker→Web, QoS0); token trả `expiresAt` |
| Worker | `src/index.js` | `kind:"alarms"` (lô ≤8) trả phán quyết từng sự kiện; `throttled` kèm `retry_after_ms`; thiết bị chưa đăng ký = `rejected` (không retry vô hạn) |
| Firmware | `mqtt_uplink.h` | bỏ `suspect()`; thăm dò PINGREQ theo yêu cầu + bằng chứng; `YieldWhy` |
| Firmware | `alarm_fallback_policy.h` | phân loại bằng bằng chứng transport (Healthy/Probing/HalfOpen/Down/Flapping/Recovering/BrokerNotStoring); bỏ NoAck/NotStoring; beacon chỉ khi transport hỏng |
| Firmware | `cloud_alert_link.h` | quyết định tuyến **trước** khi publish; PUBACK trễ giữ sự kiện + yêu cầu thăm dò; ngoại lệ có chủ đích cho critical đói 60 s/≥3 lần; nhả TLS ngay khi xong; beacon thử lại sau 500 ms |
| Firmware | `mqtt_transport.h` | QoS1 kẹt trên link còn nói chuyện thì hết hạn (không đóng); chỉ đóng khi kẹt **và** im lặng; dự trữ 3/8 chỗ cho ACK; mã `CLOSE` của broker; đếm và lý do mọi lần đóng |
| Firmware | `transaction_bridge.h` | ACK retry back-off, log giới hạn |
| Web | `app.js`, `mqtt_transport.js` | thay token make-before-break; làm mới credential cho reconnect của chính socket; subscribe `uplink` |
| Test | xem mục 6 |

## 5. Hợp đồng PUBACK và an toàn khi mất điện thiết bị

PUBACK `alarm` = **BROKER_STORED**. Hàng đợi thiết bị (`CLOUD_OUTBOX_SIZE`=16) ở RAM: một sự kiện **chưa** được PUBACK mà thiết bị mất điện thì mất (giới hạn đã có từ trước, không đổi). Lỗi vẫn còn khi bật lại sẽ được phát hiện lại như sự kiện mới. Sau PUBACK sự kiện sống sót qua mất điện thiết bị (T8), restart broker (T9) và Worker/D1 hỏng (T5, T7).

## 6. Kiểm thử

| Mục tiêu | Test |
|---|---|
| 1 MQTT khỏe, 1 Critical | `uplink_resilience` T1: PUBACK 17–22 ms; `runtime-alarm-fallback` #1 |
| 2 100 cảnh báo có kiểm soát | T2: 100/100 lưu 1 lần, đúng thứ tự, PUBACK p50 16–23 ms/p95 33–47 ms/max ≤62 ms, 33–39 lần gọi Worker; `runtime-alarm-fallback` luồng 100 báo: 0 HTTPS, 0 đóng MQTT |
| 3 D1 chậm hơn timeout PUBACK | T3 (Worker 6 s/lần): PUBACK ≤118 ms, PINGRESP ≤6 ms, PUBACK heartbeat ≤32 ms, link không đóng; firmware: PUBACK 6 s → 0 HTTPS, 0 đóng |
| 4 D1/Worker 429 cooldown | T4 (`throttled` + chờ cooldown, rồi giao đúng thứ tự); unit `alarm-delivery` |
| 5 Worker lỗi, MQTT sống | T5 (503), T6 (mất phản hồi), `runtime-alarm-fallback` "Worker/D1 down invisible" |
| 6 Broker không PUBACK | firmware: critical đói → HTTPS **sau ≥60 s và ≥3 lần**, lý do `alarm:critical-starved`; routine không bao giờ |
| 7 MQTT TCP/WS mất thật | `runtime-alarm-fallback` #2/#8 (critical HTTPS 0 ms), T8 (LWT thật) |
| 8 half-open có chứng cứ độc lập | #6: im lặng >20 s **và** thăm dò không trả lời 4 s → HTTPS sau ≈4 s; <20 s im lặng → không kết luận |
| 9 Beacon khi MQTT khỏe | #G: 30 phút heartbeat không PUBACK nhưng broker còn nói chuyện → 0 beacon, 0 HTTPS, MQTT không đóng; bất biến vét cạn (1 792 tổ hợp) |
| 10 QoS1 đầy + nhiều lệnh Web | `firmware_wss_interop` (firmware thật + broker thật): 40 lệnh → 40 ACK tới Web, `ackRefusals=0`; hardening: loạt 40 xếp hàng đúng thứ tự, 0 đóng; kẹt >20 s hoặc 128 đầy → mới đóng |
| 11 Web thay token đang theo dõi | `run_mvp` W6: 0/335 mẫu không-online (trước khi sửa: 2 mẫu `cache`), snapshot vẫn chạy, lệnh vẫn thực thi; W7: backlog của broker hiện rõ cho người dùng, kết nối vẫn `online` |
| 12 Broker chết rồi hồi phục | T9 (kill broker, DO storage còn nguyên, tự giao) |
| 13 Worker/D1 hồi phục khi có cảnh báo chờ | T5b, T7b (70 sự kiện, 6 bị từ chối do đầy rồi nhận lại) |
| 14 20 thiết bị | T10: 20 thiết bị + 20 Web, snapshot 1 Hz, heartbeat tăng tốc, vòng cảnh báo, 1 reconnect: 0 đóng ngoài ý muốn, 40/40 lưu 1 lần |
| 15 ESP32 mất điện khi còn pending | T8 (sau PUBACK sống sót); trước PUBACK: giới hạn đã nêu mục 5 (RAM) |

Chạy cục bộ: `node --test tests/*.test.cjs` (225 pass) · `python3 tools/test_runtime_buses.py --sanitize` (ASan/UBSan) · `node tools/e2e/firmware_wss_interop.cjs` (9/9) · `node tools/e2e/uplink_resilience.cjs` (18/18) · `node tools/e2e/run_mvp.cjs` (17/17, cần Chromium). Cả ba e2e đã vào `reliability-checks.yml`.

Hai điều test **cố ý đổi** (không sửa cho khớp lỗi): hợp đồng cũ "PUBACK sau D1" và chính sách cũ "đóng socket khi quá tải cửa sổ 16" mã hóa đúng hai lỗi R1/R2; hợp đồng mới được ghi ở `doc/MQTT_CONTRACT.md` và test cũ được thay bằng test hành vi mới (cùng mức nghiêm ngặt, đảo chiều kỳ vọng).

## 7. Quota Cloudflare Free cho 20 máy — **ƯỚC LƯỢNG, chưa đo trên Cloudflare**

Giới hạn đang áp dụng (kiểm tra lại trên dashboard; có thể thay đổi): Workers 100 k request/ngày; Durable Objects (SQLite) 100 k request/ngày, tin nhắn WebSocket đến tính 1 request / 20 tin, mỗi lần DO alarm chạy tính 1 request; D1 100 k hàng ghi/ngày.

Số đo từ test (workerd): 100 sự kiện → 33–39 lần gọi Worker; heartbeat chuyển tiếp ≤ 1 lần/30 s/máy; mỗi sự kiện = 3 hàng ghi DO (+3 khi chuyển xong).

| Thành phần / máy / ngày | Trước | Sau |
|---|---|---|
| Alarm DO | 5 760 (cố định 15 s) | ≈ 1 900–2 800 (hạn chót ≈ lastRx + 46 s) + 1 lần/lần có cảnh báo |
| Tin WS vào DO, máy rảnh, không Web | ping 5 760 + heartbeat 1 440 + snapshot rảnh 720 → ÷20 ≈ 400 | như trước |
| Tin WS vào DO, 1 Web mở 8 giờ | snapshot 1 Hz 28 800 + session 3 s 9 600 + ping ≈ 2 000 request đã chia 20 | như trước |
| Gọi Worker (ingest) | 1 440 (heartbeat) + 1/cảnh báo | 1 440 + ≈ 0,4/cảnh báo |
| Hàng ghi D1 (touch heartbeat) | 1 440 | 1 440 |

20 máy, không Web: DO ≈ 20×(400+2 400) ≈ **56 k/ngày**; 20 máy mỗi máy 1 Web 8 giờ: ≈ **96 k/ngày** (sát trần 100 k). Trước sửa alarm 15 s đã là 115 k/ngày chỉ riêng alarm. Đòn bẩy nếu cần thêm chỗ (chưa áp dụng, cần quyết định của bạn): giảm nhịp `session` Web (3 s→6 s) hoặc nhịp snapshot khi có Web (1 s→2 s) giảm gần một nửa phần lớn nhất; hoặc Workers Paid. D1: 28,8 k hàng ghi/ngày cho heartbeat của 20 máy, dưới 100 k.

## 8. Triển khai & quay lui (CHƯA triển khai — chờ bạn phê duyệt)

Thứ tự bắt buộc: (1) Worker chính (`/api/internal/uplink` hiểu `kind:"alarms"` + phán quyết); (2) broker; (3) Web (Pages/assets); (4) nạp firmware 1.1.3. Broker mới **không tương thích** với Worker cũ: Worker cũ trả `BAD_KIND` → sự kiện nằm trong hàng đợi broker (hiển thị `last_error=HTTP_400`) cho tới khi Worker mới có mặt — không mất.
Quay lui: deploy lại broker/Worker bản `bdc4ebb` (hàng đợi `uq:*` trong DO bị bỏ qua, các sự kiện còn chờ trong đó **không** được chuyển — hãy chờ `pending=0` trước khi quay lui); firmware 1.1.2 vẫn hoạt động với broker mới (PUBACK nhanh hơn, không cần đổi). Cấu hình secret không đổi.

## 9. Nghiệm thu trên mạch (tối thiểu 72 giờ) — tất cả hiện `NOT TESTED`

Chuẩn bị: nạp firmware 1.1.3, mở Serial, đặt `DIAG ON`. Theo dõi các dòng: `[MQTT] closed on purpose … why=…`, `[MQTT] link lost … brokerClose=…`, `[MQTT-STAT] closes … | qos1 expired/refusedBulk/refusedAck | probes/answered`, `[CLOUD] … INGEST_TIMEOUT`, `[ALARM-TIMING]`, `[TLS-GUARD]`, `[HEAP]`, `[TASK]`.

| # | Việc | Tiêu chí PASS | Trạng thái |
|---|---|---|---|
| H1 | 72 h chạy bình thường, Web mở 8 h/ngày | `closes cloud-tls=0`, `lost` chỉ khi bạn chủ động ngắt Wi-Fi/router; không `ACK PUB FAIL` lặp | NOT TESTED |
| H2 | Bật/tắt công tắc nhiệt liên tục (≥20 lần/10 phút) trong 1 giờ, 10 lần/ngày | mỗi lần Web nhận PUBACK sớm, `[ALARM-TIMING] via=mqtt route_to_ack` <1 s, **không** `FALLBACK HTTPS`, Web không "NGOẠI TUYẾN" | NOT TESTED |
| H3 | Bấm công tắc đèn 30 lần liên tiếp từ Web | 30 ACK về Web; `refusedAck=0` hoặc rất nhỏ; `link lost` = 0; không `brokerClose=1013` | NOT TESTED |
| H4 | Ngắt Internet của router 5 phút khi có cảnh báo Critical | `FALLBACK HTTPS cause=down` ngay, HTTPS 200; khi Internet về MQTT nối lại ≤ vài giây, backlog `uplink` về 0 | NOT TESTED |
| H5 | Chặn tạm Worker (đổi secret sai ở môi trường thử, **không** làm trên bản đang chạy) | cảnh báo vẫn PUBACK; Web hiện backlog; khôi phục → giao hết, không trùng | NOT TESTED |
| H6 | Rút điện thiết bị 1 lần khi có Critical đang chờ PUBACK | ghi nhận mức mất (chưa PUBACK) so với đã PUBACK; Critical còn lỗi được tạo lại khi bật | NOT TESTED |
| H7 | Số đo: `[HEAP] free/min/largest`, `[TASK]` stack, `[TLS-GUARD] contexts_max=1 overlap_violations=0` suốt 72 h | `contexts_max=1`, `overlap_violations=0`, heap min ≥ 30 kB, không reset | NOT TESTED |
| H8 | Web: giữ tab 3 giờ qua mốc đổi token | không đổi nhãn trạng thái; không `TOKEN_EXPIRED` | NOT TESTED |

FAIL nếu: có `closed on purpose (cloud-tls)` mà `why` không thuộc `alarm:link-down|link-flapping|half-open-confirmed|critical-starved|beacon:broker-unreachable|other`; có `brokerClose=1013` ngoài khi bạn cố tình ngắt; `overlap_violations>0`; reconnect storm (>1 nối/15 s kéo dài); controlTask trễ.

Kích thước firmware (CI, cùng profile): `bdc4ebb` Flash 1 362 297 B / RAM tĩnh 154 880 B → 1.1.3 Flash 1 364 253 B (**+1 956 B**, +0,14 %) / RAM tĩnh 154 992 B (**+112 B**); ngân sách mềm 1 460 000 / 164 000 vẫn còn dư (95 747 B / 9 008 B); giới hạn cứng phân vùng ứng dụng 3 342 336 B. Heap/TLS thật `NOT TESTED` (cần mục H7).
