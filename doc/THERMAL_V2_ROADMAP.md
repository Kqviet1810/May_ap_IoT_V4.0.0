# Thermal V2.0.0 — lộ trình xây dựng (BẢN 2, SAU RÀ SOÁT ĐỘC LẬP — chờ chủ máy duyệt, chưa có mã điều khiển)

Nhánh `thermal-control-v2.0.0`. Firmware 2.0.0 hiện chỉ có **một khe** `commandedPower = 0` trong `updateHeatingAndOutputs()`.
Bản nháp 1 đã bị 4 người rà soát độc lập (điều khiển/vật lý, nhúng/an toàn, kiểm chứng/tiến độ, phản biện đơn giản). Họ tìm ra **8 lỗi chặn** trong bản nháp 1; bản này sửa cấu trúc, không vá. Một người rà soát đã dựng thử lò hai nút và chạy luật của bản nháp 1: **lên nhiệt mất 4 600–8 100 s so với 814–1 154 s vật lý** — đó là bằng chứng bản nháp 1 sai, không phải ý kiến.

## 0. Yêu cầu và chỉ tiêu (đề nghị duyệt)

| # | Yêu cầu | Chỉ tiêu bản 2 (đo trên nhiệt độ **không khí thật** của lò ảo, kèm số trên cảm biến) |
|---|---|---|
| R1 | Lên nhiệt nhanh | Chạm SP−0,3 °C trong ≤ 1,3 × (nhanh nhất vật lý của **chính lò đó**, bank 16 kW, bơm 100 % không điều khiển) + 120 s. Mẻ đầu tính cả thời gian nhận dạng trong cùng giới hạn, tối đa 12 phút cộng thêm |
| R2 | Không vọt | **Vọt ≤ 0,2 °C**, cố định. 0 lần chạm High (38,2) |
| R3 | Ổn định nhanh | Sau SP−0,3: ±0,3 °C trong ≤ 10 phút; ±0,1 °C trong ≤ 15 phút (lò đã ấm) / ≤ 30 phút (nạp trứng lạnh). **Thay cho "15 phút" cũ: tính toán cho thấy 15 phút với trứng lạnh thấp hơn giới hạn vật lý** |
| R4 | Ổn định lâu dài | Giờ cuối của mẻ 24 h: ≥ 95 % điểm trung bình 60 s trong ±0,1 °C; hụt ≤ 0,2 °C (cảm biến 0,1 °C nên chấm trên trung bình 60 s, không chấm từng mẫu lượng tử) |
| R5 | Phục hồi nhiễu | Mở cửa −3 °C: về ±0,3 °C trong ≤ 1,3 × (giới hạn vật lý của cú tụt đó) + 60 s (≈ 245–360 s với lò mẫu; **300 s cố định thấp hơn vật lý với trễ ≥ 180 s**). Nạp khay lạnh: không kẹt dưới SP |
| R6 | Bảo vệ cao nhất | High/Emergency/cảm biến/E115 không đổi; V2 chỉ **đề nghị** công suất; thêm **bộ giới hạn nhiệt-đang-trên-đường độc lập** (mục 2.1) |
| R7 | Đổi SP | Chỉ |ΔSP| ≤ 0,5 °C nằm trong R2/R3; thay đổi lớn hơn chạy như một lần lên nhiệt nhỏ có đường dốc SP |

