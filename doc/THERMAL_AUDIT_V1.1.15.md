# Audit hệ nhiệt MAYAP (firmware 1.1.15, nhánh `claude/gracious-hopper-zjvufg`)

Báo cáo này **không có mã mới**. Nó trả lời: vì sao hệ cũ cho cảm giác "chậm như rùa, gần chạm SP thì dừng, vọt vì học sai, tụt rồi không kéo lên",
phần nào đã thay, phần nào chưa, và cái gì nên bỏ.
Mọi số liệu là của **mô phỏng chạy đúng mã firmware** trên lò ảo hai nút (72 cấu hình theo thông số bạn đưa). **Chưa có một số đo nào trên lò thật.**

## 1. Kết luận

1. Bốn triệu chứng bạn thấy **không phải là bốn lỗi rời**. Chúng có chung gốc: hệ cũ dựng trên mô hình **một nút** (chỉ thấy không khí), trong khi quyết định công suất phụ thuộc vào
   **khối trứng ~1,9 MJ/°C** mà không ai đo được; rồi mọi thiếu sót được vá bằng ngưỡng/hằng số riêng (khoảng 100 hằng số chỉ riêng `thermal_control.h`).
2. "Báo OK rồi lò thật kém hơn" là do **cách tôi nghiệm thu**: cổng kiểm thử chỉ hỏi "có tệ hơn bản trước không / có chạm High không / có dao động không".
   Nó **không đo** thời gian lên nhiệt, độ hụt dưới SP, hay thời gian kéo lại sau khi tụt. Các triệu chứng của bạn nằm đúng trong khoảng trống đó. Đó là lỗi quy trình của tôi, không phải bạn đo sai.
3. Hệ dự đoán mới (Thermal Predictive V1) **đã thay phần giữ nhiệt** (trong mô phỏng nó điều khiển công suất 96–99 % thời gian sau ~9 phút đầu) và cải thiện rõ độ vọt/hụt khi giữ. Nó **chưa thay được**:
   (a) giai đoạn lên nhiệt/khởi động, (b) đường lùi khi mô hình sai. Hai việc này đúng là hai triệu chứng khó chịu nhất ("chậm như rùa", "gần chạm thì dừng").
4. Hai lần thử thay nốt (khởi động tự đo + PI an toàn) **đều thất bại và đã được gỡ ra**. Số liệu ở mục 5.
5. Các lớp **bảo vệ** (High, Emergency, contactor, bộ chia xung SSR, mất cảm biến) **không phải nguyên nhân** và nên giữ nguyên; xem mục 4.

## 2. Hệ hiện có: ai quyết định công suất

Đường công suất (`machine_control.h`, tác vụ điều khiển):

```
Adaptive V1 (updateAdaptiveBalance) ──► effectiveLimit (trần công suất)
ThermalStartupController (phanh khởi động, ~1330 dòng thermal_control.h cùng PID) ──► trần khởi động + floor "đạp ga"
PID (Kp/Ki + feed-forward hold từ learner/profile/launch plan) ──► pidPower_
Thermal Predictive V1 (nếu Active) ──► GHI ĐÈ pidPower_
min(pidPower_, effectiveLimit) ──► bộ chia xung SSR ──► trọng tài đầu ra ──► relay
```
Song song, **không nằm trong đường trên**: High/Emergency (so sánh nhiệt độ có xác nhận thời gian, tự cắt contactor), bảo vệ cảm biến, trọng tài đầu ra.

Số khối chịu trách nhiệm điều khiển nhiệt: `thermal_control.h` 1332 dòng / 97 hằng số, `thermal_learner.h` 873, `thermal_smart_autotune.h` 894, `thermal_adaptive_v1.h` 275,
`thermal_profile*.h`, `thermal_launch_plan.h`, `thermal_observer.h`, `thermal_assist.h` ... cộng `thermal_predictive.h` 767. Sáu mô-đun "học" khác nhau cùng nuôi một đầu ra. Không ai (kể cả tôi) lập luận được toàn bộ tương tác của chúng.
Từ 04/10 đến 21/10, `thermal_control.h` có 20 lần sửa; mỗi lần là một ngưỡng hoặc một ngoại lệ mới.

