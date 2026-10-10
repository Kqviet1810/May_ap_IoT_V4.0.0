# MAYAP V4 — Production Release Readiness

**Trạng thái cuối: FIELD TEST REQUIRED** (Release Candidate cho thử nghiệm; KHÔNG phải Production).
Không có phép đo nào trên chip hay trên lò thật đã được thực hiện. `PRODUCTION READY` chỉ được tuyên bố sau biên bản nghiệm thu máy thật đạt và sau khi bạn chấp thuận.

## 1. SHA và phiên bản
| Mục | Giá trị |
|---|---|
| Nhánh / PR | `claude/gracious-hopper-zjvufg` → PR #14 (base `main`) |
| RC SHA | commit chứa chính tệp này (đầu nhánh). SHA được in trong `BUILD_INFO.txt` của artifact build và trong tin bàn giao; mọi phép thử thật phải dùng firmware sinh từ đúng SHA đó |
| Phiên bản firmware trong mã | `1.1.4` (đồng bộ code/Web/manifest: `tools/check_release_sync.py` OK — firmware 1.1.4, HMI 1.0.0, Web 1.1.11, ATtiny v4) |
| Định danh RC | `1.1.4` + `MAYAP_BUILD_REVISION=<12 ký tự đầu của SHA>` (firmware in revision này). **Không đổi số phiên bản cho RC**: hành vi mặc định (cờ 0) không đổi; đổi số phiên bản là việc của commit phát hành, sau nghiệm thu |
| Phiên bản đề xuất khi phát hành | `1.1.5`, thực hiện cùng lúc ở `config.h`, `sw.js`, `release-manifest.json`, `README.md` (cổng `check_release_sync.py` bắt lệch) — **chưa làm, chờ bạn duyệt** |
| Cấu hình build cho RC thử nghiệm | **PILOT** (`workflow_dispatch`): `MAYAP_DIAGNOSTIC_SERIAL=1`, `MAYAP_SERIAL_INPUT_SIM=0`, `MAYAP_SMART_THERMAL` không đặt (= 0). PILOT cần cho thử thật vì chỉ bản có serial chẩn đoán mới in `[TASK]`/`[HEAP]`. Bản **PROD** (tag `v*.*.*`, `DIAGNOSTIC_SERIAL=0`) chỉ do workflow phát hành tạo, sau chấp thuận |
| Bản Smart ON | artifact riêng `firmware-SMART-ON-TEST-ONLY-<sha>` (`MAYAP_SMART_THERMAL=1`); chỉ nạp khi tải sưởi đã cô lập; **không bao giờ cho vận hành** |

## 2. Đã tích hợp thật trong firmware
* Toàn bộ baseline hiện hành của V4: PID, Adaptive Thermal V1, OutputArbiter (người ghi duy nhất GPIO1), alarm/High/Emergency/E115/E104, Safety Journal, MQTT/WSS độc lập vòng nhiệt, OTA qua Worker có xác minh SHA-256 và chữ ký, rollback hai slot, Smart AutoTune V1 (do người vận hành khởi động, HMI xác nhận).
* PR #14 thêm vào mã sản xuất **chỉ phần sau, tất cả bị chặn bởi cờ `MAYAP_SMART_THERMAL` (mặc định 0 = hành vi cũ từng bit)**: nhánh Smart của `ThermalStartupController`, hold fast-track và cổng "fast gain cũ không phải bằng chứng đổi plant" trong `thermal_learner.h`, `setSmartLearning` trong `thermal_adaptive_v1.h`, macro ở `thermal_assist.h`.
* Cờ 0 và tường minh `=0` có kích thước giống hệt bản mặc định (CI so từng byte Flash/RAM).

## 3. Còn tắt / chưa tích hợp
* `MAYAP_SMART_THERMAL=1` điều khiển heater: **tắt**, chặn bởi mục 7.
* D2–D6 (`heat_latch.h`, `output_budget.h`, `thermal_program.h`, `egg_cooling.h`, `program_store.h`): module thiết kế, **không được include bởi firmware**, chưa duyệt safety contract.
* Làm mát trứng và chương trình nhiệt theo ngày: tắt, không có đường kích hoạt.
* `PriorMinError` = 3.5 giữ nguyên (ứng viên 2.0 chỉ ở thử nghiệm mô phỏng; chưa có phê duyệt).
* Không thay đổi: High 38.2 °C, Emergency 39.0 °C, oracle, SP, thứ tự/cực tính SSR–contactor, hành vi fail-safe.

