# Browser dependencies

Thư mục này chỉ chứa dependency browser được vendored có chủ ý.

- `jsQR.min.js`: fallback giải mã QR cho browser không có `BarcodeDetector` (đặc biệt WebKit/iOS).
- `mqtt.min.js`: MQTT.js **5.13.2** browser bundle, không sửa đổi, giữ nguyên bản đã chạy ổn ở V2.
  Nguồn: https://cdn.jsdelivr.net/npm/mqtt@5.13.2/dist/mqtt.min.js · Upstream: https://github.com/mqttjs/MQTT.js/tree/v5.13.2 · License MIT (`mqtt-LICENSE.md`).
  Web kết nối broker MQTT 3.1.1 qua WSS bằng bounded wrapper first-party `mqtt_transport.js`. Không có WebSocket/DeviceHub tự viết và không dùng HiveMQ.

Không thêm CDN runtime tùy ý vào đây. Dependency mới phải được review về license, kích thước, CSP/offline behavior và phải đi qua Web regression trước khi đưa vào public asset allowlist.
