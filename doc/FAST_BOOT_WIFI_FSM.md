# Khởi động nhanh có điều kiện + một máy trạng thái Wi-Fi (firmware 1.1.4, nhánh `claude/alarms-over-mqtt`)

> Mọi số trong tài liệu này là **mô phỏng trên host** (thời gian ảo, mã thật được cắt từ firmware). **Thời gian khởi động trên ESP32 thật: NOT TESTED** — cần đo bằng log `[BOOT-STAGE]`, `[BOOT-HOME]`, `[SENSOR-FORMAT]`, `[MQTT] wss+mqtt up`.

## 1. Audit nguyên nhân (đọc mã, không đoán)

| # | Phát hiện | Hệ quả |
|---|---|---|
| A1 | **Home được mở bởi "3 tác vụ cục bộ khoẻ trong 4,5 s"**, không phải bởi cảm biến; `SPLASH_MAX_MS` = 6 s còn ép sang Home bất kể | Home hiện ở ~5,5 s trong khi cảm biến chưa dùng được ⇒ "READY giả" |
| A2 | Cảm biến RS485 ở chế độ AUTO cần **6 khung CRC-đúng nhất quán** để khoá định dạng rồi **3 mẫu tốt**, nhưng thăm dò mỗi **2 s** | khoá ≈ 13 s, dùng được ≈ 17 s (mô phỏng cũ: 17,4 s tối đa) |
| A3 | Tác vụ mạng chỉ khởi chạy sau 3 s ổn định cục bộ rồi **tuần tự** Wi-Fi → MQTT → Cloud → OTA, mỗi bước chờ ≥ 1 s | các tác vụ mạng đến ~8 s mới đủ |
| A4 | **Bốn đường vào cùng một quy trình phục hồi sâu** (xả mọi socket MQTT/Cloud/OTA rồi `WiFi.reconnect`): (1) mỗi lần thử Wi-Fi thất bại gọi `mayapRequestWifiDeepRecovery()`, (2) `WifiRecovery::wanted` (6 lần / 5 phút), (3) supervisor `ServiceWatch(Network)` im 30 s, (4) dừng sóng chủ động; thêm **hai** cơ chế cách ly (3 chu kỳ → nghỉ 120 s; supervisor `Isolate` nghỉ 30 s) | mỗi lần thử hỏng đều ngắt MQTT đang sống; nhiều cơ chế tranh quyền |
| A5 | MQTT: backoff riêng, **không** gọi Wi-Fi (kiểm bằng grep + test) | đã đúng, nay có test chặn hồi quy |
| A6 | `StableWifiState` (4 s debounce) chỉ ảnh hưởng **hiển thị/lỗi mức trình bày**, MQTT/Wi-Fi dùng trạng thái thô | giữ nguyên; không quyết định reconnect; nếu muốn bỏ hẳn cần quyết định riêng (HMI sẽ nhấp nháy) |
| A7 | Chế độ tiết kiệm điện: `MAYAP_WIFI_ECO` mặc định 0 ⇒ luôn PERFORMANCE | không đổi; test khẳng định chỉ có 1 chỗ gọi `esp_wifi_set_ps` |

## 2. Thay đổi

**Khởi động**
* Home chỉ mở bởi `HomeGate` (`boot_policy.h`, mã thuần, test host): nhiệt độ hợp lệ + ổn định **1,5 s liên tục**, cùng điều kiện cục bộ: tác vụ điều khiển/HMI/giám sát khoẻ, EEPROM, LCD, không ngắt an toàn. Không có đầu vào mạng.
* Quá **30 s** chưa đạt: màn hình **chẩn đoán** thay logo — mã cụ thể (`E101` cảm biến không trả lời, `E102` sai định dạng, `E103` chưa ổn định, `E301` EEPROM, `E990` ngắt an toàn, `E991` tiến trình, `E992` LCD) + dòng "ĐIỀU KHIỂN CỤC BỘ VẪN CHẠY". Hết nguyên nhân ⇒ tự vào Home. Bấm nút ⇒ vào Home (cảnh báo thật hiện ở đó), **không** báo READY.
* Cảm biến: cùng phép kiểm chứng (6 khung nhất quán + 3 mẫu tốt) nhưng thăm dò **600 ms** khi khởi động nguội (giới hạn 9 khung và cửa sổ 15 s, hoặc khi khung vẫn đến mỗi < 3 s) ⇒ cảm biến vắng mặt không bị hỏi dồn.
* Song song: tác vụ mạng bắt đầu sau 1,5 s khoẻ cục bộ (trước 3 s) và các bước dịch vụ cách 250 ms (trước 1 s); cảm biến được hỏi từ khi tác vụ điều khiển chạy ⇒ RS485 và Wi-Fi/MQTT khởi tạo đồng thời. Các mức phục hồi sau vòng lặp lỗi (8/10/45 s) **giữ nguyên**.