## 1. Sự thật vật lý bản nháp 1 bỏ sót (quyết định cấu trúc)
1. **Hold không phải 12–28 % khi trứng còn lạnh.** Công suất giữ SP = [UA(SP−Tamb) + hA(SP−Te)]/Pmax. Trứng ở 28 °C cần **62–104 %** bank; ở 33 °C: 31–51 %; ở 36 °C: 12–19 %. Một "hold nhớ" kiểu số vô hướng sẽ làm lò **kẹt dưới SP hàng giờ** (đúng triệu chứng của 1.1.15). ⇒ phải có **một biến trạng thái nhiệt độ trứng** ước lượng (ước một trạng thái, không phải mô hình 12 tham số).
2. **Nhiệt đang trên đường lớn hơn dự tính:** tốc độ lên tới 0,02–0,05 °C/s, trễ + quán tính ≈ 190 s ⇒ 3–4 °C còn đang tới khi cắt. Vọt ≤ 0,2 °C chịu sai số dự đoán ≤ ~6 %. ⇒ không thể "đạp tới sát SP rồi cắt".
3. **Bảo vệ không cứu được nhiệt đang trên đường:** High 38,2 / Emergency 39 so với PV đo; sau khi PV chạm 38,2 vẫn còn ~3 °C sắp tới ⇒ ~41 °C. ⇒ cần bộ giới hạn độc lập, không phụ thuộc bộ điều khiển đúng hay sai.
4. **Công suất lệnh ≠ công suất giao:** cổng cho phép (quạt chưa ổn định, khóa khởi động lại 30 s, contactor, E115…) có thể cắt. Dự đoán phải dùng **năng lượng đã giao thật** (`burst.groupA && permit`).
5. **E115 là cái bẫy:** SSR bật tích lũy 900 s mà PV chưa tăng 0,3 °C ⇒ lỗi Stop cắt contactor. Chế độ "kẹp công suất" ở hold 12 % có thể tự kích hoạt lỗi sau ~54–125 phút. ⇒ V2 đọc tỷ lệ tích lũy E115 và leo công suất khi ≥ 40 % (điều kiện thoát suy ra từ ngưỡng E115, không phải số chọn tay).
6. **Cảm biến:** mẫu thật 0,5 Hz (2 s), lọc median-3 + IIR ≈ 6 s trễ nhóm; offset âm làm bảo vệ (đo thô) và điều khiển (đã bù) ở hai miền khác nhau ⇒ V2 nhận `headroomC` đã tính sẵn từ cấu hình.

## 2. Thiết kế bản 2: một luật, không bàn giao, không bộ khác

**Vào:** PV đã lọc + số thứ tự mẫu + thời điểm mẫu (không dùng cờ bool), SP, `headroomC`, cờ cho phép + trạng thái quạt (tuần hoàn, hút, `warmupVentHold_`), trần `maxHeaterPower`, tỷ lệ tích lũy E115, năng lượng đã giao.
**Ra:** `commandedPower` (%), mã trạng thái, cờ "công suất bị giới hạn" (không phải lỗi). Một khối vào – một khối ra – tự kiểm; bộ chia xung/arbiter/bảo vệ không đọc nó.

**Luật duy nhất (mọi trạng thái là cùng một phương trình ở khoảng cách khác nhau):**

`u = u_ff + Kc·(SP − PV_dự_đoán) + I`, bão hòa [0, trần]

