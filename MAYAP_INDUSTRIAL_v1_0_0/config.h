#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>

#include "build_public.h"

// ============================================================================
// MAY AP TRUNG INDUSTRIAL v3.4.0 - CAU HINH DUY NHAT CAN SUA
// MCU: ESP32-S3-WROOM-1U-N8, FLASH THAT 8MB (da xac nhan qua "esptool.py
// flash_id" -> "Detected flash size: 8MB"), KHONG PSRAM.
//
// Cau hinh Arduino IDE BAT BUOC (Tools menu):
//   - Flash Size        = "8MB (64Mb)"
//   - Partition Scheme   = "8M with spiffs (3MB APP/1.5MB SPIFFS)"
//                          (2 khe OTA that su, ~3.1875MB/khe, du rong cho
//                          firmware hien tai ~1.3MB)
//
// SU CO THUC TE da gap va da xac dinh dut diem nguyen nhan: co lan Flash
// Size bi de nham "4MB" trong khi Partition Scheme van dung "8M with
// spiffs" - Arduino IDE khai bao flash 4MB vao header anh nap, nen bootloader
// tinh khe OTA thu 2 (ota_1, offset 0x340000 size 0x330000 - dung 2 khe cua
// scheme nay) VUOT QUA gioi han 4MB gia (0x400000) du chip that la 8MB ->
// "boot: Failed to verify partition table" -> may khong boot duoc. Danh sach
// gia tri hop le da doi chieu truc tiep qua `arduino-cli board details -b
// esp32:esp32:esp32s3 --full` (khong doan mo ho): FlashSize hop le la
// 4M/8M/16M/32M; Partition Scheme "8M with spiffs" co khoa noi bo la
// "default_8MB" (KHAC voi "huge_app" - "Huge APP" ghi ro la "No OTA", chi
// 1 khe, KHONG dung cho tinh nang cap nhat firmware cua du an nay).
// ============================================================================

constexpr char MAYAP_FIRMWARE_VERSION[] = "1.1.2";
#ifndef MAYAP_BUILD_REVISION
#define MAYAP_BUILD_REVISION local
#endif
#define MAYAP_BUILD_STRING_IMPL(value) #value
#define MAYAP_BUILD_STRING(value) MAYAP_BUILD_STRING_IMPL(value)
constexpr char MAYAP_BUILD_ID[] = MAYAP_BUILD_STRING(MAYAP_BUILD_REVISION) " " __DATE__ " " __TIME__;
constexpr char MAYAP_HARDWARE_REVISION[] = "CTRL-S3-N8-R1";
constexpr char HMI_FIRMWARE_VERSION[] = "1.0.0";
constexpr char HMI_HARDWARE_REVISION[] = "HMI-S3-R2";

// ----------------------------- BUILD -----------------------------------------
// 1: mo phong 8 input bang Serial. 0: doc input 12 V that qua opto.
#ifndef MAYAP_SERIAL_INPUT_SIM
#define MAYAP_SERIAL_INPUT_SIM 0
#endif

// Log chan doan khong tham gia dieu khien. Nen de 0 o ban giao thuong mai.
#ifndef MAYAP_DIAGNOSTIC_SERIAL
#define MAYAP_DIAGNOSTIC_SERIAL 1
#endif

// Thong tin Wi-Fi chi duoc dung khi nguoi van hanh chon ONLINE tren HMI.
// De trong SSID de vo hieu hoa ket noi mang ngay ca khi vo tinh chon ONLINE.
// Co the thay hai macro nay bang build flags de khong ghi mat khau vao source.
#ifndef MAYAP_WIFI_SSID
#define MAYAP_WIFI_SSID ""
#endif
#ifndef MAYAP_WIFI_PASSWORD
#define MAYAP_WIFI_PASSWORD ""
#endif
constexpr char NETWORK_WIFI_SSID[] = MAYAP_WIFI_SSID;
constexpr char NETWORK_WIFI_PASSWORD[] = MAYAP_WIFI_PASSWORD;
constexpr char NETWORK_WIFI_HOSTNAME[] = "mayap-industrial";
static_assert(sizeof(NETWORK_WIFI_SSID) <= 33U,
              "Wi-Fi SSID toi da 32 ky tu");
static_assert(sizeof(NETWORK_WIFI_PASSWORD) <= 64U,
              "Wi-Fi password toi da 63 ky tu");
static_assert(sizeof(NETWORK_WIFI_HOSTNAME) <= 33U,
              "Wi-Fi hostname toi da 32 ky tu");

// ------------------------- Nap firmware ------------------------------------
// Nap truc tiep chi qua cap USB. Khong con nap qua mang bang Arduino IDE (ArduinoOTA/mDNS da
// bi go bo: khong mo cong lang nghe nao tren LAN). Cap nhat tu xa chi qua Cloudflare co ky so
// (ota_web_update.h), sau khi nguoi van hanh xac nhan tren HMI.

// ------------------------- Application transaction cadence -------------------
// Generic bounded publication intervals for a future transport owner.
constexpr uint32_t REALTIME_SNAPSHOT_ACTIVE_INTERVAL_MS = 1000UL;
constexpr uint32_t REALTIME_SNAPSHOT_IDLE_INTERVAL_MS = 120000UL;
constexpr uint32_t REALTIME_COMMAND_ACK_TIMEOUT_MS = 8000UL;
constexpr uint32_t REALTIME_CONFIG_SAVE_ACK_TIMEOUT_MS = 8000UL;

// --------------------------- Cloud Push (Cloudflare Worker, doc lap voi Web) ---
// KENH RIENG, KHONG DI QUA MQTT/WEB: cloud_alert_link.h tu mo ket noi HTTPS
// rieng toi Cloudflare Worker (xem thu muc cloudflare/), khong phu thuoc
// broker MQTT hay Web con song hay khong. Thay the hoan toan kenh Telegram cu.
// device_key la bi mat cua firmware (nhu mat khau Wi-Fi/MQTT o tren) - dat qua
// build_flags, KHONG hard-code truc tiep truoc khi build ban thuong mai.
// KHAC Telegram truoc day (can nhap Chat ID): nguoi dung cuoi KHONG can cau
// hinh gi tren ESP32/cong Wi-Fi cho kenh nay - device_id (tu MAC, xem
// mayapDeviceIdText()) la dinh danh cong khai, viec "ghep" trinh duyet nhan
// thong bao hoan toan thuc hien o phia trang web (xem push.js/setup.html).
#ifndef MAYAP_DEVICE_SECRET
#define MAYAP_DEVICE_SECRET ""
#endif
constexpr char CLOUD_DEVICE_SECRET[] = MAYAP_DEVICE_SECRET;

#ifndef MAYAP_CLOUD_API_HOST
#define MAYAP_CLOUD_API_HOST "mayap-push-worker.vietk-mayaptrung.workers.dev"
#endif
// Chi ten host, KHONG "https://" o dau (vd: "mayap-push-worker.abc.workers.dev"
// hoac "api.tenmiencuaban.vn" neu da gan custom domain cho Worker).
constexpr char CLOUD_API_HOST[] = MAYAP_CLOUD_API_HOST;

// Chuoi PEM gom mot hoac nhieu CA goc tin cay. Ban thuong mai phai nhung
// qua build secret. De rong => cac kenh TLS that bai dong, tuyet doi khong
// ha cap sang che do TLS khong xac thuc.
#ifndef MAYAP_TLS_ROOT_CA
#define MAYAP_TLS_ROOT_CA ""
#endif
constexpr char TLS_ROOT_CA[] = MAYAP_TLS_ROOT_CA;
// Khong cho phep tao file firmware cloud "gia hop le" khi thieu CA: truoc
// day macro rong van bien dich va chi den khi chay moi in "TLS bi khoa".
// Loi nay phai dung NGAY luc bien dich de khong co ban nap loi ra thiet bi.
static_assert(sizeof(TLS_ROOT_CA) > 1U,
              "THIEU MAYAP_TLS_ROOT_CA: them PEM CA vao build_public.h truoc khi bien dich");

#ifndef MAYAP_OTA_SIGNING_PUBLIC_KEY
#define MAYAP_OTA_SIGNING_PUBLIC_KEY ""
#endif
constexpr char OTA_SIGNING_PUBLIC_KEY[] = MAYAP_OTA_SIGNING_PUBLIC_KEY;

// Nhip kiem tra dieu kien canh bao - rut tiep tu 2s xuong 0.5s de loi that
// (cam bien, cong tac nhiet...) duoc phat hien va day vao hang doi gui nhanh
// hon nua; ban than buoc kiem tra khong co I/O mang (chi so sanh bien trong
// RAM, mo/dong 1 critical section rat ngan) nen re, khong anh huong hieu
// nang - chi con bi gioi han boi nhip 250ms cua networkTask (NETWORK_TASK_
// PERIOD_MS). Do tre CON LAI (thuong 0.5-2s) la TLS handshake + HTTPS that
// su qua mang toi Worker + Apple/Google giao thong bao ve may - nam ngoai
// kha nang toi uu them cua firmware (xem giai thich day du trong bao cao
// gui nguoi dung).
constexpr uint32_t CLOUD_CHECK_INTERVAL_MS = 500UL;
// Khoang cach toi thieu giua 2 lan goi HTTPS that su, tranh don don nhieu tin
// cung luc khi nhieu loi phat sinh gan nhau (moi lan goi block networkTask
// vai giay do TLS handshake, nen khong the/khong nen ban song song).
constexpr uint32_t CLOUD_MIN_SEND_GAP_MS = 3000UL;
constexpr uint32_t CLOUD_HTTP_TIMEOUT_MS = 8000UL;
constexpr uint32_t CLOUD_HTTP_CONNECT_TIMEOUT_MS = 5000UL;
// Cap nhat firmware TU XA qua Cloudflare (ota_web_update.h) - nhip tu kiem
// tra ban moi khi dang ONLINE. Khong can nhanh: nguoi van hanh van phai tu
// tay xac nhan tren HMI moi thuc su tai ve/nap, day chi la "co gi moi khong".
// Moi lan kiem tra la mot phien TLS rieng (~70 kB heap luc cao diem) nen chi lam 1 lan/ngay, lech
// ngau nhien de cac may khong cung goi mot luc, va lan DAU cho vai phut sau khi bat nguon de
// khong tranh phien TLS voi luc MQTT/Cloud dang bat tay. Nut "kiem tra ngay" van co san.
constexpr uint32_t FIRMWARE_CHECK_INTERVAL_MS = 24UL * 60UL * 60UL * 1000UL;  // 24 gio
constexpr uint32_t FIRMWARE_CHECK_JITTER_MS = 30UL * 60UL * 1000UL;           // + 0..30 phut
constexpr uint32_t FIRMWARE_FIRST_CHECK_DELAY_MS = 10UL * 60UL * 1000UL;      // lan dau: 10 phut sau khi bat nguon
// Nhip bao "con song" len Worker (cap nhat last_seen/status trong D1). Ngoai
// hien thi trang thai lien ket tren web, day cung la co so de Worker phat
// hien mat dien/mat mang (xem cloudflare/src/index.js::checkDeviceConnectivity,
// nguong DEVICE_OFFLINE_THRESHOLD_MS) - giu ty le >=4-5 lan nhip/nguong de
// tranh bao nham khi mang chi chap chon vai chuc giay. 15s la muc nhanh nhat
// hop ly cho kenh HTTPS dinh ky kieu nay. Cron kiem tra phia
// Worker toi da 1 phut/lan (san co, gioi han cua nen tang) nen day la do
// tre nhanh nhat dat duoc cho kenh bao qua dien thoai voi kien truc hien tai.
constexpr uint32_t CLOUD_HEARTBEAT_INTERVAL_MS = 60000UL;
// Chu ky nhac lai khi loi con ton tai (tuy muc do - CRITICAL nhac nhanh hon
// WARNING nhu yeu cau). "Info" gan nhu khong dung cho loi that (chi day phong).
constexpr uint32_t CLOUD_REPEAT_WARNING_MS = 600000UL;    // 10 phut
constexpr uint32_t CLOUD_REPEAT_CRITICAL_STOP_MS = 300000UL;      // 5 phut
constexpr uint32_t CLOUD_REPEAT_CRITICAL_EMERGENCY_MS = 120000UL; // 2 phut
constexpr uint32_t CLOUD_REPEAT_INFO_MS = 1800000UL;      // 30 phut (du phong)
// Nhac lai neu den van bat trong luc me ap dang chay (xem
// cloud_alert_link.h::checkLightAfterBatch) - co the tat rieng qua
// MachineConfig::lightAfterBatchAlarmEnabled, khong anh huong cac canh bao khac.
constexpr uint32_t CLOUD_LIGHT_AFTER_BATCH_REPEAT_MS = 1800000UL; // 30 phut

