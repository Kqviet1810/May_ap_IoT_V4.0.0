# MAYAP V4 — MQTT contract (Phase 2A đã duyệt + điều chỉnh 2B)

Nguồn: Phase 2A audit (feature parity V2 → V4), đã sửa theo chỉ đạo trước khi
code Phase 2B. Mọi mục mâu thuẫn với tài liệu khác đều lấy file này làm chuẩn.

## 1. Protocol

- MQTT 3.1.1 (giao thức tên `"MQTT"`, Protocol Level `4`). Giao thức khác
  (ví dụ MQTT 5) sẽ nhận CONNACK `0x01` (Unacceptable protocol) rồi đóng.
- Clean Session = `true` **bắt buộc** cho mọi client. Broker không giữ phiên
  offline, không giữ hàng đợi lệnh.
- Keepalive **30–120 s** inclusive. Giá trị 0 (keepalive-disabled) không
  được chấp nhận vì retained presence không được stale trên half-open link.
  Ngoài vùng → CONNACK `0x05` + close.
- Chỉ hỗ trợ QoS 0 và QoS 1. QoS 2 bị từ chối ngay ở CONNECT và PUBLISH.
- Gói MQTT tối đa 4096 B kể cả fixed header, variable header, payload và topic.
  Vượt → broker đóng kết nối không CONNACK.
- Transport: MQTT trên WebSocket Secure (`wss://`), subprotocol `mqtt`,
  path `/mqtt/<deviceId>` (xem §4).
- Không MQTT 5, không shared subscription, không session persistence.

## 2. Topic / QoS / Retain

Root `mayap/v1/<deviceId>/…`. QoS và retain phải khớp CHÍNH XÁC bảng dưới.
Fanout QoS = `min(publishQoS, grantedQoS)`. Broker **không bao giờ** nâng QoS:
một PUBLISH QoS0 vẫn fanout ở QoS0 ngay cả khi subscriber xin QoS1.

Broker enforce "Publish QoS" cột dưới **chính xác**: một PUBLISH với QoS
khác giá trị bảng cho topic đó bị coi là protocol misuse → PUBACK (nếu
QoS1) rồi DROP (không retain, không fanout). Lặp lại 2 lần liên tiếp
trong cùng kết nối → đóng kết nối. Publish hợp lệ reset bộ đếm vi phạm.

| Topic | Hướng | Publish QoS | Retain | Subscribe QoS tối đa |
|---|---|---|---|---|
| `presence` | ESP32 → Web | **1** | **true** | 1 |
| `snapshot` | ESP32 → Web | **0** | false | **0** |
| `ack` | ESP32 → Web | **1** | false | 1 |
| `log` | ESP32 → Web | **0** | false | **0** |
| `config/reported` | ESP32 → Web | **1** | false | 1 |
| `reminders/reported` | ESP32 → Web | **1** | **true** | 1 |
| `history/reported` | ESP32 → Web | **1** | false | 1 |
| `alarm` | ESP32 → cloud ingest | **1** | false | — (không ai subscribe) |
| `heartbeat` | ESP32 → cloud ingest | **1** | false | — (không ai subscribe) |
| `command` | Web → ESP32 | **1** | false | 1 |
| `config/set` | Web → ESP32 | **1** | false | 1 |
| `reminders/set` | Web → ESP32 | **1** | false | 1 |
| `history/request` | Web → ESP32 | **1** | false | 1 |
| `session` | Web → ESP32 | **0** | false | **0** |

- **Uplink `alarm` / `heartbeat` (ESP32 → Worker, không fanout).** Payload JSON một sự kiện
  (`alarm`: `event_id`, `alarm_type`, `severity`, `state`, `message`, `temperature?`, `humidity?`,
  `detected_uptime_ms` — cùng trường với `POST /api/device/alarms`; `heartbeat`: `batch_running`).
  Broker không lưu/không phát cho subscriber; nó ký (`x-mayap-ts`, `x-mayap-sig` =
  HMAC-SHA256 theo `MQTT_DEVICE_SECRET`) rồi chuyển cho Worker chính qua service binding
  `CLOUD_INGEST` (`POST /api/internal/uplink`) và **chỉ PUBACK sau khi Worker xác nhận đã ghi bền**
  (`durable:true`). Không PUBACK khi: payload sai JSON, Worker lỗi/timeout, sự kiện bị từ chối
  (xung đột `event_id`, throttle cooldown), hoặc thiếu binding/secret — thiết bị giữ sự kiện và thử lại
  (idempotent theo `event_id`; sau 1 lần thiếu PUBACK nó dùng HTTPS trong 60 s). Các job của cùng một
  kết nối chạy tuần tự đúng thứ tự đến (alarm trước, recovery sau). Web không được publish/subscribe
  hai topic này.