- `PV_dự_đoán = PV + k_air · Σ w_i·(P_giao,i − u_cb,i)` trên cửa sổ **(θ_d + 3·τ_lag)**, trọng số w theo đáp ứng xung của trễ + quán tính (không phải cửa sổ chữ nhật).
- `u_ff` = công suất cân bằng tổn thất `[UA(PV−Tamb) + hA(PV−Te)]/Pmax` **cộng** phần đẩy nhiệt vào trứng `β·hA(SP−Te)/Pmax` (β ≈ 0,7, hệ số tin cậy). `Te` là **một** trạng thái ước lượng: `Te' = hA·(PV−Te)/Ce`, khởi tạo bằng PV khi lò đã phẳng. Tamb = PV lúc phẳng đầu mẻ.
- `Kc, τI` theo **FOPDT SIMC**: `Kc = τ1/(g(τc+θ_eff))`, `τI = min(τ1, 4(τc+θ_eff))`, `θ_eff = θ_d + τ_lag/2`, `τc ≥ θ_eff`. (Công thức tích phân thuần + "bỏ min()" của bản 1 làm tích phân chậm gấp 6 lần.) Sẽ kiểm biên pha (PM ≥ 40°) khi `k̂ ∈ [0,3; 1,0]·danh định`, `θ ∈ [0,7; 1,4]·danh định` trong test đơn vị.
- **Một bộ tích phân không bao giờ reset**; mọi sự kiện: `I := u_trước − u_ff − Kc·e` (êm theo xây dựng). Chống bão hòa: tích phân có điều kiện (đóng băng khi bão hòa cùng chiều) + kẹp |I| ≤ 60 điểm.
- **Bất đối xứng công khai** (L4): hệ số `Kc_lên = 3·Kc` khi `PV_dự_đoán > SP` (vọt tệ hơn hụt). Không có luật bất đối xứng ẩn nào khác.
- **Giới hạn ngân sách vọt có công thức:** `(u − u_ff) ≤ 0,2 / (ε̂·k̂·θ̂)` với `ε̂` = sai số dự đoán tương đối đo được từ chính bộ dự đoán (|PV − PV_dự_đoán(t−θ)| / lượng nhiệt đang trên đường). Sai số ước lượng k càng lớn ⇒ ga càng bị hạ ở đoạn cuối. Không có chế độ "tin/không tin", không CUSUM, không đường lùi khác.
- **Nhận dạng ngay trong lúc lên nhiệt, không có trạng thái ĐO MÁY riêng:** θ̂ = mẫu đầu vượt 3σ_lượng_tử sau khi bật; `k_air` = hồi quy trên cửa sổ 60–120 s sau θ̂ (trước khi trứng hút nhiệt); `g, τ1` từ phần sau của cùng đường cong. Hai độ lợi tách riêng (không nhồi một tỷ lệ độ dốc/công suất vào cả hai chỗ). Cập nhật chỉ khi có kích thích (P giao > 30 % **và** độ dốc > 4Δ/W); **đóng băng sau lên nhiệt**.
- **Khởi động gần SP** (khoảng cách < ~4 °C, không đo được độ dốc trước khi phải cắt): dùng tiên nghiệm danh định (bank 16 kW, v_max ≈ 0,02–0,05 °C/s, θ_hi = 180 s) + giới hạn ngân sách vọt. Mẻ đầu **không** đoán bừa: bị chặn bởi bộ giới hạn độc lập.
- **Nhiễu:** mở cửa = cùng luật (khoảng cách lớn ⇒ công suất lớn); nạp trứng lạnh = Te kéo `u_ff` lên, không kẹt; quạt hút: **hoãn lệnh quạt θ_d giây trong khi nâng ga trước** (lịch quạt biết trước trong firmware) thay vì bù phản ứng; sụt áp = `k_air` đổi, ε̂ phình ⇒ ga bị hạ; mất cho phép (cảm biến/contactor) = **đóng băng, không phát công suất**, khi trở lại sau > N giây tính lại từ PV mới; mất điện = khởi động lại từ PV (RAM, không NVS).

### 2.1 Bộ giới hạn độc lập (R6) — mới
Sau bộ điều khiển, trước bộ chia xung; **không điều chỉnh được bởi bộ điều khiển**; chỉ có thể **hạ** u. Hằng số trường hợp xấu cố định theo vật lý: `k_hi = 0,02/80 = 2,5e-4 °C/s/%`, `θ_hi = 180 s`, `h_lo = 12 %`; kẹp u sao cho `PV + k_hi·∫(đã giao − h_lo) + k_hi·θ_hi·(u − h_lo) ≤ min(High, ventOn) − headroom`. Mục đích: High/Emergency không phụ thuộc V2 đúng hay sai.

## 4. Những gì bị **bỏ khỏi V2.0.0** so với bản nháp 1 (để đơn giản hơn, sửa được sau khi có log thật)
ĐO MÁY (trạng thái riêng, 5–8 phút) · cập nhật k' liên tục · CUSUM tin/không tin · chế độ bảo thủ riêng · trạng thái PHỤC HỒI riêng · bàn giao WARM→HOLD · hằng m · lưu NVS hold · Commissioning · dữ liệu danh định công suất nhập tay · bù quạt phản ứng. Mỗi mục có thể thêm ở V2.1 **khi log thật chứng minh cần**.