**Wi-Fi** (`wifi_fsm.h`, một chủ duy nhất `networkTask`)
`CONNECTING → CONNECTED → BACKOFF → RECOVERY`. Đầu vào chỉ là sự kiện Wi-Fi (đã kết nối, driver báo hết lượt thử, thời gian). MQTT/Cloud/OTA **không** là đầu vào.
* Mất kết nối thật: thử lại **ngay** một lần (nhẹ: `WiFi.reconnect()` tại chỗ, **không** xả socket, **không** WIFI_OFF/ON, không khởi tạo lại driver); hỏng thì thang 1/2/4/8/16/30/60 s + jitter.
* `RECOVERY` (nặng: xả chủ sở hữu rồi một lần reconnect tại chỗ) chỉ sau **6 lần thử hỏng hoặc 5 phút mất liên tục**, cách nhau ≥ 120 s.
* Xoá: yêu cầu phục hồi sau mỗi lần thử, cầu nối supervisor→Wi-Fi, `WifiRecovery`, hai cơ chế cách ly.

## 3. Kết quả mô phỏng host (không phải ESP32)

30 lần khởi động nguội, cảm biến tốt, jitter khởi động ngẫu nhiên (`tests/runtime-buses.cpp`, mã thật `SHT485Industrial` + `HomeGate` + `Sequencer`):

| Giai đoạn | Cũ (mô phỏng cùng harness, nhịp 2 s) | Mới |
|---|---|---|
| Khoá định dạng cảm biến | ≤ 13,4 s | **≤ 5,0 s** |
| Cảm biến dùng được | ≤ 17,4 s | **≤ 6,2 s** |
| Home (khi cảm biến thật sự sẵn sàng) | 16,9 – 19,4 s (nếu có cổng); **thực tế cũ ≈ 5,5 s không cần cảm biến** | **7,1 – 8,2 s** (TB 7,4 s) |
| Wi-Fi lên / MQTT lên (mô hình mạng, không phải mã thật) | ≤ 8,1 s / ≤ 16,1 s | không đổi (mô hình) |

Các kịch bản khác (đều khẳng định **không vào Home khi cảm biến chưa sẵn sàng**): cảm biến ấm máy chậm 12 s / 20 s ⇒ Home < 30 s; cảm biến đến sau 40 s ⇒ chẩn đoán `E101` đúng 30 s rồi tự vào Home khi có; cảm biến mất ⇒ chẩn đoán, không bao giờ READY; khung CRC lỗi ⇒ chẩn đoán cảm biến; từng điều kiện an toàn cục bộ; cổng không mở khi cảm biến chập chờn.
Wi-Fi (`tests/wifi-fsm.cpp`, `tests/runtime-network.cpp`): khởi động nguội, mất/ nối lại, chập chờn 200 lần (đúng 1 lần thử nhẹ mỗi lần mất, 0 phục hồi nặng), driver báo hết lượt thử, thang backoff, ngưỡng + cooldown của RECOVERY, MQTT hỏng nhiều giờ ⇒ **không** có hành động Wi-Fi, tràn `millis()`, 12 giờ mất mạng: số lần nặng ≤ 1 / 5 phút và không lần nào tắt/bật driver.

