> **Đã gỡ khỏi firmware 2.0.0 (Thermal Control V2.0.0).** Tài liệu này mô tả hệ nhiệt cũ (≤ 1.1.15) đã bị xoá khỏi nhánh `thermal-control-v2.0.0`; chỉ còn giá trị lịch sử. Mã nguồn cũ nằm ở `main` (e8b557e) và nhánh `claude/gracious-hopper-zjvufg`. Xem `doc/THERMAL_V2_REMOVAL_REPORT.md`.

# Thermal Predictive V1 (firmware 1.1.15) — bộ điều khiển nhiệt dự đoán, tự học

Trạng thái: **đã tích hợp vào firmware, kiểm chứng trên mô phỏng chạy đúng mã firmware; CHƯA kiểm chứng trên lò thật.**
Mọi con số trong tài liệu này là của mô phỏng (lò ảo hai nút theo thông số người dùng cung cấp), không phải số đo trên lò.

## 1. Vì sao thay mới thay vì vá

Hệ cũ (PID + mô hình khởi động + Adaptive V1) là một chồng bản vá: mỗi lỗi sửa bằng một ngưỡng.
Triệu chứng người dùng thấy ("không bao giờ vọt nhưng cũng không bao giờ chạm SP", PV treo thấp hơn SP 0,2 °C hàng chục phút)
là hệ quả của cấu trúc: bộ điều khiển chỉ thấy **không khí**, còn khối trứng (~1,9 MJ/°C) mới là thứ quyết định
cần bao nhiêu công suất; mô hình một nút không biết điều đó.

Thermal Predictive V1 thay phần *quyết định công suất* bằng một bộ điều khiển dựa trên mô hình vật lý hai nút
được **nhận dạng liên tục trên chính lò đang chạy**. Hệ cũ vẫn chạy bên dưới và là đường lùi.

## 2. Mô hình

Đơn vị là **% của dàn nhiệt** nên không cần biết công suất thật (16–20 kW):

```
cf·dTa/dt = hl − a·(Ta − Tref) − k·(Ta − Te) + d − vp·(Ta − Tphòng)/12,5·quạt_hút   (không khí; hl = công suất đã trễ `dead` và lọc `tau`)
ce·dTe/dt = k·(Ta − Te) + w                                                           (trứng; w = nhiệt phôi)
```

- Tham số vật lý: `cf, ce, k, a, dead, tau, vp, Tphòng` (8).
- Nhiễu chậm: `d` (công suất ròng phía không khí), `w` (nhiệt phôi), `dr` (công suất thêm trong 15 phút gần nhất — cửa mở, phòng nóng lên, tải đổi), chênh `Ta0−Te0`.
- Nhiệt độ trứng **không đo được**: nó là trạng thái ước lượng (moving-horizon estimate) từ mô phỏng của mô hình khớp nhất.

## 3. Các khối

| Khối | Việc | Học hỏi từ |
|---|---|---|
| `History` | Vòng 10 s/bin: % bật SSR **thực** (đã đo), PV, quạt hút. 90 phút + 10 phút đệm. | — |
| `Identifier` | Tìm tham số khớp 90 phút gần nhất ((1+1)-ES, 1 mô phỏng/lần gọi, ~200 lần/s), prior log-chuẩn rộng (mọi lò cùng cỡ) hoặc chặt (mô hình mẻ trước). Bước "Newton" cho công suất thiếu từ độ dốc phần dư. | Adaptive tuning liên tục (Honeywell Accutune, Watlow TRU-TUNE+: chỉnh khi đang chạy, không cần "bấm tune") |
| `Planner` | ~260 phương án: luật cân bằng năng lượng + lệch `du` có cam kết `t1` phút + hằng số kéo `tau_c` ∈ {2, 3,5, 7, 15}×(dead+tau) + xả gió *chỉ khi heater một mình không đủ*. Mô phỏng 2 giờ, chọn rẻ nhất, áp bước đầu. Chi phí **bất đối xứng**: vượt SP đắt gấp 12 lần hụt, vượt quá 0,35 °C tăng bình phương. | Cutback của Eurotherm (giảm công suất *trước* khi tới SP; ở đây khoảng cách và độ sâu được **tính** từ trễ/quán tính đã học, không phải hằng số 3×PB), Fuzzy overshoot suppression của Honeywell (đổi đáp ứng tạm thời, không đổi hằng số PID), SIMC (τc ≥ θ) |
| Luật cân bằng | `u = a(Ta−Tref) + k(Ta−Te) − d + công suất quạt hút + cf(SP−Ta)/τc` | Cascade theo nhiệt độ *trứng* (Petersime OvoScan điều khiển theo nhiệt độ vỏ trứng) |
| Tích phân bù | Ki = (a+k)/(4θ) (SIMC: Ti = τ1 = cf/(a+k)), ±8 %, đóng băng khi bão hòa hoặc |lỗi| ≥ 0,4 °C | SIMC |
| Lịch quạt hút | Quạt hút theo lịch/biểu đồ là nhiễu **đã biết**: bù công suất trước `dead` giây. | Feed-forward |
| `Controller` (giám sát) | Learning → Active khi mô hình khớp (rms ≤ 0,10 °C, đủ kích thích, không chạm biên vật lý); giữ tới rms 0,20; xấu liên tục 5 phút → lùi về PID 20 phút rồi học lại. | — |

