> **V3 LEGACY:** This document describes the pre-V4 realtime stack. The V4 clean network baseline has no WebSocket/DeviceHub runtime and no MQTT implementation yet.

# MAYAP Web 12.1 — giao diện và kết nối

Tiếp nối `feat/adaptive-staged-boot`, firmware/HMI vẫn là 1.0.0. Thay đổi
này tập trung vào web; các file firmware ESP32, ATtiny, Adaptive Boot và
Runtime Recovery giữ nguyên so với `a60fe7e`.

## Cài đặt và ngôn ngữ

Các nhóm là giao diện/kết nối, cảnh báo/nhắc nhở, vận hành mẻ, quạt hút,
nhiệt độ/cảm biến, nâng cao và thiết bị/hỗ trợ. Nhãn nhóm nằm cùng section
với các thẻ của nhóm, tránh đặt cảm biến dưới tiêu đề quạt hút khi một thẻ
bị ẩn. Nhóm quạt luôn có thẻ riêng.

Quạt hút có ngưỡng bật/tắt theo nhiệt; thông gió tự động; mức Thấp/Tiêu
chuẩn/Cao; chu kỳ 40–120 phút, bước 10; tỷ lệ thời gian chạy 5–90% cho
ngày 1–3, 4–7, 8–11, 12–15, 16–18 và 19–21. Các giá trị mặc định và giới
hạn lấy từ cài đặt HMI hiện tại. Web kiểm tra nhiệt độ đặt ≤ ngưỡng tắt,
ngưỡng bật cao hơn ngưỡng tắt ít nhất 0,1°C và không vượt cảnh báo nhiệt cao.
Phần trăm là thời gian bật trong chu kỳ, không phải tốc độ quay của quạt.
Khi bật chế độ theo ngày ấp, lịch thông gió cũ được tắt theo logic web hiện có.

Web nhận cấu hình thật trước khi cho lưu. Máy cũ thiếu đủ trường thông gió
theo ngày vẫn chỉnh được ngưỡng nhiệt; phần chưa hỗ trợ bị khóa và có giải
thích. Web không tự thêm các trường mới hoặc tắt lịch cũ trên máy đó.

Tiêu đề trình duyệt: **MAYAP · Máy ấp trứng**. Các hướng dẫn Wi-Fi, cập
nhật, bộ nhớ và kết nối dùng từ “máy”, “màn hình máy”, “phần mềm máy”.
PIN ghép nối/PIN hiện tại tiếp tục nhận 4–8 số để tương thích máy đã có;
PIN mới phải 6–8 số, khớp backend. Không đổi quy tắc xác thực backend.

Giao diện sáng dùng nền xám xanh, thẻ trắng và xanh lam ngọc; giao diện tối dùng
nền xanh than và điểm nhấn xanh lam ngọc. Màu chữ và màu nền nút tách riêng để giữ
tương phản. Biểu tượng trợ giúp/cảnh báo dùng SVG mask thay ký tự font.
Các nút trợ giúp có vùng chạm 44px; trạng thái đang gửi/chờ xác nhận hiển
thị tại form. Chế độ theo hệ thống, giảm chuyển động và zoom vẫn hoạt động.

### Web 12.1.1 — màu đơn sắc

Bỏ toàn bộ gradient trên nền trang, thẻ, nút, công tắc, tiến trình và màn
quét QR. Điểm nhấn chính `#087f8c`; nền sáng `#f3f7f9`, nền tối `#0e1b24`.
Giảm bóng đổ, bỏ hiệu ứng phát sáng màu để giao diện dịu hơn. Màu cảnh báo,
lỗi và an toàn giữ nguyên ý nghĩa. Theme PWA và trang bật thông báo đồng bộ
với bảng màu; cache tăng lên `12.1.1` để nhận CSS mới.

Browser QA kiểm tra background được render của mọi phần tử trên cả ba tab
không có gradient, ở năm kích thước và hai theme. Kiểm tra tương phản chữ/nút
vẫn đạt ≥4,5:1. Bản sửa màu không thay đổi `app.js`, firmware, Boot Manager,
Runtime Recovery, giao dịch ghi hoặc MQTT.

