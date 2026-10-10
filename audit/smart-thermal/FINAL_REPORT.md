# Smart Thermal — báo cáo cuối (giai đoạn phần mềm, MÔ PHỎNG)

Nhánh `claude/gracious-hopper-zjvufg`, xuất phát từ `8d51432` (main `313a7ac`). **Không merge main, không deploy, không nạp máy thật.**
Mọi số dưới đây là kết quả chạy thật trong phiên này trên đúng SHA ghi ở `final/qualification-manifest.txt` (HEAD `f696040` + các commit tài liệu sau đó);
cái gì chưa chạy được ghi `NOT TESTED`. Đây là bằng chứng mô phỏng (plant bậc nhất + trễ + trễ heater, chưa hiệu chuẩn), **không phải** nghiệm thu vật lý.

## 1. Kết luận ngắn
* **An toàn (mô phỏng):** số ca plant vượt High trong ma trận 2160 giảm 33 → 18, vượt Emergency 17 → 3, trên đúng cùng các plant. Không có ca nào xấu hơn; ngưỡng, oracle, SP,
  thứ tự SSR/contactor, arbiter, fail-safe **không bị đụng tới**. `ACCEPTED_BAD = 0` giữ nguyên (AutoTune không đổi).
* **PASS (oracle gốc, không đổi):** 788: 509 → **546**; 2160: 1242 → **1397**; tập **holdout 360 (không dùng khi chỉnh)**: 254 → **282**. Mục tiêu nghiên cứu ≥ 95 % STANDARD
  trong miền normal/reachable **chưa đạt**: REACHABLE 2160 đạt 1232/1535 = 80.3 %, holdout 250/290 = 86.2 %.
* **Phát hiện P1 mới** (harness cũ chấm High/Emergency bằng nhiệt độ thật của plant): các giới hạn của cảm biến đơn và của SSR dính — xem §4; cần quyết định của người duyệt (§6).
* **Mọi thay đổi thuật toán nằm sau cờ `MAYAP_SMART_THERMAL` (mặc định 0 = byte-identical hành vi hiện hành).** Rollback = build lại với 0.
* **Chưa làm được:** build firmware ESP32-S3 và đo Flash/RAM/timing trên chip (môi trường không tải được core Arduino-ESP32, xem §5). **Không thể coi giai đoạn phần mềm là "hoàn thành"** theo §24 cho tới khi CI build + đo chip đạt.

## 2. Bảng bắt buộc (§23)
| Metric | Legacy PID | Adaptive V1 | Smart Thermal New |
|---|---|---|---|
| Adaptive 788 STANDARD | 186/788 | 509/788 | **546/788** |
| Adaptive 788 PRACTICAL | NOT TESTED | NOT TESTED | NOT TESTED (định nghĩa PRACTICAL nằm ở báo cáo WSL2, không có trong workspace) |
| Adaptive 2160 STANDARD | 319/2160 | 1242/2160 | **1397/2160** |
| Adaptive 2160 PRACTICAL | NOT TESTED | NOT TESTED | NOT TESTED |
| Normal subset (788 / 2160) | NOT TESTED | NOT TESTED | NOT TESTED (tập con do báo cáo WSL2 định nghĩa) |
| Extended matrix (SP × ambient mở rộng) | NOT TESTED | NOT TESTED | NOT TESTED; thay bằng **holdout 360** (giá trị tham số ngoài cả hai ma trận): 45 / 254 / **282** |
| REACHABLE (788 / 2160 / holdout) | 139 / 304 / 45 | 387 / 1090 / 224 | **418 / 1232 / 250** |
| High (số ca, plant-true, 2160) | 33 | 33 | **18** |
| Emergency (số ca, plant-true, 2160) | 17 | 17 | **3** |
| High / Emergency (788) | 1 / 0 | 0 / 0 | 0 / 0 |
| Smart AutoTune ACCEPT (540) | relay cũ: 1/36 mini (nguội) | 231/540 | 231/540 (engine không đổi; cờ BẬT + AutoTune: NOT TESTED) |
| ACCEPTED_BAD | 0 | 0 | 0 |
| Vent tail MAE (63 ca, °C) | 0.376 | 0.102 | **0.093** |
| Vent 600 W/K tail MAE (21 ca) | 1.025 | 0.320 | 0.317 |
| Long-run MAE (mô phỏng 12 h/72 h, 8 ca, sau 6 h đầu, °C) | 0.380 | 0.090 | 0.090 |
| Hardware change (16 ca) tail MAE, High+Emergency | 0.458, 0 | 0.109, 0 | **0.083**, 0 |
| RAM (kích thước object, layout host x86-64) | — | StartupCtrl 588 B, Learner 1544 B | +8 B và +8 B = **+16 B** |
| Flash / RAM tĩnh trên ESP32-S3 | NOT TESTED | NOT TESTED | NOT TESTED (CI: Flash ≤ 1 480 000, RAM ≤ 164 000; headroom đã ghi ≈ 5 kB / < 1 kB) |
| CPU thêm (host, KHÔNG phải chip) | — | 148 ns / quyết định startup | ≤ ≈ 30 ns thêm / quyết định 2 s; learner: O(1) mỗi cửa sổ hold |
"B PASS → C FAIL" (9 / 48 / 8 ca ở 788 / 2160 / holdout) và "B FAIL → C PASS" (46 / 203 / 36) có trong `final/ab-*.csv` theo từng case ID.

