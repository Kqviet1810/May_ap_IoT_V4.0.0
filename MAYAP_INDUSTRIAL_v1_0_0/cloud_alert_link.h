#pragma once

#include "config.h"
#include "service_recovery.h"
#include "network_io_guard.h"
#include "bounded_http.h"
#include "cloud_fault_events.h"
#include "cloud_alarm_receipt.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <string.h>

// ============================================================================
// KENH CANH BAO CLOUD (CLOUDFLARE WORKER) - THAY THE HOAN TOAN TELEGRAM CU
// ----------------------------------------------------------------------------
// KHONG dung chung ket noi/hang doi voi transaction_bridge.h. File nay tu mo HTTPS
// rieng toi Cloudflare Worker (xem thu muc cloudflare/) - mat MQTT/Web KHONG
// anh huong gi den kenh nay, va nguoc lai. Nguon du lieu goc van la
// MachineController (qua mayapCloudSetRuntime, goi tu controlTask giong het
// pattern mayapRealtimeSetRuntime cua transaction_bridge.h).
//
// KHAC Telegram truoc day: kenh nay chi MOT CHIEU (ESP32 -> Worker -> Web
// Push -> trinh duyet). Khong con lenh /status, /help hoi nguoc lai ESP32 -
// dieu khien 2 chieu da co san qua MQTT (transaction_bridge.h), khong can lam lai
// o day. Nguoi dung cuoi KHONG cau hinh gi tren ESP32 cho kenh nay (khong con
// "Chat ID" nhu Telegram) - viec ghep trinh duyet nhan thong bao hoan toan
// thuc hien o trang web (push.js/setup.html), dung device_id cong khai.
//
// Vi tri include: sau hmi.h (dung mayapDeviceIdText tu network_service.h va
// cac kieu MachineRuntime/FaultCode tu machine_control.h/config.h da include
// truoc do), truoc machine_control.h (MachineController goi nguoc lai
// mayapCloudSetRuntime()).
//
// Thu vien can cai: KHONG can cai them - HTTPClient/WiFiClientSecure co san
// trong ESP32 Arduino core; ArduinoJson da la yeu cau cua transaction_bridge.h.
//
// Tai nguyen: moi lan goi tao MOI mot WiFiClientSecure NGAN HAN (huy ngay sau
// khi xong), khong giu ket noi thuong truc nhu MQTT - phu hop voi tan suat
// thap (vai phut/lan) va tranh chiem RAM lau dai tren thiet bi khong PSRAM.
// TLS bat buoc xac thuc CA goc tu MAYAP_TLS_ROOT_CA; thieu CA thi kenh dong -
// Cloudflare dung chung chi hop le, ESP32 Arduino core khong co san bo goc
// CA de xac thuc day du ma khong tang dang ke dung luong firmware; day la
// danh doi bao mat da duoc ghi nhan, xem bao cao audit.
// ============================================================================

