#pragma once

#include "config.h"
#include "adaptive_thermal_balance.h"
#include "adaptive_persistence.h"
#include "thermal_adaptive_v1.h"
#include "thermal_profile_storage.h"
#include "turn_schedule_policy.h"
#include "firmware_update_guard.h"
#include "boot_diagnostic.h"
#include "startup_output_policy.h"
#include "attiny_state_sync.h"
#include <Arduino.h>
#include <HardwareSerial.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp32-hal-rgb-led.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <algorithm>

#include "serial_diagnostics.h"

namespace Mayap {

static_assert(MayapAdaptive::Policy::CoolingMinMs == RELAY_VENT_MIN_ON_MS,
              "Smart cooling minimum ON must match vent relay protection");
static_assert(MayapAdaptive::Policy::CoolingMinMs == RELAY_VENT_MIN_OFF_MS,
              "Smart cooling minimum OFF must match vent relay protection");

// ============================================================================
// TIEN ICH CHUNG
// ============================================================================
inline uint32_t elapsedMs(uint32_t now, uint32_t then) {
  return static_cast<uint32_t>(now - then);
}
inline bool timeReached(uint32_t now, uint32_t target) {
  return static_cast<int32_t>(now - target) >= 0;
}
inline float clampFloat(float value, float low, float high) {
  if (!isfinite(value)) return low;
  if (value < low) return low;
  if (value > high) return high;
  return value;
}
inline uint32_t mcCrc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  while (length--) {
    crc ^= *data++;
    for (uint8_t i = 0; i < 8; ++i) {
      crc = (crc >> 1U) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
  }
  return ~crc;
}

inline void setCompileDate(char out[11]) {
  static const char *const months[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
  };
  char monthText[4] = {__DATE__[0], __DATE__[1], __DATE__[2], '\0'};
  uint8_t month = 1;
  for (uint8_t i = 0; i < 12; ++i) {
    if (strncmp(monthText, months[i], 3) == 0) { month = i + 1U; break; }
  }
  const uint8_t day = static_cast<uint8_t>(
      (__DATE__[4] == ' ' ? 0 : __DATE__[4] - '0') * 10 + (__DATE__[5] - '0'));
  const uint16_t year = static_cast<uint16_t>(
      (__DATE__[7]-'0')*1000 + (__DATE__[8]-'0')*100 +
      (__DATE__[9]-'0')*10 + (__DATE__[10]-'0'));
  snprintf(out, 11, "%02u/%02u/%04u", day, month, year);
}

inline const char *resetReasonText(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXTERNAL";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}

inline bool resetReasonIsPowerInterruption(esp_reset_reason_t reason) {
  return reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT ||
         reason == ESP_RST_UNKNOWN;
}

inline bool resetReasonIsAutomaticRecovery(esp_reset_reason_t reason) {
  return reason == ESP_RST_SW || reason == ESP_RST_EXT ||
         reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
         reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
}

// ============================================================================
// KERNEL NHO: SCHEDULER DINH KY + HANG DOI TINH, KHONG CAP PHAT DONG
// ============================================================================
class PeriodicGate {
 public:
  explicit PeriodicGate(uint32_t periodMs = 0U) : periodMs_(periodMs) {}
  void setPeriod(uint32_t periodMs) { periodMs_ = periodMs; }
  void reset(uint32_t now = 0U) { lastAt_ = now; initialized_ = false; }
  bool due(uint32_t now, bool immediateFirst = false) {
    if (!initialized_) {
      initialized_ = true;
      lastAt_ = now;
      return immediateFirst;
    }
    if (periodMs_ == 0U) return false;
    const uint32_t elapsed = elapsedMs(now, lastAt_);
    if (elapsed < periodMs_) return false;
    // Cap nhat bang phep chia de thoi gian thuc thi luon huu han, khong lap bu.
    lastAt_ += (elapsed / periodMs_) * periodMs_;
    return true;
  }
 private:
  uint32_t periodMs_ = 0U;
  uint32_t lastAt_ = 0U;
  bool initialized_ = false;
};

template <typename T, uint8_t Capacity>
class FixedRing {
 public:
  static_assert(Capacity > 1U, "FixedRing capacity phai > 1");
  bool push(const T &value) {
    if (count_ >= Capacity) {
      // Giu su kien moi nhat: bo phan tu cu nhat va tang bo dem overflow.
      tail_ = static_cast<uint8_t>((tail_ + 1U) % Capacity);
      --count_;
      if (overflow_ < UINT32_MAX) ++overflow_;
    }
    data_[head_] = value;
    head_ = static_cast<uint8_t>((head_ + 1U) % Capacity);
    ++count_;
    return true;
  }
  bool pop(T &out) {
    if (!count_) return false;
    out = data_[tail_];
    tail_ = static_cast<uint8_t>((tail_ + 1U) % Capacity);
    --count_;
    return true;
  }
  void clear() { head_ = tail_ = count_ = 0U; overflow_ = 0U; }
  uint8_t size() const { return count_; }
  uint32_t overflowCount() const { return overflow_; }
 private:
  T data_[Capacity]{};
  uint8_t head_ = 0U;
  uint8_t tail_ = 0U;
  uint8_t count_ = 0U;
  uint32_t overflow_ = 0U;
};

// ============================================================================
// EVENT LOG RAM: NHAT KY NHE, KHONG GHI FLASH NOI DINH KY
// ============================================================================
enum class EventType : uint8_t {
  Boot = 1, InputChanged, OutputChanged, FaultRaised, FaultCleared, FaultAck,
  BatchStart, BatchStop, ModeChanged, ConfigSaved, SensorLost, SensorRestored,
  AutoTuneStart, AutoTuneEnd, StorageError, Recovery, Turning, System, Network,
  Adaptive = 250
};

enum class EventCode : uint16_t {
  None = 0,
  BootPowerOn = 1, BootExternal, BootSoftware, BootPanic, BootWdt, BootBrownout,
  BatchStart = 20, BatchStop, BatchResume,
  ResumePrompt = 23, ResumeAccepted, ResumeRejected,
  ModeAuto = 30, ModeManual, AutoLostDuringBatch, AutoRestoredDuringBatch,
  SensorOnline = 40, SensorOffline, RtcReconnected, RtcAutoRepaired,
  RtcNtpSynced,
  ConfigSaved = 50, AutoTuneStarted, AutoTuneSuccess, AutoTuneFailed,
  StorageReconnected,
  // value (int16_t) mang so GIO da mat dien, xem adjustResumeElapsedFromRtc().
  PowerOutageDuration,
  TurnStartLeft = 60, TurnStartRight, TurnHomeLeft, TurnHomeRight,
  TurnCompleteLeft, TurnCompleteRight,
  NetworkModeOffline = 70, NetworkConnecting, NetworkConnected,
  NetworkDisconnected, NetworkConfigMissing,
  CommandRejected = 80,
  SirenSelfTest = 81,
  InputBase = 100,
  OutputBase = 200,
  FaultBase = 1000,
  AdaptiveChanged = 450, AdaptiveModelInvalidated, AdaptiveEnabled, AdaptiveDisabled,
  ThermalLearnChanged, ThermalModelMismatch
};

struct EventEntry {
  uint32_t sequence = 0U;
  uint32_t atMs = 0U;
  uint32_t epoch = 0U;
  uint16_t code = 0U;
  int16_t value = 0;
  uint8_t type = 0U;
  uint8_t flags = 0U;
};

class EventLog {
 public:
  using PersistSinkFn = void (*)(const EventEntry &entry, void *context);

  void begin(uint32_t) {}
  void attachPersistSink(PersistSinkFn sink, void *context) {
    persistSink_ = sink;
    persistContext_ = context;
  }

  // Dong bo moc thoi gian DS3231 voi millis(). Khi RTC vua hop le, cac su kien
  // da phat sinh trong lan khoi dong hien tai duoc bo sung timestamp theo tuoi.
  void syncClock(uint32_t now, uint32_t epoch) {
    if (epoch == 0U) return;
    if (clockEpoch_ == 0U) {
      for (uint8_t offset = 0U; offset < count_; ++offset) {
        const uint8_t index = static_cast<uint8_t>(
            (head_ + EVENT_LOG_RAM_SIZE - 1U - offset) % EVENT_LOG_RAM_SIZE);
        EventEntry &entry = entries_[index];
        if (entry.epoch == 0U) {
          const uint32_t ageSec = elapsedMs(now, entry.atMs) / 1000UL;
          entry.epoch = epoch > ageSec ? epoch - ageSec : epoch;
        }
      }
    }
    clockEpoch_ = epoch;
    clockSyncedAtMs_ = now;
  }

  uint32_t estimatedEpoch(uint32_t now) const {
    if (clockEpoch_ == 0U) return 0U;
    return clockEpoch_ + elapsedMs(now, clockSyncedAtMs_) / 1000UL;
  }

  // Nhat ky chi co y nghia trong pham vi mot me. Ngoai me chi giu Boot va AutoTune start/end; khong ghi su kien thuong - tranh nhieu su kien khong
  // lien quan lam loang lich su thao tac cua me that su.
  void setLoggingEnabled(bool enabled) { loggingEnabled_ = enabled; }
  bool loggingEnabled() const { return loggingEnabled_; }

  void push(uint32_t now, EventType type, uint16_t code,
            int16_t value = 0, uint8_t flags = 0U) {
    // Boot (ly do khoi dong lai: PANIC/WATCHDOG/BROWNOUT/...) LUON duoc ghi
    // bat ke logging co dang bat hay khong - day la thong tin chan doan quan
    // trong ca khi may dang RANH (ngoai me), va thuc te BI GHI TRUOC khi
    // setLoggingEnabled() lan dau duoc goi trong begin() (power_.begin() chay
    // som hon, luc loggingEnabled_ con mac dinh false) - neu khong co ngoai
    // le nay, su kien Boot se LUON bi mat ngay tu dau, khong bao gio thay
    // duoc ly do reset that su tren man Nhat Ky.
    // Retain bounded commissioning transitions outside a batch: Boot,
    // AutoTune start/end and Adaptive. Ordinary network/I/O remains disabled.
    if (!loggingEnabled_ && type != EventType::Boot &&
        type != EventType::AutoTuneStart && type != EventType::AutoTuneEnd &&
        type != EventType::Adaptive) return;
    EventEntry &entry = entries_[head_];
    entry.sequence = ++sequence_;
    entry.atMs = now;
    entry.epoch = estimatedEpoch(now);
    entry.code = code;
    entry.value = value;
    entry.type = static_cast<uint8_t>(type);
    entry.flags = flags;
    head_ = static_cast<uint8_t>((head_ + 1U) % EVENT_LOG_RAM_SIZE);
    if (count_ < EVENT_LOG_RAM_SIZE) ++count_;
    if (persistSink_) persistSink_(entry, persistContext_);
  }
  bool clear(uint32_t) { head_ = count_ = 0U; ++sequence_; return true; }
  uint32_t sequence() const { return sequence_; }
  void print(uint8_t maximum = 20U) const {
    const uint8_t n = std::min<uint8_t>(maximum, count_);
    mayapSerialPrintf(false, "--- EVENT LOG count=%u ---\n", n);
    for (uint8_t i = 0U; i < n; ++i) {
      const uint8_t index = static_cast<uint8_t>(
          (head_ + EVENT_LOG_RAM_SIZE - n + i) % EVENT_LOG_RAM_SIZE);
      const EventEntry &e = entries_[index];
      mayapSerialPrintf(false, "#%lu t=%lums epoch=%lu type=%u code=%u value=%d flags=%u\n",
          static_cast<unsigned long>(e.sequence),
          static_cast<unsigned long>(e.atMs),
          static_cast<unsigned long>(e.epoch),
          e.type, e.code, e.value, e.flags);
    }
  }

  void snapshotRecent(uint32_t now, HmiEventSnapshot &out) const {
    out = HmiEventSnapshot{};
    out.sourceSequence = sequence_;
    const uint8_t available = std::min<uint8_t>(count_, HMI_EVENT_DISPLAY_CAPACITY);
    for (uint8_t offset = 0U; offset < available; ++offset) {
      const uint8_t index = static_cast<uint8_t>(
          (head_ + EVENT_LOG_RAM_SIZE - 1U - offset) % EVENT_LOG_RAM_SIZE);
      const EventEntry &e = entries_[index];
      HmiEventItem &dst = out.items[offset];
      dst.sequence = e.sequence;
      dst.epoch = e.epoch;
      dst.ageSec = elapsedMs(now, e.atMs) / 1000UL;
      dst.code = e.code;
      dst.value = e.value;
      dst.type = e.type;
      dst.flags = e.flags;
    }
    out.count = available;
    out.totalInWindow = count_;
  }
 private:
  EventEntry entries_[EVENT_LOG_RAM_SIZE]{};
  uint8_t head_ = 0U;
  uint8_t count_ = 0U;
  uint32_t sequence_ = 0U;
  uint32_t clockEpoch_ = 0U;
  uint32_t clockSyncedAtMs_ = 0U;
  PersistSinkFn persistSink_ = nullptr;
  void *persistContext_ = nullptr;
  bool loggingEnabled_ = false;
};

// ============================================================================
// NHAT KY FLASH TAM VO HIEU HOA O BAN RECOVERY.
//
// Ly do: filesystem/flash I/O tuyet doi khong duoc nam tren duong real-time
// cua task dieu khien. Ban nay giu API no-op de interlock/HMI chay nhu cu,
// nhung KHONG mount/format/ghi flash va KHONG thay doi partition table.
// Khi logger bat dong bo da duoc bench-test se bat lai o ban sau.
// ============================================================================
class BatchLogger {
 public:
  bool begin() { active_ = false; return true; }
  bool start(uint32_t, bool) { active_ = true; return true; }
  void stop() { active_ = false; }
  bool ready() const { return false; }
  bool active() const { return active_; }
  bool healthy() const { return true; }
  uint8_t queued() const { return 0U; }
  uint32_t batchStartEpoch() const { return 0U; }
  const char *path() const { return ""; }
  void printFiles() const {
    mayapSerialPrintf(false, "[BLOG] DISABLED IN RECOVERY BUILD\n");
  }
  bool appendEvent(const EventEntry &) { return true; }
  bool appendSample(uint32_t, uint32_t, float, float, float, uint16_t, uint8_t) { return true; }
  bool service(uint8_t = 2U) { return true; }
  static void eventSink(const EventEntry &, void *) {}
 private:
  bool active_ = false;
};

// ============================================================================
// FAULT MANAGER: MOT NOI DUY NHAT CHUYEN LOI -> ALARM/HEAT INHIBIT/LOG
// ============================================================================
enum class FaultSeverity : uint8_t { Info = 0, Warning = 1, Stop = 2, Emergency = 3 };
enum class FaultCode : uint16_t {
  None = 0,
  SensorLost = 101,
  SensorInvalid = 102,
  SensorSuspect = 103,
  // F-08 (audit truoc phat hanh v3.7.1): cam bien "dung hinh" o mot gia tri
  // hop le (khong loi CRC, khong mat tin hieu) - xem processSensor().
  SensorFrozen = 104,
  LowTemperature = 110,
  HighTemperature = 111,
  EmergencyTemperature = 112,
  TemperatureRateExceeded = 113,
  TemperatureUnstable = 114,
  HeaterNotHeating = 115,
  HumidityLow = 120,
  HumidityHigh = 121,
  HeaterSwitchOffDuringBatch = 130,
  // 131 (quat tuan hoan tat giua me) da bo: quat tuan hoan la bat buoc va duoc
  // ep bat cung nao me con chay, khong con phu thuoc cong tac tay nua.
  ResumeRequiresAuto = 132,
  AutoModeOffDuringBatch = 133,
  AutoTurningDisabledDuringBatch = 134,
  // Man hinh "Ap lai me cu" sau mat dien cho xac nhan qua lau khong ai thao tac.
  ResumeConfirmationPending = 135,
  // Me da qua so ngay ap du kien (config.totalIncubationDays) ma chua ket
  // thuc - canh bao tai cho tren HMI/web, doc lap voi thong bao Cloud Push
  // (cloud_alert_link.h) de van thay duoc ke ca khi may khong co mang.
  BatchOverdue = 136,
  // Cho phuc hoi me ma RTC khong hop le qua lau (RESUME_RTC_WAIT_ALERT_MS) -
  // khac ResumeConfirmationPending (co nguoi bam duoc), truong hop nay may
  // KHONG TU LAM GI DUOC, chi biet cho RTC song lai (auto-repair/NTP).
  ResumeRtcWaitTooLong = 137,
  // F-06 (audit truoc phat hanh v3.7.1): khac BatchOverdue (136, chi canh
  // bao tai cho, khong lam gi ca) - ma nay CHU DONG doi hoi xac nhan: bat coi
  // (dung chung sirenMutedUntil_/AlarmAck) va TU DONG DUNG ME neu qua
  // BATCH_OVERDUE_AUTO_STOP_GRACE_SEC ke tu ngay du du kien ma khong ai
  // ACK/dung me thu cong. Xem updateBatchOverdue().
  BatchOverdueConfirmationPending = 138,
  TurnLimitConflict = 201,
  TurnTimeout = 202,
  TurnLimitStuck = 203,
  TurnCommandConflict = 204,
  // Loi dao lap lai qua TURN_FAULT_STREAK_LIMIT lan lien tiep khong co lan
  // dao thanh cong xen giua - nghi ngo hong co khi, khoa dao hoan toan cho
  // toi khi xac nhan lai qua Test Mode (ca 2 cong tac hanh trinh deu Success).
  TurnMechanicalCheckRequired = 205,
  StorageUnavailable = 301,
  StorageDegraded = 302,
  AbnormalReset = 303,
  OutputConflict = 304,
  RelayRateExceeded = 305,
  RtcFailure = 306,
  BatchStateClearPending = 313,
  SafetyJournalUnavailable = 314,
  BatchLogUnavailable = 315,
  // Nhom 400: GIAM SAT SUC KHOE HE THONG (v3.6.0) - canh bao DU DOAN SOM,
  // khong phai loi an toan tuc thi. Xem serviceHealthMonitor().
  HeapLow = 401,
  HeapCritical = 402,
  TemperatureTrendWarning = 403,
  StorageRetryTrend = 404,
  // Nhom 500: bao mat dien qua ATtiny13A (mach doc lap dung pin CR2032, xem
  // doc/attiny_power_alarm.md) - CHI CANH BAO CHAN DOAN, khong anh huong
  // dieu khien nhiet/dao (dropHeatMaster/inhibitSsr deu false).
  AttinyBusUnresponsive = 501,  // mat ACK/status - kiem tra CR2032/day BUS
  SirenBatteryLow = 502,         // status Tiny bao nguon 9V cap coi LOW
  AttinyStateUnsynced = 503      // link song nhung batch ESP/Tiny khong khop
};

// So luong gia tri FaultCode THUC (khong tinh None) - dem tay tu enum o tren.
// CAP NHAT gia tri nay moi khi them/bot mot ma loi, static_assert canh
// FaultManager::MAX_FAULTS se bao loi bien dich neu quen (xem F-02: truoc day
// co 36 ma loi thuc nhung MAX_FAULTS chi la 32, lam FaultManager tran o va
// ghi de len nhau - vd 4 ma loi dao trung E201-E204 bi xoa khoi he thong canh
// bao chi sau ~30s).
constexpr uint8_t FAULT_CODE_REAL_COUNT = 41U;

struct FaultDescriptor {
  FaultCode code;
  FaultSeverity severity;
  uint8_t displayPriority;
  uint32_t alarmBit;
  bool latching;
  // Tach ro hai tang bao ve nhiet:
  // inhibitSsr: cam D1 nhung co the van giu contactor tong nhiet.
  // dropHeatMaster: bat buoc nha contactor tong nhiet va dong thoi cam D1.
  bool inhibitSsr;
  bool dropHeatMaster;
  bool inhibitsTurning;
  bool forceVentFan;
  bool forceCirculationFan;
  const char *text;
};

inline const FaultDescriptor &faultDescriptor(FaultCode code) {
  static const FaultDescriptor unknown{FaultCode::None, FaultSeverity::Info, 0U,
      AlarmNone, false, false, false, false, false, false, "NONE"};
  static const FaultDescriptor table[] = {
    // code, severity, priority, alarm, latch, inhibitSSR, dropMaster, stopTurn, forceVent, forceCirculation, text
    // v3.8.x: mat/sai/nghi ngo cam bien la STOP fault. Ca SSR va contactor
    // tong nhiet deu bi cat; cong tac HEATER vat ly chi la dieu kien cho
    // phep, khong duoc vuot qua fault. Day la lop software bo sung cho
    // thermostat co khi + cau chi nhiet doc lap ngoai firmware.
    {FaultCode::SensorLost, FaultSeverity::Stop, 235U, AlarmSensor, false, true, true, false, false, true, "SENSOR LOST"},
    {FaultCode::SensorInvalid, FaultSeverity::Stop, 230U, AlarmSensor, false, true, true, false, false, true, "SENSOR INVALID"},
    {FaultCode::SensorSuspect, FaultSeverity::Stop, 225U, AlarmSensor, false, true, true, false, false, true, "SENSOR SUSPECT"},
    // F-08: canh bao THUAN CHAN DOAN - gia tri cam bien "dung hinh" (khong
    // SensorFrozen chi active khi PV dung gia du lau VA heater da tich luy
    // ON-time dang ke trong cung cua so. Khi da co bang chung nay, fault
    // STOP cat ca SSR va contactor; o diem dat on dinh voi heater gan nhu
    // khong cap se khong bi false-trip.
    {FaultCode::SensorFrozen, FaultSeverity::Stop, 228U, AlarmSensor, true, true, true, false, true, true, "SENSOR FROZEN"},
    {FaultCode::LowTemperature, FaultSeverity::Warning, 55U, AlarmTempLow, false, false, false, false, false, false, "TEMP LOW"},
    // Nhiet cao: cam SSR, nha contactor tong, bat ca hai quat.
    {FaultCode::HighTemperature, FaultSeverity::Stop, 240U, AlarmTempHigh, false, true, true, true, true, true, "TEMP HIGH"},
    // Khan cap: cam SSR va nha contactor tong ngay.
    {FaultCode::EmergencyTemperature, FaultSeverity::Emergency, 255U, AlarmEmergency, false, true, true, true, true, true, "TEMP EMERGENCY"},
    {FaultCode::HumidityLow, FaultSeverity::Warning, 40U, AlarmHumidityLow, false, false, false, false, false, false, "HUM LOW"},
    {FaultCode::HumidityHigh, FaultSeverity::Warning, 40U, AlarmHumidityLow, false, false, false, false, false, false, "HUM HIGH"},
    // 3 loi bo sung duoi day la CANH BAO SOM/CHAN DOAN, khong phai cat an
    // toan cung nhu Low/HighTemperature - khong inhibit SSR/master, chi bao.
    {FaultCode::TemperatureRateExceeded, FaultSeverity::Warning, 60U, AlarmTempHigh, false, false, false, false, false, false, "TEMP RATE HIGH"},
    {FaultCode::TemperatureUnstable, FaultSeverity::Warning, 45U, AlarmTempHigh, false, false, false, false, false, false, "TEMP UNSTABLE"},
    {FaultCode::HeaterNotHeating, FaultSeverity::Stop, 226U, AlarmSystem, true, true, true, false, true, true, "HEATER NOT HEATING"},
    // Mat mot chuc nang thiet yeu trong luc me dang chay phai bao ngay.
    {FaultCode::HeaterSwitchOffDuringBatch, FaultSeverity::Stop, 222U, AlarmSystem, false, true, true, false, false, false, "HEATER SWITCH OFF"},
    {FaultCode::ResumeRequiresAuto, FaultSeverity::Warning, 180U, AlarmSystem, false, false, false, false, false, false, "RESUME NEEDS AUTO"},
    // Cho xac nhan "Ap lai me cu" qua lau khong ai thao tac - chi canh bao,
    // KHONG tu dong lam gi thay nguoi dung (van cho dung y do an toan cua
    // resumeConfirmationRequired_). Tu het khi da xac nhan CO/HUY.
    {FaultCode::ResumeConfirmationPending, FaultSeverity::Warning, 175U, AlarmSystem, false, false, false, false, false, false, "RESUME PENDING TOO LONG"},
    // Qua so ngay ap du kien - chi canh bao (khong khoa gi), doc lap voi
    // thong bao Cloud Push tuong ung trong cloud_alert_link.h.
    {FaultCode::BatchOverdue, FaultSeverity::Warning, 50U, AlarmSystem, false, false, false, false, false, false, "BATCH OVERDUE"},
    // Chi canh bao (khong khoa gi them - ResumeBlockReason::Rtc da tu chan
    // resume roi), muc dich la de nguoi dung/quan tri BIET may dang treo cho
    // vi sao thay vi im lang khong ro ly do.
    {FaultCode::ResumeRtcWaitTooLong, FaultSeverity::Warning, 176U, AlarmSystem, false, false, false, false, false, false, "RESUME WAIT RTC"},
    // F-06: qua ngay du kien ma chua ai dung/xac nhan me - bat coi (dung
    // chung sirenMutedUntil_) va se TU DONG DUNG ME sau 12h neu khong ai
    // ACK/thao tac gi (xem updateBatchOverdue()). Khong khoa nhiet/dao (van
    // giu am binh thuong trong luc cho quyet dinh).
    {FaultCode::BatchOverdueConfirmationPending, FaultSeverity::Stop, 177U, AlarmSystem, false, false, false, false, false, false, "BATCH OVERDUE CONFIRM"},
    // AUTO bi tat giua me dang chay: van giu dieu khien nhiet nhu cu. Theo
    // yeu cau nguoi lap dat, KHONG con khoa dao tay nua (inhibitsTurning=
    // false) - cong tac dao trai/phai hoat dong binh thuong nhu ngoai me,
    // ma nay chi con la CANH BAO nhac gat lai AUTO (xem updateTurning() -
    // block khoa dao rieng cho truong hop nay da bo, chi con giu khoa cho
    // resumePending_ khi CHUA xac nhan ap lai).
    {FaultCode::AutoModeOffDuringBatch, FaultSeverity::Stop, 220U, AlarmAutoMode, false, false, false, false, false, false, "AUTO OFF DURING BATCH"},
    {FaultCode::AutoTurningDisabledDuringBatch, FaultSeverity::Stop, 219U, AlarmTurning, false, false, false, true, false, false, "AUTO TURN DISABLED"},
    {FaultCode::TurnLimitConflict, FaultSeverity::Stop, 160U, AlarmTurning, true, false, false, true, false, false, "LIMIT CONFLICT"},
    {FaultCode::TurnTimeout, FaultSeverity::Stop, 150U, AlarmTurning, true, false, false, true, false, false, "TURN TIMEOUT"},
    {FaultCode::TurnLimitStuck, FaultSeverity::Stop, 155U, AlarmTurning, true, false, false, true, false, false, "LIMIT STUCK"},
    {FaultCode::TurnCommandConflict, FaultSeverity::Stop, 145U, AlarmTurning, true, false, false, true, false, false, "TURN CMD CONFLICT"},
    // Loi dao lap lai >= TURN_FAULT_STREAK_LIMIT lan lien tiep (xem
    // latchTurnFault()) - khoa dao HOAN TOAN, khong tu clear qua ACK thuong
    // (latching=false NHUNG dieu kien chi tat khi turnMechanicalCheckRequired_
    // tat, ma bien do chi tat trong updateTestMode() khi ca 2 CTHT deu xac
    // nhan Success trong Test Mode - xem ghi chu tai latchTurnFault()).
    {FaultCode::TurnMechanicalCheckRequired, FaultSeverity::Stop, 165U, AlarmTurning, false, false, false, true, false, false, "TURN MECHANICAL CHECK"},
    // F-03: dropHeatMaster=false (xem ghi chu day du canh SensorLost o tren) -
    // ap dung cho ca 4 dong duoi day.
    {FaultCode::StorageUnavailable, FaultSeverity::Stop, 205U, AlarmSystem, true, true, false, false, false, false, "EEPROM UNAVAILABLE"},
    {FaultCode::StorageDegraded, FaultSeverity::Warning, 90U, AlarmSystem, false, false, false, false, false, false, "EEPROM DEGRADED"},
    {FaultCode::AbnormalReset, FaultSeverity::Stop, 210U, AlarmSystem, true, true, false, false, false, false, "ABNORMAL RESET"},
    {FaultCode::OutputConflict, FaultSeverity::Emergency, 250U, AlarmSystem, true, true, false, true, false, false, "OUTPUT CONFLICT"},
    {FaultCode::RelayRateExceeded, FaultSeverity::Warning, 70U, AlarmSystem, true, false, false, false, false, false, "RELAY RATE"},
    {FaultCode::RtcFailure, FaultSeverity::Warning, 80U, AlarmSystem, false, false, false, false, false, false, "RTC FAILURE"},
    // Da dung/huy me nhung ban ghi wasRunning=0 chua duoc EEPROM xac nhan.
    // F-03: dropHeatMaster=false (xem ghi chu day du canh SensorLost o tren).
    {FaultCode::BatchStateClearPending, FaultSeverity::Stop, 212U, AlarmSystem, false, true, false, false, false, false, "BATCH CLEAR PENDING"},
    // NVS noi bo chi lam tombstone an toan va dem reset. Mat no thi cam
    // bat dau/phuc hoi de tranh tu khoi dong lai mot me da duoc dung.
    {FaultCode::SafetyJournalUnavailable, FaultSeverity::Stop, 214U, AlarmSystem, true, true, false, false, false, false, "SAFETY JOURNAL FAIL"},
    // Mat log khong duoc lam dung gia nhiet/dao cua me, nhung phai canh bao ro.
    {FaultCode::BatchLogUnavailable, FaultSeverity::Warning, 75U, AlarmSystem, false, false, false, false, false, false, "BATCH LOG UNAVAILABLE"},
    // Nhom 400: canh bao DU DOAN SOM cua serviceHealthMonitor() - CHU DICH
    // khong dat inhibitSsr/dropHeatMaster/inhibitsTurning nao: day la canh
    // bao he thong/phan mem, khong phai su co nhiet/co khi that su, khong
    // duoc phep tu y cat nhiet/khoa dao chi vi RAM thap hay du doan xu huong.
    // latching=false: tu het khi xu huong tro lai binh thuong (co tre/
    // hysteresis rieng trong logic tinh, xem serviceHealthMonitor()).
    {FaultCode::HeapLow, FaultSeverity::Warning, 30U, AlarmSystem, false, false, false, false, false, false, "HEAP LOW"},
    // HeapCritical sheds Online workloads; local control never restarts for RAM pressure.
    {FaultCode::HeapCritical, FaultSeverity::Warning, 35U, AlarmSystem, false, false, false, false, false, false, "HEAP CRITICAL"},
    {FaultCode::TemperatureTrendWarning, FaultSeverity::Warning, 48U, AlarmTempHigh, false, false, false, false, false, false, "TEMP TREND WARNING"},
    {FaultCode::StorageRetryTrend, FaultSeverity::Warning, 72U, AlarmSystem, false, false, false, false, false, false, "STORAGE RETRY TREND"},
    // Nhom 500: bao mat dien qua ATtiny13A - chi chan doan, khong dropHeatMaster/
    // inhibitSsr/inhibitsTurning (mach nay hoan toan tach biet dieu khien chinh).
    {FaultCode::AttinyBusUnresponsive, FaultSeverity::Warning, 73U, AlarmSystem, false, false, false, false, false, false, "ATTINY BUS UNRESPONSIVE"},
    {FaultCode::SirenBatteryLow, FaultSeverity::Warning, 40U, AlarmSystem, false, false, false, false, false, false, "SIREN BATTERY LOW"},
    {FaultCode::AttinyStateUnsynced, FaultSeverity::Warning, 74U, AlarmSystem, false, false, false, false, false, false, "ATTINY STATE UNSYNC"}
  };
  for (const auto &item : table) if (item.code == code) return item;
  return unknown;
}

struct FaultState {
  FaultCode code = FaultCode::None;
  bool condition = false;
  bool active = false;
  bool acknowledged = false;
  uint32_t firstAt = 0U;
  uint32_t lastAt = 0U;
  uint16_t occurrences = 0U;
  int16_t detail = 0;
};

class FaultManager {
 public:
  explicit FaultManager(EventLog *log = nullptr) : log_(log) {}
  void attachLog(EventLog *log) { log_ = log; }

  void set(FaultCode code, bool condition, uint32_t now, int16_t detail = 0) {
    if (code == FaultCode::None) return;
    FaultState &state = slot(code);
    const FaultDescriptor &desc = faultDescriptor(code);
    if (condition) {
#ifdef MAYAP_CLOUD_FAULT_EVENTS
      if (!state.condition) mayapCloudRecordFault(static_cast<uint16_t>(code),static_cast<uint8_t>(desc.severity),true,now);
#endif
      state.condition = true;
      state.detail = detail;
      state.lastAt = now;
      if (!state.active) {
        state.active = true;
        state.acknowledged = false;
        state.firstAt = now;
        if (state.occurrences < UINT16_MAX) ++state.occurrences;
        ++notificationSequence_;
        lastRaisedCode_ = code;
        lastRaisedSeverity_ = desc.severity;
        if (log_) log_->push(now, EventType::FaultRaised,
            static_cast<uint16_t>(EventCode::FaultBase) + static_cast<uint16_t>(code),
            detail, static_cast<uint8_t>(desc.severity));
        mayapSerialPrintf(false, "[FAULT] SET %u %s detail=%d\n",
                         static_cast<unsigned>(code), desc.text, detail);
      }
      return;
    }

#ifdef MAYAP_CLOUD_FAULT_EVENTS
    if (state.condition) mayapCloudRecordFault(static_cast<uint16_t>(code),static_cast<uint8_t>(desc.severity),false,now);
#endif
    state.condition = false;
    if (!state.active) return;
    if (desc.latching && !state.acknowledged) return;
    clearState(state, now);
  }

  bool acknowledge(uint32_t now, uint32_t alarmMask = ALARM_KNOWN_MASK) {
    bool changed = false;
    for (uint8_t i = 0; i < count_; ++i) {
      FaultState &state = states_[i];
      if (!state.active) continue;
      const FaultDescriptor &desc = faultDescriptor(state.code);
      if (!(desc.alarmBit & alarmMask)) continue;
      state.acknowledged = true;
      changed = true;
      if (log_) log_->push(now, EventType::FaultAck,
          static_cast<uint16_t>(EventCode::FaultBase) + static_cast<uint16_t>(state.code),
          state.detail, static_cast<uint8_t>(desc.severity));
      if (!state.condition) clearState(state, now);
    }
    return changed;
  }

  // Xoa mot loi latched khi chinh thiet bi da duoc kiem tra va phuc hoi.
  // Khong dung cho loi an toan nhiet/dao; chi goi tu logic reconnect RTC/EEPROM.
  bool clearRecovered(FaultCode code, uint32_t now) {
    FaultState *state = find(code);
    if (!state || !state->active) return false;
#ifdef MAYAP_CLOUD_FAULT_EVENTS
    if (state->condition) mayapCloudRecordFault(static_cast<uint16_t>(code),static_cast<uint8_t>(faultDescriptor(code).severity),false,now);
#endif
    state->condition = false;
    state->acknowledged = true;
    clearState(*state, now);
    return true;
  }

  bool active(FaultCode code) const {
    const FaultState *state = findConst(code);
    return state && state->active;
  }
  uint32_t alarmMask() const {
    uint32_t mask = AlarmNone;
    for (uint8_t i = 0; i < count_; ++i) {
      if (states_[i].active) mask |= faultDescriptor(states_[i].code).alarmBit;
    }
    return mask;
  }
  bool ssrInhibited() const {
    for (uint8_t i = 0; i < count_; ++i) {
      if (states_[i].active && faultDescriptor(states_[i].code).inhibitSsr) return true;
    }
    return false;
  }
  bool masterDropRequired() const {
    for (uint8_t i = 0; i < count_; ++i) {
      if (states_[i].active && faultDescriptor(states_[i].code).dropHeatMaster) return true;
    }
    return false;
  }
  bool ventForced() const {
    for (uint8_t i = 0; i < count_; ++i) {
      if (states_[i].active && faultDescriptor(states_[i].code).forceVentFan) return true;
    }
    return false;
  }
  bool circulationForced() const {
    for (uint8_t i = 0; i < count_; ++i) {
      if (states_[i].active && faultDescriptor(states_[i].code).forceCirculationFan) return true;
    }
    return false;
  }
  bool turningInhibited() const {
    for (uint8_t i = 0; i < count_; ++i) {
      if (states_[i].active && faultDescriptor(states_[i].code).inhibitsTurning) return true;
    }
    return false;
  }
  FaultCode primary() const {
    FaultCode best = FaultCode::None;
    uint8_t priority = 0U;
    for (uint8_t i = 0; i < count_; ++i) {
      if (!states_[i].active) continue;
      const uint8_t candidate = faultDescriptor(states_[i].code).displayPriority;
      if (best == FaultCode::None || candidate > priority) {
        best = states_[i].code;
        priority = candidate;
      }
    }
    return best;
  }
  uint8_t activeCount() const {
    uint8_t active = 0U;
    for (uint8_t i = 0; i < count_; ++i) if (states_[i].active) ++active;
    return active;
  }
  void print() const {
    mayapSerialPrintf(false, "--- FAULTS active=%u ---\n", activeCount());
    for (uint8_t i = 0; i < count_; ++i) {
      const FaultState &state = states_[i];
      if (!state.active) continue;
      const FaultDescriptor &desc = faultDescriptor(state.code);
      mayapSerialPrintf(false, "code=%u sev=%u cond=%u ack=%u count=%u detail=%d %s\n",
          static_cast<unsigned>(state.code), static_cast<unsigned>(desc.severity),
          state.condition, state.acknowledged, state.occurrences,
          state.detail, desc.text);
    }
  }

  uint32_t notificationSequence() const { return notificationSequence_; }
  FaultCode lastRaisedCode() const { return lastRaisedCode_; }
  FaultSeverity lastRaisedSeverity() const { return lastRaisedSeverity_; }

  uint8_t copyActiveForHmi(HmiFaultItem *out, uint8_t capacity) const {
    if (!out || !capacity) return 0U;
    bool copied[MAX_FAULTS]{};
    uint8_t written = 0U;
    while (written < capacity) {
      int16_t bestIndex = -1;
      uint8_t bestPriority = 0U;
      for (uint8_t i = 0U; i < count_; ++i) {
        if (copied[i] || !states_[i].active) continue;
        const uint8_t candidate = faultDescriptor(states_[i].code).displayPriority;
        if (bestIndex < 0 || candidate > bestPriority) {
          bestIndex = static_cast<int16_t>(i);
          bestPriority = candidate;
        }
      }
      if (bestIndex < 0) break;
      const uint8_t index = static_cast<uint8_t>(bestIndex);
      copied[index] = true;
      const FaultState &state = states_[index];
      const FaultDescriptor &desc = faultDescriptor(state.code);
      HmiFaultItem &dst = out[written++];
      dst.code = static_cast<uint16_t>(state.code);
      dst.detail = state.detail;
      dst.severity = static_cast<uint8_t>(desc.severity);
      dst.flags = static_cast<uint8_t>((state.condition ? 0x01U : 0U) |
                                      (state.acknowledged ? 0x02U : 0U));
    }
    return written;
  }

 private:
  // FAULT_CODE_REAL_COUNT (khai bao canh enum FaultCode) la so ma loi thuc
  // hien co - MAX_FAULTS PHAI luon > gia tri do, static_assert ben duoi bao
  // loi bien dich neu ai them ma loi ma quen tang MAX_FAULTS (xem F-02: truoc
  // day co 36 ma loi thuc nhung MAX_FAULTS chi la 32 - moi ma loi deu duoc
  // set() moi nhip dieu khien/health nen bang day that su, va 4 ma loi dao
  // trung E201-E204 (khong bao gio duoc set() truc tiep, chi cap slot qua
  // latchTurnFault) la nhung ma duy nhat khong co slot rieng - bi slot() danh
  // de cho ma khac ghi de chi sau 1 nhip health (<=30s), xoa mat canh bao dao
  // trung dang active). De headroom ro rang cho ca ma loi tuong lai.
  static constexpr uint8_t MAX_FAULTS = 48U;
  static_assert(MAX_FAULTS > FAULT_CODE_REAL_COUNT,
                "MAX_FAULTS phai lon hon so ma FaultCode thuc - tang MAX_FAULTS "
                "(va cap nhat FAULT_CODE_REAL_COUNT) khi them ma loi moi");
  FaultState &slot(FaultCode code) {
    for (uint8_t i = 0; i < count_; ++i) if (states_[i].code == code) return states_[i];
    if (count_ < MAX_FAULTS) {
      states_[count_].code = code;
      return states_[count_++];
    }
    // Bang day (khong nen xay ra voi headroom hien tai): uu tien tim mot slot
    // KHONG active de tai su dung, tranh ghi de mat mot loi dang active that
    // su (day la nguyen nhan goc cua F-02). Chi khi moi slot deu dang active
    // (tinh huong cuc doan hon nua) moi danh doi dung slot cuoi nhu truoc.
    for (uint8_t i = 0; i < MAX_FAULTS; ++i) {
      if (!states_[i].active) { states_[i] = FaultState{}; states_[i].code = code; return states_[i]; }
    }
    states_[MAX_FAULTS - 1U].code = code;
    return states_[MAX_FAULTS - 1U];
  }
  FaultState *find(FaultCode code) {
    for (uint8_t i = 0; i < count_; ++i) if (states_[i].code == code) return &states_[i];
    return nullptr;
  }
  const FaultState *findConst(FaultCode code) const {
    for (uint8_t i = 0; i < count_; ++i) if (states_[i].code == code) return &states_[i];
    return nullptr;
  }
  void clearState(FaultState &state, uint32_t now) {
    const FaultDescriptor &desc = faultDescriptor(state.code);
    if (log_) log_->push(now, EventType::FaultCleared,
        static_cast<uint16_t>(EventCode::FaultBase) + static_cast<uint16_t>(state.code),
        state.detail, static_cast<uint8_t>(desc.severity));
    mayapSerialPrintf(false, "[FAULT] CLEAR %u %s\n",
                     static_cast<unsigned>(state.code), desc.text);
    state.active = false;
    state.acknowledged = false;
    state.lastAt = now;
  }

  FaultState states_[MAX_FAULTS]{};
  uint8_t count_ = 0U;
  EventLog *log_ = nullptr;
  uint32_t notificationSequence_ = 0U;
  FaultCode lastRaisedCode_ = FaultCode::None;
  FaultSeverity lastRaisedSeverity_ = FaultSeverity::Info;
};

// ============================================================================
// SAFETY JOURNAL + POWER MANAGER
// NVS noi bo chi luu 2 gia tri rat nho: stop-intent va bo dem reset.
// Cau hinh/trang thai me van nam tren AT24C32 ngoai.
// ============================================================================
constexpr uint32_t BATCH_STOP_INTENT_MAGIC = 0x42535450UL; // BSTP

class SafetyJournal {
 public:
  bool begin() {
    ready_ = prefs_.begin(SAFETY_NVS_NAMESPACE, false);
    return ready_;
  }
  bool ready() const { return ready_; }

  bool stopIntentPending() {
    return ready_ && prefs_.getUInt(SAFETY_NVS_STOP_KEY, 0U) ==
        BATCH_STOP_INTENT_MAGIC;
  }

  bool setStopIntent() {
    if (!ready_) return false;
    if (prefs_.putUInt(SAFETY_NVS_STOP_KEY, BATCH_STOP_INTENT_MAGIC) !=
        sizeof(uint32_t)) return false;
    return stopIntentPending();
  }

  bool clearStopIntent() {
    if (!ready_) return false;
    if (prefs_.putUInt(SAFETY_NVS_STOP_KEY, 0U) != sizeof(uint32_t))
      return false;
    return !stopIntentPending();
  }

  uint8_t resetCount() {
    return ready_ ? prefs_.getUChar(SAFETY_NVS_RESET_KEY, 0U) : 0U;
  }

  bool setResetCount(uint8_t value) {
    if (!ready_) return false;
    if (prefs_.putUChar(SAFETY_NVS_RESET_KEY, value) != sizeof(uint8_t))
      return false;
    return prefs_.getUChar(SAFETY_NVS_RESET_KEY, 0xFFU) == value;
  }

  // F-07: khoa "can kiem tra co khi dao" + so loi lien tiep - xem
  // latchTurnFault()/updateTestMode() trong MachineController. Song sot qua
  // reboot giong stop_intent, khac voi truoc day chi la bien RAM.
  bool turnMechanicalCheckRequired() {
    return ready_ && prefs_.getUChar(SAFETY_NVS_TURN_CHECK_KEY, 0U) != 0U;
  }

  bool setTurnMechanicalCheckRequired(bool value) {
    if (!ready_) return false;
    const uint8_t raw = value ? 1U : 0U;
    if (prefs_.putUChar(SAFETY_NVS_TURN_CHECK_KEY, raw) != sizeof(uint8_t))
      return false;
    return prefs_.getUChar(SAFETY_NVS_TURN_CHECK_KEY, 0xFFU) == raw;
  }

  uint8_t turnFaultStreak() {
    return ready_ ? prefs_.getUChar(SAFETY_NVS_TURN_STREAK_KEY, 0U) : 0U;
  }

  bool setTurnFaultStreak(uint8_t value) {
    if (!ready_) return false;
    if (prefs_.putUChar(SAFETY_NVS_TURN_STREAK_KEY, value) != sizeof(uint8_t))
      return false;
    return prefs_.getUChar(SAFETY_NVS_TURN_STREAK_KEY, 0xFFU) == value;
  }

  // F-06: nguoi van hanh da xac nhan "tiep tuc" cho me qua han HIEN TAI chua.
  bool batchOverdueAcknowledged() {
    return ready_ && prefs_.getUChar(SAFETY_NVS_OVERDUE_KEY, 0U) != 0U;
  }

  bool setBatchOverdueAcknowledged(bool value) {
    if (!ready_) return false;
    const uint8_t raw = value ? 1U : 0U;
    if (prefs_.putUChar(SAFETY_NVS_OVERDUE_KEY, raw) != sizeof(uint8_t))
      return false;
    return prefs_.getUChar(SAFETY_NVS_OVERDUE_KEY, 0xFFU) == raw;
  }

 private:
  Preferences prefs_{};
  bool ready_ = false;
};

class PowerManager {
 public:
  void begin(uint32_t now, EventLog &log, SafetyJournal &journal) {
    bootAt_ = now;
    journal_ = &journal;
    reason_ = esp_reset_reason();

    // Khoi dong lai CO CHU DICH (OTA/rollback vua thanh cong - xem giai
    // thich tai mayapMarkIntentionalRestart() trong config.h): du reason_ la
    // ESP_RST_SW giong het 1 lan crash/watchdog that, KHONG duoc tinh vao bo
    // dem "reset bat thuong" - xoa het bo dem, coi nhu khoi dong sach.
    const bool intentional = mayapConsumeIntentionalRestart();
    uint8_t resetCount = journal.ready() ? journal.resetCount() : 0U;
    if (intentional) {
      resetCount = 0U;
    } else if (resetReasonIsAutomaticRecovery(reason_)) {
      if (resetCount < UINT8_MAX) ++resetCount;
    } else {
      resetCount = 0U;
    }
    resetCount_ = resetCount;
    journalWriteOk_ = journal.ready() && journal.setResetCount(resetCount_);
    ackRequired_ = resetCount_ >= RESET_STORM_LIMIT;
    log.push(now, EventType::Boot, bootEventCode(reason_),
             static_cast<int16_t>(reason_), ackRequired_ ? 1U : 0U);
  }
  void service(uint32_t now) {
    if (!ackRequired_ && resetCount_ != 0U &&
        elapsedMs(now, bootAt_) >= RESET_STORM_STABLE_CLEAR_MS) {
      if (journal_ && journal_->setResetCount(0U)) {
        resetCount_ = 0U;
        journalWriteOk_ = true;
      } else {
        journalWriteOk_ = false;
      }
    }
  }
  esp_reset_reason_t reason() const { return reason_; }
  bool ackRequired() const { return ackRequired_; }
  uint8_t resetStormCount() const { return resetCount_; }
  bool journalWriteOk() const { return journalWriteOk_; }
  void acknowledge() {
    ackRequired_ = false;
    resetCount_ = 0U;
    journalWriteOk_ = journal_ && journal_->setResetCount(0U);
  }
 private:
  static uint16_t bootEventCode(esp_reset_reason_t reason) {
    switch (reason) {
      case ESP_RST_POWERON: return static_cast<uint16_t>(EventCode::BootPowerOn);
      case ESP_RST_EXT: return static_cast<uint16_t>(EventCode::BootExternal);
      case ESP_RST_SW: return static_cast<uint16_t>(EventCode::BootSoftware);
      case ESP_RST_PANIC: return static_cast<uint16_t>(EventCode::BootPanic);
      case ESP_RST_INT_WDT:
      case ESP_RST_TASK_WDT:
      case ESP_RST_WDT: return static_cast<uint16_t>(EventCode::BootWdt);
      case ESP_RST_BROWNOUT: return static_cast<uint16_t>(EventCode::BootBrownout);
      default: return static_cast<uint16_t>(EventCode::BootSoftware);
    }
  }
  esp_reset_reason_t reason_ = ESP_RST_UNKNOWN;
  bool ackRequired_ = false;
  bool journalWriteOk_ = false;
  uint8_t resetCount_ = 0U;
  uint32_t bootAt_ = 0U;
  SafetyJournal *journal_ = nullptr;
};

// ============================================================================
// DS3231: DOC CO TIMEOUT, KIEM TRA OSF, KHONG DUNG WIFI/RTC
// ============================================================================
class RtcDs3231 {
 public:
  void begin(uint32_t now) {
    lastReadAt_ = now - RTC_READ_PERIOD_MS;
    lastRepairAttemptAt_ = now - RTC_AUTO_REPAIR_RETRY_MS;
    update(now);
    // Lan doc dau la khoi tao, khong ghi thanh su kien "ket noi lai".
    reconnectNotice_ = false;
    autoRepairNotice_ = false;
  }

  void update(uint32_t now) {
    if (elapsedMs(now, lastReadAt_) < RTC_READ_PERIOD_MS) return;
    lastReadAt_ = now;

    DateTime next{};
    bool osf = true;
    if (!readDateTime(next) || !readOsf(osf)) {
      invalidReadConfirm_ = 0U;
      if (failedReads_ < 255U) ++failedReads_;
      if (failedReads_ >= 3U) {
        online_ = false;
        valid_ = false;
      }
      return;
    }

    const bool wasOnline = online_;
    failedReads_ = 0U;
    online_ = true;
    if (!wasOnline) reconnectNotice_ = true;
    osf_ = osf;

    const uint32_t nextEpoch = validDate(next) ? toEpoch(next) : 0U;

    // Neu module vua duoc lap lai ma OSF=1/du lieu sai, chi tu sua khi ESP32
    // van con nguon va da co moc RTC hop le trong RAM. Khong tu doan gio sau
    // khi ca he thong da mat nguon vi khong biet thoi gian mat dien bao lau.
    if (osf || nextEpoch == 0U) {
      valid_ = false;
      if (invalidReadConfirm_ < 255U) ++invalidReadConfirm_;
      (void)tryAutoRepair(now);
      return;
    }

    invalidReadConfirm_ = 0U;
    if (nextEpoch != lastObservedEpoch_) {
      lastObservedEpoch_ = nextEpoch;
      lastEpochChangeAt_ = now;
    }
    const bool clockAdvancing = lastEpochChangeAt_ != 0U &&
        elapsedMs(now, lastEpochChangeAt_) <= RTC_STUCK_TIMEOUT_MS;
    valid_ = clockAdvancing;
    if (!valid_) {
      if (invalidReadConfirm_ < 255U) ++invalidReadConfirm_;
      (void)tryAutoRepair(now);
      return;
    }

    invalidReadConfirm_ = 0U;
    autoRepairAttempts_ = 0U;
    epoch_ = nextEpoch;
    updateDateText(next);
    syncShadow(now, nextEpoch);
  }

  bool set(uint16_t year, uint8_t month, uint8_t day,
           uint8_t hour, uint8_t minute, uint8_t second) {
    const DateTime value{year, month, day, hour, minute, second};
    if (!validDate(value)) return false;
    return setDateTimeInternal(value, millis());
  }

  bool online() const { return online_; }
  bool valid() const { return valid_; }
  bool oscillatorStopped() const { return osf_; }

  // Khi RTC tam mat nhung ESP32 van dang chay, tra ve dong ho bong de log va
  // checkpoint khong bi mat moc thoi gian. valid() van false de HMI bao loi RTC.
  uint32_t epoch() const {
    if (valid_) return epoch_;
    return estimatedShadowEpoch(millis());
  }

  const char *dateText() const { return valid_ ? dateText_ : "--/--/----"; }
  const char *timeText() const { return valid_ ? timeText_ : "--:--"; }

  bool takeReconnectNotice() {
    const bool value = reconnectNotice_;
    reconnectNotice_ = false;
    return value;
  }

  bool takeAutoRepairNotice() {
    const bool value = autoRepairNotice_;
    autoRepairNotice_ = false;
    return value;
  }

 private:
  struct DateTime {
    uint16_t year = 2000U;
    uint8_t month = 1U;
    uint8_t day = 1U;
    uint8_t hour = 0U;
    uint8_t minute = 0U;
    uint8_t second = 0U;
  };

  static uint8_t fromBcd(uint8_t v) {
    return static_cast<uint8_t>((v >> 4U) * 10U + (v & 0x0FU));
  }
  static bool validBcd(uint8_t value, uint8_t maximum) {
    return (value & 0x0FU) <= 9U && ((value >> 4U) & 0x0FU) <= 9U &&
           fromBcd(value) <= maximum;
  }
  static uint8_t toBcd(uint8_t v) {
    return static_cast<uint8_t>(((v / 10U) << 4U) | (v % 10U));
  }
  static bool leap(uint16_t y) {
    return (y % 4U == 0U && y % 100U != 0U) || (y % 400U == 0U);
  }
  static uint8_t daysInMonth(uint16_t y, uint8_t m) {
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m < 1U || m > 12U) return 0U;
    return static_cast<uint8_t>(days[m - 1U] +
        ((m == 2U && leap(y)) ? 1U : 0U));
  }
  static bool validDate(const DateTime &d) {
    return d.year >= RTC_VALID_YEAR_MIN && d.year <= RTC_VALID_YEAR_MAX &&
           d.month >= 1U && d.month <= 12U && d.day >= 1U &&
           d.day <= daysInMonth(d.year, d.month) && d.hour < 24U &&
           d.minute < 60U && d.second < 60U;
  }
  static uint32_t toEpoch(const DateTime &d) {
    uint32_t days = 0U;
    for (uint16_t y = 1970U; y < d.year; ++y) days += leap(y) ? 366U : 365U;
    for (uint8_t m = 1U; m < d.month; ++m) days += daysInMonth(d.year, m);
    days += static_cast<uint32_t>(d.day - 1U);
    const uint64_t total = static_cast<uint64_t>(days) * 86400ULL +
        static_cast<uint64_t>(d.hour) * 3600ULL +
        static_cast<uint64_t>(d.minute) * 60ULL + d.second;
    return total <= UINT32_MAX ? static_cast<uint32_t>(total) : 0U;
  }
  static bool fromEpoch(uint32_t epoch, DateTime &out) {
    uint32_t days = epoch / 86400UL;
    uint32_t seconds = epoch % 86400UL;
    uint16_t year = 1970U;
    while (year <= RTC_VALID_YEAR_MAX) {
      const uint16_t yearDays = leap(year) ? 366U : 365U;
      if (days < yearDays) break;
      days -= yearDays;
      ++year;
    }
    if (year < RTC_VALID_YEAR_MIN || year > RTC_VALID_YEAR_MAX) return false;
    uint8_t month = 1U;
    while (month <= 12U) {
      const uint8_t monthDays = daysInMonth(year, month);
      if (days < monthDays) break;
      days -= monthDays;
      ++month;
    }
    if (month > 12U) return false;
    out.year = year;
    out.month = month;
    out.day = static_cast<uint8_t>(days + 1U);
    out.hour = static_cast<uint8_t>(seconds / 3600UL);
    out.minute = static_cast<uint8_t>((seconds % 3600UL) / 60UL);
    out.second = static_cast<uint8_t>(seconds % 60UL);
    return validDate(out);
  }

  void updateDateText(const DateTime &value) {
    snprintf(dateText_, sizeof(dateText_), "%02u/%02u/%04u",
             static_cast<unsigned>(value.day % 100U),
             static_cast<unsigned>(value.month % 100U),
             static_cast<unsigned>(value.year % 10000U));
    snprintf(timeText_, sizeof(timeText_), "%02u:%02u",
             static_cast<unsigned>(value.hour % 24U),
             static_cast<unsigned>(value.minute % 60U));
  }

  void syncShadow(uint32_t now, uint32_t value) {
    if (value == 0U) return;
    shadowEpoch_ = value;
    shadowAtMs_ = now;
    shadowValid_ = true;
  }

  uint32_t estimatedShadowEpoch(uint32_t now) const {
    if (!shadowValid_) return 0U;
    const uint32_t gapSec = elapsedMs(now, shadowAtMs_) / 1000UL;
    if (gapSec > RTC_AUTO_REPAIR_MAX_GAP_SEC) return 0U;
    if (shadowEpoch_ > UINT32_MAX - gapSec) return 0U;
    return shadowEpoch_ + gapSec;
  }

  bool tryAutoRepair(uint32_t now) {
    if (!RTC_AUTO_REPAIR_ENABLED ||
        invalidReadConfirm_ < RTC_AUTO_REPAIR_CONFIRM_READS ||
        autoRepairAttempts_ >= RTC_AUTO_REPAIR_MAX_ATTEMPTS ||
        elapsedMs(now, lastRepairAttemptAt_) < RTC_AUTO_REPAIR_RETRY_MS) {
      return false;
    }
    lastRepairAttemptAt_ = now;
    ++autoRepairAttempts_;
    const uint32_t estimated = estimatedShadowEpoch(now);
    if (estimated == 0U || !setEpochInternal(estimated, now)) return false;
    autoRepairNotice_ = true;
    reconnectNotice_ = true;
    invalidReadConfirm_ = 0U;
    return true;
  }

  bool setEpochInternal(uint32_t value, uint32_t now) {
    DateTime converted{};
    return fromEpoch(value, converted) && setDateTimeInternal(converted, now);
  }

  bool setDateTimeInternal(const DateTime &value, uint32_t now) {
    uint8_t data[7] = {
      toBcd(value.second), toBcd(value.minute), toBcd(value.hour), 1U,
      toBcd(value.day), toBcd(value.month),
      toBcd(static_cast<uint8_t>(value.year - 2000U))
    };
    if (!writeRegisters(0x00U, data, sizeof(data))) return false;

    uint8_t status = 0U;
    if (!readRegisters(0x0FU, &status, 1U)) return false;
    status &= static_cast<uint8_t>(~0x80U); // clear OSF sau khi da ghi gio
    if (!writeRegisters(0x0FU, &status, 1U)) return false;

    failedReads_ = 0U;
    invalidReadConfirm_ = 0U;
    autoRepairAttempts_ = 0U;
    online_ = true;
    osf_ = false;
    valid_ = true;
    epoch_ = toEpoch(value);
    lastObservedEpoch_ = epoch_;
    lastEpochChangeAt_ = now;
    updateDateText(value);
    syncShadow(now, epoch_);
    return true;
  }

  bool readDateTime(DateTime &out) const {
    uint8_t data[7]{};
    if (!readRegisters(0x00U, data, sizeof(data))) return false;
    const uint8_t secondBcd = data[0] & 0x7FU;
    const uint8_t minuteBcd = data[1] & 0x7FU;
    const uint8_t dayBcd = data[4] & 0x3FU;
    const uint8_t monthBcd = data[5] & 0x1FU;
    const uint8_t yearBcd = data[6];
    if (!validBcd(secondBcd, 59U) || !validBcd(minuteBcd, 59U) ||
        !validBcd(dayBcd, 31U) || fromBcd(dayBcd) == 0U ||
        !validBcd(monthBcd, 12U) || fromBcd(monthBcd) == 0U ||
        !validBcd(yearBcd, 99U)) {
      return false;
    }

    out.second = fromBcd(secondBcd);
    out.minute = fromBcd(minuteBcd);
    const uint8_t hourRegister = data[2];
    if ((hourRegister & 0x40U) != 0U) {
      // DS3231 dang o che do 12 gio: bit5 la PM, 1..12 nam trong bit4..0.
      const uint8_t hourBcd = hourRegister & 0x1FU;
      if (!validBcd(hourBcd, 12U) || fromBcd(hourBcd) == 0U) return false;
      const uint8_t hour12 = fromBcd(hourBcd);
      out.hour = static_cast<uint8_t>((hour12 % 12U) +
          ((hourRegister & 0x20U) != 0U ? 12U : 0U));
    } else {
      const uint8_t hourBcd = hourRegister & 0x3FU;
      if (!validBcd(hourBcd, 23U)) return false;
      out.hour = fromBcd(hourBcd);
    }
    out.day = fromBcd(dayBcd);
    out.month = fromBcd(monthBcd);
    out.year = static_cast<uint16_t>(2000U + fromBcd(yearBcd));
    return true;
  }
  bool readOsf(bool &osf) const {
    uint8_t status = 0U;
    if (!readRegisters(0x0FU, &status, 1U)) return false;
    osf = (status & 0x80U) != 0U;
    return true;
  }
  static bool readRegisters(uint8_t reg, uint8_t *data, size_t length) {
    if (!data || !length || length > 32U) return false;
    if (!mayapI2cLock(I2C_STORAGE_LOCK_TIMEOUT_MS)) return false;
    Wire.beginTransmission(RTC_I2C_ADDRESS);
    Wire.write(reg);
    const uint8_t err = Wire.endTransmission(false);
    if (err != 0U) {
      mayapI2cReport(RTC_I2C_ADDRESS, false);
      mayapI2cUnlock();
      return false;
    }
    const size_t got = Wire.requestFrom(
        RTC_I2C_ADDRESS, static_cast<uint8_t>(length), true);
    if (got != length) {
      mayapI2cReport(RTC_I2C_ADDRESS, false);
      while (Wire.available()) (void)Wire.read();
      mayapI2cUnlock();
      return false;
    }
    for (size_t i = 0; i < length; ++i) {
      if (!Wire.available()) {
        mayapI2cReport(RTC_I2C_ADDRESS, false);
        mayapI2cUnlock();
        return false;
      }
      data[i] = static_cast<uint8_t>(Wire.read());
    }
    mayapI2cUnlock();
    mayapI2cReport(RTC_I2C_ADDRESS, true);
    return true;
  }
  static bool writeRegisters(uint8_t reg, const uint8_t *data, size_t length) {
    if (!data || !length || length > 30U) return false;
    if (!mayapI2cLock(I2C_STORAGE_LOCK_TIMEOUT_MS)) return false;
    Wire.beginTransmission(RTC_I2C_ADDRESS);
    Wire.write(reg);
    const size_t written = Wire.write(data, length);
    const uint8_t err = Wire.endTransmission(true);
    mayapI2cUnlock();
    mayapI2cReport(RTC_I2C_ADDRESS, written == length && err == 0U);
    return written == length && err == 0U;
  }

  uint32_t epoch_ = 0U;
  uint32_t lastObservedEpoch_ = 0U;
  uint32_t lastReadAt_ = 0U;
  uint32_t lastEpochChangeAt_ = 0U;
  uint32_t lastRepairAttemptAt_ = 0U;
  uint32_t shadowEpoch_ = 0U;
  uint32_t shadowAtMs_ = 0U;
  uint8_t failedReads_ = 0U;
  uint8_t invalidReadConfirm_ = 0U;
  uint8_t autoRepairAttempts_ = 0U;
  bool online_ = false;
  bool valid_ = false;
  bool osf_ = true;
  bool shadowValid_ = false;
  bool reconnectNotice_ = false;
  bool autoRepairNotice_ = false;
  char dateText_[11] = "--/--/----";
  char timeText_[6] = "--:--";
};

// ============================================================================
// SANITIZE CAU HINH - DONG BO RANG BUOC VOI HMI
// ============================================================================
inline void sanitizeMachineConfig(MachineConfig &cfg) {
  const MachineConfig defaults{};
  if (!isfinite(cfg.targetTemp)) cfg.targetTemp = defaults.targetTemp;
  if (!isfinite(cfg.tempHysteresis)) cfg.tempHysteresis = defaults.tempHysteresis;
  if (!isfinite(cfg.lowTempAlarm)) cfg.lowTempAlarm = defaults.lowTempAlarm;
  if (!isfinite(cfg.highTempAlarm)) cfg.highTempAlarm = defaults.highTempAlarm;
  if (!isfinite(cfg.emergencyTemp)) cfg.emergencyTemp = defaults.emergencyTemp;
  if (!isfinite(cfg.kp)) cfg.kp = defaults.kp;
  if (!isfinite(cfg.ki)) cfg.ki = defaults.ki;
  if (!isfinite(cfg.kd)) cfg.kd = defaults.kd;
  if (!isfinite(cfg.lowHumidityAlarm)) cfg.lowHumidityAlarm = defaults.lowHumidityAlarm;
  if (!isfinite(cfg.targetHumidity)) cfg.targetHumidity = defaults.targetHumidity;
  if (!isfinite(cfg.ventOnTemp)) cfg.ventOnTemp = defaults.ventOnTemp;
  if (!isfinite(cfg.ventOffTemp)) cfg.ventOffTemp = defaults.ventOffTemp;
  if (!isfinite(cfg.tempOffset)) cfg.tempOffset = defaults.tempOffset;
  if (!isfinite(cfg.humidityOffset)) cfg.humidityOffset = defaults.humidityOffset;

  cfg.targetTemp = clampFloat(cfg.targetTemp, TARGET_TEMP_MIN_C, TARGET_TEMP_MAX_C);

  // Legacy repair duy nhat cho truong hop doi SV nhung TOAN BO envelope nhiet
  // van con y nguyen bo mac dinh cua SV 37.5 C. Chi sua khi ca 5 nguong van
  // dung mac dinh; neu nguoi dung da tuy chinh bat ky nguong nao thi giu
  // nguyen va chi ap dung cac invariant/clamp ben duoi.
  const bool legacyDefaultEnvelope =
      fabsf(cfg.targetTemp - defaults.targetTemp) > 0.0005f &&
      fabsf(cfg.lowTempAlarm - defaults.lowTempAlarm) <= 0.0005f &&
      fabsf(cfg.highTempAlarm - defaults.highTempAlarm) <= 0.0005f &&
      fabsf(cfg.emergencyTemp - defaults.emergencyTemp) <= 0.0005f &&
      fabsf(cfg.ventOnTemp - defaults.ventOnTemp) <= 0.0005f &&
      fabsf(cfg.ventOffTemp - defaults.ventOffTemp) <= 0.0005f;
  if (legacyDefaultEnvelope) {
    const float delta = cfg.targetTemp - defaults.targetTemp;
    cfg.lowTempAlarm += delta;
    cfg.highTempAlarm += delta;
    cfg.emergencyTemp += delta;
    cfg.ventOnTemp += delta;
    cfg.ventOffTemp += delta;
  }

  cfg.tempHysteresis = clampFloat(cfg.tempHysteresis, 0.1f, 1.0f);
  cfg.lowTempAlarm = clampFloat(cfg.lowTempAlarm, 25.0f,
                                cfg.targetTemp - LOW_ALARM_GAP_C);
  cfg.highTempAlarm = clampFloat(cfg.highTempAlarm,
                                 cfg.targetTemp + HIGH_ALARM_GAP_C,
                                 HIGH_ALARM_MAX_C);
  cfg.emergencyTemp = clampFloat(cfg.emergencyTemp,
                                 cfg.highTempAlarm + EMERGENCY_ABOVE_HIGH_C,
                                 EMERGENCY_MAX_C);

  cfg.controlMode = ControlMode::Pid;
  cfg.kp = clampFloat(cfg.kp, 0.0f, 100.0f);
  cfg.ki = clampFloat(cfg.ki, 0.0f, 20.0f);
  cfg.kd = clampFloat(cfg.kd, 0.0f, 200.0f);
  cfg.pidCycleSec = static_cast<uint16_t>(constrain(
      static_cast<int>(cfg.pidCycleSec), 1, 60));
  cfg.maxHeaterPower = static_cast<uint8_t>(constrain(
      static_cast<int>(cfg.maxHeaterPower), 10, 100));

  // Nang cao (schema 8) - gioi han cung dam bao khong the chinh ve gia tri
  // vo nghia/gay bao gia tu web hay HMI (vd 0 phut, 0 lan doi dau...).
  cfg.heaterStuckMinRiseC = clampFloat(cfg.heaterStuckMinRiseC, 0.05f, 5.0f);
  cfg.heaterStuckDurationSec = static_cast<uint16_t>(constrain(
      static_cast<int>(cfg.heaterStuckDurationSec), 60, 3600));
  cfg.tempRateLimitC = clampFloat(cfg.tempRateLimitC, 0.1f, 10.0f);
  cfg.tempRateWindowSec = static_cast<uint16_t>(constrain(
      static_cast<int>(cfg.tempRateWindowSec), 30, 1800));
  cfg.tempOscillationCrossLimit = static_cast<uint8_t>(constrain(
      static_cast<int>(cfg.tempOscillationCrossLimit), 2, 30));
  cfg.tempOscillationWindowSec = static_cast<uint16_t>(constrain(
      static_cast<int>(cfg.tempOscillationWindowSec), 60, 3600));
  cfg.autotuneRelayPowerPercent = static_cast<uint8_t>(constrain(
      static_cast<int>(cfg.autotuneRelayPowerPercent), 10, 80));
  cfg.autotuneBandC = clampFloat(cfg.autotuneBandC, 0.05f, 1.0f);

  cfg.lowHumidityAlarm = clampFloat(cfg.lowHumidityAlarm, 10.0f, 90.0f);
  cfg.humidityAlarmDelaySec = static_cast<uint16_t>(constrain(
      static_cast<int>(cfg.humidityAlarmDelaySec), 0, 600));
  cfg.targetHumidity = clampFloat(cfg.targetHumidity, 30.0f, 90.0f);
  cfg.humidifierHysteresisRh = static_cast<uint8_t>(constrain(
      static_cast<int>(cfg.humidifierHysteresisRh), 1, 10));
  if (!cfg.humidifierInstalled) cfg.humidifierEnabled = false;

  cfg.ventScheduleCount = static_cast<uint8_t>(constrain(
      static_cast<int>(cfg.ventScheduleCount), 1, VENT_SCHEDULE_MAX_RUNS));
  cfg.ventScheduleDurationMin = static_cast<uint8_t>(constrain(
      static_cast<int>(cfg.ventScheduleDurationMin), 1, 60));
  cfg.ventScheduleHour1 = static_cast<uint8_t>(constrain(static_cast<int>(cfg.ventScheduleHour1), 0, 23));
  cfg.ventScheduleHour2 = static_cast<uint8_t>(constrain(static_cast<int>(cfg.ventScheduleHour2), 0, 23));
  cfg.ventScheduleHour3 = static_cast<uint8_t>(constrain(static_cast<int>(cfg.ventScheduleHour3), 0, 23));
  cfg.ventScheduleHour4 = static_cast<uint8_t>(constrain(static_cast<int>(cfg.ventScheduleHour4), 0, 23));
  cfg.ventScheduleHour5 = static_cast<uint8_t>(constrain(static_cast<int>(cfg.ventScheduleHour5), 0, 23));
  cfg.ventScheduleHour6 = static_cast<uint8_t>(constrain(static_cast<int>(cfg.ventScheduleHour6), 0, 23));
  cfg.ventProfileLevel = static_cast<uint8_t>(constrain(static_cast<int>(cfg.ventProfileLevel), 0, 2));
  cfg.ventCycleMinutes = static_cast<uint8_t>(constrain(static_cast<int>(cfg.ventCycleMinutes), 40, 120));
  cfg.ventDutyDay1To3 = static_cast<uint8_t>(constrain(static_cast<int>(cfg.ventDutyDay1To3), 5, 90));
  cfg.ventDutyDay4To7 = static_cast<uint8_t>(constrain(static_cast<int>(cfg.ventDutyDay4To7), 5, 90));
  cfg.ventDutyDay8To11 = static_cast<uint8_t>(constrain(static_cast<int>(cfg.ventDutyDay8To11), 5, 90));
  cfg.ventDutyDay12To15 = static_cast<uint8_t>(constrain(static_cast<int>(cfg.ventDutyDay12To15), 5, 90));
  cfg.ventDutyDay16To18 = static_cast<uint8_t>(constrain(static_cast<int>(cfg.ventDutyDay16To18), 5, 90));
  cfg.ventDutyDay19To21 = static_cast<uint8_t>(constrain(static_cast<int>(cfg.ventDutyDay19To21), 5, 90));
  if (cfg.ventAutoEnabled) cfg.ventScheduleEnabled = false;

  // Trong AUTO, quat tuan hoan la chuc nang bat buoc. Giu truong nay trong
  // schema EEPROM de tuong thich ban cu, nhung khong cho du lieu cu tat quat.
  cfg.circulationFanEnabled = true;

  // Chuoi dong bo nhiet: SV <= HUT OFF < HUT ON <= BAO CAO < KHAN CAP.
  const float ventOnMin = cfg.targetTemp + VENT_ON_ABOVE_SV_C;
  const float ventOnMax = cfg.highTempAlarm;
  cfg.ventOnTemp = clampFloat(cfg.ventOnTemp, ventOnMin, ventOnMax);
  const float ventOffMin = cfg.targetTemp + VENT_OFF_ABOVE_SV_C;
  const float ventOffMax = cfg.ventOnTemp - VENT_HYSTERESIS_C;
  cfg.ventOffTemp = clampFloat(cfg.ventOffTemp, ventOffMin, ventOffMax);
  // Phong du lieu cu/sai thu tu: Bao cao khong duoc nam duoi nguong bat hut.
  cfg.highTempAlarm = clampFloat(cfg.highTempAlarm, cfg.ventOnTemp, HIGH_ALARM_MAX_C);
  cfg.emergencyTemp = clampFloat(cfg.emergencyTemp,
                                 cfg.highTempAlarm + EMERGENCY_ABOVE_HIGH_C,
                                 EMERGENCY_MAX_C);

  cfg.turnIntervalMin = static_cast<uint16_t>(constrain(
      static_cast<int>(cfg.turnIntervalMin), 1, 720));
  cfg.turnMaxRunSec = static_cast<uint16_t>(constrain(
      static_cast<int>(cfg.turnMaxRunSec), 5, 600));
  if (static_cast<uint8_t>(cfg.nextDirection) >
      static_cast<uint8_t>(TurnDirection::Right)) {
    cfg.nextDirection = defaults.nextDirection;
  }

  cfg.totalIncubationDays = static_cast<uint8_t>(constrain(
      static_cast<int>(cfg.totalIncubationDays), 1, 40));
  cfg.powerRestoreDelaySec = static_cast<uint16_t>(constrain(
      static_cast<int>(cfg.powerRestoreDelaySec), 0, 600));
  cfg.tempOffset = clampFloat(cfg.tempOffset, -5.0f, 5.0f);
  cfg.humidityOffset = clampFloat(cfg.humidityOffset, -20.0f, 20.0f);
  // sensorTimeoutSec la slot legacy: khong cho cau hinh ben ngoai lam dai
  // thoi gian may tiep tuc cap nhiet bang mau cam bien cu.
  cfg.sensorTimeoutSec = defaults.sensorTimeoutSec;
  if (static_cast<uint8_t>(cfg.connectivityMode) >
      static_cast<uint8_t>(ConnectivityMode::Online)) {
    cfg.connectivityMode = defaults.connectivityMode;
  }
  cfg.alarmEnabled = true; // alarm an toan khong cho tat
}

inline uint8_t ventProfileDutyPercent(const MachineConfig &cfg,
                                     uint32_t incubationDay, bool rhLow) {
  uint8_t base = cfg.ventDutyDay19To21;
  if (incubationDay <= 3U) base = cfg.ventDutyDay1To3;
  else if (incubationDay <= 7U) base = cfg.ventDutyDay4To7;
  else if (incubationDay <= 11U) base = cfg.ventDutyDay8To11;
  else if (incubationDay <= 15U) base = cfg.ventDutyDay12To15;
  else if (incubationDay <= 18U) base = cfg.ventDutyDay16To18;
  const uint16_t scale = cfg.ventProfileLevel == 0U ? 70U :
                         cfg.ventProfileLevel == 2U ? 130U : 100U;
  const uint8_t duty = static_cast<uint8_t>(std::min<uint16_t>(90U,
      (static_cast<uint16_t>(base) * scale + 50U) / 100U));
  return rhLow ? std::min<uint8_t>(duty, 5U) : duty;
}

// ============================================================================
// LUU AT24C32 HAI KHE + CRC. MAT DIEN KHI GHI VAN CON BAN CU HOP LE.
// ============================================================================
#pragma pack(push, 1)
struct PackedMachineConfigV1 {
  float targetTemp;
  float tempHysteresis;
  float lowTempAlarm;
  float highTempAlarm;
  float emergencyTemp;
  uint8_t controlMode;
  float kp;
  float ki;
  float kd;
  uint16_t pidCycleSec;
  uint8_t maxHeaterPower;
  float lowHumidityAlarm;
  uint16_t humidityAlarmDelaySec;
  uint8_t circulationFanEnabled;
  float ventOnTemp;
  float ventOffTemp;
  uint8_t turningEnabled;
  uint16_t turnIntervalMin;
  uint16_t turnMaxRunSec;
  uint8_t nextDirection;
  uint8_t totalIncubationDays;
  uint8_t allowHeatWithoutBatch;
  uint16_t powerRestoreDelaySec;
  float tempOffset;
  float humidityOffset;
  uint16_t sensorTimeoutSec;
  uint8_t alarmEnabled;
  uint8_t connectivityMode;
  uint8_t autoResumeOnPowerLoss;  // schema 5+: "Ap lai"
  uint8_t lightAfterBatchAlarmEnabled;  // schema 6+: canh bao den con bat khi dang ap
  uint8_t highTempAlarmWithoutBatch;  // schema 7+: bao nhiet cao/khan cap ke ca khong co me
  // schema 8+: "Nang cao" - nguong chan doan nhiet + tham so Auto Tune, truoc
  // day la hang so cung trong config.h (xem MachineConfig o config.h).
  float heaterStuckMinRiseC;
  uint16_t heaterStuckDurationSec;
  float tempRateLimitC;
  uint16_t tempRateWindowSec;
  uint8_t tempOscillationCrossLimit;
  uint16_t tempOscillationWindowSec;
  uint8_t autotuneRelayPowerPercent;
  float autotuneBandC;
  // schema 9+: F-13 - dao tay co dong lich dao tu dong hay khong.
  uint8_t manualTurnReanchorsSchedule;
  // schema 10+: tu kiem tra coi dinh ky (backlog, opt-in, mac dinh TAT).
  uint8_t sirenSelfTestEnabled;
  // schema 11+: lich thong gio dinh ky theo RTC. Them CUOI de schema 10 cu
  // van la prefix nguyen ven va migrate bang memcpy an toan.
  uint8_t ventScheduleEnabled;
  uint8_t ventScheduleCount;
  uint8_t ventScheduleDurationMin;
  uint8_t ventScheduleHour1;
  uint8_t ventScheduleHour2;
  uint8_t ventScheduleHour3;
  uint8_t ventScheduleHour4;
  uint8_t ventScheduleHour5;
  uint8_t ventScheduleHour6;
  // schema 12+: profile thong gio theo ngay ap, append-only de migrate.
  uint8_t ventAutoEnabled;
  uint8_t ventProfileLevel;
  uint8_t ventCycleMinutes;
  uint8_t ventDutyDay1To3;
  uint8_t ventDutyDay4To7;
  uint8_t ventDutyDay8To11;
  uint8_t ventDutyDay12To15;
  uint8_t ventDutyDay16To18;
  uint8_t ventDutyDay19To21;
  uint8_t adaptiveThermalBalanceEnabled; // schema 13, append-only
};
struct ConfigRecordV1 {
  uint32_t magic;
  uint16_t schema;
  uint16_t size;
  uint32_t sequence;
  PackedMachineConfigV1 payload;
  uint32_t crc;
};
// Config schema 3 cua v3.3.2 co payload giong phan dau schema 4, chua co
// connectivityMode. Dung mang byte de giu dung kich thuoc/CRC cu va migrate.
constexpr size_t CONFIG_V3_PAYLOAD_BYTES =
    offsetof(PackedMachineConfigV1, connectivityMode);
// Sau connectivityMode la 4 truong uint8_t moi hon: autoResumeOnPowerLoss
// (schema 5), lightAfterBatchAlarmEnabled (schema 6), highTempAlarmWithoutBatch
// (schema 7) - dat lai gia tri mac dinh tuong minh sau memcpy.
// So sanh voi offsetof truong Nang cao dau tien (schema 8), KHONG PHAI
// sizeof(PackedMachineConfigV1) - struct con them cac truong Nang cao phia
// sau highTempAlarmWithoutBatch nen sizeof toan struct khong con dung bang
// mep cuoi cua 4 truong nay nua.
static_assert(CONFIG_V3_PAYLOAD_BYTES + 4U * sizeof(uint8_t) ==
                  offsetof(PackedMachineConfigV1, heaterStuckMinRiseC),
              "connectivityMode phai la truong cuoi cung cua schema 3, "
              "theo sau boi dung 4 truong uint8_t moi hon");
struct ConfigRecordLegacyV3 {
  uint32_t magic;
  uint16_t schema;
  uint16_t size;
  uint32_t sequence;
  uint8_t payload[CONFIG_V3_PAYLOAD_BYTES];
  uint32_t crc;
};
// Config schema 4 (v3.5.0 truoc ban co "Ap lai") co payload giong schema 5
// tru autoResumeOnPowerLoss. Dung de nang cap tai cho khong mat cau hinh cu.
constexpr size_t CONFIG_V4_PAYLOAD_BYTES =
    offsetof(PackedMachineConfigV1, autoResumeOnPowerLoss);
// So sanh voi offsetof truong Nang cao dau tien (schema 8) - xem ghi chu o
// static_assert cua CONFIG_V3_PAYLOAD_BYTES o tren.
static_assert(CONFIG_V4_PAYLOAD_BYTES + 3U * sizeof(uint8_t) ==
                  offsetof(PackedMachineConfigV1, heaterStuckMinRiseC),
              "autoResumeOnPowerLoss + lightAfterBatchAlarmEnabled + "
              "highTempAlarmWithoutBatch phai la 3 truong cuoi cung cua "
              "PackedMachineConfigV1 truoc schema 8");
struct ConfigRecordLegacyV4 {
  uint32_t magic;
  uint16_t schema;
  uint16_t size;
  uint32_t sequence;
  uint8_t payload[CONFIG_V4_PAYLOAD_BYTES];
  uint32_t crc;
};
// Config schema 5 (truoc ban co canh bao "den con bat khi dang ap") co
// payload giong schema 6 tru lightAfterBatchAlarmEnabled. Dung de nang cap
// tai cho khong mat cau hinh cu, giong het cach lam voi schema 3/4 o tren.
constexpr size_t CONFIG_V5_PAYLOAD_BYTES =
    offsetof(PackedMachineConfigV1, lightAfterBatchAlarmEnabled);
struct ConfigRecordLegacyV5 {
  uint32_t magic;
  uint16_t schema;
  uint16_t size;
  uint32_t sequence;
  uint8_t payload[CONFIG_V5_PAYLOAD_BYTES];
  uint32_t crc;
};
// Config schema 6 (truoc ban co "Bao nhiet ngoai me") co payload giong
// schema 7 tru highTempAlarmWithoutBatch. Dung de nang cap tai cho khong
// mat cau hinh cu, giong het cach lam voi schema 3/4/5 o tren.
constexpr size_t CONFIG_V6_PAYLOAD_BYTES =
    offsetof(PackedMachineConfigV1, highTempAlarmWithoutBatch);
static_assert(CONFIG_V5_PAYLOAD_BYTES + 1U * sizeof(uint8_t) ==
                  CONFIG_V6_PAYLOAD_BYTES,
              "lightAfterBatchAlarmEnabled phai la truong duy nhat giua "
              "schema 5 va schema 6");
struct ConfigRecordLegacyV6 {
  uint32_t magic;
  uint16_t schema;
  uint16_t size;
  uint32_t sequence;
  uint8_t payload[CONFIG_V6_PAYLOAD_BYTES];
  uint32_t crc;
};
// Config schema 7 (truoc ban co "Nang cao": nguong chan doan nhiet + tham so
// Auto Tune) co payload giong schema 8 tru 8 truong moi cuoi cung. Dung de
// nang cap tai cho khong mat cau hinh cu, giong het cach lam voi cac schema
// truoc do.
constexpr size_t CONFIG_V7_PAYLOAD_BYTES =
    offsetof(PackedMachineConfigV1, heaterStuckMinRiseC);
static_assert(CONFIG_V6_PAYLOAD_BYTES + 1U * sizeof(uint8_t) ==
                  CONFIG_V7_PAYLOAD_BYTES,
              "highTempAlarmWithoutBatch phai la truong duy nhat giua "
              "schema 6 va schema 7");
static_assert(CONFIG_V7_PAYLOAD_BYTES + 3U * sizeof(float) + 3U * sizeof(uint16_t) +
                  2U * sizeof(uint8_t) ==
                  offsetof(PackedMachineConfigV1, manualTurnReanchorsSchedule),
              "8 truong Nang cao phai nam ngay truoc "
              "manualTurnReanchorsSchedule (schema 9) trong PackedMachineConfigV1");
struct ConfigRecordLegacyV7 {
  uint32_t magic;
  uint16_t schema;
  uint16_t size;
  uint32_t sequence;
  uint8_t payload[CONFIG_V7_PAYLOAD_BYTES];
  uint32_t crc;
};
// Config schema 8 (ban truoc F-13: dao tay co dong lich hay khong) co payload
// giong schema 9 tru manualTurnReanchorsSchedule. Dung de nang cap tai cho
// khong mat cau hinh cu, giong het cach lam voi cac schema truoc do.
constexpr size_t CONFIG_V8_PAYLOAD_BYTES =
    offsetof(PackedMachineConfigV1, manualTurnReanchorsSchedule);
static_assert(CONFIG_V8_PAYLOAD_BYTES + 1U * sizeof(uint8_t) ==
                  offsetof(PackedMachineConfigV1, sirenSelfTestEnabled),
              "manualTurnReanchorsSchedule (schema 9) phai nam ngay truoc "
              "sirenSelfTestEnabled (schema 10) trong PackedMachineConfigV1");
struct ConfigRecordLegacyV8 {
  uint32_t magic;
  uint16_t schema;
  uint16_t size;
  uint32_t sequence;
  uint8_t payload[CONFIG_V8_PAYLOAD_BYTES];
  uint32_t crc;
};
// Config schema 9 (ban truoc tinh nang tu kiem tra coi dinh ky) co payload
// giong schema 10 tru sirenSelfTestEnabled. Dung de nang cap tai cho khong
// mat cau hinh cu, giong het cach lam voi cac schema truoc do.
constexpr size_t CONFIG_V9_PAYLOAD_BYTES =
    offsetof(PackedMachineConfigV1, sirenSelfTestEnabled);
static_assert(CONFIG_V9_PAYLOAD_BYTES + 1U * sizeof(uint8_t) ==
                  offsetof(PackedMachineConfigV1, ventScheduleEnabled),
              "sirenSelfTestEnabled phai la truong cuoi schema 10");
struct ConfigRecordLegacyV9 {
  uint32_t magic;
  uint16_t schema;
  uint16_t size;
  uint32_t sequence;
  uint8_t payload[CONFIG_V9_PAYLOAD_BYTES];
  uint32_t crc;
};
// Schema 10 la ban ngay truoc lich thong gio dinh ky.
constexpr size_t CONFIG_V10_PAYLOAD_BYTES =
    offsetof(PackedMachineConfigV1, ventScheduleEnabled);
static_assert(CONFIG_V10_PAYLOAD_BYTES + 9U * sizeof(uint8_t) ==
                  offsetof(PackedMachineConfigV1, ventAutoEnabled),
              "9 truong lich thong gio phai nam cuoi schema 11");
struct ConfigRecordLegacyV10 {
  uint32_t magic;
  uint16_t schema;
  uint16_t size;
  uint32_t sequence;
  uint8_t payload[CONFIG_V10_PAYLOAD_BYTES];
  uint32_t crc;
};
constexpr size_t CONFIG_V11_PAYLOAD_BYTES =
    offsetof(PackedMachineConfigV1, ventAutoEnabled);
static_assert(CONFIG_V11_PAYLOAD_BYTES + 9U == offsetof(PackedMachineConfigV1, adaptiveThermalBalanceEnabled),
              "9 truong profile thong gio phai nam cuoi schema 12");
struct ConfigRecordLegacyV11 {
  uint32_t magic;
  uint16_t schema;
  uint16_t size;
  uint32_t sequence;
  uint8_t payload[CONFIG_V11_PAYLOAD_BYTES];
  uint32_t crc;
};
constexpr size_t CONFIG_V12_PAYLOAD_BYTES = offsetof(PackedMachineConfigV1, adaptiveThermalBalanceEnabled);
struct ConfigRecordLegacyV12 {
  uint32_t magic; uint16_t schema; uint16_t size; uint32_t sequence;
  uint8_t payload[CONFIG_V12_PAYLOAD_BYTES]; uint32_t crc;
};
// Schema batch v3 bo sung moc bat dau me va lan dao thanh cong gan nhat.
struct PackedBatchV1 {
  uint8_t wasRunning;
  uint8_t nextDirection;
  uint16_t turnCountToday;
  uint32_t turnCountBatch;
  uint32_t elapsedSec;
  uint32_t checkpointEpoch; // DS3231 epoch tai checkpoint, 0 neu RTC loi
  uint32_t batchStartEpoch; // dung de noi tiep file log sau mat dien
  uint32_t lastTurnEpoch;   // moc scheduler dao, 0 neu chua co moc RTC
};
struct BatchRecordV1 {
  uint32_t magic;
  uint16_t schema;
  uint16_t size;
  uint32_t sequence;
  PackedBatchV1 payload;
  uint32_t crc;
};
// Tuong thich ban v3.2.8 / batch schema 2 de khong lam mat me dang chay khi nang cap.
struct PackedBatchLegacyV2 {
  uint8_t wasRunning;
  uint8_t nextDirection;
  uint16_t turnCountToday;
  uint32_t turnCountBatch;
  uint32_t elapsedSec;
  uint32_t checkpointEpoch;
};
struct BatchRecordLegacyV2 {
  uint32_t magic;
  uint16_t schema;
  uint16_t size;
  uint32_t sequence;
  PackedBatchLegacyV2 payload;
  uint32_t crc;
};

#pragma pack(pop)

constexpr uint32_t CONFIG_MAGIC = 0x4D415943UL; // MAYC
constexpr uint32_t BATCH_MAGIC  = 0x4D415942UL; // MAYB
constexpr uint16_t CONFIG_SCHEMA = 13;
constexpr uint16_t CONFIG_SCHEMA_LEGACY_V12 = 12;
constexpr uint16_t CONFIG_SCHEMA_LEGACY = 3;
constexpr uint16_t CONFIG_SCHEMA_LEGACY_V4 = 4;
constexpr uint16_t CONFIG_SCHEMA_LEGACY_V5 = 5;
constexpr uint16_t CONFIG_SCHEMA_LEGACY_V6 = 6;
constexpr uint16_t CONFIG_SCHEMA_LEGACY_V7 = 7;
constexpr uint16_t CONFIG_SCHEMA_LEGACY_V8 = 8;
constexpr uint16_t CONFIG_SCHEMA_LEGACY_V9 = 9;
constexpr uint16_t CONFIG_SCHEMA_LEGACY_V10 = 10;
constexpr uint16_t CONFIG_SCHEMA_LEGACY_V11 = 11;
constexpr uint16_t BATCH_SCHEMA = 3;
constexpr uint16_t BATCH_SCHEMA_LEGACY = 2;

inline PackedMachineConfigV1 packConfig(const MachineConfig &c) {
  PackedMachineConfigV1 p{};
  p.targetTemp = c.targetTemp;
  p.tempHysteresis = c.tempHysteresis;
  p.lowTempAlarm = c.lowTempAlarm;
  p.highTempAlarm = c.highTempAlarm;
  p.emergencyTemp = c.emergencyTemp;
  p.controlMode = static_cast<uint8_t>(c.controlMode);
  p.kp = c.kp; p.ki = c.ki; p.kd = c.kd;
  p.pidCycleSec = c.pidCycleSec;
  p.maxHeaterPower = c.maxHeaterPower;
  p.lowHumidityAlarm = c.lowHumidityAlarm;
  // Delay 0..600 chi can 10 bit. Bit 10..13 giu khoang bat/tat tao am;
  // 0 trong record cu nghia la mac dinh 2%RH. Giu nguyen schema/kich thuoc.
  // Bit 14: co phan cung; bit 15: cho phep tao am.
  p.humidityAlarmDelaySec = static_cast<uint16_t>(c.humidityAlarmDelaySec & 0x03FFU) |
      (static_cast<uint16_t>(c.humidifierHysteresisRh & 0x0FU) << 10U) |
      (c.humidifierInstalled ? 0x4000U : 0U) |
      (c.humidifierEnabled ? 0x8000U : 0U);
  p.circulationFanEnabled = static_cast<uint8_t>(constrain(
      static_cast<int>(lroundf(c.targetHumidity)), 30, 90));
  p.ventOnTemp = c.ventOnTemp;
  p.ventOffTemp = c.ventOffTemp;
  p.turningEnabled = c.turningEnabled ? 1U : 0U;
  p.turnIntervalMin = c.turnIntervalMin;
  p.turnMaxRunSec = c.turnMaxRunSec;
  p.nextDirection = static_cast<uint8_t>(c.nextDirection);
  p.totalIncubationDays = c.totalIncubationDays;
  p.allowHeatWithoutBatch = c.allowHeatWithoutBatch ? 1U : 0U;
  p.powerRestoreDelaySec = c.powerRestoreDelaySec;
  p.tempOffset = c.tempOffset;
  p.humidityOffset = c.humidityOffset;
  p.sensorTimeoutSec = c.sensorTimeoutSec;
  p.alarmEnabled = c.alarmEnabled ? 1U : 0U;
  p.connectivityMode = static_cast<uint8_t>(c.connectivityMode);
  p.autoResumeOnPowerLoss = c.autoResumeOnPowerLoss ? 1U : 0U;
  p.lightAfterBatchAlarmEnabled = c.lightAfterBatchAlarmEnabled ? 1U : 0U;
  p.highTempAlarmWithoutBatch = c.highTempAlarmWithoutBatch ? 1U : 0U;
  p.heaterStuckMinRiseC = c.heaterStuckMinRiseC;
  p.heaterStuckDurationSec = c.heaterStuckDurationSec;
  p.tempRateLimitC = c.tempRateLimitC;
  p.tempRateWindowSec = c.tempRateWindowSec;
  p.tempOscillationCrossLimit = c.tempOscillationCrossLimit;
  p.tempOscillationWindowSec = c.tempOscillationWindowSec;
  p.autotuneRelayPowerPercent = c.autotuneRelayPowerPercent;
  p.autotuneBandC = c.autotuneBandC;
  p.manualTurnReanchorsSchedule = c.manualTurnReanchorsSchedule ? 1U : 0U;
  p.sirenSelfTestEnabled = c.sirenSelfTestEnabled ? 1U : 0U;
  p.ventScheduleEnabled = c.ventScheduleEnabled ? 1U : 0U;
  p.ventScheduleCount = c.ventScheduleCount;
  p.ventScheduleDurationMin = c.ventScheduleDurationMin;
  p.ventScheduleHour1 = c.ventScheduleHour1;
  p.ventScheduleHour2 = c.ventScheduleHour2;
  p.ventScheduleHour3 = c.ventScheduleHour3;
  p.ventScheduleHour4 = c.ventScheduleHour4;
  p.ventScheduleHour5 = c.ventScheduleHour5;
  p.ventScheduleHour6 = c.ventScheduleHour6;
  p.adaptiveThermalBalanceEnabled = c.adaptiveThermalBalanceEnabled ? 1U : 0U;
  p.ventAutoEnabled = c.ventAutoEnabled ? 1U : 0U;
  p.ventProfileLevel = c.ventProfileLevel;
  p.ventCycleMinutes = c.ventCycleMinutes;
  p.ventDutyDay1To3 = c.ventDutyDay1To3;
  p.ventDutyDay4To7 = c.ventDutyDay4To7;
  p.ventDutyDay8To11 = c.ventDutyDay8To11;
  p.ventDutyDay12To15 = c.ventDutyDay12To15;
  p.ventDutyDay16To18 = c.ventDutyDay16To18;
  p.ventDutyDay19To21 = c.ventDutyDay19To21;
  return p;
}
inline MachineConfig unpackConfig(const PackedMachineConfigV1 &p) {
  MachineConfig c{};
  c.targetTemp = p.targetTemp;
  c.tempHysteresis = p.tempHysteresis;
  c.lowTempAlarm = p.lowTempAlarm;
  c.highTempAlarm = p.highTempAlarm;
  c.emergencyTemp = p.emergencyTemp;
  c.controlMode = static_cast<ControlMode>(p.controlMode);
  c.kp = p.kp; c.ki = p.ki; c.kd = p.kd;
  c.pidCycleSec = p.pidCycleSec;
  c.maxHeaterPower = p.maxHeaterPower;
  c.lowHumidityAlarm = p.lowHumidityAlarm;
  c.humidifierInstalled = (p.humidityAlarmDelaySec & 0x4000U) != 0U;
  c.humidifierEnabled = (p.humidityAlarmDelaySec & 0x8000U) != 0U;
  c.humidityAlarmDelaySec = static_cast<uint16_t>(p.humidityAlarmDelaySec & 0x03FFU);
  const uint8_t humidityGap = static_cast<uint8_t>((p.humidityAlarmDelaySec >> 10U) & 0x0FU);
  c.humidifierHysteresisRh = humidityGap ? humidityGap : MachineConfig{}.humidifierHysteresisRh;
  c.targetHumidity = (p.circulationFanEnabled >= 30U && p.circulationFanEnabled <= 90U)
      ? static_cast<float>(p.circulationFanEnabled) : MachineConfig{}.targetHumidity;
  c.circulationFanEnabled = true;
  c.ventOnTemp = p.ventOnTemp;
  c.ventOffTemp = p.ventOffTemp;
  c.turningEnabled = p.turningEnabled != 0U;
  c.turnIntervalMin = p.turnIntervalMin;
  c.turnMaxRunSec = p.turnMaxRunSec;
  c.nextDirection = static_cast<TurnDirection>(p.nextDirection);
  c.totalIncubationDays = p.totalIncubationDays;
  c.allowHeatWithoutBatch = p.allowHeatWithoutBatch != 0U;
  c.powerRestoreDelaySec = p.powerRestoreDelaySec;
  c.tempOffset = p.tempOffset;
  c.humidityOffset = p.humidityOffset;
  c.sensorTimeoutSec = p.sensorTimeoutSec;
  c.alarmEnabled = p.alarmEnabled != 0U;
  c.connectivityMode = static_cast<ConnectivityMode>(p.connectivityMode);
  c.autoResumeOnPowerLoss = p.autoResumeOnPowerLoss != 0U;
  c.lightAfterBatchAlarmEnabled = p.lightAfterBatchAlarmEnabled != 0U;
  c.highTempAlarmWithoutBatch = p.highTempAlarmWithoutBatch != 0U;
  c.heaterStuckMinRiseC = p.heaterStuckMinRiseC;
  c.heaterStuckDurationSec = p.heaterStuckDurationSec;
  c.tempRateLimitC = p.tempRateLimitC;
  c.tempRateWindowSec = p.tempRateWindowSec;
  c.tempOscillationCrossLimit = p.tempOscillationCrossLimit;
  c.tempOscillationWindowSec = p.tempOscillationWindowSec;
  c.autotuneRelayPowerPercent = p.autotuneRelayPowerPercent;
  c.autotuneBandC = p.autotuneBandC;
  c.manualTurnReanchorsSchedule = p.manualTurnReanchorsSchedule != 0U;
  c.sirenSelfTestEnabled = p.sirenSelfTestEnabled != 0U;
  c.ventScheduleEnabled = p.ventScheduleEnabled != 0U;
  c.ventScheduleCount = p.ventScheduleCount;
  c.ventScheduleDurationMin = p.ventScheduleDurationMin;
  c.ventScheduleHour1 = p.ventScheduleHour1;
  c.ventScheduleHour2 = p.ventScheduleHour2;
  c.ventScheduleHour3 = p.ventScheduleHour3;
  c.ventScheduleHour4 = p.ventScheduleHour4;
  c.ventScheduleHour5 = p.ventScheduleHour5;
  c.ventScheduleHour6 = p.ventScheduleHour6;
  c.adaptiveThermalBalanceEnabled = p.adaptiveThermalBalanceEnabled != 0U;
  c.ventAutoEnabled = p.ventAutoEnabled != 0U;
  c.ventProfileLevel = p.ventProfileLevel;
  c.ventCycleMinutes = p.ventCycleMinutes;
  c.ventDutyDay1To3 = p.ventDutyDay1To3;
  c.ventDutyDay4To7 = p.ventDutyDay4To7;
  c.ventDutyDay8To11 = p.ventDutyDay8To11;
  c.ventDutyDay12To15 = p.ventDutyDay12To15;
  c.ventDutyDay16To18 = p.ventDutyDay16To18;
  c.ventDutyDay19To21 = p.ventDutyDay19To21;
  sanitizeMachineConfig(c);
  return c;
}

class ExternalEeprom24xx {
 public:
  // Per-instance diagnostic snapshot; never prints or changes retry/I2C policy.
  struct WriteTrace { uint16_t address=0; uint8_t reason=0, requested=0, written=0, error=0; };
  WriteTrace lastWriteTrace() const { return writeTrace_; }
  WriteTrace lastReadTrace() const { return readTrace_; }
  bool begin() const {
    for (uint8_t attempt = 0U; attempt < EEPROM_IO_RETRIES; ++attempt) {
      if (probe()) {
        if (attempt > 0U) ++softRetryEvents_;
        return true;
      }
      finiteRetryPause();
    }
    ++softRetryEvents_;
    return false;
  }

  bool readBytes(uint16_t address, void *destination, size_t length) const {
    readTrace_=WriteTrace{};
    if (!destination || !rangeValid(address, length)) return false;
    for (uint8_t attempt = 0U; attempt < EEPROM_IO_RETRIES; ++attempt) {
      if (readBytesOnce(address, destination, length)) {
        if (attempt > 0U) ++softRetryEvents_;
        return true;
      }
      finiteRetryPause();
    }
    ++softRetryEvents_;
    return false;
  }

  bool writeBytes(uint16_t address, const void *source, size_t length) const {
    writeTrace_=WriteTrace{};
    if (!source || !rangeValid(address, length)) return false;
    // Neu mot lan ghi bi ngat giua chung, ghi lai toan bo record vao cung slot.
    // Slot A/B con lai van nguyen ven; CRC se loai slot dang ghi do dang.
    for (uint8_t attempt = 0U; attempt < EEPROM_IO_RETRIES; ++attempt) {
      if (writeBytesOnce(address, source, length)) {
        if (attempt > 0U) ++softRetryEvents_;
        return true;
      }
      finiteRetryPause();
    }
    ++softRetryEvents_;
    return false;
  }

  // Dem "su kien phai thu lai" (thanh cong sau >=1 lan that bai, HOAC that
  // bai toan bo) - dau hieu suy giam som cua chip nho, doc va tu xoa ve 0
  // moi chu ky kiem tra suc khoe (xem serviceHealthMonitor() trong
  // MachineController). mutable vi cac ham tren la const (chi lam I/O,
  // khong doi trang thai cau hinh cua chinh object nay).
  uint32_t takeSoftRetryEvents() const {
    const uint32_t value = softRetryEvents_;
    softRetryEvents_ = 0U;
    return value;
  }

 private:
  mutable uint32_t softRetryEvents_ = 0U;
  mutable WriteTrace writeTrace_{},readTrace_{};
  static void finiteRetryPause() {
    if (EEPROM_RETRY_GAP_MS != 0U) {
      vTaskDelay(pdMS_TO_TICKS(EEPROM_RETRY_GAP_MS));
    }
  }

  static bool rangeValid(uint16_t address, size_t length) {
    return length <= EEPROM_CAPACITY_BYTES &&
           static_cast<uint32_t>(address) <= EEPROM_CAPACITY_BYTES - length;
  }

  // Khoa/mo I2C theo TUNG chunk (toi da 32 byte), khong khoa 1 lan cho ca
  // ban ghi dai - ban ghi cau hinh/me ap co the dai vai tram byte (~chuc
  // chunk). Neu giu 1 khoa xuyen suot, LCD (hmiTask, uu tien thap hon) co
  // the phai cho toi vai chuc ms khong duoc ve man hinh dang doc EEPROM.
  // Nha khoa giua cac chunk khong mat an toan: moi chunk tu gui lai dia chi
  // day du (khong dua vao con tro noi bo con lai tu chunk truoc).
  bool readBytesOnce(uint16_t address, void *destination, size_t length) const {
    uint8_t *out = static_cast<uint8_t *>(destination);
    bool ok = true;
    while (length && ok) {
      const uint8_t chunk = static_cast<uint8_t>(std::min<size_t>(length, 32U));
      if (!mayapI2cLock(I2C_STORAGE_LOCK_TIMEOUT_MS)) { readTrace_.address=address;readTrace_.reason=1;readTrace_.requested=chunk;ok = false; break; }
      Wire.beginTransmission(EEPROM_I2C_ADDRESS);
      Wire.write(static_cast<uint8_t>(address >> 8U));
      Wire.write(static_cast<uint8_t>(address & 0xFFU));
      const uint8_t wireResult=Wire.endTransmission(false);
      if (wireResult != 0U) { readTrace_.address=address;readTrace_.reason=3;readTrace_.requested=chunk;readTrace_.error=wireResult;mayapI2cUnlock(); ok = false; break; }
      const size_t got = Wire.requestFrom(EEPROM_I2C_ADDRESS, chunk, true);
      if (got != chunk) {
        readTrace_.address=address;readTrace_.reason=2;readTrace_.requested=chunk;readTrace_.written=static_cast<uint8_t>(got);
        while (Wire.available()) (void)Wire.read();
        mayapI2cUnlock();
        ok = false;
        break;
      }
      for (uint8_t i = 0U; i < chunk; ++i) {
        if (!Wire.available()) { readTrace_.address=address;readTrace_.reason=5;readTrace_.requested=chunk;readTrace_.written=i;ok = false; break; }
        out[i] = static_cast<uint8_t>(Wire.read());
      }
      mayapI2cUnlock();
      if (!ok) break;
      address = static_cast<uint16_t>(address + chunk);
      out += chunk;
      length -= chunk;
    }
    mayapI2cReport(EEPROM_I2C_ADDRESS, ok);
    return ok;
  }

  // Khoa/mo I2C theo TUNG trang (32 byte) thay vi khoa 1 lan cho ca ban ghi -
  // ly do giong het readBytesOnce() o tren, nhung anh huong ro hon o day: moi
  // trang con phai CHO them toi 20ms (EEPROM_WRITE_TIMEOUT_MS, waitWrite
  // CompleteLocked) de AT24C32 ghi xong noi bo. Ghi 1 ban ghi ~7 trang ma
  // khoa xuyen suot co the giu bus toi ~140ms lien tuc - qua lau so voi
  // ngan sach cho cua LCD (I2C_TIMEOUT_MS=25ms), gay lag ro khi vua luu cau
  // hinh/so lieu me ap vua thao tac man hinh. Nha khoa giua cac trang cho
  // LCD/RTC xen vao cong bang hon; slot A/B + CRC (xem ghi chu writeBytes())
  // da san sang cho truong hop 1 trang bi ngat giua chung do mat khoa.
  bool writeBytesOnce(uint16_t address, const void *source, size_t length) const {
    const uint8_t *in = static_cast<const uint8_t *>(source);
    bool ok = true;
    while (length && ok) {
      const uint8_t pageRemain = static_cast<uint8_t>(
          EEPROM_PAGE_SIZE - (address % EEPROM_PAGE_SIZE));
      const uint8_t chunk = static_cast<uint8_t>(
          std::min<size_t>(length, std::min<size_t>(pageRemain, EEPROM_MAX_WRITE_CHUNK)));
      if (!mayapI2cLock(I2C_STORAGE_LOCK_TIMEOUT_MS)) { writeTrace_.address=address;writeTrace_.reason=1;writeTrace_.requested=chunk; ok = false; break; }
      Wire.beginTransmission(EEPROM_I2C_ADDRESS);
      Wire.write(static_cast<uint8_t>(address >> 8U));
      Wire.write(static_cast<uint8_t>(address & 0xFFU));
      const size_t written = Wire.write(in, chunk);
      const uint8_t err = Wire.endTransmission(true);
      const bool complete = (written == chunk && err == 0U) ?
          waitWriteCompleteLocked() : false;
      mayapI2cUnlock();
      if (written != chunk || err != 0U || !complete) {
        writeTrace_.address=address;writeTrace_.requested=chunk;writeTrace_.written=static_cast<uint8_t>(written);writeTrace_.error=err;
        writeTrace_.reason=written!=chunk?2:err!=0U?3:4;
        ok = false;
        break;
      }
      address = static_cast<uint16_t>(address + chunk);
      in += chunk;
      length -= chunk;
    }
    mayapI2cReport(EEPROM_I2C_ADDRESS, ok);
    return ok;
  }

  static bool probeLocked() {
    Wire.beginTransmission(EEPROM_I2C_ADDRESS);
    return Wire.endTransmission(true) == 0U; // EEPROM write-busy NACK is normal.
  }

  static bool probe() {
    if (!mayapI2cLock(I2C_STORAGE_LOCK_TIMEOUT_MS)) return false;
    const bool ok = probeLocked();
    mayapI2cReport(EEPROM_I2C_ADDRESS, ok);
    mayapI2cUnlock();
    return ok;
  }

  static bool waitWriteCompleteLocked() {
    const uint32_t started = millis();
    for (;;) {
      // A delayed task resume must observe the chip before declaring timeout.
      // A still-busy chip exits at the existing deadline; no extra sleep/retry.
      if (probeLocked()) return true;
      if (elapsedMs(millis(), started) >= EEPROM_WRITE_TIMEOUT_MS) return false;
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
};

static_assert(sizeof(ConfigRecordV1) <= EEPROM_CONFIG_SLOT_BYTES,
              "Config record khong vua slot AT24C32");
static_assert(sizeof(ConfigRecordLegacyV3) <= EEPROM_CONFIG_SLOT_BYTES,
              "Legacy config record khong vua slot AT24C32");
static_assert(sizeof(ConfigRecordLegacyV4) <= EEPROM_CONFIG_SLOT_BYTES,
              "Legacy config record V4 khong vua slot AT24C32");
static_assert(sizeof(ConfigRecordLegacyV5) <= EEPROM_CONFIG_SLOT_BYTES,
              "Legacy config record V5 khong vua slot AT24C32");
static_assert(sizeof(ConfigRecordLegacyV6) <= EEPROM_CONFIG_SLOT_BYTES,
              "Legacy config record V6 khong vua slot AT24C32");
static_assert(sizeof(ConfigRecordLegacyV7) <= EEPROM_CONFIG_SLOT_BYTES,
              "Legacy config record V7 khong vua slot AT24C32");
static_assert(sizeof(ConfigRecordLegacyV8) <= EEPROM_CONFIG_SLOT_BYTES,
              "Legacy config record V8 khong vua slot AT24C32");
static_assert(sizeof(ConfigRecordLegacyV9) <= EEPROM_CONFIG_SLOT_BYTES,
              "Legacy config record V9 khong vua slot AT24C32");
static_assert(sizeof(ConfigRecordLegacyV10) <= EEPROM_CONFIG_SLOT_BYTES,
              "Legacy config record V10 khong vua slot AT24C32");
static_assert(sizeof(ConfigRecordLegacyV11) <= EEPROM_CONFIG_SLOT_BYTES,
              "Legacy config record V11 khong vua slot AT24C32");
static_assert(sizeof(BatchRecordV1) <= EEPROM_BATCH_SLOT_BYTES,
              "Batch record khong vua slot AT24C32");
static_assert(sizeof(BatchRecordLegacyV2) <= EEPROM_BATCH_SLOT_BYTES,
              "Legacy batch record khong vua slot AT24C32");

class PersistentStore {
 public:
  bool begin() {
    configCacheValid_ = false;
    batchCacheValid_ = false;
    ready_ = !EXTERNAL_EEPROM_ENABLED || eeprom_.begin();
    return ready_ || !EXTERNAL_EEPROM_REQUIRED;
  }
  bool ready() const { return ready_; }
  bool probe() const {
    return !EXTERNAL_EEPROM_ENABLED || eeprom_.begin();
  }

  // Xem ExternalEeprom24xx::takeSoftRetryEvents() - dung cho
  // serviceHealthMonitor() phat hien chip nho suy giam som.
  uint32_t takeEepromSoftRetryEvents() { return eeprom_.takeSoftRetryEvents(); }

  // Thu lai dung dia chi EEPROM da cau hinh; khong quet bus. Dung khi module
  // DS3231+AT24C32 bi thao ra/lap lai trong luc ESP32 van dang chay.
  bool reconnect() {
    if (!EXTERNAL_EEPROM_ENABLED) {
      ready_ = true;
      return true;
    }
    if (!eeprom_.begin()) {
      ready_ = false;
      return false;
    }
    ready_ = true;
    configCacheValid_ = false;
    batchCacheValid_ = false;
    return true;
  }

  void markOffline() {
    ready_ = false;
    configCacheValid_ = false;
    batchCacheValid_ = false;
  }

  bool loadConfig(MachineConfig &out) {
    if (!ready_ || !refreshConfigCache()) return false;
    out = unpackConfig(configPayload_);
    return true;
  }

  bool saveConfig(const MachineConfig &input, MachineConfig &readback) {
    if (!ready_) return false;
    MachineConfig clean = input;
    sanitizeMachineConfig(clean);
    const PackedMachineConfigV1 payload = packConfig(clean);

    if (!configCacheValid_) (void)refreshConfigCache();
    if (configCacheValid_ &&
        memcmp(&configPayload_, &payload, sizeof(payload)) == 0) {
      readback = clean;
      return true;
    }

    ConfigRecordV1 record{};
    record.magic = CONFIG_MAGIC;
    record.schema = CONFIG_SCHEMA;
    record.size = sizeof(record);
    record.sequence = configCacheValid_ ? configSequence_ + 1U : 1U;
    record.payload = payload;
    record.crc = mcCrc32(reinterpret_cast<const uint8_t *>(&record),
                         offsetof(ConfigRecordV1, crc));

    const bool targetIsA = !configCacheValid_ || !configCurrentIsA_;
    const uint16_t target = targetIsA ? EEPROM_ADDR_CONFIG_A
                                      : EEPROM_ADDR_CONFIG_B;
    if (!writeRecord(target, record)) return false;

    ConfigRecordV1 verify{};
    if (!readRecord(target, verify) || !validConfig(verify) ||
        verify.sequence != record.sequence ||
        memcmp(&verify.payload, &payload, sizeof(payload)) != 0) {
      return false;
    }

    configCacheValid_ = true;
    configCurrentIsA_ = targetIsA;
    configSequence_ = verify.sequence;
    configPayload_ = verify.payload;
    readback = unpackConfig(verify.payload);
    return true;
  }

  bool loadBatch(PackedBatchV1 &out) {
    if (!ready_ || !refreshBatchCache()) return false;
    out = batchPayload_;
    return true;
  }

  bool saveBatch(const PackedBatchV1 &payload) {
    if (!ready_) return false;
    if (!batchCacheValid_) (void)refreshBatchCache();
    if (batchCacheValid_ &&
        memcmp(&batchPayload_, &payload, sizeof(payload)) == 0) {
      return true;
    }

    BatchRecordV1 record{};
    record.magic = BATCH_MAGIC;
    record.schema = BATCH_SCHEMA;
    record.size = sizeof(record);
    record.sequence = batchCacheValid_ ? batchSequence_ + 1U : 1U;
    record.payload = payload;
    record.crc = mcCrc32(reinterpret_cast<const uint8_t *>(&record),
                         offsetof(BatchRecordV1, crc));

    const bool targetIsA = !batchCacheValid_ || !batchCurrentIsA_;
    const uint16_t target = targetIsA ? EEPROM_ADDR_BATCH_A
                                      : EEPROM_ADDR_BATCH_B;
    if (!writeRecord(target, record)) return false;

    BatchRecordV1 verify{};
    if (!readRecord(target, verify) || !validBatch(verify) ||
        verify.sequence != record.sequence ||
        memcmp(&verify.payload, &payload, sizeof(payload)) != 0) {
      return false;
    }

    batchCacheValid_ = true;
    batchCurrentIsA_ = targetIsA;
    batchSequence_ = verify.sequence;
    batchPayload_ = verify.payload;
    return true;
  }

 private:
  static bool newer(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
  }

  template <typename T>
  bool readRecord(uint16_t address, T &record) const {
    return eeprom_.readBytes(address, &record, sizeof(record));
  }
  template <typename T>
  bool writeRecord(uint16_t address, const T &record) const {
    return eeprom_.writeBytes(address, &record, sizeof(record));
  }

  static bool validConfig(const ConfigRecordV1 &r) {
    return r.magic == CONFIG_MAGIC && r.schema == CONFIG_SCHEMA &&
           r.size == sizeof(r) &&
           r.crc == mcCrc32(reinterpret_cast<const uint8_t *>(&r),
                            offsetof(ConfigRecordV1, crc));
  }
  static bool validConfigLegacy(const ConfigRecordLegacyV3 &r) {
    return r.magic == CONFIG_MAGIC && r.schema == CONFIG_SCHEMA_LEGACY &&
           r.size == sizeof(r) &&
           r.crc == mcCrc32(reinterpret_cast<const uint8_t *>(&r),
                            offsetof(ConfigRecordLegacyV3, crc));
  }
  static bool validConfigLegacyV4(const ConfigRecordLegacyV4 &r) {
    return r.magic == CONFIG_MAGIC && r.schema == CONFIG_SCHEMA_LEGACY_V4 &&
           r.size == sizeof(r) &&
           r.crc == mcCrc32(reinterpret_cast<const uint8_t *>(&r),
                            offsetof(ConfigRecordLegacyV4, crc));
  }
  static bool validConfigLegacyV5(const ConfigRecordLegacyV5 &r) {
    return r.magic == CONFIG_MAGIC && r.schema == CONFIG_SCHEMA_LEGACY_V5 &&
           r.size == sizeof(r) &&
           r.crc == mcCrc32(reinterpret_cast<const uint8_t *>(&r),
                            offsetof(ConfigRecordLegacyV5, crc));
  }
  static bool validConfigLegacyV6(const ConfigRecordLegacyV6 &r) {
    return r.magic == CONFIG_MAGIC && r.schema == CONFIG_SCHEMA_LEGACY_V6 &&
           r.size == sizeof(r) &&
           r.crc == mcCrc32(reinterpret_cast<const uint8_t *>(&r),
                            offsetof(ConfigRecordLegacyV6, crc));
  }
  static bool validConfigLegacyV7(const ConfigRecordLegacyV7 &r) {
    return r.magic == CONFIG_MAGIC && r.schema == CONFIG_SCHEMA_LEGACY_V7 &&
           r.size == sizeof(r) &&
           r.crc == mcCrc32(reinterpret_cast<const uint8_t *>(&r),
                            offsetof(ConfigRecordLegacyV7, crc));
  }
  static bool validConfigLegacyV8(const ConfigRecordLegacyV8 &r) {
    return r.magic == CONFIG_MAGIC && r.schema == CONFIG_SCHEMA_LEGACY_V8 &&
           r.size == sizeof(r) &&
           r.crc == mcCrc32(reinterpret_cast<const uint8_t *>(&r),
                            offsetof(ConfigRecordLegacyV8, crc));
  }
  static bool validConfigLegacyV9(const ConfigRecordLegacyV9 &r) {
    return r.magic == CONFIG_MAGIC && r.schema == CONFIG_SCHEMA_LEGACY_V9 &&
           r.size == sizeof(r) &&
           r.crc == mcCrc32(reinterpret_cast<const uint8_t *>(&r),
                            offsetof(ConfigRecordLegacyV9, crc));
  }
  static bool validConfigLegacyV10(const ConfigRecordLegacyV10 &r) {
    return r.magic == CONFIG_MAGIC && r.schema == CONFIG_SCHEMA_LEGACY_V10 &&
           r.size == sizeof(r) &&
           r.crc == mcCrc32(reinterpret_cast<const uint8_t *>(&r),
                            offsetof(ConfigRecordLegacyV10, crc));
  }
  static bool validConfigLegacyV12(const ConfigRecordLegacyV12 &r) {
    return r.magic == CONFIG_MAGIC && r.schema == CONFIG_SCHEMA_LEGACY_V12 &&
      r.size == sizeof(r) && r.crc == mcCrc32(reinterpret_cast<const uint8_t *>(&r), offsetof(ConfigRecordLegacyV12, crc));
  }
  static bool validConfigLegacyV11(const ConfigRecordLegacyV11 &r) {
    return r.magic == CONFIG_MAGIC && r.schema == CONFIG_SCHEMA_LEGACY_V11 &&
           r.size == sizeof(r) &&
           r.crc == mcCrc32(reinterpret_cast<const uint8_t *>(&r),
                            offsetof(ConfigRecordLegacyV11, crc));
  }
  static bool validBatch(const BatchRecordV1 &r) {
    return r.magic == BATCH_MAGIC && r.schema == BATCH_SCHEMA &&
           r.size == sizeof(r) &&
           r.crc == mcCrc32(reinterpret_cast<const uint8_t *>(&r),
                            offsetof(BatchRecordV1, crc));
  }
  static bool validBatchLegacy(const BatchRecordLegacyV2 &r) {
    return r.magic == BATCH_MAGIC && r.schema == BATCH_SCHEMA_LEGACY &&
           r.size == sizeof(r) &&
           r.crc == mcCrc32(reinterpret_cast<const uint8_t *>(&r),
                            offsetof(BatchRecordLegacyV2, crc));
  }


  bool refreshConfigCache() {
    ConfigRecordV1 a{}, b{};
    const bool va = readRecord(EEPROM_ADDR_CONFIG_A, a) && validConfig(a);
    const bool vb = readRecord(EEPROM_ADDR_CONFIG_B, b) && validConfig(b);
    if (va || vb) {
      const bool useA = !vb || (va && newer(a.sequence, b.sequence));
      const ConfigRecordV1 &best = useA ? a : b;
      configCacheValid_ = true;
      configCurrentIsA_ = useA;
      configSequence_ = best.sequence;
      configPayload_ = best.payload;
      return true;
    }

    // Schema 12 keeps its exact prefix and CRC. New opt-in remains OFF.
    ConfigRecordLegacyV12 a12{}, b12{};
    const bool va12=readRecord(EEPROM_ADDR_CONFIG_A,a12)&&validConfigLegacyV12(a12);
    const bool vb12=readRecord(EEPROM_ADDR_CONFIG_B,b12)&&validConfigLegacyV12(b12);
    if(va12||vb12){
      const bool useA=!vb12||(va12&&newer(a12.sequence,b12.sequence));
      const auto &best=useA?a12:b12;
      configPayload_=packConfig(MachineConfig{});
      memcpy(&configPayload_,best.payload,sizeof(best.payload));
      configCacheValid_=true;configCurrentIsA_=useA;configSequence_=best.sequence;return true;
    }

    // Fallback schema 11: giu nguyen config cu, them profile moi mac dinh TAT.
    ConfigRecordLegacyV11 la11{}, lb11{};
    const bool vla11 = readRecord(EEPROM_ADDR_CONFIG_A, la11) &&
                       validConfigLegacyV11(la11);
    const bool vlb11 = readRecord(EEPROM_ADDR_CONFIG_B, lb11) &&
                       validConfigLegacyV11(lb11);
    if (vla11 || vlb11) {
      const bool useA11 = !vlb11 || (vla11 && newer(la11.sequence, lb11.sequence));
      const ConfigRecordLegacyV11 &best11 = useA11 ? la11 : lb11;
      configPayload_ = packConfig(MachineConfig{});
      memcpy(&configPayload_, best11.payload, sizeof(best11.payload));
      configCacheValid_ = true;
      configCurrentIsA_ = useA11;
      configSequence_ = best11.sequence;
      return true;
    }

    // Fallback config schema 10 (ban truoc lich thong gio dinh ky).
    // Toan bo lich moi mac dinh TAT; cac gia tri gio/thoi luong duoc nap
    // default de lan dau BAT co cau hinh de hieu.
    ConfigRecordLegacyV10 la10{}, lb10{};
    const bool vla10 = readRecord(EEPROM_ADDR_CONFIG_A, la10) &&
                       validConfigLegacyV10(la10);
    const bool vlb10 = readRecord(EEPROM_ADDR_CONFIG_B, lb10) &&
                       validConfigLegacyV10(lb10);
    if (vla10 || vlb10) {
      const bool useA10 = !vlb10 || (vla10 && newer(la10.sequence, lb10.sequence));
      const ConfigRecordLegacyV10 &best10 = useA10 ? la10 : lb10;
      configPayload_ = packConfig(MachineConfig{});
      memcpy(&configPayload_, best10.payload, sizeof(best10.payload));
      configPayload_.ventScheduleEnabled = 0U;
      configPayload_.ventScheduleCount = 2U;
      configPayload_.ventScheduleDurationMin = 5U;
      configPayload_.ventScheduleHour1 = 8U;
      configPayload_.ventScheduleHour2 = 20U;
      configPayload_.ventScheduleHour3 = 12U;
      configPayload_.ventScheduleHour4 = 16U;
      configPayload_.ventScheduleHour5 = 0U;
      configPayload_.ventScheduleHour6 = 4U;
      configCacheValid_ = true;
      configCurrentIsA_ = useA10;
      configSequence_ = best10.sequence;
      return true;
    }

    // Fallback config schema 9 (ban truoc tinh nang tu kiem tra coi dinh
    // ky). Truong moi mac dinh TAT (khop dung default cua
    // MachineConfig::sirenSelfTestEnabled - tinh nang opt-in, khong tu bat
    // len khi nang cap firmware); lan luu cau hinh tiep theo se ghi schema
    // 10 vao khe doi dien.
    ConfigRecordLegacyV9 la9{}, lb9{};
    const bool vla9 = readRecord(EEPROM_ADDR_CONFIG_A, la9) &&
                      validConfigLegacyV9(la9);
    const bool vlb9 = readRecord(EEPROM_ADDR_CONFIG_B, lb9) &&
                      validConfigLegacyV9(lb9);
    if (vla9 || vlb9) {
      const bool useA9 = !vlb9 || (vla9 && newer(la9.sequence, lb9.sequence));
      const ConfigRecordLegacyV9 &best9 = useA9 ? la9 : lb9;
      configPayload_ = packConfig(MachineConfig{});
      memcpy(&configPayload_, best9.payload, sizeof(best9.payload));
      configPayload_.sirenSelfTestEnabled = 0U;
      configCacheValid_ = true;
      configCurrentIsA_ = useA9;
      configSequence_ = best9.sequence;
      return true;
    }

    // Fallback config schema 8 (ban truoc F-13: dao tay co dong lich hay
    // khong). Truong moi mac dinh TAT (khop dung default cua
    // MachineConfig::manualTurnReanchorsSchedule - giu nguyen hanh vi cu, dao
    // tay khong dong lich); lan luu cau hinh tiep theo se ghi schema 10 (hien
    // tai) vao khe doi dien.
    ConfigRecordLegacyV8 la8{}, lb8{};
    const bool vla8 = readRecord(EEPROM_ADDR_CONFIG_A, la8) &&
                      validConfigLegacyV8(la8);
    const bool vlb8 = readRecord(EEPROM_ADDR_CONFIG_B, lb8) &&
                      validConfigLegacyV8(lb8);
    if (vla8 || vlb8) {
      const bool useA8 = !vlb8 || (vla8 && newer(la8.sequence, lb8.sequence));
      const ConfigRecordLegacyV8 &best8 = useA8 ? la8 : lb8;
      configPayload_ = packConfig(MachineConfig{});
      memcpy(&configPayload_, best8.payload, sizeof(best8.payload));
      configPayload_.manualTurnReanchorsSchedule = 0U;
      configCacheValid_ = true;
      configCurrentIsA_ = useA8;
      configSequence_ = best8.sequence;
      return true;
    }

    // Fallback config schema 7 (ban truoc khi co "Nang cao": nguong chan
    // doan nhiet + tham so Auto Tune). 8 truong moi duoc dat lai dung gia tri
    // mac dinh cua MachineConfig (khop y het hang so cu tung dung o config.h
    // truoc khi chuyen sang schema 8) thay vi de nguyen 0 tu memcpy; lan luu
    // cau hinh tiep theo se ghi schema 8 vao khe doi dien.
    ConfigRecordLegacyV7 la7{}, lb7{};
    const bool vla7 = readRecord(EEPROM_ADDR_CONFIG_A, la7) &&
                      validConfigLegacyV7(la7);
    const bool vlb7 = readRecord(EEPROM_ADDR_CONFIG_B, lb7) &&
                      validConfigLegacyV7(lb7);
    if (vla7 || vlb7) {
      const bool useA7 = !vlb7 || (vla7 && newer(la7.sequence, lb7.sequence));
      const ConfigRecordLegacyV7 &best7 = useA7 ? la7 : lb7;
      configPayload_ = packConfig(MachineConfig{});
      memcpy(&configPayload_, best7.payload, sizeof(best7.payload));
      configPayload_.heaterStuckMinRiseC = 0.3f;
      configPayload_.heaterStuckDurationSec = 900;
      configPayload_.tempRateLimitC = 1.0f;
      configPayload_.tempRateWindowSec = 120;
      configPayload_.tempOscillationCrossLimit = 6;
      configPayload_.tempOscillationWindowSec = 600;
      configPayload_.autotuneRelayPowerPercent = 30;
      configPayload_.autotuneBandC = 0.20f;
      configCacheValid_ = true;
      configCurrentIsA_ = useA7;
      configSequence_ = best7.sequence;
      return true;
    }

    // Fallback config schema 6 (ban truoc khi co "Bao nhiet ngoai me"). Muc
    // nay mac dinh BAT cho ban ghi cu (giu nguyen hanh vi tu truoc gio: canh
    // bao nhiet cao/khan cap hoat dong ke ca khong co me), khop dung default
    // cua MachineConfig::highTempAlarmWithoutBatch; lan luu cau hinh tiep
    // theo se ghi schema 7 vao khe doi dien.
    ConfigRecordLegacyV6 la6{}, lb6{};
    const bool vla6 = readRecord(EEPROM_ADDR_CONFIG_A, la6) &&
                      validConfigLegacyV6(la6);
    const bool vlb6 = readRecord(EEPROM_ADDR_CONFIG_B, lb6) &&
                      validConfigLegacyV6(lb6);
    if (vla6 || vlb6) {
      const bool useA6 = !vlb6 || (vla6 && newer(la6.sequence, lb6.sequence));
      const ConfigRecordLegacyV6 &best6 = useA6 ? la6 : lb6;
      configPayload_ = packConfig(MachineConfig{});
      memcpy(&configPayload_, best6.payload, sizeof(best6.payload));
      configPayload_.highTempAlarmWithoutBatch = 1U;
      configCacheValid_ = true;
      configCurrentIsA_ = useA6;
      configSequence_ = best6.sequence;
      return true;
    }

    // Fallback config schema 5 (ban truoc khi co canh bao "den con bat khi
    // dang ap"). Canh bao nay mac dinh BAT cho ban ghi cu (khop dung default
    // cua MachineConfig::lightAfterBatchAlarmEnabled); lan luu cau hinh tiep
    // theo se ghi schema 6 vao khe doi dien.
    ConfigRecordLegacyV5 la5{}, lb5{};
    const bool vla5 = readRecord(EEPROM_ADDR_CONFIG_A, la5) &&
                      validConfigLegacyV5(la5);
    const bool vlb5 = readRecord(EEPROM_ADDR_CONFIG_B, lb5) &&
                      validConfigLegacyV5(lb5);
    if (vla5 || vlb5) {
      const bool useA5 = !vlb5 || (vla5 && newer(la5.sequence, lb5.sequence));
      const ConfigRecordLegacyV5 &best5 = useA5 ? la5 : lb5;
      configPayload_ = packConfig(MachineConfig{});
      memcpy(&configPayload_, best5.payload, sizeof(best5.payload));
      configPayload_.lightAfterBatchAlarmEnabled = 1U;
      configPayload_.highTempAlarmWithoutBatch = 1U;
      configCacheValid_ = true;
      configCurrentIsA_ = useA5;
      configSequence_ = best5.sequence;
      return true;
    }

    // Fallback config schema 4 (ban truoc khi co "Ap lai"). Ap lai mac dinh
    // TAT cho ban ghi cu; lan luu cau hinh tiep theo se ghi schema 6 vao khe
    // doi dien. Day chinh la duong nang cap giu lai toan bo cau hinh nguoi
    // dung da chinh (nhiet do, PID, dao trung...) thay vi mat trang ve default.
    ConfigRecordLegacyV4 la4{}, lb4{};
    const bool vla4 = readRecord(EEPROM_ADDR_CONFIG_A, la4) &&
                      validConfigLegacyV4(la4);
    const bool vlb4 = readRecord(EEPROM_ADDR_CONFIG_B, lb4) &&
                      validConfigLegacyV4(lb4);
    if (vla4 || vlb4) {
      const bool useA4 = !vlb4 || (vla4 && newer(la4.sequence, lb4.sequence));
      const ConfigRecordLegacyV4 &best4 = useA4 ? la4 : lb4;
      configPayload_ = packConfig(MachineConfig{});
      memcpy(&configPayload_, best4.payload, sizeof(best4.payload));
      configPayload_.autoResumeOnPowerLoss = 0U;
      configPayload_.lightAfterBatchAlarmEnabled = 1U;
      configPayload_.highTempAlarmWithoutBatch = 1U;
      configCacheValid_ = true;
      configCurrentIsA_ = useA4;
      configSequence_ = best4.sequence;
      return true;
    }

    // Fallback config schema 3. Che do mang moi mac dinh OFFLINE; lan luu
    // cau hinh tiep theo se ghi schema 6 vao khe doi dien.
    ConfigRecordLegacyV3 la{}, lb{};
    const bool vla = readRecord(EEPROM_ADDR_CONFIG_A, la) &&
                     validConfigLegacy(la);
    const bool vlb = readRecord(EEPROM_ADDR_CONFIG_B, lb) &&
                     validConfigLegacy(lb);
    if (!vla && !vlb) {
      configCacheValid_ = false;
      return false;
    }
    const bool useA = !vlb || (vla && newer(la.sequence, lb.sequence));
    const ConfigRecordLegacyV3 &best = useA ? la : lb;
    configPayload_ = packConfig(MachineConfig{});
    memcpy(&configPayload_, best.payload, sizeof(best.payload));
    configPayload_.connectivityMode =
        static_cast<uint8_t>(ConnectivityMode::Offline);
    configPayload_.autoResumeOnPowerLoss = 0U;  // chua ton tai o schema 3
    configPayload_.lightAfterBatchAlarmEnabled = 1U;
    configPayload_.highTempAlarmWithoutBatch = 1U;
    configCacheValid_ = true;
    configCurrentIsA_ = useA;
    configSequence_ = best.sequence;
    return true;
  }

  bool refreshBatchCache() {
    BatchRecordV1 a{}, b{};
    const bool va = readRecord(EEPROM_ADDR_BATCH_A, a) && validBatch(a);
    const bool vb = readRecord(EEPROM_ADDR_BATCH_B, b) && validBatch(b);
    if (va || vb) {
      const bool useA = !vb || (va && newer(a.sequence, b.sequence));
      const BatchRecordV1 &best = useA ? a : b;
      batchCacheValid_ = true;
      batchCurrentIsA_ = useA;
      batchSequence_ = best.sequence;
      batchPayload_ = best.payload;
      return true;
    }

    // Fallback schema 2 cua v3.2.8. Sau lan checkpoint/save tiep theo se
    // tu dong duoc nang len schema 3 o khe doi dien.
    BatchRecordLegacyV2 la{}, lb{};
    const bool vla = readRecord(EEPROM_ADDR_BATCH_A, la) && validBatchLegacy(la);
    const bool vlb = readRecord(EEPROM_ADDR_BATCH_B, lb) && validBatchLegacy(lb);
    if (!vla && !vlb) {
      batchCacheValid_ = false;
      return false;
    }
    const bool useA = !vlb || (vla && newer(la.sequence, lb.sequence));
    const BatchRecordLegacyV2 &best = useA ? la : lb;
    batchCacheValid_ = true;
    batchCurrentIsA_ = useA;
    batchSequence_ = best.sequence;
    batchPayload_ = PackedBatchV1{};
    batchPayload_.wasRunning = best.payload.wasRunning;
    batchPayload_.nextDirection = best.payload.nextDirection;
    batchPayload_.turnCountToday = best.payload.turnCountToday;
    batchPayload_.turnCountBatch = best.payload.turnCountBatch;
    batchPayload_.elapsedSec = best.payload.elapsedSec;
    batchPayload_.checkpointEpoch = best.payload.checkpointEpoch;
    if (best.payload.checkpointEpoch >= best.payload.elapsedSec) {
      batchPayload_.batchStartEpoch = best.payload.checkpointEpoch - best.payload.elapsedSec;
    }
    batchPayload_.lastTurnEpoch = 0U; // legacy khong co moc nay; scheduler se khoi tao an toan.
    return true;
  }


  ExternalEeprom24xx eeprom_{};
  bool ready_ = false;

  bool configCacheValid_ = false;
  bool configCurrentIsA_ = false;
  uint32_t configSequence_ = 0U;
  PackedMachineConfigV1 configPayload_{};

  bool batchCacheValid_ = false;
  bool batchCurrentIsA_ = false;
  uint32_t batchSequence_ = 0U;
  PackedBatchV1 batchPayload_{};

};

// ============================================================================
// INPUT: CUNG MOT HOP DONG CHO GPIO THAT VA SERIAL MO PHONG
// ============================================================================
struct InputState {
  bool limitLeft = false;
  bool limitRight = false;
  bool autoMode = false;
  bool heaterEnable = false;
  bool circulationFan = false;
  bool light = false;
  bool turnLeft = false;
  bool turnRight = false;
};

enum class InputChannel : uint8_t {
  LimitLeft, LimitRight, Auto, Heater, Fan, Light, TurnLeft, TurnRight, Count
};

struct InputEvent {
  uint32_t timestamp = 0U;
  InputChannel channel = InputChannel::LimitLeft;
  bool active = false;
  bool rising = false;
};

class InputManager {
 public:
  void begin() {
    const uint32_t now = millis();
#if MAYAP_SERIAL_INPUT_SIM
    state_ = InputState{};
    for (uint8_t i = 0; i < channelCount(); ++i) {
      raw_[i] = stable_[i] = false;
      changedAt_[i] = now;
    }
#else
    for (uint8_t i = 0; i < channelCount(); ++i) {
      const InputChannel channel = static_cast<InputChannel>(i);
      pinMode(pinFor(channel), INPUT_PULLUP);
      const bool active = readActive(pinFor(channel));
      raw_[i] = stable_[i] = active;
      changedAt_[i] = now;
    }
    rebuildState();
#endif
    lastScanAt_ = now;
  }

  void update(uint32_t now) {
#if MAYAP_SERIAL_INPUT_SIM
    (void)now;
#else
    if (elapsedMs(now, lastScanAt_) < INPUT_SCAN_MS) return;
    lastScanAt_ = now;
    for (uint8_t i = 0; i < channelCount(); ++i) {
      const InputChannel channel = static_cast<InputChannel>(i);
      const bool sample = readActive(pinFor(channel));
      if (sample != raw_[i]) {
        raw_[i] = sample;
        changedAt_[i] = now;
      }
      const uint32_t debounce = (channel == InputChannel::LimitLeft ||
                                 channel == InputChannel::LimitRight)
                                  ? LIMIT_DEBOUNCE_MS : INPUT_DEBOUNCE_MS;
      if (stable_[i] != raw_[i] && elapsedMs(now, changedAt_[i]) >= debounce) {
        commitStable(channel, raw_[i], now);
      }
    }
#endif
  }

  const InputState &state() const { return state_; }
  bool popEvent(InputEvent &event) { return events_.pop(event); }
  uint32_t droppedEvents() const { return events_.overflowCount(); }

#if MAYAP_SERIAL_INPUT_SIM
  bool setSimulated(const char *name, bool value, uint32_t now) {
    InputChannel channel{};
    if (!channelFromName(name, channel)) return false;
    commitStable(channel, value, now);
    raw_[static_cast<uint8_t>(channel)] = value;
    changedAt_[static_cast<uint8_t>(channel)] = now;
    return true;
  }
  void resetSimulation(uint32_t now) {
    for (uint8_t i = 0; i < channelCount(); ++i) {
      commitStable(static_cast<InputChannel>(i), false, now);
      raw_[i] = false;
      changedAt_[i] = now;
    }
  }
#endif

  static const char *name(InputChannel channel) {
    switch (channel) {
      case InputChannel::LimitLeft: return "LIMIT_LEFT";
      case InputChannel::LimitRight: return "LIMIT_RIGHT";
      case InputChannel::Auto: return "AUTO";
      case InputChannel::Heater: return "HEATER";
      case InputChannel::Fan: return "FAN";
      case InputChannel::Light: return "LIGHT";
      case InputChannel::TurnLeft: return "LEFT";
      case InputChannel::TurnRight: return "RIGHT";
      default: return "UNKNOWN";
    }
  }

 private:
  static constexpr uint8_t channelCount() {
    return static_cast<uint8_t>(InputChannel::Count);
  }
  static uint8_t pinFor(InputChannel channel) {
    switch (channel) {
      case InputChannel::LimitLeft: return PIN_IN_LIMIT_LEFT;
      case InputChannel::LimitRight: return PIN_IN_LIMIT_RIGHT;
      case InputChannel::Auto: return PIN_IN_AUTO;
      case InputChannel::Heater: return PIN_IN_HEATER_ENABLE;
      case InputChannel::Fan: return PIN_IN_CIRC_FAN;
      case InputChannel::Light: return PIN_IN_LIGHT;
      case InputChannel::TurnLeft: return PIN_IN_TURN_LEFT;
      case InputChannel::TurnRight: return PIN_IN_TURN_RIGHT;
      default: return 0;
    }
  }
  static bool channelFromName(const char *text, InputChannel &channel) {
    if (!text) return false;
    for (uint8_t i = 0; i < channelCount(); ++i) {
      const InputChannel candidate = static_cast<InputChannel>(i);
      if (!strcmp(text, name(candidate))) { channel = candidate; return true; }
    }
    return false;
  }
  static bool readActive(uint8_t pin) {
    const bool level = digitalRead(pin) != LOW;
    return INPUT_ACTIVE_LOW ? !level : level;
  }
  void commitStable(InputChannel channel, bool active, uint32_t now) {
    const uint8_t index = static_cast<uint8_t>(channel);
    if (stable_[index] == active) return;
    stable_[index] = active;
    rebuildState();
    InputEvent event{};
    event.timestamp = now;
    event.channel = channel;
    event.active = active;
    event.rising = active;
    events_.push(event);
  }
  void rebuildState() {
    state_.limitLeft = stable_[0];
    state_.limitRight = stable_[1];
    state_.autoMode = stable_[2];
    state_.heaterEnable = stable_[3];
    state_.circulationFan = stable_[4];
    state_.light = stable_[5];
    state_.turnLeft = stable_[6];
    state_.turnRight = stable_[7];
  }

  InputState state_{};
  bool raw_[8]{};
  bool stable_[8]{};
  uint32_t changedAt_[8]{};
  uint32_t lastScanAt_ = 0U;
  FixedRing<InputEvent, INPUT_EVENT_QUEUE_SIZE> events_{};
};

// ============================================================================
// SHT RS485 INDUSTRIAL v2.2 - MAY TRANG THAI MODBUS KHONG BLOCKING
// ============================================================================
#include "sensor_format.h"

namespace SHT485Config {
constexpr uint8_t UART_PORT = SHT_UART_PORT;
constexpr uint8_t PIN_RX = PIN_RS485_RX;
constexpr uint8_t PIN_TX = PIN_RS485_TX;
constexpr uint8_t PIN_DE_RE = PIN_RS485_DE_RE;
constexpr uint32_t BAUD = 9600;
constexpr uint8_t SLAVE_ID = 1;
constexpr uint32_t FIRST_POLL_DELAY_MS = 500;
constexpr uint32_t POLL_PERIOD_MS = 2000;
constexpr uint32_t RESPONSE_TIMEOUT_MS = 200;
constexpr uint32_t RETRY_GAP_MS = 80;
constexpr uint8_t ATTEMPTS_PER_CYCLE = 2;
constexpr uint8_t OFFLINE_AFTER_FAILED_CYCLES = 2;
constexpr uint32_t DATA_STALE_MS = 6500;
constexpr uint32_t STARTUP_REPORT_MS = 5000;
constexpr uint32_t DE_SETUP_US = 300;
constexpr uint32_t TX_TAIL_GUARD_US = 1500;
constexpr uint32_t TX_ROOM_TIMEOUT_MS = 20;
constexpr uint8_t RX_BYTES_PER_UPDATE = 16;
constexpr uint8_t RX_PURGE_BYTES_PER_ATTEMPT = 16;
constexpr int16_t TEMP_MIN_X10 = -400;
constexpr int16_t TEMP_MAX_X10 = 600;
constexpr int16_t HUM_MIN_X10 = 0;
constexpr int16_t HUM_MAX_X10 = 1000;
constexpr uint8_t MEDIAN_WINDOW = 3;
constexpr int32_t IIR_NUMERATOR = 3;
constexpr int32_t IIR_DENOMINATOR = 8;
}

enum class SHT485Event : uint8_t {
  None = 0, PresentAtStartup, MissingAtStartup, Lost, Restored
};

class SHT485Industrial {
 public:
  SHT485Industrial() : serial_(SHT485Config::UART_PORT) {}

  void begin() {
    pinMode(SHT485Config::PIN_DE_RE, OUTPUT);
    digitalWrite(SHT485Config::PIN_DE_RE, LOW);
    serial_.begin(SHT485Config::BAUD, SERIAL_8N1,
                  SHT485Config::PIN_RX, SHT485Config::PIN_TX);
    purgeRxBounded(SHT485Config::RX_PURGE_BYTES_PER_ATTEMPT);
    const uint32_t now = millis();
    bootMs_ = now;
    nextPollMs_ = now + SHT485Config::FIRST_POLL_DELAY_MS;
    state_ = State::Idle;
  }

  void update() {
    const uint32_t nowMs = millis();
    const uint32_t nowUs = micros();
    updateFreshness(nowMs);
    updateStartupStatus(nowMs);
    switch (state_) {
      case State::Idle:
        if (timeReached(nowMs, nextPollMs_)) { attempt_ = 0; beginAttempt(); }
        break;
      case State::WaitTxRoom:
        if (serial_.availableForWrite() >= static_cast<int>(RequestSize)) {
          digitalWrite(SHT485Config::PIN_DE_RE, HIGH);
          deadlineUs_ = nowUs + SHT485Config::DE_SETUP_US;
          state_ = State::DeSetup;
        } else if (elapsedMs(nowMs, txRoomWaitStartedMs_) >=
                   SHT485Config::TX_ROOM_TIMEOUT_MS) {
          ++txErrors_; failAttempt(nowMs);
        }
        break;
      case State::DeSetup:
        if (static_cast<int32_t>(nowUs - deadlineUs_) >= 0) {
          const size_t written = serial_.write(request_, RequestSize);
          if (written == RequestSize) {
            deadlineUs_ = nowUs + txDurationUs(RequestSize);
            state_ = State::TxActive;
          } else { ++txErrors_; failAttempt(nowMs); }
        }
        break;
      case State::TxActive:
        if (static_cast<int32_t>(nowUs - deadlineUs_) >= 0) {
          digitalWrite(SHT485Config::PIN_DE_RE, LOW);
          responseStartedMs_ = nowMs;
          resetParser();
          state_ = State::WaitResponse;
        }
        break;
      case State::WaitResponse:
        consumeRxBounded();
        if (frameComplete_) {
          if (decodeFrame(nowMs)) completeCycleSuccess(nowMs);
          else failAttempt(nowMs);
        } else if (elapsedMs(nowMs, responseStartedMs_) >=
                   SHT485Config::RESPONSE_TIMEOUT_MS) {
          ++timeoutErrors_; failAttempt(nowMs);
        }
        break;
      case State::RetryGap:
        if (timeReached(nowMs, retryAtMs_)) beginAttempt();
        break;
    }
  }

  bool takeNewData() { const bool r = newData_; newData_ = false; return r; }
  bool popEvent(SHT485Event &event) {
    if (pendingEvents_ == 0U) { event = SHT485Event::None; return false; }
    if (takeEventBit(EventStartupPresent)) event = SHT485Event::PresentAtStartup;
    else if (takeEventBit(EventStartupMissing)) event = SHT485Event::MissingAtStartup;
    else if (takeEventBit(EventLost)) event = SHT485Event::Lost;
    else { (void)takeEventBit(EventRestored); event = SHT485Event::Restored; }
    return true;
  }
  bool online() const { return online_; }
  bool dataValid() const {
    return decoder_.valid() && online_ && hasEverReceivedData_ &&
           elapsedMs(millis(), lastGoodFrameMs_) < SHT485Config::DATA_STALE_MS;
  }
  float temperatureC() const {
    return filterInitialized_ ? filteredTemp_ : NAN;
  }
  float humidityRH() const {
    return filterInitialized_ ? filteredHum_ : NAN;
  }
  float rawTemperatureC() const {
    return decoder_.temperature();
  }
  SensorProfile sensorProfile() const { return decoder_.profile(); }
  uint16_t rawTemperatureRegister() const { return rawTempRegister_; }
  bool formatLocked() const { return decoder_.locked(); }
  uint32_t dataAgeMs() const {
    return hasEverReceivedData_ ? elapsedMs(millis(), lastGoodFrameMs_) : UINT32_MAX;
  }
  uint32_t goodFrames() const { return goodFrames_; }
  uint32_t crcErrors() const { return crcErrors_; }
  uint32_t timeoutErrors() const { return timeoutErrors_; }
  uint32_t formatErrors() const { return formatErrors_; }
  uint32_t rangeErrors() const { return rangeErrors_; }
  uint32_t txErrors() const { return txErrors_; }
  uint32_t ignoredBytes() const { return ignoredBytes_; }

 private:
  enum class State : uint8_t { Idle, WaitTxRoom, DeSetup, TxActive, WaitResponse, RetryGap };
  static constexpr uint8_t RequestSize = 8;
  static constexpr uint8_t ResponseSize = 9;
  static constexpr uint8_t EventStartupPresent = 0x01;
  static constexpr uint8_t EventStartupMissing = 0x02;
  static constexpr uint8_t EventLost = 0x04;
  static constexpr uint8_t EventRestored = 0x08;

  HardwareSerial serial_;
  State state_ = State::Idle;
  const uint8_t request_[RequestSize] = {
    0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B
  };
  uint8_t response_[ResponseSize]{};
  uint8_t responseLength_ = 0;
  bool frameComplete_ = false;
  uint8_t attempt_ = 0;
  uint8_t failedCycles_ = 0;
  uint8_t uartRecoveryCount_ = 0U;
  bool uartRecoveredBefore_ = false;
  uint32_t uartRecoveryAt_ = 0U;
  uint32_t bootMs_ = 0;
  uint32_t nextPollMs_ = 0;
  uint32_t retryAtMs_ = 0;
  uint32_t responseStartedMs_ = 0;
  uint32_t txRoomWaitStartedMs_ = 0;
  uint32_t deadlineUs_ = 0;
  uint32_t lastGoodFrameMs_ = 0;
  bool online_ = false;
  bool hasEverReceivedData_ = false;
  bool newData_ = false;
  bool startupResolved_ = false;
  uint8_t pendingEvents_ = 0;
  SensorFormatDecoder decoder_{static_cast<SensorProfile>(MAYAP_SENSOR_PROFILE)};
  uint16_t rawTempRegister_ = 0U;
  float tempWindow_[3]{};
  float humWindow_[3]{};
  uint8_t windowCount_ = 0;
  uint8_t windowIndex_ = 0;
  float filteredTemp_ = 0.0f;
  float filteredHum_ = 0.0f;
  bool filterInitialized_ = false;
  uint32_t goodFrames_ = 0;
  uint32_t crcErrors_ = 0;
  uint32_t timeoutErrors_ = 0;
  uint32_t formatErrors_ = 0;
  uint32_t rangeErrors_ = 0;
  uint32_t txErrors_ = 0;
  uint32_t ignoredBytes_ = 0;

  static uint32_t txDurationUs(uint8_t bytes) {
    const uint32_t bits = static_cast<uint32_t>(bytes) * 10UL;
    const uint64_t duration =
        (static_cast<uint64_t>(bits) * 1000000ULL +
         SHT485Config::BAUD - 1ULL) /
        SHT485Config::BAUD + SHT485Config::TX_TAIL_GUARD_US;
    return duration <= UINT32_MAX ? static_cast<uint32_t>(duration)
                                  : UINT32_MAX;
  }
  void purgeRxBounded(uint8_t budget) {
    for (uint8_t i = 0; i < budget && serial_.available() > 0; ++i) {
      (void)serial_.read(); ++ignoredBytes_;
    }
  }
  void beginAttempt() {
    ++attempt_;
    purgeRxBounded(SHT485Config::RX_PURGE_BYTES_PER_ATTEMPT);
    resetParser();
    digitalWrite(SHT485Config::PIN_DE_RE, LOW);
    txRoomWaitStartedMs_ = millis();
    state_ = State::WaitTxRoom;
  }
  void resetParser() { responseLength_ = 0; frameComplete_ = false; }
  void consumeRxBounded() {
    for (uint8_t i = 0; i < SHT485Config::RX_BYTES_PER_UPDATE; ++i) {
      if (serial_.available() <= 0 || frameComplete_) break;
      const int incoming = serial_.read();
      if (incoming < 0) break;
      parseByte(static_cast<uint8_t>(incoming));
    }
  }
  void parseByte(uint8_t value) {
    if (responseLength_ == 0U) {
      if (value == SHT485Config::SLAVE_ID) response_[responseLength_++] = value;
      else ++ignoredBytes_;
      return;
    }
    if (responseLength_ == 1U) {
      if (value == 0x03U) response_[responseLength_++] = value;
      else if (value == SHT485Config::SLAVE_ID) response_[0] = value;
      else { ++ignoredBytes_; responseLength_ = 0; }
      return;
    }
    if (responseLength_ == 2U) {
      if (value == 0x04U) response_[responseLength_++] = value;
      else { ++formatErrors_; responseLength_ = 0; }
      return;
    }
    response_[responseLength_++] = value;
    if (responseLength_ == ResponseSize) frameComplete_ = true;
  }
  bool decodeFrame(uint32_t now) {
    if (responseLength_ != ResponseSize || response_[0] != SHT485Config::SLAVE_ID ||
        response_[1] != 0x03U || response_[2] != 0x04U) {
      ++formatErrors_; return false;
    }
    const uint16_t received = static_cast<uint16_t>(response_[7]) |
                              (static_cast<uint16_t>(response_[8]) << 8U);
    if (crc16(response_, 7) != received) { ++crcErrors_; return false; }
    const uint16_t hum = (static_cast<uint16_t>(response_[3]) << 8U) | response_[4];
    const uint16_t temp = (static_cast<uint16_t>(response_[5]) << 8U) | response_[6];
    rawTempRegister_ = temp;
    const bool wasLocked = decoder_.locked();
    const bool usable = decoder_.accept(temp, hum);
    if (!wasLocked && decoder_.locked()) {
      mayapSerialPrintf(false, "[SENSOR-FORMAT] locked=%u raw=%u T=%.4f\n",
          static_cast<unsigned>(decoder_.profile()), temp, decoder_.temperature());
    }
    ++goodFrames_;
    // A CRC-valid discovery frame completes the poll without rapid retry.
    // Heater stays inhibited until six consecutive samples lock one format.
    if (!usable) {
      if (decoder_.locked()) { ++rangeErrors_; newData_ = true; }
      return true;
    }
    updateFilter(decoder_.temperature(), decoder_.humidity());
    lastGoodFrameMs_ = now;
    hasEverReceivedData_ = true;
    newData_ = true;
    return true;
  }
  void updateFilter(float temp, float hum) {
    tempWindow_[windowIndex_] = temp;
    humWindow_[windowIndex_] = hum;
    windowIndex_ = static_cast<uint8_t>((windowIndex_ + 1U) % 3U);
    if (windowCount_ < 3U) ++windowCount_;
    const float targetTemp = medianTarget(tempWindow_, windowCount_);
    const float targetHum = medianTarget(humWindow_, windowCount_);
    if (!filterInitialized_) {
      filteredTemp_ = targetTemp; filteredHum_ = targetHum;
      filterInitialized_ = true; return;
    }
    // Same median-three + IIR 3/8; decoded native precision is never rounded.
    const float alpha = static_cast<float>(SHT485Config::IIR_NUMERATOR) /
                        SHT485Config::IIR_DENOMINATOR;
    filteredTemp_ += (targetTemp - filteredTemp_) * alpha;
    filteredHum_ += (targetHum - filteredHum_) * alpha;
  }
  static float medianTarget(const float *v, uint8_t count) {
    if (count <= 1U) return v[0];
    if (count == 2U) return (v[0] + v[1]) * 0.5f;
    const float a = v[0], b = v[1], c = v[2];
    return (a > b) ? ((b > c) ? b : ((a > c) ? c : a))
                   : ((a > c) ? a : ((b > c) ? c : b));
  }
  void completeCycleSuccess(uint32_t now) {
    const bool wasOnline = online_;
    online_ = true; failedCycles_ = 0;
    uartRecoveryCount_ = 0U;
    if (!startupResolved_) { startupResolved_ = true; setEvent(EventStartupPresent); }
    else if (!wasOnline) setEvent(EventRestored);
    nextPollMs_ = now + SHT485Config::POLL_PERIOD_MS;
    state_ = State::Idle;
  }
  void recoverUart(uint32_t now) {
    // Owner-only, after a fully failed cycle. Preserve counters/filter and
    // every plausibility/freshness/three-good-sample gate in processSensor().
    purgeRxBounded(128U);
    resetParser();
    digitalWrite(SHT485Config::PIN_DE_RE, LOW);
    online_ = false;
    newData_ = false;
    serial_.end();
    serial_.begin(SHT485Config::BAUD, SERIAL_8N1, SHT485Config::PIN_RX, SHT485Config::PIN_TX);
    purgeRxBounded(SHT485Config::RX_PURGE_BYTES_PER_ATTEMPT);
    uartRecoveredBefore_ = true;
    uartRecoveryAt_ = now;
    if (uartRecoveryCount_ < 255U) ++uartRecoveryCount_;
    failedCycles_ = 0U;
    attempt_ = 0U;
    nextPollMs_ = now + (uartRecoveryCount_ >= 3U ? 30000U : SHT485Config::FIRST_POLL_DELAY_MS);
    state_ = State::Idle;
    mayapSerialPrintf(false, "[UART-RECOVERY] RS485 reinit=%u isolate=%u\n", uartRecoveryCount_, uartRecoveryCount_ >= 3U);
  }
  void failAttempt(uint32_t now) {
    // CRC/transport failure breaks boot verification, but must not publish a
    // bogus new measurement. Once locked, keep the old bounded freshness/retry
    // policy; decoded wrong-format/range samples are invalidated in decodeFrame.
    if (!decoder_.locked()) decoder_.rejectSample();
    digitalWrite(SHT485Config::PIN_DE_RE, LOW);
    resetParser();
    if (attempt_ < SHT485Config::ATTEMPTS_PER_CYCLE) {
      retryAtMs_ = now + SHT485Config::RETRY_GAP_MS;
      state_ = State::RetryGap; return;
    }
    if (failedCycles_ < 255U) ++failedCycles_;
    if (online_ && failedCycles_ >= SHT485Config::OFFLINE_AFTER_FAILED_CYCLES) {
      online_ = false; setEvent(EventLost);
    }
    if (failedCycles_ >= 6U && (!uartRecoveredBefore_ || elapsedMs(now, uartRecoveryAt_) >= 30000U)) {
      recoverUart(now);
      return;
    }
    nextPollMs_ = now + SHT485Config::POLL_PERIOD_MS;
    state_ = State::Idle;
  }
  void updateFreshness(uint32_t now) {
    if (online_ && hasEverReceivedData_ &&
        elapsedMs(now, lastGoodFrameMs_) >= SHT485Config::DATA_STALE_MS) {
      online_ = false; setEvent(EventLost);
    }
  }
  void updateStartupStatus(uint32_t now) {
    if (!startupResolved_ && elapsedMs(now, bootMs_) >= SHT485Config::STARTUP_REPORT_MS) {
      startupResolved_ = true; online_ = false; setEvent(EventStartupMissing);
    }
  }
  void setEvent(uint8_t bit) { pendingEvents_ |= bit; }
  bool takeEventBit(uint8_t bit) {
    if ((pendingEvents_ & bit) == 0U) return false;
    pendingEvents_ &= static_cast<uint8_t>(~bit); return true;
  }
  static uint16_t crc16(const uint8_t *data, uint8_t length) {
    uint16_t crc = 0xFFFFU;
    for (uint8_t i = 0; i < length; ++i) {
      crc ^= data[i];
      for (uint8_t bit = 0; bit < 8U; ++bit) {
        crc = (crc & 1U) ? static_cast<uint16_t>((crc >> 1U) ^ 0xA001U)
                         : static_cast<uint16_t>(crc >> 1U);
      }
    }
    return crc;
  }
};

// ============================================================================
// PID THEO Nhip MAU CAM BIEN + ANTI-WINDUP + DAO HAM TREN PV
// ============================================================================
#include "thermal_control.h"
#include "thermal_smart_autotune.h"
#include "heater_burst_scheduler.h"

class ConditionTimer {
 public:
  bool update(uint32_t now, bool condition, uint32_t requiredMs) {
    if (!condition) { active_ = false; since_ = 0; return false; }
    if (!active_) { active_ = true; since_ = now; }
    return requiredMs == 0U || elapsedMs(now, since_) >= requiredMs;
  }
  void reset() { active_ = false; since_ = 0; }
 private:
  bool active_ = false;
  uint32_t since_ = 0;
};

// ============================================================================
// OUTPUT ARBITER: CHI NOI DUY NHAT DUOC PHEP DIGITALWRITE CO CAU CHAP HANH
// ============================================================================
struct OutputRequest {
  bool heaterSsr = false;
  bool heatMaster = false;
  bool turnLeft = false;
  bool turnRight = false;
  bool ventFan = false;
  bool ventFanForceOn = false;
  bool ventFanBypassTiming = false;
  bool light = false;
  bool circulationFan = false;
  bool siren = false;
  bool humidifier = false;
  bool immediateMasterDrop = false;
  bool forceAllSafe = false;
};
struct OutputState {
  bool heaterSsr = false;
  bool heatMaster = false;
  bool turnLeft = false;
  bool turnRight = false;
  bool ventFan = false;
  bool light = false;
  bool circulationFan = false;
  bool siren = false;
  bool humidifier = false;
};

enum class OutputChannel : uint8_t {
  HeaterSsr = 0, HeatMaster, TurnLeft, TurnRight, VentFan, Light,
  CirculationFan, Siren, Humidifier, Count
};

struct OutputEvent {
  uint32_t timestamp = 0U;
  OutputChannel channel = OutputChannel::HeaterSsr;
  bool active = false;
};

inline const char *outputName(OutputChannel channel) {
  switch (channel) {
    case OutputChannel::HeaterSsr: return "HEATER_SSR";
    case OutputChannel::HeatMaster: return "HEAT_MASTER";
    case OutputChannel::TurnLeft: return "TURN_LEFT";
    case OutputChannel::TurnRight: return "TURN_RIGHT";
    case OutputChannel::VentFan: return "VENT_FAN";
    case OutputChannel::Light: return "LIGHT";
    case OutputChannel::CirculationFan: return "CIRC_FAN";
    case OutputChannel::Siren: return "SIREN";
    case OutputChannel::Humidifier: return "RELAY_SPARE";
    default: return "UNKNOWN";
  }
}

inline void writeLogical(uint8_t pin, bool on) {
  digitalWrite(pin, (on == OUTPUT_ACTIVE_HIGH) ? HIGH : LOW);
}

inline void mayapSafeOutputsEarly() {
  const uint8_t pins[] = {
    PIN_OUT_HEATER_SSR, PIN_OUT_TURN_RIGHT,
    PIN_OUT_TURN_LEFT, PIN_OUT_VENT_FAN, PIN_OUT_LIGHT,
    PIN_OUT_HEAT_MASTER, PIN_OUT_CIRC_FAN, PIN_OUT_SIREN,
    PIN_OUT_HUMIDIFIER
  };
  for (uint8_t pin : pins) {
    // Nap muc OFF vao output latch truoc khi chuyen sang OUTPUT de giam xung
    // kich relay trong giai do firmware vua bat dau chay.
    writeLogical(pin, false);
    pinMode(pin, OUTPUT);
    writeLogical(pin, false);
  }
}

class OutputArbiter {
 public:
  void begin() {
    mayapSafeOutputsEarly();
    state_ = OutputState{};
    masterOnAt_ = masterDropAt_ = turnInterlockUntil_ = 0U;
    masterLastOffAt_ = 0U;
    transitionHourStartedAt_ = millis();
  }

  void update(uint32_t now, const OutputRequest &input) {
    const bool forceSafe = input.forceAllSafe || mayapSystemTripLatched();
    OutputRequest request = input;
    mayapApplyStartupOutputPolicy(request, mayapBootOperationsReady(),
        input.ventFanForceOn, input.siren);
    if (forceSafe) {
      request = OutputRequest{};
      request.forceAllSafe = true;
      request.immediateMasterDrop = true;
    }

    if (elapsedMs(now, transitionHourStartedAt_) >= 3600000UL) {
      transitionHourStartedAt_ = now;
      transitionsThisHour_ = 0U;
      relayRateExceeded_ = false;
    }

    // Lien dong output doc lap voi state machine: OFF chieu cu truoc,
    // doi dead-time, sau do moi duoc phep ON chieu moi.
    updateTurnOutputs(now, request.turnLeft, request.turnRight, forceSafe);

    // forceAllSafe/system-trip bo qua moi thoi gian minimum-on/minimum-switch.
    // Neu khong, quat/den/spare co the con giu ON toi 2 giay sau mot trip.
    if (forceSafe) {
      setHeaterBank(false, now);
      const bool masterWasOn = state_.heatMaster;
      setImmediate(PIN_OUT_HEAT_MASTER, OutputChannel::HeatMaster,
                   false, state_.heatMaster, now);
      if (masterWasOn) masterLastOffAt_ = now;
      masterDropAt_ = masterOnAt_ = 0U;
      setImmediate(PIN_OUT_VENT_FAN, OutputChannel::VentFan,
                   false, state_.ventFan, now);
      setImmediate(PIN_OUT_LIGHT, OutputChannel::Light,
                   false, state_.light, now);
      setImmediate(PIN_OUT_CIRC_FAN, OutputChannel::CirculationFan,
                   false, state_.circulationFan, now);
      setImmediate(PIN_OUT_SIREN, OutputChannel::Siren,
                   false, state_.siren, now);
      setImmediate(PIN_OUT_HUMIDIFIER, OutputChannel::Humidifier,
                   false, state_.humidifier, now);
      return;
    }

    // Profile bao ve ca ON/OFF relay. Qua nhiet/loi an toan duoc bat NGAY,
    // khong cho minimum-OFF giu quat tat khi nhiet dang nguy hiem.
    if (request.ventFanBypassTiming) {
      setImmediate(PIN_OUT_VENT_FAN, OutputChannel::VentFan,
                   request.ventFan, state_.ventFan, now);
    } else if (request.ventFan && request.ventFanForceOn) {
      setImmediate(PIN_OUT_VENT_FAN, OutputChannel::VentFan,
                   true, state_.ventFan, now);
    } else if (request.ventFan) {
      setMinOff(PIN_OUT_VENT_FAN, OutputChannel::VentFan,
                true, state_.ventFan, now, RELAY_VENT_MIN_OFF_MS);
    } else {
      setMinOn(PIN_OUT_VENT_FAN, OutputChannel::VentFan,
               false, state_.ventFan, now, RELAY_VENT_MIN_ON_MS);
    }
    setMinSwitch(PIN_OUT_LIGHT, OutputChannel::Light,
                 request.light, state_.light, now, RELAY_LIGHT_MIN_SWITCH_MS);
    setMinOn(PIN_OUT_CIRC_FAN, OutputChannel::CirculationFan,
             request.circulationFan, state_.circulationFan, now, RELAY_FAN_MIN_ON_MS);
    setImmediate(PIN_OUT_SIREN, OutputChannel::Siren,
                 request.siren, state_.siren, now);

    // Khi cat nhiet: SSR OFF truoc, contactor nha sau mot khoang ngan.
    if (!request.heatMaster || request.forceAllSafe) {
      setHeaterBank(false, now);
      if (state_.heatMaster) {
        if (request.immediateMasterDrop || request.forceAllSafe) {
          setImmediate(PIN_OUT_HEAT_MASTER, OutputChannel::HeatMaster,
                       false, state_.heatMaster, now);
          masterLastOffAt_ = now;
          masterDropAt_ = masterOnAt_ = 0U;
        } else {
          if (masterDropAt_ == 0U) masterDropAt_ = now + HEAT_MASTER_DROP_DELAY_MS;
          if (timeReached(now, masterDropAt_)) {
            setImmediate(PIN_OUT_HEAT_MASTER, OutputChannel::HeatMaster,
                         false, state_.heatMaster, now);
            masterLastOffAt_ = now;
            masterDropAt_ = masterOnAt_ = 0U;
          }
        }
      }
    } else {
      masterDropAt_ = 0U;
      if (!state_.heatMaster) {
        const bool offTimeSatisfied = masterLastOffAt_ == 0U ||
            elapsedMs(now, masterLastOffAt_) >= HEAT_MASTER_MIN_OFF_MS;
        if (offTimeSatisfied) {
          setImmediate(PIN_OUT_HEAT_MASTER, OutputChannel::HeatMaster,
                       true, state_.heatMaster, now);
          masterOnAt_ = now;
        }
      }
      const bool pickupDone = state_.heatMaster &&
          elapsedMs(now, masterOnAt_) >= HEAT_MASTER_PICKUP_MS;
      const bool enable = request.heaterSsr && pickupDone;
      setHeaterBank(enable, now);
    }

    setMinSwitch(PIN_OUT_HUMIDIFIER, OutputChannel::Humidifier,
                 request.humidifier, state_.humidifier, now,
                 RELAY_GENERAL_MIN_SWITCH_MS);
  }

  void forceSafe(uint32_t now) {
    OutputRequest request{};
    request.forceAllSafe = true;
    request.immediateMasterDrop = true;
    update(now, request);
  }

  bool heaterReady(uint32_t now) const {
    return state_.heatMaster && elapsedMs(now, masterOnAt_) >= HEAT_MASTER_PICKUP_MS;
  }
  const OutputState &state() const { return state_; }
  bool popEvent(OutputEvent &event) { return events_.pop(event); }
  bool conflictDetected() const { return outputConflict_; }
  bool relayRateExceeded() const { return relayRateExceeded_; }
  uint16_t transitionsThisHour() const { return transitionsThisHour_; }
  uint32_t droppedEvents() const { return events_.overflowCount(); }
  uint32_t masterRestartWaitMs(uint32_t now) const {
    if (state_.heatMaster || masterLastOffAt_ == 0U) return 0U;
    const uint32_t elapsed = elapsedMs(now, masterLastOffAt_);
    return elapsed >= HEAT_MASTER_MIN_OFF_MS ? 0U
                                             : HEAT_MASTER_MIN_OFF_MS - elapsed;
  }

 private:
  void setHeaterBank(bool enabled, uint32_t now) {
    setImmediate(PIN_OUT_HEATER_SSR, OutputChannel::HeaterSsr,
                 enabled, state_.heaterSsr, now);
  }

  void updateTurnOutputs(uint32_t now, bool wantLeft, bool wantRight,
                         bool forceSafe) {
    outputConflict_ = wantLeft && wantRight;
    if (forceSafe || outputConflict_) {
      const bool wasMoving = state_.turnLeft || state_.turnRight;
      setImmediate(PIN_OUT_TURN_LEFT, OutputChannel::TurnLeft,
                   false, state_.turnLeft, now);
      setImmediate(PIN_OUT_TURN_RIGHT, OutputChannel::TurnRight,
                   false, state_.turnRight, now);
      if (wasMoving) turnInterlockUntil_ = now + TURN_DIRECTION_DEADTIME_MS;
      return;
    }

    if (!wantLeft && !wantRight) {
      const bool wasMoving = state_.turnLeft || state_.turnRight;
      setImmediate(PIN_OUT_TURN_LEFT, OutputChannel::TurnLeft,
                   false, state_.turnLeft, now);
      setImmediate(PIN_OUT_TURN_RIGHT, OutputChannel::TurnRight,
                   false, state_.turnRight, now);
      if (wasMoving) turnInterlockUntil_ = now + TURN_DIRECTION_DEADTIME_MS;
      return;
    }

    if (wantLeft) {
      if (state_.turnRight) {
        setImmediate(PIN_OUT_TURN_RIGHT, OutputChannel::TurnRight,
                     false, state_.turnRight, now);
        turnInterlockUntil_ = now + TURN_DIRECTION_DEADTIME_MS;
      }
      if (!state_.turnRight && timeReached(now, turnInterlockUntil_)) {
        setImmediate(PIN_OUT_TURN_LEFT, OutputChannel::TurnLeft,
                     true, state_.turnLeft, now);
      } else {
        setImmediate(PIN_OUT_TURN_LEFT, OutputChannel::TurnLeft,
                     false, state_.turnLeft, now);
      }
      return;
    }

    if (state_.turnLeft) {
      setImmediate(PIN_OUT_TURN_LEFT, OutputChannel::TurnLeft,
                   false, state_.turnLeft, now);
      turnInterlockUntil_ = now + TURN_DIRECTION_DEADTIME_MS;
    }
    if (!state_.turnLeft && timeReached(now, turnInterlockUntil_)) {
      setImmediate(PIN_OUT_TURN_RIGHT, OutputChannel::TurnRight,
                   true, state_.turnRight, now);
    } else {
      setImmediate(PIN_OUT_TURN_RIGHT, OutputChannel::TurnRight,
                   false, state_.turnRight, now);
    }
  }

  void recordTransition(OutputChannel channel, bool active, uint32_t now) {
    lastTransitionAt_[static_cast<uint8_t>(channel)] = now;
    // Pulse-rated SSR switching is not mechanical relay wear. Preserve the
    // existing alarm threshold for contactor/fan/light/turn/spare transitions.
    if (channel != OutputChannel::HeaterSsr) {
      if (transitionsThisHour_ < UINT16_MAX) ++transitionsThisHour_;
      if (transitionsThisHour_ > MAX_RELAY_TRANSITIONS_PER_HOUR)
        relayRateExceeded_ = true;
    }
    OutputEvent event{};
    event.timestamp = now;
    event.channel = channel;
    event.active = active;
    events_.push(event);
  }

  void setImmediate(uint8_t pin, OutputChannel channel, bool requested,
                    bool &stored, uint32_t now) {
    if (requested == stored) return;
    stored = requested;
    writeLogical(pin, requested);
    recordTransition(channel, requested, now);
  }

  void setMinSwitch(uint8_t pin, OutputChannel channel, bool requested,
                    bool &stored, uint32_t now, uint32_t minimumMs) {
    if (requested == stored) return;
    const uint32_t last = lastTransitionAt_[static_cast<uint8_t>(channel)];
    if (last && elapsedMs(now, last) < minimumMs) return;
    setImmediate(pin, channel, requested, stored, now);
  }

  void setMinOn(uint8_t pin, OutputChannel channel, bool requested,
                bool &stored, uint32_t now, uint32_t minimumOnMs) {
    if (requested == stored) return;
    // Bat la uu tien an toan, khong bi tri hoan.
    if (requested) {
      setImmediate(pin, channel, true, stored, now);
      return;
    }
    const uint32_t last = lastTransitionAt_[static_cast<uint8_t>(channel)];
    if (last && elapsedMs(now, last) < minimumOnMs) return;
    setImmediate(pin, channel, false, stored, now);
  }

  void setMinOff(uint8_t pin, OutputChannel channel, bool requested,
                 bool &stored, uint32_t now, uint32_t minimumOffMs) {
    if (requested == stored) return;
    if (!requested) {
      setImmediate(pin, channel, false, stored, now);
      return;
    }
    const uint32_t last = lastTransitionAt_[static_cast<uint8_t>(channel)];
    if (last && elapsedMs(now, last) < minimumOffMs) return;
    setImmediate(pin, channel, true, stored, now);
  }

  OutputState state_{};
  uint32_t lastTransitionAt_[static_cast<uint8_t>(OutputChannel::Count)]{};
  FixedRing<OutputEvent, OUTPUT_EVENT_QUEUE_SIZE> events_{};
  uint32_t masterOnAt_ = 0U;
  uint32_t masterDropAt_ = 0U;
  uint32_t masterLastOffAt_ = 0U;
  uint32_t turnInterlockUntil_ = 0U;
  uint32_t transitionHourStartedAt_ = 0U;
  uint16_t transitionsThisHour_ = 0U;
  bool outputConflict_ = false;
  bool relayRateExceeded_ = false;
};

// ============================================================================
// LED TRANG THAI SK6812 - CHI CAP NHAT KHI MAU/PHASE THAY DOI
// ============================================================================
enum class LedCode : uint8_t {
  Off, Boot, ReadyAuto, ReadyManual, RunningAuto, RunningManual,
  AutoTune, TempWarning,
  SystemFault, SensorFault, TurnFault, Emergency
};

class StatusLed {
 public:
  void begin() {
    rgbLedWriteOrdered(PIN_STATUS_RGB, LED_COLOR_ORDER_GRB, 0, 0, 0);
    lastCode_ = LedCode::Off;
    lastVisible_ = false;
  }
  void update(uint32_t now, LedCode code) {
    bool visible = true;
    switch (code) {
      case LedCode::Boot: visible = ((now / 500U) & 1U) == 0U; break;
      case LedCode::TempWarning: visible = ((now / 400U) & 1U) == 0U; break;
      case LedCode::SystemFault: visible = ((now / 180U) & 1U) == 0U; break;
      case LedCode::SensorFault: visible = ((now / 250U) & 1U) == 0U; break;
      case LedCode::TurnFault: visible = ((now / 180U) % 6U) < 2U; break;
      case LedCode::Emergency: visible = ((now / 120U) & 1U) == 0U; break;
      default: break;
    }
    if (code == lastCode_ && visible == lastVisible_) return;
    lastCode_ = code; lastVisible_ = visible;
    uint8_t r = 0, g = 0, b = 0;
    if (visible) {
      switch (code) {
        case LedCode::Boot: r = 0; g = 0; b = RGB_BRIGHTNESS_NORMAL; break;
        case LedCode::ReadyAuto: r = 0; g = RGB_BRIGHTNESS_LOW; b = RGB_BRIGHTNESS_LOW; break;
        case LedCode::ReadyManual: r = RGB_BRIGHTNESS_NORMAL; g = RGB_BRIGHTNESS_NORMAL; b = 0; break;
        case LedCode::RunningAuto: r = 0; g = RGB_BRIGHTNESS_NORMAL; b = 0; break;
        case LedCode::RunningManual: r = RGB_BRIGHTNESS_NORMAL; g = RGB_BRIGHTNESS_LOW; b = 0; break;
        case LedCode::AutoTune: r = 0; g = RGB_BRIGHTNESS_LOW; b = RGB_BRIGHTNESS_NORMAL; break;
        case LedCode::TempWarning: r = RGB_BRIGHTNESS_ALARM; g = RGB_BRIGHTNESS_LOW; b = 0; break;
        case LedCode::SystemFault: r = RGB_BRIGHTNESS_ALARM; g = 0; b = RGB_BRIGHTNESS_LOW; break;
        case LedCode::SensorFault: r = RGB_BRIGHTNESS_NORMAL; g = 0; b = RGB_BRIGHTNESS_NORMAL; break;
        case LedCode::TurnFault: r = RGB_BRIGHTNESS_ALARM; g = 0; b = 0; break;
        case LedCode::Emergency: r = RGB_BRIGHTNESS_ALARM; g = 0; b = 0; break;
        default: break;
      }
    }
    rgbLedWriteOrdered(PIN_STATUS_RGB, LED_COLOR_ORDER_GRB, r, g, b);
  }
 private:
  LedCode lastCode_ = LedCode::Off;
  bool lastVisible_ = false;
};

// ============================================================================
// MAY TRANG THAI DAO TRUNG
// ============================================================================
enum class TrayPosition : uint8_t { Unknown, Left, Right };
enum class TurnPhase : uint8_t {
  Idle, DeadtimeLeft, DeadtimeRight, MovingLeft, MovingRight, Fault
};

// ============================================================================
// AUTO TUNE RELAY AN TOAN
// ============================================================================

// ============================================================================
// BO DIEU KHIEN TONG
// ============================================================================
class MachineController {
 public:
  // Bounded local initialization before sensor/batch recovery. No network I/O.
  // Runtime store, RTC, batch and safety algorithms remain the same.
  void beginStorage() {
    if (startupStorageInitialized_) return;
    rtc_.begin(millis());
    startupStoreReady_ = store_.begin();
    startupStorageInitialized_ = true;
  }

  void begin() {
    if (!startupStorageInitialized_) beginStorage();
    bootAt_ = millis();
    eventLog_.begin(bootAt_);
    (void)batchLogger_.begin();
    // Recovery build: khong attach persistent sink vao duong event/control.
    faults_.attachLog(&eventLog_);
    const bool safetyJournalReady = safetyJournal_.begin();
    power_.begin(bootAt_, eventLog_, safetyJournal_);
    safetyJournalFaultLatched_ = !safetyJournalReady || !power_.journalWriteOk();
    resetReason_ = power_.reason();
    abnormalResetLatched_ = power_.ackRequired();
    faults_.set(FaultCode::AbnormalReset, abnormalResetLatched_, bootAt_,
                static_cast<int16_t>(resetReason_));
    faults_.set(FaultCode::SafetyJournalUnavailable,
                safetyJournalFaultLatched_, bootAt_);
    // F-07: khoi phuc khoa "can kiem tra co khi dao" + so loi lien tiep tu
    // NVS - truoc day 2 bien nay luon ve lai false/0 sau moi lan reboot du
    // may that su dang bi khoa cho kiem tra vat ly (xem latchTurnFault()).
    if (safetyJournalReady) {
      turnMechanicalCheckRequired_ = safetyJournal_.turnMechanicalCheckRequired();
      turnFaultStreak_ = safetyJournal_.turnFaultStreak();
      if (turnMechanicalCheckRequired_) {
        faults_.set(FaultCode::TurnMechanicalCheckRequired, true, bootAt_,
                    static_cast<int16_t>(turnFaultStreak_));
      }
      // F-06: khoi phuc xac nhan "tiep tuc u am" cho me qua han (neu co) -
      // tranh hoi lai/bat coi ngay sau reboot du nguoi dung da tra loi truoc do.
      batchOverdueAcknowledged_ = safetyJournal_.batchOverdueAcknowledged();
    }
    outputs_.begin();
    led_.begin();
    // Bao mat dien qua ATtiny13A (mach doc lap dung pin CR2032, xem
    // doc/attiny_power_alarm.md) - pulse-width v4 tren PIN_ATTINY_BUS.
    mayapAttinyBusBegin();
    inputs_.begin();
    sensor_.begin();
    sensorStartupGraceUntil_ = bootAt_ + SENSOR_STARTUP_GRACE_MS;

    const bool storeReady = startupStoreReady_;
    MachineConfig loaded{};
    if (storeReady && store_.loadConfig(loaded)) {
      config_ = loaded;
      configLoaded_ = true;
    } else {
      config_ = MachineConfig{};
      sanitizeMachineConfig(config_);
      MachineConfig verify{};
      configLoaded_ = storeReady && store_.saveConfig(config_, verify);
      if (configLoaded_) config_ = verify;
    }
    storageFaultLatched_ = EXTERNAL_EEPROM_REQUIRED && (!storeReady || !configLoaded_);
    faults_.set(FaultCode::StorageUnavailable, storageFaultLatched_, bootAt_);
    mayapSetConnectivityMode(config_.connectivityMode);
    lastNetworkStatus_ = mayapGetNetworkStatus();
    networkStatusInitialized_ = true;

    if (DISPLAY_BUILD_DATE_WHEN_RTC_MISSING) setCompileDate(runtime_.dateText);
    else snprintf(runtime_.dateText, sizeof(runtime_.dateText), "--/--/----");
    hmiSetConfig(config_);
    mayapRealtimeSetConfig(config_);
    mayapCloudSetConfig(config_);

    PackedBatchV1 batch{};
    const bool hasBatchRecord = storeReady && store_.loadBatch(batch);
    const bool stopIntentPending = safetyJournal_.stopIntentPending();
    if (stopIntentPending) {
      if (hasBatchRecord && !batch.wasRunning) {
        // EEPROM da xac nhan me dung. Tombstone NVS chi duoc xoa sau khi
        // doc lai wasRunning=0 thanh cong.
        if (!safetyJournal_.clearStopIntent()) {
          safetyJournalFaultLatched_ = true;
          faults_.set(FaultCode::SafetyJournalUnavailable, true, bootAt_);
        }
      } else {
        // EEPROM con wasRunning=1 HOAC chua doc duoc. Khong duoc tu phuc
        // hoi lai me da co lenh DUNG/HUY.
        batchClearPending_ = true;
        batchClearRetryAt_ = bootAt_;
      }
    } else if (hasBatchRecord && batch.wasRunning) {
      resumePending_ = true;
      // "Ap lai" BAT: bo qua man hinh hoi CO/HUY ke ca sau mat dien that su,
      // tu dong ap tiep ngay khi cac dieu kien an toan khac (AUTO, cong tac
      // nhiet, cam bien, RTC...) duoc dap ung trong processResume().
      resumeConfirmationRequired_ = resetReasonIsPowerInterruption(resetReason_) &&
                                    !config_.autoResumeOnPowerLoss;
      resumeConfirmPromptAt_ = bootAt_;
      // Ghi nhan "vua co dien lai giua me ap" DOC LAP voi viec co phai hoi
      // xac nhan hay khong (autoResumeOnPowerLoss BAT thi khong hoi, nhung
      // van la mat dien that) - cloud_alert_link.h can co nay de bao ve dien
      // thoai du nguoi dung dang de che do ap lai tu dong.
      powerLossRecovery_ = resetReasonIsPowerInterruption(resetReason_);
      automaticResetRecovery_ = resetReasonIsAutomaticRecovery(resetReason_);
      elapsedBeforeStartSec_ = batch.elapsedSec;
      savedElapsedAtCheckpoint_ = batch.elapsedSec;
      lastCheckpointEpoch_ = batch.checkpointEpoch;
      batchStartEpoch_ = batch.batchStartEpoch;
      lastTurnEpoch_ = batch.lastTurnEpoch;
      lastTurnAt_ = 0U;
      resumeClockAdjusted_ = batch.checkpointEpoch == 0U;
      turnCountToday_ = batch.turnCountToday;
      turnCountBatch_ = batch.turnCountBatch;
      // F-15: khoi tao lastTurnCounterDay_ DUNG ngay-tai-thoi-diem-checkpoint
      // (cung cong thuc voi updateBatchTime(): dayIndex = giay da qua/86400)
      // thay vi de mac dinh 0 - truoc day, nhip updateBatchTime() DAU TIEN sau
      // reboot luon roi vao nhanh "lastTurnCounterDay_ == 0U" va am tham NHAN
      // ngay hien tai lam moc, bo qua viec mat dien co the da lam TRAI QUA
      // MOT (hoac nhieu) RANH GIOI NGAY - khien turnCountToday_ phuc hoi tu
      // EEPROM (co the la so dem cua NGAY HOM QUA) bi giu nguyen dai qua ngay
      // moi thay vi duoc reset ve 0 dung luc.
      lastTurnCounterDay_ = elapsedBeforeStartSec_ / 86400UL + 1U;
      config_.nextDirection = static_cast<TurnDirection>(batch.nextDirection <= 1U
          ? batch.nextDirection : 0U);
      if (batchStartEpoch_ != 0U) {
        batchLogFaultActive_ = !batchLogger_.start(batchStartEpoch_, true);
      }
    }
    faults_.set(FaultCode::BatchStateClearPending, batchClearPending_, bootAt_);
    // Nhat ky chi ghi trong pham vi mot me: dang chay hoac dang cho phuc hoi
    // (ke ca dang cho xac nhan HMI) deu tinh la "trong me".
    eventLog_.setLoggingEnabled(batchRunning_ || resumePending_);

    heatRestartNotBefore_ = bootAt_ +
        static_cast<uint32_t>(config_.powerRestoreDelaySec) * 1000U;
    previousAutoMode_ = inputs_.state().autoMode;
    runtime_.alarmMask = faults_.alarmMask();
    runtime_.sensorStartupGrace = true;
    runtime_.resumeConfirmationRequired = resumeConfirmationRequired_;
    runtime_.powerLossRecovery = powerLossRecovery_;
    copyRuntimeToHmi();

    mayapSerialPrintf(false,
        "\n=== MAYAP INDUSTRIAL v%s | HW=%s | HMI=%s/%s ===\n",
        MAYAP_FIRMWARE_VERSION, MAYAP_HARDWARE_REVISION,
        HMI_FIRMWARE_VERSION, HMI_HARDWARE_REVISION);
    mayapSerialPrintf(false, "[BOOT] reset=%s config=%s resume=%u input=%s\n",
                     resetReasonText(resetReason_),
                     configLoaded_ ? "EEPROM" : "DEFAULT",
                     resumePending_, MAYAP_SERIAL_INPUT_SIM ? "SERIAL_SIM" : "GPIO_REAL");
    if (batchClearPending_) {
      mayapSerialPrintf(false,
          "[BOOT] STOP INTENT PENDING - KHONG TU PHUC HOI ME CU\n");
    }
    if (resumeConfirmationRequired_) {
      eventLog_.push(bootAt_, EventType::Recovery,
                     static_cast<uint16_t>(EventCode::ResumePrompt));
      mayapSerialPrintf(false, "[BOOT] MAT DIEN GIUA ME - CHO XAC NHAN HMI\n");
    } else if (automaticResetRecovery_ && resumePending_) {
      mayapSerialPrintf(false, "[BOOT] RESET HE THONG - TU PHUC HOI ME TU EEPROM\n");
    }
    printConfig();
    if (mayapSerialDebugEnabled()) printSerialHelp();
  }

  void update(uint32_t now) {
    power_.service(now);
    if (!power_.journalWriteOk()) {
      safetyJournalFaultLatched_ = true;
    }
    inputs_.update(now);
    processInputEvents(now);
    serviceSerial(now);
    if (networkPollGate_.due(now, true)) processNetworkState(now);
    sensor_.update();
    processSensor(now);
    rtc_.update(now);
    serviceI2cDeviceRecovery(now);
    serviceBatchClear(now);
    eventLog_.syncClock(now, rtc_.epoch());
    faults_.set(FaultCode::RtcFailure, !rtc_.valid(), now,
                rtc_.online() ? (rtc_.oscillatorStopped() ? 1 : 3) : 2);
    adjustResumeElapsedFromRtc(now);
    const bool operationsReady = mayapBootOperationsReady();
    if (operationsReady) processInputModeTransition(now);
    processHmiTransactions(now);
    if (operationsReady) {
      updateTestMode(now);
      processResume(now);
    }
    updateAlarms(now);
    if (operationsReady) {
      updateBatchOverdue(now);
      updateSirenSelfTest(now);
      updateAutoTune(now);
      updateTurning(now);
      updateHeatingAndOutputs(now);
    } else {
      OutputRequest startup{};
      mayapApplyStartupOutputPolicy(startup, false,
          emergencyActive_ || highTemperatureActive_,
          emergencyActive_ && timeReached(now, sirenMutedUntil_));
      outputs_.update(now, startup);
      pid_.reset();
    }
    processOutputEvents(now);
    syncOutputFaults(now);
    // ATtiny activity phai doc OUTPUT THUC TE cua chinh chu ky nay.
    updateAttinyLink(now);
    updateBatchTime(now);
    serviceBatchLog(now);
    const OutputState &maintenanceOut = outputs_.state();
    mayapSetFirmwareMaintenanceReady(!batchRunning_ && !resumePending_ &&
        !batchClearPending_ && !autotune_.running() && !testModeActive_ &&
        !maintenanceOut.heaterSsr && !maintenanceOut.heatMaster &&
        !maintenanceOut.turnLeft && !maintenanceOut.turnRight &&
        !highTemperatureActive_ && !emergencyActive_);
    serviceHealthMonitor(now);
    // Lich su nhiet tach khoi Flash ESP32/Cloud: chi ghi AT24C32 moi 5 phut.
    // Ham tu bo qua neu RTC/cam bien khong hop le va tu tranh ghi lap sau reboot.
    mayapTemperatureHistorySample(rtc_.valid() ? rtc_.epoch() : 0U,
                                  temperature_, sensorUsable_);
    updateLed(now);
    if (runtimeGate_.due(now, true)) copyRuntimeToHmi();
    if (checkpointGate_.due(now, false)) checkpointBatch();
    diagnosticGate_.setPeriod(diagnosticFast_ ? DIAGNOSTIC_FAST_STATUS_MS
                                              : DIAGNOSTIC_STATUS_MS);
    if (diagnosticGate_.due(now, false)) printStatus(now, false);
  }

  const MachineConfig &config() const { return config_; }
  const MachineRuntime &runtime() const { return runtime_; }
  const OutputState &outputs() const { return outputs_.state(); }

 private:
  enum class BatchPhase : uint8_t { Stopped, Prestart, Homing, Running };
  enum class ResumeBlockReason : uint8_t {
    None = 0, Confirmation, BatchClear, Storage, ResetAck, AutoMode,
    HeaterSwitch, Sensor, Rtc, TurnFault, LimitConflict, Temperature, Delay,
    TurningDisabled
  };

  // Heap pressure is diagnostic and sheds only Online workloads. Local
  // control, fault thresholds and output arbitration are unchanged.
  void serviceHealthHeap(uint32_t now, uint32_t recoveredHeap) {
    if (!healthBaselineCaptured_) return;
    const uint32_t freeHeap = ESP.getFreeHeap();
    const uint32_t instantPercent = static_cast<uint32_t>(
        (static_cast<uint64_t>(freeHeap) * 100ULL) / healthHeapBaseline_);
    const uint32_t percent = static_cast<uint32_t>(
        (static_cast<uint64_t>(recoveredHeap) * 100ULL) / healthHeapBaseline_);

    // Critical memory pressure closes Online admission, never the controller.
    if (instantPercent <= HEALTH_HEAP_CRITICAL_PERCENT) {
      faults_.set(FaultCode::HeapCritical, true, now, static_cast<int16_t>(instantPercent));
      mayapSetOnlineMemoryPressure(true);
      mayapSerialPrintf(false,
          "[HEALTH] Heap NGUY CAP %lu%% (%lu/%lu bytes) - tam dung Online, MAY VAN CHAY\n",
          static_cast<unsigned long>(instantPercent), static_cast<unsigned long>(freeHeap),
          static_cast<unsigned long>(healthHeapBaseline_));
      return;
    }
    faults_.set(FaultCode::HeapCritical, false, now);

    // Sustained pressure also pauses Online; no reset is scheduled.
    if (percent <= HEALTH_HEAP_SERIOUS_PERCENT) {
      if (healthHeapSeriousStreak_ < UINT8_MAX) ++healthHeapSeriousStreak_;
    } else {
      healthHeapSeriousStreak_ = 0U;
    }
    if (healthHeapSeriousStreak_ >= HEALTH_STREAK_CONFIRM) {
      mayapSetOnlineMemoryPressure(true);
      mayapSerialPrintf(false,
          "[HEALTH] Heap thap keo dai %lu%% - tam dung Online, MAY VAN CHAY\n",
          static_cast<unsigned long>(percent));
    }

    // Muc 1 - canh bao som, co tre (hysteresis 30%/35%) de khong nhay lien
    // tuc quanh nguong khi heap dao dong nhe quanh muc canh bao.
    if (!healthHeapLowActive_) {
      if (percent <= HEALTH_HEAP_WARN_PERCENT) {
        if (healthHeapLowStreak_ < UINT8_MAX) ++healthHeapLowStreak_;
      } else {
        healthHeapLowStreak_ = 0U;
      }
      if (healthHeapLowStreak_ >= HEALTH_STREAK_CONFIRM) healthHeapLowActive_ = true;
    } else if (percent >= HEALTH_HEAP_WARN_CLEAR_PERCENT) {
      healthHeapLowActive_ = false;
      healthHeapLowStreak_ = 0U;
    }
    faults_.set(FaultCode::HeapLow, healthHeapLowActive_, now, static_cast<int16_t>(percent));
  }

  // Du doan nhiet do: ngoai suy toc do thay doi hien tai (do rieng, cua so
  // HEALTH_TEMP_TREND_WINDOW_MS - KHAC voi tempRateRefValue_/tempRateWindowSec
  // dung phat hien thay doi DOT NGOT) - neu voi toc do nay se cham nguong
  // Bao cao/Bao thap trong vong HEALTH_TEMP_TREND_LOOKAHEAD_MIN phut nua thi
  // bao TRUOC, khong doi that su cham nguong roi HighTemperature/LowTemperature
  // moi bao nhu truoc day.
  void serviceHealthTemperatureTrend(uint32_t now) {
    bool warn = false;
    int16_t warnDetail = 0;
    const uint32_t baselineAgeMs = elapsedMs(now, healthTempBaselineAt_);
    if (batchRunning_ && sensorUsable_ && isfinite(temperature_) &&
        isfinite(healthTempBaseline_) && baselineAgeMs > 0U) {
      const float windowMin = static_cast<float>(baselineAgeMs) / 60000.0f;
      const float ratePerMin = (temperature_ - healthTempBaseline_) / windowMin;
      if (ratePerMin >= HEALTH_TEMP_TREND_MIN_RATE_C_PER_MIN &&
          temperature_ < config_.highTempAlarm) {
        const float minutesToHigh = (config_.highTempAlarm - temperature_) / ratePerMin;
        if (minutesToHigh > 0.0f && minutesToHigh <= HEALTH_TEMP_TREND_LOOKAHEAD_MIN) {
          warn = true;
          warnDetail = static_cast<int16_t>(lroundf(minutesToHigh));
        }
      } else if (ratePerMin <= -HEALTH_TEMP_TREND_MIN_RATE_C_PER_MIN &&
                 temperature_ > config_.lowTempAlarm) {
        const float minutesToLow = (temperature_ - config_.lowTempAlarm) / (-ratePerMin);
        if (minutesToLow > 0.0f && minutesToLow <= HEALTH_TEMP_TREND_LOOKAHEAD_MIN) {
          warn = true;
          // Am = dang huong xuong Bao thap (phan biet voi huong len Bao cao).
          warnDetail = static_cast<int16_t>(-lroundf(minutesToLow));
        }
      }
    }
    faults_.set(FaultCode::TemperatureTrendWarning, warn, now, warnDetail);

    if (!isfinite(healthTempBaseline_) ||
        baselineAgeMs >= HEALTH_TEMP_TREND_WINDOW_MS) {
      healthTempBaseline_ = (sensorUsable_ && isfinite(temperature_))
                                 ? temperature_ : NAN;
      healthTempBaselineAt_ = now;
    }
  }

  // EEPROM phai thu lai (retry) qua nhieu lan trong 1 chu ky kiem tra - dau
  // hieu suy giam som cua chip nho truoc khi hong han. Bo dem tu xoa ve 0
  // moi chu ky (xem ExternalEeprom24xx::takeSoftRetryEvents()).
  void serviceHealthStorageRetry(uint32_t now) {
    if (!EXTERNAL_EEPROM_ENABLED) return;
    const uint32_t retries = store_.takeEepromSoftRetryEvents();
    faults_.set(FaultCode::StorageRetryTrend,
                retries >= HEALTH_EEPROM_RETRY_WARN_COUNT, now,
                static_cast<int16_t>(std::min<uint32_t>(retries, INT16_MAX)));
  }

  void serviceHealthMonitor(uint32_t now) {
    const uint32_t bootAge = elapsedMs(now, bootAt_);
    // Loai nua dau warm-up khoi baseline: Wi-Fi va MQTT van dang khoi dong.
    if (!healthBaselineCaptured_ && !healthWarmupWindowStarted_ &&
        bootAge >= HEALTH_BASELINE_CAPTURE_DELAY_MS / 2U) {
      healthWarmupWindowStarted_ = true;
      healthHeapWindowBest_ = 0U;
      healthHeapWindowWorst_ = UINT32_MAX;
    }
    if (healthHeapSampleGate_.due(now, true)) {
      const uint32_t freeHeap = ESP.getFreeHeap();
      if (freeHeap < 16384U) mayapSetOnlineMemoryPressure(true);
      else if (freeHeap >= 73728U && ESP.getMaxAllocHeap() >= 24576U)
        mayapSetOnlineMemoryPressure(false);
      healthHeapWindowBest_ = std::max(healthHeapWindowBest_, freeHeap);
      healthHeapWindowWorst_ = std::min(healthHeapWindowWorst_, freeHeap);
    }
    if (!healthBaselineCaptured_) {
      // Doi he thong chay on dinh sau boot roi moi chup heap nen, bo qua
      // bien dong tam thoi luc vua khoi dong Wi-Fi/MQTT/OTA.
      if (bootAge < HEALTH_BASELINE_CAPTURE_DELAY_MS) return;
      healthHeapBaseline_ = healthHeapWindowBest_;
      healthBaselineCaptured_ = healthHeapBaseline_ > 0U;
      mayapSerialPrintf(false, "[HEALTH] Heap nen = %lu bytes\n",
                        static_cast<unsigned long>(healthHeapBaseline_));
      healthHeapWindowBest_ = 0U;
      healthHeapWindowWorst_ = UINT32_MAX;
      healthCheckGate_.reset(now);
      (void)healthCheckGate_.due(now, false);
      return;
    }
    if (!healthCheckGate_.due(now, false)) return;
    // E401/E402 nghiem trong chi tinh tu muc heap DA PHUC HOI trong 30 giay,
    // khong tu mot mau ngau nhien trung luc HTTPS cap phat bo nho. Muc nguy cap
    // 6% van kiem tra heap hien tai de bao ve truong hop that su sap het RAM.
    healthHeapLastBest_ = healthHeapWindowBest_;
    healthHeapLastWorst_ = healthHeapWindowWorst_ == UINT32_MAX
        ? ESP.getFreeHeap() : healthHeapWindowWorst_;
    serviceHealthHeap(now, healthHeapLastBest_ ? healthHeapLastBest_ : ESP.getFreeHeap());
    healthHeapWindowBest_ = 0U;
    healthHeapWindowWorst_ = UINT32_MAX;
    serviceHealthTemperatureTrend(now);
    serviceHealthStorageRetry(now);
  }

  // ------------------------- I2C device recovery -----------------------------
  void serviceI2cDeviceRecovery(uint32_t now) {
    // Gio moi tu NTP (networkTask, xem serviceNtpSync() trong network_service.h)
    // - nguon phuc hoi THU HAI, doc lap voi RTC_AUTO_REPAIR_* o tren: giup khi
    // module DS3231 hong that/mat dong ho bong (ca 2 co che tren deu bo tay),
    // dong thoi sua troi dat nho tich luy giua cac lan neu RTC van dang chay
    // binh thuong. Ap dung TRUOC khoi RTC auto-repair ben duoi de khong tranh
    // chap (rtc_.set() tu dat lai autoRepairAttempts_/invalidReadConfirm_).
    uint16_t ntpYear; uint8_t ntpMonth, ntpDay, ntpHour, ntpMinute, ntpSecond;
    if (mayapTakePendingNtpTime(ntpYear, ntpMonth, ntpDay, ntpHour, ntpMinute,
                                 ntpSecond)) {
      const bool ok = rtc_.set(ntpYear, ntpMonth, ntpDay, ntpHour, ntpMinute,
                                ntpSecond);
      if (ok) {
        eventLog_.push(now, EventType::Recovery,
                       static_cast<uint16_t>(EventCode::RtcNtpSynced));
      }
      mayapSerialPrintf(false, "[RTC] NTP SYNC %s -> %04u-%02u-%02u %02u:%02u:%02u\n",
          ok ? "OK" : "FAIL", ntpYear, ntpMonth, ntpDay, ntpHour, ntpMinute,
          ntpSecond);
    }

    const bool rtcAutoRepaired = rtc_.takeAutoRepairNotice();
    const bool rtcReconnected = rtc_.takeReconnectNotice();
    if (rtcAutoRepaired) {
      eventLog_.push(now, EventType::Recovery,
                     static_cast<uint16_t>(EventCode::RtcAutoRepaired));
      mayapSerialPrintf(false,
          "[RTC] AUTO REPAIR OK addr=0x%02X epoch=%lu\n",
          RTC_I2C_ADDRESS, static_cast<unsigned long>(rtc_.epoch()));
    } else if (rtcReconnected) {
      eventLog_.push(now, EventType::Recovery,
                     static_cast<uint16_t>(EventCode::RtcReconnected));
      mayapSerialPrintf(false,
          "[RTC] CONNECTION RESTORED addr=0x%02X valid=%u osf=%u\n",
          RTC_I2C_ADDRESS, rtc_.valid(), rtc_.oscillatorStopped());
    }

    if (!EXTERNAL_EEPROM_ENABLED) return;

    // Kiem tra dia chi 0x57 dinh ky, rat nhe va co timeout. Hai/ba lan loi
    // lien tiep moi nang muc canh bao, tranh mot xung nhieu I2C lam bao gia.
    if (store_.ready() && !storageFaultLatched_ && !storageDegraded_ &&
        elapsedMs(now, lastStorageHealthCheckAt_) >= EEPROM_HEALTH_CHECK_MS) {
      lastStorageHealthCheckAt_ = now;
      if (!store_.probe()) {
        store_.markOffline();
        latchStorageFault("HEALTH CHECK");
      } else {
        storageFailureStreak_ = 0U;
      }
    }

    const bool needRecovery = storageFaultLatched_ || storageDegraded_ ||
                              !store_.ready();
    if (!needRecovery) return;
    if (elapsedMs(now, lastStorageReconnectAt_) < EEPROM_RECONNECT_PERIOD_MS) {
      return;
    }
    lastStorageReconnectAt_ = now;

    if (!store_.reconnect()) return;

    bool verified = false;
    MachineConfig recovered{};

    if (batchRunning_ || resumePending_) {
      // Dang chay me: RAM la nguon dung. Khong tai cau hinh cu tu EEPROM vao
      // giua me; ghi lai cau hinh hien tai de phuc hoi kha nang checkpoint.
      MachineConfig readback{};
      verified = store_.saveConfig(config_, readback);
      if (verified) {
        config_ = readback;
        configLoaded_ = true;
        mayapSetConnectivityMode(config_.connectivityMode);
      }
    } else if (store_.loadConfig(recovered)) {
      // May dang dung: doc lap lai de chac chan module vua lap lai on dinh,
      // sau do moi dua cau hinh EEPROM vao RAM/HMI.
      PackedMachineConfigV1 first = packConfig(recovered);
      verified = true;
      for (uint8_t i = 1U; i < EEPROM_RECOVERY_VERIFY_COUNT; ++i) {
        MachineConfig second{};
        if (!store_.loadConfig(second)) {
          verified = false;
          break;
        }
        const PackedMachineConfigV1 secondPacked = packConfig(second);
        if (memcmp(&first, &secondPacked, sizeof(first)) != 0) {
          verified = false;
          break;
        }
      }
      if (verified) {
        config_ = recovered;
        sanitizeMachineConfig(config_);
        configLoaded_ = true;
        hmiSetConfig(config_);
        mayapRealtimeSetConfig(config_);
        mayapCloudSetConfig(config_);
        mayapSetConnectivityMode(config_.connectivityMode);
        pid_.applyConfigBumpless(now, config_.targetTemp, temperature_, config_);
      }
    } else {
      // EEPROM trang/du lieu cu hong: chi khoi tao lai khi may dang dung.
      MachineConfig readback{};
      verified = store_.saveConfig(config_, readback);
      if (verified) {
        config_ = readback;
        configLoaded_ = true;
        hmiSetConfig(config_);
        mayapRealtimeSetConfig(config_);
        mayapCloudSetConfig(config_);
        mayapSetConnectivityMode(config_.connectivityMode);
      }
    }

    if (!verified) {
      store_.markOffline();
      return;
    }

    storageFailureStreak_ = 0U;
    lastStorageHealthCheckAt_ = now;
    storageDegraded_ = false;
    storageFaultLatched_ = false;
    (void)faults_.clearRecovered(FaultCode::StorageDegraded, now);
    (void)faults_.clearRecovered(FaultCode::StorageUnavailable, now);
    eventLog_.push(now, EventType::Recovery,
                   static_cast<uint16_t>(EventCode::StorageReconnected));
    mayapSerialPrintf(false,
        "[EEPROM] CONNECTION RESTORED addr=0x%02X config=OK\n",
        EEPROM_I2C_ADDRESS);

    if (batchRunning_ || resumePending_) {
      // Ghi checkpoint ngay sau khi module quay lai. Neu that bai, ham nay se
      // dua EEPROM ve trang thai suy giam mot cach co kiem soat.
      (void)saveBatchRecord();
    }
  }

  // ----------------------------- Event kernel --------------------------------
  void processInputEvents(uint32_t now) {
    InputEvent event{};
    uint8_t budget = INPUT_EVENT_QUEUE_SIZE;
    while (budget-- && inputs_.popEvent(event)) {
      const uint16_t code = static_cast<uint16_t>(EventCode::InputBase) +
                            static_cast<uint8_t>(event.channel);
      eventLog_.push(now, EventType::InputChanged, code,
                     event.active ? 1 : 0);
      mayapSerialPrintf(false, "[IN] %s=%s\n", InputManager::name(event.channel),
                       event.active ? "ON" : "OFF");
    }
  }

  void processOutputEvents(uint32_t now) {
    OutputEvent event{};
    uint8_t budget = OUTPUT_EVENT_QUEUE_SIZE;
    while (budget-- && outputs_.popEvent(event)) {
      const uint16_t code = static_cast<uint16_t>(EventCode::OutputBase) +
                            static_cast<uint8_t>(event.channel);
      // Khong dua xung SSR vao nhat ky HMI: PID co the doi moi vai giay va
      // se day mat cac lenh/loi quan trong. Serial van co the xem khi debug.
      if (event.channel != OutputChannel::HeaterSsr &&
          event.channel != OutputChannel::Humidifier) {
        eventLog_.push(now, EventType::OutputChanged, code,
                       event.active ? 1 : 0);
      }
      mayapSerialPrintf(false, "[OUT] %s=%s\n", outputName(event.channel),
                       event.active ? "ON" : "OFF");
    }
  }

  void syncOutputFaults(uint32_t now) {
    faults_.set(FaultCode::OutputConflict, outputs_.conflictDetected(), now);
    faults_.set(FaultCode::RelayRateExceeded, outputs_.relayRateExceeded(), now,
                static_cast<int16_t>(std::min<uint16_t>(
                    outputs_.transitionsThisHour(), INT16_MAX)));
    runtime_.alarmMask = faults_.alarmMask();
  }

  void processNetworkState(uint32_t now) {
    const NetworkStatus status = mayapGetNetworkStatus();
    if (!networkStatusInitialized_) {
      lastNetworkStatus_ = status;
      networkStatusInitialized_ = true;
      return;
    }
    if (status.requestedMode == lastNetworkStatus_.requestedMode &&
        status.state == lastNetworkStatus_.state &&
        status.connected == lastNetworkStatus_.connected) {
      return;
    }

    // Sau khi luu ONLINE, networkTask co toi 250 ms de nhan yeu cau. Khong
    // ghi sai su kien CONNECTING trong khoang published state con OFFLINE.
    if (status.requestedMode == ConnectivityMode::Online &&
        status.state == NetworkStateCode::Offline) {
      lastNetworkStatus_ = status;
      return;
    }

    EventCode code = EventCode::NetworkConnecting;
    if (status.requestedMode == ConnectivityMode::Offline) {
      code = EventCode::NetworkModeOffline;
    } else if (status.state == NetworkStateCode::NotConfigured) {
      code = EventCode::NetworkConfigMissing;
    } else if (status.state == NetworkStateCode::Connected) {
      code = EventCode::NetworkConnected;
    } else if (lastNetworkStatus_.connected) {
      code = EventCode::NetworkDisconnected;
    }
    eventLog_.push(now, EventType::Network, static_cast<uint16_t>(code),
                   status.connected ? status.rssiDbm : 0);
    mayapSerialPrintf(false, "[NET] mode=%s state=%u connected=%u rssi=%d\n",
        status.requestedMode == ConnectivityMode::Online ? "ONLINE" : "OFFLINE",
        static_cast<unsigned>(status.state), status.connected,
        static_cast<int>(status.rssiDbm));
    lastNetworkStatus_ = status;
  }


  // ----------------------------- Sensor --------------------------------------
  void processSensor(uint32_t now) {
    const bool wasUsable = sensorUsable_;
    const bool newData = sensor_.takeNewData();
    if (newData) {
      const float candidateTemp = sensor_.temperatureC() + config_.tempOffset;
      const float candidateHum = clampFloat(
          sensor_.humidityRH() + config_.humidityOffset, 0.0f, 100.0f);
      const float rawSensorTemp = sensor_.rawTemperatureC();
      // Bu am co the can cho calibration/PID nhung KHONG duoc ha nguong bao
      // ve qua nhiet. Bu duong van duoc ap dung theo huong fail-safe.
      const float candidateRawCorrected = rawSensorTemp + config_.tempOffset;
      const float candidateRaw = std::max(rawSensorTemp, candidateRawCorrected);
      const bool frameValid = sensor_.dataValid() && isfinite(candidateTemp) &&
                              isfinite(candidateHum) && isfinite(rawSensorTemp) &&
                              isfinite(candidateRaw);
      latestFrameValid_ = frameValid;

      // Gia tri raw duoc cap nhat ngay de nhanh bao ve qua nhiet khong bi bo qua.
      // Gia tri loc dung cho PID chi duoc chap nhan sau kiem tra tinh hop ly.
      if (frameValid) {
        rawTemperature_ = candidateRaw;
        humidity_ = candidateHum;
      }

      bool acceptSample = frameValid;
      if (frameValid && isfinite(lastAcceptedTemperature_) &&
          candidateTemp < lastAcceptedTemperature_ - SENSOR_MAX_DOWN_STEP_C) {
        acceptSample = false;
        if (!sensorSuspect_) {
          sensorSuspect_ = true;
          sensorPlausibilityStreak_ = 1U;
        } else if (isfinite(lastSuspectCandidate_) &&
                   fabsf(candidateTemp - lastSuspectCandidate_) <=
                       SENSOR_PLAUSIBILITY_MATCH_C) {
          if (sensorPlausibilityStreak_ < UINT8_MAX) ++sensorPlausibilityStreak_;
        } else {
          sensorPlausibilityStreak_ = 1U;
        }
        lastSuspectCandidate_ = candidateTemp;
        if (sensorPlausibilityStreak_ >= SENSOR_PLAUSIBILITY_CONFIRM_SAMPLES) {
          acceptSample = true;
          sensorSuspect_ = false;
          sensorPlausibilityStreak_ = 0U;
        }
      } else if (frameValid && sensorSuspect_) {
        // Neu mau quay lai vung hop ly, van yeu cau nhieu mau lien tiep de
        // tranh mot frame don le lam mo khoa nhiet qua som.
        if (sensorPlausibilityStreak_ < UINT8_MAX) ++sensorPlausibilityStreak_;
        if (sensorPlausibilityStreak_ >= SENSOR_PLAUSIBILITY_CONFIRM_SAMPLES) {
          sensorSuspect_ = false;
          sensorPlausibilityStreak_ = 0U;
          acceptSample = true;
        } else {
          acceptSample = false;
        }
      }

      if (acceptSample) {
        temperature_ = candidateTemp;
        lastAcceptedTemperature_ = candidateTemp;
        lastSuspectCandidate_ = candidateTemp;
        if (goodSensorStreak_ < UINT8_MAX) ++goodSensorStreak_;
        newSensorSample_ = true;
        // F-08: theo doi moc "gia tri vua thay doi that su" - moi lan mau
        // moi lech qua SENSOR_FROZEN_EPSILON_C so voi moc hien tai thi doi
        // moc va reset dong ho; neu khong, cu de nguyen (dang "dung hinh" o
        // gan moc do). isnan(sensorFrozenRefTemp_) chi dung cho mau dau tien.
        if (!isfinite(sensorFrozenRefTemp_) ||
            fabsf(candidateTemp - sensorFrozenRefTemp_) > SENSOR_FROZEN_EPSILON_C) {
          sensorFrozenRefTemp_ = candidateTemp;
          sensorFrozenSince_ = now;
        }
      } else {
        goodSensorStreak_ = 0U;
        newSensorSample_ = false;
      }
    }

    // Fail-safe freshness is owned by SHT485Industrial::dataValid()/online
    // policy. sensorTimeoutSec is retained only for EEPROM/protocol compatibility.
    const bool transportValid = sensor_.dataValid() &&
                                latestFrameValid_ && isfinite(rawTemperature_) &&
                                isfinite(humidity_);
    safetySampleValid_ = transportValid;
    if (!transportValid) goodSensorStreak_ = 0U;
    sensorUsable_ = transportValid && !sensorSuspect_ &&
                    isfinite(temperature_) &&
                    goodSensorStreak_ >= SENSOR_RECOVERY_GOOD_SAMPLES;

    if (wasUsable != sensorUsable_) {
      pid_.reset();
      heatRestartNotBefore_ = now + HEAT_RESTART_LOCKOUT_MS;
      if (!sensorUsable_) postCoolUntil_ = now + POST_COOL_MS;
      // F-08: cam bien vua mat/phuc hoi - xoa moc dong bang cu, tranh mang
      // theo mot moc gia tri thuoc ve LAN CAM BIEN KHAC (truoc khi mat/sau
      // khi phuc hoi) sang lan nay.
      sensorFrozenRefTemp_ = NAN;
      sensorFrozenSince_ = now;
      eventLog_.push(now,
          sensorUsable_ ? EventType::SensorRestored : EventType::SensorLost,
          static_cast<uint16_t>(sensorUsable_ ? EventCode::SensorOnline
                                              : EventCode::SensorOffline),
          sensorSuspect_ ? 1 : 0, 0U);
      mayapSerialPrintf(false, "[SHT] CONTROL %s%s\n",
          sensorUsable_ ? "READY" : "BLOCKED",
          sensorSuspect_ ? " (PLAUSIBILITY)" : "");
    }

    SHT485Event event;
    while (sensor_.popEvent(event)) {
      switch (event) {
        case SHT485Event::PresentAtStartup: mayapSerialPrintf(false, "[SHT] CO CAM BIEN KHI KHOI DONG\n"); break;
        case SHT485Event::MissingAtStartup: mayapSerialPrintf(false, "[SHT] MAT CAM BIEN KHI KHOI DONG\n"); break;
        case SHT485Event::Lost: mayapSerialPrintf(false, "[SHT] MAT KET NOI\n"); break;
        case SHT485Event::Restored: mayapSerialPrintf(false, "[SHT] DA PHUC HOI\n"); break;
        default: break;
      }
    }
  }

  // ----------------------------- HMI/EEPROM ----------------------------------
  void processHmiTransactions(uint32_t now) {
    MachineConfig requested{};
    uint32_t transactionId = 0;
    if (hmiTakeSavedConfig(requested, transactionId)) {
      sanitizeMachineConfig(requested);
      // nextDirection la trang thai scheduler noi bo, HMI khong duoc ghi de.
      requested.nextDirection = config_.nextDirection;
      // turningEnabled/turnIntervalMin/turnMaxRunSec KHONG con bi khoa khi
      // dang co me nua (theo dung UI HMI/web da mo khoa cho "phan dao" -
      // Trong me/resume chi mo cac tham so VAN HANH: SV, bao am, nguong
      // quat hut, chu ky/bat-tat dao, dao tay dong lich va cho phep ONLINE->
      // OFFLINE. Calibration, nguong safety, PID/tuning, timeout co khi,
      // sensor/recovery va OFFLINE->ONLINE deu bi khoa o lop luu that su nay
      // de web/Serial/HMI khong the di vong qua khoa giao dien.
      const float targetDelta = requested.targetTemp - config_.targetTemp;
      const bool targetChanged = fabsf(targetDelta) > 0.0005f;
      const auto shiftedWithTarget = [targetDelta](float before, float after) {
        return fabsf((after - before) - targetDelta) <= 0.0015f;
      };
      const bool thermalEnvelopeFollowsTarget = targetChanged &&
          shiftedWithTarget(config_.lowTempAlarm, requested.lowTempAlarm) &&
          shiftedWithTarget(config_.highTempAlarm, requested.highTempAlarm) &&
          shiftedWithTarget(config_.emergencyTemp, requested.emergencyTemp) &&
          shiftedWithTarget(config_.ventOnTemp, requested.ventOnTemp) &&
          shiftedWithTarget(config_.ventOffTemp, requested.ventOffTemp);
      const bool directSafetyThresholdChange =
          requested.lowTempAlarm != config_.lowTempAlarm ||
          requested.highTempAlarm != config_.highTempAlarm ||
          requested.emergencyTemp != config_.emergencyTemp;
      // Trong me, SV duoc phep doi. Neu SV doi, chi cho phep ba nguong safety
      // va hai nguong quat hut di chuyen CUNG delta de giu nguyen khoang cach;
      // moi kieu sua safety truc tiep khac van bi khoa.
      const bool thermalEnvelopeViolation = targetChanged
          ? !thermalEnvelopeFollowsTarget
          : directSafetyThresholdChange;
      const bool pidAuthorityValid = mayapPidHasAuthority(requested);
      const bool protectedBatchChange = (batchRunning_ || resumePending_) && (
          requested.autoResumeOnPowerLoss != config_.autoResumeOnPowerLoss ||
          requested.totalIncubationDays != config_.totalIncubationDays ||
          thermalEnvelopeViolation ||
          requested.tempOffset != config_.tempOffset ||
          requested.humidityOffset != config_.humidityOffset ||
          requested.highTempAlarmWithoutBatch != config_.highTempAlarmWithoutBatch ||
          requested.kp != config_.kp || requested.ki != config_.ki ||
          requested.kd != config_.kd ||
          requested.pidCycleSec != config_.pidCycleSec ||
          requested.maxHeaterPower != config_.maxHeaterPower ||
          requested.heaterStuckMinRiseC != config_.heaterStuckMinRiseC ||
          requested.heaterStuckDurationSec != config_.heaterStuckDurationSec ||
          requested.tempRateLimitC != config_.tempRateLimitC ||
          requested.tempRateWindowSec != config_.tempRateWindowSec ||
          requested.tempOscillationCrossLimit != config_.tempOscillationCrossLimit ||
          requested.tempOscillationWindowSec != config_.tempOscillationWindowSec ||
          requested.autotuneRelayPowerPercent != config_.autotuneRelayPowerPercent ||
          requested.autotuneBandC != config_.autotuneBandC ||
          requested.turnMaxRunSec != config_.turnMaxRunSec ||
          requested.powerRestoreDelaySec != config_.powerRestoreDelaySec ||
          requested.sensorTimeoutSec != config_.sensorTimeoutSec ||
          requested.allowHeatWithoutBatch != config_.allowHeatWithoutBatch ||
          requested.sirenSelfTestEnabled != config_.sirenSelfTestEnabled ||
          (requested.connectivityMode == ConnectivityMode::Online &&
           config_.connectivityMode != ConnectivityMode::Online));
      const MachineConfig previousConfig = config_;
      MachineConfig readback{};
      const bool safetySaveBlocked = batchClearPending_ || safetyJournalFaultLatched_;
      const bool saveAllowed = !safetySaveBlocked &&
          !protectedBatchChange && pidAuthorityValid;
      const bool ok = saveAllowed && store_.saveConfig(requested, readback);
      if (ok) {
        config_ = readback;
        hmiSetConfig(config_);
        mayapRealtimeSetConfig(config_);
        mayapCloudSetConfig(config_);
        mayapSetConnectivityMode(config_.connectivityMode);
        eventLog_.push(now, EventType::ConfigSaved,
                       static_cast<uint16_t>(EventCode::ConfigSaved));

        // Chi re-project I khi hinh dang PID/actuator thuc su doi. SV la lenh
        // dieu khien: khong duoc back-calculate I de huy P-response cua buoc SV.
        // Cac thay doi khong lien quan (dao, am, thong gio...) cung khong duoc
        // reset derivative/timestamp cua PID. Neu gain/cap va SV cung doi, giu
        // bumpless tai SV cu; sample cam bien ke tiep se ap dung buoc SV moi.
        if (thermalPidRuntimeConfigChanged(previousConfig, config_)) {
          pid_.applyConfigBumpless(now, previousConfig.targetTemp,
                                   temperature_, config_);
          pidPower_ = pid_.output();
        }

        // Lich dao la moc tuyet doi tinh tu LAN DAO THAT gan nhat. Doi chu ky
        // hoac BAT/TAT dao trong luc me dang chay chi chieu lai deadline tu
        // lastTurnEpoch_/lastTurnAt_, tuyet doi khong lay thoi diem bam LUU
        // lam moc moi (neu khong moi lan sua chu ky se bi dem lai tu dau).
        if (batchRunning_ &&
            (previousConfig.turningEnabled != config_.turningEnabled ||
             previousConfig.turnIntervalMin != config_.turnIntervalMin)) {
          scheduleNextTurnFromAnchor(now);
        }

      } else if (saveAllowed) {
        latchStorageFault("CONFIG SAVE");
      }
      if (ok) clearStorageDegraded(now);
      hmiConfirmConfigSave(transactionId, ok, ok ? &readback : nullptr);
      const char *configFailureReason = !pidAuthorityValid ? "INVALID_PID_GAINS"
          : protectedBatchChange ? "CONFIG_BATCH_LOCKED"
          : safetySaveBlocked ? "CONFIG_SAFETY_BLOCK"
          : "CONFIG_EEPROM_ERROR";
      mayapRealtimeConfirmConfigSave(transactionId, ok, ok ? &readback : nullptr,
                                configFailureReason);
      const char *configFailureTag = ok ? ""
          : !pidAuthorityValid ? "(INVALID_PID)"
          : protectedBatchChange ? "(BATCH_LOCK)"
          : safetySaveBlocked ? "(SAFETY_BLOCK)" : "(EEPROM)";
      mayapSerialPrintf(false, "[CFG] save=%s%s SV=%.1f HIGH=%.1f EMG=%.1f turn=%umin\n",
                       ok ? "OK" : "FAIL", configFailureTag, requested.targetTemp,
                       requested.highTempAlarm, requested.emergencyTemp,
                       requested.turnIntervalMin);
    }


    HmiCommand command{};
    uint8_t budget = 0;
    while (budget++ < COMMAND_QUEUE_SIZE && hmiTakeCommand(command)) {
      bool ok = false;
      const char *message = "LENH KHONG HOP LE";
      switch (command.type) {
        case HmiCommandType::BatchStart:
          ok = startBatch(now, message); break;
        case HmiCommandType::BatchStop:
          ok = stopBatch(now, message); break;
        case HmiCommandType::AlarmAck: {
          // F-12: chuoi bao tam dung coi truoc day hard-code "5 PHUT" trong
          // khi SIREN_TEMPORARY_MUTE_MS thuc te la 60000UL (1 phut) - nguoi
          // dung duoc bao sai thoi gian, coi thuc te reo lai som hon nhieu so
          // voi thong bao. Tinh so phut truc tiep tu hang so de khong con
          // lech nhau du sau nay co doi SIREN_TEMPORARY_MUTE_MS.
          char sirenMuteMsg[24];
          snprintf(sirenMuteMsg, sizeof(sirenMuteMsg), "COI TAM DUNG %lu PHUT",
                   static_cast<unsigned long>(SIREN_TEMPORARY_MUTE_MS / 60000UL));
          // F-09: 2 hanh dong nay can nguoi thuc su o canh may kiem tra vat
          // ly (coi khan cap = qua nhiet that, loi dao = nghi ngo hong co
          // khi) - tu choi rieng 2 hanh dong nay neu lenh den qua MQTT
          // (command.source == Remote), cac loi/canh bao khac trong cung
          // lenh AlarmAck van duoc xu ly binh thuong ben duoi.
          const bool remoteAck = command.source == HmiCommandSource::Remote;
          const bool emergencyAckBlocked = remoteAck && emergencyActive_;
          const bool hadTurnFault = turnFaultLatched_;
          const bool turnAckBlocked = remoteAck && hadTurnFault;
          // F-06: ACK cung tam tat coi cho me qua han (khong bi han che
          // remote/local nhu 2 truong hop tren - day khong phai su co can
          // kiem tra vat ly, chi la mot quyet dinh van hanh).
          if ((!emergencyAckBlocked && emergencyActive_) || batchOverdueSirenActive_) {
            sirenMutedUntil_ = now + SIREN_TEMPORARY_MUTE_MS;
          }
          const bool turnCleared = (hadTurnFault && !turnAckBlocked) ? clearTurnFault() : false;
          const bool systemAck = (command.alarmMask & AlarmSystem) != 0U;
          const bool resetCleared = systemAck && abnormalResetLatched_;
          if (resetCleared) {
            abnormalResetLatched_ = false;
            power_.acknowledge();
            if (!power_.journalWriteOk()) {
              safetyJournalFaultLatched_ = true;
              faults_.set(FaultCode::SafetyJournalUnavailable, true, now);
            }
            faults_.set(FaultCode::AbnormalReset, false, now);
            heatRestartNotBefore_ = now + HEAT_RESTART_LOCKOUT_MS;
            pid_.reset();
          }
          (void)faults_.acknowledge(now, command.alarmMask);
          const bool persistentSystemFault = systemAck &&
              (storageFaultLatched_ || safetyJournalFaultLatched_ ||
               batchClearPending_);
          ok = !persistentSystemFault && !emergencyAckBlocked && !turnAckBlocked;
          message = persistentSystemFault ? "LOI HE THONG CHUA XOA"
                  : emergencyAckBlocked ? "COI KHAN CAP CAN ACK TAI MAY"
                  : turnAckBlocked ? "LOI DAO CAN ACK TAI MAY"
                  : emergencyActive_ ? sirenMuteMsg
                  : batchOverdueSirenActive_ ? sirenMuteMsg
                  : resetCleared ? "DA XAC NHAN RESET LOI"
                  : turnCleared ? "DA XOA LOI DAO"
                  : hadTurnFault ? "THA NUT/KT HANH TRINH"
                  : "DA XAC NHAN";
          break;
        }
        case HmiCommandType::BatchOverdueContinue: {
          // F-06: xac nhan "tiep tuc u am" - huy coi + huy dem nguoc tu dong
          // dung 12h cho ca phan con lai cua me nay (khong lam gi khac, may
          // van dieu khien nhiet/dao binh thuong nhu truoc gio).
          if (!batchRunning_) { message = "KHONG CO ME DANG CHAY"; break; }
          batchOverdueAcknowledged_ = true;
          (void)safetyJournal_.setBatchOverdueAcknowledged(true);
          faults_.set(FaultCode::BatchOverdueConfirmationPending, false, now);
          batchOverdueSirenActive_ = false;
          ok = true;
          message = "DA XAC NHAN TIEP TUC U AM";
          break;
        }
        case HmiCommandType::TestModeEnter:
          ok = enterTestMode(now, message); break;
        case HmiCommandType::TestModeExit:
          exitTestMode(now);
          ok = true; message = "DA THOAT TEST";
          break;
        case HmiCommandType::TestOutputPulse:
          ok = testOutputPulse(now,
              static_cast<TestOutputId>(command.alarmMask & 0xFFU), message);
          break;
        case HmiCommandType::TestOutputStop:
          testOutputStop(now, static_cast<TestOutputId>(command.alarmMask & 0xFFU));
          ok = true; message = "DA TAT THIET BI";
          break;
        case HmiCommandType::TestLimitStart:
          ok = testLimitStart(now,
              static_cast<TestLimitId>(command.alarmMask & 0xFFU), message);
          break;
        case HmiCommandType::TestLimitCancel:
          testLimitCancel(now);
          ok = true; message = "DA HUY KIEM TRA";
          break;
        case HmiCommandType::WifiPortalStart:
          testModeLastActivityAt_ = now;
          ok = mayapRequestWifiPortal();
          message = ok ? "DANG MO CONG DOI WIFI" : "CHI DUNG DUOC KHI ONLINE";
          break;
        case HmiCommandType::WifiPortalCancel:
          mayapCancelWifiPortal();
          ok = true; message = "DA DONG CONG WIFI";
          break;
        case HmiCommandType::CloudPinReset:
          // Chi dat co hieu cho networkTask (mayapCloudAlertUpdate() trong
          // cloud_alert_link.h) - khong tu goi HTTPS tai day, controlTask
          // khong duoc phep block.
          mayapRequestCloudPinReset();
          ok = true; message = "DA GUI YEU CAU DAT LAI PIN";
          break;
        case HmiCommandType::FirmwareWebApply:
          if (!mayapFirmwareMaintenanceReady() || mayapFirmwareMaintenanceActive()) {
            message = "DUNG ME/TEST VA TAT CONG TAC NHIET TRUOC"; break;
          }
          // Chi dat co hieu cho otaTask (mayapFirmwareWebUpdate() trong
          // ota_web_update.h) - viec tai ve/flash thuc su co the mat vai
          // chuc giay, controlTask khong duoc phep block.
          mayapRequestFirmwareWebApply();
          ok = true; message = "DANG TAI FIRMWARE...";
          break;
        case HmiCommandType::FirmwareWebCheckNow:
          // Nut "Cap nhat" tren web dashboard - CHI yeu cau kiem tra ngay
          // (bo qua nhip 6h dinh ky), KHONG tu tai ve/flash gi ca. Neu co
          // ban moi, se hien tren HMI nhu binh thuong, van phai xac nhan
          // vat ly tai may (xem ota_web_update.h).
          mayapRequestFirmwareWebCheckNow();
          ok = true; message = "DANG KIEM TRA BAN MOI";
          break;
        case HmiCommandType::FirmwareRollback:
          if (!mayapFirmwareMaintenanceReady() || mayapFirmwareMaintenanceActive()) {
            message = "DUNG ME/TEST VA TAT CONG TAC NHIET TRUOC"; break;
          }
          // Kiem tra NGAY tren controlTask (chi doc 1 byte flash, khong
          // ghi/block) de tra ve ket qua chinh xac ngay lap tuc thay vi bao
          // "dang xu ly" roi im lang neu khong con ban de quay lai - dung
          // yeu cau moi lenh phai bao ro thanh cong/that bai. Viec THAT SU
          // doi con tro khoi dong + restart nam ben otaTask (xem ota_
          // rollback.h), giong moi thao tac dung den flash khac.
          if (mayapRollbackAvailable()) {
            mayapRequestFirmwareRollback();
            ok = true; message = "DANG QUAY LAI FIRMWARE CU...";
          } else {
            message = "KHONG CO BAN CU DE QUAY LAI";
          }
          break;
        case HmiCommandType::LightToggle: {
          const bool currentlyOn = lightWebOverrideActive_
              ? lightWebOverrideValue_ : inputs_.state().light;
          lightWebOverrideValue_ = !currentlyOn;
          lightWebOverrideActive_ = true;
          lightWebOverrideRefInput_ = inputs_.state().light;
          ok = true;
          message = lightWebOverrideValue_ ? "DA BAT DEN" : "DA TAT DEN";
          break;
        }
        case HmiCommandType::AutoTuneStart:
          ok = startAutoTune(now, message); break;
        case HmiCommandType::AutoTuneCancel:
          ok = cancelAutoTune(now, message); break;
        case HmiCommandType::ResumeYes:
          if (resumePending_ && resumeConfirmationRequired_) {
            resumeConfirmationRequired_ = false;
            eventLog_.push(now, EventType::Recovery,
                           static_cast<uint16_t>(EventCode::ResumeAccepted));
            ok = true; message = "DA CHON TIEP TUC ME";
          } else {
            message = "KHONG CO ME CHO XAC NHAN";
          }
          break;
        case HmiCommandType::ResumeNo:
          if (resumePending_ && resumeConfirmationRequired_) {
            if (!safetyJournal_.setStopIntent()) {
              safetyJournalFaultLatched_ = true;
              faults_.set(FaultCode::SafetyJournalUnavailable, true, now);
            }
            batchClearPending_ = true;
            batchClearAttemptCount_ = 0U;
            batchClearRetryAt_ = now;
            resumeConfirmationRequired_ = false;
            resumePending_ = false;
            automaticResetRecovery_ = false;
            resumeBlockReason_ = ResumeBlockReason::None;
            elapsedBeforeStartSec_ = 0U;
            savedElapsedAtCheckpoint_ = 0U;
            lastCheckpointEpoch_ = 0U;
            resumeClockAdjusted_ = true;
            turnCountToday_ = 0U;
            turnCountBatch_ = 0U;
            const bool cleared = tryCommitBatchClear(now);
            eventLog_.push(now, EventType::Recovery,
                           static_cast<uint16_t>(EventCode::ResumeRejected),
                           cleared ? 0 : 1);
            eventLog_.setLoggingEnabled(false);
            batchLogger_.stop();
            batchStartEpoch_ = 0U;
            lastTurnEpoch_ = 0U;
            lastTurnAt_ = 0U;
            batchLogFaultActive_ = false;
            ok = true;
            message = cleared ? "DA HUY ME CU" : "DA HUY - CHO XOA BO NHO";
          } else {
            message = "KHONG CO ME CHO XAC NHAN";
          }
          break;
        default: break;
      }
      if (!ok) {
        eventLog_.push(now, EventType::System,
                       static_cast<uint16_t>(EventCode::CommandRejected),
                       static_cast<int16_t>(command.type));
      }
      hmiConfirmCommand(command.id, ok, message);
      mayapRealtimeConfirmCommand(command.id, ok, message);
    }
  }

  bool startBatch(uint32_t now, const char *&message) {
    if (!mayapBootOperationsReady()) { message = "DANG KHOI DONG"; return false; }
    if (mayapFirmwareMaintenanceActive()) { message = "DANG CAP NHAT FIRMWARE"; return false; }
    const InputState &in = inputs_.state();
    if (testModeActive_) { message = "HAY THOAT TEST TRUOC"; return false; }
    if (batchRunning_) { message = "ME DANG CHAY"; return false; }
    if (batchClearPending_) { message = "DANG XOA DU LIEU ME CU"; return false; }
    if (safetyJournalFaultLatched_) { message = "LOI NHAT KY AN TOAN"; return false; }
    if (storageFaultLatched_ || storageDegraded_) { message = "LOI BO NHO CAU HINH"; return false; }
    if (abnormalResetLatched_) { message = "HAY XAC NHAN RESET LOI"; return false; }
    if (resumePending_) { message = "DUNG ME CU TRUOC"; return false; }
    if (autotune_.running()) { message = "AUTO TUNE DANG CHAY"; return false; }
    if (!in.autoMode) { message = "HAY CHUYEN SANG AUTO"; return false; }
    if (!sensorUsable_) { message = "CAM BIEN CHUA SAN SANG"; return false; }
    if (!rtc_.valid()) { message = "RTC CHUA HOP LE"; return false; }
    if (in.limitLeft && in.limitRight) { message = "LOI 2 HANH TRINH"; return false; }
    if (turnFaultLatched_) { message = "DANG CO LOI DAO"; return false; }
    // Khoa kiem tra co khi dao (E205, bat sau 3 lan loi dao lien tiep) khong
    // tu het khi turnFaultLatched_ duoc ACK - phai vao Test Mode xac nhan lai
    // ca 2 cong tac hanh trinh (updateTestMode) moi duoc go. Neu khong chan o
    // day, me moi se chay het chu ky ma khong bao gio dao (F-04).
    if (faults_.turningInhibited()) { message = "CAN KIEM TRA CO KHI DAO"; return false; }
    if (highTemperatureActive_ || emergencyActive_) { message = "NHIET DANG QUA CAO"; return false; }
    if (!in.heaterEnable) {
      message = "HAY BAT CONG TAC NHIET"; return false;
    }
    if (!config_.turningEnabled) {
      message = "HAY BAT TU DONG DAO"; return false;
    }

    // Moi me co mot moc thoi gian va mot file log rieng. Xoa RAM log truoc
    // khi phat BatchStart de HMI chi hien lich su cua me hien tai.
    eventLog_.clear(now);
    batchStartEpoch_ = rtc_.epoch();
    lastTurnEpoch_ = batchStartEpoch_;
    lastTurnAt_ = now;
    batchLogSampleSequence_ = 0U;
    batchLogGate_.reset(now);
    batchLogFaultActive_ = !batchLogger_.start(batchStartEpoch_, false);

    batchRunning_ = true;
    eventLog_.setLoggingEnabled(true);
    batchPhase_ = BatchPhase::Prestart;
    batchStartedAt_ = now;
    phaseStartedAt_ = now;
    elapsedBeforeStartSec_ = 0;
    savedElapsedAtCheckpoint_ = 0U;
    lastCheckpointEpoch_ = 0U;
    resumeClockAdjusted_ = true;
    turnCountToday_ = 0;
    turnCountBatch_ = 0;
    // F-15: reset ve 0 (khong phai stale tu me truoc) de updateBatchTime()
    // tu bootstrap dung ngay 1 qua nhanh "== 0U" cua chinh no.
    lastTurnCounterDay_ = 0U;
    // F-06: me MOI bat dau chua the nao "qua han" - xoa xac nhan cu (neu co,
    // khong lien quan gi den me nay) de updateBatchOverdue() tu tinh lai
    // dung han tu batchStartEpoch_ moi.
    batchOverdueAcknowledged_ = false;
    (void)safetyJournal_.setBatchOverdueAcknowledged(false);
    nextTurnAt_ = 0;
    needHome_ = !(in.limitLeft ^ in.limitRight);
    if (in.limitLeft) trayPosition_ = TrayPosition::Left;
    else if (in.limitRight) trayPosition_ = TrayPosition::Right;
    else trayPosition_ = TrayPosition::Unknown;
    lowTempTimer_.reset();
    humidityTimer_.reset();
    heatRestartNotBefore_ = std::max<uint32_t>(
        heatRestartNotBefore_, now + FAN_PRESTART_MS);
    if (!saveBatchRecord()) {
      batchRunning_ = false;
      eventLog_.setLoggingEnabled(false);
      batchPhase_ = BatchPhase::Stopped;
      batchLogger_.stop();
      batchStartEpoch_ = 0U;
      lastTurnEpoch_ = 0U;
      lastTurnAt_ = 0U;
      batchLogFaultActive_ = false;
      message = "LOI LUU TRANG THAI ME";
      return false;
    }
    message = "DA BAT DAU ME";
    eventLog_.push(now, EventType::BatchStart,
                   static_cast<uint16_t>(EventCode::BatchStart));
    mayapSerialPrintf(false, "[BATCH] START\n");
    // Bao mat dien qua ATtiny13A: kiem tra con song (ping) truoc khi bat dau
    // me, theo dung yeu cau nguoi lap dat - KHONG chan viec bat dau me neu
    // khong phan hoi (mach dinh khong nen tri hoan ca me ap chi vi 1 mach
    // phu), chi canh bao ro rang de nguoi dung tu kiem tra pin CR2032/day
    // noi som, truoc khi mat lop bao ve du phong suot ca me.
    attinyBatchSync_.expect(true, millis());
    (void)mayapAttinyBusRequest(ATTINY_MSG_BATCH_START);
    (void)mayapAttinyBusRequest(ATTINY_MSG_STATUS_QUERY);
    attinyLastStatusQueryAt_ = now;
    return true;
  }

  bool stopBatch(uint32_t now, const char *&message) {
    if (batchClearPending_) {
      message = "DANG XOA DU LIEU ME";
      return false;
    }
    if (!batchRunning_ && !resumePending_) {
      message = "KHONG CO ME DANG CHAY";
      return false;
    }

    // Tombstone duoc dat TRUOC khi thay doi RAM/EEPROM. Neu WDT reset dung
    // giua giao dich, boot sau khong duoc tu phuc hoi ban ghi wasRunning cu.
    if (!safetyJournal_.setStopIntent()) {
      safetyJournalFaultLatched_ = true;
      faults_.set(FaultCode::SafetyJournalUnavailable, true, now);
    }
    batchClearPending_ = true;
    batchClearAttemptCount_ = 0U;
    batchClearRetryAt_ = now;

    batchRunning_ = false;
    resumePending_ = false;
    resumeConfirmationRequired_ = false;
    automaticResetRecovery_ = false;
    resumeBlockReason_ = ResumeBlockReason::None;
    batchPhase_ = BatchPhase::Stopped;
    stopTurn(false);
    nextTurnAt_ = 0U;
    postCoolUntil_ = now + POST_COOL_MS;
    elapsedBeforeStartSec_ = 0U;
    savedElapsedAtCheckpoint_ = 0U;
    lastCheckpointEpoch_ = 0U;
    resumeClockAdjusted_ = true;

    const bool cleared = tryCommitBatchClear(now);
    message = cleared ? "DA DUNG ME" : "DA DUNG - CHO XOA BO NHO";
    eventLog_.push(now, EventType::BatchStop,
                   static_cast<uint16_t>(EventCode::BatchStop),
                   cleared ? 0 : 1);
    // Ghi xong dong cuoi cung cua me nay roi moi tat: ngoai me khong con ghi gi nua.
    eventLog_.setLoggingEnabled(false);
    batchLogger_.stop();
    batchStartEpoch_ = 0U;
    lastTurnEpoch_ = 0U;
    lastTurnAt_ = 0U;
    batchLogFaultActive_ = false;
    batchLogSampleSequence_ = 0U;
    mayapSerialPrintf(false, "[BATCH] STOP clear=%s\n",
                      cleared ? "OK" : "PENDING");
    // Chuyen giao khong tao "khoang mu": neu luc dung me van con mot tai
    // quan trong THUC TE dang ON (vd quat post-cool/dao/SSR), arm activity
    // TRUOC roi moi xoa batch tren Tiny. Den khong tham gia dieu kien nay.
    const OutputState &stopOut = outputs_.state();
    const bool keepPowerLossArmed = stopOut.turnLeft || stopOut.turnRight ||
        stopOut.circulationFan || stopOut.ventFan || stopOut.heatMaster;
    attinyBatchSync_.expect(false, millis());
    if (keepPowerLossArmed) (void)mayapAttinyBusRequest(ATTINY_MSG_ACTIVITY_ON);
    (void)mayapAttinyBusRequest(ATTINY_MSG_BATCH_END);
    return true;
  }

  bool startAutoTune(uint32_t now, const char *&message) {
    if (!mayapBootOperationsReady()) { message = "DANG KHOI DONG"; return false; }
    if (mayapFirmwareMaintenanceActive()) { message = "DANG CAP NHAT FIRMWARE"; return false; }
    const InputState &in = inputs_.state();
    if (mayapSystemTripLatched() || faults_.masterDropRequired() || faults_.ssrInhibited()) {
      message = "DUONG NHIET DANG BI KHOA"; return false;
    }
    if (autotune_.running()) { message = "AUTO TUNE DANG CHAY"; return false; }
    if (testModeActive_) { message = "HAY THOAT TEST TRUOC"; return false; }
    if (batchRunning_ || resumePending_) { message = "DUNG ME TRUOC"; return false; }
    if (batchClearPending_) { message = "DANG XOA DU LIEU ME CU"; return false; }
    if (safetyJournalFaultLatched_) { message = "LOI NHAT KY AN TOAN"; return false; }
    if (storageFaultLatched_ || storageDegraded_) { message = "LOI BO NHO CAU HINH"; return false; }
    if (abnormalResetLatched_) { message = "HAY XAC NHAN RESET LOI"; return false; }
    if (!in.autoMode) { message = "HAY CHUYEN SANG AUTO"; return false; }
    if (!in.heaterEnable) { message = "HAY BAT CONG TAC NHIET"; return false; }
    if (!sensorUsable_ || !isfinite(temperature_) || !isfinite(rawTemperature_)) { message = "CAM BIEN CHUA SAN SANG"; return false; }
    if (!rtc_.valid()) { message = "RTC CHUA HOP LE"; return false; }
    if (highTemperatureActive_ || emergencyActive_) { message = "NHIET DANG QUA CAO"; return false; }
    if (config_.targetTemp + config_.autotuneBandC >= config_.highTempAlarm) {
      message = "KHOANG NHIET KHONG DU"; return false;
    }
    if (!autotune_.preflight(temperature_, config_, message)) return false;
    autotune_.configure(config_.targetTemp);
    autotune_.start(now, temperature_);
    pid_.reset();
    heaterBurst_.reset();
    postCoolUntil_ = 0;
    message = "AUTO TUNE DA BAT DAU";
    eventLog_.push(now, EventType::AutoTuneStart,
                   static_cast<uint16_t>(EventCode::AutoTuneStarted));
    mayapSerialPrintf(false, "[TUNE] START PV=%.3f SP=%.2f High=%.2f relay<=%u%% band=%.2fC (headroom to High %.2fC)\n",
                     temperature_, config_.targetTemp, config_.highTempAlarm,
                     std::min<uint8_t>(config_.autotuneRelayPowerPercent, config_.maxHeaterPower),
                     config_.autotuneBandC, config_.highTempAlarm - temperature_);
    return true;
  }

  bool cancelAutoTune(uint32_t now, const char *&message) {
    if (!autotune_.running()) {
      message = "AUTO TUNE KHONG CHAY";
      return false;
    }
    autotune_.cancel();
    heaterBurst_.reset();
    pid_.reset();
    // Keep the same cool-down and restart lockout used by every terminal
    // Auto Tune path. Stored/current PID gains are not modified on cancel.
    postCoolUntil_ = now + POST_COOL_MS;
    heatRestartNotBefore_ = now + HEAT_RESTART_LOCKOUT_MS;
    message = "DA HUY AUTO TUNE";
    mayapSerialPrintf(false, "[TUNE] CANCEL operator\n");
    return true;
  }

  // Dao tay chi di qua cong tac cung (PIN_IN_TURN_LEFT/RIGHT); HMI khong co
  // menu dao tay nen khong can duong lenh rieng cho no.

  // Cong bu phan thoi gian mat dien bang moc RTC da ghi cung checkpoint.
  // Chi chay mot lan khi DS3231 hop le; neu RTC loi thi dung elapsedSec da luu.
  void adjustResumeElapsedFromRtc(uint32_t now) {
    if (resumeClockAdjusted_ || lastCheckpointEpoch_ == 0U ||
        !rtc_.valid() || (!resumePending_ && !batchRunning_)) return;
    const uint32_t currentEpoch = rtc_.epoch();
    if (currentEpoch < lastCheckpointEpoch_) {
      resumeClockAdjusted_ = true;
      mayapSerialPrintf(false, "[TIME] BO QUA BU GIO: EPOCH LUI\n");
      return;
    }
    const uint32_t delta = currentEpoch - lastCheckpointEpoch_;
    if (delta > MAX_RTC_RECOVERY_GAP_SEC) {
      resumeClockAdjusted_ = true;
      mayapSerialPrintf(false, "[TIME] BO QUA BU GIO: GAP=%lus\n",
                        static_cast<unsigned long>(delta));
      return;
    }
    const uint64_t corrected = static_cast<uint64_t>(savedElapsedAtCheckpoint_) + delta;
    elapsedBeforeStartSec_ = corrected > UINT32_MAX
        ? UINT32_MAX : static_cast<uint32_t>(corrected);
    if (batchRunning_) batchStartedAt_ = now;
    resumeClockAdjusted_ = true;
    mayapSerialPrintf(false, "[TIME] DA BU THOI GIAN TU RTC: +%lus, elapsed=%lus\n",
                      static_cast<unsigned long>(delta),
                      static_cast<unsigned long>(elapsedBeforeStartSec_));
    // Ghi lai thoi luong giu khoang trong (mat dien/reset) vao nhat ky su
    // kien - doi ra GIO cho gon (gap toi da MAX_RTC_RECOVERY_GAP_SEC=45 ngay
    // = 1080h, thua suc nam trong int16_t) thay vi giay/phut de khong tran.
    const uint32_t gapHours = delta / 3600UL;
    eventLog_.push(now, EventType::Recovery,
                   static_cast<uint16_t>(EventCode::PowerOutageDuration),
                   static_cast<int16_t>(std::min<uint32_t>(gapHours, INT16_MAX)));
  }

  // ----------------------------- Resume --------------------------------------
  static const char *resumeBlockReasonText(ResumeBlockReason reason) {
    switch (reason) {
      case ResumeBlockReason::None: return "READY";
      case ResumeBlockReason::Confirmation: return "WAIT_CONFIRM";
      case ResumeBlockReason::BatchClear: return "BATCH_CLEAR";
      case ResumeBlockReason::Storage: return "STORAGE";
      case ResumeBlockReason::ResetAck: return "RESET_ACK";
      case ResumeBlockReason::AutoMode: return "AUTO_SWITCH";
      case ResumeBlockReason::HeaterSwitch: return "HEATER_SWITCH";
      case ResumeBlockReason::Sensor: return "SENSOR";
      case ResumeBlockReason::Rtc: return "RTC";
      case ResumeBlockReason::TurnFault: return "TURN_FAULT";
      case ResumeBlockReason::LimitConflict: return "LIMIT_CONFLICT";
      case ResumeBlockReason::Temperature: return "TEMP_HIGH";
      case ResumeBlockReason::Delay: return "RESTORE_DELAY";
      case ResumeBlockReason::TurningDisabled: return "TURNING_DISABLED";
      default: return "UNKNOWN";
    }
  }

  void setResumeBlockReason(ResumeBlockReason reason, uint32_t now) {
    if (resumeBlockReason_ == reason) return;
    resumeBlockReason_ = reason;
    resumeBlockReasonAt_ = now;
    mayapSerialPrintf(false, "[RECOVERY] block=%s\n",
                      resumeBlockReasonText(reason));
  }

  // Trong luc dang CHO xac nhan/cho dieu kien phuc hoi, thoi gian sinh hoc
  // cua me van tiep tuc troi. Checkpoint + RTC la moc that, khong de dong ho
  // dung lai chi vi nguoi dung chua bam Tiep tuc. Khong ghi flash tai day;
  // checkpointGate van la noi duy nhat luu dinh ky.
  void refreshPendingResumeElapsedFromRtc() {
    if (!resumePending_ || lastCheckpointEpoch_ == 0U || !rtc_.valid()) return;
    const uint32_t currentEpoch = rtc_.epoch();
    if (currentEpoch < lastCheckpointEpoch_) return;
    const uint32_t delta = currentEpoch - lastCheckpointEpoch_;
    if (delta > MAX_RTC_RECOVERY_GAP_SEC) return;
    const uint64_t corrected = static_cast<uint64_t>(savedElapsedAtCheckpoint_) + delta;
    const uint32_t refreshed = corrected > UINT32_MAX
        ? UINT32_MAX : static_cast<uint32_t>(corrected);
    if (refreshed > elapsedBeforeStartSec_) elapsedBeforeStartSec_ = refreshed;
  }

  void processResume(uint32_t now) {
    if (!resumePending_) {
      setResumeBlockReason(ResumeBlockReason::None, now);
      faults_.set(FaultCode::ResumeConfirmationPending, false, now);
      faults_.set(FaultCode::ResumeRtcWaitTooLong, false, now);
      return;
    }

    // Cap nhat ngay/elapsed TRUOC moi nhanh return cua recovery (ke ca dang
    // cho xac nhan, AUTO, heater, sensor hay powerRestoreDelay).
    refreshPendingResumeElapsedFromRtc();

    if (resumeConfirmationRequired_) {
      setResumeBlockReason(ResumeBlockReason::Confirmation, now);
      // Chi canh bao "cho qua lau", KHONG tu lam gi thay nguoi dung - van
      // giu dung y do an toan cua man hinh xac nhan (phai co nguoi quyet
      // dinh CO/HUY, khong tu dong ap lai/huy khi qua thoi gian).
      faults_.set(FaultCode::ResumeConfirmationPending,
                  elapsedMs(now, resumeConfirmPromptAt_) >= RESUME_CONFIRM_ALERT_MS,
                  now, static_cast<int16_t>(std::min<uint32_t>(
                      elapsedMs(now, resumeConfirmPromptAt_) / 60000UL, INT16_MAX)));
      return;
    }
    faults_.set(FaultCode::ResumeConfirmationPending, false, now);

    const InputState &in = inputs_.state();
    if (batchClearPending_) {
      setResumeBlockReason(ResumeBlockReason::BatchClear, now);
      faults_.set(FaultCode::ResumeRtcWaitTooLong, false, now);
      return;
    }
    if (safetyJournalFaultLatched_ || storageFaultLatched_ || storageDegraded_) {
      setResumeBlockReason(ResumeBlockReason::Storage, now);
      faults_.set(FaultCode::ResumeRtcWaitTooLong, false, now);
      return;
    }
    if (abnormalResetLatched_) {
      setResumeBlockReason(ResumeBlockReason::ResetAck, now);
      faults_.set(FaultCode::ResumeRtcWaitTooLong, false, now);
      return;
    }
    if (!in.autoMode) {
      setResumeBlockReason(ResumeBlockReason::AutoMode, now);
      faults_.set(FaultCode::ResumeRtcWaitTooLong, false, now);
      return;
    }
    if (!in.heaterEnable) {
      setResumeBlockReason(ResumeBlockReason::HeaterSwitch, now);
      faults_.set(FaultCode::ResumeRtcWaitTooLong, false, now);
      return;
    }
    if (!sensorUsable_) {
      setResumeBlockReason(ResumeBlockReason::Sensor, now);
      faults_.set(FaultCode::ResumeRtcWaitTooLong, false, now);
      return;
    }
    if (!rtc_.valid()) {
      setResumeBlockReason(ResumeBlockReason::Rtc, now);
      // Khac cac ly do khac o day (co the tu sua/nguoi dung sua duoc ngay) -
      // truong hop nay may CHI CO THE CHO (auto-repair hoac NTP tu phuc hoi
      // RTC), nen canh bao SOM hon nhieu (RESUME_RTC_WAIT_ALERT_MS, xem
      // config.h) de nguoi dung/quan tri biet may dang treo cho vi RTC.
      faults_.set(FaultCode::ResumeRtcWaitTooLong,
                  elapsedMs(now, resumeBlockReasonAt_) >= RESUME_RTC_WAIT_ALERT_MS,
                  now, static_cast<int16_t>(std::min<uint32_t>(
                      elapsedMs(now, resumeBlockReasonAt_) / 60000UL, INT16_MAX)));
      return;
    }
    faults_.set(FaultCode::ResumeRtcWaitTooLong, false, now);
    if (turnFaultLatched_) {
      setResumeBlockReason(ResumeBlockReason::TurnFault, now); return;
    }
    if (in.limitLeft && in.limitRight) {
      setResumeBlockReason(ResumeBlockReason::LimitConflict, now); return;
    }
    // Dong bo voi startBatch(): khong duoc phuc hoi neu "Tu dong dao" dang
    // tat - truoc day thieu dieu kien nay (khac voi startBatch), che do tu
    // phuc hoi co the chay tiep voi dao tat ma khong bi chan truoc, chi duoc
    // AutoTurningDisabledDuringBatch bat lai 1 chu ky sau. Nay chan som nhu
    // startBatch() cho nhat quan; an toan de them vi turningEnabled gio da
    // sua duoc ngay ca khi dang cho phuc hoi (protectedBatchChange da bo
    // khoa truong nay - xem processHmiTransactions()), nen khong tao ra bi
    // ket "khong sua duoc, khong phuc hoi duoc".
    if (!config_.turningEnabled) {
      setResumeBlockReason(ResumeBlockReason::TurningDisabled, now); return;
    }
    if (highTemperatureActive_ || emergencyActive_) {
      setResumeBlockReason(ResumeBlockReason::Temperature, now); return;
    }
    if (elapsedMs(now, bootAt_) <
        static_cast<uint32_t>(config_.powerRestoreDelaySec) * 1000UL) {
      setResumeBlockReason(ResumeBlockReason::Delay, now); return;
    }

    setResumeBlockReason(ResumeBlockReason::None, now);
    if (batchStartEpoch_ == 0U && rtc_.valid()) {
      const uint32_t nowEpoch = rtc_.epoch();
      batchStartEpoch_ = nowEpoch > elapsedBeforeStartSec_
          ? nowEpoch - elapsedBeforeStartSec_ : nowEpoch;
    }
    if (!batchLogger_.active() && batchStartEpoch_ != 0U) {
      batchLogFaultActive_ = !batchLogger_.start(batchStartEpoch_, true);
    }
    batchLogSampleSequence_ = 0U;
    batchLogGate_.reset(now);
    resumePending_ = false;
    batchRunning_ = true;
    batchPhase_ = BatchPhase::Prestart;
    batchStartedAt_ = now;
    phaseStartedAt_ = now;
    nextTurnAt_ = 0U;
    if (in.limitLeft) trayPosition_ = TrayPosition::Left;
    else if (in.limitRight) trayPosition_ = TrayPosition::Right;
    else trayPosition_ = TrayPosition::Unknown;
    needHome_ = trayPosition_ == TrayPosition::Unknown;
    heatRestartNotBefore_ = now + FAN_PRESTART_MS;
    eventLog_.push(now, EventType::BatchStart,
                   static_cast<uint16_t>(EventCode::BatchResume),
                   static_cast<int16_t>(std::min<uint32_t>(elapsedBeforeStartSec_ / 60UL,
                                                          INT16_MAX)),
                   1U);
    mayapSerialPrintf(false, "[BATCH] AUTO RESUME elapsed=%lus\n",
                     static_cast<unsigned long>(elapsedBeforeStartSec_));
  }

  // ----------------------------- Alarm ---------------------------------------
  void updateAlarms(uint32_t now) {
    const float safetyTemp = isfinite(rawTemperature_)
        ? (isfinite(temperature_) ? std::max(rawTemperature_, temperature_) : rawTemperature_)
        : temperature_;
    const bool validSafety = safetySampleValid_ && isfinite(safetyTemp);
    const bool wasHigh = highTemperatureActive_;
    const bool wasEmergency = emergencyActive_;
    // Che do "Bao nhiet ngoai me" (cai dat nhiet - mac dinh BAT, giu nguyen
    // hanh vi tu truoc gio): TAT thi cap 2/3 CHI duoc phep TRIGGER khi dang
    // co me ap, giong het cap 1 (Nhiet do thap) von da luon nhu vay. Chi cong
    // gate nay vao dieu kien KHOI TAO (trip) - mot khi da active thi tiep tuc
    // theo doi/thoat binh thuong qua hysteresis o duoi, KHONG bi "quen" giua
    // chung neu me vua ket thuc luc dang co loi (an toan: khong bao gio boi
    // roi 1 canh bao qua nhiet dang active).
    // F-05: them autotune_.running() vao dieu kien - truoc day Auto Tune (chi
    // chay khi KHONG co me, xem startAutoTune()) bi loai het khoi canh bao qua
    // nhiet (E111/E112) neu nguoi dung tung tat "Bao nhiet ngoai me", vo tinh
    // tat luon lop bao ve khan cap trong luc dang chu dong dieu khien nhiet do
    // (relay mode). Tuy chinh nay chi de giam bao gia (hop nong), khong nham
    // tat bao ve trong luc thuc su dang gia nhiet.
    const bool tempAlarmEligible = config_.highTempAlarmWithoutBatch || batchRunning_ ||
                                    autotune_.running() || testModeActive_;

    // Cap 3 vao ngay bang mau hop le dau tien, sau do giu qua mat cam bien.
    if (tempAlarmEligible && validSafety && safetyTemp >= config_.emergencyTemp) {
      emergencyActive_ = true;
      emergencyClearTimer_.reset();
    } else if (emergencyActive_) {
      const bool safeToClear = validSafety &&
          safetyTemp <= config_.emergencyTemp - EMERGENCY_CLEAR_HYSTERESIS_C;
      if (emergencyClearTimer_.update(now, safeToClear,
                                     EMERGENCY_CLEAR_CONFIRM_MS)) {
        emergencyActive_ = false;
        emergencyClearTimer_.reset();
      }
    }

    // Cap 2 co xac nhan vao va xac nhan thoat rieng, khong chap chon contactor.
    if (!highTemperatureActive_) {
      const bool highTrip = tempAlarmEligible && validSafety &&
                            safetyTemp >= config_.highTempAlarm;
      if (highTripTimer_.update(now, highTrip, HIGH_TEMP_CONFIRM_MS)) {
        highTemperatureActive_ = true;
        highClearTimer_.reset();
      }
    } else if (!emergencyActive_) {
      const bool safeToClear = validSafety &&
          safetyTemp <= config_.highTempAlarm - HIGH_TEMP_CLEAR_HYSTERESIS_C;
      if (highClearTimer_.update(now, safeToClear,
                                HIGH_TEMP_CLEAR_CONFIRM_MS)) {
        highTemperatureActive_ = false;
        highTripTimer_.reset();
        highClearTimer_.reset();
      }
    }
    if (emergencyActive_) highTemperatureActive_ = true;

    if (autotune_.running()) {
      ventTemperatureActive_ = false;
    } else {
      if (!ventTemperatureActive_ && validSafety &&
          safetyTemp >= config_.ventOnTemp) ventTemperatureActive_ = true;
      if (ventTemperatureActive_ && validSafety &&
          safetyTemp <= config_.ventOffTemp) ventTemperatureActive_ = false;
      if (!validSafety) ventTemperatureActive_ = false;
    }

    if ((!wasHigh && highTemperatureActive_) ||
        (!wasEmergency && emergencyActive_)) {
      pid_.reset();
      postCoolUntil_ = now + POST_COOL_MS;
    }
    // High la STOP fault: SSR + contactor tong deu da bi cat. Khi High clear,
    // reset PID; OutputArbiter chi duoc dong lai master sau minimum-OFF/pickup.
    // Emergency con co them HEAT_RESTART_LOCKOUT_MS o nhanh duoi.
    if (wasHigh && !highTemperatureActive_) {
      pid_.reset();
    }
    // Sau cap khan cap contactor tong nhiet da bi nha, can khoa khoi dong lai co thoi han.
    if (wasEmergency && !emergencyActive_) {
      pid_.reset();
      heatRestartNotBefore_ = now + HEAT_RESTART_LOCKOUT_MS;
      sirenMutedUntil_ = 0;
    }

    const bool lowEligible = batchRunning_ && sensorUsable_ &&
        elapsedBatchMs(now) >= LOW_TEMP_STARTUP_GRACE_MS;
    const bool lowCondition = lowEligible && temperature_ <= config_.lowTempAlarm;
    lowTemperatureActive_ = lowTempTimer_.update(now, lowCondition,
                                                 LOW_TEMP_CONFIRM_MS);
    if (lowTemperatureActive_ && temperature_ >= config_.lowTempAlarm + 0.2f) {
      lowTemperatureActive_ = false;
      lowTempTimer_.reset();
    }

    const bool humCondition = batchRunning_ && sensorUsable_ &&
                              humidity_ <= config_.lowHumidityAlarm;
    humidityLowActive_ = humidityTimer_.update(
        now, humCondition, static_cast<uint32_t>(config_.humidityAlarmDelaySec) * 1000UL);
    if (humidityLowActive_ && humidity_ >= config_.lowHumidityAlarm + 2.0f) {
      humidityLowActive_ = false;
      humidityTimer_.reset();
    }

    const bool humHighCondition = batchRunning_ && sensorUsable_ &&
                                  humidity_ >= HUMIDITY_HIGH_ALARM_C;
    humidityHighActive_ = humidityHighTimer_.update(
        now, humHighCondition, static_cast<uint32_t>(config_.humidityAlarmDelaySec) * 1000UL);
    if (humidityHighActive_ && humidity_ <= HUMIDITY_HIGH_ALARM_C - HUMIDITY_HIGH_HYSTERESIS_C) {
      humidityHighActive_ = false;
      humidityHighTimer_.reset();
    }

    // Toc do tang/giam nhiet bat thuong: so sanh voi chinh no TEMP_RATE_WINDOW_MS
    // truoc. Chi danh gia lai moi khi het 1 khung gio (khong phai lien tuc) -
    // du cho canh bao chan doan phu, khong phai cat an toan tuc thi.
    if (!isfinite(tempRateRefValue_) || elapsedMs(now, tempRateRefAt_) >=
                                             config_.tempRateWindowSec * 1000UL) {
      if (isfinite(tempRateRefValue_) && sensorUsable_ && isfinite(temperature_) && batchRunning_) {
        temperatureRateActive_ = fabsf(temperature_ - tempRateRefValue_) >= config_.tempRateLimitC;
      } else {
        temperatureRateActive_ = false;
      }
      tempRateRefValue_ = (sensorUsable_ && isfinite(temperature_)) ? temperature_ : NAN;
      tempRateRefAt_ = now;
    }

    // Dao dong nhiet mat on dinh: dem so lan doi dau quanh diem dat (ra khoi
    // dai hysteresis) trong 1 khung gio co dinh TEMP_OSCILLATION_WINDOW_MS.
    if (elapsedMs(now, tempOscillationWindowStart_) >=
        config_.tempOscillationWindowSec * 1000UL) {
      tempOscillationLastWindowCrossCount_ = tempOscillationCrossCount_;
      temperatureUnstableActive_ = batchRunning_ && sensorUsable_ &&
          tempOscillationLastWindowCrossCount_ >= config_.tempOscillationCrossLimit;
      tempOscillationCrossCount_ = 0U;
      tempOscillationWindowStart_ = now;
      tempOscillationLastSign_ = 0;
    }
    if (sensorUsable_ && isfinite(temperature_)) {
      const float diff = temperature_ - config_.targetTemp;
      const int8_t sign = (diff > config_.tempHysteresis) ? 1 :
                           (diff < -config_.tempHysteresis) ? -1 : tempOscillationLastSign_;
      if (tempOscillationLastSign_ != 0 && sign != 0 && sign != tempOscillationLastSign_ &&
          tempOscillationCrossCount_ < UINT8_MAX) {
        ++tempOscillationCrossCount_;
      }
      if (sign != 0) tempOscillationLastSign_ = sign;
    }

    // Watchdog dap ung nhiet: khong con doi SSR phai ON LIEN TUC. PID co the
    // chia xung theo cua so nen ta tich luy thoi gian SSR THUC SU ON (tuong
    // duong nang luong full-power). Neu dang can gia nhiet ma da cap du tong
    // on-time cau hinh nhung PV khong tang toi thieu -> nghi thanh nhiet/SSR/
    // sensor khong dap ung. Loi nay cat CA SSR + contactor tong va bat 2 quat.
    const bool responseDemand = batchRunning_ && inputs_.state().heaterEnable &&
        sensorUsable_ && isfinite(temperature_) &&
        temperature_ < config_.targetTemp - config_.tempHysteresis;
    const uint32_t responseRequiredOnMs =
        static_cast<uint32_t>(config_.heaterStuckDurationSec) * 1000UL;
    if (!responseDemand) {
      heaterStuckTracking_ = false;
      heaterStuckSinceAt_ = now;
      heaterStuckAccumOnMs_ = 0U;
      heaterStuckStartTemp_ = NAN;
      heaterNotHeatingActive_ = false;
    } else {
      if (!heaterStuckTracking_) {
        heaterStuckTracking_ = true;
        heaterStuckSinceAt_ = now;
        heaterStuckAccumOnMs_ = 0U;
        heaterStuckStartTemp_ = temperature_;
        heaterNotHeatingActive_ = false;
      } else {
        // Gioi han dt 1 s de mot lan task tre bat thuong khong duoc tinh nhu
        // da cap nhiet lien tuc trong ca khoang tre do.
        const uint32_t dt = std::min<uint32_t>(elapsedMs(now, heaterStuckSinceAt_), 1000UL);
        heaterStuckSinceAt_ = now;
        // GPIO1 energizes both physical SSRs: ON is full 16 kW bank power.
        const uint32_t energyMs = outputs_.state().heaterSsr ? dt : 0U;
        if (heaterStuckAccumOnMs_ < UINT32_MAX - energyMs) {
          heaterStuckAccumOnMs_ += energyMs;
        }
        const float rise = isfinite(heaterStuckStartTemp_)
            ? temperature_ - heaterStuckStartTemp_ : 0.0f;
        if (isfinite(heaterStuckStartTemp_) &&
            rise >= config_.heaterStuckMinRiseC) {
          // He thong co dap ung: bat dau cua so nang luong moi tu PV hien tai.
          heaterStuckStartTemp_ = temperature_;
          heaterStuckAccumOnMs_ = 0U;
          heaterNotHeatingActive_ = false;
        } else if (heaterStuckAccumOnMs_ >= responseRequiredOnMs) {
          heaterNotHeatingActive_ = true;
        }
      }
    }

    const bool sensorGrace = !timeReached(now, sensorStartupGraceUntil_) &&
                             !sensorUsable_;
    faults_.set(FaultCode::SensorLost, !sensorUsable_ && !sensorGrace, now,
                static_cast<int16_t>(std::min<uint32_t>(sensor_.dataAgeMs() / 100U,
                                                       INT16_MAX)));
    faults_.set(FaultCode::SensorInvalid,
                !sensorGrace && sensor_.online() && !safetySampleValid_, now);
    faults_.set(FaultCode::SensorSuspect, !sensorGrace && sensorSuspect_, now,
                isfinite(lastSuspectCandidate_)
                    ? static_cast<int16_t>(lroundf(lastSuspectCandidate_ * 10.0f))
                    : 0);
    faults_.set(FaultCode::LowTemperature, lowTemperatureActive_, now,
                isfinite(temperature_) ? static_cast<int16_t>(lroundf(temperature_ * 10.0f)) : 0);
    faults_.set(FaultCode::HighTemperature, highTemperatureActive_, now,
                isfinite(safetyTemp) ? static_cast<int16_t>(lroundf(safetyTemp * 10.0f)) : 0);
    faults_.set(FaultCode::EmergencyTemperature, emergencyActive_, now,
                isfinite(safetyTemp) ? static_cast<int16_t>(lroundf(safetyTemp * 10.0f)) : 0);
    faults_.set(FaultCode::HumidityLow, humidityLowActive_, now,
                isfinite(humidity_) ? static_cast<int16_t>(lroundf(humidity_ * 10.0f)) : 0);
    faults_.set(FaultCode::HumidityHigh, humidityHighActive_, now,
                isfinite(humidity_) ? static_cast<int16_t>(lroundf(humidity_ * 10.0f)) : 0);
    faults_.set(FaultCode::TemperatureRateExceeded, temperatureRateActive_, now,
                isfinite(temperature_) ? static_cast<int16_t>(lroundf(temperature_ * 10.0f)) : 0);
    faults_.set(FaultCode::TemperatureUnstable, temperatureUnstableActive_, now,
                static_cast<int16_t>(tempOscillationLastWindowCrossCount_));
    faults_.set(FaultCode::HeaterNotHeating, heaterNotHeatingActive_, now,
                (isfinite(heaterStuckStartTemp_) && isfinite(temperature_))
                    ? static_cast<int16_t>(lroundf((temperature_ - heaterStuckStartTemp_) * 10.0f)) : 0);
    // F-08: cam bien "dung hinh" - chi canh bao trong luc dang co me (giong
    // cach LowTemperature/HumidityLow chi xet trong batch), vi ngoai me gia
    // tri on dinh dai han la binh thuong (khong dieu nhiet chu dong).
    // Khong cat nhiet chi vi buong dang on dinh that su. SensorFrozen chi
    // duoc nang thanh STOP khi gia tri dung hinh lau VA trong chinh thoi gian
    // do heater da cap mot luong on-time dang ke ma PV van khong nhuc nhich.
    // Dieu nay tranh false-trip o diem dat, nhung van bat duoc tinh huong nguy
    // hiem "sensor ket o muc thap -> PID cu tiep tuc gia nhiet".
    const uint32_t frozenEvidenceOnMs = std::max<uint32_t>(60000UL,
        (static_cast<uint32_t>(config_.heaterStuckDurationSec) * 1000UL) / 2UL);
    const bool sensorFrozenActive = batchRunning_ && sensorUsable_ &&
        isfinite(sensorFrozenRefTemp_) &&
        elapsedMs(now, sensorFrozenSince_) >= SENSOR_FROZEN_TIMEOUT_MS &&
        heaterStuckAccumOnMs_ >= frozenEvidenceOnMs;
    faults_.set(FaultCode::SensorFrozen, sensorFrozenActive, now,
                isfinite(sensorFrozenRefTemp_)
                    ? static_cast<int16_t>(lroundf(sensorFrozenRefTemp_ * 10.0f)) : 0);
    // Phan anh dung trang thai turnMechanicalCheckRequired_ (bat trong
    // latchTurnFault() khi loi dao lap lai qua nguong, tat trong
    // updateTestMode() khi da xac nhan lai ca 2 CTHT).
    faults_.set(FaultCode::TurnMechanicalCheckRequired,
                turnMechanicalCheckRequired_, now,
                static_cast<int16_t>(turnFaultStreak_));
    // Qua so ngay ap du kien - canh bao tai cho, doc lap voi Cloud Push
    // tuong ung trong cloud_alert_link.h (khong dung runtime_.currentDay vi
    // gia tri do bi gioi han khong vuot qua totalIncubationDays, xem
    // updateBatchTime()/copyRuntimeToHmi()).
    const uint32_t batchOverdueDays = batchRunning_ && config_.totalIncubationDays > 0U
        ? elapsedBatchSec(now) / 86400UL
        : 0U;
    faults_.set(FaultCode::BatchOverdue,
                batchRunning_ && config_.totalIncubationDays > 0U &&
                batchOverdueDays >= static_cast<uint32_t>(config_.totalIncubationDays),
                now, static_cast<int16_t>(std::min<uint32_t>(batchOverdueDays, INT16_MAX)));
    const InputState &in = inputs_.state();
    const bool recoveryExecuting = resumePending_ && !resumeConfirmationRequired_;
    const bool heatFunctionRequired = batchRunning_ || recoveryExecuting;

    // E130 bao ngay khi mat quyen nhiet. Khi cong tac bat lai, doi them 1 s
    // on dinh truoc khi xoa loi; trong thoi gian nay Fault Manager van khoa
    // D1/D11 nen contactor khong the dong lai do tiep diem rung.
    if (!heatFunctionRequired) {
      heaterSwitchFaultActive_ = false;
      heaterRestoreTimer_.reset();
    } else if (!in.heaterEnable) {
      heaterSwitchFaultActive_ = true;
      heaterRestoreTimer_.reset();
    } else if (heaterSwitchFaultActive_ &&
               heaterRestoreTimer_.update(
                   now, true, ESSENTIAL_INPUT_RESTORE_CONFIRM_MS)) {
      heaterSwitchFaultActive_ = false;
      heaterRestoreTimer_.reset();
    }

    // Trong me, AUTO OFF khong con chuyen he thong sang MANUAL. Firmware
    // tiep tuc PID + quat tuan hoan bao ve me va khoa toan bo dao. Vi vay
    // cong tac quat tay khong con la dieu kien E131 trong mot me dang chay.
    // AUTO OFF hien trang thai suy giam ngay, nhung chi phat E133/coi sau 2 phut.
    // Khi AUTO tro lai thi timer va fault duoc xoa ngay.
    autoModeFaultActive_ = autoLostTimer_.update(
        now, batchRunning_ && !in.autoMode, AUTO_LOST_ALARM_DELAY_MS);

    faults_.set(FaultCode::HeaterSwitchOffDuringBatch,
                heaterSwitchFaultActive_, now);
    faults_.set(FaultCode::AutoModeOffDuringBatch,
                autoModeFaultActive_, now);
    faults_.set(FaultCode::AutoTurningDisabledDuringBatch,
                batchRunning_ && !turningLockdownActive(now) &&
                !config_.turningEnabled, now);
    faults_.set(FaultCode::BatchLogUnavailable,
                (batchRunning_ || resumePending_) &&
                (batchLogFaultActive_ || !batchLogger_.healthy()), now);
    faults_.set(FaultCode::ResumeRequiresAuto,
                recoveryExecuting && !in.autoMode, now);
    faults_.set(FaultCode::StorageUnavailable, storageFaultLatched_, now);
    faults_.set(FaultCode::StorageDegraded, storageDegraded_, now);
    faults_.set(FaultCode::AbnormalReset, abnormalResetLatched_, now,
                static_cast<int16_t>(resetReason_));
    faults_.set(FaultCode::BatchStateClearPending, batchClearPending_, now,
                static_cast<int16_t>(batchClearAttemptCount_));
    faults_.set(FaultCode::SafetyJournalUnavailable,
                safetyJournalFaultLatched_, now);
    runtime_.alarmMask = faults_.alarmMask();
  }

  // ----------------------------- Auto Tune -----------------------------------
  void updateAutoTune(uint32_t now) {
    if (!autotune_.running()) return;
    const InputState &in = inputs_.state();
    // ACTUAL heater state (post-arbiter) is what identification integrates, never the request.
    autotune_.tick(now, outputs_.state().heaterSsr && outputs_.state().heatMaster);
    const AutoTunePhase previousPhase = autotune_.phase();
    const uint32_t previousCycle = autotune_.cycleSerial();
    const uint32_t previousValidation = autotune_.validationSerial();
    const uint32_t previousModel = autotune_.modelSerial();
    AutoTuneReason abortReason = AutoTuneReason::None;
    if (!sensorUsable_ || !isfinite(temperature_) || !isfinite(rawTemperature_))
      abortReason = AutoTuneReason::SensorAbort;
    else if (highTemperatureActive_ || emergencyActive_ ||
        std::max(rawTemperature_, temperature_) >= config_.highTempAlarm ||
        storageFaultLatched_ || storageDegraded_ || abnormalResetLatched_ ||
        safetyJournalFaultLatched_ || batchClearPending_ ||
        faults_.masterDropRequired() || faults_.ssrInhibited() ||
        mayapFirmwareMaintenanceActive() || mayapSystemTripLatched() ||
        !mayapBootOperationsReady()) abortReason = AutoTuneReason::SafetyAbort;
    else if (!in.autoMode || !in.heaterEnable || batchRunning_ || testModeActive_)
      abortReason = AutoTuneReason::ModeAbort;
    if (abortReason != AutoTuneReason::None) autotune_.abort(abortReason);
    autotune_.checkTimeout(now);
    autotune_.noteController(pidPower_, pid_.integral());
    MachineConfig tuned{};
    const bool tunedReady = autotune_.running() && newSensorSample_ &&
        autotune_.update(now, temperature_, config_, tuned);
    if (autotune_.cycleSerial() != previousCycle) {
      const auto &cycle = autotune_.lastCycle();
      mayapSerialPrintf(false, "[TUNE] CYCLE n=%lu high=%.3f low=%.3f A=%.3f Pu=%.1f heat_s=%.1f cool_s=%.1f\n",
          static_cast<unsigned long>(autotune_.cycleSerial()), cycle.high, cycle.low, cycle.amplitude,
          cycle.periodMs * 0.001f, cycle.heatMs * 0.001f, cycle.coolMs * 0.001f);
    }
    if (autotune_.validationSerial() != previousValidation)
      mayapSerialPrintf(false, "[TUNE] VALIDATING cycles=%u quality=%s\n",
          autotune_.cycleCount(), autoTuneReasonName(autotune_.rejection()));
    if (autotune_.phase() != previousPhase && autotune_.running())
      mayapSerialPrintf(false, "[TUNE] %s power=%.1f%% PV=%.3f\n",
          autoTunePhaseName(autotune_.phase()), autotune_.power(), temperature_);
    if (autotune_.modelSerial() != previousModel)
      mayapSerialPrintf(false, "[TUNE] MODEL gain=%.5f delay=%.1fs coast100=%.2fC hold=%.1f%% Ku=%.2f Pu=%.0fs conf=%u cand Kp=%.2f Ki=%.4f\n",
          autotune_.model().gain, autotune_.model().delaySec, autotune_.model().coast100C, autotune_.model().holdPct,
          autotune_.model().ku, autotune_.model().periodSec, static_cast<unsigned>(autotune_.model().confidence),
          autotune_.candidateKp(), autotune_.candidateKi());
    // Candidate takes over: no relay excitation, backlog or integral is carried into the closed-loop check.
    float holdPct = 0.0f;
    if (autotune_.takeValidationStart(holdPct)) {
      MachineConfig candidate = config_;
      autotune_.applyCandidate(candidate);
      heaterBurst_.reset();
      startupHeat_.reset();
      (void)candidate; (void)holdPct;
      pid_.reset();           // zero integral: the check must include the wind-up / recovery a real approach produces
      pidPower_ = 0.0f;
    }
    if (!tunedReady && autotune_.state() == AutoTuneState::Failed) {
      heaterBurst_.reset();
      pid_.reset();
      startupHeat_.reset();
      pidPower_ = 0.0f;
      postCoolUntil_ = now + POST_COOL_MS;
      heatRestartNotBefore_ = now + HEAT_RESTART_LOCKOUT_MS;
      eventLog_.push(now, EventType::AutoTuneEnd,
                     static_cast<uint16_t>(EventCode::AutoTuneFailed),
                     static_cast<int16_t>(autotune_.reason()), 1U);
      mayapSerialPrintf(false, "[TUNE] FAIL reason=%s quality=%s over=%.2f mae=%.3f p95=%.3f ripple=%.2f (old PID and profile kept)\n",
          autoTuneReasonName(autotune_.reason()), autoTuneReasonName(autotune_.rejection()),
          autotune_.eval().overshoot, autotune_.eval().mae, autotune_.eval().p95, autotune_.eval().ripple);
      return;
    }
    if (tunedReady) {
      MachineConfig readback{};
      if (store_.saveConfig(tuned, readback)) {
        config_ = readback;
        hmiSetConfig(config_);
        mayapRealtimeSetConfig(config_);
        mayapCloudSetConfig(config_);
        mayapSetConnectivityMode(config_.connectivityMode);
        // Accepted: the measured plant seeds the thermal profile (a seed, not full trust: Adaptive V1 keeps refining
        // it online and withdraws it on a long mismatch). The PID record above is the commit point; the profile
        // record is a capped seed, so losing power between the two leaves a safe pair (new PID, old/no profile).
        if (autotune_.hasProfile() && config_.adaptiveThermalBalanceEnabled) {
          MayapThermal::ThermalProfile seed = autotune_.profile();
          seed.signature = thermalSignature();
          seed.epoch = rtc_.epoch();
          seed.sequence = 0;
          MayapThermal::sealProfile(seed);
          thermalV1_.learner().seed(seed, 75, seed.holdPowerPct > 0.5f);
          MayapThermal::profileStorage.offerForced(seed);
        }
        eventLog_.push(now, EventType::AutoTuneEnd,
                       static_cast<uint16_t>(EventCode::AutoTuneSuccess),
                       static_cast<int16_t>(lroundf(config_.kp * 10.0f)));
        mayapSerialPrintf(false, "[TUNE] RESULT phase=SUCCESS Kp=%.3f Ki=%.5f Kd=%.3f gain=%.5f delay=%.1fs mae=%.3f p95=%.3f ripple=%.3f over=%.3f\n",
                         config_.kp, config_.ki, config_.kd, autotune_.model().gain, autotune_.model().delaySec,
                         autotune_.eval().mae, autotune_.eval().p95, autotune_.eval().ripple, autotune_.eval().overshoot);
      } else {
        // Save failed: nothing was applied (the candidate only ever lived in the engine), old PID/profile stay.
        autotune_.abort(AutoTuneReason::SaveFailed);
        latchStorageFault("AUTOTUNE SAVE");
        eventLog_.push(now, EventType::AutoTuneEnd,
                       static_cast<uint16_t>(EventCode::AutoTuneFailed),
                       static_cast<int16_t>(AutoTuneReason::SaveFailed), 1U);
        mayapSerialPrintf(false, "[TUNE] FAIL reason=SAVE_FAILED\n");
      }
      heaterBurst_.reset();
      pid_.reset();
      startupHeat_.reset();
      pidPower_ = 0.0f;
      postCoolUntil_ = now + POST_COOL_MS;
      heatRestartNotBefore_ = now + HEAT_RESTART_LOCKOUT_MS;
    }
  }

  // ----------------------------- AUTO/MANUAL ---------------------------------
  void processInputModeTransition(uint32_t now) {
    const bool autoMode = inputs_.state().autoMode;
    if (autoMode == previousAutoMode_) return;
    previousAutoMode_ = autoMode;
    stopTurn(false);
    // KHONG dat manualTurnRearmRequired_ = true o day: cong tac dao vat ly
    // la quyen cao nhat, gat sang MANUAL ma cong tac dang giu huong nao thi
    // quay ngay huong do, khong bat buoc nha-bam lai (yeu cau nguoi lap dat).
    // Chi xet heaterSsr, khong xet heatMaster (cung ly do nhu trong
    // updateHeatingAndOutputs() - heatMaster co the BAT chi vi cong tac vat
    // ly, khong dam bao dang thuc su cap nhiet).
    if (!autoMode && !inputs_.state().circulationFan &&
        outputs_.state().heaterSsr) {
      postCoolUntil_ = now + POST_COOL_MS;
    }
    if (autoMode) {
      const InputState &in = inputs_.state();
      if (in.limitLeft) trayPosition_ = TrayPosition::Left;
      else if (in.limitRight) trayPosition_ = TrayPosition::Right;
      else { trayPosition_ = TrayPosition::Unknown; needHome_ = batchRunning_; }
      if (batchRunning_) scheduleNextTurnFromAnchor(now);
    }
    EventCode modeEvent = autoMode ? EventCode::ModeAuto : EventCode::ModeManual;
    if (batchRunning_) {
      modeEvent = autoMode ? EventCode::AutoRestoredDuringBatch
                           : EventCode::AutoLostDuringBatch;
    }
    eventLog_.push(now, EventType::ModeChanged,
                   static_cast<uint16_t>(modeEvent));
    mayapSerialPrintf(false, "[MODE] %s%s\n", autoMode ? "AUTO" : "AUTO OFF",
                     batchRunning_ ? " (BATCH PROTECTED)" : "");
  }

  // ----------------------------- Turning -------------------------------------
  bool turningLockdownActive(uint32_t now) const {
    if (!batchRunning_ || TURN_LOCKDOWN_DAYS == 0U ||
        config_.totalIncubationDays <= TURN_LOCKDOWN_DAYS) return false;
    const uint32_t stopAfterSec =
        static_cast<uint32_t>(config_.totalIncubationDays - TURN_LOCKDOWN_DAYS) * 86400UL;
    return elapsedBatchSec(now) >= stopAfterSec;
  }

  void setTurnScheduleAnchor(uint32_t now) {
    lastTurnAt_ = now;
    lastTurnEpoch_ = rtc_.valid() ? rtc_.epoch() : 0U;
    turnAnchorWaitSince_ = 0U;
    scheduleNextTurnFromAnchor(now);
  }

  void scheduleNextTurnFromAnchor(uint32_t now) {
    if (!batchRunning_ || !config_.turningEnabled || turningLockdownActive(now)) {
      nextTurnAt_ = 0U;
      return;
    }
    const uint32_t intervalSec = static_cast<uint32_t>(config_.turnIntervalMin) * 60UL;
    if (rtc_.valid() && lastTurnEpoch_ != 0U) {
      const MayapTurning::EpochSchedule projected =
          MayapTurning::fromEpoch(rtc_.epoch(), lastTurnEpoch_,
                                  config_.turnIntervalMin);
      if (projected.scheduled) {
        nextTurnAt_ = projected.due ? now : now + projected.remainingMs;
        return;
      }
    }
    const uint32_t intervalMs = intervalSec * 1000UL;
    if (lastTurnAt_ != 0U) {
      nextTurnAt_ = lastTurnAt_ + intervalMs;
      if (timeReached(now, nextTurnAt_)) nextTurnAt_ = now;
    } else if (lastTurnEpoch_ != 0U && !rtc_.valid()) {
      // Phuc hoi me sau reset: da co moc RTC cua lan dao truoc (lastTurnEpoch_)
      // nhung RTC dang khong hop le dung luc can lap lich, nen chua the quy
      // doi ra thoi gian con lai. Cho RTC song lai thay vi cap nguyen mot chu
      // ky moi (se xoa mat toan bo thoi gian da troi qua truoc do). Chi cho
      // toi da mot chu ky; qua thoi han do uu tien dao ngay hon la bo lo han.
      if (turnAnchorWaitSince_ == 0U) turnAnchorWaitSince_ = now;
      if (elapsedMs(now, turnAnchorWaitSince_) >= intervalMs) {
        lastTurnAt_ = now;
        nextTurnAt_ = now;
      } else {
        nextTurnAt_ = 0U;  // giu 0 de ham nay duoc goi lai o chu ky ke tiep
      }
    } else {
      lastTurnAt_ = now;
      nextTurnAt_ = now + intervalMs;
    }
  }

  void updateTurning(uint32_t now) {
    // Che do thu nghiem tu dieu khien dao trai/phai qua xung rieng
    // (updateTestModeOutputs); may trang thai dao binh thuong tam dung hoan
    // toan de khong phat sinh loi dao "ao" trong luc thao tac thu nghiem.
    if (testModeActive_) { stopTurn(false); return; }
    const InputState &in = inputs_.state();
    if (in.limitLeft && in.limitRight) {
      latchTurnFault(FaultCode::TurnLimitConflict, "HAI HANH TRINH CUNG ON");
    }
    if (turnFaultLatched_) { stopTurn(false); turnPhase_ = TurnPhase::Fault; return; }
    if (faults_.turningInhibited() || highTemperatureActive_ || emergencyActive_) {
      stopTurn(false);
      manualTurnRearmRequired_ = true;
      return;
    }
    // AUTO tuyet doi khong duoc khoi dong/re-khoi dong motor khi mau cam bien
    // chua hop le. Kiem tra som de tranh chuoi xung 5 ms sau moi dead-time.
    if (in.autoMode && !sensorUsable_) {
      stopTurn(false);
      manualTurnRearmRequired_ = true;
      return;
    }

    // Xac nhan den dich.
    if (turnPhase_ == TurnPhase::MovingLeft && in.limitLeft) {
      completeTurn(now, TrayPosition::Left);
    } else if (turnPhase_ == TurnPhase::MovingRight && in.limitRight) {
      completeTurn(now, TrayPosition::Right);
    }

    if (turnPhase_ == TurnPhase::MovingLeft || turnPhase_ == TurnPhase::MovingRight) {
      const bool originStillActive =
          (turnPhase_ == TurnPhase::MovingLeft && moveOrigin_ == TrayPosition::Right && in.limitRight) ||
          (turnPhase_ == TurnPhase::MovingRight && moveOrigin_ == TrayPosition::Left && in.limitLeft);
      if (originStillActive && elapsedMs(now, moveStartedAt_) >= TURN_LIMIT_RELEASE_TIMEOUT_MS) {
        latchTurnFault(FaultCode::TurnLimitStuck, "HANH TRINH KHONG NHA"); return;
      }
      if (elapsedMs(now, moveStartedAt_) >=
          static_cast<uint32_t>(config_.turnMaxRunSec) * 1000UL) {
        latchTurnFault(FaultCode::TurnTimeout, "DAO QUA THOI GIAN"); return;
      }
    }

    if (turnPhase_ == TurnPhase::DeadtimeLeft && timeReached(now, deadtimeUntil_)) {
      beginPhysicalMove(now, TurnDirection::Left);
    } else if (turnPhase_ == TurnPhase::DeadtimeRight && timeReached(now, deadtimeUntil_)) {
      beginPhysicalMove(now, TurnDirection::Right);
    }

    // resumePending_ (dang cho nguoi dung xac nhan Ap lai/Huy - CHUA phai
    // dang ap that su) van khoa dao tay khi AUTO tat, giu nguyen y dinh an
    // toan cu cho giai doan chua xac nhan nay.
    if (resumePending_ && !batchRunning_ && !in.autoMode) {
      stopTurn(false);
      manualTurnRearmRequired_ = true;
      return;
    }
    // Theo yeu cau nguoi lap dat: me DANG CHAY that su (batchRunning_) ma
    // AUTO bi tat thi KHONG con khoa dao tay nua - roi xuong MANUAL turning
    // binh thuong nhu ngoai me (cong tac dao trai/phai dang giu huong nao
    // quay ngay huong do, khong bi buoc nha-bam lai vi khong con set
    // manualTurnRearmRequired_ o day). Nhiet van duoc dieu khien doc lap
    // (xem updateHeatingAndOutputs()), khong bi anh huong. AUTO OFF vi the
    // chi con la CANH BAO (E133, xem faultDescriptor()) nhac gat lai AUTO
    // sau AUTO_LOST_ALARM_DELAY_MS, khong con cat dao. Vi tri khay (trayPosition_)
    // van duoc cap nhat dung tu cong tac hanh trinh trong luc dao tay (xem
    // completeTurn(), dung chung cho ca dao AUTO va MANUAL) nen khi gat lai
    // AUTO, logic tim goc/tiep tuc lich (needHome_/batchPhase_ Homing) hoat
    // dong y het truoc gio, khong can thay doi gi them.
    if (!in.autoMode) {
      updateManualTurning(now, in);
      return;
    }

    // 3 ngay cuoi: khoa dao tu dong de trung vao tu the no.
    if (turningLockdownActive(now)) {
      stopTurn(false);
      nextTurnAt_ = 0U;
      return;
    }

    // AUTO: cong tac dao tay bi vo hieu hoa hoan toan.
    if (!batchRunning_ || !config_.turningEnabled || batchPhase_ == BatchPhase::Prestart) {
      if (turnPhase_ != TurnPhase::Idle) stopTurn(false);
      return;
    }

    if (batchPhase_ == BatchPhase::Homing || needHome_) {
      if (turnPhase_ == TurnPhase::Idle) {
        if (in.limitLeft) { trayPosition_ = TrayPosition::Left; finishHoming(now); }
        else if (in.limitRight) { trayPosition_ = TrayPosition::Right; finishHoming(now); }
        else requestTurn(now, HOME_TO_LEFT ? TurnDirection::Left : TurnDirection::Right,
                         true, false);
      }
      return;
    }

    if (nextTurnAt_ == 0U) scheduleNextTurnFromAnchor(now);
    // Zero means waiting for the persisted RTC anchor, not an expired deadline.
    if (nextTurnAt_ == 0U) return;
    if (turnPhase_ == TurnPhase::Idle && timeReached(now, nextTurnAt_)) {
      if (trayPosition_ == TrayPosition::Left) {
        requestTurn(now, TurnDirection::Right, false, true);
      } else if (trayPosition_ == TrayPosition::Right) {
        requestTurn(now, TurnDirection::Left, false, true);
      } else {
        needHome_ = true;
      }
    }
  }

  void updateManualTurning(uint32_t now, const InputState &in) {
    bool left = in.turnLeft;
    bool right = in.turnRight;
    if (manualTurnRearmRequired_) {
      if (left || right) {
        stopTurn(false);
        return;
      }
      manualTurnRearmRequired_ = false;
    }

    if (left && right) {
      if (manualConflictSince_ == 0U) manualConflictSince_ = now;
      stopTurn(false);
      if (elapsedMs(now, manualConflictSince_) >= TURN_INPUT_CONFLICT_MS) {
        latchTurnFault(FaultCode::TurnCommandConflict, "HAI LENH DAO CUNG ON");
      }
      return;
    }
    manualConflictSince_ = 0;

    if (!left && !right) {
      if (turnPhase_ != TurnPhase::Idle) stopTurn(false);
      return;
    }
    if (turnPhase_ == TurnPhase::Idle) {
      requestTurn(now, left ? TurnDirection::Left : TurnDirection::Right,
                  false, false);
    } else if ((left && (turnPhase_ == TurnPhase::MovingRight ||
                        turnPhase_ == TurnPhase::DeadtimeRight)) ||
               (right && (turnPhase_ == TurnPhase::MovingLeft ||
                          turnPhase_ == TurnPhase::DeadtimeLeft))) {
      stopTurn(false); // nha het truoc, lan update sau moi doi chieu
    }
  }

  void requestTurn(uint32_t now, TurnDirection direction, bool homing, bool countMove) {
    const InputState &in = inputs_.state();
    if (in.limitLeft && in.limitRight) { latchTurnFault(FaultCode::TurnLimitConflict, "LOI HANH TRINH"); return; }
    if ((direction == TurnDirection::Left && in.limitLeft) ||
        (direction == TurnDirection::Right && in.limitRight)) {
      trayPosition_ = direction == TurnDirection::Left
          ? TrayPosition::Left : TrayPosition::Right;
      if (homing) finishHoming(now);
      return;
    }
    stopTurn(false);
    moveIsHoming_ = homing;
    moveCounts_ = countMove;
    moveOrigin_ = in.limitLeft ? TrayPosition::Left
                : in.limitRight ? TrayPosition::Right : TrayPosition::Unknown;
    deadtimeUntil_ = now + TURN_DIRECTION_DEADTIME_MS;
    turnPhase_ = direction == TurnDirection::Left
        ? TurnPhase::DeadtimeLeft : TurnPhase::DeadtimeRight;
  }

  void beginPhysicalMove(uint32_t now, TurnDirection direction) {
    const InputState &in = inputs_.state();
    if ((direction == TurnDirection::Left && in.limitLeft) ||
        (direction == TurnDirection::Right && in.limitRight)) {
      completeTurn(now, direction == TurnDirection::Left
                        ? TrayPosition::Left : TrayPosition::Right);
      return;
    }
    moveStartedAt_ = now;
    turnPhase_ = direction == TurnDirection::Left
        ? TurnPhase::MovingLeft : TurnPhase::MovingRight;
    eventLog_.push(now, EventType::Turning,
        static_cast<uint16_t>(moveIsHoming_
            ? (direction == TurnDirection::Left ? EventCode::TurnHomeLeft
                                                : EventCode::TurnHomeRight)
            : (direction == TurnDirection::Left ? EventCode::TurnStartLeft
                                                : EventCode::TurnStartRight)));
  }

  void completeTurn(uint32_t now, TrayPosition position) {
    stopTurn(false);
    // Bat ky chuyen dong nao toi duoc CTHT (homing hay dao thuong) deu chung
    // minh co khi/cong tac hanh trinh dang hoat dong tot - xoa chuoi loi dao
    // lien tiep (xem latchTurnFault()) de khong bi cong don voi lan loi khac
    // nhau ve nguyen nhan.
    turnFaultStreak_ = 0U;
    // F-07: dong bo NVS khi streak ve 0 tu mot lan dao thanh cong - tranh
    // begin() sau nay nap lai gia tri cu con sot trong NVS (xem F-07 tai
    // latchTurnFault()/begin()).
    (void)safetyJournal_.setTurnFaultStreak(0U);
    trayPosition_ = position;
    if (moveIsHoming_) {
      finishHoming(now);
    } else if (moveCounts_) {
      eventLog_.push(now, EventType::Turning,
          static_cast<uint16_t>(position == TrayPosition::Left
              ? EventCode::TurnCompleteLeft : EventCode::TurnCompleteRight));
      if (turnCountBatch_ < UINT32_MAX) ++turnCountBatch_;
      if (turnCountToday_ < UINT16_MAX) ++turnCountToday_;
      config_.nextDirection = position == TrayPosition::Left
          ? TurnDirection::Right : TurnDirection::Left;
      setTurnScheduleAnchor(now);
      (void)saveBatchRecord();
    } else if (config_.manualTurnReanchorsSchedule) {
      // F-13: dao TAY (moveCounts_ false) nhung cau hinh yeu cau dong lich -
      // coi day la mot lan dao THAT cho scheduler, nen doi moc thoi gian dao
      // tu dong. KHONG dem vao turnCountToday_/turnCountBatch_ va KHONG doi
      // nextDirection (thong ke/hien thi Auto van doc lap voi dao tay).
      //
      // Quan trong: luu batch NGAY tai day. Truoc day anchor moi chi nam RAM,
      // neu mat dien truoc checkpoint 5 phut thi reboot nap lai lastTurnEpoch
      // cu va lich dao bi sai.
      setTurnScheduleAnchor(now);
      (void)saveBatchRecord();
    }
    moveIsHoming_ = false;
    moveCounts_ = false;
  }

  void finishHoming(uint32_t now) {
    needHome_ = false;
    if (batchPhase_ == BatchPhase::Homing) batchPhase_ = BatchPhase::Running;

    // HOMING chi xac dinh lai VI TRI CO KHI, khong phai mot lan dao theo lich.
    // Giu nguyen lastTurnEpoch_/lastTurnAt_ va chieu lai deadline tu anchor cu.
    // Nho vay mat dien/reset hoac MANUAL->AUTO khi khay dang o giua khong con
    // lam chu ky dao dem lai tu dau. Neu day la ban ghi legacy khong co anchor,
    // scheduleNextTurnFromAnchor() van tu tao fallback RAM an toan tu thoi diem
    // homing xong.
    scheduleNextTurnFromAnchor(now);
    mayapSerialPrintf(false, "[TURN] HOME OK pos=%s\n",
                     trayPosition_ == TrayPosition::Left ? "LEFT" : "RIGHT");
  }

  void stopTurn(bool fault) {
    (void)fault;
    // Bi ngat DUNG LUC dang di chuyen (chua cham CTHT dich) - trayPosition_
    // dang giu vi tri XUAT PHAT cua lan di nay (moveOrigin_), KHONG PHAI vi
    // tri vat ly thuc te (khay dang o giua). Danh dau Unknown va bat needHome_
    // de lan dao AUTO ke tiep phai ve lai CTHT da biet truoc, thay vi ngam
    // dinh sai rang khay van con o vi tri cu.
    if (turnPhase_ == TurnPhase::MovingLeft || turnPhase_ == TurnPhase::MovingRight) {
      trayPosition_ = TrayPosition::Unknown;
      needHome_ = true;
    }
    if (turnPhase_ != TurnPhase::Fault) turnPhase_ = TurnPhase::Idle;
    moveStartedAt_ = 0;
  }

  void latchTurnFault(FaultCode code, const char *reason) {
    if (!turnFaultLatched_) mayapSerialPrintf(false, "[TURN] FAULT: %s\n", reason ? reason : "UNKNOWN");
    // F-11: goi stopTurn() TRUOC khi chuyen turnPhase_ sang Fault ben duoi -
    // truoc day gan turnPhase_ = Fault truc tiep nen nhanh vo hieu hoa
    // trayPosition_ trong stopTurn() (chi chay khi turnPhase_ dang Moving)
    // khong bao gio duoc kich hoat: khi loi xay ra GIUA luc khay dang di
    // chuyen, trayPosition_ van giu nguyen goc xuat phat cu (moveOrigin_),
    // KHONG PHAI vi tri vat ly thuc te, cho toi khi ACK/bat dau me tinh
    // lai tu CTHT song (chi anh huong hien thi, khong anh huong dieu khien
    // vi moi noi doc trayPosition_ trong updateTurning() deu bi chan boi
    // turnFaultLatched_).
    stopTurn(true);
    turnFaultLatched_ = true;
    turnFaultCode_ = code;
    const uint32_t now = millis();
    faults_.set(code, true, now);
    // Dem chuoi loi "khong hoan tat duoc chuyen dong" (timeout/CTHT khong nha)
    // lien tiep - day la dau hieu nghi ngo hong co khi/day CTHT, khac voi
    // TurnLimitConflict (2 CTHT cung ON, thuong la loi day/ngan mach) hay
    // TurnCommandConflict (loi thao tac 2 lenh tay cung luc).
    if (code == FaultCode::TurnTimeout || code == FaultCode::TurnLimitStuck) {
      if (turnFaultStreak_ < UINT8_MAX) ++turnFaultStreak_;
      // F-07: luu streak vao NVS moi lan tang - song sot qua reboot thay vi
      // ve lai 0, tranh phai gap lai du 3 loi lien tiep tu dau sau mot lan
      // mat dien/watchdog xen giua.
      (void)safetyJournal_.setTurnFaultStreak(turnFaultStreak_);
      if (!turnMechanicalCheckRequired_ &&
          turnFaultStreak_ >= TURN_FAULT_STREAK_LIMIT) {
        turnMechanicalCheckRequired_ = true;
        testLimitVerifiedLeft_ = false;
        testLimitVerifiedRight_ = false;
        faults_.set(FaultCode::TurnMechanicalCheckRequired, true, now);
        (void)safetyJournal_.setTurnMechanicalCheckRequired(true);
        mayapSerialPrintf(false,
            "[TURN] MECHANICAL CHECK REQUIRED sau %u loi lien tiep\n",
            static_cast<unsigned>(turnFaultStreak_));
      }
    }
    turnPhase_ = TurnPhase::Fault;
    moveStartedAt_ = 0;
  }

  bool clearTurnFault() {
    const InputState &in = inputs_.state();
    if ((in.limitLeft && in.limitRight) || in.turnLeft || in.turnRight) return false;
    const uint32_t now = millis();
    if (turnFaultCode_ != FaultCode::None) {
      faults_.set(turnFaultCode_, false, now);
      faults_.acknowledge(now, AlarmTurning);
    }
    turnFaultCode_ = FaultCode::None;
    turnFaultLatched_ = false;
    turnPhase_ = TurnPhase::Idle;
    trayPosition_ = in.limitLeft ? TrayPosition::Left
                  : in.limitRight ? TrayPosition::Right : TrayPosition::Unknown;
    needHome_ = batchRunning_ && in.autoMode &&
                trayPosition_ == TrayPosition::Unknown;
    return true;
  }

  uint32_t adaptiveCompatibility() const {
    const float signature[]={config_.targetTemp,config_.kp,config_.ki,config_.kd,static_cast<float>(config_.controlMode),
      static_cast<float>(config_.maxHeaterPower),config_.tempOffset,
      static_cast<float>(sensor_.sensorProfile()),static_cast<float>(MAYAP_SENSOR_PROFILE),
      MayapAdaptive::Policy::BankWatts,static_cast<float>(HEATER_BURST_QUANTUM_MS),static_cast<float>(PIN_OUT_HEATER_SSR),1.0f};
    return mcCrc32(reinterpret_cast<const uint8_t *>(signature),sizeof(signature));
  }
  // Hardware identity for the stored thermal profile. tempOffset/SP/PID are deliberately NOT part
  // of it: the SHT30 calibration offset and tuning do not change the plant's dynamics.
  uint32_t thermalSignature() const {
    const float signature[]={MayapAdaptive::Policy::BankWatts,static_cast<float>(HEATER_BURST_QUANTUM_MS),
      static_cast<float>(PIN_OUT_HEATER_SSR),static_cast<float>(sensor_.sensorProfile()),
      static_cast<float>(MAYAP_SENSOR_PROFILE),1.0f};
    return mcCrc32(reinterpret_cast<const uint8_t *>(signature),sizeof(signature));
  }
  void trackAdaptiveEnergy(uint32_t now) {
    adaptiveThermal_.tick(now, outputs_.state().heaterSsr);
    thermalV1_.tick(now, outputs_.state().heaterSsr);
    if(testModeActive_ || resumeConfirmationRequired_){
      MayapAdaptive::modelStorage.discardPending();
      MayapAdaptive::Observation o; o.test=testModeActive_;o.recovery=resumeConfirmationRequired_;
      adaptiveThermal_.update(now,config_.adaptiveThermalBalanceEnabled,config_.maxHeaterPower,o);
    }
  }
  bool adaptiveCoolingRequested() const {return adaptiveThermal_.decision().cooling;}
  float updateAdaptiveBalance(uint32_t now,bool permit,bool fanStable,bool vent) {
    const uint32_t signature=newSensorSample_?adaptiveCompatibility():adaptiveSignature_;
    if(newSensorSample_ && adaptiveSignatureSeen_ && signature!=adaptiveSignature_){
      adaptiveThermal_.invalidate();
      if(config_.adaptiveThermalBalanceEnabled)
        eventLog_.push(now,EventType::Adaptive,static_cast<uint16_t>(EventCode::AdaptiveModelInvalidated),static_cast<int16_t>(MayapAdaptive::Reason::ConfigChanged));
    }
    adaptiveSignature_=signature;adaptiveSignatureSeen_=adaptiveSignatureSeen_||newSensorSample_;
    MayapAdaptive::Observation o;
    o.pv=temperature_;o.raw=rawTemperature_;o.sp=config_.targetTemp;o.high=config_.highTempAlarm;
    o.requested=pidPower_;o.effective=adaptiveThermal_.decision().effective;
    o.sensor=sensorUsable_;o.fanStable=fanStable && outputs_.state().circulationFan;
    o.vent=vent || (outputs_.state().ventFan && !adaptiveCoolingRequested());
    o.cooling=adaptiveCoolingRequested();o.tune=autotune_.running();
    o.safety=highTemperatureActive_||emergencyActive_||ventTemperatureActive_||
      faults_.ssrInhibited()||faults_.masterDropRequired()||mayapSystemTripLatched()||
      storageFaultLatched_||storageDegraded_||safetyJournalFaultLatched_;
    o.recovery=!mayapBootOperationsReady()||abnormalResetLatched_||resumeConfirmationRequired_||
      !timeReached(now,sensorStartupGraceUntil_);
    o.test=testModeActive_;o.maintenance=mayapFirmwareMaintenanceActive();
    // AutoTune must see the unmodified bank. No sample is learned without heat context.
    o.recovery=o.recovery || (!permit && !o.cooling && !o.tune);
    if(newSensorSample_ && config_.adaptiveThermalBalanceEnabled){
      adaptiveThermal_.sample(now,o);
    }
    const auto d=adaptiveThermal_.update(now,config_.adaptiveThermalBalanceEnabled,config_.maxHeaterPower,o,MAYAP_ADAPTIVE_OBSERVER_ONLY!=0);
    if(newSensorSample_ && (!config_.adaptiveThermalBalanceEnabled || !adaptiveThermal_.observer().estimates().learningValid))
      MayapAdaptive::modelStorage.discardPending();
    if(newSensorSample_ && config_.adaptiveThermalBalanceEnabled && sensorUsable_ && rtc_.valid()){
      if(MayapAdaptive::modelStorage.takeInvalid()){
        eventLog_.push(now,EventType::Adaptive,static_cast<uint16_t>(EventCode::AdaptiveModelInvalidated),static_cast<int16_t>(MayapAdaptive::Reason::SeedInvalid));
        mayapSerialPrintf(false,"[THERMAL-ADAPT] model CRC/version invalid; relearn\n");
      }
      MayapAdaptive::Model seed{};
      if(MayapAdaptive::modelStorage.takeSeed(seed)){
        if(MayapAdaptive::compatibleModel(seed,signature,rtc_.epoch(),abnormalResetLatched_))
          adaptiveThermal_.observer().seed(seed.load,seed.coast,seed.coastSec,seed.hold);
        else {
          eventLog_.push(now,EventType::Adaptive,static_cast<uint16_t>(EventCode::AdaptiveModelInvalidated),static_cast<int16_t>(MayapAdaptive::Reason::SeedInvalid));
          mayapSerialPrintf(false,"[THERMAL-ADAPT] seed discarded\n");
        }
      }
      const auto &estimate=adaptiveThermal_.observer().estimates();
      if(config_.adaptiveThermalBalanceEnabled &&
         MayapAdaptive::persistenceEligible(d,estimate))
        MayapAdaptive::modelStorage.offer(MayapAdaptive::makeModel(estimate,signature,rtc_.epoch(),0));
    }
    if(adaptiveThermal_.changedEnabled()){
      heaterBurst_.reset();
      eventLog_.push(now,EventType::Adaptive,static_cast<uint16_t>(config_.adaptiveThermalBalanceEnabled?
        EventCode::AdaptiveEnabled:EventCode::AdaptiveDisabled));
    }
    const auto &e=adaptiveThermal_.observer().estimates();
    runtime_.adaptiveEnabled=config_.adaptiveThermalBalanceEnabled;
    runtime_.adaptiveState=static_cast<uint8_t>(d.state);runtime_.lastAdaptiveReason=static_cast<uint8_t>(d.reason);
    runtime_.adaptiveConfidence=adaptiveThermal_.effectiveConfidence();runtime_.adaptiveLoadIndex=e.load;
    runtime_.adaptiveCoastRiseC=e.coast;runtime_.adaptiveCoastTimeSec=e.coastSec;
    runtime_.adaptiveHoldPowerPct=e.hold;runtime_.effectiveMaxPowerPct=d.effective;
    runtime_.adaptiveApproachBandC=d.approach;runtime_.adaptiveSelfHeating=d.selfHeating;
    runtime_.adaptiveCoolingDemand=d.coolingDemand;runtime_.observerValidWindows=e.windows;
    if(d.state!=adaptiveLoggedState_){
      eventLog_.push(now,EventType::Adaptive, static_cast<uint16_t>(EventCode::AdaptiveChanged),static_cast<int16_t>(d.state));
      adaptiveLoggedState_=d.state;
    }
    if(config_.adaptiveThermalBalanceEnabled && elapsedMs(now,adaptiveDiagnosticAt_)>=MayapAdaptive::Policy::DiagnosticMs){
      adaptiveDiagnosticAt_=now;
      mayapSerialPrintf(false,"[THERMAL-ADAPT] state=%s conf=%.0f load=%.3f coast=%.3f hold=%.1f max=%.1f approach=%.3f selfheat=%u cool=%u\n",
        MayapAdaptive::stateName(d.state),adaptiveThermal_.effectiveConfidence(),e.load,e.coast,e.hold,d.effective,d.approach,d.selfHeating,d.cooling);
    }
    // ---- Adaptive Thermal V1: learn the effective thermal profile (consumed below by the PID) ----
    thermalV1_.setEnabled(config_.adaptiveThermalBalanceEnabled);
    if(config_.adaptiveThermalBalanceEnabled && newSensorSample_){
      MayapThermal::LearnInput li;
      li.pv=temperature_;li.raw=rawTemperature_;li.sp=config_.targetTemp;li.high=config_.highTempAlarm;
      li.sensor=sensorUsable_;li.fanStable=o.fanStable;li.ventActive=outputs_.state().ventFan;
      li.safety=o.safety;li.tune=o.tune;li.test=o.test;li.maintenance=o.maintenance;
      li.recovery=!mayapBootOperationsReady()||abnormalResetLatched_||resumeConfirmationRequired_||
        !timeReached(now,sensorStartupGraceUntil_);
      li.heaterBlocked=!permit;
      MayapThermal::Hints hints;hints.valid=true;hints.coast=e.coast;hints.coastSec=e.coastSec;
      hints.coastWindows=e.coastWindows;hints.holdWindows=e.holdWindows;hints.hold=e.hold;
      thermalV1_.learner().sample(now,li,hints);
      const uint32_t tsig=thermalSignature();
      if(sensorUsable_ && rtc_.valid()){
        if(MayapThermal::profileStorage.takeInvalid())
          eventLog_.push(now,EventType::Adaptive,static_cast<uint16_t>(EventCode::AdaptiveModelInvalidated),static_cast<int16_t>(MayapAdaptive::Reason::SeedInvalid));
        MayapThermal::ThermalProfile seedProfile{};
        if(MayapThermal::profileStorage.takeSeed(seedProfile)){
          if(MayapThermal::profileCompatible(seedProfile,tsig,abnormalResetLatched_))
            thermalV1_.learner().seed(seedProfile,MayapThermal::seedConfidenceCap(seedProfile,rtc_.epoch()));
          else
            eventLog_.push(now,EventType::Adaptive,static_cast<uint16_t>(EventCode::AdaptiveModelInvalidated),static_cast<int16_t>(MayapAdaptive::Reason::SeedInvalid));
        }
        const auto &lp=thermalV1_.learner().profile();
        const auto ls=thermalV1_.learner().state();
        if((ls==MayapThermal::LearnState::Qualified||ls==MayapThermal::LearnState::Adapting)&&
           thermalV1_.learner().gate()==MayapThermal::GateReason::Open){
          MayapThermal::ThermalProfile out=lp;
          out.signature=tsig;out.epoch=rtc_.epoch();out.sequence=0;MayapThermal::sealProfile(out);
          MayapThermal::profileStorage.offer(out);
        } else MayapThermal::profileStorage.discardPending();
      } else MayapThermal::profileStorage.discardPending();
    } else if(!config_.adaptiveThermalBalanceEnabled) MayapThermal::profileStorage.discardPending();
    return d.effective;
  }

  // Runtime telemetry + rate-limited diagnostics for Adaptive Thermal V1. Read-only.
  void publishThermalLearning(uint32_t now) {
    if(!config_.adaptiveThermalBalanceEnabled){
      runtime_.thermalLearnState=0;runtime_.thermalConfidence=0;runtime_.thermalVentPhase=0;
      return;
    }
    const auto &lrn=thermalV1_.learner();const auto &pr=lrn.profile();
    runtime_.thermalLearnState=pr.state;runtime_.thermalConfidence=pr.confidence;
    runtime_.thermalGain=pr.heaterGain;runtime_.thermalDelaySec=pr.heaterDelaySec;
    runtime_.thermalCoastC=pr.coastRiseC;runtime_.thermalHoldPct=pr.holdPowerPct;
    runtime_.thermalVentGain=pr.ventCoolingGain;runtime_.thermalPredictionError=lrn.predictionError();
    runtime_.thermalVentPhase=static_cast<uint8_t>(thermalV1_.plan().ventPhase);
    const auto st=lrn.state();
    if(st!=thermalLoggedState_){
      eventLog_.push(now,EventType::Adaptive,static_cast<uint16_t>(EventCode::ThermalLearnChanged),static_cast<int16_t>(st));
      thermalLoggedState_=st;
    }
    if(lrn.mismatch()!=thermalMismatchLogged_){
      thermalMismatchLogged_=lrn.mismatch();
      if(thermalMismatchLogged_)
        eventLog_.push(now,EventType::Adaptive,static_cast<uint16_t>(EventCode::ThermalModelMismatch),static_cast<int16_t>(pr.confidence));
    }
    if(elapsedMs(now,thermalDiagnosticAt_)>=30000UL){
      thermalDiagnosticAt_=now;
      mayapSerialPrintf(false,"[THERMAL-LEARN] state=%s conf=%u gain=%.4f delay=%.0f coast=%.2f hold=%.1f ventGain=%.4f ventConf=%u predErr=%.2f gate=%s vent=%s ff=%.1f\n",
        MayapThermal::learnStateName(st),static_cast<unsigned>(pr.confidence),pr.heaterGain,pr.heaterDelaySec,
        pr.coastRiseC,pr.holdPowerPct,pr.ventCoolingGain,static_cast<unsigned>(pr.ventConfidence),
        lrn.predictionError(),MayapThermal::gateReasonName(lrn.gate()),
        MayapThermal::ventPhaseName(thermalV1_.plan().ventPhase),thermalV1_.plan().assist.feedForward);
    }
  }

  // ----------------------------- Heating/Output -------------------------------
  void updateHeatingAndOutputs(uint32_t now) {
    trackAdaptiveEnergy(now);
    startupHeat_.observe(now, outputs_.state().heaterSsr);
    if (testModeActive_) { startupHeat_.reset(); heaterBurst_.reset(); updateTestModeOutputs(now); return; }
    const InputState &in = inputs_.state();
    OutputRequest req{};
    // Cong tac vat ly doi kien tu luc ghi de (hoac chua tung ghi de) - cong
    // tac lay lai quyen dieu khien. Xem ghi chu tai lightWebOverrideActive_.
    if (lightWebOverrideActive_ && in.light != lightWebOverrideRefInput_) {
      lightWebOverrideActive_ = false;
    }
    req.light = lightWebOverrideActive_ ? lightWebOverrideValue_ : in.light; // doc lap AUTO va me

    // Sau mat dien, trong luc dang cho nguoi dung chon TIEP TUC/HUY, tat toan
    // bo co cau, quat hut va nhiet. Den van doc lap de nguoi dung thao tac HMI.
    if (resumeConfirmationRequired_) {
      startupHeat_.reset();
      heaterBurst_.reset();
      req.immediateMasterDrop = true;
      outputs_.update(now, req);
      pid_.reset();
      pidPower_ = 0.0f;
      runtime_.heaterPower = 0.0f;
      return;
    }

    // Canh me vua bat dau (chuyen tu khong chay -> dang chay): dat mot moc
    // tri hoan ngan cho rieng ngo ra quat, de khong dong dien cung luc voi
    // contactor nhiet tong (yeu cau rieng ve dien, xem CIRC_FAN_BATCH_START_
    // STAGGER_MS). Contactor nhiet KHONG bi anh huong - van dong tuc thi.
    if (batchRunning_ && !prevBatchRunningForOutputs_) {
      circFanStaggerUntil_ = now + CIRC_FAN_BATCH_START_STAGGER_MS;
    }
    prevBatchRunningForOutputs_ = batchRunning_;
    const bool circFanStaggerActive = !timeReached(now, circFanStaggerUntil_);

    bool baseCirculation = false;
    const bool protectedBatchControl = batchRunning_;
    if (in.autoMode || protectedBatchControl) {
      // Dang co me: ke ca mat cong tac AUTO, giu PID/quat theo che do bao ve
      // me; AUTO OFF chi khoa dao va sinh E133 sau 120 s. Ngoai me, MANUAL
      // van ton trong quyen dieu khien quat tay nhu cu. circFanStaggerActive
      // chi giu quat cham lai vai tram ms dung luc me vua bat dau - cac
      // dieu kien an toan ben duoi (safetyForcesCirculation) van doc lap va
      // KHONG bi tri hoan.
      baseCirculation = (batchRunning_ && !circFanStaggerActive) || autotune_.running();
    } else {
      baseCirculation = in.circulationFan;
    }
    // Khong cho quat tat cung luc voi bo gia nhiet: giu hau lam mat.
    // CHI xet heaterSsr (dong nhiet THAT su qua D1/PID, van doi hoi batch/
    // autotune dang chay - xem heaterPidConditions ben duoi), KHONG xet
    // heatMaster: tu khi relay tong bam theo cong tac vat ly, no co the BAT
    // ca khi khong co me (cong tac de nguyen) - xet heatMaster o day se lam
    // quat hau lam mat bi kich hoat lai lien tuc, khong bao gio tat duoc.
    if (!baseCirculation && outputs_.state().heaterSsr) {
      postCoolUntil_ = now + POST_COOL_MS;
    }
    const bool postCooling = !timeReached(now, postCoolUntil_);
    const bool sensorStartupGraceActive =
        !timeReached(now, sensorStartupGraceUntil_);
    // Khong bat quat hut chi vi cam bien chua khoi tao xong. Chi lam mat khi
    // loi cam bien da duoc xac nhan va may dang/da co nhiet can xa. (Cung
    // chi xet heaterSsr, ly do nhu tren - khong xet heatMaster.)
    const bool sensorFaultNeedsFan = !sensorUsable_ && !sensorStartupGraceActive &&
        (batchRunning_ || outputs_.state().heaterSsr || postCooling);
    bool scheduledVentActive = false;
    // Ventilation owns WHEN and HOW LONG; thermal is only told, so the heater can be
    // coordinated (Adaptive Thermal V1). Negative == not known.
    float ventStartsInSec = -1.0f, ventRemainingSec = -1.0f;
    if (!config_.ventAutoEnabled && config_.ventScheduleEnabled &&
        batchRunning_ && rtc_.valid()) {
      const uint8_t hours[VENT_SCHEDULE_MAX_RUNS] = {
        config_.ventScheduleHour1, config_.ventScheduleHour2,
        config_.ventScheduleHour3, config_.ventScheduleHour4,
        config_.ventScheduleHour5, config_.ventScheduleHour6
      };
      const uint8_t count = std::min<uint8_t>(config_.ventScheduleCount,
                                               VENT_SCHEDULE_MAX_RUNS);
      const uint32_t secondOfDay = rtc_.epoch() % 86400UL;
      const uint32_t durationSec =
          static_cast<uint32_t>(config_.ventScheduleDurationMin) * 60UL;
      uint32_t nextStartWaitSec = 0xFFFFFFFFUL;
      for (uint8_t i = 0U; i < count; ++i) {
        const uint32_t start = static_cast<uint32_t>(hours[i]) * 3600UL;
        const uint32_t end = start + durationSec;
        const bool inWindow = end <= 86400UL
            ? (secondOfDay >= start && secondOfDay < end)
            : (secondOfDay >= start || secondOfDay < (end - 86400UL));
        if (inWindow) {
          scheduledVentActive = true;
          uint32_t remaining = ((end - secondOfDay) + 86400UL) % 86400UL;
          if (remaining == 0U) remaining = durationSec;
          // Contiguous or overlapping runs keep the fan on: report the end of the whole interval.
          for (uint8_t pass = 0U; pass < count; ++pass) {
            for (uint8_t j = 0U; j < count; ++j) {
              const uint32_t startsIn = (static_cast<uint32_t>(hours[j]) * 3600UL + 86400UL - secondOfDay) % 86400UL;
              if (startsIn > 0U && startsIn <= remaining && startsIn + durationSec > remaining)
                remaining = startsIn + durationSec;
            }
          }
          ventRemainingSec = static_cast<float>(remaining);
          break;
        }
        const uint32_t wait = (start + 86400UL - secondOfDay) % 86400UL;
        if (wait < nextStartWaitSec) nextStartWaitSec = wait;
      }
      if (!scheduledVentActive && nextStartWaitSec != 0xFFFFFFFFUL)
        ventStartsInSec = static_cast<float>(nextStartWaitSec);
    }

    bool profileVentActive = false;
    if (config_.ventAutoEnabled && batchRunning_) {
      const uint32_t elapsedSec = elapsedBatchSec(now);
      const uint32_t day = elapsedSec / 86400UL + 1UL;
      const uint8_t duty = ventProfileDutyPercent(config_, day,
          humidityLowActive_);
      const uint32_t cycleSec = static_cast<uint32_t>(config_.ventCycleMinutes) * 60UL;
      const uint32_t onSec = cycleSec * duty / 100UL;
      profileVentActive = (elapsedSec % cycleSec) < onSec;
      if (onSec > 0UL) {
        const uint32_t phaseSec = elapsedSec % cycleSec;
        if (phaseSec < onSec) ventRemainingSec = static_cast<float>(onSec - phaseSec);
        else ventStartsInSec = static_cast<float>(cycleSec - phaseSec);
      }
    }

    const bool safetyForcesCirculation = faults_.circulationForced() ||
        ventTemperatureActive_ || scheduledVentActive || profileVentActive ||
        sensorFaultNeedsFan || postCooling;
    const bool circulation = baseCirculation || safetyForcesCirculation;
    req.circulationFan = circulation;

    if (circulation != previousFanCommand_) {
      previousFanCommand_ = circulation;
      fanOnSince_ = circulation ? now : 0;
    }
    const bool fanStable = circulation && fanOnSince_ != 0U &&
                           elapsedMs(now, fanOnSince_) >= FAN_PRESTART_MS;

    // Thu tu: Emergency/High Temp > CO2 (chua co cam bien, nen khong tao
    // gia tri gia) > profile ngay ap > gioi han RH. RH chi ha duty profile
    // xuong 5%, khong bao gio khoa duong lam mat qua nhiet.
    req.ventFanForceOn = emergencyActive_ || highTemperatureActive_ ||
        ventTemperatureActive_ || faults_.ventForced() || sensorFaultNeedsFan;
    req.ventFan = req.ventFanForceOn || profileVentActive || scheduledVentActive;

    // "Nhiet ngoai me" da bo khoi HMI (khong con cach nao nguoi dung tat lai
    // duoc neu no da tung bat), nen KHONG con duoc phep giu nhiet chay khi
    // khong co me: DUNG ME phai luon luon tat nhiet ngay, khong ngoai le.
    const bool batchAllowsHeat = batchRunning_;
    const bool fanAllowsHeat = !MANUAL_FAN_CAN_DISABLE_HEATING || fanStable;
    // Mat EEPROM giua me: tiep tuc bang cau hinh RAM; cam me moi/nhiet ngoai me.
    const bool storageAllowsHeat = !batchClearPending_ &&
                                   !safetyJournalFaultLatched_ &&
        !storageFaultLatched_ && (!storageDegraded_ || batchRunning_);

    // RELAY NHIET TONG (contactor): cong tac HEATER vat ly la dieu kien cho
    // phep, KHONG phai quyen vuot qua bao ve. Trong moi nhu cau nhiet, bat ky
    // fault co dropHeatMaster=true (gom High/Emergency, sensor STOP...) deu
    // nha contactor tong. Day la lop du phong doc lap neu SSR D1 bi ket/chay
    // ON; ngoai me/AutoTune thi contactor cung khong duoc treo vo ich.
    const bool heatDemandContext = batchRunning_ || autotune_.running();
    // v3.8.0: cong tac vat ly la yeu cau BAT, khong phai quyen vuot qua
    // bao ve. Mat/loi cam bien hoac fault yeu cau cat tong phai nha contactor.
    const bool normalMasterPermit =
        in.heaterEnable && sensorUsable_ && !faults_.masterDropRequired() &&
        !emergencyActive_ && heatDemandContext;
    // D1 SSR/PID (dong nhiet THAT su) van giu NGUYEN VEN toan bo cac dieu
    // kien an toan nhu truoc gio - chi RIENG relay tong o tren la doi theo
    // yeu cau, khong lam long cac dieu kien cho dong dien nhiet thuc te.
    const bool heaterPidConditions = !faults_.masterDropRequired() &&
        storageAllowsHeat && !abnormalResetLatched_ &&
        batchAllowsHeat && sensorUsable_ && fanAllowsHeat &&
        timeReached(now, heatRestartNotBefore_);
    const bool normalSsrPermit = normalMasterPermit && heaterPidConditions &&
        !faults_.ssrInhibited() && !ventTemperatureActive_;
    const bool actuatorReady = outputs_.heaterReady(now) && mayapBootOperationsReady() &&
        !mayapSystemTripLatched() && !mayapFirmwareMaintenanceActive();

    const float effectiveLimit = updateAdaptiveBalance(now, normalSsrPermit && actuatorReady, fanStable, req.ventFan);
    req.ventFan = req.ventFan || adaptiveCoolingRequested();
    // Adaptive Thermal V1 plan: feed-forward + integral policy + startup hint. It can only
    // propose; the result still passes the startup ceiling, effectiveLimit, scheduler and arbiter.
    ventInfo_.active = outputs_.state().ventFan;
    // The origin of a protective run is latched until the fan is actually off: relay run-on after the
    // request clears must not be compensated as an ordinary vent.
    if (!ventInfo_.active) ventForcedRun_ = false;
    else if (req.ventFanForceOn || adaptiveCoolingRequested()) ventForcedRun_ = true;
    ventInfo_.forced = req.ventFanForceOn || adaptiveCoolingRequested() || ventForcedRun_;
    ventInfo_.startsInSec = ventStartsInSec;
    // A schedule shorter than the vent relay's minimum-on time ends while the fan keeps running:
    // report "ends now" (not "unknown") so the compensation fades instead of snapping back.
    if (ventInfo_.active && ventRemainingSec < 0.0f && !ventInfo_.forced && !profileVentActive &&
        !scheduledVentActive && config_.ventScheduleEnabled && !config_.ventAutoEnabled)
      ventRemainingSec = 0.0f;
    ventInfo_.remainingSec = ventRemainingSec;
    const MayapThermal::Plan &v1plan = thermalV1_.update(now, normalSsrPermit && actuatorReady && config_.controlMode == ControlMode::Pid &&
        MAYAP_ADAPTIVE_OBSERVER_ONLY == 0,
        autotune_.running(), ventInfo_, temperature_, config_.targetTemp, pid_.integral(),
        config_.highTempAlarm);
    publishThermalLearning(now);
    // Smart AutoTune VALIDATING: the normal controller runs the CANDIDATE gains under the tune permit (no batch).
    const bool tunePermit = !faults_.ssrInhibited() &&
                            !faults_.masterDropRequired() &&
                            !storageFaultLatched_ && !storageDegraded_ &&
                            !abnormalResetLatched_ && autotune_.running() &&
                            in.autoMode && in.heaterEnable &&
                            sensorUsable_ && fanStable &&
                            !highTemperatureActive_ && !emergencyActive_;
    const bool tuneClosedLoop = autotune_.externalControl() && tunePermit && actuatorReady;
    float commandedPower = 0.0f;
    if (autotune_.running() && !autotune_.externalControl()) {
      startupHeat_.reset();
      commandedPower = autotune_.power();
    } else if ((normalSsrPermit && actuatorReady) || tuneClosedLoop) {
      if (newSensorSample_) {
        MachineConfig actuatorConfig = config_;
        autotune_.applyCandidate(actuatorConfig);   // no-op unless the closed-loop check is running
        const MayapThermal::Assist tuneAssist = autotune_.assist();
        actuatorConfig.maxHeaterPower = static_cast<uint8_t>(effectiveLimit);
        float ceiling = effectiveLimit;
        bool freezePositiveIntegral = false;
        if (actuatorConfig.controlMode == ControlMode::Pid) {
          startupHeat_.setHint(autotune_.externalControl() ? autotune_.startupHint(config_.highTempAlarm, config_.targetTemp) : v1plan.hint);
          const auto decision = startupHeat_.decide(now, config_.targetTemp,
              temperature_, effectiveLimit);
          ceiling = decision.ceiling;
          freezePositiveIntegral = decision.freezePositiveIntegral;
        }
        pidPower_ = pid_.updateOnNewSample(now, config_.targetTemp,
            temperature_, actuatorConfig, true, ceiling, freezePositiveIntegral,
            autotune_.externalControl() ? &tuneAssist : &v1plan.assist);
      }
      if (config_.controlMode == ControlMode::Pid && startupHeat_.sampleStale(now)) {
        pid_.reset();
        pidPower_ = 0.0f;
      }
      commandedPower = std::min(pidPower_, effectiveLimit);
      startupHeat_.requested(commandedPower);
    } else {
      startupHeat_.reset();
      pid_.reset();
      pidPower_ = 0.0f;
    }
    newSensorSample_ = false;

    const bool masterPermit = normalMasterPermit || tunePermit;
    const bool ssrPermit = (normalSsrPermit || tunePermit) && masterPermit;
    req.heatMaster = masterPermit;
    const auto burst = heaterBurst_.update(now, commandedPower,
        ssrPermit && actuatorReady);
    req.heaterSsr = burst.groupA;
    req.immediateMasterDrop = batchClearPending_ ||
                              safetyJournalFaultLatched_ ||
                              faults_.masterDropRequired() ||
                              emergencyActive_ || !sensorUsable_ ||
                              storageFaultLatched_ ||
                              (storageDegraded_ && !batchRunning_) ||
                              abnormalResetLatched_;

    req.turnLeft = turnPhase_ == TurnPhase::MovingLeft;
    req.turnRight = turnPhase_ == TurnPhase::MovingRight;
    // F-06: coi cung reo cho me qua han chua xac nhan, dung chung co che
    // tam tat (sirenMutedUntil_) voi Nhiet do khan cap - AlarmAck se tu lam
    // moi lai dinh ky dung nhu duoc yeu cau ("keu dinh ky neu bi tat tam").
    // Tu kiem tra coi (sirenSelfTestActive_) la 1 tieng bip NGAN, KHONG lien
    // quan co che tam tat/ACK cua canh bao that (updateSirenSelfTest() da tu
    // bo qua neu dang co canh bao that de khong bao gio trung voi nhanh nay).
    req.siren = ((emergencyActive_ || batchOverdueSirenActive_) &&
                 timeReached(now, sirenMutedUntil_)) || sirenSelfTestActive_;

    // Tao am chi hoat dong trong me, khi cam bien hop le. Hysteresis 2%%RH:
    // dang OFF thi chi bat khi <= SV-2; dang ON thi giu den khi dat SV.
    // Moi nhanh safe/fault cua OutputArbiter van co quyen ep relay OFF ngay.
    const bool humidifierPermit = config_.humidifierInstalled &&
                                  config_.humidifierEnabled && batchRunning_ &&
                                  sensorUsable_ && !faults_.masterDropRequired();
    if (humidifierPermit) {
      if (outputs_.state().humidifier) {
        req.humidifier = humidity_ < config_.targetHumidity;
      } else {
        req.humidifier = humidity_ <=
            (config_.targetHumidity - config_.humidifierHysteresisRh);
      }
    }

    if (mayapFirmwareMaintenanceActive()) {
      heaterBurst_.reset();
      req.heaterSsr = req.heatMaster = req.turnLeft = req.turnRight = false;
      req.immediateMasterDrop = true;
      pid_.reset();
      commandedPower = 0.0f;
    }
    outputs_.update(now, req);
    runtime_.heaterPower = commandedPower;
  }

  // ----------------------------- Che do thu nghiem ----------------------------
  // Trang rieng tren HMI de nguoi lap dat bat/tat tung ngo ra va kiem tra cong
  // tac hanh trinh doc lap, khong dinh gi den logic AUTO/PID. Bi khoa hoan
  // toan khi dang co me/resume/auto-tune de khong the vo tinh dieu khien
  // thiet bi that trong luc dang ap trung.
  bool enterTestMode(uint32_t now, const char *&message) {
    if (!mayapBootOperationsReady()) { message = "DANG KHOI DONG"; return false; }
    if (mayapFirmwareMaintenanceActive()) { message = "DANG CAP NHAT FIRMWARE"; return false; }
    if (batchRunning_ || resumePending_) {
      message = "DANG CO ME - KHONG TEST DUOC"; return false;
    }
    if (autotune_.running()) { message = "AUTO TUNE DANG CHAY"; return false; }
    if (batchClearPending_) { message = "DANG XOA DU LIEU ME CU"; return false; }
    if (emergencyActive_) { message = "DANG QUA NHIET KHAN CAP"; return false; }
    testModeActive_ = true;
    testOutputMaskActive_ = 0U;
    for (uint32_t &t : testOutputPulseUntil_) t = 0U;
    testHeaterStartedAt_ = 0U;
    testHeaterPostCoolUntil_ = 0U;
    testTurnStartedAt_ = 0U;
    testTurnOrigin_ = TrayPosition::Unknown;
    testTurnDirection_ = TestOutputId::Count;
    testLimitPhase_ = TestLimitPhase::Idle;
    testLimitDeadline_ = 0U;
    testLimitBuzzUntil_ = 0U;
    testModeLastActivityAt_ = now;
    pid_.reset();
    message = "DA VAO CHE DO TEST";
    mayapSerialPrintf(false, "[TEST] ENTER\n");
    return true;
  }

  void exitTestMode(uint32_t now) {
    if (!testModeActive_) return;
    testModeActive_ = false;
    testOutputMaskActive_ = 0U;
    for (uint32_t &t : testOutputPulseUntil_) t = 0U;
    testHeaterStartedAt_ = 0U;
    testHeaterPostCoolUntil_ = 0U;
    testTurnStartedAt_ = 0U;
    testTurnOrigin_ = TrayPosition::Unknown;
    testTurnDirection_ = TestOutputId::Count;
    testLimitPhase_ = TestLimitPhase::Idle;
    testLimitDeadline_ = 0U;
    testLimitBuzzUntil_ = 0U;
    outputs_.forceSafe(now);
    mayapSerialPrintf(false, "[TEST] EXIT\n");
  }

  bool testOutputPulse(uint32_t now, TestOutputId id, const char *&message) {
    if (!testModeActive_) { message = "CHUA VAO CHE DO TEST"; return false; }
    const uint8_t idx = static_cast<uint8_t>(id);
    if (idx >= static_cast<uint8_t>(TestOutputId::Count)) {
      message = "THIET BI KHONG HOP LE"; return false;
    }
    const InputState &in = inputs_.state();
    if ((id == TestOutputId::TurnLeft || id == TestOutputId::TurnRight) &&
        in.limitLeft && in.limitRight) {
      message = "HAI HANH TRINH CUNG ON"; return false;
    }
    if (id == TestOutputId::TurnLeft && in.limitLeft) {
      message = "DA O HANH TRINH TRAI"; return false;
    }
    if (id == TestOutputId::TurnRight && in.limitRight) {
      message = "DA O HANH TRINH PHAI"; return false;
    }
    if (id == TestOutputId::HeaterSsr) {
      const bool heaterSafe = sensorUsable_ && !faults_.masterDropRequired() &&
          !faults_.ssrInhibited() && !emergencyActive_ && !highTemperatureActive_;
      if (!heaterSafe) { message = "NHIET CHUA AN TOAN DE TEST"; return false; }
      testHeaterStartedAt_ = now;
      testHeaterPostCoolUntil_ = 0U;
      testOutputPulseUntil_[idx] = now + TEST_HEATER_FAN_PRESTART_MS +
                                   TEST_HEATER_HOLD_MAX_MS;
      testModeLastActivityAt_ = now;
      message = "QUAT CHAY TRUOC - CHO 5 GIAY";
      return true;
    }
    // Dao trai/phai loai tru nhau; OutputArbiter van giu dead-time 500 ms.
    if (id == TestOutputId::TurnLeft || id == TestOutputId::TurnRight) {
      const TestOutputId opposite = id == TestOutputId::TurnLeft
          ? TestOutputId::TurnRight : TestOutputId::TurnLeft;
      testOutputPulseUntil_[static_cast<uint8_t>(opposite)] = 0U;
      testTurnStartedAt_ = now;
      if (in.limitLeft) testTurnOrigin_ = TrayPosition::Left;
      else if (in.limitRight) testTurnOrigin_ = TrayPosition::Right;
      else testTurnOrigin_ = TrayPosition::Unknown;
      testTurnDirection_ = id;
    }
    const uint32_t hold = id == TestOutputId::VentFan
        ? TEST_VENT_CONFIRM_HOLD_MAX_MS : TEST_OUTPUT_HOLD_MAX_MS;
    testOutputPulseUntil_[idx] = now + hold;
    testModeLastActivityAt_ = now;
    message = "DANG BAT THIET BI";
    return true;
  }

  void testOutputStop(uint32_t now, TestOutputId id) {
    const uint8_t idx = static_cast<uint8_t>(id);
    if (idx >= static_cast<uint8_t>(TestOutputId::Count)) return;
    testOutputPulseUntil_[idx] = 0U;
    if (id == TestOutputId::HeaterSsr) {
      testHeaterStartedAt_ = 0U;
      testHeaterPostCoolUntil_ = now + POST_COOL_MS;
    }
    if (id == TestOutputId::TurnLeft || id == TestOutputId::TurnRight) {
      testTurnStartedAt_ = 0U;
      testTurnOrigin_ = TrayPosition::Unknown;
      testTurnDirection_ = TestOutputId::Count;
    }
    testModeLastActivityAt_ = now;
  }

  bool testLimitStart(uint32_t now, TestLimitId target, const char *&message) {
    if (!testModeActive_) { message = "CHUA VAO CHE DO TEST"; return false; }
    testLimitTarget_ = target;
    testLimitPhase_ = TestLimitPhase::Waiting;
    testLimitDeadline_ = now + TEST_LIMIT_TIMEOUT_MS;
    testLimitBuzzUntil_ = 0U;
    testModeLastActivityAt_ = now;
    message = "HAY TAC DONG CONG TAC HANH TRINH";
    return true;
  }

  void testLimitCancel(uint32_t now) {
    testLimitPhase_ = TestLimitPhase::Idle;
    testLimitDeadline_ = 0U;
    testLimitBuzzUntil_ = 0U;
    testModeLastActivityAt_ = now;
  }

  void updateTestMode(uint32_t now) {
    if (!testModeActive_) return;
    const InputState &in = inputs_.state();
    if (testLimitPhase_ == TestLimitPhase::Waiting) {
      const bool hit = testLimitTarget_ == TestLimitId::Left ? in.limitLeft : in.limitRight;
      if (hit) {
        testLimitPhase_ = TestLimitPhase::Success;
        testLimitBuzzUntil_ = now + TEST_LIMIT_CONFIRM_BUZZ_MS;
        testModeLastActivityAt_ = now;
        // Xac nhan co khi (xem latchTurnFault()): can CA 2 CTHT deu tung
        // Success trong Test Mode moi duoc mo khoa dao lai.
        if (turnMechanicalCheckRequired_) {
          if (testLimitTarget_ == TestLimitId::Left) testLimitVerifiedLeft_ = true;
          else testLimitVerifiedRight_ = true;
          if (testLimitVerifiedLeft_ && testLimitVerifiedRight_) {
            turnMechanicalCheckRequired_ = false;
            turnFaultStreak_ = 0U;
            // F-07: xoa khoa NGAY trong NVS luc thuc su go khoa - day la noi
            // DUY NHAT duoc phep clear (yeu cau da xac nhan ca 2 CTHT trong
            // Test Mode), khong duoc de sot lai trong NVS.
            (void)safetyJournal_.setTurnMechanicalCheckRequired(false);
            (void)safetyJournal_.setTurnFaultStreak(0U);
            mayapSerialPrintf(false,
                "[TURN] MECHANICAL CHECK OK - da xac nhan ca 2 CTHT\n");
          }
        }
      } else if (timeReached(now, testLimitDeadline_)) {
        testLimitPhase_ = TestLimitPhase::Timeout;
      }
    } else if (testLimitPhase_ == TestLimitPhase::Success &&
               timeReached(now, testLimitBuzzUntil_)) {
      testLimitPhase_ = TestLimitPhase::Idle;
    }

    // SSR test: het 3 phut nhiet that (sau 5 s prestart) hoac safety mat thi
    // cat heater ngay. Ghi lai moc activity de HMI con du thoi gian hoi 2
    // cau xac nhan, khong bi idle-timeout da ra khoi Test Mode ngay lap tuc.
    uint32_t &heaterDeadline = testOutputPulseUntil_[static_cast<uint8_t>(TestOutputId::HeaterSsr)];
    if (heaterDeadline != 0U) {
      const bool heaterSafe = sensorUsable_ && !faults_.masterDropRequired() &&
          !faults_.ssrInhibited() && !emergencyActive_ && !highTemperatureActive_;
      if (!heaterSafe || timeReached(now, heaterDeadline)) {
        heaterDeadline = 0U;
        testHeaterStartedAt_ = 0U;
        testHeaterPostCoolUntil_ = now + POST_COOL_MS;
        testModeLastActivityAt_ = now;
      }
    }

    // Test motor cung ton trong CTHT nhu van hanh that, khong con la cap relay
    // tho. Cham dich -> OFF ngay; hai CTHT cung ON / CTHT goc khong nha ->
    // OFF va latch loi co khi. OutputArbiter van dam bao dead-time doi chieu.
    const bool testLeft = testOutputPulseUntil_[static_cast<uint8_t>(TestOutputId::TurnLeft)] != 0U &&
        !timeReached(now, testOutputPulseUntil_[static_cast<uint8_t>(TestOutputId::TurnLeft)]);
    const bool testRight = testOutputPulseUntil_[static_cast<uint8_t>(TestOutputId::TurnRight)] != 0U &&
        !timeReached(now, testOutputPulseUntil_[static_cast<uint8_t>(TestOutputId::TurnRight)]);
    if (testLeft || testRight) {
      bool stopTestTurn = false;
      if (in.limitLeft && in.limitRight) {
        stopTestTurn = true;
        latchTurnFault(FaultCode::TurnLimitConflict, "TEST: HAI HANH TRINH CUNG ON");
      } else if ((testLeft && in.limitLeft) || (testRight && in.limitRight)) {
        stopTestTurn = true; // den dung CTHT dich: ket thuc binh thuong
      } else {
        const bool originStillActive =
            (testLeft && testTurnOrigin_ == TrayPosition::Right && in.limitRight) ||
            (testRight && testTurnOrigin_ == TrayPosition::Left && in.limitLeft);
        if (originStillActive && testTurnStartedAt_ != 0U &&
            elapsedMs(now, testTurnStartedAt_) >= TURN_LIMIT_RELEASE_TIMEOUT_MS) {
          stopTestTurn = true;
          latchTurnFault(FaultCode::TurnLimitStuck, "TEST: HANH TRINH KHONG NHA");
        }
      }
      if (stopTestTurn) {
        testOutputPulseUntil_[static_cast<uint8_t>(TestOutputId::TurnLeft)] = 0U;
        testOutputPulseUntil_[static_cast<uint8_t>(TestOutputId::TurnRight)] = 0U;
        testTurnStartedAt_ = 0U;
        testTurnOrigin_ = TrayPosition::Unknown;
        testTurnDirection_ = TestOutputId::Count;
        testModeLastActivityAt_ = now;
      }
    }
    // Quen thoat trang thu nghiem: tu dong ve trang thai an toan sau mot thoi
    // gian khong thao tac, tranh de may "mo cua" cho lenh tay vo thoi han.
    const bool longHeaterTestActive = heaterDeadline != 0U && !timeReached(now, heaterDeadline);
    if (!longHeaterTestActive &&
        elapsedMs(now, testModeLastActivityAt_) >= TEST_MODE_IDLE_EXIT_MS) {
      exitTestMode(now);
    }
  }

  void updateTestModeOutputs(uint32_t now) {
    const auto pulseOn = [&](TestOutputId id) {
      const uint32_t deadline = testOutputPulseUntil_[static_cast<uint8_t>(id)];
      return deadline != 0U && !timeReached(now, deadline);
    };
    const InputState &in = inputs_.state();
    const bool heaterTestSafe = sensorUsable_ && !faults_.masterDropRequired() &&
        !faults_.ssrInhibited() && !emergencyActive_ && !highTemperatureActive_;
    const bool heaterRequested = pulseOn(TestOutputId::HeaterSsr);
    const bool heaterPrestartDone = heaterRequested && testHeaterStartedAt_ != 0U &&
        elapsedMs(now, testHeaterStartedAt_) >= TEST_HEATER_FAN_PRESTART_MS;
    const bool heaterPostCool = testHeaterPostCoolUntil_ != 0U &&
        !timeReached(now, testHeaterPostCoolUntil_);

    OutputRequest req{};
    req.heaterSsr = heaterPrestartDone && heaterTestSafe;
    req.heatMaster = (req.heaterSsr || pulseOn(TestOutputId::HeatMaster)) && heaterTestSafe;
    // Test SSR luon ep quat tuan hoan tu TRUOC khi cap nhiet va giu them
    // POST_COOL_MS sau khi dung; fault safety van co quyen ep 2 quat rieng.
    req.circulationFan = pulseOn(TestOutputId::CirculationFan) || heaterRequested ||
                         heaterPostCool || faults_.circulationForced();
    req.ventFan = pulseOn(TestOutputId::VentFan) || faults_.ventForced();
    req.ventFanForceOn = req.ventFan;
    req.ventFanBypassTiming = true;
    req.light = pulseOn(TestOutputId::Light);
    req.siren = pulseOn(TestOutputId::Siren);
    // CTHT la interlock cuoi cung ngay tai output request, ngoai logic stop/
    // latch o updateTestMode(): du 1 chu ky state chua kip clear cung khong
    // the tiep tuc day motor vao dung dau hanh trinh.
    req.turnLeft = pulseOn(TestOutputId::TurnLeft) && !in.limitLeft && !in.limitRight;
    req.turnRight = pulseOn(TestOutputId::TurnRight) && !in.limitRight && !in.limitLeft;
    req.immediateMasterDrop = !heaterTestSafe || faults_.masterDropRequired() || !sensorUsable_;

    outputs_.update(now, req);
    runtime_.heaterPower = req.heaterSsr ? 100.0f : 0.0f;

    uint8_t mask = 0U;
    for (uint8_t i = 0; i < static_cast<uint8_t>(TestOutputId::Count); ++i) {
      if (pulseOn(static_cast<TestOutputId>(i))) mask |= static_cast<uint8_t>(1U << i);
    }
    testOutputMaskActive_ = mask;
  }

  // ----------------------------- Batch ---------------------------------------
  // F-06 (audit truoc phat hanh v3.7.1): me qua so ngay ap du kien truoc day
  // chi co canh bao Warning tai cho (BatchOverdue, van giu nguyen o duoi),
  // khong co gi doi hoi hanh dong - may co the giu am vo thoi han neu bi bo
  // quen. Ham nay THEM MOT LOP nua: bat coi (dung chung sirenMutedUntil_/
  // AlarmAck - xem updateHeatingAndOutputs()) tu luc qua han, va TU DONG
  // DUNG ME neu qua BATCH_OVERDUE_AUTO_STOP_GRACE_SEC (12h) ke tu luc do ma
  // khong ai bam "Tiep tuc u am" (HmiCommandType::BatchOverdueContinue) hay
  // "Ket thuc me" (BatchStop, van dung binh thuong nhu moi luc). Dung
  // elapsedBatchSec() (persisted, da qua kiem chung o F-15) thay vi RTC epoch
  // de khong phu thuoc RTC con song hay khong.
  void updateBatchOverdue(uint32_t now) {
    if (!batchRunning_ || batchOverdueAcknowledged_) {
      faults_.set(FaultCode::BatchOverdueConfirmationPending, false, now);
      batchOverdueSirenActive_ = false;
      return;
    }
    const uint32_t totalSec = elapsedBatchSec(now);
    const uint32_t dueSec = static_cast<uint32_t>(config_.totalIncubationDays) * 86400UL;
    if (totalSec < dueSec) {
      faults_.set(FaultCode::BatchOverdueConfirmationPending, false, now);
      batchOverdueSirenActive_ = false;
      return;
    }
    const uint32_t overdueSec = totalSec - dueSec;
    faults_.set(FaultCode::BatchOverdueConfirmationPending, true, now,
                static_cast<int16_t>(std::min<uint32_t>(overdueSec / 60UL, INT16_MAX)));
    batchOverdueSirenActive_ = true;
    if (overdueSec >= BATCH_OVERDUE_AUTO_STOP_GRACE_SEC) {
      mayapSerialPrintf(false,
          "[BATCH] Qua han xac nhan %lu gio khong ai thao tac - TU DONG DUNG ME\n",
          static_cast<unsigned long>(BATCH_OVERDUE_AUTO_STOP_GRACE_SEC / 3600UL));
      const char *stopMsg = nullptr;
      (void)stopBatch(now, stopMsg);
    }
  }

  // Tu kiem tra coi dinh ky (de xuat backlog, khong thuoc audit truoc phat
  // hanh v3.7.1) - opt-in, mac dinh TAT (sirenSelfTestEnabled). Muc dich:
  // phat hien som coi bao (loa/relay coi that) bi hong/dut day ma khong ai
  // biet, tranh den luc that su can canh bao khan cap thi coi khong keu.
  // Chi la 1 tieng "bip" NGAN dinh ky, KHONG dung chung co che voi canh bao
  // that (sirenMutedUntil_/AlarmAck) - khong can ACK, tu dong bo qua va doi
  // sang chu ky sau neu dang co bat ky canh bao/loi that nao dang hoat dong,
  // dang o Test Mode, hoac coi dang duoc dung cho muc dich khac (khan cap/
  // me qua han) - tranh nham lan voi tinh huong that.
  void updateSirenSelfTest(uint32_t now) {
    if (!config_.sirenSelfTestEnabled) {
      sirenSelfTestNextAt_ = 0U;
      sirenSelfTestPulseUntil_ = 0U;
      sirenSelfTestActive_ = false;
      return;
    }
    // Lan dau bat cai dat: doi du 1 chu ky roi moi tu kiem tra lan dau,
    // khong keu ngay luc vua bat (nguoi dung co the dang dung canh may).
    if (sirenSelfTestNextAt_ == 0U) {
      sirenSelfTestNextAt_ = now + SIREN_SELF_TEST_INTERVAL_MS;
    }
    if (sirenSelfTestPulseUntil_ != 0U && timeReached(now, sirenSelfTestPulseUntil_)) {
      sirenSelfTestPulseUntil_ = 0U;
    }
    sirenSelfTestActive_ = sirenSelfTestPulseUntil_ != 0U;
    if (sirenSelfTestActive_ || !timeReached(now, sirenSelfTestNextAt_)) return;
    if (faults_.alarmMask() != AlarmNone || testModeActive_ ||
        emergencyActive_ || batchOverdueSirenActive_) {
      // Dang co viec quan trong hon dung den coi - thu lai vao chu ky sau
      // thay vi chen ngang hoac tro thanh 1 tieng bip lac long giua canh
      // bao that.
      sirenSelfTestNextAt_ = now + SIREN_SELF_TEST_INTERVAL_MS;
      return;
    }
    sirenSelfTestPulseUntil_ = now + SIREN_SELF_TEST_PULSE_MS;
    sirenSelfTestActive_ = true;
    sirenSelfTestNextAt_ = now + SIREN_SELF_TEST_INTERVAL_MS;
    mayapSerialPrintf(false, "[SIREN] Tu kiem tra coi dinh ky - phat 1 tieng bip ngan\n");
    eventLog_.push(now, EventType::System,
                   static_cast<uint16_t>(EventCode::SirenSelfTest));
  }

  // Bao mat dien qua ATtiny13A - protocol v4.
  // Trong me: EEPROM batch state cua Tiny arm bao mat dien nhu cu.
  // Ngoai me: activity arm neu OUTPUT THUC TE cua it nhat mot tai sau ON:
  // DAO trai/phai, QUAT TUAN HOAN, QUAT HUT, CONTACTOR NGUON NHIET (heatMaster).
  // Lay contactor tong vi day la dieu kien cap nguon chinh cho cum SSR; KHONG
  // bam theo xung PID SSR. DEN, coi va relay spare KHONG duoc tinh.
  void updateAttinyLink(uint32_t now) {
    now = millis(); // Storage may have blocked earlier in this control cycle.
    mayapAttinyBusUpdate(now);
    const bool expectedBatch = batchRunning_ || resumePending_;
    const OutputState &physicalOut = outputs_.state();
    const bool rawCriticalActivity = !expectedBatch &&
        (physicalOut.turnLeft || physicalOut.turnRight ||
         physicalOut.circulationFan || physicalOut.ventFan || physicalOut.heatMaster);

    // Arm ON ngay de khong tao cua so mat bao ve. OFF chi chap nhan sau 30 s
    // khong con bat ky tai quan trong nao, de khong ghi EEPROM Tiny lien tuc
    // neu relay/nguoi dung dao dong trang thai ngan.
    if (expectedBatch) {
      attinyActivityDesired_ = false;
      attinyActivityOffSince_ = 0U;
    } else if (rawCriticalActivity) {
      attinyActivityDesired_ = true;
      attinyActivityOffSince_ = 0U;
    } else if (attinyActivityDesired_) {
      if (attinyActivityOffSince_ == 0U) attinyActivityOffSince_ = now;
      if (elapsedMs(now, attinyActivityOffSince_) >= ATTINY_ACTIVITY_OFF_CONFIRM_MS) {
        attinyActivityDesired_ = false;
        attinyActivityOffSince_ = 0U;
      }
    } else {
      attinyActivityOffSince_ = 0U;
    }
    const bool expectedActivity = attinyActivityDesired_;
    const bool desiredSiren = emergencyActive_ && timeReached(now, sirenMutedUntil_);
    attinyBatchSync_.expect(expectedBatch, now);
    attinyActivitySync_.expect(expectedActivity, now);
    const uint8_t batchCommand = expectedBatch ? ATTINY_MSG_BATCH_START : ATTINY_MSG_BATCH_END;
    const uint8_t activityCommand = expectedActivity ? ATTINY_MSG_ACTIVITY_ON : ATTINY_MSG_ACTIVITY_OFF;
    const uint8_t oppositeBatchCommand = expectedBatch ? ATTINY_MSG_BATCH_END : ATTINY_MSG_BATCH_START;
    const uint8_t oppositeActivityCommand = expectedActivity ? ATTINY_MSG_ACTIVITY_OFF : ATTINY_MSG_ACTIVITY_ON;

    uint8_t completedCode = 0U;
    bool completedOk = false;
    const bool haveBusResult = mayapAttinyBusTakeResult(completedCode, completedOk);
    // expect() alone misses an unchanged desired value with an old opposite
    // command in flight. Its reply is a transitional snapshot, not E503.
    // Include a just-completed opposite command: the driver has dequeued it.
    if (mayapAttinyBusCommandPending(oppositeBatchCommand) ||
        (haveBusResult && completedCode == oppositeBatchCommand))
      attinyBatchSync_.reconcile(now);
    if (mayapAttinyBusCommandPending(oppositeActivityCommand) ||
        (haveBusResult && completedCode == oppositeActivityCommand))
      attinyActivitySync_.reconcile(now);
    if (haveBusResult) {
      attinyLinkChecked_ = true;
      if (completedOk) {
        attinyLinkHealthy_ = true;
        if (completedCode == ATTINY_MSG_BATCH_START || completedCode == ATTINY_MSG_BATCH_END) {
          attinyBatchSynced_ = (completedCode == ATTINY_MSG_BATCH_START) == expectedBatch;
          attinyLastResyncAt_ = now;
          if (mayapAttinyBusRequest(ATTINY_MSG_STATUS_QUERY)) attinyLastStatusQueryAt_ = now;
        } else if (completedCode == ATTINY_MSG_ACTIVITY_ON || completedCode == ATTINY_MSG_ACTIVITY_OFF) {
          attinyActivityMirrorOn_ = completedCode == ATTINY_MSG_ACTIVITY_ON;
          attinyActivitySynced_ = (attinyActivityMirrorOn_ == expectedActivity);
        } else if (completedCode == ATTINY_MSG_SIREN_ON) {
          attinySirenMirrorOn_ = true;
          attinyLastSirenAssertAt_ = now;
        } else if (completedCode == ATTINY_MSG_SIREN_OFF) {
          attinySirenMirrorOn_ = false;
        } else if (completedCode == ATTINY_MSG_STATUS_QUERY) {
          attinyStatusAwaiting_ = true;
          attinyStatusDeadline_ = now + ATTINY_STATUS_RESPONSE_TIMEOUT_MS;
          mayapAttinyBusHoldTxUntil(attinyStatusDeadline_);
        }
      } else {
        attinyLinkHealthy_ = false;
        if (completedCode == batchCommand) attinyBatchSync_.commandFailed();
        if (completedCode == activityCommand) attinyActivitySync_.commandFailed();
        if (completedCode == ATTINY_MSG_BATCH_START || completedCode == ATTINY_MSG_BATCH_END) {
          attinyBatchSynced_ = false;
        }
        if (completedCode == ATTINY_MSG_ACTIVITY_ON || completedCode == ATTINY_MSG_ACTIVITY_OFF) {
          // Khong duoc tin mirror neu Tiny khong ACK lenh activity.
          attinyActivityMirrorOn_ = !expectedActivity;
          attinyActivitySynced_ = false;
        }
        if (completedCode == ATTINY_MSG_STATUS_QUERY) {
          attinyStatusAwaiting_ = false;
          mayapAttinyBusHoldTxUntil(now);
        }
      }
    }

    const uint8_t incoming = mayapAttinyBusPollIncoming();
    if (incoming >= ATTINY_MSG_STATUS_BASE && incoming <= ATTINY_MSG_STATUS_MAX) {
      const uint8_t flags = static_cast<uint8_t>(incoming - ATTINY_MSG_STATUS_BASE);
      const bool reported9vLow = (flags & ATTINY_STATUS_FLAG_9V_LOW) != 0U;
      attinyTinyBatch_ = (flags & ATTINY_STATUS_FLAG_BATCH) != 0U;
      attinyTinySirenOn_ = (flags & ATTINY_STATUS_FLAG_SIREN) != 0U;
      attinyTinyActivity_ = (flags & ATTINY_STATUS_FLAG_ACTIVITY) != 0U;
      attinyStatusKnown_ = true;
      attinyLinkChecked_ = true;
      attinyLinkHealthy_ = true;
      attinyStatusAwaiting_ = false;
      attinyLastStatusAt_ = now;
      mayapAttinyBusHoldTxUntil(now);

      if (reported9vLow == attiny9vLow_) {
        attiny9vConfirmPending_ = false;
      } else if (!attiny9vConfirmPending_ || attiny9vCandidate_ != reported9vLow) {
        attiny9vCandidate_ = reported9vLow;
        attiny9vConfirmPending_ = true;
        attiny9vConfirmAt_ = now + ATTINY_9V_CONFIRM_MS;
      } else {
        attiny9vLow_ = reported9vLow;
        attiny9vConfirmPending_ = false;
      }

      attinyBatchSynced_ = (attinyTinyBatch_ == expectedBatch);
      attinyActivitySynced_ = (attinyTinyActivity_ == expectedActivity);
      // One frame contains BOTH fields, but a batch command only confirms
      // batch, and an activity command only confirms activity. During a
      // handoff, reconcile an unexpected counterpart instead of flashing
      // E503 on the HMI before its own corrective command has completed.
      if (attinyBatchSync_.pending() || attinyActivitySync_.pending()) {
        if (!attinyBatchSynced_) attinyBatchSync_.reconcile(now);
        if (!attinyActivitySynced_) attinyActivitySync_.reconcile(now);
      }
      attinyBatchSync_.observe(attinyTinyBatch_,
          completedOk && completedCode == batchCommand &&
          !mayapAttinyBusCommandPending(oppositeBatchCommand), now);
      attinyActivitySync_.observe(attinyTinyActivity_,
          completedOk && completedCode == activityCommand &&
          !mayapAttinyBusCommandPending(oppositeActivityCommand), now);
      // Status Tiny la nguon su that sau reset rieng le; cap nhat mirror de
      // block thay-doi-ben-duoi tu dong gui lai neu co mismatch.
      attinyActivityMirrorOn_ = attinyTinyActivity_;
      mayapSerialPrintf(false,
          "[ATTINY] v=%u link=1 batchSync=%u activitySync=%u expectedBatch=%u activity=%u tinyBatch=%u tinyActivity=%u siren=%u tinySiren=%u 9v=%s\n",
          ATTINY_PROTOCOL_VERSION, attinyBatchSynced_ ? 1U : 0U,
          attinyActivitySynced_ ? 1U : 0U, expectedBatch ? 1U : 0U,
          expectedActivity ? 1U : 0U, attinyTinyBatch_ ? 1U : 0U,
          attinyTinyActivity_ ? 1U : 0U, desiredSiren ? 1U : 0U,
          attinyTinySirenOn_ ? 1U : 0U, reported9vLow ? "LOW" : "OK");
    }

    if (attinyStatusAwaiting_ && timeReached(now, attinyStatusDeadline_)) {
      attinyStatusAwaiting_ = false;
      attinyLinkChecked_ = true;
      attinyLinkHealthy_ = false;
      mayapAttinyBusHoldTxUntil(now);
    }
    // Fail safe even if the low-level bus driver never yields a result.
    if (!attinyLinkChecked_ && elapsedMs(now, bootAt_) >= 10000UL) {
      attinyLinkChecked_ = true;
      attinyLinkHealthy_ = false;
    }
    if (attinyStatusKnown_) attinyBatchSynced_ = (attinyTinyBatch_ == expectedBatch);
    if (attinyStatusKnown_) attinyActivitySynced_ = (attinyTinyActivity_ == expectedActivity);
    attinyBatchSync_.update(now);
    attinyActivitySync_.update(now);

    if (desiredSiren != attinySirenMirrorOn_ ||
        (desiredSiren && elapsedMs(now, attinyLastSirenAssertAt_) >= ATTINY_SIREN_REASSERT_MS)) {
      if (mayapAttinyBusRequest(desiredSiren ? ATTINY_MSG_SIREN_ON : ATTINY_MSG_SIREN_OFF) && desiredSiren) {
        attinyLastSirenAssertAt_ = now;
      }
    }
    if (attinyStatusKnown_ && attinyTinySirenOn_ != desiredSiren) {
      (void)mayapAttinyBusRequest(desiredSiren ? ATTINY_MSG_SIREN_ON : ATTINY_MSG_SIREN_OFF);
    }

    // Activity Tiny da duoc luu EEPROM + tra lai trong STATUS. Vi vay chi
    // gui khi trang thai THUC SU doi hoac status cho thay mismatch; KHONG con
    // reassert 5 s, giup Tiny ngu gan nhu toan bo thoi gian.
    if (expectedActivity != attinyActivityMirrorOn_) {
      (void)mayapAttinyBusRequest(
          expectedActivity ? ATTINY_MSG_ACTIVITY_ON : ATTINY_MSG_ACTIVITY_OFF);
    }

    // Preserve siren/activity priority above; pending transitions must have
    // their latest desired command queued. Rapid START/STOP/START can leave an
    // opposite command behind an active matching command; enqueue the final
    // desired state after that old command, without extending the deadline.
    if (attinyBatchSync_.pending()) (void)mayapAttinyBusRequest(batchCommand);
    if (attinyActivitySync_.pending()) (void)mayapAttinyBusRequest(activityCommand);

    if (attinyStartupProbePending_) {
      if (mayapAttinyBusRequest(ATTINY_MSG_STATUS_QUERY)) {
        attinyStartupProbePending_ = false;
        attinyLastStatusQueryAt_ = now;
      }
    } else if (attiny9vConfirmPending_ && timeReached(now, attiny9vConfirmAt_)) {
      if (mayapAttinyBusRequest(ATTINY_MSG_STATUS_QUERY)) {
        attinyLastStatusQueryAt_ = now;
        attiny9vConfirmAt_ = now + ATTINY_9V_CONFIRM_MS;
      }
    } else if (elapsedMs(now, attinyLastStatusQueryAt_) >=
               ((expectedBatch || expectedActivity) ? ATTINY_STATUS_ARMED_INTERVAL_MS
                                                    : ATTINY_STATUS_IDLE_INTERVAL_MS)) {
      if (mayapAttinyBusRequest(ATTINY_MSG_STATUS_QUERY)) attinyLastStatusQueryAt_ = now;
    } else if (attinyLinkChecked_ && !attinyLinkHealthy_ &&
               elapsedMs(now, attinyLastStatusQueryAt_) >= ATTINY_RESYNC_RETRY_MS) {
      if (mayapAttinyBusRequest(ATTINY_MSG_STATUS_QUERY)) attinyLastStatusQueryAt_ = now;
    }

    if (attinyStatusKnown_ && !attinyBatchSynced_ &&
        elapsedMs(now, attinyLastResyncAt_) >= ATTINY_RESYNC_RETRY_MS) {
      if (mayapAttinyBusRequest(expectedBatch ? ATTINY_MSG_BATCH_START : ATTINY_MSG_BATCH_END)) {
        attinyLastResyncAt_ = now;
      }
    }

    faults_.set(FaultCode::AttinyBusUnresponsive,
                attinyLinkChecked_ && !attinyLinkHealthy_, now);
    faults_.set(FaultCode::SirenBatteryLow,
                attinyStatusKnown_ && attiny9vLow_, now);
    const uint8_t syncFaultMask = (attinyBatchSync_.fault() ? 1U : 0U) |
        (attinyActivitySync_.fault() ? 2U : 0U);
    if (syncFaultMask != attinySyncFaultDetail_) {
      attinySyncFaultDetail_ = syncFaultMask;
      mayapSerialPrintf(false,
          "[ATTINY-SYNC] fault=%u expected=%u/%u reported=%u/%u pending=%u/%u cmd=%u ok=%u\n",
          syncFaultMask, expectedBatch, expectedActivity, attinyTinyBatch_,
          attinyTinyActivity_, attinyBatchSync_.pending(), attinyActivitySync_.pending(),
          completedCode, completedOk);
    }
    faults_.set(FaultCode::AttinyStateUnsynced,
                attinyStatusKnown_ && (attinyBatchSync_.fault() || attinyActivitySync_.fault()),
                now, syncFaultMask);

    runtime_.attinyLinkHealthy = attinyLinkChecked_ && attinyLinkHealthy_;
    runtime_.attinyBatchSynced = attinyStatusKnown_ && attinyBatchSynced_;
    runtime_.attinyStatusKnown = attinyStatusKnown_;
    runtime_.attinySirenBatteryLow = attinyStatusKnown_ && attiny9vLow_;
    runtime_.attinyCriticalActivityArmed = expectedActivity &&
        attinyActivityMirrorOn_ && (!attinyStatusKnown_ || attinyActivitySynced_);
    runtime_.attinyLastStatusAgeSec = attinyStatusKnown_
        ? elapsedMs(now, attinyLastStatusAt_) / 1000UL : UINT32_MAX;
  }

  void updateBatchTime(uint32_t now) {
    if (batchRunning_ && batchPhase_ == BatchPhase::Prestart &&
        elapsedMs(now, phaseStartedAt_) >= FAN_PRESTART_MS) {
      if (needHome_ && inputs_.state().autoMode) {
        batchPhase_ = BatchPhase::Homing;
      } else {
        batchPhase_ = BatchPhase::Running;
        scheduleNextTurnFromAnchor(now);
      }
    }

    if (!batchRunning_) return;
    const uint32_t totalSec = elapsedBatchSec(now);
    const uint32_t dayIndex = totalSec / 86400UL;
    // KHONG gioi han theo totalIncubationDays nua - "NGAY X/Y" voi X > Y la
    // tin hieu quan trong (me qua han), can hien thi dung thay vi ke bang
    // Y mai. Gia tri nay cung la dau vao cho canh bao BatchOverdue cuc bo
    // (updateAlarms()) va nhac "qua han" qua Cloud Push (cloud_alert_link.h) -
    // truoc day bi ke bang totalIncubationDays nen dieu kien "qua han" cua
    // ca 2 noi khong bao gio dung duoc (loi da phat hien khi vien).
    runtime_.currentDay = static_cast<uint8_t>(std::min<uint32_t>(255U, dayIndex + 1U));
    const uint32_t dayNumber = dayIndex + 1U;
    if (lastTurnCounterDay_ == 0U) lastTurnCounterDay_ = dayNumber;
    if (dayNumber != lastTurnCounterDay_) {
      lastTurnCounterDay_ = dayNumber;
      turnCountToday_ = 0;
      (void)saveBatchRecord();
    }
  }

  uint32_t elapsedBatchSec(uint32_t now) const {
    if (!batchRunning_) return elapsedBeforeStartSec_;
    return elapsedBeforeStartSec_ + elapsedMs(now, batchStartedAt_) / 1000UL;
  }
  uint32_t elapsedBatchMs(uint32_t now) const {
    const uint64_t ms = static_cast<uint64_t>(elapsedBeforeStartSec_) * 1000ULL +
                        (batchRunning_ ? elapsedMs(now, batchStartedAt_) : 0U);
    return ms > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(ms);
  }

  void serviceBatchLog(uint32_t now) {
    if (!batchRunning_) return;
    if (!batchLogger_.active() || !batchLogger_.healthy()) {
      batchLogFaultActive_ = true;
      return;
    }

    // Xa toi da 2 record moi chu ky control; event push chi enqueue RAM.
    if (!batchLogger_.service(2U)) {
      batchLogFaultActive_ = true;
      return;
    }
    if (!batchLogGate_.due(now, true)) return;

    uint8_t flags = 0U;
    if (inputs_.state().autoMode) flags |= 1U << 0U;
    if (outputs_.state().heaterSsr) flags |= 1U << 1U;
    if (outputs_.state().circulationFan) flags |= 1U << 2U;
    if (outputs_.state().ventFan) flags |= 1U << 3U;
    if (turningLockdownActive(now)) flags |= 1U << 4U;
    if (turnPhase_ == TurnPhase::MovingLeft ||
        turnPhase_ == TurnPhase::MovingRight) flags |= 1U << 5U;

    const bool ok = batchLogger_.appendSample(
        rtc_.epoch(), ++batchLogSampleSequence_, temperature_, humidity_,
        runtime_.heaterPower, static_cast<uint16_t>(runtime_.stateCode), flags);
    if (!ok) batchLogFaultActive_ = true;
  }
  void checkpointBatch() {
    if (!batchRunning_ && !resumePending_) return;
    (void)saveBatchRecord();
  }
  bool saveBatchRecord() {
    PackedBatchV1 p{};
    p.wasRunning = (batchRunning_ || resumePending_) ? 1U : 0U;
    p.nextDirection = static_cast<uint8_t>(config_.nextDirection);
    p.turnCountToday = turnCountToday_;
    p.turnCountBatch = turnCountBatch_;
    p.elapsedSec = elapsedBatchSec(millis());
    p.checkpointEpoch = rtc_.epoch();
    p.batchStartEpoch = batchStartEpoch_;
    p.lastTurnEpoch = lastTurnEpoch_;
    const bool ok = store_.saveBatch(p);
    if (!ok) {
      latchStorageFault("BATCH SAVE");
    } else {
      clearStorageDegraded(millis());
    }
    if (ok && p.checkpointEpoch != 0U) {
      savedElapsedAtCheckpoint_ = p.elapsedSec;
      lastCheckpointEpoch_ = p.checkpointEpoch;
      resumeClockAdjusted_ = true;
    }
    return ok;
  }
  bool clearBatchRecord() {
    PackedBatchV1 p{};
    const bool ok = store_.saveBatch(p);
    if (!ok) {
      latchStorageFault("BATCH CLEAR");
    } else {
      clearStorageDegraded(millis());
      savedElapsedAtCheckpoint_ = 0U;
      lastCheckpointEpoch_ = 0U;
      resumeClockAdjusted_ = true;
    }
    return ok;
  }

  bool tryCommitBatchClear(uint32_t now) {
    if (!batchClearPending_) return true;
    if (batchClearAttemptCount_ < UINT8_MAX) ++batchClearAttemptCount_;

    if (!store_.ready() || !clearBatchRecord()) {
      batchClearRetryAt_ = now + BATCH_CLEAR_RETRY_MS;
      faults_.set(FaultCode::BatchStateClearPending, true, now,
                  static_cast<int16_t>(batchClearAttemptCount_));
      return false;
    }

    batchClearPending_ = false;
    batchClearAttemptCount_ = 0U;
    batchClearRetryAt_ = 0U;
    if (!safetyJournal_.clearStopIntent()) {
      safetyJournalFaultLatched_ = true;
      faults_.set(FaultCode::SafetyJournalUnavailable, true, now);
    }
    (void)faults_.clearRecovered(FaultCode::BatchStateClearPending, now);
    mayapSerialPrintf(false, "[BATCH] EEPROM STOP RECORD VERIFIED\n");
    return true;
  }

  void serviceBatchClear(uint32_t now) {
    if (!batchClearPending_) return;
    faults_.set(FaultCode::BatchStateClearPending, true, now,
                static_cast<int16_t>(batchClearAttemptCount_));
    if (!timeReached(now, batchClearRetryAt_)) return;
    if (!store_.ready()) {
      batchClearRetryAt_ = now + BATCH_CLEAR_RETRY_MS;
      return;
    }
    (void)tryCommitBatchClear(now);
  }
  void latchStorageFault(const char *where) {
    const uint32_t now = millis();
    if (storageFailureStreak_ < UINT8_MAX) ++storageFailureStreak_;
    const bool keepRunningFromRam = batchRunning_ || resumePending_;
    const bool hardFailure = !keepRunningFromRam &&
        storageFailureStreak_ >= EEPROM_FAILURE_LATCH_COUNT;

    // Mot giao dich thoang qua khong duoc bien thanh loi vinh vien. Hai lan dau
    // chi danh dau suy giam va cho phep thu lai. Neu me dang chay, tiep tuc dieu
    // khien tu RAM de khong pha me; khong cho sua cau hinh/bat dau me moi.
    if (!hardFailure) {
      if (!storageDegraded_) {
        mayapSerialPrintf(false,
            "[EEPROM] DEGRADED at %s streak=%u%s\n",
            where ? where : "UNKNOWN",
            static_cast<unsigned>(storageFailureStreak_),
            keepRunningFromRam ? " - CONTINUE FROM RAM" : "");
        eventLog_.push(now, EventType::StorageError,
                       static_cast<uint16_t>(EventCode::FaultBase) +
                       static_cast<uint16_t>(FaultCode::StorageDegraded),
                       static_cast<int16_t>(storageFailureStreak_), 1U);
      }
      storageDegraded_ = true;
      faults_.set(FaultCode::StorageDegraded, true, now,
                  static_cast<int16_t>(storageFailureStreak_));
      return;
    }

    if (!storageFaultLatched_) {
      mayapSerialPrintf(false, "[EEPROM] UNAVAILABLE at %s streak=%u\n",
                        where ? where : "UNKNOWN",
                        static_cast<unsigned>(storageFailureStreak_));
      eventLog_.push(now, EventType::StorageError,
                     static_cast<uint16_t>(EventCode::FaultBase) +
                     static_cast<uint16_t>(FaultCode::StorageUnavailable),
                     static_cast<int16_t>(storageFailureStreak_), 2U);
    }
    storageDegraded_ = false;
    faults_.set(FaultCode::StorageDegraded, false, now);
    storageFaultLatched_ = true;
    faults_.set(FaultCode::StorageUnavailable, true, now,
                static_cast<int16_t>(storageFailureStreak_));
    pid_.reset();
  }

  void clearStorageDegraded(uint32_t now) {
    storageFailureStreak_ = 0U;
    if (!storageDegraded_) return;
    storageDegraded_ = false;
    faults_.set(FaultCode::StorageDegraded, false, now);
    mayapSerialPrintf(false, "[EEPROM] CONNECTION RESTORED\n");
  }

  // ----------------------------- Runtime/LED ---------------------------------
  void copyRuntimeToHmi() {
    const uint32_t now = millis();

    runtime_.temperature = temperature_;
    runtime_.humidity = humidity_;
    runtime_.sensorOnline = sensorUsable_;
    runtime_.sensorStartupGrace = !timeReached(now, sensorStartupGraceUntil_) &&
                                  !sensorUsable_;
    runtime_.resumeConfirmationRequired = resumeConfirmationRequired_;
    runtime_.batchOverdueConfirmationPending = batchOverdueSirenActive_;
    runtime_.sirenSelfTestActive = sirenSelfTestActive_;
    runtime_.powerLossRecovery = powerLossRecovery_;
    runtime_.timeValid = rtc_.valid();
    const NetworkStatus &network = lastNetworkStatus_;
    runtime_.connectivityMode = network.requestedMode;
    runtime_.networkState = network.state;
    runtime_.networkConfigured = network.credentialsConfigured;
    runtime_.networkConnected = network.connected;
    runtime_.networkRssiDbm = network.rssiDbm;
    runtime_.testModeActive = testModeActive_;
    runtime_.testOutputMaskActive = testOutputMaskActive_;
    runtime_.testLimitTarget = testLimitTarget_;
    runtime_.testLimitPhase = testLimitPhase_;
    const WifiPortalStatus portal = mayapGetWifiPortalStatus();
    runtime_.wifiPortalState = portal.state;
    snprintf(runtime_.wifiPortalApName, sizeof(runtime_.wifiPortalApName), "%s",
             portal.apName);
    snprintf(runtime_.wifiPortalPassword, sizeof(runtime_.wifiPortalPassword), "%s",
             portal.password);
    if (runtime_.timeValid) {
      snprintf(runtime_.dateText, sizeof(runtime_.dateText), "%s",
               rtc_.dateText());
      snprintf(runtime_.timeText, sizeof(runtime_.timeText), "%s",
               rtc_.timeText());
    } else {
      snprintf(runtime_.timeText, sizeof(runtime_.timeText), "--:--");
    }
    runtime_.batchRunning = batchRunning_ || resumePending_;
    if (resumePending_ && !batchRunning_) {
      const uint32_t dayIndex = elapsedBeforeStartSec_ / 86400UL;
      runtime_.currentDay = static_cast<uint8_t>(std::min<uint32_t>(255U, dayIndex + 1U));
    } else if (!batchRunning_) {
      runtime_.currentDay = 0;
    }
    runtime_.turnCountToday = turnCountToday_;
    runtime_.turnCountBatch = turnCountBatch_;
    runtime_.autoMode = inputs_.state().autoMode;
    runtime_.turningLockdown = turningLockdownActive(now);
    runtime_.batchLogAvailable = false;
    // "OUT" ben canh SSR% tren HMI la trang thai relay NHIET TONG (contactor,
    // heatMaster) - khac voi SSR% (cong suat D1/PID). Tu khi relay tong bam
    // theo cong tac vat ly (khong con phu thuoc me/PID), heaterSsr co the OFF
    // trong khi relay tong dang BAT (vd: cong tac bat nhung chua co me) - phai
    // doc dung heatMaster, khong doc heaterSsr, de "OUT" khop voi relay that.
    runtime_.heaterOn = outputs_.state().heatMaster;
    runtime_.circulationFanOn = outputs_.state().circulationFan;
    runtime_.ventFanOn = outputs_.state().ventFan;
    runtime_.humidifierOn = outputs_.state().humidifier;
    runtime_.lightOn = outputs_.state().light;
    runtime_.sirenOn = outputs_.state().siren;
    runtime_.autoTuneState = autotune_.state();
    runtime_.autoTuneProgress = autotune_.progress();
    runtime_.primaryFaultCode = static_cast<uint16_t>(faults_.primary());
    runtime_.activeFaultCount = faults_.activeCount();
    runtime_.activeFaultDisplayCount = faults_.copyActiveForHmi(
        runtime_.activeFaults, HMI_FAULT_DISPLAY_CAPACITY);
    runtime_.faultNotificationSequence = faults_.notificationSequence();
    runtime_.lastRaisedFaultCode = static_cast<uint16_t>(faults_.lastRaisedCode());
    runtime_.lastRaisedFaultSeverity = static_cast<uint8_t>(faults_.lastRaisedSeverity());
    runtime_.eventSequence = eventLog_.sequence();
    runtime_.relayTransitionsHour = outputs_.transitionsThisHour();
    runtime_.alarmMask = faults_.alarmMask();

    if (turnFaultLatched_) runtime_.turnState = TurnState::Fault;
    else if (turnPhase_ == TurnPhase::MovingLeft) runtime_.turnState = TurnState::Left;
    else if (turnPhase_ == TurnPhase::MovingRight) runtime_.turnState = TurnState::Right;
    else if (batchRunning_ && inputs_.state().autoMode &&
             config_.turningEnabled && !runtime_.turningLockdown)
      runtime_.turnState = TurnState::Waiting;
    else runtime_.turnState = TurnState::Stopped;
    // Dong co dang chay THUC SU vi tim goc (khong phai dao dinh ky binh
    // thuong) - xem ghi chu tai MachineRuntime::turnHoming trong config.h.
    runtime_.turnHoming = moveIsHoming_ &&
        (turnPhase_ == TurnPhase::MovingLeft || turnPhase_ == TurnPhase::MovingRight);

    runtime_.nextTurnScheduled = batchRunning_ && config_.turningEnabled &&
                                 !runtime_.turningLockdown && nextTurnAt_ != 0U;
    if (runtime_.nextTurnScheduled && !timeReached(now, nextTurnAt_)) {
      runtime_.nextTurnMinutes = static_cast<uint16_t>(
          std::min<uint32_t>(65535U,
              (nextTurnAt_ - now + 59999U) / 60000U));
    } else runtime_.nextTurnMinutes = 0;

    const InputState &in = inputs_.state();
    const char *state = "SAN SANG";
    MachineStateCode stateCode = MachineStateCode::Boot;
    if (emergencyActive_) {
      state = "QUA NHIET CAP 3"; stateCode = MachineStateCode::Emergency;
    } else if (testModeActive_) {
      state = "CHE DO TEST"; stateCode = MachineStateCode::ReadyManual;
    } else if (storageFaultLatched_ || abnormalResetLatched_ ||
               faults_.active(FaultCode::OutputConflict)) {
      state = storageFaultLatched_ ? "LOI BO NHO" :
              abnormalResetLatched_ ? "CHO XN RESET LOI" : "LOI HE THONG";
      stateCode = MachineStateCode::SystemFault;
    } else if (batchClearPending_) {
      state = "CHO XOA DU LIEU"; stateCode = MachineStateCode::SystemFault;
    } else if (runtime_.sensorStartupGrace) {
      state = "KHOI TAO CAM BIEN"; stateCode = MachineStateCode::Boot;
    } else if (!sensorUsable_) {
      state = "MAT CAM BIEN"; stateCode = MachineStateCode::SensorFault;
    } else if (faults_.active(FaultCode::HeaterSwitchOffDuringBatch)) {
      state = "TAT CONG TAC NHIET"; stateCode = MachineStateCode::SystemFault;
    } else if (batchRunning_ && !in.autoMode) {
      // Dao tay van hoat dong binh thuong trong trang thai nay (xem
      // updateTurning()) - nhan chi con mang tinh nhac nho gat lai AUTO.
      state = "AUTO OFF - DAO TAY"; stateCode = MachineStateCode::SystemFault;
    } else if (turnFaultLatched_) {
      state = "LOI DAO"; stateCode = MachineStateCode::TurningFault;
    } else if (autotune_.running()) {
      state = "DANG AUTO TUNE"; stateCode = MachineStateCode::AutoTune;
    } else if (resumePending_) {
      switch (resumeBlockReason_) {
        case ResumeBlockReason::Confirmation: state = "CHO XAC NHAN"; break;
        case ResumeBlockReason::BatchClear: state = "CHO XOA DU LIEU"; break;
        case ResumeBlockReason::Storage: state = "CHO BO NHO"; break;
        case ResumeBlockReason::ResetAck: state = "CHO XN RESET"; break;
        case ResumeBlockReason::AutoMode: state = "HAY CHUYEN AUTO"; break;
        case ResumeBlockReason::HeaterSwitch: state = "HAY BAT NHIET"; break;
        case ResumeBlockReason::Sensor: state = "CHO CAM BIEN"; break;
        case ResumeBlockReason::Rtc: state = "CHO RTC HOP LE"; break;
        case ResumeBlockReason::TurnFault: state = "LOI DAO"; break;
        case ResumeBlockReason::LimitConflict: state = "LOI HANH TRINH"; break;
        case ResumeBlockReason::Temperature: state = "CHO HA NHIET"; break;
        case ResumeBlockReason::Delay: state = "CHO TRE PHUC HOI"; break;
        case ResumeBlockReason::TurningDisabled: state = "HAY BAT TU DONG DAO"; break;
        default: state = "CHO PHUC HOI"; break;
      }
      stateCode = MachineStateCode::ResumeWait;
    } else if (batchRunning_ &&
               (batchPhase_ == BatchPhase::Homing || runtime_.turnHoming)) {
      // runtime_.turnHoming bat luon truong hop tim goc GIUA me (needHome_,
      // vd sau khi mat moc vi tri) - khong chi luc khoi dong me
      // (batchPhase_==Homing), de man hinh chinh luon phan anh dung dong co
      // dang thuc su lam gi.
      state = "DANG TIM GOC"; stateCode = MachineStateCode::Homing;
    } else if (batchRunning_ && batchPhase_ != BatchPhase::Running) {
      state = "KHOI DONG ME"; stateCode = MachineStateCode::Prestart;
    } else if (batchRunning_) {
      state = runtime_.turningLockdown ? "DANG AP - KHOA DAO" : "DANG AP AUTO";
      stateCode = MachineStateCode::RunningAuto;
    } else {
      state = in.autoMode ? "SAN SANG AUTO" : "SAN SANG MANUAL";
      stateCode = in.autoMode ? MachineStateCode::ReadyAuto
                              : MachineStateCode::ReadyManual;
    }
    runtime_.stateCode = stateCode;
    snprintf(runtime_.machineState, sizeof(runtime_.machineState), "%s", state);
    hmiSetRuntime(runtime_);
    mayapRealtimeSetRuntime(runtime_);
    mayapCloudSetRuntime(runtime_);
    if (lastHmiEventSequence_ != eventLog_.sequence() ||
        elapsedMs(now, lastHmiEventPushAt_) >= 30000UL) {
      HmiEventSnapshot snapshot{};
      eventLog_.snapshotRecent(now, snapshot);
      hmiSetEventLog(snapshot);
      mayapRealtimePushEventLog(snapshot);
      lastHmiEventSequence_ = eventLog_.sequence();
      lastHmiEventPushAt_ = now;
    }
  }

  void updateLed(uint32_t now) {
    LedCode code = LedCode::Boot;
    const InputState &in = inputs_.state();
    if (emergencyActive_) code = LedCode::Emergency;
    else if (storageFaultLatched_ || safetyJournalFaultLatched_ ||
             abnormalResetLatched_ || batchClearPending_ ||
             faults_.active(FaultCode::HeaterSwitchOffDuringBatch) ||
             (batchRunning_ && !in.autoMode))
      code = LedCode::SystemFault;
    else if (!sensorUsable_ && !timeReached(now, sensorStartupGraceUntil_)) code = LedCode::Boot;
    else if (!sensorUsable_) code = LedCode::SensorFault;
    else if (turnFaultLatched_) code = LedCode::TurnFault;
    else if (highTemperatureActive_ || lowTemperatureActive_) code = LedCode::TempWarning;
    else if (autotune_.running()) code = LedCode::AutoTune;
    else if (batchRunning_) code = LedCode::RunningAuto;
    else code = in.autoMode ? LedCode::ReadyAuto : LedCode::ReadyManual;
    led_.update(now, code);
  }

  // ----------------------------- Serial --------------------------------------
  static void upperAscii(char *text) {
    while (text && *text) {
      if (*text >= 'a' && *text <= 'z') *text = static_cast<char>(*text - 'a' + 'A');
      ++text;
    }
  }
  static bool parseOnOff(const char *text, bool &value) {
    if (!text) return false;
    if (!strcmp(text, "ON") || !strcmp(text, "1")) { value = true; return true; }
    if (!strcmp(text, "OFF") || !strcmp(text, "0")) { value = false; return true; }
    return false;
  }
  void serviceSerial(uint32_t now) {
#if MAYAP_SERIAL_INPUT_SIM || MAYAP_DIAGNOSTIC_SERIAL
    uint8_t budget = 32;
    while (budget-- && Serial.available() > 0) {
      const char c = static_cast<char>(Serial.read());
      if (c == '\r') continue;
      if (c == '\n') {
        if (!serialDiscardUntilNewline_) {
          serialLine_[serialLength_] = '\0';
          handleSerialLine(now, serialLine_);
        }
        serialLength_ = 0;
        serialDiscardUntilNewline_ = false;
      } else if (serialDiscardUntilNewline_) {
        continue;
      } else if (serialLength_ + 1U < sizeof(serialLine_)) {
        serialLine_[serialLength_++] = c;
      } else {
        serialLength_ = 0;
        serialDiscardUntilNewline_ = true;
        mayapSerialPrintf(false, "[SER] DONG QUA DAI\n");
      }
    }
#else
    (void)now;
#endif
  }





  void handleSerialLine(uint32_t now, char *line) {
    if (!line || !*line) return;
    upperAscii(line);
    char *save = nullptr;
    char *cmd = strtok_r(line, " ", &save);
    char *arg1 = strtok_r(nullptr, " ", &save);
    char *arg2 = strtok_r(nullptr, " ", &save);
    char *arg3 = strtok_r(nullptr, " ", &save);

    if (cmd && !strcmp(cmd, "SERIAL")) {
      const bool enabled = !mayapSerialDebugEnabled();
      mayapSetSerialDebugEnabled(enabled);
      mayapSerialPrintf(true, "[SERIAL] %s\n", enabled ? "ON" : "OFF");
      if (enabled) {
        printSerialHelp();
        printStatus(now);
      }
      return;
    }
#if MAYAP_SERIAL_INPUT_SIM
    if (cmd && !strcmp(cmd, "IN") && arg1 && arg2) {
      bool value = false;
      const bool ok = parseOnOff(arg2, value) && inputs_.setSimulated(arg1, value, now);
      mayapSerialPrintf(false, "[SIM] %s %s = %s\n", arg1, arg2, ok ? "OK" : "FAIL");
      return;
    }
    if (cmd && !strcmp(cmd, "SIM") && arg1 && !strcmp(arg1, "RESET")) {
      inputs_.resetSimulation(now); mayapSerialPrintf(false, "[SIM] RESET\n"); return;
    }
#endif


    if (cmd && !strcmp(cmd, "TIME") && arg1 && !strcmp(arg1, "SET") && arg2 && arg3) {
      unsigned year = 0U, month = 0U, day = 0U, hour = 0U, minute = 0U, second = 0U;
      int dateConsumed = 0;
      int timeConsumed = 0;
      const bool parsed =
          sscanf(arg2, "%4u-%2u-%2u%n", &year, &month, &day,
                 &dateConsumed) == 3 &&
          dateConsumed > 0 && arg2[dateConsumed] == '\0' &&
          sscanf(arg3, "%2u:%2u:%2u%n", &hour, &minute, &second,
                 &timeConsumed) == 3 &&
          timeConsumed > 0 && arg3[timeConsumed] == '\0' &&
          year >= RTC_VALID_YEAR_MIN && year <= RTC_VALID_YEAR_MAX &&
          month >= 1U && month <= 12U && day >= 1U && day <= 31U &&
          hour < 24U && minute < 60U && second < 60U;
      const bool ok = parsed && rtc_.set(static_cast<uint16_t>(year),
          static_cast<uint8_t>(month), static_cast<uint8_t>(day),
          static_cast<uint8_t>(hour), static_cast<uint8_t>(minute),
          static_cast<uint8_t>(second));
      mayapSerialPrintf(false, "[RTC] SET=%s %s %s\n", ok ? "OK" : "FAIL", arg2, arg3);
      return;
    }

    if (cmd && !strcmp(cmd, "BATCH") && arg1) {
      const char *message = nullptr;
      const bool ok = !strcmp(arg1, "START") ? startBatch(now, message)
                    : !strcmp(arg1, "STOP") ? stopBatch(now, message) : false;
      mayapSerialPrintf(false, "[SER] BATCH %s: %s - %s\n", arg1,
                       ok ? "OK" : "FAIL", message ? message : "SAI LENH");
    } else if (cmd && !strcmp(cmd, "ACK")) {
      if (emergencyActive_) {
        sirenMutedUntil_ = now + SIREN_TEMPORARY_MUTE_MS;
        mayapSerialPrintf(false, "[SER] SIREN MUTE 5 MIN\n");
      }
      if (abnormalResetLatched_) {
        abnormalResetLatched_ = false;
        power_.acknowledge();
        if (!power_.journalWriteOk()) {
          safetyJournalFaultLatched_ = true;
          faults_.set(FaultCode::SafetyJournalUnavailable, true, now);
        }
        faults_.set(FaultCode::AbnormalReset, false, now);
        heatRestartNotBefore_ = now + HEAT_RESTART_LOCKOUT_MS;
        pid_.reset();
      }
      (void)faults_.acknowledge(now, ALARM_KNOWN_MASK);
      mayapSerialPrintf(false, "[SER] ACK DONE\n");
    } else if (cmd && !strcmp(cmd, "FAULT") && arg1 && !strcmp(arg1, "CLEAR")) {
      const bool turnOk = clearTurnFault();
      (void)faults_.acknowledge(now, ALARM_KNOWN_MASK);
      mayapSerialPrintf(false, "[SER] CLEAR TURN=%s\n", turnOk ? "OK" : "BLOCKED");
    } else if (cmd && !strcmp(cmd, "FAULT") && arg1 && !strcmp(arg1, "LIST")) {
      faults_.print();
    } else if (cmd && !strcmp(cmd, "LOG") && !arg1) {
      // "LOG" khong tham so = bat debug Serial ngay (doi xung voi EXIT tat
      // ngay ben duoi) - de nguoi dung chi can go 2 lenh don gian LOG/EXIT
      // thay vi phai nho lenh SERIAL (toggle) hay cac lenh con LOG SHOW/FILES/
      // CLEAR khac.
      mayapSetSerialDebugEnabled(true);
      mayapSerialPrintf(true, "[SERIAL] ON (LOG)\n");
      printSerialHelp();
      printStatus(now);
    } else if (cmd && !strcmp(cmd, "LOG") && arg1 && !strcmp(arg1, "SHOW")) {
      eventLog_.print(arg2 ? static_cast<uint8_t>(constrain(atoi(arg2), 1, 96)) : 20U);
    } else if (cmd && !strcmp(cmd, "LOG") && arg1 && !strcmp(arg1, "FILES")) {
      batchLogger_.printFiles();
    } else if (cmd && !strcmp(cmd, "LOG") && arg1 && !strcmp(arg1, "CLEAR")) {
      const bool allowed = !batchRunning_ && !resumePending_;
      const bool ok = allowed && eventLog_.clear(now);
      mayapSerialPrintf(false, "[LOG] RAM CLEAR=%s%s\n",
          ok ? "OK" : "BLOCKED", allowed ? "" : " - DANG CO ME");
    } else if (cmd && !strcmp(cmd, "DIAG") && arg1) {
      bool value = false;
      if (parseOnOff(arg1, value)) diagnosticFast_ = value;
      mayapSerialPrintf(false, "[DIAG] FAST=%s\n", diagnosticFast_ ? "ON" : "OFF");
    } else if (cmd && !strcmp(cmd, "POWER")) {
      mayapSerialPrintf(false, "[POWER] reset=%s storm=%u ack=%u safetyNvs=%u\n",
          resetReasonText(power_.reason()), power_.resetStormCount(),
          power_.ackRequired(), !safetyJournalFaultLatched_);
    } else if (cmd && !strcmp(cmd, "HEAP")) {
      mayapSerialPrintf(false,
          "[HEAP] free=%lu minEver=%lu largest=%lu baseline=%lu windowBest=%lu windowWorst=%lu\n",
          static_cast<unsigned long>(ESP.getFreeHeap()),
          static_cast<unsigned long>(ESP.getMinFreeHeap()),
          static_cast<unsigned long>(ESP.getMaxAllocHeap()),
          static_cast<unsigned long>(healthHeapBaseline_),
          static_cast<unsigned long>(healthHeapLastBest_),
          static_cast<unsigned long>(healthHeapLastWorst_));
    } else if (cmd && !strcmp(cmd, "STATUS")) {
      printStatus(now);
    } else if (cmd && !strcmp(cmd, "CONFIG")) {
      // Debug day du theo yeu cau: khong chi thong so TINH (nguong/PID/...)
      // ma ca trang thai SONG (cai gi dang bat/tat, Wi-Fi da luu chua, MQTT/
      // Cloud Push co dang ket noi khong...) trong CUNG mot lenh, khong bat
      // nguoi dung phai go them STATUS moi thay het.
      printConfig();
      mayapPrintNetworkConfig();
      mayapPrintWebStatus();
      mayapPrintCloudStatus(now);
      printStatus(now);
    } else if (cmd && !strcmp(cmd, "EXIT")) {
      // Go EXIT = tat debug Serial ngay (khong doi trang thai hien tai la
      // ON/OFF - luon chac chan ve OFF). Luu y: ESP32 KHONG the tin cay phat
      // hien "dong cong Serial" o muc firmware (DTR/RTS va hanh vi USB-CDC
      // khac nhau tuy board/driver may tinh) nen chi ho tro lenh go tay nay,
      // khong tu dong theo trang thai cong that.
      mayapSetSerialDebugEnabled(false);
      mayapSerialPrintf(true, "[SERIAL] OFF (EXIT)\n");
    } else if (cmd && !strcmp(cmd, "HELP")) {
      printSerialHelp();
    } else {
      mayapSerialPrintf(false, "[SER] LENH SAI. GO HELP\n");
    }
  }

  void printSerialHelp() {
    mayapSerialPrintf(false, "[BUILD] fw=%s build=%s\n", MAYAP_FIRMWARE_VERSION, MAYAP_BUILD_ID);
    mayapSerialPrintf(false, "\n--- MAYAP INDUSTRIAL v%s SERIAL ---\n",
                     MAYAP_FIRMWARE_VERSION);
#if MAYAP_SERIAL_INPUT_SIM
    mayapSerialPrintf(false, "IN AUTO|HEATER|FAN|LIGHT|LEFT|RIGHT|LIMIT_LEFT|LIMIT_RIGHT ON|OFF\n");
    mayapSerialPrintf(false, "SIM RESET\n");
#endif
    mayapSerialPrintf(false, "TIME SET YYYY-MM-DD HH:MM:SS\n");
    mayapSerialPrintf(false, "BATCH START|STOP   ACK   FAULT LIST|CLEAR   STATUS   CONFIG   POWER   HEAP\n");
    mayapSerialPrintf(false, "Nhap SERIAL de tat/bat toan bo debug. DIAG ON|OFF doi toc do STATUS.\n");
    mayapSerialPrintf(false, "LOG = BAT debug Serial ngay. EXIT = TAT debug Serial ngay.\n");
    mayapSerialPrintf(false, "CONFIG = dump debug DAY DU (thong so + Wi-Fi/MQTT/Cloud Push + STATUS).\n");
    mayapSerialPrintf(false, "LOG SHOW [N]|FILES|CLEAR(RAM)   DIAG ON|OFF\n");
    mayapSerialPrintf(false, "PIN OUT: LEFT=D%u RIGHT=D%u LIGHT=D%u VENT=D%u MASTER=D%u SSR=D%u\n",
        PIN_OUT_TURN_LEFT, PIN_OUT_TURN_RIGHT, PIN_OUT_LIGHT,
        PIN_OUT_VENT_FAN, PIN_OUT_HEAT_MASTER, PIN_OUT_HEATER_SSR);
  }
  const char *heatBlockReason(uint32_t now) const {
    const InputState &in = inputs_.state();
    if (batchClearPending_) return "BATCH_CLEAR_PENDING";
    if (safetyJournalFaultLatched_) return "SAFETY_JOURNAL_FAIL";
    if (storageFaultLatched_) return "EEPROM_FAULT";
    if (storageDegraded_ && !batchRunning_) return "EEPROM_DEGRADED";
    if (abnormalResetLatched_) return "RESET_NOT_ACK";
    if (faults_.active(FaultCode::HeaterSwitchOffDuringBatch))
      return "E130_HEATER_SWITCH_OFF";
    if (faults_.masterDropRequired()) return "MASTER_SAFETY_DROP";
    if (!in.heaterEnable) return "HEATER_SWITCH_OFF";
    if (!batchRunning_) return "BATCH_NOT_RUNNING";
    if (!sensorUsable_) return "SENSOR_NOT_READY";
    if (emergencyActive_) return "TEMP_EMERGENCY_MASTER_OFF";
    if (!timeReached(now, heatRestartNotBefore_)) return "RESTART_DELAY";
    if (highTemperatureActive_) return "TEMP_HIGH_SSR_OFF";
    if (faults_.ssrInhibited()) return "SSR_SAFETY_BLOCK";
    if (outputs_.masterRestartWaitMs(now) != 0U) return "MASTER_MIN_OFF";
    const bool baseFan = in.autoMode
        ? (batchRunning_ || autotune_.running())
        : in.circulationFan;
    const bool fanCommand = baseFan || ventTemperatureActive_ || highTemperatureActive_ ||
                            emergencyActive_ || !timeReached(now, postCoolUntil_);
    const bool fanStable = fanCommand && fanOnSince_ != 0U &&
                           elapsedMs(now, fanOnSince_) >= FAN_PRESTART_MS;
    if (MANUAL_FAN_CAN_DISABLE_HEATING && !fanStable) return "FAN_PRESTART";
    if (ventTemperatureActive_) return "VENT_LEVEL_1";
    return "READY";
  }

  uint32_t heatWaitRemainingMs(uint32_t now) const {
    if (!timeReached(now, heatRestartNotBefore_)) return heatRestartNotBefore_ - now;
    const uint32_t masterWait = outputs_.masterRestartWaitMs(now);
    if (masterWait != 0U) return masterWait;
    if (fanOnSince_ != 0U && elapsedMs(now, fanOnSince_) < FAN_PRESTART_MS) {
      return FAN_PRESTART_MS - elapsedMs(now, fanOnSince_);
    }
    return 0U;
  }

  // Cac thong so DIEU KHIEN quan trong - in mot lan luc boot va bat cu khi
  // nao go lenh CONFIG, de theo doi tu xa (chi doc, khong sua duoc tu day)
  // dung nguong nao dang co hieu luc ma khong can mo man hinh HMI.
  void printConfig() const {
    mayapSerialPrintf(false,
        "[CONFIG] SV=%.1fC hys=%.2fC bao_thap=%.1fC bao_cao=%.1fC khan_cap=%.1fC che_do=%s\n",
        config_.targetTemp, config_.tempHysteresis, config_.lowTempAlarm,
        config_.highTempAlarm, config_.emergencyTemp,
        config_.controlMode == ControlMode::Pid ? "PID" : "ON/OFF");
    mayapSerialPrintf(false,
        "[CONFIG] PID kp=%.2f ki=%.2f kd=%.2f legacy_cycle=%us burst_candidate=%lums cong_suat_max=%u%%\n",
        config_.kp, config_.ki, config_.kd, config_.pidCycleSec,
        static_cast<unsigned long>(HEATER_BURST_QUANTUM_MS), config_.maxHeaterPower);
    mayapSerialPrintf(false,
        "[CONFIG] Nang cao: ket_dinh=%.2fC/%us toc_do=%.2fC/%us dao_dong=%u/%us "
        "tune_cong_suat=%u%% tune_band=%.2fC\n",
        config_.heaterStuckMinRiseC, config_.heaterStuckDurationSec,
        config_.tempRateLimitC, config_.tempRateWindowSec,
        config_.tempOscillationCrossLimit, config_.tempOscillationWindowSec,
        config_.autotuneRelayPowerPercent, config_.autotuneBandC);
    mayapSerialPrintf(false,
        "[CONFIG] am_thap=%.0f%% tre_bao_am=%us quat_hut_bat=%.1fC quat_hut_tat=%.1fC quat_tuan_hoan=%s\n",
        config_.lowHumidityAlarm, config_.humidityAlarmDelaySec,
        config_.ventOnTemp, config_.ventOffTemp,
        config_.circulationFanEnabled ? "BAT" : "TAT");
    mayapSerialPrintf(false,
        "[CONFIG] dao_trung=%s chu_ky=%uph tre_loi_dao=%us huong_ke=%s so_ngay_ap=%u\n",
        config_.turningEnabled ? "TU DONG" : "TAT", config_.turnIntervalMin,
        config_.turnMaxRunSec,
        config_.nextDirection == TurnDirection::Right ? "PHAI" : "TRAI",
        config_.totalIncubationDays);
    mayapSerialPrintf(false,
        "[CONFIG] ap_lai_sau_mat_dien=%s tre_khoi_phuc=%us bu_nhiet=%.1fC bu_am=%.0f%% timeout_cam_bien=%us\n",
        config_.autoResumeOnPowerLoss ? "TU DONG" : "HOI XAC NHAN",
        config_.powerRestoreDelaySec, config_.tempOffset,
        config_.humidityOffset, config_.sensorTimeoutSec);
    mayapSerialPrintf(false, "[CONFIG] canh_bao_am_thanh=%s che_do_ket_noi=%s\n",
        config_.alarmEnabled ? "BAT" : "TAT",
        config_.connectivityMode == ConnectivityMode::Online ? "ONLINE" : "OFFLINE");
    mayapSerialPrintf(false, "[CONFIG] canh_bao_den_con_bat_khi_dang_ap=%s\n",
        config_.lightAfterBatchAlarmEnabled ? "BAT" : "TAT");
    mayapSerialPrintf(false, "[CONFIG] bao_nhiet_ngoai_me=%s\n",
        config_.highTempAlarmWithoutBatch ? "BAT" : "TAT");
  }

  void printStatus(uint32_t now, bool detailed = true) {
    const InputState &in = inputs_.state();
    mayapSerialPrintf(false, "[PROCESS] batch=%u sensor=%u T=%.2f SV=%.2f RH=%.1f PID=%.1f ssr=%u master=%u fault=%u\n",
      batchRunning_, sensorUsable_, temperature_, config_.targetTemp, humidity_, runtime_.heaterPower,
      outputs_.state().heaterSsr, outputs_.state().heatMaster, static_cast<unsigned>(faults_.primary()));
    mayapSerialPrintf(false, "[HEAP] free=%lu minEver=%lu largest=%lu tlsBusy=%u deferred=%lu\n",
      static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMinFreeHeap()),
      static_cast<unsigned long>(ESP.getMaxAllocHeap()), mayapTlsBusy(),
      static_cast<unsigned long>(mayapTlsDeferredCount()));
    if (!detailed) return;
    mayapSerialPrintf(false, "[SENSOR] profile=%u register=%u raw=%.4f filtered=%.4f humidity=%.2f\n",
        static_cast<unsigned>(sensor_.sensorProfile()), sensor_.rawTemperatureRegister(),
        sensor_.rawTemperatureC(), sensor_.temperatureC(), sensor_.humidityRH());
    mayapSerialPrintf(false, "[STATUS] switch=%s batch=%u resume=%u clear=%u recovery=%s phase=%u sensor=%u storage=%u safetyNvs=%u resetFault=%u T=%.2f raw=%.2f H=%.1f\n",
      in.autoMode ? "AUTO" : "MAN",
      batchRunning_, resumePending_, batchClearPending_,
      resumeBlockReasonText(resumeBlockReason_),
      static_cast<unsigned>(batchPhase_),
      sensorUsable_, (storageFaultLatched_ || storageDegraded_),
      !safetyJournalFaultLatched_, abnormalResetLatched_,
      temperature_, rawTemperature_, humidity_);
    mayapSerialPrintf(false, "[STATUS] IN heat=%u fan=%u light=%u L=%u R=%u limL=%u limR=%u\n",
      in.heaterEnable, in.circulationFan, in.light, in.turnLeft, in.turnRight,
      in.limitLeft, in.limitRight);
    mayapSerialPrintf(false, "[RTC] addr=0x%02X online=%u valid=%u osf=%u epoch=%lu date=%s\n",
      RTC_I2C_ADDRESS, rtc_.online(), rtc_.valid(), rtc_.oscillatorStopped(),
      static_cast<unsigned long>(rtc_.epoch()), rtc_.dateText());
    mayapSerialPrintf(false, "[EEPROM] type=AT24C32 addr=0x%02X capacity=%u page=%u ready=%u\n",
      EEPROM_I2C_ADDRESS,
      static_cast<unsigned>(EEPROM_CAPACITY_BYTES),
      static_cast<unsigned>(EEPROM_PAGE_SIZE),
      !(storageFaultLatched_ || storageDegraded_));
    const NetworkStatus network = mayapGetNetworkStatus();
    mayapSerialPrintf(false,
      "[NET] mode=%s state=%u configured=%u connected=%u rssi=%d dBm\n",
      network.requestedMode == ConnectivityMode::Online ? "ONLINE" : "OFFLINE",
      static_cast<unsigned>(network.state), network.credentialsConfigured,
      network.connected, static_cast<int>(network.rssiDbm));
    mayapSerialPrintf(false,
      "[HEAT] block=%s wait=%lums heatIn=D%u:%d master=D%u:%d ssr=D%u:%d\n",
      heatBlockReason(now), static_cast<unsigned long>(heatWaitRemainingMs(now)),
      PIN_IN_HEATER_ENABLE, digitalRead(PIN_IN_HEATER_ENABLE),
      PIN_OUT_HEAT_MASTER, digitalRead(PIN_OUT_HEAT_MASTER),
      PIN_OUT_HEATER_SSR, digitalRead(PIN_OUT_HEATER_SSR));
    mayapSerialPrintf(false,
      "[PIN] left=D%u right=D%u circ=D%u master=D%u light=D%u vent=D%u ssr=D%u\n",
      PIN_OUT_TURN_LEFT, PIN_OUT_TURN_RIGHT, PIN_OUT_CIRC_FAN,
      PIN_OUT_HEAT_MASTER, PIN_OUT_LIGHT, PIN_OUT_VENT_FAN,
      PIN_OUT_HEATER_SSR);
    mayapSerialPrintf(false, "[STATUS] OUT ssr=%u master=%u fan=%u vent=%u light=%u left=%u right=%u siren=%u spareR=%u PID=%.1f alarm=0x%08lX\n",
      outputs_.state().heaterSsr, outputs_.state().heatMaster,
      outputs_.state().circulationFan, outputs_.state().ventFan,
      outputs_.state().light, outputs_.state().turnLeft,
      outputs_.state().turnRight, outputs_.state().siren,
      outputs_.state().humidifier,
      runtime_.heaterPower, static_cast<unsigned long>(runtime_.alarmMask));
    mayapSerialPrintf(false, "[KERNEL] fault=%u count=%u events=%lu relay/h=%u inDrop=%lu outDrop=%lu\n",
      static_cast<unsigned>(faults_.primary()), faults_.activeCount(),
      static_cast<unsigned long>(eventLog_.sequence()), outputs_.transitionsThisHour(),
      static_cast<unsigned long>(inputs_.droppedEvents()),
      static_cast<unsigned long>(outputs_.droppedEvents()));
    mayapSerialPrintf(false, "[SHT] good=%lu timeout=%lu crc=%lu fmt=%lu range=%lu tx=%lu ignored=%lu age=%lums heap=%u\n",
      static_cast<unsigned long>(sensor_.goodFrames()),
      static_cast<unsigned long>(sensor_.timeoutErrors()),
      static_cast<unsigned long>(sensor_.crcErrors()),
      static_cast<unsigned long>(sensor_.formatErrors()),
      static_cast<unsigned long>(sensor_.rangeErrors()),
      static_cast<unsigned long>(sensor_.txErrors()),
      static_cast<unsigned long>(sensor_.ignoredBytes()),
      static_cast<unsigned long>(sensor_.dataAgeMs()),
      ESP.getFreeHeap());
    (void)now;
  }
  // ----------------------------- Members -------------------------------------
  EventLog eventLog_{};
  BatchLogger batchLogger_{};
  FaultManager faults_{&eventLog_};
  SafetyJournal safetyJournal_{};
  PowerManager power_{};
  RtcDs3231 rtc_{};
  PersistentStore store_{};
  InputManager inputs_{};
  SHT485Industrial sensor_{};
  ThermalController pid_{};
  ThermalStartupController startupHeat_{};
  AutoTuneEngine autotune_{};
  OutputArbiter outputs_{};
  StatusLed led_{};

  MachineConfig config_{};
  MachineRuntime runtime_{};
  bool configLoaded_ = false;

  uint32_t bootAt_ = 0;
  esp_reset_reason_t resetReason_ = ESP_RST_UNKNOWN;
  bool abnormalResetLatched_ = false;
  bool sensorUsable_ = false;
  bool safetySampleValid_ = false;
  uint32_t sensorStartupGraceUntil_ = 0U;
  uint8_t goodSensorStreak_ = 0;
  bool newSensorSample_ = false;
  float temperature_ = NAN;
  float rawTemperature_ = NAN;
  float humidity_ = NAN;
  float lastAcceptedTemperature_ = NAN;
  float lastSuspectCandidate_ = NAN;
  bool latestFrameValid_ = false;
  bool sensorSuspect_ = false;
  uint8_t sensorPlausibilityStreak_ = 0U;
  // F-08: xem processSensor()/updateAlarms() - phat hien cam bien "dung hinh".
  float sensorFrozenRefTemp_ = NAN;
  uint32_t sensorFrozenSince_ = 0U;

  bool batchRunning_ = false;
  bool resumePending_ = false;
  bool resumeConfirmationRequired_ = false;
  uint32_t resumeConfirmPromptAt_ = 0U;
  bool automaticResetRecovery_ = false;
  // Dat 1 lan trong begin() khi phat hien khoi dong lai sau mat dien giua me
  // ap; giu nguyen suot phien chay (khong tu xoa) de cloud_alert_link.h doc.
  bool powerLossRecovery_ = false;
  ResumeBlockReason resumeBlockReason_ = ResumeBlockReason::None;
  uint32_t resumeBlockReasonAt_ = 0U;
  bool batchClearPending_ = false;
  uint8_t batchClearAttemptCount_ = 0U;
  uint32_t batchClearRetryAt_ = 0U;
  BatchPhase batchPhase_ = BatchPhase::Stopped;
  uint32_t batchStartedAt_ = 0;
  uint32_t phaseStartedAt_ = 0;
  uint32_t elapsedBeforeStartSec_ = 0;
  uint32_t savedElapsedAtCheckpoint_ = 0U;
  uint32_t lastCheckpointEpoch_ = 0U;
  bool resumeClockAdjusted_ = true;
  uint32_t lastTurnCounterDay_ = 0;
  uint16_t turnCountToday_ = 0;
  uint32_t turnCountBatch_ = 0;
  uint32_t batchStartEpoch_ = 0U;
  uint32_t lastTurnEpoch_ = 0U;
  uint32_t lastTurnAt_ = 0U;
  uint32_t turnAnchorWaitSince_ = 0U;
  uint32_t batchLogSampleSequence_ = 0U;
  bool batchLogFaultActive_ = false;

  TrayPosition trayPosition_ = TrayPosition::Unknown;
  TrayPosition moveOrigin_ = TrayPosition::Unknown;
  TurnPhase turnPhase_ = TurnPhase::Idle;
  bool moveIsHoming_ = false;
  bool moveCounts_ = false;
  bool needHome_ = false;
  bool turnFaultLatched_ = false;
  FaultCode turnFaultCode_ = FaultCode::None;
  // So lan TurnTimeout/TurnLimitStuck lien tiep KHONG co lan dao thanh cong
  // xen giua (xem latchTurnFault()/completeTurn()). Vuot TURN_FAULT_STREAK_LIMIT
  // thi bat TurnMechanicalCheckRequired, khoa dao hoan toan cho toi khi xac
  // nhan lai qua Test Mode (ca 2 CTHT deu Success - xem updateTestMode()).
  uint8_t turnFaultStreak_ = 0U;
  bool turnMechanicalCheckRequired_ = false;
  // F-06: xem updateBatchOverdue()/SafetyJournal::batchOverdueAcknowledged().
  bool batchOverdueAcknowledged_ = false;
  bool batchOverdueSirenActive_ = false;
  // Tu kiem tra coi dinh ky (backlog, khong thuoc audit v3.7.1) - xem
  // updateSirenSelfTest(). Chi ton tai trong RAM (khong can song sot qua
  // reboot - tinh nang khong an toan quan trong, mat lich 1 lan sau reset
  // khong sao, se tu tinh lai chu ky moi tu luc khoi dong lai).
  uint32_t sirenSelfTestNextAt_ = 0U;
  uint32_t sirenSelfTestPulseUntil_ = 0U;
  bool sirenSelfTestActive_ = false;
  // Bao mat dien qua ATtiny13A protocol v4. Batch + activity duoc Tiny
  // luu EEPROM co verify; 9V sense/E502 van giu nguyen.
  bool attinySirenMirrorOn_ = false;
  bool attinyActivityMirrorOn_ = false;
  bool attinyActivityDesired_ = false;
  bool attinyActivitySynced_ = false;
  AttinyStateSync attinyBatchSync_;
  AttinyStateSync attinyActivitySync_;
  uint8_t attinySyncFaultDetail_ = 0U;
  bool attinyTinyActivity_ = false;
  bool attinyStartupProbePending_ = true;
  bool attinyLinkChecked_ = false;
  bool attinyLinkHealthy_ = false;
  bool attinyStatusKnown_ = false;
  bool attinyTinyBatch_ = false;
  bool attinyTinySirenOn_ = false;
  bool attinyBatchSynced_ = false;
  bool attiny9vLow_ = false;
  bool attiny9vCandidate_ = false;
  bool attiny9vConfirmPending_ = false;
  bool attinyStatusAwaiting_ = false;
  uint32_t attinyLastStatusQueryAt_ = 0U;
  uint32_t attinyLastStatusAt_ = 0U;
  uint32_t attinyLastResyncAt_ = 0U;
  uint32_t attinyLastSirenAssertAt_ = 0U;
  uint32_t attinyActivityOffSince_ = 0U;
  uint32_t attinyStatusDeadline_ = 0U;
  uint32_t attiny9vConfirmAt_ = 0U;
  bool testLimitVerifiedLeft_ = false;
  bool testLimitVerifiedRight_ = false;
  uint32_t moveStartedAt_ = 0;
  uint32_t deadtimeUntil_ = 0;
  uint32_t nextTurnAt_ = 0;
  uint32_t manualConflictSince_ = 0;
  bool previousAutoMode_ = false;
  // Theo yeu cau nguoi lap dat: cong tac dao vat ly la QUYEN CAO NHAT, ke ca
  // ngay luc may vua khoi dong - dang giu huong nao la quay huong do ngay,
  // khong bat buoc nha-bam lai. Vi vay mac dinh la false (KHONG doi rearm),
  // khac voi truoc day (= true) tung khien lan dau tien sau khi bat may bo
  // qua vi tri cong tac hien tai. Cac diem con lai van dat true co chu dich
  // rieng (loi turningInhibited/qua nhiet vua het, mat cam bien) - KHONG bi
  // anh huong boi thay doi nay.
  bool manualTurnRearmRequired_ = false;

  bool testModeActive_ = false;
  uint8_t testOutputMaskActive_ = 0U;
  uint32_t testOutputPulseUntil_[static_cast<uint8_t>(TestOutputId::Count)]{};
  uint32_t testHeaterStartedAt_ = 0U;
  uint32_t testHeaterPostCoolUntil_ = 0U;
  uint32_t testTurnStartedAt_ = 0U;
  TrayPosition testTurnOrigin_ = TrayPosition::Unknown;
  TestOutputId testTurnDirection_ = TestOutputId::Count;
  TestLimitId testLimitTarget_ = TestLimitId::Left;
  TestLimitPhase testLimitPhase_ = TestLimitPhase::Idle;
  uint32_t testLimitDeadline_ = 0U;
  uint32_t testLimitBuzzUntil_ = 0U;
  uint32_t testModeLastActivityAt_ = 0U;

  ConditionTimer heaterRestoreTimer_{};
  ConditionTimer autoLostTimer_{};
  bool heaterSwitchFaultActive_ = false;
  bool autoModeFaultActive_ = false;

  ConditionTimer lowTempTimer_{};
  ConditionTimer highTripTimer_{};
  ConditionTimer highClearTimer_{};
  ConditionTimer emergencyClearTimer_{};
  ConditionTimer humidityTimer_{};
  bool ventTemperatureActive_ = false;
  bool lowTemperatureActive_ = false;
  bool highTemperatureActive_ = false;
  bool emergencyActive_ = false;
  bool humidityLowActive_ = false;

  // --- Nhom canh bao bo sung (chan doan/canh bao som) ---
  ConditionTimer humidityHighTimer_{};
  bool humidityHighActive_ = false;
  float tempRateRefValue_ = NAN;
  uint32_t tempRateRefAt_ = 0U;
  bool temperatureRateActive_ = false;
  int8_t tempOscillationLastSign_ = 0;
  uint8_t tempOscillationCrossCount_ = 0U;
  uint8_t tempOscillationLastWindowCrossCount_ = 0U;
  uint32_t tempOscillationWindowStart_ = 0U;
  bool temperatureUnstableActive_ = false;
  bool heaterStuckTracking_ = false;
  uint32_t heaterStuckSinceAt_ = 0U;
  uint32_t heaterStuckAccumOnMs_ = 0U;
  float heaterStuckStartTemp_ = NAN;
  bool heaterNotHeatingActive_ = false;

  // --- Giam sat suc khoe he thong (v3.6.0) - xem serviceHealthMonitor() ---
  PeriodicGate healthCheckGate_{HEALTH_CHECK_INTERVAL_MS};
  PeriodicGate healthHeapSampleGate_{HEALTH_HEAP_SAMPLE_INTERVAL_MS};
  bool healthBaselineCaptured_ = false;
  bool healthWarmupWindowStarted_ = false;
  bool startupStorageInitialized_ = false;
  bool startupStoreReady_ = false;
  uint32_t healthHeapBaseline_ = 0U;
  uint32_t healthHeapWindowBest_ = 0U;
  uint32_t healthHeapWindowWorst_ = UINT32_MAX;
  uint32_t healthHeapLastBest_ = 0U;
  uint32_t healthHeapLastWorst_ = 0U;
  uint8_t healthHeapLowStreak_ = 0U;
  bool healthHeapLowActive_ = false;
  uint8_t healthHeapSeriousStreak_ = 0U;
  float healthTempBaseline_ = NAN;
  uint32_t healthTempBaselineAt_ = 0U;
  uint32_t sirenMutedUntil_ = 0;
  // Cong tac den (PIN_IN_LIGHT) van la nguon dieu khien mac dinh (xem
  // updateHeatingAndOutputs()); nut bat/tat den tren web CHI de lai 1 lenh
  // "ghi de" tam thoi - het hieu luc ngay khi cong tac vat ly thuc su doi
  // trang thai (xem lightWebOverrideRefInput_), tranh truong hop web tuong
  // den dang OFF trong khi ai do da bat cong tac that tai may.
  bool lightWebOverrideActive_ = false;
  bool lightWebOverrideValue_ = false;
  bool lightWebOverrideRefInput_ = false;
  uint32_t heatRestartNotBefore_ = 0;
  uint32_t postCoolUntil_ = 0;
  bool safetyJournalFaultLatched_ = false;
  bool storageFaultLatched_ = false;
  bool storageDegraded_ = false;
  uint8_t storageFailureStreak_ = 0U;
  uint32_t lastStorageReconnectAt_ = 0U;
  uint32_t lastStorageHealthCheckAt_ = 0U;
  NetworkStatus lastNetworkStatus_{};
  bool networkStatusInitialized_ = false;

  float pidPower_ = 0.0f;
  MayapAdaptive::AdaptiveThermalSupervisor adaptiveThermal_;
  MayapThermal::AdaptiveV1 thermalV1_;
  MayapThermal::VentInfo ventInfo_{};
  bool ventForcedRun_ = false;
  uint32_t thermalDiagnosticAt_=0;
  MayapThermal::LearnState thermalLoggedState_=MayapThermal::LearnState::Unlearned;
  bool thermalMismatchLogged_=false;
  uint32_t adaptiveDiagnosticAt_=0, adaptiveSignature_=0;
  bool adaptiveSignatureSeen_=false;
  MayapAdaptive::State adaptiveLoggedState_=MayapAdaptive::State::Disabled;
  HeaterBurstScheduler heaterBurst_{HEATER_GROUP_COUNT, HEATER_BURST_QUANTUM_MS};
  bool previousFanCommand_ = false;
  uint32_t fanOnSince_ = 0;
  // Contactor nhiet tong PHAI dong tuc thi cung cong tac (yeu cau nguoi lap
  // dat, xem comment tai normalMasterPermit) nen KHONG duoc tri hoan no. De
  // 2 relay (quat tuan hoan + contactor) khong dong DIEN CUNG 1 thoi diem
  // luc bat dau me (yeu cau rieng ve dien), chi tri hoan NGO RA QUAT mot
  // khoang rat ngan tai dung thoi diem me bat dau - khong anh huong nhiet
  // that su vi SSR van doi fanStable (FAN_PRESTART_MS) nhu cu.
  bool prevBatchRunningForOutputs_ = false;
  uint32_t circFanStaggerUntil_ = 0;

  PeriodicGate runtimeGate_{RUNTIME_TO_HMI_MS};
  PeriodicGate checkpointGate_{BATCH_CHECKPOINT_MS};
  PeriodicGate batchLogGate_{BATCH_LOG_SAMPLE_MS};
  PeriodicGate diagnosticGate_{DIAGNOSTIC_STATUS_MS};
  PeriodicGate networkPollGate_{NETWORK_TASK_PERIOD_MS};
  uint32_t lastHmiEventSequence_ = UINT32_MAX;
  uint32_t lastHmiEventPushAt_ = 0U;
  bool diagnosticFast_ = false;
  char serialLine_[128]{};
  uint8_t serialLength_ = 0;
  bool serialDiscardUntilNewline_ = false;
};

// Mot doi tuong duy nhat, khong new/delete trong van hanh.
static MachineController Machine;

} // namespace Mayap