// ---------------------------------------------------------------------------
// Nguong cho nhom canh bao bo sung (ra soat toan dien - xem machine_control.h
// FaultCode::HumidityHigh/TemperatureRateExceeded/TemperatureUnstable/
// HeaterNotHeating va cloud_alert_link.h checkBatchSchedule/checkTurnCycle
// Missed/checkMaintenanceReminder/checkWifiSignal). CO CHU DICH de o dang
// hang so co dinh (khong them truong MachineConfig/khong doi CONFIG_SCHEMA)
// de tranh dung lai loi migrate EEPROM da gap 2 lan truoc do - neu sau nay
// can chinh tay theo may, doi truc tiep gia tri o day roi nap lai firmware.
// ---------------------------------------------------------------------------
constexpr float HUMIDITY_HIGH_ALARM_C = 85.0f;
constexpr float HUMIDITY_HIGH_HYSTERESIS_C = 2.0f;
// Tao am 1 relay: OFF->ON khi RH <= setpoint - 2%%, giu ON den khi dat setpoint.
// Khong dung PID de tranh relay dong/ngat lien tuc.
constexpr float HUMIDIFIER_HYSTERESIS_RH = 2.0f;
constexpr uint8_t VENT_SCHEDULE_MAX_RUNS = 6U;
constexpr uint32_t RELAY_VENT_MIN_ON_MS = 120000UL;
constexpr uint32_t RELAY_VENT_MIN_OFF_MS = 120000UL;
// [DA CHUYEN SANG MachineConfig, schema 8] Toc do tang/giam nhiet bat thuong,
// dao dong nhiet mat on dinh, va nghi ngo SSR/relay dinh (thanh nhiet BAT lau
// ma nhiet khong tang) tung la hang so cung o day (TEMP_RATE_WINDOW_MS/
// TEMP_RATE_LIMIT_C, TEMP_OSCILLATION_WINDOW_MS/CROSS_LIMIT,
// HEATER_STUCK_DURATION_MS/MIN_RISE_C). Da chuyen thanh truong MachineConfig
// (tempRateWindowSec/tempRateLimitC, tempOscillationWindowSec/CrossLimit,
// heaterStuckDurationSec/MinRiseC - xem MachineConfig o duoi) de nguoi dung
// tu chinh qua HMI/web khi lap may that (dan nhiet cong suat lon co the can
// nguong khac mac dinh) ma khong can nap lai firmware. Gia tri mac dinh giu
// nguyen y het hang so cu.
// Bo lo lich dao: khong ghi nhan lan dao nao thanh cong qua
// TURN_MISSED_MULTIPLIER x chu ky dao da cau hinh.
constexpr uint8_t TURN_MISSED_MULTIPLIER = 2U;
// Nguong severity duoc coi la "nghiem trong" (may KHONG con ap duoc binh
// thuong nua) dung de hmi.h quyet dinh co bat buoc ngat ngang man hinh
// nguoi dung ve man Canh bao hay khong (xem applyRuntime() trong hmi.h).
// Khop dung FaultSeverity::Stop trong machine_control.h (0=Info,1=Warning,
// 2=Stop,3=Emergency) - dat rieng o day (config.h duoc include TRUOC hmi.h
// va machine_control.h) vi hmi.h khong the thay enum FaultSeverity luc do.
constexpr uint8_t FAULT_SEVERITY_STOP_THRESHOLD = 2U;
// Nhac truoc khi den ngay du kien no.
constexpr uint8_t BATCH_NEARING_END_DAYS_LEFT = 2U;
// Wi-Fi yeu keo dai (dBm cang am cang yeu) truoc khi bao, tranh bao ngay khi
// chi thoang qua yeu vai giay.
constexpr int8_t WIFI_RSSI_WEAK_DBM = -82;
constexpr uint32_t WIFI_RSSI_WEAK_DURATION_MS = 300000UL; // 5 phut

// ---------------------------------------------------------------------------
// GIAM SAT SUC KHOE HE THONG (v3.6.0) - du doan som truoc khi thanh loi that
// su xay ra, thay vi chi bao khi da qua nguong. Xem serviceHealthMonitor()
// trong machine_control.h. CO CHU DICH de hang so cung (giong nhom canh bao
// bo sung o tren) - day la nguong an toan he thong, khong phai tham so van
// hanh nguoi dung can chinh theo tung may.
// ---------------------------------------------------------------------------
// Chu ky lay mau suc khoe he thong (heap/nhiet do xu huong/EEPROM). Du nhe de
// chay moi chu ky dieu khien (5ms) ma khong ton chi phi dang ke.
constexpr uint32_t HEALTH_CHECK_INTERVAL_MS = 30000UL; // 30 giay
// Lay mau nhe trong ca cua so: HTTPS/TLS co the ha heap trong vai giay roi tra lai.
constexpr uint32_t HEALTH_HEAP_SAMPLE_INTERVAL_MS = 1000UL;
// Doi he thong chay on dinh sau boot roi moi chup heap nen (bo qua dinh cao
// tam thoi luc vua khoi dong cac dich vu Wi-Fi/MQTT/OTA).
constexpr uint32_t HEALTH_BASELINE_CAPTURE_DELAY_MS = 60000UL; // 60 giay
// % heap con lai SO VOI heap nen - duoi nguong nay lien tuc
// HEALTH_STREAK_CONFIRM lan (~streak*30s) moi bao, tranh bao gia do dao dong
// tam thoi (dang publish MQTT, dang parse JSON...).
constexpr uint8_t HEALTH_HEAP_WARN_PERCENT = 30U;
constexpr uint8_t HEALTH_HEAP_WARN_CLEAR_PERCENT = 35U; // tre (hysteresis) khi het canh bao
constexpr uint8_t HEALTH_HEAP_SERIOUS_PERCENT = 15U;    // len lich tu khoi dong lai luc an toan
constexpr uint8_t HEALTH_HEAP_CRITICAL_PERCENT = 6U;    // khoi dong lai NGAY, khong cho
constexpr uint8_t HEALTH_STREAK_CONFIRM = 3U;
// Du doan nhiet do: chu ky lay mau nen de tinh toc do, va thoi gian "nhin
// truoc" - neu toc do hien tai se cham nguong Bao cao/Bao thap trong khoang
// thoi gian nay thi bao truoc, KHONG cho doi that su cham nguong.
constexpr uint32_t HEALTH_TEMP_TREND_WINDOW_MS = 180000UL; // 3 phut/mau nen
constexpr float HEALTH_TEMP_TREND_MIN_RATE_C_PER_MIN = 0.02f; // bo qua troi noise
constexpr float HEALTH_TEMP_TREND_LOOKAHEAD_MIN = 5.0f; // canh bao neu <=5 phut nua cham nguong
// EEPROM phai thu lai (retry) qua nhieu lan trong 1 chu ky kiem tra - dau
// hieu suy giam som cua chip nho truoc khi hong han.
constexpr uint8_t HEALTH_EEPROM_RETRY_WARN_COUNT = 5U;

constexpr uint8_t CLOUD_OUTBOX_SIZE = 16U;
// >= HMI_FAULT_DISPLAY_CAPACITY (so loi dang active toi da doc duoc tu runtime
// snapshot moi lan), du du de theo doi tat ca dong thoi.
constexpr uint8_t CLOUD_ACTIVE_TRACK_SIZE = 48U;

// HMI chi duoc bien dich trong firmware tong; da loai bo demo doc lap.
#define MAYAP_HMI_OWNS_I2C_BUS 0
#define MAYAP_HMI_ENCODER_INTERRUPT 1

// ----------------------------- GPIO ------------------------------------------
// Output HIGH = ON.
constexpr uint8_t PIN_OUT_HEATER_SSR   = 1;   // Both SSRs, one logical 16 kW bank
constexpr uint8_t HEATER_GROUP_COUNT = 1U;
// Provisional commissioning default; final timing is not physically validated.
constexpr uint32_t HEATER_BURST_QUANTUM_MS = 300UL;
#ifdef MAYAP_HEATER_SSR_B_PIN
#error "This board has one heater control GPIO; both SSRs share GPIO1"
#endif
#if defined(MAYAP_SENSOR_TEMP_FORMAT) || defined(MAYAP_SENSOR_HUMIDITY_RAW16)
#error "Use MAYAP_SENSOR_PROFILE for paired temperature/humidity decoding"
#endif

// Paired T/RH Modbus profile: 0=AUTO, 1=X10/RH-X10, 2=X100/RH-X10,
// 3=native SHT30 T+RH. An explicit profile is still verified every boot.
// Service commissioning build: observe ON without actuator adaptation.
#ifndef MAYAP_ADAPTIVE_OBSERVER_ONLY
#define MAYAP_ADAPTIVE_OBSERVER_ONLY 0
#endif
static_assert(MAYAP_ADAPTIVE_OBSERVER_ONLY == 0 || MAYAP_ADAPTIVE_OBSERVER_ONLY == 1,
              "Observer-only override must be 0 or 1");
#ifndef MAYAP_SENSOR_PROFILE
#define MAYAP_SENSOR_PROFILE 0
#endif
static_assert(MAYAP_SENSOR_PROFILE >= 0 && MAYAP_SENSOR_PROFILE <= 3,
              "Invalid RS485 sensor profile");
constexpr float THERMAL_PID_BETA = 1.0f;
// Chan 2 truoc day du phong (PULSE_SPARE), sau do gan LED xanh bao "dang co
// me ap" - nay bo han tinh nang LED nay, chan 2 chuyen thanh coi HMI (xem
// PIN_BUZZER ben duoi). GPIO41 (coi HMI cu) tung de trong, nay da dung lai
// cho PIN_ATTINY_BUS (xem duoi).
// Pinmap thuc te da doi lai theo dung board dang lap (xem anh pinmap):
// TRAI <-> QUAT, PHAI <-> NHIET van giu nguyen tu v3.2.8.
// (Nghi ngo truoc do ve cheo chan HEAT_MASTER/TURN_RIGHT da duoc loai bo:
// trieu chung "bat cong tac nhiet ngoai me khong len relay" la CHU DICH
// phan mem - xem heatDemandContext trong updateHeatingAndOutputs(), khong
// phai loi chan. Giu nguyen pinmap theo anh.)
constexpr uint8_t PIN_OUT_CIRC_FAN     = 21;
constexpr uint8_t PIN_OUT_HEAT_MASTER  = 14;
constexpr uint8_t PIN_OUT_LIGHT        = 12;
constexpr uint8_t PIN_OUT_VENT_FAN     = 13;
constexpr uint8_t PIN_OUT_TURN_RIGHT   = 11;
constexpr uint8_t PIN_OUT_TURN_LEFT    = 10;
constexpr uint8_t PIN_OUT_SIREN        = 47;
constexpr uint8_t PIN_OUT_HUMIDIFIER  = 48;  // relay tao am tuy chon, mac dinh TAT
constexpr uint8_t PIN_STATUS_RGB       = 42;  // SK6812MINI-C
// Bus giao tiep 2 chieu voi ATtiny13A (mach bao mat dien doc lap dung pin
// CR2032, xem doc/attiny_power_alarm.md). La bus "ho tro" (open-drain) dung
// chung 1 day: ca 2 ben CHI duoc keo LOW hoac tha noi de nghi len HIGH qua
// dien tro keo len R8. ESP phat lenh bang GPIO/esp_timer + IRAM GPIO RX; Tiny tra
// trang thai. Pull-up noi 3.3 V cua ESP chi la du phong idle, khong thay R8.
constexpr uint8_t PIN_ATTINY_BUS = 41;

// Pulse-width protocol v4. Each command is one LOW pulse (milliseconds).
// Tiny returns preamble + four status bits + XOR parity; that frame is ACK.
constexpr uint16_t ATTINY_COMMAND_WIDTH_MS[8] = {0U, 30U, 55U, 90U,
                                                  140U, 210U, 310U, 450U};
constexpr uint8_t ATTINY_BUS_MAX_RETRY = 3U;

