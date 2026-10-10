# Báo cáo sau triển khai — quyết định D1–D6

Cơ sở: `main` thực tế = `8d51432`; nhánh `claude/gracious-hopper-zjvufg` nằm thẳng trên đó (0 commit phía sau, PR #14). Không quay về nhánh thermal cũ.
Không push main, không deploy, không nạp thiết bị. Oracle, ngưỡng High 38.2 / Emergency 39.0, baseline đóng băng: **không đổi** (A/B tái hiện baseline từng dòng: 788, 2160, holdout).
Phân loại: **IMPLEMENTED** = đã viết; **TESTED** = đã chạy và kết quả ghi bên cạnh; **BLOCKED** = không làm được ở đây; **PENDING APPROVAL** = chờ bạn duyệt, chưa đổi production.

## Kiểm thử đã hoàn tất trước khi kết luận
| Kiểm thử | Kết quả |
|---|---|
| CI `reliability` và `build` trên PR #14, head `4989dbb` | **cả hai xanh** (build ESP32-S3 thật: **Flash 1 432 549 / 1 480 000 B, RAM tĩnh 160 904 / 164 000 B**, build cờ mặc định = 0; đây là số đo đầu tiên trên toolchain chip; headroom Flash 47 451 B, RAM 3 096 B) |
| Host: A/B/C 788, 2160, holdout 360, vent, ventx, hwchange, faults, long-run | không đổi so với báo cáo cuối (546/788, 1397/2160, 282/360; High 33→18, Emergency 17→3) |
| Smart AutoTune 540 | 231 accepted, ACCEPTED_BAD 0, các dòng giống baseline đóng băng |
| Sensor-path 60 ca (adaptive + smart, ASan/UBSan) | cổng ratchet đạt |
| Test đơn vị mới: `heat-latch` 302 177, `program-store` 248, `thermal-program` 440 658, `output-budget` 11 (ASan/UBSan) | PASS |
| Chưa xác nhận | bước CI mới "Smart Thermal variants" (xem D1) chạy lần đầu trên lần push này |

## D1 — Smart Thermal commissioning
| Hạng mục | Trạng thái |
|---|---|
| `MAYAP_SMART_THERMAL=0` mặc định cho bản thử đầu | **IMPLEMENTED** (đã có từ trước; không đổi) |
| Build OFF giữ nguyên baseline | **TESTED** trên host (A/B tái hiện từng dòng; vân tay bảo toàn `thermal_control.h`/`runtime-recovery` xanh); firmware: CI build mặc định đạt ngân sách (số trên) |
| Build `=0` tường minh và `=1` trong CI, cùng ngân sách, in chênh lệch Flash/RAM so với build mặc định, `=0` phải trùng kích thước build mặc định | **IMPLEMENTED** (`.github/workflows/build-firmware.yml`, YAML hợp lệ, chạy thử phần sed trên log mẫu) — **chưa chạy trong CI** |
| Bật `=1` | **PENDING APPROVAL**: chỉ đề xuất khi CI + sanitizer + build + RAM/Flash + thời gian thực trên ESP32 + thử từng bước (`COMMISSIONING_TEST_PLAN.md`) đạt. Không tự kích hoạt trên máy thật. |
| Thời gian thực trên ESP32 (5 ms, stack, heap, WDT) | **BLOCKED** (cần đo trên chip; Giai đoạn 1.8 của kế hoạch nghiệm thu) |

## D2 — E115/E104
| | |
|---|---|
| Harness, trace, kiểm định cảm biến đứng/trôi, đánh giá giới hạn phát hiện | **IMPLEMENTED + TESTED** (`tests/thermal-safety-proposals.h`, `fpscan` trên 6 774 lượt chạy không lỗi, 60 ca lỗi cảm biến/cơ cấu; `phase8/detector-false-positive-*`, `latch-detectors-*`) |
| Logic hiện hành / đề xuất / false-positive / false-negative / latency / tác động heater-contactor | **IMPLEMENTED** trong `D2_D3_SAFETY_DESIGN.md` §1–2 |
| Kết quả | kiểm tra năng lượng: FP 1.2–9.7 % lượt chạy bình thường, drift trong dải không phát hiện được → **không đề xuất đổi E115/E104**; không coi nó thay cảm biến/thermostat độc lập |
| Thay đổi hành vi fail-safe E115/E104 | **PENDING APPROVAL** (không có đề xuất nào đủ tốt để trình) |

## D3 — Chốt contactor sau High lặp lại
| | |
|---|---|
| Nghiên cứu + kiểm định (phân biệt coast / lỗi cảm biến / High lặp / nghi SSR dính; tiêu chí không dùng N cố định; Emergency xử lý ngay, không đợi N) | **IMPLEMENTED + TESTED** (mô phỏng; bảng lựa chọn có FP/latency) |
| Bảng chuyển trạng thái thực thi được, ack chỉ từ HMI cục bộ (Web/MQTT/Cloud/watchdog/boot bị từ chối), khôi phục sau mất điện từ journal, điều kiện cho phép khôi phục | **IMPLEMENTED + TESTED** (`heat_latch.h`, `tests/heat-latch.cpp`, 302 177 phép kiểm; **không nối vào firmware**) |
| Khả năng cắt nguồn thật của contactor, trường hợp SSR dính / SSR + contactor dính | **IMPLEMENTED** (mô tả + số liệu): latch giảm thời gian trên High 3 594 s → 365–723 s; contactor dính ⇒ 108.6 °C, chỉ cầu chì/thermostat xử lý được |
| Thay safety contract (bảng lỗi, `updateAlarms`, `SafetyJournal`, HMI) | **PENDING APPROVAL** — cần bạn chọn luật/tham số, ack có PIN hay không, khóa NVS, mã sự kiện, phép thử phần cứng (§5 của tài liệu) |

## D4 — Quạt hút khi High
| | |
|---|---|
| Kiểm định phòng lạnh hơn / bằng / nóng hơn buồng | **TESTED** (mô phỏng, `phase8/vent-environment.csv`): quạt giúp khi phòng lạnh (thời gian trên High giảm 17–41 %), trung tính khi bằng, không đổi được kết quả khi phòng ≥ High |
| Không giả định quạt luôn làm mát; không có cảm biến phòng ⇒ giữ nguyên logic đã kiểm định | **IMPLEMENTED** (không thay đổi mã) |
| Chính sách mới | **PENDING APPROVAL** (cần số đo nhiệt phòng tại cửa hút khi commissioning; chưa mô hình hóa độ ẩm/CO₂) |

## D5 — Relay budget AutoTune
| | |
|---|---|
| Số liệu thật từ mô phỏng: một lần tune làm SSR đóng/cắt trung vị 4 078, p95 11 222, tối đa 21 867 lần (≤ 2 001 lần/10 phút); contactor và quạt **0** lần | **IMPLEMENTED + TESTED** (`phase8/autotune-output-edges.csv`, `D5_RELAY_BUDGET.md`) |
| Cơ chế đếm cạnh + thời gian ON/OFF tối thiểu + yêu cầu hủy, mặc định tắt (mọi giới hạn 0) | **IMPLEMENTED + TESTED** (`output_budget.h`, 11 phép kiểm), **chưa nối vào `updateAutoTune`** |
| Giá trị ngân sách từ linh kiện thực | **BLOCKED**: repo không có thông số SSR/relay/contactor; cần datasheet (liệt kê trong tài liệu) |
| Nối vào AutoTune (hàm có vân tay bảo toàn) | **PENDING APPROVAL** sau khi có số |

## D6 — Egg Cooling và Thermal Program
| | |
|---|---|
| Kiến trúc, kiểm thử, persistence A/B + CRC (torn write từng byte, wrap seq, schema lạ ⇒ mặc định OFF, giới hạn ghi) | **IMPLEMENTED + TESTED** (`thermal_program.h`, `egg_cooling.h`, `program_store.h`) |
| Egg Cooling mặc định OFF; Thermal Program **không tự đổi setpoint mẻ đang chạy** (chỉ tác động sau `activateForBatch(batchId)` xác nhận cho đúng mẻ đó, reconfigure/đổi mẻ/reboot không có xác nhận lưu ⇒ setpoint cấu hình) | **IMPLEMENTED + TESTED** |
| Không kích hoạt lịch làm mát/đổi nhiệt theo ngày trong đợt thử đầu | **IMPLEMENTED** (OFF; không có lịch mặc định) |
| Nối vào firmware: cấp địa chỉ EEPROM, màn hình HMI xác nhận theo mẻ, Web, mã sự kiện | **PENDING APPROVAL** |

## Còn mở, không bị chặn bởi quyết định
Chưa có: PRACTICAL / normal subset / extended normal (định nghĩa ở báo cáo WSL2); 12/24/72 giờ chạy thật; Smart AutoTune khi cờ BẬT; Node test cục bộ thiếu `cloudflare/node_modules` (CI đủ gói và xanh).