## 3. Bốn triệu chứng: do đâu, vì sao xảy ra

### 3.1 Lên nhiệt chậm như rùa
- Phanh khởi động phải chống vọt trước khi biết lò là loại nào. Nên nó giả định **trường hợp xấu nhất**: trễ 135 s mặc định, "gain tối đa" cho tới khi đo được, dự trữ (`kappa`) trên quán tính, đạp ga theo từng đợt thăm dò.
  Chính mã ghi: *"the energy brake (sized for a 16 kW bank on a light chamber)"*, tức phanh được cỡ cho một lò khác lò của bạn.
- Hậu quả đo được: từ 28 °C hay từ 33 °C đều mất **trung vị ~15 phút** để chạm 37,2 °C (lò nhanh ~10–12 phút, lò trễ 120 s trung vị 39 phút, tệ nhất 70 phút). Giới hạn vật lý của lò 20 kW chỉ khoảng 2–6 phút cho 4,5 K.
  Xuất phát từ 33 °C **không nhanh hơn** xuất phát từ 28 °C: thời gian bị quyết bởi sự thận trọng của phanh, không bởi khoảng cách nhiệt độ.

### 3.2 Gần chạm SP thì dừng hẳn
- Cơ chế có trong mã: khi còn "nhiệt đang trên đường" thì phanh trừ nó khỏi công suất; ở vùng gần SP trừ mãi, nên PV **đậu dưới SP một khoảng không đổi**. Mã ghi: phanh giữ độ lệch vĩnh viễn ≈ *hold × horizon × 0,13 °C/s*.
- Cách thoát duy nhất là learner đo được công suất giữ (hold). Nó cần các cửa sổ "phẳng" liên tiếp; với cảm biến bước 0,1 °C, độ dốc nhiễu nên cửa sổ phẳng hiếm. Các bản 1.1.12–1.1.14 là các lần vá đúng chỗ này (đo hold theo cửa sổ, probe 0,1 °C...).
- Triệu chứng bạn thấy (PV treo thấp hơn SP ~0,2 °C hàng chục phút) khớp cơ chế này.

### 3.3 Vọt nhiệt vì học sai
- Mô hình một nút học `gain/delay` từ các đoạn dữ liệu bị lẫn quạt hút, mở cửa, nhiệt phôi; học sai thì phanh và hold đều sai (đã gặp: học Gain 0,030 / Delay 45 s sai ở 1.1.1x).
- Không có bước **đối chiếu mô hình với thực tế** trước khi tin nó (hệ mới có: sai số khớp > 0,10 °C thì không điều khiển).
- Số đo trên lò ảo: bản 1.1.14 vọt trung bình 0,49 °C ở mẻ đầu, tệ nhất 1,66 °C; chạm vùng High: tổng 180 mẫu ở mẻ đầu và 189 mẫu ở kịch bản phòng nóng lên 5 °C (cộng trên 72/44 lò).

### 3.4 Tụt xuống rồi không kéo lên
- Có **sự bất đối xứng cố ý** trong mã: khi PV còn dưới SP, hold chỉ tăng `+dt/60` mỗi giây tích lũy; khi PV trên SP hoặc dốc, hold giảm `−3·dt/60`: giảm nhanh gấp 3 lần tăng.
  Thiết kế này an toàn về vọt, nhưng sau một cú tụt (mở cửa), nó kéo lên chậm gấp 3 lần nó tụt. Cộng với khóa tích phân khi bão hòa và coi sự kiện cửa là "đổi lò" (phải học lại), bộ cũ **nhất quyết không kéo lên**.
