# Trọng tài đường lên (TX arbiter) — Web phản ứng chậm, `1013 SLOW_CONSUMER`, PUBACK bị mất

Phạm vi: đường MQTT-over-WSS giữa ESP32 và broker Cloudflare (`mqtt_transport.h`, `transaction_bridge.h`, `mqtt_tx_arbiter.h`, `cloudflare/src/broker/broker-do.js`).
Mục tiêu: lệnh của Web (bật/tắt đèn, lưu cấu hình) được **xác nhận trong thời gian ngắn và có thể dự đoán**, đường lên không bao giờ làm mất nghĩa vụ với broker, và broker không cắt một thiết bị còn sống.

> **Chưa kiểm chứng trên máy thật.** Mọi con số dưới đây đến từ host test (giả lập socket, broker thật chạy trên workerd). Phần "Nạp máy thật" ở cuối nêu cách đo và tiêu chí đạt.

## 1. Điều log thực địa cho thấy (nguyên nhân gốc, không phải triệu chứng)

| # | Hiện tượng | Cơ chế |
|---|---|---|
| A | Web trễ vài giây sau khi bấm | **Đảo ưu tiên.** Cửa sổ TCP gửi của ESP32 rất nhỏ (~5,7 KB, `select()` chỉ báo ghi được khi còn > một nửa ≈ 2,9 KB) và RTT qua Cloudflare 100–400 ms. Snapshot 1–2 KB (QoS0) được thử **mỗi vòng**; ack kết thúc (≈ 300 B, QoS1) chờ trong bộ đếm back-off 50→1000 ms. Mỗi lần socket ghi được, snapshot lấy chỗ trước. Ngoài ra `publishAck` đặt lại đồng hồ snapshot ở **mỗi lần thử lại**, nên snapshot luôn "đến hạn". |
| B | `ACK PUB FAIL` lặp, CPU/heap bị đốt | Mỗi lần thử lại dựng lại JSON + ký HMAC rồi mới biết socket đóng. |
| C | PUBACK biến mất lặng lẽ | PUBACK dùng chung hàng đợi 6 ô (mỗi ô tối đa 96 B) với PINGREQ/PONG. Đầy → `++txDropped; return true` (báo thành công). Broker giữ gói đó trong cửa sổ in-flight 16 và chờ PUBACK vô hạn. |
| D | Broker đóng thiết bị bằng `1013 SLOW_CONSUMER`, thiết bị mất 8–24 s nối lại, cảnh báo trễ | Cửa sổ in-flight đầy + gói cũ nhất > 20 s chưa PUBACK = "kẹt". Hệ quả trực tiếp của C — nhưng thiết bị **đang sống** (`rxAge=0 ms`). |

## 2. Luật của trọng tài (cao → thấp)

1. **PUBACK nợ broker** — hàng đợi *chỉ id* (`PubackQueue`, 32 ô, 64 B), gửi trước mọi thứ, dựng khung tại thời điểm ghi (trên stack). **Không bao giờ bị bỏ.** Đầy → khởi động lại liên kết có kiểm soát (`link lost reason=puback-overflow`, đếm `ovf`), vì giấu mất mát chính là thứ đã dẫn tới D.
2. **Alarm / heartbeat uplink** — `pumpUplink` của transport, chạy trước mọi thứ cầu giao dịch nói.
3. **Ack kết thúc của lệnh** — thử lại **ở mọi vòng**, ngay khi socket ghi được (`select()`); không còn back-off theo giờ. Trong lúc có ack chờ, các làn dưới *nhường* (tối đa `ACK_PRIORITY_MAX_MS` = 2,5 s để một ack hỏng không câm telemetry mãi).
4. **Báo cáo lớn** (`config/reported`, `history/reported`) — cần ô QoS1 ngoài phần dự trữ cho ack và socket mở.
5. **Telemetry** (snapshot, log) — *latest wins*, có nhịp. Socket đóng → không dựng JSON.

### Hỏi trước khi dựng (gate-before-build)

`laneGate(lane)` (transport) trả lời bằng **một** `select()` + hai phép so sánh: socket ghi được? còn PUBACK/PING chưa gửi? cửa sổ QoS1 còn chỗ cho làn này? Cầu giao dịch gọi nó **trước** khi dựng snapshot, ký ack hay tuần tự hóa báo cáo. Socket đóng được ghi nhận là *stall* cho bộ giám sát liên kết (`tx-stall` sau 25 s + broker im 10 s) — gate không làm mất khả năng phát hiện liên kết chết.

### Nhịp snapshot thích nghi (`SnapshotPacer`)