// ESP32 is the only initiator. Status flags preserve batch, 9V, siren and
// critical-activity reporting to the machine controller.
constexpr uint8_t ATTINY_PROTOCOL_VERSION = 4U;
constexpr uint8_t ATTINY_MSG_BATCH_START = 1U;
constexpr uint8_t ATTINY_MSG_BATCH_END = 2U;
constexpr uint8_t ATTINY_MSG_SIREN_ON = 3U;
constexpr uint8_t ATTINY_MSG_SIREN_OFF = 4U;
constexpr uint8_t ATTINY_MSG_STATUS_QUERY = 5U;
constexpr uint8_t ATTINY_MSG_ACTIVITY_ON = 6U;
constexpr uint8_t ATTINY_MSG_ACTIVITY_OFF = 7U;
constexpr uint8_t ATTINY_MSG_STATUS_BASE = 8U;
constexpr uint8_t ATTINY_MSG_STATUS_MAX = 23U;
constexpr uint8_t ATTINY_MSG_MAX_COMMAND = 7U;
constexpr uint8_t ATTINY_MSG_MAX_CODE = 23U;
constexpr uint8_t ATTINY_STATUS_FLAG_BATCH = 1U;
constexpr uint8_t ATTINY_STATUS_FLAG_9V_LOW = 2U;
constexpr uint8_t ATTINY_STATUS_FLAG_SIREN = 4U;
constexpr uint8_t ATTINY_STATUS_FLAG_ACTIVITY = 8U;
// One-wire open drain cannot detect a disconnected peer between transactions.
// Bound E501 detection with periodic STATUS probes. These intervals trade
// CR2032 life for fault-detection latency; measure actual PCB current.
constexpr uint32_t ATTINY_STATUS_ARMED_INTERVAL_MS = 10000UL;
constexpr uint32_t ATTINY_STATUS_IDLE_INTERVAL_MS = 30000UL;
constexpr uint32_t ATTINY_STATUS_RESPONSE_TIMEOUT_MS = 2500UL;
constexpr uint32_t ATTINY_RESYNC_RETRY_MS = 10000UL;
constexpr uint32_t ATTINY_SIREN_REASSERT_MS = 15000UL;
// ON duoc arm ngay. OFF phai on dinh 30 s moi ghi lai Tiny EEPROM, de gop
// cac dao dong ngan va giam so chu ky ghi EEPROM.
constexpr uint32_t ATTINY_ACTIVITY_OFF_CONFIRM_MS = 30000UL;
constexpr uint32_t ATTINY_9V_CONFIRM_MS = 3000UL;

// Input opto ACTIVE-LOW: kich 12 V => ngo ra opto keo GPIO xuong GND.
constexpr uint8_t PIN_IN_LIMIT_LEFT    = 4;
constexpr uint8_t PIN_IN_LIMIT_RIGHT   = 5;
constexpr uint8_t PIN_IN_AUTO          = 6;
constexpr uint8_t PIN_IN_HEATER_ENABLE = 7;
constexpr uint8_t PIN_IN_CIRC_FAN      = 15;
constexpr uint8_t PIN_IN_LIGHT         = 16;
constexpr uint8_t PIN_IN_TURN_LEFT     = 17;
constexpr uint8_t PIN_IN_TURN_RIGHT    = 18;  // da doi tu GPIO8 de tranh SDA

constexpr bool INPUT_ACTIVE_LOW = true;
constexpr bool OUTPUT_ACTIVE_HIGH = true;

// MAX3485 / Modbus RTU.
constexpr uint8_t SHT_UART_PORT = 1;
constexpr uint8_t PIN_RS485_RX = 37;     // RO
constexpr uint8_t PIN_RS485_DE_RE = 36;  // DE + /RE
constexpr uint8_t PIN_RS485_TX = 35;     // DI

// HMI ST7567 + rotary + coi phu duy nhat. GPIO2 bao phim, trang thai, dao va loi
// (truoc day la GPIO41 - da chuyen sang GPIO2 vi GPIO41 khong con dung, xem
// ghi chu tai PIN_OUT_HEATER_SSR ben tren).
// GPIO47 chi danh cho coi lon qua nhiet cap cao nhat.
constexpr uint8_t LCD_I2C_ADDRESS = 0x3F;
constexpr uint8_t PIN_I2C_SDA = 8;
constexpr uint8_t PIN_I2C_SCL = 9;
constexpr uint8_t PIN_ENCODER_CLK = 38;
constexpr uint8_t PIN_ENCODER_DT  = 39;
constexpr uint8_t PIN_ENCODER_SW  = 40;
constexpr uint8_t PIN_BUZZER      = 2;
constexpr bool BUZZER_ACTIVE_HIGH = true;

// Kiem tra toan bo GPIO tai compile-time.
constexpr uint8_t MAYAP_USED_PINS[] = {
  PIN_OUT_HEATER_SSR, PIN_OUT_TURN_RIGHT,
  PIN_OUT_TURN_LEFT, PIN_OUT_VENT_FAN, PIN_OUT_LIGHT,
  PIN_OUT_HEAT_MASTER, PIN_OUT_CIRC_FAN, PIN_OUT_SIREN,
  PIN_OUT_HUMIDIFIER, PIN_STATUS_RGB, PIN_ATTINY_BUS,
  PIN_IN_LIMIT_LEFT, PIN_IN_LIMIT_RIGHT, PIN_IN_AUTO,
  PIN_IN_HEATER_ENABLE, PIN_IN_CIRC_FAN, PIN_IN_LIGHT,
  PIN_IN_TURN_LEFT, PIN_IN_TURN_RIGHT,
  PIN_RS485_RX, PIN_RS485_DE_RE, PIN_RS485_TX,
  PIN_I2C_SDA, PIN_I2C_SCL, PIN_ENCODER_CLK, PIN_ENCODER_DT,
  PIN_ENCODER_SW, PIN_BUZZER
};

constexpr bool mayapPinsValidAndUnique() {
  constexpr size_t count = sizeof(MAYAP_USED_PINS) / sizeof(MAYAP_USED_PINS[0]);
  for (size_t i = 0; i < count; ++i) {
    if (MAYAP_USED_PINS[i] > 48U) return false;
    for (size_t j = i + 1; j < count; ++j) {
      if (MAYAP_USED_PINS[i] == MAYAP_USED_PINS[j]) return false;
    }
  }
  return true;
}
static_assert(mayapPinsValidAndUnique(), "MAYAP: GPIO trung nhau/ngoai pham vi");

// ----------------------------- HMI -------------------------------------------
constexpr uint8_t HMI_MAX_VALID_GPIO = 48;
constexpr uint8_t HMI_USED_PINS[] = {
  PIN_I2C_SDA, PIN_I2C_SCL, PIN_ENCODER_CLK, PIN_ENCODER_DT,
  PIN_ENCODER_SW, PIN_BUZZER
};
constexpr bool hmiPinsAreValidAndUnique() {
  constexpr size_t count = sizeof(HMI_USED_PINS) / sizeof(HMI_USED_PINS[0]);
  for (size_t i = 0; i < count; ++i) {
    if (HMI_USED_PINS[i] > HMI_MAX_VALID_GPIO) return false;
    for (size_t j = i + 1; j < count; ++j) {
      if (HMI_USED_PINS[i] == HMI_USED_PINS[j]) return false;
    }
  }
  return true;
}
static_assert(hmiPinsAreValidAndUnique(), "HMI: GPIO trung nhau/ngoai pham vi");

// QUAY LAI 100kHz - tung thu ha xuong 50kHz de tang chong nhieu, nhung
// nguoi dung bao HMI bi lag ro sau doi nay - dung nhu du doan: hmiTask ve
// man va doc encoder tren CUNG 1 task/vong lap (xem hmiUpdate()), nen moi
// lan sendBuffer() (~1KB) BLOCK toan bo task, ke ca doc nut xoay, cho toi
// khi gui xong; ha clock lam MOI lan gui cham gap doi, tuc la MOI thao tac
// tren HMI (khong rieng gi luc tu "lam lanh") deu keo dai thoi gian "dung
// hinh" do. Doi lai trong khi van bao mai khong het loi vo/soc (nguoi dung
// xac nhan "van bi vo khi chuyen man") - cai gia (lag lien tuc) khong dang
// so voi loi ich chua ro rang. Chong nhieu gio dua chinh vao 2 co che tu
// phuc hoi ben duoi (health-check + tu lam moi dinh ky) thay vi ha toc do.
constexpr uint32_t I2C_CLOCK_HZ = 100000UL;
constexpr uint16_t I2C_TIMEOUT_MS = 25;          // timeout phan cung moi giao dich
constexpr uint16_t I2C_STORAGE_LOCK_TIMEOUT_MS = 120; // doi LCD full-buffer toi da co gioi han
constexpr uint8_t DEFAULT_CONTRAST = 230;
constexpr bool REVERSE_ENCODER = false;
constexpr uint32_t LCD_RETRY_INTERVAL_MS = 3000UL;
// Rut tu 5000 xuong 2000ms - phat hien LCD "chet"/mat ACK nhanh hon, giam
// thoi gian man hinh dung hinh/sai ma khong ai biet truoc khi tu phuc hoi.
// Chi la 1 giao dich tham do 0-byte (vai chuc micro giay), khong dang ke
// den do lag nhu sendBuffer() day du nen giu nguyen, khong lien quan gioi
// han o tren.
constexpr uint32_t LCD_HEALTH_CHECK_MS = 2000UL;
constexpr uint32_t LCD_FAULT_LOG_INTERVAL_MS = 30000UL;
// Reinitialize only after real I2C failure/recovery, never on a healthy timer.
#ifndef LCD_PROFILE
#define LCD_PROFILE 1
#endif
constexpr uint8_t ENCODER_STEPS_PER_DETENT = 4;
constexpr uint8_t ENCODER_MAX_STEPS_PER_UPDATE = 3;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 30UL;
constexpr uint32_t BUTTON_RELEASE_DEBOUNCE_MS = 70UL;
constexpr uint32_t BUTTON_LONG_PRESS_MS = 700UL;
constexpr uint32_t DISPLAY_MIN_DRAW_MS = 110UL;
constexpr uint32_t HOME_REFRESH_MS = 5000UL;
// Man hinh khoi dong (splash): hien toi thieu ngan nay roi moi vao man chinh,
// va chi vao khi da nhan du du lieu that (runtime + config) de nguoi dung
// khong bao gio thay man chinh voi cac o con trong/gia tri mac dinh.
constexpr uint32_t SPLASH_MIN_MS = 1500UL;
// Chan tren: du chua nhan duoc du lieu (cam bien/EEPROM loi) cung khong ket o
// man khoi dong mai - sau moc nay luon vao man chinh de con thao tac duoc.
constexpr uint32_t SPLASH_MAX_MS = 6000UL;
constexpr uint32_t ALARM_REFRESH_MS = 5000UL;
// Man hinh "Dang cap nhat firmware..." can lam moi nhanh hon nhieu de thanh
// % chay muot, khac han HOME_REFRESH_MS (5s qua cham cho viec nay).
constexpr uint32_t FIRMWARE_PROGRESS_REFRESH_MS = 400UL;
constexpr uint32_t HMI_COMMAND_POLL_MS = 100UL;
constexpr uint32_t MENU_IDLE_TIMEOUT_MS = 60000UL;
// Rieng man DOI WIFI can du 2 phut de nguoi dung ket noi AP va nhap mat khau.
constexpr uint32_t WIFI_PORTAL_UI_IDLE_TIMEOUT_MS = 120000UL;
constexpr uint32_t SAVE_CONFIRM_TIMEOUT_MS = 8000UL;
constexpr uint32_t COMMAND_CONFIRM_TIMEOUT_MS = 8000UL;
constexpr uint16_t COMMAND_DEFAULT_VALID_MS = 5000U;
constexpr uint16_t COMMAND_AUTOTUNE_VALID_MS = 5000U;
// Rut ngan hon truoc (2200/4200ms) - man HMI nho, de bi che noi dung neu
// thanh thong bao hien qua lau o cuoi man.
constexpr uint32_t TOAST_INFO_MS = 1400UL;
constexpr uint32_t TOAST_ERROR_MS = 2500UL;
// Thong bao HMI chi chiem thanh trang thai 9 px o day, khong che noi dung.
constexpr uint8_t HMI_STATUS_TEXT_MAX_CHARS = 22U;
constexpr uint32_t HMI_INPUT_GUARD_MS = 140UL;
// Coi dao: hai bip ngan moi chu ky, khong keu lien tuc.
constexpr uint32_t TURN_BUZZER_ON1_MS = 90UL;
constexpr uint32_t TURN_BUZZER_GAP_MS = 120UL;
constexpr uint32_t TURN_BUZZER_ON2_MS = 90UL;
constexpr uint32_t TURN_BUZZER_PAUSE_MS = 1700UL;
constexpr uint8_t COMMAND_QUEUE_SIZE = 4;
constexpr uint8_t COMMAND_ACK_QUEUE_SIZE = 4;
constexpr uint32_t EMERGENCY_RESOUND_MS = 60000UL;
constexpr uint32_t CRITICAL_RESOUND_MS = 300000UL;
// "Coi thong minh" rieng cho AlarmTempHigh (xem buzzerUpdate() trong hmi.h):
// sau khi ACK, kiem tra xu huong nhiet moi khoang nay - neu nhiet giam it
// nhat TEMP_HIGH_SMART_MUTE_EPSILON_C so voi lan kiem truoc thi coi la "dang
// giam", giu im lang; dung yen hoac tang thi keu lai ngay (khong doi het
// CRITICAL_RESOUND_MS). Epsilon 0.05C du lon de vuot nhieu cam bien thong
// thuong (~0.02-0.03C) nhung du nho de nhan ra xu huong that trong 20s.
constexpr uint32_t TEMP_HIGH_SMART_MUTE_SAMPLE_MS = 20000UL;
constexpr float TEMP_HIGH_SMART_MUTE_EPSILON_C = 0.05f;
// AUTO bi tat giua me: chi bao sau 2 phut, sau ACK neu van OFF thi keu lai 10 phut/lan.
constexpr uint32_t AUTO_LOST_ALARM_DELAY_MS = 120000UL;
constexpr uint32_t AUTO_LOST_RESOUND_MS = 600000UL;
// Nhac nguoi van hanh xac nhan phuc hoi me sau mat dien. Bat buoc keu,
// khong bi vo hieu boi tuy chon tat coi nhac nho.
constexpr uint32_t RESUME_PROMPT_ON_MS = 220UL;
constexpr uint32_t RESUME_PROMPT_OFF_MS = 780UL;

