# Ứng viên `PriorMinError` 3.5 → 2.0 — phân tích 9 ca hồi quy phát sinh (chỉ thử nghiệm A/B, KHÔNG đổi production)

Trạng thái: **không phê duyệt đổi production**. `PriorMinError` trong `thermal_control.h` vẫn là 3.5. Số liệu dưới đây lấy từ bản dựng thử nghiệm trên 2160 ca (chỉ chế độ Smart), mã sản xuất không bị sửa.

## Kết quả đo (mã Smart trước khi sửa hold/Kh)
| | PASS | High | Emergency | B PASS → C FAIL |
|---|---|---|---|---|
| Smart, 3.5 (hiện hành) | 1397 | 18 | 3 | 48 |
| Smart, 2.0 | 1391 | 8 | 3 | 57 |
Chênh lệch ca hồi quy: **9 ca mới, 0 ca được cứu.** Bộ 788: 546 → 542 PASS, High/Emergency 0/0. Holdout: 282 → 282.

## 9 ca mới — cùng một họ, đã giải thích
Cả 9 ca đều là **ambient 35 °C, sai lệch ban đầu 2.5 °C, độ phân giải 0.01**, tức đúng vùng 2.0 ≤ sai lệch < 3.5 mà prior bắt đầu hãm.
| Ca | Plant (eff / kJ/K / loss / dead / lag) | Hiện hành | Với 2.0 |
|---|---|---|---|
| m39 | 0.5 / 180 / 120 / 120 / 3 | PASS, settle 2703 s | MAE 0.29, settle −1 |
| m1103 | 1.2 / 180 / 120 / 30 / 30 | PASS, settle 7847 s | ripple 0.290, settle −1 |
| m1279 | 1.2 / 600 / 180 / 120 / 8 | PASS, settle 2643 s | MAE 0.28, settle −1 |
| m1319 | 1.2 / 600 / 300 / 120 / 30 | PASS, settle 5218 s | ripple 0.21, settle −1 |
| m1591 | 1.5 / 600 / 120 / 60 / 8 | PASS, MAE 0.097 | MAE 0.109 (sát ngưỡng) |
| m1671 | 1.5 / 600 / 300 / 60 / 3 | PASS | ripple 0.251 (sát ngưỡng) |
| m1679 | 1.5 / 600 / 300 / 120 / 30 | PASS, settle 2219 s | settle −1 |
| m1999 | 2.0 / 600 / 180 / 120 / 8 | PASS, MAE 0.096 | MAE 0.104, settle −1 |
| m2031 | 2.0 / 600 / 300 / 60 / 3 | PASS, settle 910 s | ripple 0.277, settle 7843 s |

**Cơ chế (truy vết m39 từng 600 s):** prior hãm tối đa ≈ 12–13 s ON trong 150 s đầu khi chưa thấy đáp ứng. Plant chậm (dead 60–120 s hoặc tải nặng) nhận quá ít năng lượng nên learner **không đủ kích thích để đạt độ tin cậy ≥ 60**: m39 dừng ở `LEARNING`, confidence 39 (hiện hành: 63 và `QUALIFIED` lúc 2999 s), PV đứng 0.1–0.3 °C dưới SP suốt phần còn lại của lần chạy. Đây **không phải lỗi điều khiển mới**; đó là cơ chế đã biết ở mục 3 của báo cáo Phase 9 (khởi động hãm → learner thiếu kích thích → vòng điều khiển ở trạng thái chưa đạt chuẩn). 6/9 ca có `settle = −1` (m39, m1103, m1279, m1319, m1679, m1999); 3/9 sát ngưỡng hoặc chậm (m1591, m1671, m2031).

## Kết luận
* Hạ ngưỡng giảm 10 ca High nhưng đổi lại 9 ca kiểm soát kém hơn, cùng một cơ chế learner chưa đạt chuẩn. Đúng yêu cầu "không đánh đổi bằng một lỗi kiểm soát khác chưa giải thích": lỗi **đã giải thích**, nhưng chưa có biện pháp giảm nhẹ.
* Điều kiện tối thiểu để xem xét lại: một cách cho learner đạt chuẩn khi prior hãm (ví dụ mồi hold/gain từ profile đã lưu), có bằng chứng A/B không tạo thêm ca settle −1. Việc đó là phát triển thuật toán mới nên **không làm trong đợt này**.
* Giữ `PriorMinError` = 3.5 trong production. Ứng viên 2.0 chỉ nằm trong thử nghiệm A/B (`ablation-2160.csv`).