### Web 12.1.2 — trắng và xanh #64C9D1

Màu thương hiệu theo yêu cầu là `#64C9D1`. Nền sáng `#F3F8F9` và thẻ trắng
`#FFFFFF`; điều hướng sáng cũng dùng trắng, trạng thái chọn xanh nhạt
`#D9F1F3`. Nền tối tăng sáng lên `#20323C`, thẻ `#2B414C`, ô nhập `#324C57`
để các lớp dễ phân biệt. Không dùng gradient.

Phương án này là lựa chọn thiết kế riêng của MAYAP sau khi đối chiếu:

- [Radix — composing a palette](https://www.radix-ui.com/colors/docs/palette-composition/composing-a-palette): phối nền trung tính pha cùng tông với màu chủ đạo; thêm bảng màu thương hiệu riêng.
- [Radix — use cases](https://www.radix-ui.com/colors/docs/palette-composition/understanding-the-scale): tách màu nền, trạng thái chọn, viền và chữ theo vai trò.
- [Atlassian — color](https://atlassian.design/foundations/color): tách brand với warning/danger; giá trị màu thay đổi theo theme và phải kiểm tra tương phản.
- [IBM Carbon — color](https://carbondesignsystem.com/elements/color/overview/): nền sáng dùng các lớp trắng/xám nhẹ; trong theme tối, lớp phía trên sáng hơn nền.

Đây không phải bảng màu sao chép từ các hệ thống trên. Nút dùng đúng
`#64C9D1` với chữ `#173E45` (khoảng 5,97:1); chữ trắng trên cùng màu chỉ
khoảng 1,94:1. Biểu tượng và chữ xanh ở theme sáng dùng `#286974`; theme tối
dùng `#92DCE2`. Viền nút, focus và công tắc được chỉnh theo theme.

Đường nhiệt độ và chú giải biểu đồ có màu riêng: xanh đậm khi sáng, màu
thương hiệu khi tối để giữ độ tương phản. `app.js` chỉ đổi token màu vẽ
đường nhiệt, không đổi dữ liệu, phép tính, kết nối hoặc giao dịch.

Cache tăng lên `12.1.2`. Kiểm tra trình duyệt vẫn gồm năm kích thước, cả
hai theme, nền đơn sắc, tương phản chữ/nút, điều hướng, biểu đồ và placeholder.

### Web 12.1.3 — căn chỉnh công tắc, icon và viền

Chấm tròn công tắc trước đây dùng kích thước content-box nên viền cộng thêm
vào đường kính, làm lệch tâm và sát mép khi bật. Thumb nay không có viền đậm,
được tính kích thước đầy đủ và căn theo tâm track; trạng thái bật/tắt đều cách
mép 3px, cho cả công tắc thường lẫn loại nhỏ. Label có vùng bấm cao 44px;
bỏ hit-area mở rộng tràn ra ngoài hàng. `switchRow` giữ bố cục flex của hàng.

Icon Cài đặt thay bằng đường vẽ đối xứng quanh `(12,12)`, nằm hoàn toàn trong
viewBox; ô icon điều hướng và icon trong thẻ được căn giữa thống nhất.

Bỏ pseudo-element hình tròn trợ giúp ở toàn bộ cài đặt. Dòng chữ vẫn bấm được
để xem hướng dẫn. Viền nút chính, nút thêm máy, ô nhập và các thẻ dùng chung
token nhẹ hơn: sáng `#DFECEF`, tối `#405966`; bóng đổ cũng giảm. Màu thương
hiệu `#64C9D1`, thông tin cảnh báo và logic vận hành giữ nguyên.

Cache tăng lên `12.1.3`. Browser QA đo thumb ở cả hai trạng thái, icon Cài đặt,
vùng bấm, viền dùng chung và không còn vòng tròn trợ giúp ở năm kích thước,
cả hai theme; kiểm tra label click, phím Space và mở hướng dẫn bằng dòng chữ.

### Web 12.1.4 — dải thông số đầu trang

Gom nhiệt độ, độ ẩm và trạng thái vào một dải trắng/xanh xám chung, chỉ có
viền ngoài nhẹ và hai đường chia ngắn. Bỏ viền, bóng và chữ in hoa của từng
ô; giá trị đo nổi bật hơn nhãn. Giữ nguyên màu thương hiệu và ý nghĩa màu lỗi.

Trên điện thoại và tablet dọc, tiêu đề và mô tả nằm trên dải thông số,
không bị ép vào cột nhỏ. Desktop và điện thoại ngang giữ một hàng để chừa
chiều cao cho nội dung. Các nhãn và giá trị căn cùng hàng, trạng thái không
làm đổi chiều cao khi xuất hiện cảnh báo. Popup chi tiết lỗi vẫn mở ngoài
dải thông số, không bị cắt bởi góc bo.

Nội dung Thiết bị/Mẻ ấp không co thấp hơn các thẻ bên trong rồi tràn vào
khoảng dành cho thanh điều hướng. Khi màn hình nhỏ, cuộn nội dung sẽ đưa
được toàn bộ nút lưu/bắt đầu lên vùng bấm; sửa selector `#quickForm` để
các quy tắc rút gọn hiện có thật sự áp dụng vào form.
Rà vận hành trên điện thoại phổ biến: giảm khoảng đệm/giãn cách trong thẻ
Thiết bị, giữ nguyên cỡ chữ và vùng chạm. Toàn bộ thẻ điều khiển cách thanh
điều hướng ít nhất 8px ở fixture 390/430×844; màn hình nhỏ hơn vẫn cuộn được.

Cache tăng lên `12.1.4`. Browser QA kiểm tra cả ba tab tại tám cấu hình
viewport, hai theme, năm trạng thái: đang ấp, ngoại tuyến, cảnh báo, lỗi dừng
và khẩn cấp. Kiểm tra chữ không bị cắt, vị trí nhãn, chiều cao dải và việc
mở/đóng popup có đủ mọi lỗi đang hoạt động. Không sửa `app.js`, firmware,
Adaptive Boot, Runtime Recovery hoặc giao dịch điều khiển.

### Web 12.1.5 — khung vận hành, theme tối và vuốt

Tiêu đề Thiết bị/Mẻ ấp/Cài đặt cùng hàng với thông số trên điện thoại.
Ẩn mô tả phụ ở màn hình nhỏ để dành chỗ cho nội dung vận hành. Trên PC,
đầu trang và Mẻ ấp dùng chung hai cột: dải thông số có đúng chiều rộng và
vị trí ngang của biểu đồ nhiệt độ. Cột trạng thái dành đủ chỗ cho cả
“Ngoại tuyến”, không phụ thuộc việc máy có đang kết nối hay không.

Hai trang Thiết bị/Mẻ ấp nhận chiều cao viewport. Các thẻ giữ chiều cao
nội dung; biểu đồ nhận phần còn lại, canvas không làm bố cục tự nở khi
đổi kích thước. Màn hình thấp dùng khoảng đệm gọn hơn; ở 390×667 dùng
ba cột trạng thái đầu ra. Vẫn giữ nhãn, đơn vị và vùng chạm ít nhất 44px.
Browser QA yêu cầu không cuộn ở fixture vận hành tại 320×844, 390×667,
390/430/768/850/1440×844, 1025×768 và 1719×600. 320×568 và điện thoại
ngang 844×390 được cuộn vì nội dung thực sự cao hơn vùng hiển thị.
Thêm thiết bị tùy chọn, chữ lớn hoặc bàn phím mở có thể cần cuộn;
không cắt nội dung để giả tạo một trang vừa màn hình.

Theme tối dùng nền `#17272F`, thẻ `#243944`, ô `#304B58`, chữ chính
`#F4FAFC` và chữ phụ `#C4D5DC`. Bỏ bóng và blur đầu trang; nền/thẻ/chữ
phân biệt rõ hơn, giữ `#64C9D1` cho nút chính. Màu cảnh báo và ý nghĩa
lỗi giữ nguyên. Các cặp chữ/nền được QA có tương phản ít nhất 4,5:1.

Vuốt hiển thị dịch chuyển theo ngón tay qua requestAnimationFrame,
giới hạn 72px, chỉ dùng transform. Thả một vuốt hợp lệ chuyển cảnh
70ms + 150ms bằng Web Animations; vuốt ngắn/hủy trả về 120ms. Giữ
ngưỡng đổi tab hiện có, bảo vệ nhập liệu/nút/hộp thoại và hỗ trợ màn
hình ngang. Không gửi lệnh máy khi vuốt. Thiết lập giảm chuyển động
của hệ thống bỏ chuyển cảnh. Đổi tab đưa cả khung cuộn nội bộ và trang
về đầu, giữ dữ liệu form đang sửa.

`html`, `body` và vùng dưới thanh tab cùng màu nền. `theme-color` được
cập nhật theo theme trước khi vẽ và khi đổi lựa chọn/hệ thống; manifest
dùng nền sáng làm màu khởi chạy mặc định. `viewport-fit=cover` giữ nguyên.
Padding đáy dùng safe-area-max-inset-bottom với fallback; vị trí thanh
tab theo safe-area-inset-bottom, để biến động thanh trình duyệt không
làm đổi chiều cao nội dung. QA giả lập inset trên 24px, dưới 34px và
ẩn/hiện vùng dưới, kiểm tra nền/điểm chạm/chiều cao nội dung.

Theo [Chrome edge-to-edge](https://developer.chrome.com/docs/css-ui/edge-to-edge),
khả năng web phủ vùng điều hướng Android phụ thuộc chế độ điều hướng
và phiên bản trình duyệt. Đặt theme-color và màu gốc là phần web kiểm
soát được; không thể bảo đảm màu thanh ba nút của mọi ROM bằng CSS.
[WebKit](https://webkit.org/blog/7929/designing-websites-for-iphone-x/)
hướng dẫn nền html/body phủ vùng inset và bảo vệ thao tác bằng safe-area.
Chưa xác minh trên chính điện thoại Android trong ảnh hoặc Safari thật.

Cache tăng lên `12.1.5`; kiểm tra nâng cache từ `12.1.4` và xóa cache cũ.
Lần này chỉ sửa UI trong `app.js`, CSS, HTML, manifest/cache/release,
README, browser QA và tài liệu này. Firmware, Adaptive Boot, Runtime
Recovery, safety và các hàm giao dịch bảo vệ bằng hash giữ nguyên.

### Web 12.1.6 — chiều cao khung và tám ô trạng thái

Ở PC (từ 1025px và cao hơn 500px), hàng Mẻ ấp lấy chiều cao từ nội
dung form thay vì kéo tới đáy viewport. Hai khung cấu hình/biểu đồ vẫn
cùng cao và cùng đáy; khung cấu hình kết thúc sau hai nút Lưu/Bắt đầu
cùng khoảng đệm. Giữ dải thông số thẳng cột biểu đồ. Layout Mẻ ấp dọc
trên mobile tiếp tục dùng phần chiều cao còn lại cho biểu đồ.

Khôi phục mô tả dưới tiêu đề mobile, chữ 10px và xuống dòng tự nhiên,
vẫn giữ tiêu đề cùng hàng với thông số. Tám ô Thiết bị dùng một wrapper
`.deviceStatusGrid`; hai nhóm DOM cũ dùng display:contents trên mobile,
giữ IDs, input/label và các nút thao tác hiện có. Các hàng có chiều cao
bằng nhau, tính cả hai ô nhập Nhiệt độ đặt/Chu kỳ đảo. Thẻ trạng thái
nhận chiều cao còn lại và cách thanh tab 10px khi đủ chỗ. Không kéo giãn
thẻ đầu chọn máy, không che nội dung khi màn hình ngắn.

Thông thường dùng hai cột; điện thoại rộng 361–800px và cao tối đa
700px dùng ba cột gọn để tận dụng chiều ngang. Thiết bị không có tạo
ẩm vẫn ẩn ô tùy chọn, không để CSS ghi đè hidden. Khi có tạo ẩm thì ô
thứ chín vẫn xuất hiện, các ô cùng cao; màn hình ngắn có thể cần cuộn.
Vùng chạm và bảo vệ lệnh máy giữ nguyên.

Vuốt cập nhật tab ngay khi thả thay vì chờ chuyển cảnh đi ra 70ms.
Chỉ giữ chuyển cảnh vào 100ms, không fade làm nội dung mờ/đứt đoạn;
vuốt hụt trở về trong 80ms. Phản hồi theo ngón tay tăng từ hệ số 0,3
lên 0,6, giới hạn 96px. Giữ ngưỡng đổi tab, guard nhập liệu/nút/modal
và giảm chuyển động. Browser QA đo release-to-commit trong một frame
budget ở fixture, kiểm tra animation không quá 100ms; chưa đo FPS
hoặc cảm giác vuốt trên chính điện thoại của người dùng.

Browser QA thêm màn PC 1734×965 tái hiện ảnh người dùng, kiểm tra đáy
hai khung Mẻ ấp, khoảng cách từ nút tới đáy khung, tám ô có cùng chiều
cao, subtitle nhìn thấy và thẻ trạng thái sát thanh tab với khoảng an
toàn. Kiểm tra sáng/tối, online/offline/chưa ghép nối và thiết bị tạo ẩm.
Cache/release web tăng lên 12.1.6. Chỉ sửa phần UI vuốt trong app.js;
không sửa firmware, boot, runtime recovery hoặc giao dịch điều khiển.

Trên mobile, vuốt ngang sang tab bên cạnh theo thứ tự Thiết bị → Mẻ ấp →
Cài đặt, vuốt ngược để quay lại. Không vòng từ tab cuối sang tab đầu.
Vuốt dọc, vuốt tại ô nhập/nút/summary, thao tác nhiều ngón, mép màn hình
và hộp thoại không đổi tab. Chuyển tab giữ lại nội dung đang sửa và không
tự gửi cài đặt.

## Vì sao mở web/ghép nối chậm và phần đã cải thiện

| Điểm chờ | Thay đổi |
|---|---|
| Thêm máy xong tự tải lại trang sau 400ms, phải xin phiên và kết nối lần nữa | Bỏ reload; dùng credential vừa xác thực, giữ trang và mở/tái sử dụng kết nối hiện tại |
| Thư viện MQTT từ CDN ngoài | Đóng gói nguyên bản MQTT.js 5.13.2, có license; cache vendor theo phiên bản web |
| Script tải nối tiếp và chặn parser | `defer` theo đúng thứ tự, tải song song và chạy sau khi đọc HTML |
| 7 lời gọi SUBSCRIBE và SUBACK riêng | Gộp thành một SUBSCRIBE, giữ nguyên 7 topic/QoS và kiểm tra đủ grant; gộp yêu cầu trùng |
| Bản tin xin đồng bộ QoS0 đầu tiên bị mất | Thử thêm ở 700ms và 1600ms, chỉ khi còn thiếu snapshot/config; giữ chu kỳ hiện có 3 giây |
| SUBACK mất hoặc trả sau đổi kết nối | Timeout 8 giây, thử lại sau 3 giây; không nhận grant từ kết nối cũ/đã timeout |
| API cấp phiên lỗi tạm thời lúc mở trang | Timeout 10 giây cho yêu cầu cấp phiên, retry nền với backoff 5/10/20/30 giây; thử lại khi Internet trở về |
| Ghép nối hết hạn/thu hồi | HTTP 401/403 dừng retry tự động, yêu cầu ghép nối lại bằng PIN |

Bảy SUBSCRIBE cũ được gửi song song, vì vậy gộp packet không có nghĩa là
tăng tốc bảy lần. Thời gian xác thực, DNS/TLS/WebSocket/MQTT và phản hồi
thật từ máy vẫn phụ thuộc Internet, máy chủ và Wi-Fi. Không bỏ xác thực
hoặc báo trực tuyến chỉ dựa vào retained presence. Không thể hứa độ trễ
bằng 0 hoặc một thời gian cố định cho mọi mạng.

## Bảo toàn giao dịch và kiểm thử

MQTT topics/protocol, request ID, revision, HMAC, TTL, retry cùng signed
envelope, ACK cuối, UNCERTAIN và đối chiếu báo cáo bộ nhớ giữ nguyên.
Không coi PUBACK/received là lưu thành công. Credential MQTT và khóa
điều khiển vẫn nằm trong RAM; không ghi password xuống localStorage.
Timeout HTTP chỉ thêm cho cấp phiên kết nối/điều khiển, không áp cho API
đổi PIN/đổi tên hoặc thay đổi quy tắc giao dịch ghi cài đặt.

Manifest bảo toàn cũ đóng băng toàn bộ `app.js`, bao gồm cả giao diện.
Với phạm vi chỉnh web đã được yêu cầu, nó được thay bằng hash các hàm
giao dịch quan trọng từ `a60fe7e`; chỉ cho phép các thay đổi câu chữ được
liệt kê chính xác. Các kiểm thử giao dịch cũ vẫn giữ assertion hiện có.
Hash firmware/Boot/safety/schema/protocol vẫn giữ nguyên.

- `node --test tests/*.test.cjs`: 50/50 bài tại thời điểm sửa.
- Các kiểm tra release, ATtiny, reliability, EEPROM history và secrets qua.
- Browser QA Chrome thật: 320/390/430/768/1440px, sáng/tối; ba tab không
  tràn ngang; vuốt touch, cuộn dọc, nhập liệu, hộp thoại và giữ nội dung sửa.
- Kiểm tra API 503, mất sync đầu, pairing không reload, một MQTT client,
  credential không nằm trong native persistent storage và theme theo hệ thống.
- Tương phản các cặp màu chữ/nền và nút chính đã kiểm tra ≥4,5:1 ở hai theme.
- Các workflow hiện có tiếp tục chạy test C++/ASan/UBSan và compile ESP32 + ATtiny.

Browser QA dùng fixture và mock API/MQTT; không liên lạc hoặc điều khiển
máy thật. Khoảng 700ms đo được là thời điểm **retry sync**, không phải thời
gian ghép nối đầu-cuối của phần cứng. Chưa đo P50/P95 trên máy thật hoặc
kiểm tra thao tác bằng iPhone/Safari thật; cần nghiệm thu đó trước thương mại.

Chạy lại browser QA: phục vụ repo tại `http://127.0.0.1:8765` bằng HTTP
server cục bộ, cài Playwright bên ngoài repo, rồi chạy
`node tools/test_web_experience.cjs <thư_mục_ảnh>`.
`MAYAP_PLAYWRIGHT` chỉ đường dẫn package nếu không có trong node_modules;
`MAYAP_CHROME` chỉ executable Chrome tùy máy. Trên Windows mặc định dùng
Chrome đã cài; trên Linux dùng Chromium của Playwright.

## File thay đổi

`index.html`, `styles.css`, `app.js`, `config.js`, `sw.js`,
`manifest.webmanifest`, `release-manifest.json`, `README.md`,
`vendor/mqtt.min.js`, `vendor/mqtt-LICENSE.md`, `vendor/README.md`,
`tests/web-experience.test.cjs`, `tools/test_web_experience.cjs`,
`tests/runtime-preservation.json`, `tests/runtime-recovery.test.cjs`,
`tests/transaction-lifecycle.test.cjs` và tài liệu này.

Tham khảo API đã dùng: [MQTT.js 5.13.2](https://github.com/mqttjs/MQTT.js/blob/v5.13.2/README.md),
[Pointer Events](https://developer.mozilla.org/en-US/docs/Web/API/Pointer_events),
[touch-action](https://developer.mozilla.org/en-US/docs/Web/CSS/Reference/Properties/touch-action).
