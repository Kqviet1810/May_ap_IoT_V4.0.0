# MAYAP V4 — MQTT contract (Phase 2A đã duyệt + điều chỉnh 2B)

Nguồn: Phase 2A audit (feature parity V2 → V4), đã sửa theo chỉ đạo trước khi
code Phase 2B. Mọi mục mâu thuẫn với tài liệu khác đều lấy file này làm chuẩn.

## 1. Protocol

- MQTT 3.1.1 (giao thức tên `"MQTT"`, Protocol Level `4`).
- Clean Session = `true` **bắt buộc** cho mọi client. Broker không giữ phiên
  offline, không giữ hàng đợi lệnh.
- Keepalive 60 giây mặc định (client có thể đề nghị 30–120).
- Chỉ hỗ trợ QoS 0 và QoS 1. QoS 2 bị từ chối ngay ở CONNECT và PUBLISH.
- Gói MQTT tối đa 4096 B kể cả fixed header, variable header, payload và topic.
  Vượt → broker đóng kết nối không CONNACK.
- Transport: MQTT trên WebSocket Secure (`wss://`), subprotocol `mqtt`,
  path `/mqtt/<deviceId>` (xem §4).
- Không MQTT 5, không shared subscription, không session persistence.

## 2. Topic / QoS / Retain

Root `mayap/v1/<deviceId>/…`. QoS và retain phải khớp CHÍNH XÁC bảng dưới. Mọi
subscriber được cấp QoS tối đa bằng `max(requestedQoS, publisherQoS)` nhưng
**không bao giờ** nâng QoS của PUBLISH gốc QoS0 lên QoS1 chỉ vì subscriber yêu
cầu QoS1. Fanout giữ nguyên QoS của PUBLISH.

| Topic | Hướng | Publish QoS | Retain | Subscribe QoS tối đa |
|---|---|---|---|---|
| `presence` | ESP32 → * | **1** | **true** | 1 |
| `snapshot` | ESP32 → * | **0** | false | 1 |
| `ack` | ESP32 → * | **1** | false | 1 |
| `log` | ESP32 → * | **0** | false | 1 |
| `config/reported` | ESP32 → * | **1** | false | 1 |
| `reminders/reported` | ESP32 → * | **1** | **true** | 1 |
| `history/reported` | ESP32 → * | **1** | false | 1 |
| `command` | Web → ESP32 | **1** | false | 1 |
| `config/set` | Web → ESP32 | **1** | false | 1 |
| `reminders/set` | Web → ESP32 | **1** | false | 1 |
| `history/request` | Web → ESP32 | **1** | false | 1 |
| `session` | Web ↔ ESP32 | **0** | false | 0 |

- **LWT** của ESP32: topic `presence`, payload `{"online":false,...}`,
  QoS 1, retain `true`.
- Lệnh (`command`, `config/set`, `reminders/set`, `history/request`) **cấm**
  retain. Broker set retain=0 trong fanout cho các topic này ngay cả khi
  client gửi retain=1 → broker ACK PUBACK, drop retain bit và từ chối lưu.
- `session` dùng QoS 0 không retain. Topic này chỉ truyền trạng thái session
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

- Web/ESP32 kết nối WSS tới `https://<broker-host>/mqtt/<deviceId>`.
- Worker route rút `deviceId`, chọn Durable Object bằng
  `env.MQTT_BROKER.idFromName(deviceId)` và forward WebSocket Upgrade.
- `deviceId` trong URL **không phải secret**. Mọi auth phải thực hiện khi
  nhận CONNECT (username/password) + ACL (xem §5).
- Mỗi DO chỉ phục vụ đúng `mayap/v1/<deviceId>/…`. Subscribe/publish ngoài
  root này bị từ chối SUBACK 0x80 / đóng kết nối.

## 5. Auth + ACL

- **Device role**: username = `deviceId`, password = `deviceToken` do Worker
  cấp khi `/api/device/register` hoặc `/api/device/rotate-key`.
  - Publish được: `presence`, `snapshot`, `ack`, `log`, `config/reported`,
    `reminders/reported`, `history/reported`, `session`.
  - Subscribe được: `command`, `config/set`, `reminders/set`,
    `history/request`, `session`.
- **Web role**: username = `web:<userId>`, password = short-lived token
  (ttl 60 phút) do `/api/mqtt-session` cấp sau khi xác thực Google +
  ownership trong D1.
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
    buffer dở, QoS1 inflight ring) → `ws.serializeAttachment(...)`.
  - Retained messages (`presence`, `reminders/reported`) → DO Storage
    key `retained:<topic>` chứa `{qos, payloadB64, ts}`.
  - QoS1 inflight server→client → trong attachment, bounded ring 16 entries.
- Clearing retained: PUBLISH payload rỗng + retain=1 → xoá key storage.
- Alarm định kỳ 15 s để kiểm tra keepalive quá hạn (1.5× keepalive) và
  giải phóng LWT.

## 7. Parser

- Streaming, bounded. Không giả định một WebSocket message = một MQTT packet.
- Buffer tích luỹ ≤ 4096 B. Vượt → đóng kết nối, không CONNACK.
- Từ chối:
  - Remaining Length encoding > 4 byte.
  - Packet type 0 hoặc 15.
  - CONNECT có protocol name khác `"MQTT"` v4.
  - Reserved flags sai (ví dụ PUBLISH QoS 3).
  - QoS > 1.
  - Topic rỗng, chứa null byte, hoặc UTF-8 bất hợp lệ.
  - Wildcard (`+`, `#`) trong topic của PUBLISH.
- Chỉ nhận packet type: CONNECT, PUBLISH, PUBACK, SUBSCRIBE, PINGREQ,
  DISCONNECT. Phần còn lại trả CONNACK 0x05 (CONNECT), close (sau CONNECT).

## 8. Broker không biết nghiệp vụ

- Không parse payload JSON của `command`, `config/set`, `reminders/set`,
  `ack`. Không log payload. Không D1 trong hot path. Không hiểu
  heater/PID/batch/turning/reminder nội dung.
- Chỉ đếm bytes, enforce ACL, retain, LWT, QoS1 PUBACK, hibernation.
