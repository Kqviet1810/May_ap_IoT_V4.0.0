# Log Serial: `LOG CPU` và `LOG AI`

Hai chế độ log mới đọc qua cổng USB-C (Serial 115200) của board điều khiển. Cùng với log máy hiện có, **tại một thời điểm chỉ một chế độ được bật**:
bật chế độ này thì chế độ kia tắt ngay, hàng đợi của chế độ cũ bị xóa.

| Lệnh | Chế độ | Có trong bản Production |
|---|---|---|
| `LOG` (hoặc `SERIAL`) | Log máy: văn bản như hiện nay (`[PROCESS]`, `[HEAP]`, lỗi...) | Không (chỉ bản diagnostic) |
| `LOG CPU` | Heap, stack, chu kỳ điều khiển/HMI, nhiệt chip, % CPU từng lõi | **Có** |
| `LOG AI` | Một dòng CSV mỗi giây để huấn luyện / đánh giá mô hình nhiệt | **Có** |
| `LOG OFF` hoặc `EXIT` | Tắt log | **Có** |

Sau mỗi lần khởi động lại, chế độ là **TẮT**. Bản Production chỉ nhận đúng 4 lệnh trên (chữ hoa/thường đều được); mọi byte khác bị bỏ qua.
Các lệnh `TIME SET`, `BATCH`, `FAULT`, `CONFIG`... vẫn chỉ có ở bản diagnostic.

## Bảo đảm không ảnh hưởng vận hành

* Vòng điều khiển **không bao giờ** định dạng hay ghi chữ ra Serial. Nó chỉ sao chép một bản chụp nhỏ (khoảng 110 B, mỗi 200 ms) và chỉ khi `LOG AI` đang bật.
* Tác vụ giám sát (supervisor) là nơi duy nhất ghi Serial, mỗi lượt (50 ms) tối đa **một** dòng.
* Không có máy tính đọc cổng: hàng đợi (4 dòng ở bản Production, 8 dòng ở bản diagnostic) đầy thì dòng mới bị bỏ và được đếm (`drop`, khoảng trống ở `seq`); không chờ, không chặn.
* Tự tắt: chu kỳ điều khiển ≥ 100 ms trong 3 lượt liên tiếp, hoặc stack của supervisor còn dưới 2 KB, thì log tự về TẮT (có dòng `[LOG] AUTO-OFF ...`).
* Log không đọc đầu ra, cấu hình hay bộ nhớ. Ngưỡng High 38,2 °C / Emergency 39,0 °C, `OutputArbiter` và AutoTune không đổi.

## `LOG AI` (CSV, 1 dòng/giây)

Đầu luồng (và lặp lại mỗi 10 phút) có 3 dòng bắt đầu bằng `#`: tiêu đề `#MAYAP-AI-LOG schema=1 fw=...`, cấu hình `#cfg sp= kp= ki= kd=` và dòng tên cột.
Mỗi dòng dữ liệu kết thúc bằng `,<CRC-8>` (đa thức 0x07, tính trên toàn bộ chữ đứng trước dấu phẩy cuối). Dòng sai CRC hoặc thiếu `seq` là dữ liệu hỏng/mất.