- Số đo: sau khi mở cửa (−3 °C), hệ mới cũng chỉ phục hồi 184 s so với 214 s của 1.1.14, tức **gần như chưa cải thiện**. Chỗ này tôi chưa giải quyết.

## 4. Từng thành phần: có phải nguyên nhân, thay được chưa

| Thành phần | Có gây triệu chứng? | Thay được chưa | Khuyến nghị |
|---|---|---|---|
| PID giữ nhiệt (Kp/Ki + hold feed-forward) | Một phần: hold sai và bất đối xứng 3:1 (3.2, 3.4) | **Đã thay** trong giữ nhiệt khi mô hình khớp (96–99 % thời gian trong mô phỏng) | Bỏ khỏi đường chính sau khi có đường lùi thay thế |
| Phanh/khởi động `ThermalStartupController` | **Nguyên nhân chính** của 3.1 và 3.2, và của vọt mẻ đầu | **Chưa**: thử thay, thất bại (mục 5) | Phải thay, không vá thêm |
| Adaptive V1, learner, profile, launch plan, observer | **Nguyên nhân chính** của 3.3, góp 3.4 | Chưa: vẫn chạy bên dưới làm đường lùi | Bỏ cùng lúc với phanh, không giữ rải rác |
| Smart AutoTune | Chỉ chạy khi người dùng bấm tune; không thuộc triệu chứng | Không cần | Giữ là công cụ riêng |
| `effectiveLimit` (trần công suất của Adaptive) | Có thể hạn chế khi lên nhiệt (cần đo riêng, chưa có số) | Chưa | Xem lại khi thay phanh |
| High / Emergency / contactor / mất cảm biến | **Không**: so sánh nhiệt độ độc lập, có trễ xác nhận, không đưa vào vòng điều khiển | Không nên thay | **Giữ nguyên.** Đã đọc logic so sánh High/Emergency; đã chạy bộ test an toàn; **chưa** audit từng dòng và chưa thử trên phần cứng trong đợt này |
| Bộ chia xung SSR, trọng tài đầu ra | Không | Không | Giữ nguyên |

## 5. Đã giải quyết bao nhiêu (số đo mô phỏng, cùng lò ảo; 1.1.14 so với bản dự đoán gần cuối)

Bản dự đoán lấy từ lần chạy lưới trước các thử nghiệm cuối; lưới cuối chưa chạy lại nên có thể lệch nhẹ.

| Kịch bản | Chỉ số | 1.1.14 | Hiện tại |
|---|---|---|---|
| Mẻ đầu (28 °C) | vọt trung bình / tệ nhất | 0,49 / 1,66 °C | 0,20 / **1,66 °C** |
| Mẻ đầu | tụt tệ nhất dưới SP | −4,16 °C | **−4,16 °C** |
| Mẻ đầu | % thời gian trong ±0,1 °C | 86 % | 97 % |
| Mẻ đầu | thời gian chạm 37,2 °C | trung vị ~15 phút | **~15 phút** |
| Kích SP | thời gian phục hồi | 293 s | 16 s |
| Mở cửa −3 °C | vọt tệ nhất | 0,63 | 0,16 |
| Mở cửa −3 °C | tụt tệ nhất / phục hồi | −2,76 °C / 214 s | **−2,75 °C / 184 s** |
| Giai đoạn cuối, nhiệt phôi tăng | phục hồi | 13199 s | 247 s |
| Phòng +5 °C đột ngột | % trong ±0,1 °C / chạm High | 81 % / 189 mẫu | 97 % / 0 |

Đọc bảng: **giữ nhiệt tốt hơn nhiều** (vọt, % trong dải, phục hồi sau kích SP và giai đoạn cuối). **Không đổi** ở mọi thứ thuộc khởi động mẻ đầu và cú tụt do mở cửa. Nên nói thẳng: triệu chứng 3.1 chưa giải quyết (0 %),
3.2 và 3.3 giải quyết phần giữ nhiệt nhưng không phải phần khởi động, 3.4 chưa đáng kể.
Tôi không đưa một con số "% tổng thể" vì nó sẽ không có cơ sở: các triệu chứng không cộng được với nhau.