// --------------------- CAU HINH LOGIC DE CHINH -------------------------------
// Huong tim goc khi khay nam giua. true = trai; false = phai.
constexpr bool HOME_TO_LEFT = true;

// Quyen cho phep nhiet ngoai me nam trong MachineConfig va chinh tren LCD.
// Mac dinh false; du bat van bat buoc quat tuan hoan da chay on dinh.
// Cong tac nhiet la dieu kien bat buoc cua mot me. Khong cho phep tat
// trong ban thuong mai vi me co the van dem thoi gian nhung khong duoc gia nhiet.
constexpr bool REQUIRE_HEATER_ENABLE_TO_START = true;

// MANUAL chi trao quyen quat tuan hoan va dao cho cong tac cung.
// Tat quat trong MANUAL se khoa ca SSR va contactor tong nhiet.
constexpr bool MANUAL_FAN_CAN_DISABLE_HEATING = true;

// WDT/panic/software reset: tu phuc hoi me neu snapshot EEPROM hop le.
// POWERON/BROWNOUT: bat buoc hoi nguoi van hanh co tiep tuc me hay khong.
constexpr uint8_t RESET_STORM_LIMIT = 3U;
constexpr uint32_t RESET_STORM_STABLE_CLEAR_MS = 600000UL; // 10 phut chay on dinh xoa dem

// PowerManager (machine_control.h) dem SO LAN reset lien tiep co ly do la
// "automatic recovery" (SW/EXT/PANIC/WDT - xem resetReasonIsAutomaticRecovery())
// va bao "ABNORMAL RESET" (E210, web dich la "mat dien"/reset bat thuong)
// khi vuot RESET_STORM_LIMIT (chi 3 lan). Van de: esp_reset_reason() tra ve
// CUNG 1 gia tri ESP_RST_SW cho MOI truong hop goi ESP.restart() - khong the
// phan biet "that su crash-loop bat thuong" voi "nap firmware/quay lai ban
// cu THANH CONG, tu chu dong khoi dong lai" (ota_web_update.h/ota_rollback.h).
// Ket qua: nap OTA vai lan lien tiep trong <10 phut (rat binh
// thuong khi dang phat trien/thu nghiem) se bi tinh nham la "reset bat
// thuong" du moi lan deu la nap firmware thanh cong, khong lien quan gi den
// mat dien/loi he thong that.
//
// Fix: danh dau "day la lan khoi dong lai CO CHU DICH" NGAY TRUOC khi goi
// ESP.restart() o 3 noi tren, dung vung nho RTC_NOINIT (KHONG bi bootloader
// xoa khi khoi dong lai bang phan mem - dung dac tinh nay de phan biet voi
// mat dien that: mat dien/brownout thi RAM mat dien that su, gia tri nay se
// KHONG con hop le). PowerManager::begin() doc co nay dau tien - neu co, coi
// nhu khoi dong sach (khong tinh vao bo dem storm) bat ke ly do reset la gi.
RTC_NOINIT_ATTR uint32_t gMayapIntentionalRestartMagic;
constexpr uint32_t MAYAP_INTENTIONAL_RESTART_MAGIC = 0x4F544149UL;  // "IATO" doc nguoc

inline void mayapMarkIntentionalRestart() {
  gMayapIntentionalRestartMagic = MAYAP_INTENTIONAL_RESTART_MAGIC;
}
// Doc VA XOA trong 1 buoc (chi co y nghia dung 1 lan cho lan khoi dong ke
// tiep) - goi 1 lan duy nhat luc boot (PowerManager::begin()).
inline bool mayapConsumeIntentionalRestart() {
  const bool was = gMayapIntentionalRestartMagic == MAYAP_INTENTIONAL_RESTART_MAGIC;
  gMayapIntentionalRestartMagic = 0U;
  return was;
}

// Cac moc thoi gian an toan.
constexpr uint32_t INPUT_SCAN_MS = 5UL;
constexpr uint32_t INPUT_DEBOUNCE_MS = 30UL;
// Sau khi cong tac thiet yeu bat lai, doi on dinh truoc khi xoa E130/E131.
constexpr uint32_t ESSENTIAL_INPUT_RESTORE_CONFIRM_MS = 1000UL;
constexpr uint32_t LIMIT_DEBOUNCE_MS = 20UL;
constexpr uint32_t FAN_PRESTART_MS = 5000UL;
constexpr uint32_t HEAT_MASTER_PICKUP_MS = 500UL;
// Luc bat dau me, contactor nhiet tong dong TUC THI cung cong tac (bat
// buoc, xem normalMasterPermit trong machine_control.h). De tranh 2 relay
// (quat tuan hoan + contactor nhiet tong) dong dien CUNG 1 thoi diem (yeu
// cau rieng ve dien tu nguoi lap dat), chi tri hoan rieng ngo ra QUAT mot
// khoang ngan nay - KHONG anh huong toi thoi diem nhiet SSR thuc su bat
// dau (van doi FAN_PRESTART_MS nhu cu qua fanAllowsHeat).
constexpr uint32_t CIRC_FAN_BATCH_START_STAGGER_MS = 400UL;
constexpr uint32_t HEAT_MASTER_DROP_DELAY_MS = 120UL; // tat SSR truoc, roi nha contactor
// Bao ve contactor tong nhiet: sau khi nha output master, doi toi thieu truoc khi dong lai.
// Khong bao gio tri hoan thao tac OFF vi an toan.
constexpr uint32_t HEAT_MASTER_MIN_OFF_MS = 3000UL;
constexpr uint32_t HEAT_RESTART_LOCKOUT_MS = 30000UL;
// Thoi gian quat tuan hoan chay them sau khi het nhu cau nhiet (vd. vua ket
// thuc me luc dang gia nhiet) de tan het nhiet du tren thanh nhiet. 60 s
// truoc day khien nguoi dung tuong may bi "ket quat" khi kiem tra ngay sau
// khi bam KET THUC ME - rut xuong 10 s, van du tan nhiet du cho phan tu
// nhiet tro thuong, nhung ngan de khong gay hieu lam la loi.
constexpr uint32_t POST_COOL_MS = 10000UL;
constexpr uint32_t TURN_DIRECTION_DEADTIME_MS = 500UL;
constexpr uint32_t TURN_LIMIT_RELEASE_TIMEOUT_MS = 2500UL;
constexpr uint32_t TURN_INPUT_CONFLICT_MS = 500UL;
constexpr uint32_t SENSOR_RECOVERY_GOOD_SAMPLES = 3UL;
// Mau nhiet giam dot ngot co the lam PID tang cong suat sai. Mau tang cao
// van duoc chap nhan ngay de bao ve qua nhiet.
constexpr float SENSOR_MAX_DOWN_STEP_C = 1.5f;
constexpr float SENSOR_PLAUSIBILITY_MATCH_C = 0.30f;
constexpr uint8_t SENSOR_PLAUSIBILITY_CONFIRM_SAMPLES = 3U;
constexpr uint32_t SENSOR_STARTUP_GRACE_MS = 20000UL; // chua bao coi trong 20 s dau
// F-08 (audit truoc phat hanh v3.7.1): phat hien cam bien "dung hinh" - gia
// tri chap nhan khong doi qua SENSOR_FROZEN_EPSILON_C trong lien tuc
// SENSOR_FROZEN_TIMEOUT_MS thi coi la nghi ngo dong bang (epsilon rat nho de
// khong bao nham mot chu ky dieu nhiet that su on dinh - cam bien that co
// nhieu/troi nen it khi giu nguyen tuyet doi lau nhu vay).
constexpr float SENSOR_FROZEN_EPSILON_C = 0.01f;
constexpr uint32_t SENSOR_FROZEN_TIMEOUT_MS = 1200000UL; // 20 phut
constexpr uint32_t LOW_TEMP_STARTUP_GRACE_MS = 300000UL; // 5 phut
constexpr uint32_t LOW_TEMP_CONFIRM_MS = 300000UL;       // thap lien tuc 5 phut
constexpr uint32_t HIGH_TEMP_CONFIRM_MS = 1000UL;
constexpr float HIGH_TEMP_CLEAR_HYSTERESIS_C = 0.2f;
constexpr uint32_t HIGH_TEMP_CLEAR_CONFIRM_MS = 10000UL;
constexpr float EMERGENCY_CLEAR_HYSTERESIS_C = 0.3f;
constexpr uint32_t EMERGENCY_CLEAR_CONFIRM_MS = 30000UL;
// Tam tat coi khan cap khi ACK. Truoc la 5 phut - qua dai cho muc "khan cap"
// neu khong co ai o gan may luc do; rut xuong 1 phut, coi se tu keu lai neu
// dieu kien qua nhiet khan cap van con (khong lien quan viec cat nhiet - cat
// nhiet luon doc lap voi ACK, xem emergencyActive_/masterDropRequired()).
constexpr uint32_t SIREN_TEMPORARY_MUTE_MS = 60000UL;
constexpr uint32_t BATCH_CHECKPOINT_MS = 300000UL;
// Loi dao (TurnTimeout/TurnLimitStuck) lap lai lien tiep du da ACK, khong co
// lan dao thanh cong nao xen giua - nghi ngo hong co khi (cong tac hanh
// trinh) chu khong phai loi thoang qua. Vuot nguong nay thi khoa dao hoan
// toan (TurnMechanicalCheckRequired), bat buoc vao Test Mode xac nhan lai ca
// 2 cong tac hanh trinh (Trai va Phai deu bao Success) moi duoc mo khoa -
// khong con tu dong thu lai vo han qua ACK thuong.
constexpr uint8_t TURN_FAULT_STREAK_LIMIT = 3U;
// Man hinh cho xac nhan "Ap lai me cu" sau mat dien (resumeConfirmationRequired_)
// khong co timeout tu dong ap lai (dung y do an toan - phai co nguoi quyet
// dinh) nhung neu khong ai thao tac qua lau thi can canh bao rieng de nguoi
// dung/quan tri biet may dang treo cho, khong phai dang hoat dong binh thuong.
constexpr uint32_t RESUME_CONFIRM_ALERT_MS = 900000UL; // 15 phut
// F-06 (audit truoc phat hanh v3.7.1): me qua so ngay ap du kien
// (config.totalIncubationDays) ma khong ai dung/xac nhan "tiep tuc" thi TU
// DONG DUNG ME sau tung nay giay tinh tu dung ngay du kien (epoch, khong
// phai tinh tu luc phat hien - song sot qua reboot vi tinh truc tiep tu
// batchStartEpoch_ da luu). 12 gio du de nguoi van hanh kip thay thong bao/
// coi va quyet dinh, nhung khong de may giu am vo thoi han neu bi bo quen.
constexpr uint32_t BATCH_OVERDUE_AUTO_STOP_GRACE_SEC = 12UL * 3600UL;

