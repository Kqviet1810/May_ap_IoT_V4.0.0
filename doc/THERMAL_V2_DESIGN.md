# Thermal V2.0.0: bộ điều khiển nhiệt mới (thiết kế, chưa phải mã chạy)

Mục tiêu: **một** bộ điều khiển nhiệt thay toàn bộ PID, phanh khởi động, Adaptive, learner, profile, launch plan. Giữ nguyên lớp an toàn (High, Emergency, contactor, SSR, cảm biến).
Tài liệu này gồm: (1) bài học từ sai lầm đã trải qua, (2) tài liệu/hệ thống đã được kiểm chứng ngoài đời, (3) thiết kế rút ra từ hai điều trên, (4) cách kiểm chứng, (5) kế hoạch xây dựng.
Mọi con số hiệu năng nhắc tới là của mô phỏng trên lò ảo hai nút; **chưa có số đo trên lò thật**.

## 1. Bài học từ sai lầm của chính dự án

| # | Sai lầm đã xảy ra | Bằng chứng | Quy tắc rút ra cho V2 |
|---|---|---|---|
| L1 | Mô hình **một nút** cho lò có khối trứng ~1,9 MJ/°C | Hold sai, phanh sai, học sai | Mô hình vật lý **hai nút** là cấu trúc cố định; không có đường điều khiển nào dùng mô hình một nút |
| L2 | Mỗi lỗi vá bằng một ngưỡng riêng | ~100 hằng số trong `thermal_control.h`, 20 lần sửa trong 17 ngày | Mọi ngưỡng phải suy ra từ **nhiễu cảm biến** hoặc **vật lý**, không chọn tay theo ca |
| L3 | Học mà không đối chiếu mô hình với thực tế | Học sai Gain 0,030 / Delay 45 s; vọt | Không học vào trạng thái điều khiển nếu mô hình **chưa dự đoán đúng** dữ liệu gần nhất |
| L4 | Hold tăng chậm, giảm nhanh gấp 3 | Sau cú tụt "nhất quyết không kéo lên" | Không có luật bất đối xứng giấu trong bộ học; sự bất đối xứng (vọt tệ hơn hụt) chỉ nằm ở **hàm chi phí**, công khai |
| L5 | Nghiệm thu bằng "không tệ hơn bản trước" | Chậm/hụt cộng dồn qua các bản | Nghiệm thu bằng **tiêu chí tuyệt đối** (`acceptance.json`) và so với giới hạn vật lý |
| L6 | Mô phỏng một nút/ảo là bằng chứng | Báo OK, lò thật kém | Lò ảo phải có **sai cấu trúc có chủ ý** (ba nút, cảm biến trễ, ghép nối trứng đổi theo tốc độ gió) và phải hiệu chỉnh bằng log thật |
| L7 | Tách độ lợi heater khỏi tổn thất chỉ từ độ dốc nhiệt độ | Ba lần thử khởi động thất bại (hồi quy một nút, độ lợi trên mỗi năng lượng, xung thử) | Độ lợi và tổn thất **không tách được từ một đường cong**: cần kích thích có thiết kế và/hoặc thông số danh định của lò |
| L8 | Đường lùi = PI không biết độ lợi | 43/1248 ca mất ổn định | Đường lùi phải dùng **cùng mô hình** ở chế độ bảo thủ, không phải bộ điều khiển khác |
| L9 | Vào điều khiển sớm khi mô hình chưa kiểm chứng | Vọt 1,48 °C, 2 lò chạm High | Cổng vào điều khiển theo **kiểm chứng dự đoán**, không theo thời gian |

## 2. Cái gì đã được kiểm chứng ngoài đời (dùng lại, không tự phát minh)

