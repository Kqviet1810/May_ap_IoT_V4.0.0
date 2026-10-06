# MAYAP Cloudflare — baseline `main` 1.1.0

Cloudflare hiện là **control plane + realtime router + Web host**, không phải controller của máy. ESP32 vẫn là nơi duy nhất chạy PID, heater safety, turning, batch và output arbitration.

Runtime 1.1.0 đã bỏ MQTT broker/HiveMQ/EMQX và credential realtime dùng chung fleet. Realtime dùng **native WebSocket + SQLite Durable Object `DeviceHub`**.

## Luồng request hiện hành

`wrangler.toml` trỏ entrypoint tới:

```text
src/account-worker.js
```

`account-worker.js` xử lý Google account, ownership/role, Device ID + PIN, browser session, ticket WebSocket và routing realtime. Các endpoint vật lý của ESP32 được chuyển qua chuỗi hiện hữu:

```text
account-worker.js
  -> reliability-wrapper.js
      -> security-wrapper.js
          -> index.js
```

`DeviceHub` là một SQLite Durable Object theo từng `device_id`. Hub xác thực/giới hạn phiên và chuyển frame giữa browser ↔ ESP32; **Hub không thực thi command máy**.

## Realtime contract

Endpoints:

- `GET /realtime/device/MAP-...` — ESP32, xác thực bằng device key riêng trong NVS + `X-Mayap-Boot`.
- `GET /realtime/browser/MAP-...` — browser, dùng one-use ticket từ `POST /api/device/realtime-session`.

Browser ticket đi trong `Sec-WebSocket-Protocol` (`mayap.v1`, `ticket.<...>`), không nằm trong URL. Browser Origin phải khớp chính xác `ALLOWED_ORIGIN`.

Wire frame là JSON bounded với `v`, `channel`, `payload`. Command/config/history tiếp tục dùng signed V2 body/grant/signature và được ESP32 kiểm lại.

**`forwarded` chỉ là transport receipt.** Thành công/thất bại của thao tác chỉ được chốt bằng terminal ACK do ESP32 ký/xác thực. Mất socket hoặc timeout được coi là `UNCERTAIN`; retry phải giữ exact payload/request identity.

## Resources hiện hành

- `DB`: D1 `mayap_push`.
- SQL migrations: `0001` → `0006`.
- Ghi chú và Nhắc nhở tùy chỉnh lưu trực tiếp trong D1 theo tài khoản/máy; ESP32/AT24C512 không nằm trong đường commit dữ liệu.
- `DEVICE_HUB`: SQLite Durable Object class `DeviceHub`, Wrangler migration tag `device-hub-v1`.
- `ASSETS`: thư mục build `cloudflare/public/` từ allowlist của `tools/build_web_assets.py`.
- Worker-first routes: `/api/*`, `/realtime/*`.
- Cron: `* * * * *` để kiểm tra trạng thái offline/Push control-plane.
- Compatibility date: **`2026-10-01`**.
- Compatibility flag: `nodejs_compat`.

Telemetry realtime không được ghi D1 theo nhịp snapshot. D1 giữ account, ownership, session, device/provisioning, Push và metadata/control-plane.

## Secrets và biến cấu hình

Biến không bí mật nằm trong `wrangler.toml`, gồm `GOOGLE_CLIENT_ID`, `ALLOWED_ORIGIN`, `REQUIRE_DEVICE_INVENTORY` và rate-limit admission.

Secrets phải nằm trong Cloudflare Dashboard/`wrangler secret put`, không commit vào source:

- `DEVICE_KEY_PEPPER`
- `MAYAP_SESSION_PEPPER`
- `VAPID_PUBLIC_KEY`
- `VAPID_PRIVATE_KEY`
- `VAPID_SUBJECT`
- `GITHUB_TOKEN` *(optional cho release reads)*

Không có broker username/password hoặc fleet WebSocket credential. Device key riêng của từng ESP32 nằm trong NVS; server chỉ lưu dạng hash có pepper.

**Không xoay `DEVICE_KEY_PEPPER`/session pepper như thao tác deploy thông thường.** Việc đổi pepper có thể làm mất khả năng xác thực thiết bị/session đang tồn tại.

## Local setup

Repo đã có lockfile pnpm. Dùng đúng pnpm đã pin trong workflow:

```bash
cd cloudflare
npx --yes pnpm@11.19.0 install --frozen-lockfile --ignore-scripts
python3 ../tools/build_web_assets.py
npx pnpm@11.19.0 dev
```

Real Google login cần origin HTTPS đã được cho phép trong Google Cloud. Automated tests dùng local fixture riêng, không hạ security production.

Chạy regression chính từ root repo:

```bash
node --test tests/*.test.cjs
python3 tools/test_single_eeprom.py --sanitize --check-regression
python3 tools/test_realtime_workerd.py
```

Browser integration:

```bash
cd cloudflare
npx pnpm@11.19.0 exec playwright install --with-deps chromium
cd ..
python3 tools/test_realtime_workerd.py --browser
```

`tools/build_web_assets.py` dùng public-file allowlist; firmware, tests, node_modules, private key, local override và `.dev.vars` không được đưa vào Static Assets.

## Deploy production

Workflow chuẩn: `.github/workflows/deploy-cloudflare-worker.yml`.

Pipeline deploy:

1. checkout đúng SHA/ref;
2. Node/release/reliability checks;
3. pnpm frozen-lockfile;
4. account/transaction/realtime regressions;
5. local `workerd` integration;
6. stage reviewed Static Assets;
7. apply D1 migrations remote;
8. deploy Worker + Assets + Durable Object binding/migration.

GitHub secrets cần cho deploy:

- `CLOUDFLARE_API_TOKEN`
- `CLOUDFLARE_ACCOUNT_ID`

Auto-deploy từ `main` chỉ hoạt động khi repository variable `CLOUDFLARE_REALTIME_AUTODEPLOY=1` và build workflow của **đúng SHA** thành công. Trong giai đoạn commissioning/cutover nên để tắt và deploy thủ công ref đã review.

Không bật thêm một Cloudflare Builds auto-deploy thứ hai cho cùng production Worker vì sẽ tạo hai nguồn triển khai cạnh tranh.

## Cutover / tương thích

- Firmware 1.1.0 nói native WebSocket/DeviceHub.
- Firmware MQTT cũ không tự biến thành WebSocket chỉ vì Worker/Web mới được deploy.
- Khi rollout mixed fleet, phải phối hợp Web/Worker cutover với firmware commissioning từng máy.
- Quota Cloudflare hết hoặc realtime mất chỉ làm remote control unavailable/uncertain; điều khiển cục bộ của ESP32 vẫn phải tiếp tục.

Chi tiết transaction contract, replay/dedup bounds và application ACK: `../doc/TRANSACTION_V2_SPEC.md`.