- **LWT** của ESP32: topic `presence`, payload `{"online":false,...}`,
  QoS 1, retain `true`. Khi broker fire LWT, retained store phải được
  cập nhật *trước* fanout để subscriber kết nối sau vẫn thấy `online=false`.
- Lệnh (`command`, `config/set`, `reminders/set`, `history/request`) **cấm**
  retain. Một PUBLISH với `retain=1` lên các topic này là protocol misuse:
  broker PUBACK (nếu QoS1) rồi **DROP** — không retain, không fanout, không
  thực thi. Hành vi lặp lại trong một kết nối → đóng kết nối (slow violation
  = 2 lần).
- `session` dùng QoS 0 không retain, Web publish → ESP32 subscribe. ESP32
  không publish, Web không subscribe. Topic này chỉ truyền trạng thái
  active/ttl/sync của Web (xem §3). Không mang HMAC grant.

## 3. HMAC control grant — qua HTTPS, KHÔNG qua topic `session`

- Cấp qua HTTPS endpoint `/api/mqtt-session` trên Worker.
- Trả về `{sessionKey, grantNonce, ttlSec, bootIdExpected?}`; firmware
  xác minh HMAC `mayap-control-session:v2\n<deviceId>\n<clientId>` giống V2.
- Topic MQTT `session` **chỉ** để Web báo active/ttl/sync (ví dụ "có tab khác
  đang điều khiển"). Không phải kênh handshake HMAC.
- Lệnh (`command`, `config/set`, `reminders/set`) tiếp tục mang chữ ký HMAC
  trong payload JSON, broker không hiểu chữ ký, chỉ fanout.

## 4. Kiến trúc 1 Durable Object / 1 device

- Web/ESP32 kết nối WSS tới `https://<broker-host>/mqtt/<deviceId>` với
  WebSocket subprotocol **bắt buộc** là `mqtt`. Thiếu subprotocol → 400.
- Worker route rút `deviceId`, chọn Durable Object bằng
  `env.MQTT_BROKER.idFromName(deviceId)` và forward WebSocket Upgrade.
- `deviceId` trong URL **không phải secret**. Mọi auth phải thực hiện khi
  nhận CONNECT (username/password) + ACL (xem §5).
- Mỗi DO chỉ phục vụ đúng `mayap/v1/<deviceId>/…`. Subscribe/publish ngoài
  root này bị từ chối SUBACK 0x80 / đóng kết nối.
- **Một DO chấp nhận tối đa MỘT connection role=device** đang mở. Một
  CONNECT device mới hợp lệ sẽ takeover: broker đóng connection device cũ
  (code 1000 "TAKEOVER"), set `disconnected=true` để chặn LWT, rồi mở kết
  nối mới. Mục đích: hai ESP32 không bao giờ cùng nhận `command`.
- **ClientId takeover (MQTT-3.1.4-2)**: ngoài quy tắc trên, một CONNECT
  mới có `ClientId` trùng với một session đang mở (bất kể role) sẽ đóng
  session cũ với code 1000 "CLIENTID_TAKEOVER".

## 5. Auth + ACL

- **Device role**: username = `deviceId`, password = `deviceToken` do Worker
  cấp khi `/api/device/register` hoặc `/api/device/rotate-key`.
  - Publish được: `presence`, `snapshot`, `ack`, `log`, `config/reported`,
    `reminders/reported`, `history/reported`, `session`.
  - Subscribe được: `command`, `config/set`, `reminders/set`,
    `history/request`, `session`.
- **Web role**: username = `web:<userId>`, password = token
  `v1.<exp>.<HMAC-SHA256(secret, "mayap-mqtt-web:v1\n<deviceId>\n<username>\n<exp>")>`
  do `/api/mqtt-session` cấp sau khi xác thực Google + ownership trong D1.
  Token **gắn với đúng một thiết bị và một tài khoản**, sống 1 giờ (broker từ chối
  `exp` quá 2 giờ), broker kiểm không cần D1; Web gia hạn trước khi hết hạn.
  Token của máy A bị từ chối ở máy B.
  - Publish được: `command`, `config/set`, `reminders/set`,
    `history/request`, `session`.
  - Subscribe được: tất cả topic ESP32 → * cộng `session`.
- Vi phạm ACL ở SUBSCRIBE → return code `0x80`. Vi phạm ACL ở PUBLISH →
  broker drop packet, gửi PUBACK nếu QoS1 (giấu role structure), rồi đóng
  kết nối sau khi vi phạm thứ hai liên tiếp.
- Credentials lookup phải cache ngoài hot path (D1 không nằm trong mỗi click).

## 6. Hibernation + state

- DO phải dùng WebSocket Hibernation API: `state.acceptWebSocket(ws)`,
  callbacks `webSocketMessage/webSocketClose/webSocketError`.
- **Không** giữ dữ liệu quan trọng chỉ trong RAM:
  - Metadata kết nối (clientId, role, keepaliveMs, lastRxMs, LWT, subs,
    buffer dở, QoS1 inflight set, `nextPacketId`) →
    `ws.serializeAttachment(...)`.
  - Retained messages (`presence`, `reminders/reported`) → DO Storage
    key `retained:<topic>` chứa `{qos, payloadB64, ts}`.
  - QoS1 inflight server→client → trong attachment, bounded 16 entries.
    `nextPacketId` **per-connection** (không chia sẻ giữa client); không
    bao giờ tái dùng packetId đang inflight.
- Clearing retained: PUBLISH payload rỗng + retain=1 → xoá key storage.
- **Backpressure** cho QoS1: khi `inflight.length == 16`, broker không
  drop silently. Đóng slow consumer bằng close code 1013 "SLOW_CONSUMER"
  và dừng fanout tới nó. Transaction V2 ở tầng trên tự retry.
- Alarm định kỳ 15 s để kiểm tra keepalive quá hạn (1.5× keepalive) và
  giải phóng LWT.

## 7. Parser

- Streaming, bounded. Không giả định một WebSocket message = một MQTT
  packet. Nhiều MQTT packet hợp lệ có thể được coalesce trong một
  WebSocket frame; broker vẫn parse từng cái một.
- Giới hạn 4096 B áp dụng **PER MQTT PACKET** (`totalLen` tính từ fixed
  header + Remaining Length). Decoder buffer tích luỹ cho phép tới
  16 KiB để chứa nhiều packet coalesced + phần dở; vượt → OVERFLOW.
- Từ chối (fatal close, không CONNACK nếu còn ở `await-connect`):
  - Remaining Length encoding > 4 byte (kể cả khi byte thứ 5 có MSB=0).
  - Một packet khai báo `totalLen > 4096 B` → OVERFLOW ngay khi parse
    xong Remaining Length (chưa cần đợi đủ payload).
  - Packet type 0 hoặc 15.
  - CONNECT có protocol name khác `"MQTT"` v4, hoặc CleanSession=0.
  - Reserved flags sai (ví dụ PUBLISH QoS 3, DUP+QoS0, SUBSCRIBE flags
    không phải 0b0010, reserved bits của byte QoS trong SUBSCRIBE).
  - QoS > 1 bất cứ chỗ nào.
  - Topic rỗng, chứa null byte, hoặc UTF-8 bất hợp lệ.
  - Wildcard (`+`, `#`) trong topic của PUBLISH.
  - Text WebSocket frame (phải là binary).
- Chỉ nhận packet type: CONNECT, PUBLISH, PUBACK, SUBSCRIBE, PINGREQ,
  DISCONNECT. Phần còn lại → fatal close.

## 8. Broker không biết nghiệp vụ

- Không parse payload JSON của `command`, `config/set`, `reminders/set`,
  `ack`. Không log payload. Không D1 trong hot path. Không hiểu
  heater/PID/batch/turning/reminder nội dung.
- Chỉ đếm bytes, enforce ACL, retain, LWT, QoS1 PUBACK, hibernation.