1. **Marlin MPC / Klipper MPC** (hàng nghìn máy in 3D; mặc định ở nhiều bản firmware). Cấu trúc: mô hình vật lý nhiều khối nhiệt (không khí, khối gia nhiệt, cảm biến, tải), **mô phỏng trạng thái và kéo nhẹ về giá trị đo** (quan sát viên), công suất heater **nhập từ người dùng** (W), tự cân chỉnh bằng quy trình riêng (làm nguội về nhiệt độ phòng, gia nhiệt, đo tổn thất), và **điều chỉnh nhiệt độ phòng ước lượng** để triệt độ lệch khi mô hình sai. Rút ra: thông số công suất danh định là đầu vào hợp lệ; cân chỉnh là một quy trình chủ động, không phải học ngầm. (Marlin docs: *Model Predictive Temperature Control*; Klipper MPC port.)
2. **Skogestad SIMC / Grimholt (2018)**: PI(D) chỉnh tốt **không kém** bộ bù trễ Smith ở cùng mức bền vững và Smith **nhạy với sai số trễ** hơn. Luật: τc ≥ θ, τI = min(τ1, 4(τc+θ)). Rút ra: phần hồi tiếp gần SP nên đơn giản, bền; không dựa vào bù trễ chính xác. (Skogestad & Grimholt, *The SIMC Method for Smooth PID Controller Tuning*; Grimholt 2018.)
3. **Ước lượng thông số trực tuyến**: RLS có quên theo hàm mũ bị "covariance windup" khi kích thích kém; cần quên theo hướng, đặt lại có chu kỳ, hoặc chỉ cập nhật theo hướng được kích thích (SIFt-RLS, exponential/cyclic resetting). Giám sát chuyển mô hình theo kiểm tra dự đoán. Rút ra: **chỉ cập nhật phần được kích thích**; giữ phần còn lại ở giá trị trước.
4. **Ấp trứng thương mại (Petersime OvoScan)**: điều khiển theo **nhiệt độ vỏ trứng** (không phải không khí); nhiệt độ không khí chỉ là cơ cấu chấp hành. French (1997): nhiệt phôi tăng từ giữa chu kỳ; dẫn nhiệt không khí–trứng phụ thuộc **tốc độ gió**. Rút ra: biến cần điều khiển là nhiệt độ trứng ước lượng; ghép nối và nhiệt phôi **đổi theo thời gian** nên phải được theo dõi chậm.
5. **Điều khiển tối ưu thời gian** cho tác động có biên: nghiệm là bang-bang với **một lần chuyển** (bật tối đa rồi cắt đúng lúc). Rút ra: khởi động tối ưu là "đạp hết ga rồi cắt theo dự đoán đỉnh", đúng cái phanh cũ muốn làm nhưng thiếu dự đoán đúng.
6. **Honeywell/Eurotherm/Watlow** (đã dùng ở V1): cutback, ức chế vọt tạm thời, tự chỉnh theo dao động. Giữ ý tưởng "cắt trước SP bằng khoảng **tính** từ trễ/quán tính".

## 3. Thiết kế V2.0.0

### 3.1 Nguyên tắc
- **Một mô hình, một bộ điều khiển, một đầu ra công suất.** Không có PID bên dưới, không có đường lùi là bộ khác.
- Mô hình vật lý hai nút (không khí + trứng) với trễ vận chuyển, quán tính phần tử, quạt hút, nhiệt phôi, nhiễu công suất chậm (đã có trong `thermal_predictive.h`).
- Ngưỡng chỉ từ: độ phân giải cảm biến (σ ≈ 0,03–0,05 °C cho bước 0,1 °C), thời gian đặc trưng của lò (θ = trễ + quán tính đo được), và hàm chi phí công khai.

### 3.2 Khối chức năng