// Tu kiem tra coi dinh ky (backlog, khong thuoc audit v3.7.1) - xem
// MachineConfig::sirenSelfTestEnabled va updateSirenSelfTest() trong
// machine_control.h. 7 ngay/lan la du dan cach de khong gay phien, PULSE_MS
// chi 1.5s - vua du nghe thay "coi con keu", khong keu dai nhu canh bao that.
constexpr uint32_t SIREN_SELF_TEST_INTERVAL_MS = 7UL * 24UL * 3600UL * 1000UL;
constexpr uint32_t SIREN_SELF_TEST_PULSE_MS = 1500UL;
// Khi cho phuc hoi me ma RTC khong hop le (ResumeBlockReason::Rtc) - khac voi
// man hinh xac nhan tren (co nguoi thao tac duoc), truong hop nay may KHONG
// TU LAM GI DUOC (dang cho RTC song lai qua auto-repair/NTP), nen can canh
// bao SOM hon nhieu de nguoi dung biet may dang treo cho vi ly do gi thay vi
// tuong dang hoat dong binh thuong ma khong ro sao chua ap tiep.
constexpr uint32_t RESUME_RTC_WAIT_ALERT_MS = 300000UL; // 5 phut
// Trend trong me: 5 phut/mau. Event quan trong duoc ghi ngay khi xay ra.
constexpr uint32_t BATCH_LOG_SAMPLE_MS = 300000UL;
constexpr size_t BATCH_LOG_MIN_FREE_BYTES = 256U * 1024U;
constexpr uint8_t BATCH_LOG_MAX_FILES = 8U;
// Mac dinh ngung dao trong 3 ngay cuoi truoc no. Ap dung theo tong so ngay me.
constexpr uint8_t TURN_LOCKDOWN_DAYS = 3U;
// Khi nguoi dung dung/huy me ma chua xoa duoc ban ghi EEPROM, may van dung
// output ngay va thu lai theo chu ky huu han. Khong cho bat dau/phuc hoi me moi.
constexpr uint32_t BATCH_CLEAR_RETRY_MS = 3000UL;

// NVS noi bo chi dung lam nhat ky an toan rat nho, KHONG luu cau hinh may.
// Muc dich: khong cho phuc hoi lai me cu neu nguoi dung da bam DUNG/HUY
// nhung EEPROM ngoai chua kip xac nhan wasRunning=0 truoc mot lan reset.
constexpr char SAFETY_NVS_NAMESPACE[] = "mayap_safe";
constexpr char SAFETY_NVS_STOP_KEY[] = "stop_intent";
constexpr char SAFETY_NVS_RESET_KEY[] = "reset_count";
// F-07 (audit truoc phat hanh v3.7.1): truoc day turnMechanicalCheckRequired_/
// turnFaultStreak_ (khoa dao vi nghi ngo hong co khi, xem latchTurnFault()
// trong machine_control.h) chi la bien RAM - mat dien/watchdog/OTA restart
// se am tham go khoa nay, may lai tu dao binh thuong ma chua ai kiem tra
// thuc te, phai gap lai du 3 loi lien tiep moi khoa lai (driving mot co cau
// da nghi ngo hong them 3 lan nua). Luu ca 2 gia tri nay vao NVS giong cach
// stop_intent da lam, de song sot qua reboot.
constexpr char SAFETY_NVS_TURN_CHECK_KEY[] = "turn_check";
constexpr char SAFETY_NVS_TURN_STREAK_KEY[] = "turn_streak";
// F-06: nguoi van hanh da xac nhan "tiep tuc u am" cho me QUA HAN hien tai -
// luu NVS de song sot qua reboot (khong thi sau reboot may lai hoi lai/bat
// coi dung ngay khi con dang trong 12h an han, du nguoi dung da tra loi
// truoc do). Duoc startBatch()/stopBatch() xoa cho me tiep theo.
constexpr char SAFETY_NVS_OVERDUE_KEY[] = "batch_overdue";
constexpr uint32_t RUNTIME_TO_HMI_MS = 200UL;
constexpr uint32_t DIAGNOSTIC_STATUS_MS = 10000UL;
// Test/bench builds (workflow_dispatch PILOT) start with the debug log already ON so a flashed board
// prints without typing LOG; production and PR builds keep it OFF until the LOG command.
#ifndef MAYAP_SERIAL_DEBUG_DEFAULT_ON
#define MAYAP_SERIAL_DEBUG_DEFAULT_ON 0
#endif
constexpr bool SERIAL_DEBUG_DEFAULT_ON = MAYAP_SERIAL_DEBUG_DEFAULT_ON != 0;
constexpr uint32_t CONTROL_TASK_PERIOD_MS = 5UL;
constexpr uint32_t HMI_TASK_PERIOD_MS = 5UL;
constexpr uint32_t SUPERVISOR_TASK_PERIOD_MS = 50UL;
constexpr uint32_t NETWORK_TASK_PERIOD_MS = 250UL;
// Cap nhat firmware tu xa (kiem tra ban moi, tai ve + nap, rollback) chay tren task RIENG, khong
// chung networkTask va khong chung cloudTask: nap la HTTPS blocking toi 120 s, khong duoc lam tre
// alarm/heartbeat hay Wi-Fi. (ArduinoOTA da go bo; con lai chi la cac phien HTTPS tuan tu.)
constexpr uint32_t OTA_TASK_PERIOD_MS = 30UL;
// ESP-IDF tinh stack task theo byte. Tat ca buffer cap phat tinh, khong phan manh heap.
constexpr size_t CONTROL_TASK_STACK_BYTES = 8192U;
constexpr size_t HMI_TASK_STACK_BYTES = 10240U;
constexpr size_t SUPERVISOR_TASK_STACK_BYTES = 4096U;
// 12288 giu nguyen sau khi go ArduinoOTA: task van chay WiFiClientSecure+HTTPClient (TLS toi Cloudflare)
// + buffer 1 kB doc firmware + mbedtls_sha256_context. Dinh stack luc TAI THAT chua duoc do tren
// mach (log [TASK] chi moi ghi 5,8 kB luc nhan roi); do truoc khi thu nho hoac gop vao task khac.
constexpr size_t OTA_TASK_STACK_BYTES = 12288U;
// Bench measurement (PILOT log, 60 s [TASK] report, Wi-Fi + NTP + MQTT + Cloud running): network task used
// 2.5 kB of 12 kB at its peak. Stacks are static RAM, so every kB removed here is a kB of heap for TLS.
// 8 kB keeps >3x the measured peak; the Wi-Fi portal (WebServer/DNS/scan) path was NOT in that log, so it
// must be exercised during the soak and `net=` (bytes never used) must stay >= 2048 before going lower.
constexpr size_t NETWORK_TASK_STACK_BYTES = 8192U;
constexpr uint32_t TASK_STACK_MONITOR_MS = 60000UL;
// 5 s: du bien cho giao dich I2C huu han nhung van phat hien task bi treo.
constexpr uint32_t CONTROL_WDT_TIMEOUT_MS = 5000UL;
constexpr uint32_t CONTROL_HEARTBEAT_TIMEOUT_MS = 500UL;
// Nguong trip da tinh den giao dich EEPROM dong bo hiem khi xay ra.
constexpr uint32_t CONTROL_CYCLE_TRIP_US = 400000UL;
constexpr uint8_t CONTROL_CYCLE_TRIP_COUNT = 3U;
constexpr uint32_t HMI_HEARTBEAT_TIMEOUT_MS = 2000UL;
// HMI cham thoang qua chi canh bao; neu treo that su lau hon 8 s hoac
// lien tuc co chu ky >1.2 s thi supervisor khoi dong lai CO KIEM SOAT:
// latch output an toan -> suspend control/HMI -> safe outputs -> restart.
constexpr uint32_t HMI_FATAL_HEARTBEAT_TIMEOUT_MS = 8000UL;
constexpr uint32_t HMI_CYCLE_TRIP_US = 1200000UL;
constexpr uint8_t HMI_CYCLE_TRIP_COUNT = 3U;
constexpr uint32_t SUPERVISOR_RESTART_FALLBACK_MS = 7000UL; // TWDT 5 s duoc uu tien; day la fallback

// ------------------------- Exponential backoff (dung chung) -------------------
// Dung cho MOI vong reconnect/retry co the gap loi keo dai (Wi-Fi STA, MQTT,
// Cloud Push): khong bao gio thu lai theo chu ky co dinh vinh vien - se tu keo
// gian ra 1s -> 2s -> 4s -> 8s -> 16s -> 30s -> 60s (giu nguyen 60s ve sau,
// KHONG bao gio bo cuoc han - thiet bi khong nguoi truc phai tu ket noi lai
// duoc sau vai gio/vai ngay mat mang, khong can bam nut/khoi dong lai). 60s la
// tran an toan: du gan de nguoi dung khong cho qua lau khi mang vua co lai,
// du xa de khong "spam" CPU/song/API khi mang mat that su dai (hang gio/hang
// ngay). Bang gia tri CO DINH (khong phai cong thuc luy thua thuan tuy) vi
// 16->30->60 la buoc nhay thuc te pho bien hon 16->32->64, de kiem chung dung
// tung buoc bang mat thay vi tin cong thuc.
constexpr uint32_t BACKOFF_STEPS_MS[] = {
    1000UL, 2000UL, 4000UL, 8000UL, 16000UL, 30000UL, 60000UL};
constexpr uint8_t BACKOFF_STEP_COUNT =
    sizeof(BACKOFF_STEPS_MS) / sizeof(BACKOFF_STEPS_MS[0]);
// Jitter ngau nhien them vao MOI lan cho (0..JITTER_MAX), tranh nhieu kenh
// (Wi-Fi/MQTT/Cloud Push) hoac nhieu thiet bi cung dong loat thu lai dung 1
// thoi diem (thundering herd) sau khi mang/broker vua phuc hoi.
constexpr uint32_t BACKOFF_JITTER_MAX_MS = 500UL;

// Bo dem lui-va-doi don gian, KHONG blocking (chi so sanh moc thoi gian).
// Moi kenh retry (Wi-Fi/MQTT/Cloud Push) giu MOT instance rieng - hoan toan
// doc lap, loi/reset o kenh nay khong dung cham gi den buoc backoff cua kenh
// khac. Khong dung heap, khong tao task/timer rieng - an toan tai nguyen.
struct BackoffTimer {
  uint8_t step = 0U;
  uint32_t nextAttemptAt = 0U;

  // Cho phep thu ngay lan dau tien (vd sau boot hoac sau khi nguoi dung vua
  // bat lai ONLINE) - khong bat nguoi dung/thiet bi cho oan mot chu ky day du.
  void reset(uint32_t now) {
    step = 0U;
    nextAttemptAt = now;
  }

  bool ready(uint32_t now) const {
    return static_cast<int32_t>(now - nextAttemptAt) >= 0;
  }

  // Goi dung 1 LAN cho moi lan thu THAT BAI. Tang buoc (toi da dung o muc
  // tran BACKOFF_STEP_COUNT-1, KHONG tang vo han - tranh tran uint8_t va giu
  // do tre luon <= 60s+jitter, khong bao gio "bo cuoc" han).
  void onFailure(uint32_t now) {
    const uint32_t delay = BACKOFF_STEPS_MS[step];
    if (step + 1U < BACKOFF_STEP_COUNT) ++step;
    const uint32_t jitter = esp_random() % (BACKOFF_JITTER_MAX_MS + 1U);
    nextAttemptAt = now + delay + jitter;
  }

  // Goi khi ket noi/gui THANH CONG: dua buoc ve 0 de lan loi tiep theo (neu
  // co) lai bat dau tu do tre ngan nhat, dung yeu cau "reset retry counter".
  void onSuccess() { step = 0U; }
};

// Wi-Fi chi chay o task rieng core 0; khong duoc goi tu task dieu khien.
constexpr uint32_t NETWORK_CONNECT_TIMEOUT_MS = 20000UL;
// Cong 1 doi Wi-Fi: mo AP toi da 2 phut cho nguoi dung nhap SSID/mat khau moi,
// sau do tu dong dong portal va quay lai ket noi binh thuong.
constexpr uint32_t WIFI_PORTAL_MAX_OPEN_MS = 120000UL;
constexpr uint32_t WIFI_PORTAL_TEST_TIMEOUT_MS = 15000UL;
constexpr uint16_t WIFI_PORTAL_SSID_MAX = 32U;
constexpr uint16_t WIFI_PORTAL_PASSWORD_MAX = 64U;