## 4. Kết quả CI và AutoTune (mô phỏng + CI; SOFTWARE VERIFIED)
CI (`reliability`, `build`) được kiểm trên đúng RC SHA và báo trong tin bàn giao (không ghi vào tệp này vì tệp thuộc commit đó). Nội dung `reliability` gồm: runtime-recovery/watchdog/boot/power-loss, EEPROM/NVS (schema 3..13, A/B + CRC), Wi-Fi FSM, bus I²C/RS485, Tiny controller, OTA config, Adaptive/PID/AutoTune, sensor-path, A/B/C, AutoTune Smart OFF **và ON**.
| Chỉ số (host, kết quả sau bản sửa Kh) | Giá trị |
|---|---|
| Smart AutoTune 540, **cờ OFF** | ACCEPTED_BAD **0**, 231 chấp nhận, High/Emergency 0/0 |
| Smart AutoTune 540, **cờ ON** (oracle sau-tune ở chế độ Smart) | ACCEPTED_BAD **0** (trước bản sửa: 1, ca `full165`), 231 chấp nhận, High/Emergency 0/0; P95 xấu nhất sau tune 0.120 (trước: 0.193) |
| Rollback AutoTune | cả 309 ca bị từ chối khôi phục PID/profile cũ; 84 lần cắt tức thời, 12 điểm mất điện: nguyên vẹn |
| A/B tái lập baseline đóng băng (788/2160/holdout) | giống từng dòng |
| Adaptive STANDARD A/B/C — 788 | 186 / 509 / 548 |
| — 2160 | 319 / 1242 / 1442 |
| — holdout 360 | 45 / 254 / 288 |
| PRACTICAL (định nghĩa tái dựng, khớp đúng 7 số tham chiếu) A/B/C — 788 / 2160 / holdout | 340/594/618 · 710/1464/1656 · 127/283/316 |
| NORMAL 788 / 2160, EXTENDED holdout (STANDARD, B → C) | 45→47 / 187→207 / 86→90 |
| Hồi quy B PASS → C FAIL (2160) | 48 → 41 (30 do khởi động Smart, 10 hold fast-track, 1 cần cả hai) |
| Vent 63×3, hwchange 16, faults 30, longrun 12 h/72 h | High/Emergency 0/0; `ventx` (quạt 600 W/K, phòng nóng hơn SP) 4/4 bằng bản Adaptive, đã biết |
| Unit/sanitizer, sensor-path | PASS (mã thoát 0) |

## 5. High/Emergency còn lại và liên quan đến lò thật
* 2160 ca mô phỏng: Adaptive V1 (**đây là hành vi cờ 0, tức bản RC vận hành**) có 33 High / 17 Emergency; Smart ON có 18 / 3. **Tất cả** thuộc miền hệ số gia nhiệt `Kh ≥ 0.133 °C/s ở 100 % công suất` (hiệu suất 1.5–2.0 trên khối lượng nhẹ nhất 180 kJ/°C); miền mô phỏng có Kh từ 0.005 đến 0.178.
* Liên quan đến lò thật: **chỉ nếu Kh đo được ≥ ~0.12 °C/s** (hiện chưa biết). Quy trình nghiệm thu đo Kh ở bậc công suất thấp trước; nếu Kh > 0.12 thì dừng và báo (xem `doc/FIELD_ACCEPTANCE_PROCEDURE.md`). Công suất giới hạn trong giai đoạn đầu (10 → 20 → 30 %) làm giảm năng lượng truyền vào trước khi biết Kh; đó là giảm rủi ro, không phải bảo đảm, nên vẫn cần giám sát và giới hạn dừng.
* 10 ca High từng được gọi là "giới hạn vật lý" thực ra tránh được bằng `PriorMinError` 2.0 nhưng đổi lại 9 ca kiểm soát kém hơn (`audit/smart-thermal/phase9/PRIOR_MIN_ERROR_9_CASES.md`); không áp dụng.
* Mô phỏng không thay thế bảo vệ độc lập: các ca cảm biến sai hợp lệ có đỉnh nhiệt 40.9–77.6 °C trước khi E115/E104 tác động; chỉ thermostat cơ độc lập chặn được.

## 6. Flash/RAM và kết quả runtime trên chip
* Flash/RAM tĩnh (CI, core esp32 3.3.11): xem log job `build` của RC SHA. Tham chiếu gần nhất: mặc định 1 432 549 / 1 480 000 B Flash, 160 904 / 164 000 B RAM; cờ 0 +0/+0; cờ 1 +52 B Flash, +0 B RAM (số đo ở commit trước bản sửa Kh; CI của RC SHA là nguồn xác nhận). Bản sửa Kh thêm một biến 4 B trong learner.
* Runtime trên chip (chu kỳ 5 ms thật và jitter, thời gian thực thi, stack high-water, heap, TWDT, power-loss, boot): **chưa đo — NOT TESTED**. Danh sách đo: `audit/smart-thermal/phase9/ESP32_NO_HEAT_CHECKLIST.md`.

