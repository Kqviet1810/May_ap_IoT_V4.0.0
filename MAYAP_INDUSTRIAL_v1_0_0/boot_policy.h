#pragma once

#include <stdint.h>
#include <string.h>

// Platform-independent policy; the host tests exercise this exact code.
namespace MayapBoot {
enum class Stage : uint32_t {
  SafeOutputs, Storage, SensorMachine, Hmi, ControlSafety, LocalSettle,
  Wifi, Mqtt, Cloud, Ota, Running
};
enum class RestartReason : uint32_t {
  None, FatalInit, WdtApi, ControlHeartbeat, ControlDeadline, HmiFatal,
  HealthMonitor, ArduinoOta, InternetOta, Rollback
};
enum class ResetKind : uint8_t { PowerOn, Brownout, Software, Unexpected, DeepSleep };
enum class Status : uint8_t { Hardware, Memory, Sensor, Control, Safety, Network, Server, Ready };

// Radio tasks start as soon as the local tasks (control, HMI, supervisor) have been healthy this long. The sensor is polled by the control
// task from the same moment, so RS485 identification and Wi-Fi/MQTT initialisation run in parallel after the safe hardware is up.
constexpr uint32_t LOCAL_SETTLE_MS = 1500U;
constexpr uint32_t SUCCESS_STABLE_MS = 25000U;
constexpr uint32_t RECOVERY_LOCAL_MS = 45000U;
constexpr uint32_t FAILURES_CLEAR_MS = 600000U;
// Startup admission budgets only; runtime reconnect/backoff remains unchanged.
constexpr uint32_t WIFI_WAIT_MS = 250U;
constexpr uint32_t MQTT_WAIT_MS = 250U;
constexpr uint32_t SERVICE_GAP_MS = 250U;
constexpr uint32_t READY_DISPLAY_MS = 500U;
constexpr uint32_t RECORD_MAGIC = 0x4D425431U;
constexpr uint32_t RECORD_VERSION = 2U;

struct Diagnostic {
  uint32_t magic;
  uint32_t version;
  uint32_t sequence;
  uint32_t resetReason;
  uint32_t previousResetReason;
  Stage lastBootStage;
  Stage previousBootStage;
  uint32_t consecutiveFailedBoots;
  RestartReason plannedRestartReason;
  RestartReason previousPlannedRestartReason;
  uint32_t recoveryLevel;
  uint32_t bootCompleted;
  char restartDetail[80];
  char previousRestartDetail[80];
  uint32_t checksum;
};

inline uint32_t checksum(const Diagnostic &d) {
  // Hash fields explicitly: no ABI padding enters the retained checksum.
  uint32_t h = 2166136261U;
  const uint32_t fields[] = {d.magic, d.version, d.sequence, d.resetReason, d.previousResetReason,
    static_cast<uint32_t>(d.lastBootStage), static_cast<uint32_t>(d.previousBootStage),
    d.consecutiveFailedBoots, static_cast<uint32_t>(d.plannedRestartReason),
    static_cast<uint32_t>(d.previousPlannedRestartReason), d.recoveryLevel, d.bootCompleted};
  for (uint32_t field : fields) {
    for (uint8_t n = 0; n < 4U; ++n) { h = (h ^ (field & 255U)) * 16777619U; field >>= 8U; }
  }
  for (char c : d.restartDetail) h = (h ^ static_cast<uint8_t>(c)) * 16777619U;
  for (char c : d.previousRestartDetail) h = (h ^ static_cast<uint8_t>(c)) * 16777619U;
  return h;
}
inline bool valid(const Diagnostic &d) {
  return d.magic == RECORD_MAGIC && d.version == RECORD_VERSION &&
      d.lastBootStage <= Stage::Running && d.previousBootStage <= Stage::Running &&
      d.plannedRestartReason <= RestartReason::Rollback &&
      d.previousPlannedRestartReason <= RestartReason::Rollback &&
      d.recoveryLevel <= 3U && d.bootCompleted <= 1U && d.checksum == checksum(d);
}
inline bool operatorRestart(RestartReason reason) {
  return reason == RestartReason::ArduinoOta || reason == RestartReason::InternetOta ||
         reason == RestartReason::Rollback;
}
inline uint32_t levelForFailures(uint32_t count) { return count > 3U ? 3U : count; }

inline uint8_t newestSlot(const Diagnostic &a, const Diagnostic &b) {
  if (!valid(a)) return 1U;
  if (!valid(b)) return 0U;
  return static_cast<int32_t>(b.sequence - a.sequence) > 0 ? 1U : 0U;
}

inline Diagnostic beginDiagnostic(const Diagnostic &old, uint32_t resetReason, ResetKind kind) {
  Diagnostic next{};
  next.magic = RECORD_MAGIC;
  next.version = RECORD_VERSION;
  next.resetReason = resetReason;
  if (valid(old) && kind != ResetKind::PowerOn) {
    next.sequence = old.sequence;
    next.previousResetReason = old.resetReason;
    next.previousBootStage = old.lastBootStage;
    // A stale marker must NEVER turn a panic/WDT/brownout into an intentional reboot.
    next.previousPlannedRestartReason = old.plannedRestartReason;
    memcpy(next.previousRestartDetail, old.restartDetail, sizeof(next.previousRestartDetail));
    next.consecutiveFailedBoots = old.consecutiveFailedBoots;
    const bool expected = kind == ResetKind::Software && operatorRestart(old.plannedRestartReason);
    const bool failed = !expected && (kind != ResetKind::DeepSleep || !old.bootCompleted);
    if (failed && next.consecutiveFailedBoots != UINT32_MAX) ++next.consecutiveFailedBoots;
  }
  next.lastBootStage = Stage::SafeOutputs;
  next.recoveryLevel = levelForFailures(next.consecutiveFailedBoots);
  next.checksum = checksum(next);
  return next;
}

inline Status statusFor(Stage stage) {
  switch (stage) {
    case Stage::SafeOutputs: return Status::Hardware;
    case Stage::Storage: return Status::Memory;
    case Stage::SensorMachine: return Status::Sensor;
    case Stage::Hmi: return Status::Control;
    case Stage::ControlSafety: case Stage::LocalSettle: return Status::Safety;
    case Stage::Wifi: return Status::Network;
    case Stage::Mqtt: case Stage::Cloud: case Stage::Ota: return Status::Server;
    default: return Status::Ready;
  }
}
inline const char *stageText(Stage stage) {
  static const char *const names[] = {"SAFE_OUTPUTS", "STORAGE", "SENSOR_MACHINE", "HMI",
    "CONTROL_SAFETY", "LOCAL_SETTLE", "WIFI", "MQTT", "CLOUD", "OTA", "RUNNING"};
  const uint32_t index = static_cast<uint32_t>(stage);
  return index <= static_cast<uint32_t>(Stage::Running) ? names[index] : "UNKNOWN";
}
inline const char *restartText(RestartReason reason) {
  static const char *const names[] = {"NONE", "FATAL_INIT", "WDT_API", "CONTROL_HEARTBEAT",
    "CONTROL_DEADLINE", "HMI_FATAL", "HEALTH_MONITOR", "ARDUINO_OTA", "INTERNET_OTA", "ROLLBACK"};
  const uint32_t index = static_cast<uint32_t>(reason);
  return index <= static_cast<uint32_t>(RestartReason::Rollback) ? names[index] : "UNKNOWN";
}

class Stability {
 public:
  void update(uint32_t now, bool healthy) {
    if (!healthy) { active_ = false; return; }
    if (!active_) { active_ = true; since_ = now; }
  }
  bool held(uint32_t now, uint32_t duration) const {
    return active_ && static_cast<uint32_t>(now - since_) >= duration;
  }
 private:
  bool active_ = false;
  uint32_t since_ = 0U;
};

// Each stage is entered once. Connection waits are bounded and independent
// from Home and BOOT_SUCCESS. Offline mode still releases the service tasks.
class Sequencer {
 public:
  void begin(uint32_t level, uint32_t now) { level_ = level; stage_ = Stage::SafeOutputs; enteredAt_ = now; }
  Stage stage() const { return stage_; }
  void advance(uint32_t now) {
    if (stage_ != Stage::Running) stage_ = static_cast<Stage>(static_cast<uint32_t>(stage_) + 1U);
    enteredAt_ = now;
  }
  uint32_t age(uint32_t now) const { return static_cast<uint32_t>(now - enteredAt_); }
  uint32_t serviceGap() const { return level_ == 0U ? SERVICE_GAP_MS : 3000U; }
  uint32_t networkDelay() const {
    return level_ == 3U ? RECOVERY_LOCAL_MS : level_ == 2U ? 10000U : level_ == 1U ? 8000U : LOCAL_SETTLE_MS;
  }
  bool homeBeforeNetwork() const { return level_ >= 2U; }
  uint32_t level() const { return level_; }
  bool releaseNetwork(uint32_t now, const Stability &local) const {
    return local.held(now, networkDelay());
  }
  bool wifiDone(uint32_t now, bool ready, bool connected) const {
    return ready && age(now) >= serviceGap() && (connected || age(now) >= WIFI_WAIT_MS);
  }
  bool mqttDone(uint32_t now, bool ready, bool connected) const {
    return ready && age(now) >= serviceGap() && (connected || age(now) >= MQTT_WAIT_MS);
  }
 private:
  uint32_t level_ = 0U;
  Stage stage_ = Stage::SafeOutputs;
  uint32_t enteredAt_ = 0U;
};

// ---------------------------------------------------------------------------------------------------------------------------------
// Home gate. The splash is left for Home ONLY when the sensor delivers a valid, stable temperature and every local safety condition is
// met. If that has not happened 30 s after power-up the machine does NOT pretend: it shows the diagnostic screen with the concrete code
// of what is still missing (control keeps running locally, independent of the network) and goes to Home by itself as soon as the cause
// is gone. Pure logic: the host tests drive exactly this code.
constexpr uint32_t BOOT_DEADLINE_MS = 30000U;
constexpr uint32_t READY_HOLD_MS = 1500U;      // every condition continuously true this long ("stable")

enum class BlockReason : uint8_t {
  None, SafetyTrip, Tasks, Storage, Display, SensorNoResponse, SensorFormat, SensorUnstable
};
struct LocalInputs {
  bool tasksHealthy = false;       // control + HMI + supervisor heartbeats fresh, no cycle trip
  bool displayHealthy = false;
  bool storageBlocked = false;     // EEPROM required but unavailable
  bool tripLatched = false;        // system trip (output safe state) latched
  bool sensorUsable = false;       // controller verdict: valid frame, locked format, plausibility + 3 good samples
  bool temperatureFinite = false;
  uint8_t sensorBootReason = 1U;   // SHT485Industrial::bootReason()
};
inline BlockReason blockReason(const LocalInputs &in) {
  if (in.tripLatched) return BlockReason::SafetyTrip;
  if (!in.tasksHealthy) return BlockReason::Tasks;
  if (in.storageBlocked) return BlockReason::Storage;
  if (!in.displayHealthy) return BlockReason::Display;
  if (in.sensorUsable && in.temperatureFinite) return BlockReason::None;
  switch (in.sensorBootReason) {
    case 1U: return BlockReason::SensorNoResponse;
    case 2U: return BlockReason::SensorFormat;
    default: return BlockReason::SensorUnstable;
  }
}
// Codes reuse the controller's fault numbers where one exists (E101 sensor lost, E102 invalid, E103 suspect, E301 EEPROM).
inline uint16_t blockCode(BlockReason r) {
  switch (r) {
    case BlockReason::SafetyTrip: return 990U;
    case BlockReason::Tasks: return 991U;
    case BlockReason::Storage: return 301U;
    case BlockReason::Display: return 992U;
    case BlockReason::SensorNoResponse: return 101U;
    case BlockReason::SensorFormat: return 102U;
    case BlockReason::SensorUnstable: return 103U;
    default: return 0U;
  }
}
inline const char *blockText(BlockReason r) {
  switch (r) {
    case BlockReason::SafetyTrip: return "SYSTEM TRIP";
    case BlockReason::Tasks: return "TASK HEARTBEAT";
    case BlockReason::Storage: return "EEPROM";
    case BlockReason::Display: return "LCD";
    case BlockReason::SensorNoResponse: return "SENSOR NO REPLY";
    case BlockReason::SensorFormat: return "SENSOR FORMAT";
    case BlockReason::SensorUnstable: return "SENSOR UNSTABLE";
    default: return "OK";
  }
}
class HomeGate {
 public:
  enum class Phase : uint8_t { Waiting, Ready, Diagnostic };
  void begin(uint32_t now) { start_ = now; phase_ = Phase::Waiting; reason_ = BlockReason::SensorNoResponse; ok_ = Stability(); }
  Phase update(uint32_t now, const LocalInputs &in) {
    reason_ = blockReason(in);
    ok_.update(now, reason_ == BlockReason::None);
    if (phase_ == Phase::Ready) return phase_;           // later sensor loss is an alarm on Home, not a boot problem
    if (ok_.held(now, READY_HOLD_MS)) phase_ = Phase::Ready;
    else if (static_cast<uint32_t>(now - start_) >= BOOT_DEADLINE_MS) phase_ = Phase::Diagnostic;
    return phase_;
  }
  Phase phase() const { return phase_; }
  BlockReason reason() const { return reason_; }
  uint32_t age(uint32_t now) const { return static_cast<uint32_t>(now - start_); }
 private:
  uint32_t start_ = 0U;
  Phase phase_ = Phase::Waiting;
  BlockReason reason_ = BlockReason::SensorNoResponse;
  Stability ok_;
};
}  // namespace MayapBoot