// ----------------------- CHE DO TEST -----------------------------------
// Bat ngo ra lien tuc cho toi khi nguoi lap dat tra loi CO/KHONG - tranh
// dong/cat lap lai gay soc thiet bi. Gioi han an toan toi da phong khi
// quen khong tra loi (vd rot khoi man hinh do mat nguon/loi).
constexpr uint32_t TEST_OUTPUT_HOLD_MAX_MS = 20000UL;
// Test SSR nhiet la quy trinh rieng: quat tuan hoan chay truoc 5 s,
// sau do thanh nhiet duoc phep ON toi da 3 phut. Day la tran CUNG;
// nut HMI co the dung som hon nhung van phai qua 2 man xac nhan.
constexpr uint32_t TEST_HEATER_FAN_PRESTART_MS = 5000UL;
constexpr uint32_t TEST_HEATER_HOLD_MAX_MS = 180000UL;
// Quat hut trong buoc xac nhan sau test nhiet: cho nguoi lap dat du
// thoi gian quan sat, nhung khong the bi bo quen ON vo han.
constexpr uint32_t TEST_VENT_CONFIRM_HOLD_MAX_MS = 60000UL;
constexpr uint32_t TEST_LIMIT_TIMEOUT_MS = 20000UL;
constexpr uint32_t TEST_LIMIT_CONFIRM_BUZZ_MS = 2000UL;
// Roi trang Che do thu nghiem qua lau ma khong thao tac: tu dong thoat de
// khong bo quen may o trang thai cho lenh tay.
constexpr uint32_t TEST_MODE_IDLE_EXIT_MS = 120000UL;

// ----------------------- KERNEL / EVENT -------------------------------------
constexpr uint8_t INPUT_EVENT_QUEUE_SIZE = 16U;
constexpr uint8_t OUTPUT_EVENT_QUEUE_SIZE = 16U;
constexpr uint8_t EVENT_LOG_RAM_SIZE = 64U;
// HMI hien cac su kien RAM moi nhat. Flash log tam tat trong ban Recovery.
constexpr uint8_t HMI_EVENT_DISPLAY_CAPACITY = 24U;
constexpr uint8_t HMI_FAULT_DISPLAY_CAPACITY = 12U;
constexpr uint32_t RELAY_GENERAL_MIN_SWITCH_MS = 250UL;
constexpr uint32_t RELAY_FAN_MIN_ON_MS = 2000UL;
constexpr uint32_t RELAY_LIGHT_MIN_SWITCH_MS = 150UL;
constexpr uint32_t DIAGNOSTIC_FAST_STATUS_MS = 1000UL;
constexpr uint16_t MAX_RELAY_TRANSITIONS_PER_HOUR = 1800U;

// Legacy SSR-window limits retained for regression/config compatibility.
// Production candidate uses 300 ms pulse-density quanta (scheduler minimum 300 ms).
// The old 10 s SSR window is retained only in legacy/regression compatibility.
constexpr uint32_t SSR_MIN_ON_MS = 300UL;
constexpr uint32_t SSR_MIN_OFF_MS = 300UL;

// Auto Tune relay co gioi han, khong chay khi dang co me.
// [DA CHUYEN SANG MachineConfig, schema 8] Bien do relay % va dai xac nhan
// °C tung la hang so cung (AUTOTUNE_RELAY_POWER_PERCENT/AUTOTUNE_BAND_C) - da
// chuyen thanh MachineConfig::autotuneRelayPowerPercent/autotuneBandC de
// nguoi dung tu chinh "nhe tay" hon khi Auto Tune tren dan nhiet cong suat
// lon ma khong can nap lai firmware. Gia tri mac dinh giu nguyen y het.
constexpr uint8_t AUTOTUNE_REQUIRED_CYCLES = 3;
constexpr float AUTOTUNE_STABILITY_FRACTION = 0.20f;
constexpr float PID_D_FILTER_TAU_SEC = 5.0f;
// Bounded commissioning candidates; no power escalation or timeout extension.
constexpr uint8_t AUTOTUNE_PREHEAT_POWER_PERCENT = 30U; // Provisional 4.8 kW average, capped by maxHeaterPower
constexpr uint32_t AUTOTUNE_PREHEAT_MAX_MS = 900000UL; // 15 min separate preheat limit
constexpr uint32_t AUTOTUNE_TOTAL_MAX_MS = 2700000UL; // 45 min INCLUDING preheat
constexpr uint32_t AUTOTUNE_MAX_MS = AUTOTUNE_TOTAL_MAX_MS; // legacy host comparison alias
constexpr uint32_t AUTOTUNE_PHASE_MAX_MS = 900000UL; // moi pha toi da 15 phut
constexpr uint32_t AUTOTUNE_MIN_PERIOD_MS = 10000UL;
constexpr float AUTOTUNE_MIN_AMPLITUDE_C = 0.10f;

// LED RGB - do sang thap de khong nong/khong choi trong tu dien.
constexpr uint8_t RGB_BRIGHTNESS_LOW = 6;
constexpr uint8_t RGB_BRIGHTNESS_NORMAL = 14;
constexpr uint8_t RGB_BRIGHTNESS_ALARM = 30;

// Khong gia mao ngay thuc. Chi bat neu chap nhan hien ngay bien dich.
constexpr bool DISPLAY_BUILD_DATE_WHEN_RTC_MISSING = false;

// ----------------------- DS3231 + AT24C512 ----------------------------------
// Dia chi co dinh de code gon va xac dinh. Hay quet I2C mot lan roi sua
// Mot EEPROM AT24C512 (64 KiB), A0/A1/A2 = GND, WP = GND de cho phep ghi.
constexpr bool EXTERNAL_EEPROM_ENABLED = true;
constexpr bool EXTERNAL_EEPROM_REQUIRED = true;
constexpr uint8_t RTC_I2C_ADDRESS = 0x68U;
constexpr uint8_t EEPROM_I2C_ADDRESS = 0x50U;
constexpr uint32_t EEPROM_CAPACITY_BYTES = 65536UL;
constexpr uint8_t EEPROM_PAGE_SIZE = 128U;
// Wire TX = 128 byte, tru 2 byte dia chi o nho. Khong gui ca page 128B.
constexpr uint8_t EEPROM_MAX_WRITE_CHUNK = 126U;
constexpr uint16_t EEPROM_WRITE_TIMEOUT_MS = 20U;
constexpr uint8_t EEPROM_IO_RETRIES = 2U;
constexpr uint16_t EEPROM_RETRY_GAP_MS = 2U;
// Mot lan luu loi chi bao suy giam; chi khoa cung sau nhieu lan loi lien tiep.
constexpr uint8_t EEPROM_FAILURE_LATCH_COUNT = 3U;
constexpr uint32_t RTC_READ_PERIOD_MS = 1000UL;
constexpr uint32_t RTC_STUCK_TIMEOUT_MS = 4000UL;
constexpr uint16_t RTC_VALID_YEAR_MIN = 2024U;
constexpr uint16_t RTC_VALID_YEAR_MAX = 2099U;
// Tu phuc hoi khi thao module DS3231 ra/lap lai trong luc ESP32 van con nguon.
// Firmware giu mot dong ho bong trong RAM tu moc RTC hop le gan nhat.
constexpr bool RTC_AUTO_REPAIR_ENABLED = true;
constexpr uint8_t RTC_AUTO_REPAIR_CONFIRM_READS = 2U;
constexpr uint8_t RTC_AUTO_REPAIR_MAX_ATTEMPTS = 3U;
constexpr uint32_t RTC_AUTO_REPAIR_RETRY_MS = 3000UL;
constexpr uint32_t RTC_AUTO_REPAIR_MAX_GAP_SEC = 24UL * 3600UL;
// Thu ket noi lai EEPROM dung dia chi co dinh; khong quet bus.
constexpr uint32_t EEPROM_RECONNECT_PERIOD_MS = 3000UL;
constexpr uint32_t EEPROM_HEALTH_CHECK_MS = 5000UL;
constexpr uint8_t EEPROM_RECOVERY_VERIFY_COUNT = 2U;
constexpr uint32_t MAX_RTC_RECOVERY_GAP_SEC = 45UL * 86400UL;

// --------------------------- Dong bo gio qua NTP -----------------------------
// RTC_AUTO_REPAIR_* o tren chi phuc hoi duoc khi ESP32 VAN CON NGUON (dong ho
// bong trong RAM con song) va chua het RTC_AUTO_REPAIR_MAX_ATTEMPTS lan thu -
// khong giup gi khi module DS3231 hong that (thach anh/pin CR2032 chet) hoac
// ca ESP32 cung mat dien (dong ho bong mat theo): luc do RTC dung im vinh vien
// cho den khi co nguoi cam tay set lai (lenh serial "TIME SET" hoac tuong tu).
// Lop nay them nguon thoi gian THU HAI doc lap qua NTP khi co Wi-Fi - dong bo
// dinh ky (khong doi loi moi lam) de vua tu phuc hoi khoi tinh huong tren, vua
// sua troi dat tich luy nho cua thach anh DS3231 (~vai giay/thang) giua cac lan.
constexpr bool NTP_SYNC_ENABLED = true;
constexpr char NTP_SERVER_PRIMARY[] = "pool.ntp.org";
constexpr char NTP_SERVER_SECONDARY[] = "time.google.com";
// Viet Nam UTC+7, khong doi gio mua he. RTC va toan bo firmware luu GIO DIA
// PHUONG truc tiep (giong het lenh serial "TIME SET"/HMI dang set thu cong),
// NTP tra ve UTC nen phai cong offset nay khi doc getLocalTime().
constexpr int32_t NTP_TIMEZONE_OFFSET_SEC = 7L * 3600L;
// Chu ky dong bo dinh ky - theo de xuat nguoi dung (12h/lan): du nhanh de troi
// dat thach anh khong tich luy dang ke, du thua de khong lam phien may chu NTP.
constexpr uint32_t NTP_SYNC_INTERVAL_MS = 12UL * 3600UL * 1000UL;
constexpr uint32_t NTP_REQUEST_TIMEOUT_MS = 5000UL;


// AT24C512 layout: configuration, batch and history addresses are unchanged.
// Unused legacy ranges remain reserved; no erase or migration is performed.
constexpr uint16_t EEPROM_ADDR_CONFIG_A = 0x0000U;
constexpr uint16_t EEPROM_ADDR_CONFIG_B = 0x0100U;
constexpr uint16_t EEPROM_ADDR_BATCH_A  = 0x0200U;
constexpr uint16_t EEPROM_ADDR_BATCH_B  = 0x0280U;
constexpr uint16_t EEPROM_ADDR_RESERVED_A = 0x0300U;
constexpr uint16_t EEPROM_ADDR_RESERVED_B = 0x0700U;
constexpr uint16_t EEPROM_CONFIG_SLOT_BYTES = 0x0100U;
constexpr uint16_t EEPROM_BATCH_SLOT_BYTES = 0x0080U;
constexpr uint16_t EEPROM_RESERVED_SLOT_BYTES = 0x0400U;

// AT24C512: keep existing configuration/batch addresses and reserved ranges.
// 0x0B00..0x0FFF de nguyen (history cu); history moi 0x1000..0x2F7F.
// 7 ngay, 5 phut/mau; 0x2F80..0xFFFF du phong, KHONG format khi boot.
constexpr uint16_t EEPROM_ADDR_TEMP_HISTORY = 0x1000U;
constexpr uint16_t TEMP_HISTORY_SAMPLE_SEC = 300U;
constexpr uint16_t TEMP_HISTORY_SLOT_COUNT = 2016U;
constexpr uint16_t TEMP_HISTORY_RECORD_BYTES = 4U;
constexpr uint16_t TEMP_HISTORY_STORAGE_BYTES =
    TEMP_HISTORY_SLOT_COUNT * TEMP_HISTORY_RECORD_BYTES;
static_assert(TEMP_HISTORY_STORAGE_BYTES == 8064U,
              "History 7 ngay/5phut phai dung 8064 byte");
static_assert(TEMP_HISTORY_SLOT_COUNT < 4096U, "History vuot tag bucket 12-bit");

static_assert(EEPROM_I2C_ADDRESS >= 0x50U && EEPROM_I2C_ADDRESS <= 0x57U,
              "Dia chi AT24C512 phai nam trong 0x50..0x57");
static_assert(EEPROM_PAGE_SIZE == 128U, "AT24C512 page phai 128 byte");
static_assert(EEPROM_MAX_WRITE_CHUNK + 2U <= 128U, "Vuot Wire TX buffer");
static_assert(EEPROM_IO_RETRIES > 0U, "EEPROM_IO_RETRIES phai > 0");
static_assert(EEPROM_FAILURE_LATCH_COUNT > 0U,
              "EEPROM_FAILURE_LATCH_COUNT phai > 0");
