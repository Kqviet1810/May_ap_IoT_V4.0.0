> Tài liệu lịch sử của baseline V1.0.0/PR #24. Luồng Google account/ownership/claim vẫn là nền tảng hiện hành; mô tả transport cũ bên dưới không áp dụng cho V4 clean baseline.

# Google Account + ownership — phạm vi tối giản của PR #24

Frontend vẫn là GitHub Pages: `https://kqviet1810.github.io/May_ap_trung_V2.1.1/`.
Backend vẫn là Cloudflare Worker + D1. Web kết nối WSS **trực tiếp HiveMQ** như
`feat/web-fast-connect`. Không có gateway/VPS/Caddy/Docker/domain mới hoặc Worker
hosting frontend. Không phát lệnh deploy Worker, migration remote, merge main hay
OTA trong quá trình làm PR này. PR giữ Draft. GitHub Pages đang cấu hình tự publish
nhánh `feat/google-account-multitenant`: push PR cũng tự chạy deployment Pages theo
Settings hiện hữu, không chờ PR hết Draft. Cấu hình Pages không được thay đổi ở đây.

## Giới hạn bảo mật bắt buộc

- **Account/API isolation: đã triển khai.** Worker xác thực MAYAP session rồi kiểm
  tra `user_devices` trước khi đọc status/history/config, cấp MQTT/control grant,
  rename, change PIN hoặc liên kết Push. PIN đúng không chiếm được máy có owner khác.
- **MQTT command authorization: vẫn được HMAC V2 bảo vệ.** Grant theo từng máy,
  hạn 5 phút; command giữ bootId/expiry/anti-replay và signed ACK. Biết broker
  credential chung không cho phép tự tạo chữ ký hoặc xin grant cho máy khác.
- **MQTT read/topic isolation giữa khách hàng: CHƯA giải quyết hoàn toàn.** Web
  vẫn nhận credential HiveMQ chung. Người có credential có thể tự dùng MQTT client
  hoặc DevTools; khả năng subscribe telemetry/topic máy khác phụ thuộc ACL hiện tại
  của HiveMQ. Không mô tả credential này là tenant-safe; UI chỉ subscribe máy trong
  account không phải một hàng rào broker authorization.
- Cô lập MQTT read/topic là hạng mục tương lai trước khi yêu cầu multi-tenant thương
  mại nghiêm ngặt. PR này không thay broker hoặc transport để giải quyết hạng mục đó.

API ownership không thể thu hồi tức thì một grant HMAC đã cấp: grant đó còn hợp lệ
đến expiry tối đa 5 phút theo firmware hiện hành. Logout/revoke chặn cấp grant mới,
Web dọn key/socket hiện tại. Firmware được giữ nguyên, kể cả compatibility verifier
cũ; Web hiện hành chỉ phát V2 và `/sign-mqtt` cũ không còn cấp chữ ký qua API.

## Login và session

Landing hiện khi chưa có session; dashboard ẩn. Session sẵn có được kiểm tra trước
khi hiện dashboard, không flash landing. Google Identity Services render nút Google
chuẩn, popup/callback tại GitHub Pages. Worker cấp nonce/challenge một lần, hạn
5 phút, giới hạn 60 lần/15 phút/IP. Web gửi Google ID token qua HTTPS POST;
Worker kiểm RS256/JWKS, issuer, audience, expiry/issued-at, nonce, azp và `sub`.
Nonce challenge bị consume một lần; không tin profile tự gửi từ frontend.