## 7. Danh sách CHẶN XUẤT XƯỞNG (NO-GO) — trạng thái hiện tại
Tất cả đều **chưa được xác minh trên máy thật** nên đều đang là NO-GO cho Production. Mỗi mục có người xác nhận và bằng chứng yêu cầu.
| # | Điều kiện chặn | Phần mềm có gì | Còn phải làm trên máy thật |
|---|---|---|---|
| 1 | Chưa có/chưa kiểm chứng thiết bị ngắt quá nhiệt độc lập (cầu chì nhiệt + thermostat cơ, cảm biến riêng, cắt thẳng cuộn contactor) | không thể tự kiểm; firmware chỉ là một lớp | kỹ thuật viên có chuyên môn: ghi ngưỡng cắt, kiểm chứng riêng thiết bị trên bể/nguồn nhiệt có nhiệt kế chuẩn, **tải heater cô lập**, đo cuộn contactor nhả; ngưỡng thấp hơn 39.0 °C |
| 2 | Chưa xác minh khả năng ngắt công suất heater thật | OutputArbiter, drop SSR→master sau ≈120 ms (kiểm bằng host) | đo bằng thiết bị phù hợp: điện áp/dòng bằng 0 ở đầu tải khi SSR OFF + master nhả; không chỉ tin lệnh |
| 3 | Heater ON trái inhibit | một người ghi duy nhất (arbiter), test host | analyzer: GPIO1/GPIO14 không lên ON khi inhibit/boot/reset (W2, W4) |
| 4 | Cảm biến sai nhưng hợp lệ (kẹt/trôi trong dải) thiếu bảo vệ độc lập | E115/E104 chỉ bắt khi heater đã ON lâu; mô phỏng cho thấy giới hạn | mục 1 là lớp duy nhất; nghiệm thu so sánh cảm biến tham chiếu độc lập trong lò |
| 5 | Contactor đóng/ngắt lặp lại nguy hiểm khi SSR dính | High không chốt → master có thể đóng lại; **chưa có biện pháp được duyệt** (D3 là thiết kế chưa duyệt) | **phải duyệt D3 hoặc có biện pháp phần cứng thay thế** trước khi vận hành có trứng; không thử bằng cách nối tắt SSR |
| 6 | Reset/watchdog làm heater bật ngoài ý muốn | startup_output_policy, test host recovery | W1–W4 trên chip |
| 7 | Quá nhiệt thực tế / sai lệch nhiệt nguy hiểm chưa giải quyết | mô phỏng (mục 5) | nghiệm thu gia nhiệt có giám sát, cảm biến tham chiếu, dừng ở 38.2 °C tham chiếu |
| 8 | Bản chạy không đúng SHA RC | `BUILD_INFO.txt`, `SHA256SUMS.txt` | đối chiếu SHA-256 file nạp |
Quy tắc nghiệm thu máy thật cấm thử lỗi công suất cao bằng cách cố ý nối tắt cơ cấu bảo vệ; mục 1–2 kiểm bằng thiết bị đo ở tải cô lập.

## 8. Quy trình nghiệm thu và kết quả từng bước
Một quy trình duy nhất: `doc/FIELD_ACCEPTANCE_PROCEDURE.md`. Kết quả: **tất cả các bước NOT STARTED** (chưa có biên bản).
| Bước | Nội dung | Kết quả |
|---|---|---|
| 1 | An toàn điện và lớp ngắt nhiệt độc lập | NOT STARTED |
| 2 | Cô lập heater, trạng thái đầu ra/cảm biến/WDT (không nhiệt, hai cấu hình) | NOT STARTED |
| 3 | Gia nhiệt lò trống có giám sát, công suất giới hạn | NOT STARTED |
| 4 | Đặc tính nhiệt thật: Kh, dead time, coast, hold, phân bố nhiệt | NOT STARTED |
| 5 | Khởi động nguội/nóng, giữ nhiệt, quạt | NOT STARTED |
| 6 | Mất Wi-Fi/MQTT, mất nguồn, khởi động lại | NOT STARTED |
| 7 | Chạy dài hạn lò trống, biên bản | NOT STARTED |
| 8 | Smart ON (kiểm định riêng, chỉ sau khi baseline đạt) | NOT STARTED, không được phép trước bước 7 |

## 9. Quyết định phát hành
* Hôm nay: **FIELD TEST REQUIRED**. RC sẵn sàng để thử trên thiết bị khi bạn nạp (USB) bản cờ 0 PILOT sinh từ đúng RC SHA.
* **NO-GO cho Production** cho đến khi: mục 7 được xác nhận trên máy thật; biên bản nghiệm thu đạt; bạn chấp thuận.
* Sau chấp thuận mới được: merge PR vào `main`, tạo tag (`vX.Y.Z`), chạy workflow phát hành, kiểm artifact và chữ ký, đánh dấu Production. **Chưa thực hiện bước nào trong số này.**
* Cập nhật firmware/rollback: `doc/UPGRADE_ROLLBACK_GUIDE.md`. Ghi chú thay đổi: `doc/RELEASE_NOTES_RC.md`.
