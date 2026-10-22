# Thermal V2.0.0 — lộ trình xây dựng (BẢN NHÁP ĐỂ DUYỆT, chưa có dòng mã điều khiển nào)

Nhánh `thermal-control-v2.0.0`. Hiện firmware 2.0.0 chỉ có **một khe** `commandedPower = 0` trong `updateHeatingAndOutputs()`.
Bản này thay phần "thiết kế MPC" cũ trong `THERMAL_V2_DESIGN.md` (bộ lập kế hoạch 260 phương án, nhận dạng 12 tham số bị xoá cùng hệ cũ). Lý do ở mục 2.

## 0. Yêu cầu (chủ máy)

| # | Yêu cầu | Chỉ tiêu đo được (đề xuất, cần duyệt) |
|---|---|---|
| R1 | Lên nhiệt nhanh | Từ 28–33 °C tới SP−0,3 °C trong ≤ 1,3 × (mức nhanh nhất vật lý của chính lò đó) + 120 s |
| R2 | Không vọt | **Vọt ≤ 0,2 °C** (cố định, đã chốt). Mẻ chạm High (38,2 °C) = 0 lần |
| R3 | Ổn định nhanh | Sau khi chạm SP−0,3: vào và ở lại trong ±0,1 °C của SP trong ≤ 15 phút |
| R4 | Ổn định lâu dài, bất chấp nhiễu | Giờ cuối của mẻ 24 h: ≥ 95 % mẫu trong ±0,1 °C; hụt ≤ 0,2 °C |
| R5 | Phục hồi | Mở cửa (−3 °C): về lại ±0,3 °C trong ≤ 300 s; trứng nạp lạnh: không kẹt dưới SP |
| R6 | Bảo vệ vẫn cao nhất | High/Emergency/cảm biến/E115 không đổi; bộ điều khiển chỉ **đề nghị** công suất; một khối vào–một khối ra, tự kiểm |

## 1. Ràng buộc thực tế (nói thẳng)
- Chưa có số đo lò thật ngoài 7 ảnh PID Monitor của 1.1.15. Mọi con số từ mô phỏng **không phải bằng chứng** (bài học L6). Bằng chứng chỉ có khi chạy lò thật; bạn chỉ xem được qua Web ⇒ phải có **log tải về từ Web**.
- Lò: bank SSR 16 kW một GPIO, bước chia xung 300 ms, cảm biến SHT 0,1 °C (có thể 0,01), trễ vận chuyển + quán tính 100–180 s, hold 12–28 % (số liệu RG144 đã đo), khối trứng ≈ 1,9 MJ/°C.
- Vọt ≤ 0,2 °C với trễ 150 s và tốc độ lên tới 0,02 °C/s nghĩa là nhiệt "đang trên đường" tới ≈ 3 °C: sai số 7 % của dự đoán đã bằng 0,2 °C. Vì vậy **không thể** "đạp ga tới sát SP rồi cắt"; phải **hạ ga theo dự đoán** (mục 3.3). Đánh đổi: tốn thêm vài phút ở đoạn cuối, đổi lại vọt thấp.

## 2. Vì sao không dùng lại MPC/Predictive 1.1.15
Planner 260 phương án + nhận dạng 12 tham số + hồ sơ học là chính cấu trúc đã cho kết quả: học sai, đường lùi là bộ khác, ~100 hằng số. V2 dùng **ít thành phần, mỗi thành phần có công thức suy ra được và có tự kiểm**. Không sao chép logic cũ.

## 3. Thiết kế V2 (một khối quyết định: 1 vào – 1 ra – tự kiểm)

**Vào:** PV (đã lọc), SP, độ phân giải cảm biến, cờ cho phép (batch, sensor ok, không bị chặn), trạng thái quạt hút/quạt tuần hoàn, trần công suất `maxHeaterPower`, thời gian.
**Ra:** `commandedPower` ∈ [0, trần] (%) + mã trạng thái + cờ tự kiểm. Bộ chia xung/arbiter/bảo vệ nằm ngoài và không đọc giá trị này.

### 3.1 Trạng thái
`ĐO MÁY → LÊN NHIỆT → GIỮ NHIỆT` và `GIỮ AN TOÀN` (vào từ bất kỳ trạng thái; ra khi điều kiện hết).
Thêm trạng thái con `PHỤC HỒI` (trong GIỮ NHIỆT khi sai số < −X) dùng **cùng** công thức LÊN NHIỆT, không phải bộ khác.