1. **Dữ liệu danh định của lò** (nhập một lần, như `MPC_HEATER_POWER` của Marlin): công suất heater (kW), số xe/trứng. Dùng làm prior cho `cf`, `ce`, độ lợi nhanh. Nếu không có thì dùng prior rộng như hiện nay.
2. **Cân chỉnh có chủ ý (Commissioning)**, tùy chọn khi lò rỗng, kiểu Marlin autotune: (a) đo nhiệt độ phòng, (b) xung heater có kích thước tính được, (c) đo phản ứng không khí (cf nhanh, trễ, quán tính), (d) chờ nguội một đoạn để đo tổn thất `a`. Lưu mô hình. Cũng là cách chính để lấy dữ liệu thật.
3. **Nhận dạng theo giai đoạn (staged identification)**: giai đoạn 1 (khởi động) chỉ nhận dạng ba thông số được kích thích mạnh bởi xung đầu (cf nhanh, trễ, quán tính), giữ trứng/ghép nối/tổn thất ở prior; giai đoạn 2 mở dần các thông số còn lại **khi có kích thích** (kiểm tra điều kiện kích thích trước khi cập nhật). Khác L7: cấu trúc hai nút ngay từ đầu, nên không có suy biến một nút.
4. **Quan sát viên trạng thái** (MHE/Kalman đã có): nhiệt độ không khí đo, nhiệt độ trứng ước lượng, nhiễu công suất `d`, nhiệt phôi `w`, bù nhiễu kiểu "offset-free" (như điều chỉnh nhiệt độ phòng của Marlin MPC).
5. **Bộ lập kế hoạch** (đã có): mô phỏng 2 giờ, chọn phương án rẻ nhất, hàm chi phí bất đối xứng công khai. Thêm **phương án khởi động tối ưu thời gian** (bật tối đa rồi cắt tại thời điểm tính từ dự đoán đỉnh) làm một phần của tập phương án, không phải một bộ điều khiển riêng.
6. **Giám sát bằng thống kê**: so sai số dự đoán gần nhất với σ cảm biến (kiểm tra chi-bình-phương/CUSUM) thay cho các ngưỡng tay 0,10/0,20/0,6. Có hai mức: *tin* và *không tin*. Khi không tin: **chạy lại chính planner với mô hình bảo thủ** (đã lưu từ mẻ trước hoặc prior, độ bất định phình, công suất bị chặn quanh hold đã biết), cộng báo động cho người vận hành. Không có PID dự phòng.
7. **Lớp an toàn giữ nguyên**: High/Emergency, contactor, SSR scheduler, cảm biến. Bộ điều khiển chỉ đề nghị công suất.
8. **Ghi log phục vụ hiệu chỉnh**: định dạng log cố định (PV, công suất thực, trạng thái, mô hình) xuất qua PID Monitor/serial để đưa về lò ảo.

### 3.3 Cái gì bị gỡ
`ThermalController` (PID), `ThermalStartupController`, `ThermalLearner`, `ThermalObserver`, `AdaptiveV1`, `AdaptiveThermalSupervisor`, `ThermalProfile` + storage, `LaunchPlan` + storage, cờ Smart Thermal, `RelayAutoTune`, `SmartAutoTune` (thay bằng Commissioning), các hằng số Kp/Ki/Kd trong cấu hình và HMI/Web. Cần đổi schema cấu hình và tài liệu.

### 3.4 Thay đổi giao diện người dùng
Còn: SP, dữ liệu danh định của lò, nút Commissioning, PID Monitor (các dòng "Du doan" của 1.1.15, đổi nhãn). Mất: Kp/Ki/Kd, AutoTune, các cài đặt Adaptive.

## 4. Kiểm chứng

1. **Tiêu chí tuyệt đối** trong `tools/thermal-research/acceptance.json` (thời gian chạm SP−0,3 °C so với mức nhanh nhất vật lý của chính lò đó, vọt, hụt, High, phục hồi sau mở cửa, không ca nào kẹt). Mốc hiện tại 1.1.15: 8/12 tiêu chí chưa đạt. V2.0.0 phải đạt cả 12 **và** toàn bộ ma trận an toàn hiện có (1248 ca) **và** lưới sai cấu trúc.
2. **Lưới sai cấu trúc (mới)**: lò ba nút, cảm biến trễ thêm 20–60 s, ghép nối trứng đổi theo quạt, nhiệt phôi tăng nhanh, heater phi tuyến, nhiễu bước 0,1 °C. Mục đích: V2 không được hỏng khi cấu trúc thật khác mô hình.
3. **Bù lò thật**: log từ 1–2 mẻ thật (30 phút đầu, một lần mở cửa) để hiệu chỉnh lò ảo trước khi tin các con số.
4. **CPU ESP32**: chu kỳ điều khiển lớn nhất đã hiển thị trên PID Monitor từ 1.1.15; V2 phải giữ dưới 50 ms (đang đo).
5. **Cổng CI**: tiêu chí tuyệt đối chuyển từ báo cáo sang **chặn** khi V2 đạt.