Google `sub` là identity; email/name chỉ để hiển thị. Không tạo password riêng,
không lưu Google ID/access/refresh token, không cần OAuth client secret cho luồng GIS
popup ID-token này. [GIS integration](https://developers.google.com/identity/gsi/web/guides/integrate),
[server verification](https://developers.google.com/identity/gsi/web/guides/verify-google-id-token).

Worker cấp token MAYAP opaque 256-bit, chỉ lưu hash có pepper trong D1. Hạn tuyệt đối
24 giờ; có logout/revoke/disabled account. Browser giữ token trong RAM +
`localStorage` để giữ khi đóng tab/trình duyệt; gửi `Authorization: Bearer` cho đúng Worker
origin. Mỗi lần mở vẫn xác minh phiên trên máy chủ; hết hạn/thu hồi mới yêu cầu login.
Token cũ trong sessionStorage được chuyển sang localStorage khi mở bản mới.
Mỗi điện thoại/PC login Google là lấy lại danh sách máy, không nhập PIN từng máy nữa.

Trade-off: token đọc được bởi JavaScript nếu có XSS, kể cả token lưu bền trên thiết bị.
Giữ hạn tuyệt đối 24 giờ và server revoke; không lưu Google token hoặc broker password
bền trong trình duyệt. Trên thiết bị dùng chung, phải đăng xuất sau khi dùng. Cookie HttpOnly
cross-site giữa github.io và workers.dev phụ thuộc third-party cookie và có thể bị
iOS chặn, nên không dùng cookie cho account API. CORS chỉ cho đúng GitHub Pages
origin; explicit bearer authorization và Origin check ngăn cookie-style CSRF.
GIS login thêm nonce một lần. Không dùng `Access-Control-Allow-Origin: *` cho account.

Logout revoke current session, xóa Push links của phiên, token/cache/control keys và
MQTT socket. BroadcastChannel báo các tab cùng origin dọn phiên. Settings vẫn có
revoke session khác. Cache namespace `mayap.account.<Google sub>.runtime.v1.<Device ID>`
được hiện sau khi ownership xác thực; không coi cache là live. Cold offline reload
không xác thực được account thì giữ màn kiểm tra/retry; tab đã xác thực vẫn giữ RAM/
socket khi HTTP tạm lỗi.

## D1 và claim

Migration **`0004_accounts.sql`** thêm:

- `users`: Google sub primary key, display email/name, login timestamps, disabled.
- `user_sessions`: token hash unique, user, expiry, revoked timestamp, user agent.
- `user_devices`: user/device membership, owner/operator/viewer; unique một owner/máy.
- `google_login_challenges`: challenge hash, nonce, expiry, consume một lần.
- Push subscriptions thêm user/session association. Push cũ được xóa và tự relink
  sau login nếu browser đã bật thông báo; browser client cũ bị revoke.

Login → + Thêm máy → Device ID + PIN → Worker kiểm existing PIN hash + rate-limit
5 lần/15 phút/máy, 30 lần/15 phút/IP → insert owner atomic. Máy có owner khác trả 409,
kể cả biết PIN. Reset PIN HMI không đổi owner. Chưa thêm UI chia sẻ/chuyển owner;
schema role để mở rộng sau. Legacy `device_clients/browser_limit` giữ audit/schema,
không còn cấp quyền browser; `device_inventory` giữ admission/disable thiết bị.

Physical-device API register/heartbeat/reset-pin/rotate-key/alarm và OTA download vẫn
dùng device authentication hiện có, không Google. Public metadata/VAPID không chứa
dữ liệu riêng từng máy. Mọi API dữ liệu máy/grant/đổi máy đều cần account ownership.
Config thực tế vẫn lazy load qua MQTT; HTTP config trả hướng dẫn dùng MQTT sau khi
kiểm quyền. History API chỉ đọc tối đa 500 điểm, không thêm cloud telemetry write.

## Realtime giữ nguyên

`Web → local HMAC V2 → HiveMQ MQTT → ESP32/controller → signed ACK`. Không HTTP trong
từng click. Broker credential giữ RAM, không localStorage. Worker chỉ cấp broker
credential/grant sau kiểm membership; viewer không được control grant. HMAC key
derivation và signed ACK giữ nguyên. `/verify-pin`, `/sign-mqtt`, `/session-check`
browser cũ trả upgrade-required; Web mới không poll route cũ hoặc dùng pairing token
để cấp quyền account.

Cache-first, bootstrap retained, QoS1, lazy config/history/log, MQTT reuse và packet/
PINGRESP liveness giữ nguyên. WARM **300 giây** theo branch gốc; hidden không đóng
socket khỏe, resume reuse + sync. Grace SAVE/PERFORMANCE, snapshot cadence và Cloud
heartbeat **15 giây** cũng giữ nguyên firmware nhánh gốc. Không còn thay đổi heartbeat
60 giây hoặc hidden snapshot 3 giây trong PR này. Account session validation một
batch 5 phút/lần khi visible/resume, control grant renew ngoài click như hiện hành.

Toàn bộ firmware ESP32/ATtiny giống base `37d8035`; PID/heater/turning/HMI/EEPROM/
batch recovery không thay đổi. MQTT I/O vẫn mqttTask, Cloud I/O cloudTask.

## Các bước thủ công để cấu hình và deploy sau khi operator phê duyệt

1. Google Cloud Console → tạo/chọn project → Google Auth Platform → Branding/Audience:
   đặt tên MAYAP, support/developer email, External nếu phục vụ khách ngoài tổ chức;
   thêm test users khi Testing. Khi mở cho khách thật, chuyển Publishing status và
   hoàn tất yêu cầu consent mà Google hiển thị.
2. Clients → Create client → **Web application**. Authorized JavaScript origins:
   **`https://kqviet1810.github.io`**, không có đường dẫn repo/slash cuối. GIS popup
   callback dùng JavaScript nên **không cần Authorized redirect URI** hoặc Worker
   `/auth/google/callback`. Không dùng client type Desktop/Android hoặc OAuth access
   token flow. Copy client ID `…apps.googleusercontent.com` (public).
3. Trong `cloudflare/wrangler.toml` đặt `GOOGLE_CLIENT_ID` thành ID trên.
   `ALLOWED_ORIGIN` giữ `https://kqviet1810.github.io`. `config.js` giữ Worker URL
   `https://mayap-push-worker.vietk-mayaptrung.workers.dev`; nếu Worker URL đang vận hành
   khác thì sửa đúng URL hiện hữu, không cần domain mới. Không copy client secret vào Web.
4. Từ thư mục `cloudflare`, cài pinned packages và đăng nhập Cloudflare:

   ```powershell
   npx --yes pnpm@11.19.0 install --frozen-lockfile
   npx wrangler login
   npx wrangler secret put MAYAP_SESSION_PEPPER
   ```

   Nhập secret ngẫu nhiên mới ít nhất 32 byte, lưu ở secret manager; không dùng lại
   DEVICE_KEY_PEPPER. Giữ nguyên DEVICE_KEY_PEPPER/VAPID/OTA. Worker phải có existing
   `MAYAP_MQTT_PASSWORD` đúng credential HiveMQ hiện hành; `MAYAP_MQTT_USERNAME/HOST`
   hoặc `MAYAP_MQTT_WSS_URL` chỉ cần override nếu đã khác defaults hiện hữu.
   **Không cần GOOGLE_CLIENT_SECRET hoặc gateway/ticket secrets.**
5. Backup và kiểm tra D1 trước migration:

   ```powershell
   New-Item -ItemType Directory -Force .local
   npx wrangler d1 export mayap_push --remote --output .local/before-accounts.sql
   npx wrangler d1 migrations list mayap_push --remote
   npx wrangler d1 execute mayap_push --remote --command "PRAGMA table_info(firmware_cache); PRAGMA table_info(push_subscriptions); SELECT name FROM sqlite_master WHERE type='table';"
   ```

   Đảm bảo migrations 0001–0003 đã apply/tracked. Chỉ pending **0004_accounts.sql**
   thì chạy:

   ```powershell
   npx wrangler d1 migrations apply mayap_push --remote
   ```

   Nếu tracker và schema cũ không khớp (ví dụ signature/client_id có sẵn nhưng 0001/
   0002 báo pending), dừng đối chiếu backup/schema và reconcile tracking trước;
   không chạy lại ALTER mù. Nếu 0004 bản cũ đã từng apply ngoài PR, cũng dừng để viết
   migration follow-up thay vì chạy lại bản Draft sửa schema. PR này chưa apply remote.
6. Kiểm tra bundle và deploy **Worker API**:

   ```powershell
   npx wrangler deploy --dry-run
   npx wrangler deploy
   ```

   Hoặc chạy workflow manual `Deploy Cloudflare Worker` với đúng `source_ref` đã
   phê duyệt, sau khi xác nhận migration tracking và vars/secrets. Workflow cần existing
   GitHub secrets `CLOUDFLARE_API_TOKEN`, `CLOUDFLARE_ACCOUNT_ID`. Không auto trigger deploy
   khi push Draft. Worker không có ASSETS binding/frontend build step.
7. Dùng **GitHub Pages deployment hiện hành**; lưu ý branch hiện được auto-publish
   ngay khi push, nên Draft không ngăn deploy Pages. Operator có thể chọn staging
   branch/workflow nếu cần review trước khi xuất bản; PR không tự đổi Settings này. Đảm bảo
   account.js/styles.css cùng Web 12.2.1 được publish. Không chuyển hosting sang Worker.
   Mở URL Pages, login hai tài khoản, claim riêng, kiểm tra API/grant chéo tenant bị
   từ chối, reload/PC khác có danh sách máy và command/ACK giữ realtime.
8. Android/iOS/PC: kiểm tra popup/FedCM Google thật, consent, CORS, sessionStorage/PWA,
   đóng/mở tab, logout, WARM + OS suspend, command latency và Push. OAuth/fixture QA
   không thay kiểm thử trên tài khoản/máy thật.

[Google OAuth setup](https://developers.google.com/identity/gsi/web/guides/get-google-api-clientid)
và [GIS JavaScript reference](https://developers.google.com/identity/gsi/web/reference/js-reference).

## File bỏ / giữ và validation

Bỏ khỏi PR: toàn bộ `mqtt-gateway/` + test gateway, `tools/build_account_site.mjs`,
Worker ASSETS, ticket Ed25519/gateway session checks/renew topic, isolation gate và
gateway CI install. Khôi phục firmware và runtime host tests về branch gốc.

Giữ: landing/index/styles, `account.js`, account-auth/account-worker, migration 0004,
account membership trong app/Push/db, direct HiveMQ credential provisioning,
Worker package lock, account/API/CORS/Google/browser tests và CI kiểm regression.

Node tests và Chrome fixtures kiểm server signature/nonce/expiry/audience, ownership,
10 user × 3 device, claim/rate-limit, revoke/logout, exact-origin CORS, cross-origin
login/session, cache/retained bootstrap, WARM/socket reuse, HMAC command/signed ACK và
không HTTP mỗi click. Migration SQL chạy trong SQLite test; Worker dry-run bundle.
Build ESP32/ATtiny + host ASan/UBSan/ISR checks là CI gates. Giới hạn MQTT read/topic
ở trên vẫn còn, không có test nào tuyên bố broker shared credential đã tenant-safe.