## 5. Kỷ luật kiểm chứng (cơ chế, không phải lời hứa)
1. `doc/THERMAL_V2_CONSTANTS.md`: mỗi hằng số có dòng "suy ra từ đâu" (lượng tử Δ/W, θ, τ, vật lý). Không suy ra được ⇒ không vào mã.
2. **Tập A** (thiết kế, 12 lò, gồm lò suy ra từ log thật của chủ máy: hold 1,9 %, Cf 0,3 & 1,2 MJ, Pmax 16 & 20 kW, trễ 60–180 s); **B** (kiểm, tham số sinh từ file seed **commit trước G2**); **C** (8 lò sai cấu trúc: ba nút, cảm biến trễ thêm 20–60 s, k ±30 %, ghép trứng đổi theo quạt, heater phi tuyến/sụt áp, lượng tử 0,1 °C). Runner B/C **in ra hash commit bộ điều khiển và từ chối chạy khi cây bẩn**, đếm số lần đã xem B/C; sau mỗi thay đổi cấu trúc bốc seed mới.
3. Mô phỏng viết mới (`sim.h` ~200 dòng, không vá `thermal-adaptive-sim.h` 615 dòng), **liên kết `heater_burst_scheduler.h` thật**, chép nguyên văn khối E115, mô hình cổng cho phép (khóa 30 s, min-off 3 s, pickup 0,5 s, vent 38,0/37,6), High/Emergency trên miền đo thô, bộ lọc cảm biến thật.
4. `acceptance.json` **viết lại ở G0** theo R1–R7 trước khi có mã; chấm cả trên nhiệt độ thật và trên cảm biến.
5. Test vòng kín bảo vệ (đã xoá cùng hệ cũ) dựng lại ở G4: ép công suất tối đa với lỗi cảm biến (đứng, trôi, nhảy, mất), SSR kẹt, contactor kẹt ⇒ lớp bảo vệ phải cắt; khi cho phép = false không có lệnh sưởi lọt ra; cổng CI: 0 E115, 0 High, 0 Emergency.
6. Thất bại ở B/C ⇒ truy nguyên nhân công thức, không thêm ngưỡng riêng cho ca.

## 6. Giai đoạn, thời gian thực tế, tiêu chí qua

| GĐ | Việc | Thời gian | Tiêu chí qua |
|---|---|---:|---|
| G0 | Chốt đặc tả + `THERMAL_V2_CONSTANTS.md` + viết lại `acceptance.json` + seed A/B/C; kiểm công thức SIMC/FOPDT và nguồn | 25' | Bạn duyệt; kiểm biên pha bằng số |
| G1 | `sim.h` mới + harness (burst thật, E115, cổng cho phép, lọc) + công cụ chấm + "nhanh nhất vật lý" từng lò | 55' | Bộ điều khiển bang-bang tầm thường chạy được; số nhanh nhất khớp tính tay |
| G2 | `thermal_v2.h` (C++ thuần): ước Te, nhận dạng θ/k/g/τ, luật duy nhất, chống bão hòa, ngân sách vọt, bộ giới hạn độc lập; test đơn vị (lượng tử, trễ, bão hòa, đổi SP, tràn `millis`, khe vắng mặt) | 80' | Test xanh; tập A: R1–R3 đạt |
| G3 | Tập B + C, nhiễu (cửa, khay lạnh, quạt, sụt áp, nhiệt phôi), 24 h trên 6 lò, 4 h các lò còn lại | 35' | R1–R5 đạt trên B; C: 0 High/Emergency/E115, vọt ≤ 0,2 hoặc báo rõ ca và nguyên nhân |
| G4 | Vòng kín bảo vệ (mục 5.5) | 20' | Đạt mục 5.5 |
| G5 | Nối vào khe: đầu vào (mẫu theo số thứ tự, headroom, E115, quạt, năng lượng giao), telemetry `pidMon` (khóa ngắn, < 1 700 B), **vòng nhớ log ≤ 8 KB + kéo theo khung ≤ 1 024 B + nút tải CSV trên Web**, `slotUs_max`, HMI 3 hàng; cờ cảnh báo E113 (mục 7, câu 3) | 50' | CI xanh (biên dịch thật), Flash/RAM trong ngưỡng, chu kỳ điều khiển đo bằng `esp_timer` hiển thị |
| G6 | Rà soát độc lập lần 2 trên mã thật + sửa + bản `2.0.0-rc`; cổng tuyệt đối chuyển từ báo cáo sang **chặn** CI | 30' | Không còn lỗi mức nghiêm trọng |
| G7 | **Thử lò thật** (bạn chạy, tôi đọc log): `doc/FIELD_TRIAL_V2.md` | ngoài | R1–R5 trên lò thật |