| Cột | Ý nghĩa |
|---|---|
| `seq` | Số thứ tự dòng từ lúc bật chế độ; **khoảng trống = dòng bị mất** |
| `t` | Thời gian từ lúc bật nguồn (ms) |
| `age` | Tuổi của bản chụp (ms); bình thường nhỏ hơn khoảng 250, lớn hơn là vòng điều khiển chậm |
| `day` | Ngày ấp |
| `sp` | Nhiệt đặt (°C) |
| `pv` | Nhiệt đo đã lọc, đã bù lệch (°C) |
| `raw` | Nhiệt thô của cảm biến (°C) |
| `rh` | Độ ẩm (%) |
| `req` | Công suất heater yêu cầu cuối cùng sau mọi giới hạn (%) |
| `ssrms` | Tổng thời gian heater đã BẬT (ms), lấy mẫu 5 Hz; hiệu hai dòng liên tiếp = thời gian bật thực trong khoảng đó |
| `avg60` | Trung bình trượt 60 s của tỷ lệ SSR bật thực (%) |
| `fl` | Cờ (hex): bit0 mẻ chạy, 1 tự động, 2 cảm biến tốt, 3 SSR đang bật, 4 contactor heat-master, 5 quạt đảo, 6 quạt hút, 7 tạo ẩm, 8 đèn, 9 adaptive, 10 smart, 11 còi, 12 đảo trứng, 13 đang AutoTune |
| `pid` | P + I + D (không gồm feed-forward) |
| `hff` | Feed-forward giữ nhiệt đã học |
| `vff` | Feed-forward quạt hút |
| `lo` | Cận dưới hành lang quyền của adaptive (%) |
| `hi` | Cận trên hành lang quyền của adaptive (%) |
| `ts` | Trạng thái bộ học nhiệt (0 chưa học ... xem `thermal_learner.h`) |
| `tc` | Độ tin cậy bộ học (0–100) |
| `tg` | Hệ số gia nhiệt học được Kh (°C/s ở 100%) |
| `td` | Thời gian trễ học được (s) |
| `th` | Công suất giữ nhiệt học được (%) |
| `tk` | Độ trôi nhiệt sau khi tắt (°C) |
| `tv` | Hệ số làm mát của quạt hút |
| `te` | Sai số dự đoán của bộ học |
| `vp` | Pha quạt hút |
| `al` | Mặt nạ cảnh báo (hex) |
| `fc` | Mã lỗi chính |
| `sc` | Mã trạng thái máy |
| `at` | Trạng thái AutoTune (0 nghỉ, 1 chạy, 2 xong, 3 hỏng) |
| `ap` | Tiến độ AutoTune (%) |
| `drop` | Tổng số dòng log đã bị bỏ từ khi bật nguồn |
| `crc` | CRC-8 của dòng |

Chưa có trong log (chưa có cảm biến): nhiệt phòng, điện áp lưới, trạng thái cửa. Khi cần, gắn thêm cảm biến rồi mở rộng schema (tăng `schema=`).

Khoảng 150–190 B/dòng, tức khoảng 13–16 MB/ngày; băng thông cổng dùng chưa tới 2%.

## `LOG CPU`

Đầu luồng: `#MAYAP-CPU-LOG ...` và một dòng `[CPU] static RAM: data= bss= iram=` (hằng theo bản build, -1 = không đọc được). Sau đó:

* mỗi 10 s: `[CPU] t= heap free= min= largest= total= chip=…C drop=`, `[CPU] stack peak/size B: ctrl=.. hmi=.. sup=..` và `[CPU] cycle us: control last= max= | hmi last= max=`;
* mỗi 60 s: lấy mẫu CPU trong 2 s (đo thời gian idle-hook của từng lõi) rồi in `[CPU] load core0=..% core1=..%`. Nếu không đăng ký được idle-hook sẽ in `load n/a`.

`chip` là nhiệt cảm biến trong chip (chỉ xem xu hướng, không phải nhiệt lò). Đo CPU là ước lượng, và bộ lấy mẫu này **chưa được kiểm chứng trên mạch thật**.

## Kiểm tra trên mạch thật trước khi tin vào kết quả

1. Nạp bản có log, mở terminal Serial 115200, gõ `LOG CPU`, kiểm `ctrl last/max` và `sup` stack peak.
2. Rút USB 10 phút ở từng chế độ (`LOG CPU`, `LOG AI`): chu kỳ điều khiển tối đa (`ctrl max`) không được tăng so với khi log TẮT, máy không reset.
3. `LOG AI` ghi ≥ 1 giờ: kiểm CRC từng dòng, `seq` liên tục, `drop` không tăng.

Ví dụ kiểm CRC bằng Python (cột cuối là CRC, phần trước dấu phẩy cuối được tính):

```python
def crc8(s):
    c = 0
    for b in s.encode():
        c ^= b
        for _ in range(8): c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c
line = open('log.csv').readline().rstrip('\n')
body, crc = line.rsplit(',', 1)
assert crc8(body) == int(crc, 16)
```

## Ghi dài ngày

Cổng USB-C của ESP32-S3 không nối được vào CYD (CYD không làm USB host). Giai đoạn đầu ghi bằng máy tính (terminal có ghi file). Ghi 21 ngày bằng CYD
cần thêm một chân UART TX trên board điều khiển; việc này làm đợt sau, sau khi kiểm tra phần cứng.