* Nền: 1 s khi Web có phiên, 120 s khi rảnh (không đổi).
* Đến hạn mà socket đóng → **một** "tắc nghẽn" mỗi đợt (dù kéo dài bao nhiêu vòng) → cộng thêm 500 ms (tối đa tổng 4 s). Mỗi snapshot gửi *sạch* → trừ 250 ms. (tăng cộng/giảm cộng, không dao động).
* Snapshot ép (sau ack kết thúc) bỏ qua khoảng nền nhưng **không** bỏ khoảng tối thiểu 150 ms: năm lệnh liên tiếp chia sẻ một snapshot mới thay vì năm khung 2 KB. Chỉ lần công bố **đầu tiên** của một kết quả mới ép snapshot; thử lại cùng kết quả thì không.
* Snapshot thất bại dù cổng mở → thử lại sau 100 ms, không dựng lại mỗi vòng.

## 3. Broker

`cloudflare/src/broker/broker-do.js`: một consumer **vẫn đang gửi gói** (bất kỳ gói nào trong `PEND_STALE_MS` = 20 s) là còn sống. Khi cửa sổ in-flight của nó đầy toàn gói quá 20 s chưa PUBACK, broker **gỡ sổ** các gói cũ đó (đã gửi rồi; MQTT 3.1.1 chỉ gửi lại khi nối lại) thay vì đóng `1013`; hàng đợi `pend:*` tiếp tục chảy; đếm vào `att.ackLost`. Alarm của DO cũng quét để hàng đợi không phải chờ publish kế tiếp. Consumer **im lặng** hoặc hàng đợi 128 đầy vẫn bị đóng như trước (giới hạn bộ nhớ không đổi).

Tương thích: firmware mới + broker cũ vẫn tốt hơn trước (PUBACK không còn mất, nên broker cũ hầu như không còn lý do cắt); broker mới + firmware cũ cũng an toàn. **Broker/Worker chỉ có hiệu lực sau khi bạn deploy** (`wrangler deploy -c wrangler-broker.toml`); CI không tự deploy nhánh này.

## 4. Số đo trên máy (dòng `[MQTT-ARB]`, mỗi 10 s cùng `[MQTT-STAT]`/`[MQTT-TX]`)

```
[MQTT-ARB] wire pubackQ=<hiện tại>/<đỉnh> ovf=<n> ctrlDrop=<n> edges=<n> | wait ack=<đợt>x/<ms tối đa> bulk=<đợt>x/<ms> tele=<đợt>x/<ms>
[MQTT-ARB] lanes ack sent=<n> wireWaits=<n> failed=<n> | snap sent=<n> gateSkip=<n> ackSkip=<n> gap+<ms> congested=<n> | held=<n>
```

(Hai dòng vì một mục log serial chỉ có 224 byte kể cả tiền tố `[t=…]`; test kiểm tra ngay cả khi mọi bộ đếm ở giá trị lớn nhất.)

| Trường | Nghĩa | Đọc thế nào |
|---|---|---|
| `pubackQ=c/h` | PUBACK đang nợ / đỉnh trong cửa sổ 10 s | `h` thường 0–2. Tăng lên cao = socket đang nghẽn lúc broker đẩy lệnh. |
| `ovf` | số lần hàng đợi PUBACK đầy (⇒ khởi động lại liên kết) | **Phải là 0.** Khác 0 ⇒ nghẽn > ~20 s. |
| `edges` | số lần socket chuyển *đóng→mở* (tích lũy) | So với `wait` để biết nghẽn xảy ra bao nhiêu lần. |
| `wait ack=NxMms` | N đợt ack phải chờ socket (tích lũy), **M = chờ lâu nhất trong 10 s qua** | **Số quan trọng nhất cho "Web chậm".** M là độ trễ ack thêm do đường lên. Mong đợi < 1000 ms. |
| `ack sent / wireWaits / failed` | ack đã gửi; số vòng phải hỏi lại; số lần publish thất bại dù cổng mở | `failed` ≠ 0 liên tục = lỗi ký/mã hóa/link đóng. |
| `snap sent / gateSkip / ackSkip` | snapshot đã gửi; bị cổng chặn (không dựng); nhường cho ack | `gateSkip` cao + `wait tele` cao = đường lên nghẽn thật. |
| `gap+Xms`, `congested` | phần cộng thêm vào nhịp snapshot; số đợt tắc nghẽn | `gap+` ≠ 0 kéo dài = nhịp 1 s không thực tế trên đường truyền này (hiển thị Web chậm lại có chủ đích). |
| `held` | số vòng các làn dưới nhường cho ack | |

`[MQTT-TX] dropped=` giờ chỉ còn nghĩa "khung Droppable (snapshot/log) bị bỏ ở `sendFrame`"; PUBACK không còn nằm trong con số này (trước đây bị gộp và gây hiểu nhầm). Dòng `link lost` có thêm `pubackQ=`.

## 5. Không làm trong gói này (nói thẳng)

