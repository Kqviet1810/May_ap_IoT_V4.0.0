# HMI: Thông tin kết nối, Thiết bị kết nối, Màn hình ngủ · Web: menu 3 gạch

## HMI – CÀI ĐẶT CHUNG → HỆ THỐNG
* **Thông tin kết nối** nay là menu con: *Chi tiết* (màn thông tin kết nối cũ) · *Đổi wifi* · *Mã QR ID* · *Đặt lại mã PIN* · *Thiết bị kết nối* · *Thoát*. Đổi wifi / Đặt lại PIN / Thiết bị kết nối chỉ hiện khi máy Online (như trước đây).
* **Thiết bị kết nối**: danh sách tài khoản đang liên kết với máy (tên hiển thị + vai trò, không hiện e-mail). Chọn một dòng → xác nhận CÓ/KHÔNG → gỡ liên kết (kèm xóa đăng ký thông báo của tài khoản đó với máy này). **Chủ máy không xóa được.** Cần có mặt tại máy; dùng chính đường HTTPS "đặt lại PIN" (không thêm task, không thêm TLS owner). Cần Worker mới có `POST /api/device/members` và `/members/remove` (xác thực bằng device key).
* **Thời gian ngủ** (mục trong HỆ THỐNG): 0 (TẮT) – 30 phút, mặc định 2 phút. Lưu NVS (`mayap_ui/sleep_min`), không đụng cấu hình máy/EEPROM.
* **Màn hình ngủ**: chỉ ở màn hình chính, sau khi hết thời gian không thao tác. Chỉ nhiệt độ + độ ẩm cỡ lớn, chia đôi bằng một vạch nhỏ (cách mép trên và dưới), giờ:phút nhỏ ở góc trái giống màn hình chính. Có lỗi/cảnh báo/xác nhận thì không ngủ; thao tác đầu tiên chỉ đánh thức, không kích hoạt gì.

## Web 1.1.11
* Màn nhỏ (≤800 px hoặc điện thoại xoay ngang): thanh tab dưới đáy được thay bằng nút 3 gạch cố định ở góc trái dưới; bấm mở danh sách tab, chọn tab / bấm ra ngoài / Esc thì đóng. Nội dung không còn bị che. Màn lớn giữ sidebar.
* Nút Ghi chú nổi cố định trên mọi tab: không còn né nội dung hay nhảy khi cuộn; chỉ đổi vị trí khi người dùng kéo.

Kiểm thử: `tests/hmi-navigation.test.cjs`, `tests/web-shell.test.cjs`, `tests/account-security.test.cjs` (members). Chưa kiểm trên ESP32/LCD thật và trên iPhone thật.