Nguồn đã kiểm tra khi viết: Eurotherm *Principles of PID Control and Tuning* (high/low cutback),
Honeywell UDC2800/UDC2300 (fuzzy overshoot suppression "does not change the PID constants, but temporarily modifies the internal controller response"),
Watlow CPC400 FAQ (TRU-TUNE+ overshoot reduction không dùng thành phần đạo hàm), Skogestad *The SIMC Method for Smooth PID Controller Tuning* (τI = min(τ1, 4(τc+θ)), τc ≥ θ).
Đây là mô tả công khai của hãng về *hành vi*, không phải mã nguồn của họ; cách hiện thực ở đây là của dự án.

## 4. Cái gì được học, cái gì cố định (nói thẳng)

**Học trên từng lò, từng mẻ:** nhiệt dung không khí, nhiệt dung và ghép nối trứng, tổn thất, trễ, quán tính phần tử, công suất quạt hút và nhiệt độ phòng,
công suất ròng, nhiệt phôi, nhiệt độ trứng hiện tại. Mô hình tốt được lưu (NVS, 2 ô, CRC, ghi lại kiểm tra) làm prior cho mẻ sau (chỉ vật lý, không lưu trạng thái trứng).

**Cố định trong mã (chọn khi thiết kế, KHÔNG học):** cấu trúc mô hình hai nút, các prior/biên, tập phương án của planner,
trọng số chi phí (12× vượt, đầu gối 0,35 °C), ngưỡng của giám sát (0,10/0,20/5 phút/20 phút), cửa sổ 90 phút/15 phút.
Nếu lò thật khác cấu trúc (ví dụ ba khối nhiệt), mô hình sai → giám sát lùi về PID; hệ **không tự đổi cấu trúc**.

## 5. An toàn: bộ này chỉ ĐỀ NGHỊ công suất

- Chỉ ghi đè `pidPower_` khi `Active`; mọi lớp sau giữ nguyên: trần khởi động, `effectiveLimit`, bộ chia xung SSR, trọng tài công suất,
  High/Emergency, contactor tổng, bảo vệ cảm biến. Quạt hút do bộ này yêu cầu cũng đi qua trọng tài quạt.
- Lùi về hệ cũ: giám sát (xem §3), tắt bằng cài đặt **"Nhiet du doan"** (HMI → NANG CAO), hoặc `predictiveThermalEnabled=false`.
- Không ghi flash từ tác vụ điều khiển: mailbox → `loop()` ghi (cùng kỷ luật `LaunchPlanStorage`).
- Cấu hình: bit 4 của byte `adaptive` (schema 13), **lưu đảo** nên bản ghi cũ đọc ra BẬT; mặc định BẬT.

## 6. Kiểm chứng (mô phỏng)