**Tổng G0–G6 ≈ 4 giờ 55 phút** (trung thực; bản nháp 1 ghi 3 giờ 25 là thiếu: rà soát nói 5–6,5 giờ nếu giữ mọi mục cũ, đã cắt 11 mục ở mục 4 để còn ~5 giờ). Muốn đúng 3 giờ phải bỏ G3 24 h, G4 và rà soát lần 2 — **tôi không khuyên**: đó chính là chỗ các bản cũ hỏng.

## 7. Điều tôi cần bạn quyết (mỗi mục trả lời "đồng ý / đổi thành …")
1. **Chỉ tiêu mới** R1–R7 ở mục 0, nhất là R3 (±0,1 °C trong 30 phút với khay lạnh thay vì 15 phút) và R5 theo vật lý thay vì 300 s cố định.
2. **Thời gian:** ~5 giờ làm kỹ (khuyến nghị) hay ép 3 giờ bỏ kiểm tra (không khuyến nghị).
3. **Cho phép một thay đổi cảnh báo:** E113 (nhiệt tăng nhanh bất thường) sẽ nổ liên tục khi lên nhiệt nhanh tối ưu 0,02 °C/s. Đề nghị: không đặt E113 khi độ dốc **dương** trong lúc V2 đang lên nhiệt/phục hồi và có cho phép; độ dốc âm vẫn báo. Không đụng ngưỡng cắt.
4. **Một giá trị người dùng thứ tư:** số trứng / số xe (để lấy hA, Ce của trứng). Mặc định 4 xe ≈ 10 000 trứng như lò mô phỏng của bạn; nếu không nhập thì V2 chạy với β thấp hơn (lên nhiệt chậm hơn nhưng an toàn). Bạn có số thật không?
5. **Bộ giới hạn độc lập (2.1)** là lớp hạ công suất mới, không thay lớp High/Emergency. Đồng ý đặt?
6. **Log Web:** thêm nút tải CSV (vòng nhớ ≤ 8 KB ≈ 2,8 giờ ở 10 s, kéo theo khung) — chấp nhận đây là cách duy nhất để đóng vòng với lò thật.
7. **Thử lò thật** theo thứ tự: bench kiểm cắt High/Emergency (không sưởi) → lò rỗng, trần 25 %, SP 33 °C, có người đứng cạnh công tắc tổng → nâng trần 60 % → 100 % → SP 37,5 → có trứng. Bạn có đứng cạnh lò được không?

## 8. Rủi ro còn lại (nói thẳng)
- Chưa có số đo lò thật; mọi kết luận ở G3 là trên mô phỏng độc lập, **không phải bằng chứng lò thật**. G7 có thể buộc quay lại G2.
- `β`, `hA`, `Ce` của trứng là tiên nghiệm; sai nhiều thì lên nhiệt chậm hoặc sụt khi nạp khay lạnh (vẫn không vọt vì bộ giới hạn).
- Không còn bộ dự phòng khác: nếu V2 kẹt thì lùi bằng cách nạp lại 1.1.15 (còn nguyên ở `main`).