static_assert(sizeof(SAFETY_NVS_NAMESPACE) <= 16U,
              "NVS namespace toi da 15 ky tu");
static_assert(sizeof(SAFETY_NVS_STOP_KEY) <= 16U,
              "NVS key toi da 15 ky tu");
static_assert(sizeof(SAFETY_NVS_RESET_KEY) <= 16U,
              "NVS key toi da 15 ky tu");
static_assert(sizeof(SAFETY_NVS_TURN_CHECK_KEY) <= 16U,
              "NVS key toi da 15 ky tu");
static_assert(sizeof(SAFETY_NVS_TURN_STREAK_KEY) <= 16U,
              "NVS key toi da 15 ky tu");
static_assert(sizeof(SAFETY_NVS_OVERDUE_KEY) <= 16U,
              "NVS key toi da 15 ky tu");
static_assert(REQUIRE_HEATER_ENABLE_TO_START,
              "Ban thuong mai bat buoc cong tac nhiet ON khi bat dau me");
static_assert(RTC_AUTO_REPAIR_CONFIRM_READS > 0U,
              "RTC_AUTO_REPAIR_CONFIRM_READS phai > 0");
static_assert(RTC_AUTO_REPAIR_MAX_ATTEMPTS > 0U,
              "RTC_AUTO_REPAIR_MAX_ATTEMPTS phai > 0");
static_assert(RTC_AUTO_REPAIR_MAX_GAP_SEC < 0x7FFFFFFFUL / 1000UL,
              "RTC auto repair gap qua lon cho millis rollover");
static_assert(EEPROM_RECOVERY_VERIFY_COUNT > 0U,
              "EEPROM_RECOVERY_VERIFY_COUNT phai > 0");
static_assert(EEPROM_ADDR_CONFIG_A + EEPROM_CONFIG_SLOT_BYTES <= EEPROM_ADDR_CONFIG_B, "Config A de len B");
static_assert(EEPROM_ADDR_CONFIG_B + EEPROM_CONFIG_SLOT_BYTES <= EEPROM_ADDR_BATCH_A, "Config B de len Batch A");
static_assert(EEPROM_ADDR_BATCH_A + EEPROM_BATCH_SLOT_BYTES <= EEPROM_ADDR_BATCH_B, "Batch A de len B");
static_assert(EEPROM_ADDR_BATCH_B + EEPROM_BATCH_SLOT_BYTES <= EEPROM_ADDR_RESERVED_A,
              "Batch B de len Reserved A");
static_assert(EEPROM_ADDR_RESERVED_A + EEPROM_RESERVED_SLOT_BYTES <= EEPROM_ADDR_RESERVED_B,
              "Reserved A de len B");
static_assert(EEPROM_ADDR_RESERVED_B + EEPROM_RESERVED_SLOT_BYTES <= EEPROM_ADDR_TEMP_HISTORY,
              "Reserved B de len History");
static_assert(EEPROM_ADDR_TEMP_HISTORY + TEMP_HISTORY_STORAGE_BYTES <= EEPROM_CAPACITY_BYTES,
              "History vuot dung luong AT24C512");
static_assert(EEPROM_PAGE_SIZE % TEMP_HISTORY_RECORD_BYTES == 0U,
              "History record phai vua page");

// -------------------- RANG BUOC HMI/AN TOAN ---------------------------------
constexpr float TARGET_TEMP_MIN_C = 30.0f;
constexpr float TARGET_TEMP_MAX_C = 40.0f;
constexpr float LOW_ALARM_GAP_C = 0.1f;
constexpr float HIGH_ALARM_GAP_C = 0.1f;
constexpr float EMERGENCY_ABOVE_HIGH_C = 0.1f;
constexpr float VENT_OFF_ABOVE_SV_C = 0.0f;
constexpr float VENT_ON_ABOVE_SV_C = 0.1f;
constexpr float VENT_HYSTERESIS_C = 0.1f;
// Chuoi rang buoc: SV <= HUT OFF < HUT ON <= BAO CAO < KHAN CAP.
// VENT_HYSTERESIS_C la khoang cach toi thieu giua nguong tat va bat quat hut.
constexpr float HIGH_ALARM_MAX_C = 42.0f;
constexpr float EMERGENCY_MAX_C = 45.0f;

// ============================================================================
// HOP DONG DU LIEU HMI <-> FIRMWARE
// ============================================================================
enum class ControlMode : uint8_t { OnOff = 0, Pid = 1 };
enum class TurnDirection : uint8_t { Left = 0, Right = 1 };
enum class ConnectivityMode : uint8_t { Offline = 0, Online = 1 };
enum class NetworkStateCode : uint8_t {
  Offline = 0, NotConfigured = 1, Connecting = 2, Connected = 3
};
enum class TurnState : uint8_t {
  Stopped = 0, Left = 1, Right = 2, Waiting = 3, Fault = 4
};
enum class AutoTuneState : uint8_t {
  Idle = 0, Running = 1, Success = 2, Failed = 3
};

enum class MachineStateCode : uint8_t {
  Boot = 0, ReadyAuto, ReadyManual, ResumeWait, Prestart, Homing,
  RunningAuto, RunningManual, AutoTune,
  SensorFault, TurningFault, SystemFault, Emergency
};

enum AlarmBit : uint32_t {
  AlarmNone        = 0,
  AlarmSensor      = 1UL << 0,
  AlarmTempLow     = 1UL << 1,
  AlarmTempHigh    = 1UL << 2,
  AlarmEmergency   = 1UL << 3,
  AlarmHumidityLow = 1UL << 4,
  AlarmTurning     = 1UL << 5,
  AlarmAutoMode    = 1UL << 6,
  AlarmSystem      = 1UL << 7
};
constexpr uint32_t ALARM_KNOWN_MASK =
    AlarmSensor | AlarmTempLow | AlarmTempHigh | AlarmEmergency |
    AlarmHumidityLow | AlarmTurning | AlarmAutoMode | AlarmSystem;

struct MachineConfig {
  float targetTemp = 37.5f;
  float tempHysteresis = 0.2f;
  float lowTempAlarm = 36.5f;
  float highTempAlarm = 38.2f;
  float emergencyTemp = 39.0f;
  ControlMode controlMode = ControlMode::Pid;
  float kp = 18.0f;
  float ki = 0.8f;
  float kd = 45.0f;
  // Legacy reserved EEPROM/protocol field; runtime uses HEATER_BURST_QUANTUM_MS.
  uint16_t pidCycleSec = 10;
  uint8_t maxHeaterPower = 100;
  bool adaptiveThermalBalanceEnabled = false; // opt-in; old schema defaults OFF

  // Nang cao (schema 8): nguong chan doan nhiet + tham so Auto Tune, truoc
  // day la hang so cung trong config.h (xem ghi chu cu o khu "HANG SO AN
  // TOAN CO DINH"). Dua vao MachineConfig de nguoi dung chinh truc tiep qua
  // HMI/web khi lap may that ma khong can nap lai firmware - gia tri mac
  // dinh giu nguyen y het hang so cu.
  float heaterStuckMinRiseC = 0.3f;
  uint16_t heaterStuckDurationSec = 900;   // 15 phut
  float tempRateLimitC = 1.0f;
  uint16_t tempRateWindowSec = 120;        // 2 phut
  uint8_t tempOscillationCrossLimit = 6;
  uint16_t tempOscillationWindowSec = 600; // 10 phut
  uint8_t autotuneRelayPowerPercent = 30;
  float autotuneBandC = 0.20f;

  float lowHumidityAlarm = 45.0f;
  uint16_t humidityAlarmDelaySec = 60;
  // Phan cung tao am la tuy chon theo tung may. Co/Khong CHI duoc cau hinh
  // tren HMI; web chi doc co nay de an/hien giao dien, khong duoc thay doi.
  bool humidifierInstalled = false;
  bool humidifierEnabled = false;
  float targetHumidity = 58.0f;
  uint8_t humidifierHysteresisRh = static_cast<uint8_t>(HUMIDIFIER_HYSTERESIS_RH);

  bool circulationFanEnabled = true;
  float ventOnTemp = 38.0f;
  float ventOffTemp = 37.6f;
  // Thong gio dinh ky theo RTC. Nut BAT/TAT nam o nhom NHIET DO; chi tiet
  // lich nam o NANG CAO. Mac dinh TAT de khong thay doi hanh vi may cu.
  bool ventScheduleEnabled = false;
  uint8_t ventScheduleCount = 2;
  uint8_t ventScheduleDurationMin = 5;
  uint8_t ventScheduleHour1 = 8;
  uint8_t ventScheduleHour2 = 20;
  uint8_t ventScheduleHour3 = 12;
  uint8_t ventScheduleHour4 = 16;
  uint8_t ventScheduleHour5 = 0;
  uint8_t ventScheduleHour6 = 4;
  // Profile thong gio theo ngay ap. Mac dinh TAT khi nang cap may cu; nguoi
  // van hanh bat trong menu Quat hut. Lich gio cu duoc giu de migrate.
  bool ventAutoEnabled = false;
  uint8_t ventProfileLevel = 1;  // 0=Thap, 1=Tieu chuan, 2=Cao
  uint8_t ventCycleMinutes = 40;
  uint8_t ventDutyDay1To3 = 10;
  uint8_t ventDutyDay4To7 = 15;
  uint8_t ventDutyDay8To11 = 25;
  uint8_t ventDutyDay12To15 = 35;
  uint8_t ventDutyDay16To18 = 50;
  uint8_t ventDutyDay19To21 = 70;

  bool turningEnabled = true;
  uint16_t turnIntervalMin = 120;
  uint16_t turnMaxRunSec = 300;
  TurnDirection nextDirection = TurnDirection::Left;

  uint8_t totalIncubationDays = 21;
  bool allowHeatWithoutBatch = false;
  uint16_t powerRestoreDelaySec = 30;

  // "Ap lai": BAT = sau mat dien tu dong ap tiep me cu ngay khi co dien lai,
  // khong hoi xac nhan. TAT (mac dinh) = luon hoi CO/HUY tren HMI nhu cu.
  bool autoResumeOnPowerLoss = false;

  float tempOffset = 0.0f;
  float humidityOffset = 0.0f;
  // Legacy EEPROM/protocol compatibility field. Runtime control intentionally
  // uses the fixed fail-safe RS485 freshness/offline policy; every load/save
  // normalizes this reserved value to 10 so old clients cannot weaken it.
  uint16_t sensorTimeoutSec = 10;
  bool alarmEnabled = true;
  ConnectivityMode connectivityMode = ConnectivityMode::Offline;
  // Nhac lai moi 30 phut qua Cloud Push neu den van bat trong luc me ap dang
  // chay (xem cloud_alert_link.h::checkLightAfterBatch). Nguoi dung co the
  // tat rieng canh bao nay ma khong anh huong cac canh bao khac.
  bool lightAfterBatchAlarmEnabled = true;
  // BAT (mac dinh, giu nguyen hanh vi tu truoc gio): canh bao Nhiet do cao/
  // Qua nhiet khan cap (E111/E112) hoat dong moi luc, ke ca khong co me ap -
  // day la trang thai an toan, phat hien qua nhiet bat thuong trong phong du
  // may dang khong ap. TAT: 2 canh bao nay CHI kiem tra khi dang co me ap
  // (giong het Nhiet do thap - E110 - von da luon nhu vay), huu ich neu
  // phong dat may nong tu nhien luc khong ap gay bao gia lien tuc.
  bool highTempAlarmWithoutBatch = true;
  // F-13 (audit truoc phat hanh v3.7.1, schema 9): dao tay giua me (AUTO tam
  // tat) truoc day KHONG dong bo voi lich dao tu dong - bat AUTO lai co the
  // dao tu dong THEM 1 lan ngay sau do neu chu ky da het han trong luc dao
  // tay. BAT: coi lan dao tay la mot lan dao THAT, doi lich (nextTurnAt_)
  // tinh tu ngay luc do, tranh dao thua. TAT (mac dinh, giu nguyen hanh vi
  // cu): dao tay khong dong lich, chu ky tu dong van dem tiep nhu khong co
  // gi xay ra. Cho phep doi ngay trong luc me dang chay (khong khoa).
  bool manualTurnReanchorsSchedule = false;
  // Tu kiem tra coi dinh ky (de xuat backlog, khong thuoc audit truoc phat
  // hanh v3.7.1) - opt-in, mac dinh TAT. Muc dich: phat hien som coi bao
  // (loa/relay coi that) bi hong/dut day ma khong ai biet, tranh den luc
  // that su can canh bao khan cap thi coi khong keu. Chi la 1 tieng "bip"
  // NGAN dinh ky (xem SIREN_SELF_TEST_INTERVAL_MS/PULSE_MS), khong phai
  // canh bao that, khong can ACK, tu dong bo qua neu dang co canh bao/loi
  // that nao dang hoat dong.
  bool sirenSelfTestEnabled = false;
};