Công cụ: `tests/thermal-adaptive-sim.h` (chạy mã `updateHeatingAndOutputs` thật), lò ảo hai nút 72 cấu hình
(Cf 0,3–0,8 MJ/°C, hA 1000–1700 W/°C, quạt 1,5–2 kW, nhiệt phôi 0/1,5 kW, trễ 20/60/120 s), heater 20 kW.
Kịch bản: T1 mẻ đầu (28 °C, chưa có mô hình), T3 kích SP, T4 mở cửa −3 °C, T5 giai đoạn cuối có biểu đồ quạt hút + nhiệt phôi tăng, T6 phòng +5 °C **đột ngột**, T7 phòng +5 °C trong 2 giờ (chiều → tối).

KẾT QUẢ (mô phỏng, lò ảo hai nút, 1.1.14 so với 1.1.15; chi tiết và cách đọc ở `doc/THERMAL_AUDIT_V1.1.15.md`):

| Chỉ số | 1.1.14 | 1.1.15 |
|---|---|---|
| % thời gian trong ±0,1 °C, mẻ đầu | 86 % | 97 % |
| Kích SP: phục hồi | 293 s | 16 s |
| Mở cửa −3 °C: vọt tệ nhất | 0,63 °C | 0,16 °C |
| Giai đoạn cuối (nhiệt phôi tăng): phục hồi | 13199 s | 247 s |
| Phòng +5 °C: % trong dải / mẫu chạm High | 81 % / 189 | 97 % / 0 |
| Mẻ đầu: vọt / tụt tệ nhất | 1,66 / −4,16 °C | **1,66 / −4,16 °C (chưa đổi)** |
| Mẻ đầu: thời gian chạm SP−0,3 °C | trung vị ~15 phút | **~15 phút (chưa đổi)** |

Giữ nhiệt tốt hơn rõ; khởi động mẻ đầu **chưa** cải thiện (hệ cũ vẫn khởi động ~9 phút đầu). `tools/check_thermal_acceptance.py` đo 12 tiêu chí tuyệt đối theo triệu chứng
(chỉ báo cáo, chưa chặn CI): hiện 8/12 chưa đạt, toàn bộ nằm ở khởi động mẻ đầu và cú tụt sau khi chạm SP.

## 7. Giới hạn đã biết

1. **Mẻ đầu tiên của một lò mới, lò trễ ≥ 120 s và khối khí nhỏ:** hệ cũ vẫn là người khởi động cho tới khi mô hình khớp (~9 phút); hai thử nghiệm thay nó (khởi động tự đo độ lợi; vào điều khiển sớm) đã bị gỡ vì mất an toàn hoặc chậm hơn;
   giai đoạn này vẫn có thể vọt >1 °C. Mẻ sau dùng mô hình đã lưu.
2. Tách (nhiệt độ trứng, công suất ròng, nhiệt phôi) từ riêng nhiệt độ không khí là **không xác định duy nhất** ở trạng thái giữ;
   sai số nhiệt độ trứng ước lượng có thể ~1 K; tích phân bù và cửa sổ `dr` che phần lớn nhưng không triệt để.
3. Phòng đổi đột ngột 5 °C (không thực tế) làm mô hình khớp kém vài phút; kết quả mô phỏng vẫn không chạm High nhưng PV lệch tới ~0,5 °C.
4. Chưa đo trên lò thật. Chi phí CPU ước lượng trên ESP32-S3: nhận dạng ~0,5–1 ms/lần × 1 lần mỗi 10 ms, planner ~0,2–0,4 ms/phương án × 3 lần mỗi 10 ms (từ phép đo máy chủ, **chưa đo trên chip**).
5. Web chưa có công tắc "Nhiet du doan" (chỉ HMI).

## 8. Nghiệm thu trên lò thật (đề xuất)

1. Chạy một mẻ với công tắc BẬT, đọc PID Monitor: "Du doan: che do" phải đi LEARNING → DIEU KHIEN trong ~10–15 phút; "model rms" < 0,10.
2. Ghi lại PV 1 giờ ở trạng thái giữ: kỳ vọng |PV−SP| ≲ 0,1 °C. Nếu "LUI VE PID" xuất hiện lặp lại, tắt công tắc và gửi log. Quy trình chạy thử và ngưỡng tắt: `doc/FIELD_TRIAL_V1.1.15.md`.
3. Mẻ thứ hai (mô hình đã lưu) nên vào DIEU KHIEN sớm hơn.
4. Đo thời gian chu kỳ điều khiển (không được tăng quá vài ms).