namespace MayapCloudInternal {

inline uint32_t elapsedMs(uint32_t now, uint32_t then) {
  return static_cast<uint32_t>(now - then);
}
inline bool timeReached(uint32_t now, uint32_t target) {
  return static_cast<int32_t>(now - target) >= 0;
}

enum class NotifyLevel : uint8_t { Info, Warning, Critical, System };

inline const char *severityText(NotifyLevel level) {
  switch (level) {
    case NotifyLevel::Critical: return "critical";
    case NotifyLevel::Warning: return "warning";
    case NotifyLevel::System: return "system";
    default: return "info";
  }
}

// ------------------------------ Hop thu runtime --------------------------------
// Ghi boi controlTask (mayapCloudSetRuntime), doc boi networkTask. Copy
// nguyen struct trong critical section ngan, giong het pattern cua
// transaction_bridge.h::mayapRealtimeSetRuntime - khong I/O trong vung khoa.
static portMUX_TYPE cloudMux = portMUX_INITIALIZER_UNLOCKED;
static MachineRuntime knownRuntime{};
static bool knownRuntimeValid = false;

// Ban lam viec RIENG cua networkTask, cap nhat tu knownRuntime moi chu ky
// kiem tra (duoi khoa, xong roi sao chep ra ngoai). Tat ca ham quyet dinh
// (checkFaults/checkTransitions) chi doc bien nay - khong bao gio bi
// controlTask ghi de, tranh rang buoc "doc-roi-ghi-lai" khong an toan.
static MachineRuntime processingRuntime{};

// Hop thu config (giong het pattern hop thu runtime o tren) - chi can doc
// cac co BAT/TAT canh bao (vd lightAfterBatchAlarmEnabled), khong can toan
// bo MachineConfig nhung dung chung struct cho don gian/de doi chieu.
static MachineConfig knownConfig{};
static bool knownConfigValid = false;
static MachineConfig processingConfig{};

// Backoff RIENG cho Cloud Push - hoan toan doc lap voi backoff cua MQTT
// (transaction_bridge.h) va STA Wi-Fi (network_service.h). Dung chung cho ca 3
// loai goi HTTPS (register/heartbeat/alarm) vi ca 3 cung phan anh cung 1 cau
// hoi "co goi duoc toi Worker luc nay khong". Thanh cong o BAT KY chieu nao
// cung reset ve nhanh nhat cho ca 3.
static BackoffTimer cloudBackoff{};
static bool registered = false;
static uint32_t lastHeartbeatAt=0U;

// Co hieu "dat lai ma PIN web ve mac dinh" phat tu HMI (controlTask) toi
// networkTask - dung chung idiom voi portalRequestFlag cua network_service.h
// (volatile + __atomic_*, khong can mutex vi chi 1 writer/1 reader moi
// chieu). Nguoi lap dat co mat vat ly tai HMI la dieu kien DUY NHAT de kich
// hoat - khong co duong nao tu web tu goi duoc lenh nay (endpoint /api/
// device/reset-pin chi chap nhan device_key bi mat cua firmware, khong
// phai PIN web, xem cloudflare/src/index.js::handleResetPin).
static volatile uint8_t pinResetRequestFlag = 0U;

// -------------------------------- Hang doi gui ----------------------------------
// Chi networkTask dung (ca ghi lan doc) - moi logic quyet dinh gui gi cung
// chay trong mayapCloudAlertUpdate(), khong co task nao khac cham vao.
static uint32_t eventSequence=0U;
static uint32_t eventBootHigh=0U,eventBootLow=0U;
struct OutboxItem {
  uint32_t sequence=0U,detectedAt=0U;
  bool used = false,attempted=false;
  BackoffTimer retry{};
  char alarmType[24] = "";
  NotifyLevel severity = NotifyLevel::Info;
  bool resolved = false;
  char message[160] = "";
  bool hasReadings = false;
  float temperature = 0.0f;
  float humidity = 0.0f;
};
static OutboxItem outbox[CLOUD_OUTBOX_SIZE];
static uint8_t outboxHead = 0U, outboxTail = 0U, outboxCount = 0U;
static uint32_t lastSendAt = 0U;
static uint32_t lastRequestFinishedAt = 0U;
static bool requestDeferred = false;
static uint32_t outboxDropped = 0U, outboxCriticalDropped = 0U;

inline bool enqueueRaw(const char *alarmType, NotifyLevel severity, bool resolved,
                       const char *message, bool hasReadings, float temperature, float humidity, uint32_t detectedAt=0U) {
  if (!alarmType || !alarmType[0] || !message || !message[0]) return false;
  // Coalesce repeat notifications for the SAME state only. Keep an active
  // alarm and its recovery as distinct events (especially critical faults).
  for (uint8_t n = 0U; n < outboxCount; ++n) {
    const uint8_t p = (outboxTail + CLOUD_OUTBOX_SIZE - 1U - n) % CLOUD_OUTBOX_SIZE;
    OutboxItem &pending = outbox[p];
    if (!pending.used || strcmp(pending.alarmType, alarmType)) continue;
    if (!pending.attempted && pending.resolved == resolved && pending.severity == severity) {
      snprintf(pending.message, sizeof(pending.message), "%s", message);
      pending.hasReadings = hasReadings; pending.temperature = temperature; pending.humidity = humidity;
      return true;
    }
    break; // Never coalesce across this alarm's opposite transition.
  }
  if (outboxCount >= CLOUD_OUTBOX_SIZE || (severity!=NotifyLevel::Critical && outboxCount>=CLOUD_OUTBOX_SIZE-4U)) {
    ++outboxDropped; // admission backpressure, not eviction of a queued event
    static uint32_t lastPressureLog=0U;
    if(lastPressureLog==0U || static_cast<uint32_t>(millis()-lastPressureLog)>=30000U) {
      lastPressureLog=millis();
      mayapSerialPrintf(false,"[CLOUD] outbox backpressure=%lu; fault edges retained\n",static_cast<unsigned long>(outboxDropped));
    }
    return false;
  }
  OutboxItem &item = outbox[outboxTail];
  snprintf(item.alarmType, sizeof(item.alarmType), "%s", alarmType);
  item.sequence=++eventSequence;item.detectedAt=detectedAt?detectedAt:millis();
  item.severity = severity;
  item.resolved = resolved;
  snprintf(item.message, sizeof(item.message), "%s", message);
  item.hasReadings = hasReadings;
  item.temperature = temperature;
  item.humidity = humidity;
  item.used = true;item.attempted=false;item.retry.reset(millis());
  outboxTail = static_cast<uint8_t>((outboxTail + 1U) % CLOUD_OUTBOX_SIZE);
  ++outboxCount;
  return true;
}

inline bool enqueueLevel(const char *alarmType, NotifyLevel level, const char *body) {
  const bool hasReadings = knownRuntimeValid;
  return enqueueRaw(alarmType, level, false, body, hasReadings, processingRuntime.temperature, processingRuntime.humidity);
}

inline bool enqueueResolved(const char *alarmType, NotifyLevel level, const char *body) {
  const bool hasReadings = knownRuntimeValid;
  return enqueueRaw(alarmType, level, true, body, hasReadings, processingRuntime.temperature, processingRuntime.humidity);
}

// --------------------------- Noi dung loi (Vietnamese) --------------------------
inline const char *faultSummaryText(uint16_t code) {
  switch (code) {
    case 101: return "Mất cảm biến nhiệt độ/độ ẩm";
    case 102: return "Cảm biến trả về giá trị sai";
    case 103: return "Cảm biến bất thường (nghi ngờ hỏng)";
    case 104: return "Cảm biến đứng giá khi heater vẫn cấp nhiệt";
    case 110: return "Nhiệt độ xuống thấp hơn ngưỡng cảnh báo";
    case 111: return "Nhiệt độ vượt quá ngưỡng cảnh báo cao";
    case 112: return "QUÁ NHIỆT KHẨN CẤP - đã ngắt nguồn nhiệt ngay lập tức";
    case 113: return "Nhiệt độ biến thiên bất thường - kiểm tra quạt/relay";
    case 114: return "Nhiệt độ dao động bất thường - kiểm tra chỉnh định PID";
    case 115: return "Thanh nhiệt hoạt động nhưng nhiệt không tăng - nghi ngờ hỏng relay/SSR";
    case 120: return "Độ ẩm thấp - kiểm tra nguồn cấp nước";
    case 121: return "Độ ẩm cao bất thường - kiểm tra thông gió";
    case 130: return "Công tắc nhiệt bị tắt trong lúc đang ấp";
    case 137: return "Đang chờ RTC hợp lệ để chạy tiếp mẻ ấp - không xác định được đã mất điện bao lâu";
    case 132: return "Cần bật lại chế độ AUTO để tiếp tục mẻ";
    case 133: return "Chế độ AUTO bị tắt trong lúc đang ấp";
    case 134: return "Đảo trứng tự động bị khóa trong lúc đang ấp";
    case 201: return "Lỗi cả 2 công tắc hành trình đảo trứng";
    case 202: return "Đảo trứng quá thời gian cho phép";
    case 203: return "Cơ cấu đảo trứng bị kẹt";
    case 204: return "Xung đột lệnh điều khiển đảo trứng";
    case 301: return "Mất dữ liệu cấu hình EEPROM";
    case 302: return "Bộ nhớ EEPROM có dấu hiệu suy giảm";
    case 303: return "Máy vừa khởi động lại bất thường";
    case 304: return "Xung đột tín hiệu điều khiển đầu ra";
    case 305: return "Relay đóng cắt quá nhiều lần trong giờ";
    case 306: return "Lỗi đồng hồ thời gian thực (RTC)";
    case 313: return "Đang dọn dẹp dữ liệu mẻ cũ trước đó";
    case 314: return "Mất nhật ký an toàn (safety journal)";
    case 315: return "Mất nhật ký sự kiện của mẻ";
    case 401: return "Bộ nhớ RAM còn thấp - hệ thống đang tự theo dõi";
    case 402: return "Bộ nhớ RAM cạn kiệt - máy sẽ tự khởi động lại để phòng ngừa";
    case 403: return "Dự đoán nhiệt độ sắp chạm ngưỡng cảnh báo theo tốc độ hiện tại";
    case 404: return "Bộ nhớ EEPROM phải thử lại nhiều bất thường - có thể đang suy giảm";
    case 501: return "Mất liên lạc mạch báo mất điện ATtiny - kiểm tra pin CR2032 và dây tín hiệu";
    case 502: return "Pin còi 9V sắp hết - hãy thay pin sớm để bảo đảm còi báo khi mất điện";
    case 503: return "Trạng thái mẻ giữa ESP32 và ATtiny chưa đồng bộ";
    default: return "Lỗi không xác định";
  }
}

// HmiFaultItem.severity la uint8_t "tho" (khong phai enum Mayap::FaultSeverity)
// chinh vi ly do nay: config.h dung truoc machine_control.h trong thu tu
// include, va cloud_alert_link.h cung dung truoc machine_control.h nen KHONG
// the tham chieu toi kieu Mayap::FaultSeverity (chi ton tai sau khi
// machine_control.h duoc doc). Dung thang gia tri so, khop dinh nghia enum
// {Info=0, Warning=1, Stop=2, Emergency=3} trong machine_control.h.
constexpr uint8_t FAULT_SEVERITY_INFO = 0U;
constexpr uint8_t FAULT_SEVERITY_WARNING = 1U;
constexpr uint8_t FAULT_SEVERITY_STOP = 2U;
constexpr uint8_t FAULT_SEVERITY_EMERGENCY = 3U;

inline NotifyLevel levelForSeverity(uint8_t severity) {
  if (severity == FAULT_SEVERITY_EMERGENCY || severity == FAULT_SEVERITY_STOP) {
    return NotifyLevel::Critical;
  }
  if (severity == FAULT_SEVERITY_WARNING) return NotifyLevel::Warning;
  return NotifyLevel::Info;
}

inline uint32_t repeatIntervalForSeverity(uint8_t severity) {
  if (severity == FAULT_SEVERITY_EMERGENCY) return CLOUD_REPEAT_CRITICAL_EMERGENCY_MS;
  if (severity == FAULT_SEVERITY_STOP) return CLOUD_REPEAT_CRITICAL_STOP_MS;
  if (severity == FAULT_SEVERITY_WARNING) return CLOUD_REPEAT_WARNING_MS;
  return CLOUD_REPEAT_INFO_MS;
}

// ------------------------- Theo doi loi dang xay ra ------------------------------
// Rieng cho Cloud Push, doc lap voi eventLog_/HMI: chi quan tam "dieu kien co
// con that su xay ra hay khong" (flags bit0) de quyet dinh gui moi/nhac
// lai/da het, khong quan tam co ACK tren HMI hay chua.
struct FaultTrack {
  bool used = false;
  uint16_t code = 0;
  uint8_t severity = 0;
  uint32_t firstSentAt = 0;
  uint32_t lastSentAt = 0;
};
static FaultTrack faultTrack[CLOUD_ACTIVE_TRACK_SIZE];

inline void alarmTypeForFault(uint16_t code, char *out, size_t outLen) {
  snprintf(out, outLen, "FAULT_%u", code);
}

inline void checkFaults(uint32_t now) {
  MayapCloudFaultEvents::Event edge;
  // Drain at most one mailbox capacity per update; backpressure never waits
  // in controlTask and a blocked HTTPS request cannot erase a short fault.
  for(uint8_t n=0;n<MayapCloudFaultEvents::CAPACITY && MayapCloudFaultEvents::peek(edge);++n) {
    int16_t slot=-1;
    for(uint8_t i=0;i<CLOUD_ACTIVE_TRACK_SIZE;++i)
      if(faultTrack[i].used && faultTrack[i].code==edge.code){slot=i;break;}
    if(slot<0) for(uint8_t i=0;i<CLOUD_ACTIVE_TRACK_SIZE;++i)
      if(!faultTrack[i].used){slot=i;break;}
    if(slot<0) break;
    char type[24],body[160];alarmTypeForFault(edge.code,type,sizeof(type));
    snprintf(body,sizeof(body),"%s%s",edge.active?"":"Đã hết: ",faultSummaryText(edge.code));
    if(!enqueueRaw(type,levelForSeverity(edge.severity),!edge.active,body,true,
        processingRuntime.temperature,processingRuntime.humidity,edge.at)) break;
    FaultTrack &track=faultTrack[slot];
    track.used=edge.active;track.code=edge.code;track.severity=edge.severity;
    track.firstSentAt=track.lastSentAt=now;
    MayapCloudFaultEvents::consume(edge);
  }
  static uint32_t reportedOverflow=0U;
  const uint32_t overflow=__atomic_load_n(&MayapCloudFaultEvents::overflow,__ATOMIC_RELAXED);
  if(overflow!=reportedOverflow){reportedOverflow=overflow;mayapSerialPrintf(false,"[CLOUD] fault mailbox overflow=%lu state-resync pending\n",static_cast<unsigned long>(overflow));}
  for(auto &track:faultTrack) if(track.used && timeReached(now,track.lastSentAt+repeatIntervalForSeverity(track.severity))) {
    char type[24],body[160];alarmTypeForFault(track.code,type,sizeof(type));
    snprintf(body,sizeof(body),"Vẫn còn: %s",faultSummaryText(track.code));
    if(enqueueRaw(type,levelForSeverity(track.severity),false,body,true,
        processingRuntime.temperature,processingRuntime.humidity)) track.lastSentAt=now;
  }
}

// --------------------------- Su kien mot lan (INFO/SYSTEM) -----------------------
static bool lastBatchRunning = false;
static bool haveLastBatchRunning = false;

inline void checkTransitions(uint32_t now) {
  (void)now;
  if (!haveLastBatchRunning) {
    lastBatchRunning = processingRuntime.batchRunning;
    haveLastBatchRunning = true;
  } else if (processingRuntime.batchRunning != lastBatchRunning) {
    if(enqueueLevel(processingRuntime.batchRunning ? "BATCH_STARTED" : "BATCH_ENDED", NotifyLevel::Info,
        processingRuntime.batchRunning ? "Đã bắt đầu mẻ ấp mới." : "Đã kết thúc mẻ ấp.")) lastBatchRunning=processingRuntime.batchRunning;
  }
}

// ------------------- Thong bao: da co dien lai giua me ap -------------------
// Khi mat dien, chinh may ap cung tat theo nen KHONG the tu bao luc do (canh
// bao "mat ket noi" do Worker tu phat hien qua khoang lang heartbeat - xem
// checkDeviceConnectivity trong cloudflare/src/index.js). Nhung luc CO DIEN
// LAI thi ESP32 song lai va biet ro minh vua khoi dong sau mat dien giua me
// (runtime.powerLossRecovery, dat trong MachineController::begin) - day la
// thoi diem bao ve dien thoai chinh xac va co ich nhat: nguoi dung can biet
// dien da co lai VA me ap co tu chay tiep khong hay dang cho xac nhan tay.
static bool powerRestoreReported = false;

inline void checkPowerRestored(uint32_t now) {
  (void)now;
  if (powerRestoreReported || !processingRuntime.powerLossRecovery) return;
  // Chi bao khi thuc su co me ap dang cho phuc hoi/dang chay - mat dien luc
  // khong ap gi thi khong can lam phien (cung nguyen tac voi canh bao mat
  // ket noi phia Worker, chi bao khi dang co me).
  if (!processingRuntime.batchRunning && !processingRuntime.resumeConfirmationRequired) return;
  if (processingRuntime.resumeConfirmationRequired) {
    powerRestoreReported=enqueueLevel("POWER_RESTORED", NotifyLevel::Warning,
        "Đã có điện lại. Mẻ ấp đang CHỜ XÁC NHẬN trên máy để chạy tiếp.");
  } else {
    char body[160];
    snprintf(body, sizeof(body),
        "Đã có điện lại. Mẻ ấp đã tự chạy tiếp (ngày %u/%u).",
        static_cast<unsigned>(processingRuntime.currentDay),
        static_cast<unsigned>(processingConfig.totalIncubationDays));
    powerRestoreReported=enqueueLevel("POWER_RESTORED", NotifyLevel::Info, body);
  }
}

// --------------------- Canh bao: den van bat khi dang ap me --------------------
// Dieu kien: me ap dang chay VA den (lightOn) van bat. Gui 1 lan khi vua phat
// hien, sau do nhac lai moi CLOUD_LIGHT_AFTER_BATCH_REPEAT_MS (30 phut) neu
// van con dung, va bao "da binh thuong" ngay khi het dieu kien (tat den hoac
// ket thuc me) - dung nguyen mau checkFaults() nhung cho 1 dieu kien don, co
// the tat rieng qua config (khac cac loi FaultCode khac khong tat duoc).
static bool lightAfterBatchActive = false;
static uint32_t lightAfterBatchLastSentAt = 0;

inline void checkLightAfterBatch(uint32_t now) {
  if (!processingConfig.lightAfterBatchAlarmEnabled) {
    lightAfterBatchActive = false;  // nguoi dung vua tat: khong con "dinh" trang thai active cu
    return;
  }
  const bool condition = processingRuntime.batchRunning && processingRuntime.lightOn;
  if (condition) {
    if (!lightAfterBatchActive) {
      lightAfterBatchActive=enqueueLevel("LIGHT_ON_DURING_BATCH", NotifyLevel::Warning,
          "Đèn đang bật trong lúc mẻ ấp đang chạy - kiểm tra nếu không cần thiết.");
      if(lightAfterBatchActive) lightAfterBatchLastSentAt=now;
    } else if (timeReached(now, lightAfterBatchLastSentAt + CLOUD_LIGHT_AFTER_BATCH_REPEAT_MS)) {
      if(enqueueLevel("LIGHT_ON_DURING_BATCH", NotifyLevel::Warning,
          "Vẫn còn: đèn đang bật trong lúc mẻ ấp đang chạy.")) lightAfterBatchLastSentAt=now;
    }
  } else if (lightAfterBatchActive) {
    // Noi RO nguyen nhan het canh bao, khong bao chung chung "den da tat HOAC
    // me ap da ket thuc" - nguoi dung doc xong khong biet thuc te vua xay ra
    // chuyen gi. Tai day van con du du lieu de biet chinh xac ve nao dung.
    if (!processingRuntime.lightOn) {
      lightAfterBatchActive=!enqueueResolved("LIGHT_ON_DURING_BATCH", NotifyLevel::Warning,
          "Đã hết: đèn đã được tắt.");
    } else {
      lightAfterBatchActive=!enqueueResolved("LIGHT_ON_DURING_BATCH", NotifyLevel::Warning,
          "Đã hết: mẻ ấp đã kết thúc (đèn vẫn đang bật).");
    }
  }
}

// --------------------- Canh bao: bo lo lich dao trung ---------------------
// Khac cac loi co khi tuc thi da co (ket CTHT, qua gio dao...): day la "lich
// dao bi treo am tham" - dem so lan dao THANH CONG (turnCountBatch) khong
// tang du lau so voi chu ky da cau hinh, du khong co loi co khi ro rang nao.
static uint32_t turnMissedLastCount = 0;
static bool turnMissedHaveCount = false;
static uint32_t turnMissedCountChangedAt = 0;
static bool turnMissedActive = false;

inline void checkTurnCycleMissed(uint32_t now) {
  if (!processingConfig.turningEnabled || !processingRuntime.batchRunning) {
    turnMissedHaveCount = false;
    if (turnMissedActive) {
      // KHONG bao "da hoat dong binh thuong tro lai" o day - canh bao het
      // vi me ap dung/nguoi dung tat tu dong dao, KHONG phai vi co cau dao
      // da chay lai duoc. Bao dung su that de nguoi dung khong hieu nham la
      // may da tu khac phuc xong (nhanh "da chay lai that" nam ben duoi).
      if (!processingRuntime.batchRunning) {
        turnMissedActive=!enqueueResolved("TURN_CYCLE_STALLED", NotifyLevel::Warning,
            "Đã hết: mẻ ấp đã kết thúc (chưa kiểm tra được cơ cấu đảo).");
      } else {
        turnMissedActive=!enqueueResolved("TURN_CYCLE_STALLED", NotifyLevel::Warning,
            "Đã hết: đã tắt tự động đảo (chưa kiểm tra được cơ cấu đảo).");
      }
    }
    return;
  }
  if (!turnMissedHaveCount || processingRuntime.turnCountBatch != turnMissedLastCount) {
    turnMissedLastCount = processingRuntime.turnCountBatch;
    turnMissedCountChangedAt = now;
    turnMissedHaveCount = true;
    if (turnMissedActive) {
      // Day moi la phuc hoi THAT: dem so lan dao thanh cong vua tang tro lai.
      turnMissedActive=!enqueueResolved("TURN_CYCLE_STALLED", NotifyLevel::Warning,
          "Đã hết: đảo trứng đã chạy lại bình thường.");
      if(turnMissedActive) turnMissedHaveCount=false;
    }
    return;
  }
  const uint32_t staleLimitMs = static_cast<uint32_t>(processingConfig.turnIntervalMin) *
      60000UL * TURN_MISSED_MULTIPLIER;
  if (!turnMissedActive && staleLimitMs > 0U &&
      timeReached(now, turnMissedCountChangedAt + staleLimitMs)) {
    turnMissedActive=enqueueLevel("TURN_CYCLE_STALLED", NotifyLevel::Warning,
        "Không ghi nhận đảo trứng thành công quá lâu - kiểm tra cơ cấu đảo.");
  }
}

// --------------------- Nhac: sap den ngay no / me qua han ------------------
static bool batchNearingEndSent = false;
static bool batchOverdueActive = false;

inline void checkBatchSchedule(uint32_t now) {
  (void)now;
  if (!processingRuntime.batchRunning) {
    batchNearingEndSent = false;
    if (batchOverdueActive) {
      batchOverdueActive=!enqueueResolved("BATCH_OVERDUE", NotifyLevel::Info, "Mẻ ấp đã kết thúc.");
    }
    return;
  }
  const uint8_t total = processingConfig.totalIncubationDays;
  const uint8_t current = processingRuntime.currentDay;
  if (total == 0U) return;

  if (!batchNearingEndSent && total > current &&
      static_cast<uint8_t>(total - current) <= BATCH_NEARING_END_DAYS_LEFT) {
    char body[160];
    snprintf(body, sizeof(body),
        "Còn %u ngày đến ngày dự kiến nở (ngày %u/%u).",
        static_cast<unsigned>(total - current), static_cast<unsigned>(current),
        static_cast<unsigned>(total));
    batchNearingEndSent=enqueueLevel("BATCH_NEARING_END", NotifyLevel::Info, body);
  }
  if (!batchOverdueActive && current > total) {
    char body[160];
    snprintf(body, sizeof(body),
        "Quá hạn %u ngày (ngày %u/%u) - kiểm tra tình trạng trứng.",
        static_cast<unsigned>(current - total), static_cast<unsigned>(current),
        static_cast<unsigned>(total));
    batchOverdueActive=enqueueLevel("BATCH_OVERDUE", NotifyLevel::Warning, body);
  }
}

// ------------------------- Canh bao: Wi-Fi tin hieu yeu ---------------------
static bool wifiWeakTracking = false;
static uint32_t wifiWeakSinceAt = 0;
static bool wifiWeakActive = false;

inline void checkWifiSignal(uint32_t now) {
  const NetworkStatus status = mayapGetNetworkStatus();
  const bool onlineAndWeak = status.requestedMode == ConnectivityMode::Online &&
      status.connected && status.rssiDbm <= WIFI_RSSI_WEAK_DBM;
  if (!onlineAndWeak) {
    wifiWeakTracking = false;
    if (wifiWeakActive) {
      wifiWeakActive=!enqueueResolved("WIFI_SIGNAL_WEAK", NotifyLevel::Info, "Đã hết: tín hiệu Wi-Fi đã ổn định trở lại.");
    }
    return;
  }
  if (!wifiWeakTracking) {
    wifiWeakTracking = true;
    wifiWeakSinceAt = now;
    return;
  }
  if (!wifiWeakActive && timeReached(now, wifiWeakSinceAt + WIFI_RSSI_WEAK_DURATION_MS)) {
    char body[160];
    snprintf(body, sizeof(body),
        "Tín hiệu Wi-Fi yếu kéo dài (%d dBm) - nên đặt máy gần router hơn.",
        static_cast<int>(status.rssiDbm));
    wifiWeakActive=enqueueLevel("WIFI_SIGNAL_WEAK", NotifyLevel::Warning, body);
  }
}

// GHI CHU: tung co checkConnectivity() gui "SYSTEM_ONLINE/SYSTEM_OFFLINE" moi
// khi ESP32 tu thay doi trang thai mang - BO DI vi qua on ao (tu bao ngay ca
// khi WiFi chi giat rat ngan luc dang backoff/thu lai) va da THUA so voi canh
// bao "mat ket noi thiet bi" phia Worker (checkDeviceConnectivity trong
// cloudflare/src/index.js) - kenh do doc lap, co debounce that su (>=2 phut),
// dang tin cay hon nhieu. Trang thai online/offline tuc thi van xem duoc tren
// web qua MQTT (khong can push rieng).

// ------------------------------- Goi HTTPS ---------------------------------------
inline bool beginCloudRequest(HTTPClient &http, WiFiClientSecure &client, const char *path) {
  if (!TLS_ROOT_CA[0]) {
    mayapSetProvisioningState(MayapProvisioningState::TlsError);
    mayapSerialPrintf(false, "[CLOUD] TLS bi khoa: thieu CA goc tin cay\n");
    return false;
  }
  client.setCACert(TLS_ROOT_CA);
  client.setConnectionTimeout(CLOUD_HTTP_CONNECT_TIMEOUT_MS);
  client.setHandshakeTimeout(8);
  http.setConnectTimeout(CLOUD_HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
  char url[160];
  snprintf(url, sizeof(url), "https://%s%s", CLOUD_API_HOST, path);
  const bool started = http.begin(client, url);
  if (!started) mayapSetProvisioningState(MayapProvisioningState::CloudError);
  return started;
}

inline bool postJson(const char *path, const JsonDocument &doc, const char *logTag,
                     String *responseBody = nullptr, int *responseCode = nullptr, bool urgent = false) {
  requestDeferred = true;
  if (!urgent && lastRequestFinishedAt != 0U &&
      elapsedMs(millis(), lastRequestFinishedAt) < CLOUD_MIN_SEND_GAP_MS) {
    if (responseCode) *responseCode = 0;
    return false;
  }
  // A TLS lease held by someone else (typically the ~2.5 s realtime handshake) is waited out, never
  // pre-empted: asking realtime to yield mid-handshake aborted every reconnect while alarms were
  // queued, so the Web stayed offline for the whole backlog. Only a real heap shortage asks for a yield.
  if (urgent && !mayapTlsBusy() && ESP.getFreeHeap()<73728U) mayapRequestCloudTlsYield(millis(),true);
  MayapTlsOperation tlsOperation(MayapTlsKind::Cloud);
  if (!tlsOperation) {
    static uint32_t lastAdmissionLogAt = 0U;
    if (lastAdmissionLogAt == 0U || elapsedMs(millis(), lastAdmissionLogAt) >= 30000U) {
      lastAdmissionLogAt = millis();
      mayapSerialPrintf(false, "[CLOUD] %s deferred TLS heap=%lu largest=%lu yield=%u\n",
          logTag, static_cast<unsigned long>(ESP.getFreeHeap()),
          static_cast<unsigned long>(ESP.getMaxAllocHeap()),
          mayapCloudTlsYieldRequested(millis()) ? 1U : 0U);
    }
    if (responseCode) *responseCode = 0;
    return false; // defer through the existing bounded Cloud retry queue
  }
  requestDeferred = false;
  const uint32_t heapBefore = ESP.getFreeHeap();
  const uint32_t startedAt = millis();
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginCloudRequest(http, client, path)) {
    http.end();
    client.stop();
    if (responseCode) *responseCode = 0;
    mayapSerialPrintf(false, "[CLOUD] %s -> http.begin() THAT BAI (URL/TLS)\n", logTag);
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  char body[1024];
  const size_t bodySize = measureJson(doc);
  if (doc.overflowed() || bodySize >= sizeof(body)) {
    http.end(); client.stop();
    lastRequestFinishedAt = millis();
    if (responseCode) *responseCode = 0;
    return false;
  }
  serializeJson(doc, body, sizeof(body));
  const int code = http.POST(reinterpret_cast<uint8_t *>(body), bodySize);
  if (responseCode) *responseCode = code;
  char response[1024]{};
  const bool bodyOk = code > 0 && mayapReadBoundedHttpBody(http, response, sizeof(response));
  const bool ok = code == 200 && bodyOk;
  if (responseBody) *responseBody = bodyOk ? response : "";
  if (ok) {
    mayapSerialPrintf(false, "[CLOUD] %s -> HTTP 200 OK\n", logTag);
  } else {
    // Never print server response content (provisioning may contain secrets).
    mayapSerialPrintf(false, "[CLOUD] %s -> HTTP %d FAIL bodyOk=%u\n", logTag, code, bodyOk);
  }
  http.end();
  client.stop();
  lastRequestFinishedAt = millis();
  mayapSerialPrintf(false, "[TLS] cloud=%s ms=%lu heapBefore=%lu after=%lu minEver=%lu\n",
      logTag, static_cast<unsigned long>(elapsedMs(millis(), startedAt)),
      static_cast<unsigned long>(heapBefore), static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(ESP.getMinFreeHeap()));
  mayapServiceBeat(MayapRecovery::Service::Cloud);
  return ok;
}

inline void storeProvisioningFromResponse(const String &response) {
  JsonDocument parsed;
  if (deserializeJson(parsed, response)) return;
  if (!parsed["success"].as<bool>()) return;
  // Worker chi tra web_pin cho may moi, may vua reset PIN, hoac ban ghi cu
  // chua co PIN. May cu da co PIN phai giu PIN cu; danh dau da dong bo de
  // HMI khong hien "DANG DONG BO" vo han.
  mayapMarkWebPinConfigured();
  const char *pin = parsed["web_pin"] | "";
  if (pin[0]) mayapStoreWebPin(pin);
  const char *commandKey = parsed["command_key"] | "";
  if (commandKey[0]) {
    if (!mayapStoreCommandKey(commandKey)) {
      mayapSerialPrintf(false, "[CLOUD] command_key khong hop le/khong luu duoc\n");
    }
  }
  // Per-device MQTT broker credential (NVS); absent when the Worker has none.
  const char *mqttPassword = parsed["mqtt_password"] | "";
  if (mqttPassword[0] && !mayapStoreMqttKey(mqttPassword)) {
    mayapSerialPrintf(false, "[CLOUD] mqtt_password khong hop le/khong luu duoc\n");
  }
}

inline bool rotateLegacyDeviceKey() {
  if (!mayapDeviceUsingLegacySecret()) return true;
  char newKey[65];
  mayapGenerateDeviceSecret(newKey);
  JsonDocument doc;
  doc["device_id"] = mayapDeviceIdText();
  doc["device_key"] = mayapDeviceSecret();
  doc["new_device_key"] = newKey;
  if (!postJson("/api/device/rotate-key", doc, "rotate-key")) return false;
  return mayapCommitDeviceSecret(newKey);
}

inline bool sendRegister() {
  mayapSetProvisioningState(MayapProvisioningState::Syncing);
  JsonDocument doc;
  doc["device_id"] = mayapDeviceIdText();
  doc["device_key"] = mayapDeviceSecret();
  doc["device_name"] = mayapDeviceIdText();
  String response;
  int code = 0;
  if (!postJson("/api/device/register", doc, "register", &response, &code,outboxCount!=0U)) {
    if (code == 403) {
      mayapSetProvisioningState(MayapProvisioningState::ServerDenied);
    } else if (code == 401) {
      mayapSetProvisioningState(MayapProvisioningState::KeyMismatch);
    } else if (code != 0) {
      mayapSetProvisioningState(MayapProvisioningState::CloudError);
    }
    return false;
  }
  storeProvisioningFromResponse(response);
  return rotateLegacyDeviceKey();
}

// Tao PIN web ngau nhien moi. PIN duoc tra ve qua TLS, luu vao NVS va hien
// tren man Thong tin ket noi; khong con quay ve gia tri mac dinh.
inline bool sendResetPin() {
  JsonDocument doc;
  doc["device_id"] = mayapDeviceIdText();
  doc["device_key"] = mayapDeviceSecret();
  String response;
  int code = 0;
  if (!postJson("/api/device/reset-pin", doc, "reset-pin", &response, &code)) {
    if (code == 403) {
      mayapSetProvisioningState(MayapProvisioningState::ServerDenied);
    } else if (code == 401) {
      mayapSetProvisioningState(MayapProvisioningState::KeyMismatch);
    } else if (code != 0) {
      mayapSetProvisioningState(MayapProvisioningState::CloudError);
    }
    return false;
  }
  storeProvisioningFromResponse(response);
  return true;
}

inline bool sendHeartbeat() {
  JsonDocument doc;
  doc["device_id"] = mayapDeviceIdText();
  doc["device_key"] = mayapDeviceSecret();
  // Worker dung co nay de quyet dinh co bao "mat ket noi" hay khong - chi bao
  // khi dang co me ap chay tai lan heartbeat gan nhat (xem checkDeviceConnectivity
  // trong cloudflare/src/index.js). processingRuntime duoc lam moi moi chu ky
  // kiem tra (CLOUD_CHECK_INTERVAL_MS), du moi cho heartbeat moi 30s.
  doc["batch_running"] = processingRuntime.batchRunning;
  return postJson("/api/device/heartbeat", doc, "heartbeat");
}

inline bool sendAlarm(OutboxItem &item) {
  JsonDocument doc;
  doc["device_id"] = mayapDeviceIdText();
  doc["device_key"] = mayapDeviceSecret();
  doc["alarm_type"] = item.alarmType;
  doc["severity"] = severityText(item.severity);
  doc["state"] = item.resolved ? "resolved" : "active";
  doc["message"] = item.message;
  if (item.hasReadings) {
    doc["temperature"] = item.temperature;
    doc["humidity"] = item.humidity;
  }
  char eventId[40];
  snprintf(eventId,sizeof(eventId),"%08lx%08lx-%08lx",static_cast<unsigned long>(eventBootHigh),
      static_cast<unsigned long>(eventBootLow),static_cast<unsigned long>(item.sequence));
  doc["event_id"]=eventId;doc["detected_uptime_ms"]=item.detectedAt;
  String response;
  const bool sent=postJson("/api/device/alarm",doc,"alarm",&response,nullptr,true);
  item.attempted=item.attempted || !requestDeferred;
  if(!sent) return false;
  const bool accepted=mayapDurableAlarmReceipt(response.c_str(),eventId);
  mayapSerialPrintf(false,"[CLOUD] event=%s age=%lums durable_ack=%u\n",eventId,
      static_cast<unsigned long>(elapsedMs(millis(),item.detectedAt)),accepted?1U:0U);
  return accepted;
}

inline void drainOutbox(uint32_t now) {
  if(outboxCount==0U) return;
  uint8_t selected=0U;
  // A critical edge may pass routine messages but never its own predecessor.
  for(uint8_t n=0;n<outboxCount;++n) {
    const OutboxItem &candidate=outbox[(outboxHead+n)%CLOUD_OUTBOX_SIZE];
    if(candidate.severity!=NotifyLevel::Critical || (candidate.attempted && !candidate.retry.ready(now))) continue;
    bool predecessor=false;
    for(uint8_t p=0;p<n;++p) if(!strcmp(outbox[(outboxHead+p)%CLOUD_OUTBOX_SIZE].alarmType,candidate.alarmType)){predecessor=true;break;}
    if(!predecessor){selected=n;break;}
  }
  OutboxItem &item=outbox[(outboxHead+selected)%CLOUD_OUTBOX_SIZE];
  if(item.severity!=NotifyLevel::Critical && !timeReached(now,lastSendAt+CLOUD_MIN_SEND_GAP_MS)) return;
  if(item.attempted && !item.retry.ready(now)) return;
  const NetworkStatus status=mayapGetRawNetworkStatus();
  if(!(status.requestedMode==ConnectivityMode::Online && status.connected)) return;
  const bool ok=sendAlarm(item);
  if(!requestDeferred) lastSendAt=millis();
  if(ok) {
    item.retry.onSuccess();
    for(uint8_t n=selected;n+1U<outboxCount;++n)
      outbox[(outboxHead+n)%CLOUD_OUTBOX_SIZE]=outbox[(outboxHead+n+1U)%CLOUD_OUTBOX_SIZE];
    outboxTail=(outboxTail+CLOUD_OUTBOX_SIZE-1U)%CLOUD_OUTBOX_SIZE;
    outbox[outboxTail].used=false;--outboxCount;
  } else if(!requestDeferred) item.retry.onFailure(millis());
}

inline void serviceHeartbeat(uint32_t now) {
  if (!timeReached(now, lastHeartbeatAt + CLOUD_HEARTBEAT_INTERVAL_MS)) return;
  if (!cloudBackoff.ready(now)) return;
  const NetworkStatus status = mayapGetRawNetworkStatus();
  if (!(status.requestedMode == ConnectivityMode::Online && status.connected)) return;
  lastHeartbeatAt = now;
  if (sendHeartbeat()) {
    cloudBackoff.onSuccess();
  } else {
    if (!requestDeferred) cloudBackoff.onFailure(millis());
  }
}

inline void serviceRegister(uint32_t now) {
  if (registered) return;
  if (!cloudBackoff.ready(now)) return;
  const NetworkStatus status = mayapGetRawNetworkStatus();
  if (status.requestedMode != ConnectivityMode::Online) return;
  if (!status.connected) {
    mayapSetProvisioningState(MayapProvisioningState::CloudOffline);
    return;
  }
  if (sendRegister()) {
    registered = true;
    cloudBackoff.onSuccess();
  } else {
    if (!requestDeferred) cloudBackoff.onFailure(millis());
  }
}

inline void servicePinReset() {
  if (!__atomic_exchange_n(&pinResetRequestFlag, 0U, __ATOMIC_ACQ_REL)) return;
  const NetworkStatus netStatus = mayapGetRawNetworkStatus();
  if (netStatus.requestedMode != ConnectivityMode::Online || !netStatus.connected) {
    mayapSerialPrintf(false, "[CLOUD] reset-pin bi huy: khong online luc yeu cau\n");
    return;
  }
  if (!sendResetPin() && requestDeferred) {
    // No HTTP request was sent: retain the user's intent while admission is
    // busy. Never automatically replay a reset after an ambiguous HTTP error.
    __atomic_store_n(&pinResetRequestFlag, 1U, __ATOMIC_RELEASE);
  }
}

}  // namespace MayapCloudInternal

// ================================ API cong khai ================================

inline void mayapCloudAlertBegin() {
  MayapCloudInternal::eventBootHigh=esp_random();MayapCloudInternal::eventBootLow=esp_random();
  // Khong can khoi tao gi truoc: moi client HTTPS la ngan han, tao khi can goi.
}

// Called after an owner I/O operation has unwound its local HTTP/TLS session.
// Keep identity, PIN, event outbox and transaction data; retry registration
// through the existing backoff rather than wiping provisioning state.
inline void mayapCloudRecover(uint32_t now) {
  MayapCloudInternal::registered = false;
  MayapCloudInternal::cloudBackoff.onFailure(now);
}

// Goi tu controlTask (qua HmiCommandType::CloudPinReset, xem
// machine_control.h) khi nguoi lap dat xac nhan "Dat lai ma PIN" tren HMI.
// Chi dat co hieu cho networkTask - KHONG tu goi HTTPS o day (controlTask
// khong duoc phep block).
inline void mayapRequestCloudPinReset() {
  __atomic_store_n(&MayapCloudInternal::pinResetRequestFlag, 1U, __ATOMIC_RELEASE);
}

// Chi duoc goi tu networkTask (khong blocking task khac; ban than no CO the
// block chinh networkTask vai giay khi thuc su goi HTTPS, xem ghi chu dau file).
inline void mayapCloudAlertUpdate(uint32_t now) {
  using namespace MayapCloudInternal;

  if (!mayapDeviceSecret()[0] || !CLOUD_API_HOST[0]) {
    // Nguyen nhan PHO BIEN NHAT khien khong co canh bao nao duoc gui: worker
    // host/device_key la macro build-time trong config.h (MAYAP_CLOUD_API_HOST/
    // MAYAP_DEVICE_SECRET), chua duoc dat luc build. In canh bao ro rang, lap
    // lai dinh ky (khong lien tuc) de khong bi troi mat trong log nhung van
    // chac chan duoc nhin thay.
    static uint32_t lastConfigWarnAt = 0U;
    if (lastConfigWarnAt == 0U || MayapCloudInternal::timeReached(now, lastConfigWarnAt + 300000UL)) {
      lastConfigWarnAt = now;
      mayapSerialPrintf(false,
          "[CLOUD] CANH BAO: chua cau hinh MAYAP_CLOUD_API_HOST/MAYAP_DEVICE_SECRET "
          "trong firmware (config.h) - se KHONG gui duoc canh bao nao cho toi khi "
          "nguoi lap dat nap lai firmware voi cau hinh hop le.\n");
    }
    return;
  }

  static uint32_t lastCheckAt = 0U;
  // Goi ro namespace: bien ngoai "using namespace" dua ten nay vao ngang
  // hang voi timeReached() global cua hmi.h (khong bi che khuat nhu khi goi
  // tu BEN TRONG namespace), gay loi bien dich "goi ham mo ho" (ambiguous).
  if (MayapCloudInternal::timeReached(now, lastCheckAt + CLOUD_CHECK_INTERVAL_MS)) {
    lastCheckAt = now;
    portENTER_CRITICAL(&cloudMux);
    const bool valid = knownRuntimeValid;
    if (valid) processingRuntime = knownRuntime;
    const bool configValid = knownConfigValid;
    if (configValid) processingConfig = knownConfig;
    portEXIT_CRITICAL(&cloudMux);
    if (valid) {

      checkTransitions(now);
      if (configValid) {
        checkPowerRestored(now);
        checkLightAfterBatch(now);
        checkBatchSchedule(now);
        checkTurnCycleMissed(now);
      }
    }
    checkWifiSignal(now);
  }

  checkFaults(now);

  // Explicit user request gets first admission, not a permanently occupied
  // send gap left by routine heartbeat/alarm traffic.
  servicePinReset();
  serviceRegister(now);
  if (registered) {
    drainOutbox(now);
    if (outboxCount == 0U) serviceHeartbeat(now);
    if (outboxCount == 0U && !requestDeferred) mayapReleaseCloudTlsYield();
  }

}

// MachineController goi ham nay tu controlTask, cung noi/cung nhip voi
// mayapRealtimeSetRuntime() cua transaction_bridge.h (xem may_ap_industrial.ino/
// machine_control.h::copyRuntimeToHmi khu vuc goi hmiSetRuntime()).
inline void mayapCloudSetRuntime(const MachineRuntime &runtime) {
  using namespace MayapCloudInternal;
  portENTER_CRITICAL(&cloudMux);
  knownRuntime = runtime;
  knownRuntimeValid = true;
  portEXIT_CRITICAL(&cloudMux);
}

// Cung noi/cung nhip voi mayapRealtimeSetConfig() cua transaction_bridge.h - chi can
// cho checkLightAfterBatch() biet lightAfterBatchAlarmEnabled dang BAT/TAT.
inline void mayapCloudSetConfig(const MachineConfig &config) {
  using namespace MayapCloudInternal;
  portENTER_CRITICAL(&cloudMux);
  knownConfig = config;
  knownConfigValid = true;
  portEXIT_CRITICAL(&cloudMux);
}


// Trang thai SONG cua kenh Cloud Push (khac voi mayapPrintNetworkConfig() la
// cau hinh TINH) - dung cho lenh Serial CONFIG de debug day du: da dang ky
// voi Worker chua, hang doi con bao nhieu tin dang cho, backoff dang lui toi
// buoc may, lan gui/heartbeat gan nhat cach day bao lau. Goi ro namespace vi
// ham nay o pham vi global (xem ghi chu timeReached o mayapCloudAlertUpdate
// ben tren - cung ly do).
inline void mayapPrintCloudStatus(uint32_t now) {
  using namespace MayapCloudInternal;
  mayapSerialPrintf(false,
      "[CLOUD] host=%s device_key=%s da_dang_ky=%u outbox=%u/%u backoff_step=%u/%u\n",
      CLOUD_API_HOST[0] ? CLOUD_API_HOST : "(chua cau hinh)",
      mayapDeviceSecret()[0] ? "DA CAU HINH" : "CHUA CAU HINH",
      registered, static_cast<unsigned>(outboxCount), static_cast<unsigned>(CLOUD_OUTBOX_SIZE),
      static_cast<unsigned>(cloudBackoff.step), static_cast<unsigned>(BACKOFF_STEP_COUNT - 1U));
  const long sendAgoSec = lastSendAt == 0U
      ? -1L
      : static_cast<long>(MayapCloudInternal::elapsedMs(now, lastSendAt) / 1000U);
  const long heartbeatAgoSec = lastHeartbeatAt == 0U
      ? -1L
      : static_cast<long>(MayapCloudInternal::elapsedMs(now, lastHeartbeatAt) / 1000U);
  mayapSerialPrintf(false,
      "[CLOUD] lan_gui_gan_nhat=%lds_truoc lan_heartbeat_gan_nhat=%lds_truoc (-1 = chua tung)\n",
      sendAgoSec, heartbeatAgoSec);
}