## 4. Giới hạn trung thực
* Chưa đo trên ESP32 thật: thời gian từng giai đoạn thật, chu kỳ hỏi 600 ms có hợp mô-đun SHT485 thật không (nếu mô-đun cập nhật chậm hơn, nhận diện vẫn đúng nhưng dài hơn), `WiFi.reconnect()` tại chỗ khi AP vừa rớt, RAM/stack (Flash/RAM xem báo cáo CI).
* Thời gian Wi-Fi/MQTT lên trong bảng là **mô hình**; thực tế phụ thuộc AP, DNS, NTP, TLS (đo 42 s trước đây ở điều kiện mạng xấu — xem `REALTIME_STABILITY_1_1_4.md`).
* Không đổi: logic nhiệt, SSR, quạt, đảo trứng, cảnh báo, phục hồi mẻ, điều khiển cục bộ độc lập Internet.

## 5. Vòng hoàn thiện (sau commit `30f9dff`)

| # | Tồn tại ở `30f9dff` | Xử lý |
|---|---|---|
| H1 | `hmi.h` vẫn tự thoát splash sau `SPLASH_MAX_MS` = 35 s nếu *không có chẩn đoán đang hiển thị* ⇒ coordinator đứng/chậm thì HMI vào Home khi cảm biến chưa READY | Xóa hẳn `SPLASH_MAX_MS`. Splash chỉ kết thúc qua `MayapBoot::splashView()`: (a) coordinator nhả Home (cảm biến hợp lệ + ổn định + an toàn cục bộ), hoặc (b) người vận hành bấm nút trên màn chẩn đoán. Sau `30 s + 2 s` mà coordinator chưa công bố Ready/chẩn đoán ⇒ HMI **tự vẽ E993 "BOOT COORDINATOR"**, không bao giờ tự vào Home hay báo READY |
| H2 | Vào Home từ màn chẩn đoán có thể bị hiểu là bỏ khóa heater | Không đổi gì ở đường điều khiển: `normalMasterPermit` / `heaterPidConditions` vẫn đòi `sensorUsable_`, không lỗi cắt tổng, `storageAllowsHeat`, `batchAllowsHeat`; `startup_output_policy` giữ mọi đầu ra an toàn đến khi HMI vẽ khung không-splash đầu tiên. Có test văn bản (`staged-boot.test.cjs`) và mô hình thứ tự (`boot-safety.cpp`) |
| W1 | `WifiFsm::update()` xóa lịch sử lỗi ngay khi `associated` ⇒ AP chập chờn (nối 1–5 s rồi rớt) bị thử lại mỗi ~1 s | Lịch sử (bậc thang, số lần lỗi, đồng hồ mất mạng) chỉ xóa sau **30 s CONNECTED liên tục** (`stableConnectedMs`). Nối rồi rớt trước 30 s = một lần thử thất bại: bậc thang 1/2/4/8/16/30/60 s tiếp tục tăng. Mất mạng sau ≥30 s ổn định: thử lại ngay như cũ |
| W2 | Không phân biệt mất sóng vật lý / mất IP | `LinkLoss::{Physical, Ip}` (từ `STA_DISCONNECTED` / `STA_LOST_IP`), ghi log `[WIFI] link lost kind=…`. Chỉ phục vụ chẩn đoán, hành động vẫn là cùng một thang. MQTT/Cloudflare không phải đầu vào FSM (kiểm bằng test văn bản trên `mayapNetworkUpdate`) |
| S1 | Nhịp 600 ms làm "6 chu kỳ lỗi → khởi động lại UART" đến sau ~6,6 s thay vì ~15 s | `fastCadence()`: không reinit UART trong cửa sổ nhịp nhanh; mốc ~15 s như cũ. Số khung xác minh (6 CRC-hợp-lệ-duy-nhất + 3 mẫu tốt) không đổi |

Mô phỏng host (không phải ESP32): `tests/boot-policy.cpp` (splashView), `tests/boot-safety.cpp`, `tests/wifi-fsm.cpp` (flapping 1–5 s, 30 s ổn định, loại mất mạng), `tests/runtime-buses.cpp::sensorHardening()` (đảo thang đo, CRC xấu, nhiễu + khung trùng, mất/khôi phục, chuyển nhịp nhanh → 2 s). **Thời gian khởi động thật trên ESP32 vẫn NOT TESTED.**