## 5. Kế hoạch xây dựng (mỗi bước có tiêu chí; không đạt thì không bước tiếp)

| Bước | Việc | Tiêu chí qua bước |
|---|---|---|
| S1 | Lưới sai cấu trúc + đo lại 1.1.10/.13/.14/1.1.15 bằng công cụ nghiệm thu | Có bảng so sánh các bản cũ; lò ảo có ≥ 3 loại sai cấu trúc |
| S2 | Nhận dạng theo giai đoạn + điều kiện kích thích trong `Identifier` | Khi xung đầu: sai số ước lượng cf/trễ < 20 % trên lưới; không làm tệ hold |
| S3 | Phương án khởi động tối ưu thời gian trong planner, dùng kết quả S2 | Thời gian chạm ≤ 1,5× nhanh nhất + 180 s; vọt/hụt ≤ 0,3 °C trên lưới 72 lò × 2 nhiệt độ đầu |
| S4 | Giám sát thống kê + chế độ bảo thủ thay đường lùi (không còn PID) | 0 ca mất ổn định trong ma trận 1248 ca và lưới sai cấu trúc |
| S5 | Commissioning + dữ liệu danh định + log | Chạy được trên lò ảo; tự lưu/nạp; không ghi flash từ tác vụ điều khiển |
| S6 | Gỡ hệ cũ, đổi schema, HMI/Web, bản 2.0.0-rc | Cổng đầy đủ + CI xanh + 12/12 tiêu chí |
| S7 | Chạy thử lò thật có người theo dõi, hiệu chỉnh theo log | Báo cáo so sánh lò thật với lò ảo |

Phát hành V2.0.0 chính thức chỉ sau S7.

## 6. Rủi ro đã biết
- Chưa có dữ liệu lò thật; S7 có thể phát hiện lò ảo sai ở chỗ nào đó và buộc quay lại S2–S4.
- Sau khi gỡ hệ cũ không còn "bộ khác" để lùi về. Biện pháp: chế độ bảo thủ dùng cùng mô hình; lớp an toàn độc lập; có bản 1.1.15 để nạp lại.
- Khởi động mẻ đầu (L7) là phần khó nhất; nếu S2–S3 không đạt bằng nhận dạng thuần túy thì dùng dữ liệu danh định và Commissioning là bắt buộc cho lò mới (như Marlin yêu cầu công suất heater).
- Chi phí flash: bản hiện tại chỉ còn ~1,6 KB. Gỡ hệ cũ dự kiến giải phóng nhiều chục KB, cần đo khi làm S6.

## Nguồn
- Marlin: *Model Predictive Temperature Control* (marlinfw.org/docs/features/model_predictive_control.html); Klipper MPC port (klipper.discourse.group).
- Skogestad, Grimholt: *The SIMC Method for Smooth PID Controller Tuning*; Grimholt, *Optimal tuning of PID controllers* (NTNU, 2018).
- Shin & Lee (2020), *A New Exponential Forgetting Algorithm for RLS*; Mohseni & Bernstein, *SIFt-RLS* (2024); Pequito, *Advanced RLS* (IST).
- Petersime, *OvoScan / Embryo-Response Incubation*; French N.A. (1997), *Modeling incubation temperature*, Poult. Sci. 76:124; Van Brecht et al. (2003), Poult. Sci. 82:1677.
