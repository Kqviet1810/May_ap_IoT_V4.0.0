> **V3 LEGACY:** This document describes the pre-V4 realtime stack. The V4 clean network baseline has no WebSocket/DeviceHub runtime and no MQTT implementation yet.

> Cập nhật transport cho V1.1.0; xem [migration guide](CLOUDFLARE_REALTIME_MIGRATION.md) về cấu hình và giới hạn commissioning.

# MAYAP v3.8.1 - Dependency manifest

Mục tiêu: đủ để tái tạo build, rà lỗi dependency và hỗ trợ bảo trì dài hạn mà không triển khai hệ thống SBOM enterprise.

## Firmware ESP32

| Thành phần | Phiên bản / cấu hình |
|---|---|
| MCU | ESP32-S3-WROOM-1U-N8 |
| Flash | 8 MB |
| PSRAM | Disabled |
| Arduino ESP32 core | 3.3.11 |
| FQBN | `esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=default_8MB,FlashSize=8M,PSRAM=disabled` |
| U8g2 | 2.36.19 |
| Realtime transport | first-party RFC6455 + esp-tls async |
| ArduinoJson | 7.4.3 |
| TLS / crypto | ESP32 Arduino core + mbedTLS |
| OTA layout | dual OTA, `default_8MB` |

## ATtiny13A

| Thành phần | Cấu hình |
|---|---|
| MCU | ATtiny13A |
| Clock | 1.2 MHz |
| Toolchain | avr-g++ / avr-libc từ Ubuntu CI |
| Build | `-Os -flto -std=gnu++11 -fno-exceptions -fno-rtti` |
| Flash budget | <= 1024 bytes |
| Static RAM budget | <= 64 bytes |

## Web / Cloud

| Thành phần | Ghi chú |
|---|---|
| Cloud runtime | Cloudflare Workers |
| Database | Cloudflare D1 |
| Worker entrypoint | `cloudflare/src/account-worker.js` |
| Security layer | `cloudflare/src/security-wrapper.js` |
| Core API | `cloudflare/src/index.js` |
| Realtime hub | GA SQLite Cloudflare Durable Objects + Hibernation |
| Browser realtime | native WebSocket + first-party bounded Client |
| Push | Web Push qua Cloudflare Worker |
| Static frontend | Workers Static Assets, GitHub Actions deployment |

## Build/release invariants

- Không dùng `setInsecure()` trong firmware.
- Không commit private OTA signing key vào repository/firmware.
- Identity/credential riêng từng máy lưu NVS/D1; không có broker/fleet credential trong CI.
- OTA release phải có `.bin` và `.sig`.
- Firmware chỉ chấp nhận remote OTA khi size, SHA-256 và ECDSA signature hợp lệ.
- Tag release phải khớp `MAYAP_FIRMWARE_VERSION`.

## Chính sách nâng dependency

Không tự nâng dependency chỉ vì có phiên bản mới. Chỉ nâng khi có ít nhất một lý do:

1. Sửa CVE/lỗi ảnh hưởng trực tiếp MAYAP.
2. Sửa lỗi ổn định đã tái hiện được.
3. Cần API/tính năng mới thực sự cần thiết.
4. Toolchain hiện tại không còn build được.

Sau mỗi lần nâng dependency phải chạy lại:

- ESP32 compile CI.
- ATtiny compile/budget CI.
- provisioning 2 máy.
- WebSocket reconnect, lease, replay, native browser/workerd.
- Cloud Push.
- OTA interruption test nếu thay ESP32 core/TLS/HTTP/Update.
- ít nhất soak test rút gọn 8 giờ; release lớn vẫn yêu cầu 72 giờ.

## Rà soát định kỳ

Khuyến nghị 3-6 tháng/lần kiểm tra changelog/CVE của ESP32 core, ArduinoJson, esp-tls và U8g2. Không cần tự động merge dependency update vào firmware đang chạy ổn định.