### 3.2 ĐO MÁY (chỉ khi cần: mẻ mới không có mô hình lưu hợp lệ)
1. Chờ PV phẳng (độ dốc < ngưỡng suy ra từ σ cảm biến và cửa sổ đo) — đặc biệt sau khi nạp trứng.
2. Xung đầu có **năng lượng bị chặn** (không quá E_max suy ra từ khoảng cách tới High và độ lợi tối thiểu có thể).
3. Đo **trễ θ** (thời điểm PV vượt nhiễu σ), **độ dốc k'** (°C/s trên 1 % công suất, hồi quy trên cửa sổ sau θ). Ước lượng có khoảng tin cậy; không đủ tin cậy ⇒ xung thứ hai lớn hơn nhưng vẫn bị chặn, cuối cùng thất bại ⇒ chế độ bảo thủ (mục 3.6) + báo.
4. Nếu có mô hình lưu từ mẻ trước (θ, k', hold) cùng chữ ký phần cứng ⇒ dùng làm giá trị khởi đầu và **bỏ qua** xung đo (vẫn tự kiểm).

### 3.3 LÊN NHIỆT (thời gian tối ưu, có cắt dần)
- Nhiệt "đang trên đường": `E = ∫ công suất thực trong θ giây gần nhất`. Đỉnh dự đoán nếu cắt ngay: `PV_pk = PV + k'·E_eff − tổn thất`.
- Công suất = `min(trần, (SP − m − PV)/(k'·θ_eff))` hạn chế bởi dự đoán đỉnh (cắt mềm): xa SP thì full, gần SP thì giảm tuyến tính để `PV_pk ≤ SP − m`, m suy ra từ sai số k' đo được (không phải hằng số chọn theo ca).
- k' được **cập nhật liên tục trong chính đoạn lên nhiệt** (độ dốc thật so với công suất đã trễ) — tự sửa ngay trong lúc đi, không đợi mẻ sau.
- Chuyển sang GIỮ NHIỆT khi `|SP − PV_pk| < ngưỡng` và độ dốc nhỏ; **tích phân khởi tạo = hold đã nhớ** (không phải 0) ⇒ không giật.

### 3.4 GIỮ NHIỆT
- PI chỉnh theo SIMC cho đối tượng tích phân + trễ: `Kc = 1/(k'(τc+θ))`, `τI = 4(τc+θ)`, `τc = 1,5θ` (sẽ **kiểm lại công thức với nguồn gốc trước khi mã hóa**).
- Cấp trước (feed-forward): hold đã nhớ; **bù quạt** (mở quạt hút = tổn thất tăng, bù theo công suất đã đo, không theo ngưỡng tay).
- Chống bão hòa tích phân (back-calculation); công suất tối đa = trần.
- Hold nhớ chỉ cập nhật khi PV đã phẳng trong cửa sổ đủ dài; **tốc độ tăng = tốc độ giảm** (quy tắc L4); lưu NVS **chỉ từ `loop()`**, hiếm, không từ tác vụ điều khiển.

### 3.5 Nhiễu đã tính tới
Mở cửa/hụt nhanh (PHỤC HỒI), nạp trứng lạnh (ĐO MÁY chờ phẳng; GIỮ NHIỆT không kẹt vì tích phân + hold), quạt hút bật/tắt (FF), nhiệt phôi tăng dần (hold giảm — theo dõi chậm đối xứng), sụt áp nguồn (k' đổi — cập nhật), đổi SP, mất điện/khôi phục (khởi động lại từ mô hình lưu, không từ 0), nhiễu lượng tử 0,1 °C (mọi ngưỡng suy ra từ σ).

### 3.6 Tự kiểm và chế độ bảo thủ (cùng một công thức, không phải bộ khác)
- Sau mỗi quyết định ghi dự đoán PV(t+θ); khi tới hạn so với đo. Thống kê (ngưỡng từ σ, CUSUM) ⇒ hai mức: **tin** / **không tin**.
- Không tin ⇒ cùng công thức với k' giảm, τc tăng (chậm hơn, an toàn hơn), công suất kẹp quanh hold, cờ báo ra Web/HMI. Không có PID dự phòng khác.
- `GIỮ AN TOÀN`: cảm biến nghi ngờ/High gần ⇒ công suất = min(hold đã nhớ, ngưỡng) hoặc 0; vẫn dưới lớp bảo vệ độc lập.

### 3.7 Chỉ 3 giá trị người dùng
SP, vọt tối đa (cố định 0,2, hiện chỉ đọc), trần công suất (tùy chọn). Mọi thứ khác tự đo.

## 4. Cách tránh lặp lại sai lầm cũ (kỷ luật, không phải khẩu hiệu)
1. **Mỗi hằng số có dòng "suy ra từ đâu"** (σ cảm biến, θ, k', vật lý) trong `doc/THERMAL_V2_CONSTANTS.md`; hằng số không suy ra được ⇒ không được vào mã.
2. **Tách tập thiết kế / tập kiểm**: lò ảo tập A (≈ 12 lò) dùng khi viết; tập B (≥ 60 lò, khác tham số) và tập C (sai cấu trúc có chủ ý: ba nút, cảm biến trễ thêm 20–60 s, ghép nối trứng đổi theo quạt, heater phi tuyến/sụt áp, lượng tử 0,1 °C) **chỉ chạy để chấm**, cấm chỉnh theo kết quả B/C. Nếu phải chỉnh ⇒ chỉnh cấu trúc và ghi lý do, rồi tạo tập kiểm mới.
3. Tiêu chí tuyệt đối trong `acceptance.json` (cập nhật cho R1–R6), không "không tệ hơn bản trước".
4. **Không vá**: thất bại ở tập kiểm ⇒ truy nguyên nhân công thức, không thêm ngưỡng riêng cho ca.
5. Reviewer độc lập (xem mục 7) trước khi đóng mỗi giai đoạn.

## 5. Các giai đoạn, thời gian, tiêu chí qua (hộp thời gian; không đạt thì không bước tiếp)

| GĐ | Việc | Thời gian | Tiêu chí qua |
|---|---|---:|---|
| G0 | Chốt đặc tả: giao diện khối, công thức, danh sách hằng số + nguồn suy ra, tiêu chí R1–R6 thành số, tách tập A/B/C | 15' | Bạn duyệt; kiểm lại công thức SIMC/cutback với nguồn |
| G1 | Bộ mô phỏng + công cụ chấm (C++ thuần, dùng lại mô hình lò từ lịch sử git, **không** dùng lại bộ điều khiển cũ); tập A/B/C; đo "nhanh nhất vật lý" của từng lò | 35' | Chạy được với bộ điều khiển bang-bang tầm thường; số "nhanh nhất" khớp tính tay |
| G2 | `thermal_v2.h` (C++ thuần, không phụ thuộc Arduino): ước lượng θ/k', LÊN NHIỆT, GIỮ NHIỆT, tự kiểm + test đơn vị (nhiễu lượng tử, trễ, bão hòa, đổi SP, tràn millis) | 45' | Test đơn vị xanh; mô phỏng tập A: R1–R3 đạt |
| G3 | Vòng kín tập B + C và nhiễu (cửa, trứng lạnh, quạt, sụt áp, nhiệt phôi, 24 h) | 30' | R1–R6 đạt trên B; trên C: **0 High, 0 Emergency, vọt ≤ 0,2** hoặc báo rõ ca nào và vì sao |
| G4 | Vòng kín **bảo vệ**: ép công suất tối đa khi lỗi cảm biến/SSR kẹt ⇒ lớp bảo vệ phải cắt (thay thế bài kiểm vòng kín đã xoá) | 20' | 0 lệnh sưởi khi bảo vệ đang chặn; không ca "không dừng được" ngoài lỗi phần cứng đã biết |
| G5 | Nối vào khe firmware: vào/ra, lưu mô hình (NVS từ `loop()`), telemetry (state, θ, k', hold, PV_pk, tự kiểm), **PID Monitor Web + tải CSV log**, HMI | 40' | CI xanh (biên dịch thật), chu kỳ điều khiển lớn nhất < 50 ms trong ước lượng, Flash/RAM trong ngưỡng |
| G6 | Rà soát độc lập + sửa + bản `2.0.0-rc` | 20' | Reviewer không còn lỗi mức "nghiêm trọng"; gate tuyệt đối chuyển từ báo cáo sang **chặn** CI |
| G7 | **Thử lò thật** (bạn thực hiện, tôi đọc log): (a) lò rỗng không trứng, bạn xem Web; (b) mẻ có trứng; rồi hiệu chỉnh lò ảo theo log | ngoài 3 giờ | So sánh log thật với mô phỏng; chỉ khi đạt R1–R6 trên lò thật mới gọi là xong |

Tổng G0–G6 ≈ 3 giờ 25 phút kể cả đệm cho 1–2 vòng lặp cấu trúc. **Làm xong 3 giờ = bản RC đạt trên mô phỏng độc lập; không phải "đã đạt trên lò thật".** G7 không ép được.

## 6. Rủi ro chính
1. θ và k' thay đổi theo gió/quạt, tải trứng, điện áp ⇒ cập nhật liên tục + tự kiểm; nếu sai ⇒ chế độ bảo thủ chậm hơn nhưng không vọt.
2. Cảm biến lượng tử 0,1 °C ⇒ đo k' cần cửa sổ đủ dài; ĐO MÁY tốn vài phút.
3. Không còn bộ dự phòng ⇒ biện pháp: lớp bảo vệ độc lập, bản 1.1.15 nạp lại được.
4. Lò ảo sai ở chỗ chưa biết ⇒ G7; log Web là điều kiện để đóng vòng.
5. Hold ban đầu mẻ đầu chưa biết ⇒ tích phân + dữ liệu danh định công suất (tùy chọn).

## 7. Điều tôi cần bạn duyệt
1. Chỉ tiêu R1–R5 ở mục 0 (nhất là R1 "≤ 1,3× nhanh nhất + 120 s" và hụt ≤ 0,2).
2. Chấp nhận **ĐO MÁY ~5–8 phút** ở mẻ đầu (mẻ sau dùng mô hình đã lưu).
3. Mốc: 3 giờ ra RC trên mô phỏng; thử lò thật là bước riêng do bạn chạy.
4. Cho phép thêm vào Web: PID Monitor mới + nút tải CSV log (cần để đóng vòng).
5. Thứ tự thử lò thật: lò rỗng trước, trứng sau.