* **Kênh delta trạng thái / schema snapshot nhỏ hơn**: là thay đổi *hợp đồng* giữa firmware và Web (Web thay `device.snapshot` nguyên khối nên snapshot một phần làm hỏng bản Web đã cache). Gói này giảm *tần suất và chi phí* snapshot, không đổi nội dung nó.
* **Chuyển thao tác Cloud (HTTPS) sang MQTT** (P1-7) và việc HTTPS đóng MQTT để nhường heap.
* **Phân mảnh heap**: transport không thêm cấp phát động; chưa có đo `largest` theo thời gian.
* Nguyên nhân sâu hơn của cửa sổ gửi nhỏ (cấu hình lwIP `TCP_SND_BUF`, đường Cloudflare) **chưa xác định**. Số `wait`/`edges` trên máy thật sẽ cho biết đó là cửa sổ TCP của ESP32 hay đường truyền.

## 6. Kiểm thử (host)

| Test | Chứng minh |
|---|---|
| `tests/realtime-arbiter.cpp` (cắt thẳng `drainAckOutbox`, `serviceLiveSnapshot`, `mayapRealtimeUpdate` từ `transaction_bridge.h`) | Socket chỉ nhận **một** khung mỗi lần mở ⇒ khung đó là **ack**, không phải snapshot đang đến hạn; socket đóng ⇒ 0 snapshot/0 ack được dựng; thử lại theo cạnh mở (không back-off), 10 s đóng rồi mở ⇒ ack ra ở vòng đầu; các làn dưới nhường tối đa 2,5 s; nhịp AIMD; snapshot ép gộp trong 150 ms; thất bại không dựng lại mỗi vòng. `--check-regression` đảo từng luật và yêu cầu test **đỏ**. |
| `tests/mqtt-transport.cpp` mục 24 (`mqtt_transport.h` thật) | 20 PUBACK nợ khi socket đóng ⇒ 0 bị bỏ, ra đủ **đúng thứ tự** khi mở; 33 ⇒ `reason=puback-overflow`; `laneGate` không dựng/ghi gì, phân biệt "socket đóng" (stall) với "cửa sổ QoS1 đầy" (không stall), đo thời gian chờ; watchdog `tx-stall` vẫn hoạt động khi producer bị gate chặn; dòng `[MQTT-ARB]` được in. |
| `tests/runtime-transactions.cpp` | Thử lại cùng kết quả **không** ép snapshot lần nữa; kết quả mới thì có. |
| `tests/mqtt-broker-hardening.test.cjs` §2 (broker DO thật) | Thiết bị còn nói + mất PUBACK ⇒ không bị đóng, đủ 18 lệnh đúng thứ tự, `ackLost=16`; alarm xả hàng đợi dù không có publish mới; thiết bị im lặng vẫn bị đóng; test mới **đỏ** với broker cũ. |
| `tools/e2e/firmware_wss_interop.cjs` | Transport firmware thật ↔ broker DO thật: 40 lệnh liên tiếp ⇒ 40 ack tới Web, không đóng. |

## 7. Nạp máy thật — đo và tiêu chí đạt

Nạp firmware, deploy broker (tùy chọn nhưng khuyến nghị), mở Web, để chạy ≥ 30 phút với Web mở rồi lấy `[MQTT-ARB]`/`[MQTT-TX]`/`[MQTT-STAT]`.

| # | Kịch bản | Đạt khi |
|---|---|---|
| T1 | Bật/tắt đèn từ Web 20 lần, cách nhau 1–2 s | Không `link lost`; `ovf=0`; `wait ack` M < 1000 ms ở ≥ 95 % cửa sổ; Web đổi trạng thái đèn < 1 s sau ack |
| T2 | Bấm đèn liên tục 5 phút (cố tình) | Không `link lost`; `held` tăng; `gap+` có thể > 0; không `1013` ở broker |
| T3 | Lưu cấu hình liên tiếp (setpoint ±10) | Một `config/reported` mỗi đợt; ack không bị trễ bởi báo cáo |
| T4 | Làm yếu Wi-Fi (RSSI < −80) 5 phút | `edges`/`wait` tăng, `link lost reason=tx-stall` chỉ khi thật sự mất đường; `ovf=0` |
| T5 | Chạy 24 h, Web mở 8 h | `closes lost` chỉ do tác động bên ngoài; không `1013` trong log broker; `qos1 expired` không tăng đều |

Nếu T1 vẫn chậm với `wait ack` M thấp ⇒ độ trễ không nằm ở đường lên của thiết bị (nhìn RTT broker/Worker, xử lý ở Web). Nếu M cao ⇒ socket thật sự nghẽn: bước tiếp theo là xem `TCP_SND_BUF` và kênh delta ở mục 5.