## 3. Kiến trúc trước/sau
Đường quyết định công suất cuối **không đổi và duy nhất**: cảm biến → lọc → (ThermalObserver/Learner → AdaptiveV1 Assist) + StartupController (trần) + PID →
`min(pidPower, effectiveLimit)` → HeaterBurstScheduler (1 bank 16 kW) → **OutputArbiter** → GPIO1 / contactor tổng. Safety (High/Emergency/E101–E104/E115/inhibit/drop) đứng
sau các đề xuất và luôn thắng. Smart Thermal chỉ (a) cộng thêm phanh vào trần của StartupController (`max(ước lượng cũ, nhiệt-đang-bay tự hiệu chuẩn)` — kiểm chứng bằng
thuộc tính "trần Smart ≤ trần legacy" trên > 5000 quyết định) và (b) làm learner hội tụ hold nhanh hơn khi hai cửa sổ phẳng khớp nhau (không vượt 92 % của số đo thấp hơn,
≤ 3 pp/cửa sổ) và không coi "đang đuổi theo số đo" là "plant đổi". Không FSM mới: FullHeat/Approach/SoftLanding/Hold ≈ WARMUP/APPROACH/BALANCE, Learn states + VentCoordinator ≈ DISTURBANCE.
Không thay đổi định dạng lưu trữ (ThermalProfile 60 B, MachineConfig): không cần migration; lui về firmware cũ đọc được cùng bản ghi.

## 4. Phát hiện (chi tiết ở `PHASE0..6_*.md`)
| Mức | Phát hiện | Bằng chứng |
|---|---|---|
| P1 | Simulator cũ tính High/Emergency/inhibit từ nhiệt độ thật của plant, tức thời; firmware dùng `max(raw, filtered)` của cảm biến, xác nhận High 1 s | Phase 1 (`thermal-sensor-path` dùng chính mã sản xuất được trích tự động) |
| P1 | Cảm biến kẹt thấp (hợp lệ về giao thức): PID tiếp tục cấp nhiệt tới E115 (900 s ON) / E104 (20 phút) → buồng 40.9–77.6 °C | 8/8 ca; chỉ thermostat cơ chặn được |
| P1 | Cảm biến đứng/trôi chậm khi chỉ số nằm trong `[SP − hysteresis, SP)` không bị phát hiện (bằng chứng E115/E104 chỉ tích lũy khi PV < SP − hysteresis) | 10 ca UNDETECTED, đỉnh plant tới 41.1 °C (frozen) / 40.6 °C (trôi 2 °C/giờ) |
| P1 | SSR dính ON + contactor OK: contactor nhả/đóng quanh High, 10–25 lần, buồng ≥ High 2500–3600 s, ≥ Emergency tới 2264 s | 4/4 plant |
| P1 | SSR + contactor cùng dính: firmware không thể dừng (đỉnh 108 °C) | 4/4 plant |
| P2 | Nhiễu lọc E115 (heater bị cắt cả mẻ) khi đầu dò trễ 30–60 s | 4/16 ca không lỗi; còn 1/16 sau Smart |
| P2 | Startup đánh giá thấp lượng nhiệt đang bay cho heater mạnh hơn dự trù 35 % → 32/33 ca High là hiệu suất 2.0 trên 180 kJ/°C | Phase 0/2 |
| P2 | Learner tự báo "plant đổi" vì hold hội tụ 1 pp/cửa sổ: 227/445 ca REACHABLE FAIL của Adaptive V1 kết thúc với cờ mismatch (172 DEGRADED) | Phase 3 |
| Ghi nhận | Phòng nóng hơn SP: không có khả năng làm mát; mọi chế độ giống nhau (COOLING_REQUIRED) | Phase 4 |
| Ghi nhận | 18 ca High còn lại (10 ca sai số khởi động 2.5 °C ở 35 °C = giới hạn vật lý; 8 ca plant mạnh dead time 60–120 s chưa phân tích nguyên nhân) và 3 Emergency | Phase 2/3 |
| Ghi nhận | Smart AutoTune: 309 từ chối; 191 plant vẫn PASS khi điều khiển thụ động; 37 ứng viên "từ chối nhầm" (12 %), không đề xuất nới guard | Phase 5 |

## 5. Chưa chạy / chặn
* **Build ESP32-S3 + Flash/RAM + timing 5 ms + stack/heap + WDT trên chip: NOT TESTED.** `arduino-cli` 1.5.1 tải được từ GitHub, nhưng `downloads.arduino.cc` (index gói + công cụ built-in) bị proxy của container chặn (CONNECT 403) nên không cài được core `esp32:esp32@3.3.11`.
  Cần chạy workflow `build-firmware.yml` của repo (hoặc máy có mạng) với cờ `MAYAP_SMART_THERMAL=0` và `=1` (`--build-property compiler.cpp.extra_flags=-DMAYAP_SMART_THERMAL=1`).
