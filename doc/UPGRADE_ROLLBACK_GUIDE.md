# Hướng dẫn nâng cấp, lưu cấu hình, rollback và khôi phục (MAYAP V4 RC)

Chỉ dành cho người có thẩm quyền. **Không nạp, không OTA khi lò có trứng hoặc khi HEATER đang cấp điện.** Mọi lần nạp ghi SHA, SHA-256 file và người thực hiện vào biên bản.

## 1. Dữ liệu nằm ở đâu (để biết cái gì giữ được khi nâng cấp)
| Dữ liệu | Nơi lưu | Khi nạp/OTA |
|---|---|---|
| Cấu hình máy và mẻ (bản ghi A/B có CRC), lịch sử nâng cao | EEPROM ngoài AT24C512 | **giữ nguyên** (firmware chỉ ghi vùng ứng dụng) |
| Safety Journal, profile học nhiệt, thông tin Wi-Fi/khóa | NVS của ESP32 | giữ nguyên khi nạp bằng `Update`/USB **không** xóa flash; **bị xóa nếu dùng `erase-flash`** |
| Firmware | phân vùng `ota_0` / `ota_1` (`default_8MB`) | OTA ghi vào slot không chạy; slot đang chạy được giữ làm bản trước |
Vì vậy: **không dùng `erase-flash`/xóa toàn bộ** trừ khi người phụ trách chỉ định, vì sẽ mất Safety Journal và profile. Ghi lại cấu hình (ảnh màn hình HMI hoặc xuất từ Web) trước khi nâng cấp.

## 2. Chuẩn bị
1. Xác nhận lò **không có mẻ đang chạy** (OTA chỉ được phép khi không có mẻ và không chờ khôi phục sau mất điện) và HEATER OFF.
2. Kiểm SHA-256 của file với `SHA256SUMS.txt` của artifact; ghi `MAYAP_BUILD_ID` hiện tại (khởi động hoặc HMI).
3. Với RC thử nghiệm: **nạp bằng USB** (Arduino IDE hoặc esptool) bản cờ 0 PILOT sinh từ đúng RC SHA. Bản `SMART-ON-TEST-ONLY` chỉ nạp khi tải sưởi đã cô lập.

## 3. Nâng cấp
* **Bản RC (thử nghiệm):** USB. Không dùng OTA cho RC vì RC chưa có chữ ký phát hành (artifact `workflow_dispatch` không ký).
* **Bản phát hành (sau chấp thuận):** OTA từ xa qua Cloudflare Worker. Firmware chỉ hỏi bản mới; người vận hành tự bấm và xác nhận trên HMI; tải xuống tính SHA-256 trực tiếp, so với giá trị của Worker, **kiểm chữ ký số bằng khóa công khai nhúng trong firmware**; sai bất kỳ điều gì thì hủy và giữ nguyên firmware đang chạy; đúng thì ghi và khởi động lại.
* Sau nâng cấp: kiểm `MAYAP_BUILD_ID`, cấu hình giữ nguyên, kiểm sensor/đầu ra ở chế độ không nhiệt trước khi cho heater chạy.

## 4. Rollback
* **Quay về bản ngay trước (một bản duy nhất):** chức năng rollback của firmware (`ota_rollback.h`) đặt cờ khởi động sang slot còn lại rồi khởi động lại; không ghi/xóa firmware. Chỉ giữ được **một** bản trước; OTA hai lần liên tiếp không rollback giữa chừng sẽ ghi đè mất bản gốc.
* Sau rollback: kiểm `MAYAP_BUILD_ID`, cấu hình, và chạy lại kiểm tra không nhiệt cho bản đang chạy.

## 5. Khôi phục khi cập nhật thất bại
| Tình huống | Hành vi/biện pháp |
|---|---|
| Mạng đứt, SHA-256 hoặc chữ ký sai, ghi flash lỗi | firmware tự hủy cập nhật, giữ bản đang chạy; thử lại sau khi sửa nguyên nhân |
| Bản mới không khởi động ổn định | rollback (mục 4) nếu máy vào được; nếu không, nạp lại bằng USB file `MAYAP_INDUSTRIAL_v1_0_0.ino.merged.bin` hoặc `.bin` của bản tốt đã lưu (giữ `SHA256SUMS.txt` và `.elf` để đối chiếu) |
| Hỏng hoàn toàn phần mềm | USB: nạp `merged.bin` tại địa chỉ 0x0 **không xóa flash**; nếu buộc phải xóa, ghi nhận sẽ mất Safety Journal/profile và cần nghiệm thu lại phần liên quan |
Giữ lại cho mỗi RC: `.bin`, `.merged.bin`, `.elf`, `BUILD_INFO.txt`, `SHA256SUMS.txt` (artifact chỉ lưu 7 ngày — tải về ngay).

## 6. Ký OTA (cho người phát hành)
Khóa ký riêng nằm **chỉ** trong GitHub secret `MAYAP_OTA_SIGNING_PRIVATE_KEY`; workflow ghi ra tệp tạm, ký bằng `openssl dgst -sha256 -sign`, rồi xóa; kiểm tra CI cấm khóa riêng trong mã (`BEGIN … PRIVATE KEY`). Chữ ký (`.sig`) và `.bin` được đính vào GitHub Release **chỉ khi có tag `v*.*.*`**. Chưa tạo tag, chưa ký, chưa phát hành cho đến khi được chấp thuận.