### Hai thử nghiệm thay nốt đã thất bại (đã gỡ khỏi nhánh)
1. **PI an toàn thay đường lùi PID**: qua 6 ca khó chạy riêng, nhưng cổng đầy đủ báo 43/1248 ca "trước ổn định, nay không ổn định" (lò nặng vọt 0,6–0,7 °C và không về SP). Gỡ.
2. **Khởi động do bộ dự đoán tự đo** (cắt công suất theo năng lượng đã cấp và độ trễ đo được): từ 28 °C giảm vọt tệ nhất 1,66 → 0,61 và tụt −4,2 → −2,8, nhưng thời gian lên nhiệt trung vị tăng ~15 → ~31 phút (chậm hơn ở 59/72 lò, tệ nhất 57 phút).
   Lý do: chỉ từ độ dốc nhiệt độ không tách được độ lợi của heater và tổn thất; chuyển sang PI giữ quá chậm. Gỡ.

## 6. Vì sao mô phỏng "OK" mà lò thật kém: giới hạn của kiểm chứng
- Lò ảo hai nút do tôi dựng theo thông số bạn đưa. Nếu lò thật có cấu trúc khác (ba khối nhiệt, quạt đảo gió tạo trễ thay đổi, cảm biến đặt chỗ khác) thì cả hệ cũ lẫn mới đều có thể kém hơn mô phỏng. **Chưa từng so mô phỏng với log lò thật.**
- Cổng kiểm thử so với *bản trước* (chỉ chặn tệ đi), nên mỗi bản mới "không tệ hơn bản trước" nhưng độ chậm/hụt cộng dồn mà không ai chặn.
- Chi phí CPU của bộ dự đoán trên ESP32 chưa đo; mô hình này chạy khoảng 30–60 lần chậm hơn máy chủ theo ước lượng.

## 7. Đề xuất (chưa làm gì cho tới khi bạn đồng ý)

1. **Định nghĩa nghiệm thu theo triệu chứng của bạn, trước khi viết thêm mã**, ví dụ (đề xuất, bạn chỉnh): chạm SP−0,3 °C trong ≤ 8 phút cho lò điển hình từ 33 °C; vọt mẻ đầu ≤ 0,3 °C; hụt dưới SP sau khi chạm ≤ 0,3 °C;
   phục hồi sau mở cửa −3 °C ≤ 5 phút; không một ca nào "không về SP". Cổng chặn theo các số này, không chỉ "không tệ hơn".
2. **Một bộ điều khiển duy nhất** thay PID + phanh + Adaptive + learner + profile + launch plan: khởi động bằng mô hình hai nút với prior vật lý và xung năng lượng có kiểm soát (không dùng độ dốc suy ra độ lợi như thử nghiệm vừa rồi);
   giữ nhiệt bằng bộ dự đoán hiện có; đường lùi là chính nó ở chế độ thận trọng, không phải bản cũ. Chỉ gỡ bản cũ khi đạt nghiệm thu ở bước 1 trên toàn lưới.
3. **Lấy dữ liệu lò thật**: 1–2 mẻ log PID Monitor (30 phút đầu + 1 lần mở cửa). Có nó, tôi hiệu chỉnh lò ảo theo lò thật; đây là cách duy nhất để thu hẹp khoảng cách mô phỏng–thực tế.
4. Giữ High/Emergency/SSR/contactor nguyên vẹn trong suốt quá trình.
5. Cho đến khi xong: công tắc "Nhiet du doan" vẫn BẬT mặc định trong 1.1.15; nếu bạn thấy giữ nhiệt tốt hơn nhưng khởi động vẫn chậm, đó đúng như mục 5 mô tả.