* Bộ test Node (`node --test tests/*.test.cjs`): 17/240 lỗi do thiếu package `cloudflare/node_modules` (`@block65/webcrypto-web-push`, `jose`) trong container — không liên quan thay đổi này (không sửa JS).
* PRACTICAL, normal subset, extended normal của báo cáo WSL2: định nghĩa không có trong workspace → NOT TESTED (không bịa số).
* NOT TESTED: Smart AutoTune khi cờ BẬT; chạy thật 12/24/72 giờ (chỉ có giờ MÔ PHỎNG); mất điện giữa lúc ghi EEPROM với cờ BẬT (chỉ có thuộc tính đơn vị); HMI/Web cho Smart Thermal (không thêm gì, theo §20 chỉ sau khi core qua nghiệm thu).
* Egg cooling và chương trình nhiệt theo ngày: **thiết kế + module thuần, chưa tích hợp**, cờ mặc định OFF, 440 648 phép kiểm.

## 6. Quyết định cần người duyệt (dừng-và-hỏi theo §22)
| # | Quyết định | Ghi chú |
|---|---|---|
| D1 | Có bật `MAYAP_SMART_THERMAL=1` trong bản build thử/commissioning không? | Đề xuất: chỉ sau khi CI build + đo chip đạt và Giai đoạn 2 của `COMMISSIONING_TEST_PLAN.md` cho thấy Kh/hold của lò thật; lợi ích rõ nhất khi Kh > 0.12 °C/s hoặc learner hay báo mismatch giả |
| D2 | Mở rộng bằng chứng E115/E104 cho cảm biến đứng/trôi trong dải hysteresis (thay đổi hành vi fail-safe) | Hiện chỉ thermostat cơ chặn; đề xuất thiết kế riêng + thử nghiệm riêng, không nằm trong chương trình này |
| D3 | Chốt contactor sau N lần High liên tiếp trong cửa sổ cho đến khi người vận hành xác nhận (SSR dính) | Thay đổi fail-safe; mô phỏng cho thấy cần (40–60 phút trên High) |
| D4 | Chính sách quạt hút cưỡng bức khi High nếu phòng nóng hơn buồng | Cần số đo nhiệt phòng khi commissioning |
| D5 | Dẫn xuất ngân sách relay của AutoTune từ độ trễ đo được (99 RELAY_FAILED) | Nới guard ⇒ cần chứng minh hồi quy riêng; hiện không làm |
| D6 | Tích hợp chương trình nhiệt theo ngày / làm mát trứng | Cần quy trình ấp được duyệt + đo nhiệt trứng |
Không có thay đổi nào ở High 38.2 / Emergency 39.0, thứ tự/cực tính SSR–contactor, hoặc fail-safe hiện có.

## 7. Commit (nhánh, theo thứ tự)
| SHA | Nội dung |
|---|---|
| `d23e0fd` | Phase 0: baseline bất biến (509/788, 1242/2160, 231/540), sơ đồ dữ liệu, nguyên nhân 33 High/17 Emergency |
| `dc0092d`, `d200a63` | Phase 1: harness đường cảm biến (mã sản xuất được trích), 60 ca lỗi cảm biến/cơ cấu chấp hành, ratchet + CI |
| `d96add5`, `a671c9d` | Phase 2: phanh nhiệt-đang-bay tự hiệu chuẩn cho StartupController, test thuộc tính, công cụ A/B/C |
| `4af35e7` | Phase 3: hội tụ hold của learner + holdout 360 |
| `790eaab`, `79eff4a` | Phase 4: thông gió 600 W/K, phòng nóng, kiểm toán phần cứng |
| `46e7dc7`, `821e2b2` | Phase 5: nghiên cứu từ chối AutoTune |
| `2956059` | Phase 6: chương trình nhiệt + làm mát trứng (module thuần, chưa tích hợp) |
| `f696040` + tài liệu | Phase 7: chạy dài, kiểm định cuối, kế hoạch nghiệm thu |

## 8. Tái hiện
```
python3 tools/test_thermal_control.py --sanitize                 # 17 phút (ASan+UBSan), gồm thermal-smart-startup, thermal-program, adaptive-unit 745 phép kiểm
python3 tools/test_thermal_adaptive.py --full                    # cổng cũ (A/B không đổi so với baseline đóng băng)
python3 tools/test_smart_autotune.py --sanitize                  # 231/540, ACCEPTED_BAD 0
python3 tools/test_thermal_sensor_path.py --sanitize [--mode smart]
python3 tools/ab_smart_thermal.py                                # A/B/C + holdout + vent + ventx + hwchange + faults + long-run; tự kiểm baseline
python3 tools/autotune_reject_study.py --binary <report>/thermal-adaptive-v1
```
Bằng chứng: `baseline/` (đóng băng + SHA-256), `phase1-sensor-path-adaptive.csv`, `phase2/`, `phase3/`, `phase4/`, `phase5/`, `final/` (CSV từng case ID + `qualification-manifest.txt`).
