# Điều khiển đầu ra qua Web và log từ xa (firmware 2.0.0-rc, Web 1.1.21)

## Quyền điều khiển qua Web (chỉ đặt trên HMI)
HMI → Nâng cao → **Dieu khien Web** (cần phiên kỹ thuật). 6 quyền. Mặc định **BẬT**: Quạt hút, Quạt tuần hoàn, Đèn, Còi; **TẮT**: Thanh nhiệt, Đảo trứng (máy cũ đã lên firmware này cũng nhận mặc định đó):
Thanh nhiệt, Quạt hút, Quạt tuần hoàn, Đèn, Còi, Đảo trứng. Lưu bằng dòng LUU.
Web chỉ đọc các quyền này (`runtime.tech.webCtl`); không có lệnh nào trên Web để ghi chúng.

| Đầu ra | Hành vi khi Web được phép |
|---|---|
| Thanh nhiệt | Chỉ **ngắt** (inhibit). Không bao giờ ép bật vượt công tắc vật lý/bảo vệ. Xoá khi bắt đầu mẻ mới. |
| Quạt hút / Tuần hoàn | Ghi đè 15 phút rồi tự trả về tự động. Điều kiện an toàn ép quạt vẫn thắng. Tắt tuần hoàn thì chặn nhiệt. |
| Đèn | Như trước, nhưng nay cần quyền. |
| Còi | 30 s; còi báo động thật vẫn ưu tiên. |
| Đảo trứng | Hai nút trái/phải, **giữ để đảo, thả ra dừng** (lease 1,5 s gia hạn mỗi 0,9 s). Chỉ khi công tắc AUTO vật lý TẮT (chế độ tay). |

## Log từ xa
Máy giữ vòng 360 mẫu × 10 s (1 giờ) trong RAM. Khi thẻ PID Monitor mở, mỗi snapshot mang kèm một khối 10 mẫu; Web ghép theo chỉ số và nút **Tải log** xuất CSV
(`time_utc,index,pv_c,sp_c,pred_c,request_pct,ssr_avg60_pct,phase,suspect,vent_fan,ssr_on,heat_master,batch,sensor_ok,source`).
Giới hạn: chỉ thu được khi thẻ đang mở và máy online; mất điện thì mất vòng nhớ.

## Máy ngoại tuyến
Nhiệt độ/độ ẩm hiện "—" nếu trạng thái không phải online/reconnecting/degraded.

## "Lên nhiệt lại sau khởi động" (R1.reach.restart)
Tiêu chí: sau khi máy khởi động lại giữa mẻ (mất điện/reset), nhiệt phải về điểm đặt nhanh. Hiện đạt **0 %**: bộ điều khiển V2 không lưu đường đã học, nên học lại (~12 phút) và công suất bị chặn tới khi có fit đầu tiên được chấp nhận.
Đã thử lưu "Prior" (đường nhận dạng): đạt 52/52 nhưng đo được rủi ro — cùng lò: High 3/52 + Emergency 1/52 (nhiệt trứng sau mất điện không biết), lệch gain ±30 %: High 7–11/52, lò sai: High 38/52. Vì vậy **không bật**.
Nếu tích hợp: lên nhiệt lại nhanh hơn, nhưng có xác suất vượt nhiệt mà giới hạn bảo vệ phải cắt. Hướng an toàn hơn (chưa làm): vẫn giữ trần công suất cho đến khi biết nhiệt trứng, chỉ bỏ phần chờ học.