// PID la che do dieu khien san pham: it nhat mot he so phai co authority.
// Kp-only / Ki-only / Kd-only van duoc phep cho commissioning; ca ba bang 0
// thi bo dieu khien khong the tao cong suat ben vung va phai bi tu choi.
inline bool mayapPidHasAuthority(const MachineConfig &cfg) {
  return cfg.kp > 0.0f || cfg.ki > 0.0f || cfg.kd > 0.0f;
}



struct NetworkStatus {
  ConnectivityMode requestedMode = ConnectivityMode::Offline;
  NetworkStateCode state = NetworkStateCode::Offline;
  bool credentialsConfigured = false;
  bool connected = false;
  int8_t rssiDbm = -127;
};

// Trang thai cong 1 "Doi Wi-Fi" tren HMI: mo AP cau hinh giong nhu giu nut
// BOOT, nhung phat/dieu khien tu menu Ket noi thay vi phai mo nap tu dien.
enum class WifiPortalState : uint8_t {
  Idle = 0, Starting = 1, ApActive = 2, Testing = 3, Success = 4, Failed = 5
};
struct WifiPortalStatus {
  WifiPortalState state = WifiPortalState::Idle;
  char apName[20] = "";
  char password[16] = "";
};

// Danh sach nga ra co the bat/tat doc lap trong Che do thu nghiem. Gia tri
// trung voi thu tu hien thi tren HMI; dung chung giua hmi.h va machine_control.h.
enum class TestOutputId : uint8_t {
  HeaterSsr = 0, HeatMaster, CirculationFan, VentFan, Light, Siren,
  TurnLeft, TurnRight, Count
};
enum class TestLimitId : uint8_t { Left = 0, Right = 1 };
enum class TestLimitPhase : uint8_t { Idle = 0, Waiting = 1, Success = 2, Timeout = 3 };

struct HmiFaultItem {
  uint16_t code = 0;
  int16_t detail = 0;
  uint8_t severity = 0;
  uint8_t flags = 0;  // bit0: condition con ton tai, bit1: da ACK
};

struct HmiEventItem {
  uint32_t sequence = 0;
  uint32_t epoch = 0;       // ngay gio DS3231 tai luc su kien; 0 = chua hop le
  uint32_t ageSec = 0;
  uint16_t code = 0;
  int16_t value = 0;
  uint8_t type = 0;
  uint8_t flags = 0;
};

struct HmiEventSnapshot {
  uint32_t sourceSequence = 0;
  uint8_t count = 0;
  uint8_t totalInWindow = 0;
  HmiEventItem items[HMI_EVENT_DISPLAY_CAPACITY]{};
};

struct MachineRuntime {
  float temperature = NAN;
  float humidity = NAN;
  bool sensorOnline = false;
  bool batchRunning = false;
  bool autoMode = false;
  bool turningLockdown = false;
  bool batchLogAvailable = false;
  uint8_t currentDay = 0;
  float heaterPower = 0.0f;
  bool adaptiveEnabled = false, adaptiveSelfHeating = false;
  uint8_t adaptiveState = 0, lastAdaptiveReason = 0;
  float adaptiveConfidence = 0, adaptiveLoadIndex = 0.5f, adaptiveCoastRiseC = 0;
  float adaptiveCoastTimeSec = 0, adaptiveHoldPowerPct = 0;
  float effectiveMaxPowerPct = 100, adaptiveApproachBandC = 0, adaptiveCoolingDemand = 0;
  uint32_t observerValidWindows = 0;
  bool heaterOn = false;
  bool circulationFanOn = false;
  bool ventFanOn = false;
  bool humidifierOn = false;
  bool lightOn = false;
  bool sirenOn = false;
  TurnState turnState = TurnState::Stopped;
  // TurnState::Left/Right dung chung cho CA dao binh thuong LAN tim goc (khi
  // chua biet vi tri khay, vd sau mat dien/loi RTC) - co gioi han vat ly
  // giong het nhau nen dong co chay y het, nhung nguoi dung khong phan biet
  // duoc tren man hinh phu neu chi nhin TRAI/PHAI. Co nay bat rieng khi dang
  // THUC SU tim goc (xem moveIsHoming_ trong machine_control.h).
  bool turnHoming = false;
  uint16_t nextTurnMinutes = 0;
  bool nextTurnScheduled = false;
  uint16_t turnCountToday = 0;
  uint32_t turnCountBatch = 0;
  uint32_t alarmMask = AlarmNone;
  AutoTuneState autoTuneState = AutoTuneState::Idle;
  uint8_t autoTuneProgress = 0;
  MachineStateCode stateCode = MachineStateCode::Boot;
  uint16_t primaryFaultCode = 0;
  uint8_t activeFaultCount = 0;
  uint8_t activeFaultDisplayCount = 0;
  HmiFaultItem activeFaults[HMI_FAULT_DISPLAY_CAPACITY]{};
  uint32_t faultNotificationSequence = 0;
  uint16_t lastRaisedFaultCode = 0;
  uint8_t lastRaisedFaultSeverity = 0;
  uint32_t eventSequence = 0;
  uint16_t relayTransitionsHour = 0;
  bool sensorStartupGrace = true;
  bool resumeConfirmationRequired = false;
  // F-06: me da qua so ngay ap du kien, dang cho xac nhan "tiep tuc u am"
  // (BatchOverdueContinue) hoac tu dong dung sau 12h - xem updateBatchOverdue().
  bool batchOverdueConfirmationPending = false;
  // Dang phat tieng "bip" tu kiem tra coi dinh ky (xem sirenSelfTestEnabled
  // o tren) - HMI/web dung de hien mot dong trang thai than thien ("Dang tu
  // kiem tra coi") thay vi de nguoi dung tuong nham day la canh bao that.
  bool sirenSelfTestActive = false;
  // ATtiny backup-alarm diagnostics (RAM only).
  bool attinyLinkHealthy = false;
  bool attinyBatchSynced = false;
  bool attinyStatusKnown = false;
  bool attinySirenBatteryLow = false;
  bool attinyCriticalActivityArmed = false;
  uint32_t attinyLastStatusAgeSec = UINT32_MAX;
  // Lan khoi dong nay la khoi dong lai SAU KHI MAT DIEN giua mot me dang ap
  // (khong phai bat may binh thuong). Chi nam trong RAM (MachineRuntime khong
  // luu EEPROM) va giu nguyen suot phien chay - cloud_alert_link.h dung de
  // gui dung MOT thong bao "da co dien lai" ve dien thoai.
  bool powerLossRecovery = false;
  bool timeValid = false;
  ConnectivityMode connectivityMode = ConnectivityMode::Offline;
  NetworkStateCode networkState = NetworkStateCode::Offline;
  bool networkConfigured = false;
  bool networkConnected = false;
  int8_t networkRssiDbm = -127;
  char dateText[11] = "--/--/----";
  char timeText[6] = "--:--";  // "HH:MM" tu RTC, hien o thanh trang thai man chinh
  char machineState[20] = "KHOI DONG";

  // Che do thu nghiem: bitmask theo TestOutputId dang duoc xung ON, va
  // trang thai kiem tra cong tac hanh trinh dang chon.
  bool testModeActive = false;
  uint8_t testOutputMaskActive = 0;
  TestLimitId testLimitTarget = TestLimitId::Left;
  TestLimitPhase testLimitPhase = TestLimitPhase::Idle;

  // Trang thai cong 1 "Doi Wi-Fi" phat tu HMI, doc lap voi NetworkStatus binh thuong.
  WifiPortalState wifiPortalState = WifiPortalState::Idle;
  char wifiPortalApName[20] = "";
  char wifiPortalPassword[16] = "";
};

enum class HmiCommandType : uint8_t {
  None, BatchStart, BatchStop,
  AlarmAck, AutoTuneStart, ResumeYes, ResumeNo,
  TestModeEnter, TestModeExit, TestOutputPulse, TestOutputStop, TestLimitStart, TestLimitCancel,
  WifiPortalStart, WifiPortalCancel, CloudPinReset, FirmwareWebApply, FirmwareWebCheckNow,
  FirmwareRollback, LightToggle,
  // F-06: xac nhan "tiep tuc u am" cho me da qua han ngay du kien - huy yeu
  // cau coi + huy dem nguoc tu dong dung 12h cho me hien tai (khong can lam
  // gi them; dung "Ket thuc me" (BatchStop) da co san neu muon dung som hon).
  BatchOverdueContinue,
  // User-requested stop of an Auto Tune already in progress. Appended to keep
  // every existing command numeric value stable for queues/tests/protocol.
  AutoTuneCancel
};
// F-09 (audit truoc phat hanh v3.7.1): AlarmAck truoc day khong phan biet
// lenh den tu bang dieu khien vat ly (HMI) hay tu xa (MQTT/web) - mot nguoi
// dung tu xa co the xoa loi dao trung (can kiem tra co khi that su) hoac tam
// tat coi khan cap lien tuc ma khong ai o canh may kiem tra. Nguon mac dinh
// la Local de moi noi trong hmi.h goi queueCommand() KHONG can sua (dung y
// dinh vat ly nhu truoc gio); chi transaction_bridge.h::handleCommandMessage()
// truyen rieng Remote.
enum class HmiCommandSource : uint8_t { Local, Remote };
struct HmiCommand {
  uint32_t id = 0;
  HmiCommandType type = HmiCommandType::None;
  uint32_t createdAt = 0;
  uint16_t validForMs = 0;
  uint16_t actuatorLeaseMs = 0;
  // Dung chung cho: mat na alarm (AlarmAck) HOAC gia tri TestOutputId/TestLimitId
  // (TestOutputPulse/TestLimitStart) tuy theo command.type.
  uint32_t alarmMask = AlarmNone;
  HmiCommandSource source = HmiCommandSource::Local;
};
enum class BuzzerCue : uint8_t { None, Key, Save, Ok, Error };

using HmiI2cLockFn = bool (*)(uint32_t timeoutMs);
using HmiI2cUnlockFn = void (*)();

// Dung chung giua HMI, EEPROM va firmware tong.
bool mayapI2cLock(uint32_t timeoutMs);
void mayapI2cUnlock();
void mayapI2cReport(uint8_t address, bool ok);
uint32_t mayapI2cRecoveryEpoch();
bool mayapSerialDebugEnabled();
void mayapSetSerialDebugEnabled(bool enabled);
void mayapSerialPrintf(bool force, const char *format, ...);
// Latch an toan toan he thong: chi reset chip moi xoa duoc.
bool mayapSystemTripLatched();
void mayapLatchSystemTrip();

// API task mang: implementation nam trong network_service.h.
void mayapNetworkBegin();
void mayapNetworkUpdate(uint32_t now);
void mayapSetConnectivityMode(ConnectivityMode mode);
NetworkStatus mayapGetNetworkStatus();
NetworkStatus mayapGetRawNetworkStatus();
// Cong 1 "Doi Wi-Fi": chi co tac dung khi dang o che do ONLINE. Mo AP cau hinh
// (giong giu nut BOOT) va tu ket noi thu SSID/mat khau moi nguoi dung luu qua
// web phu; ket qua doc qua mayapGetWifiPortalStatus().
bool mayapRequestWifiPortal();
void mayapCancelWifiPortal();
WifiPortalStatus mayapGetWifiPortalStatus();

void hmiBegin();
void hmiUpdate(uint32_t now);
void hmiSetConfig(const MachineConfig &config);
void hmiSetDate(const char *dateText);
const MachineConfig &hmiGetConfig();
void hmiSetRuntime(const MachineRuntime &runtime);
void hmiSetEventLog(const HmiEventSnapshot &snapshot);
bool hmiTakeSavedConfig(MachineConfig &out, uint32_t &transactionId);
bool hmiConfirmConfigSave(uint32_t transactionId, bool ok,
                          const MachineConfig *storedConfig = nullptr);
bool hmiTakeCommand(HmiCommand &out);
bool hmiConfirmCommand(uint32_t commandId, bool ok,
                       const char *message = nullptr);
void hmiSetI2cLockCallbacks(HmiI2cLockFn lockFn, HmiI2cUnlockFn unlockFn);
void hmiPlayCue(BuzzerCue cue);
