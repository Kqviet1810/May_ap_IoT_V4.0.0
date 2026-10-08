#pragma once

#include "config.h"
#include "network_io_guard.h"
#include <Arduino.h>
#include <WiFi.h>

#include "protocol_limits.h"
#include "realtime_publish_policy.h"
#include "wifi_power_policy.h"
#include <ArduinoJson.h>
#include <mbedtls/md.h>
#include <time.h>

// Transport-neutral application transaction bridge. Network owners may attach
// a bounded publish callback in a later phase; this baseline attaches none.
namespace MayapRealtimeInternal {

inline uint32_t elapsedMs(uint32_t now, uint32_t then) {
  return static_cast<uint32_t>(now - then);
}
inline bool timeReached(uint32_t now, uint32_t target) {
  return static_cast<int32_t>(now - target) >= 0;
}

constexpr uint8_t REALTIME_REQUEST_ID_CAPACITY = 40U;  // khop firmware/web (xem app.js)

// Mutex duy nhat bao ve toan bo hop thu trao doi giua controlTask (ghi
// mayapRealtimeSet.../mayapRealtimeConfirm...) va realtime owner (doc trong
// mayapRealtimeUpdate). Cac vung critical section o day deu ngan (copy struct/
// vai truong), khong bao gio giu mutex qua mot loi goi I/O.
static portMUX_TYPE realtimeMux = portMUX_INITIALIZER_UNLOCKED;

// --------------------------- Dinh danh thiet bi ------------------------------
// "MAP-" + 12 hex + null = 17 byte toi thieu (khop DEVICE_ID_RE trong app.js).
static char deviceId[20] = "";
static uint32_t bootId = 0;
static char activeOperation[40] = "";
static uint32_t lastDeviceCompletedAt = 0U;
static uint8_t activeAckKey[32] = {};
static bool activeAckKeyValid = false;
inline bool publishAck(const char *, const char *, const char *, const char * = "",
                       uint32_t = 0U, uint32_t = 0U, const uint8_t * = nullptr);

inline void ensureIdentity() {
  if (deviceId[0]) return;
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(deviceId, sizeof(deviceId), "MAP-%02X%02X%02X%02X%02X%02X",
           static_cast<uint8_t>(mac >> 0), static_cast<uint8_t>(mac >> 8),
           static_cast<uint8_t>(mac >> 16), static_cast<uint8_t>(mac >> 24),
           static_cast<uint8_t>(mac >> 32), static_cast<uint8_t>(mac >> 40));
  bootId = esp_random();
  if (bootId == 0U) bootId = 1U;
}

using PublishCallback = bool (*)(const char *, const char *, size_t);
static PublishCallback publishCallback = nullptr;
static MayapRealtimePublish::BootstrapCadence bootstrapCadence;
static uint32_t lastSnapshotPublishAt = 0U;
static bool forceSnapshotPublish = false;

// --------------------------- Hop thu cau hinh/runtime --------------------------
// Ghi boi controlTask qua mayapRealtimeSetConfig/mayapRealtimeSetRuntime; doc boi
// realtime owner. Bao ve boi realtimeMux vi MachineConfig/MachineRuntime khong nho
// (vai chuc/vai tram byte) - copy trong critical section la ngan va an toan.
static MachineConfig knownConfig{};
static bool knownConfigValid = false;
static bool configDirty = false;      // co ban cap nhat can phat "config/reported"
static uint32_t realtimeConfigRevision = 0U;
static char lastVerifiedConfigRequestId[REALTIME_REQUEST_ID_CAPACITY] = "";
static uint32_t lastVerifiedConfigRevision = 0U;

static MachineRuntime knownRuntime{};
static bool knownRuntimeValid = false;

// --------------------- Tuong quan lenh/luu cau hinh voi web --------------------
// pendingCommands/pendingConfigSave duoc GHI boi realtime owner (khi nhan lenh tu
// web va queueCommand()/startConfigSave() thanh cong) va DOC+XOA boi ca hai
// task (realtime owner khi het han, controlTask qua mayapRealtimeConfirmCommand/
// mayapRealtimeConfirmConfigSave khi MachineController xu ly xong) - can realtimeMux.
struct PendingCommand {
  bool used = false;
  bool uncertainSent = false, completed = false, completionOk = false;
  uint32_t completedAt = 0U;
  char completionMessage[64] = "";
  uint32_t commandId = 0;
  uint32_t queuedAt = 0;
  char requestId[REALTIME_REQUEST_ID_CAPACITY] = "";
  char operation[40] = "";
  uint8_t ackKey[32] = {};
  bool signedAck = false;
};
static PendingCommand pendingCommands[COMMAND_QUEUE_SIZE];

// configSave (hmi.h) chi cho phep MOT giao dich luu dang cho tra loi tren toan
// he thong (ca web lan HMI dung chung 1 gate busy) nen khong can luu/doi chieu
// transactionId: pendingConfigSave.used dang bat nghia la giao dich HIEN CO
// chac chan la cua web, vi HMI khong the mo giao dich thu hai cung luc.
struct PendingConfigSave {
  bool used = false;
  bool uncertainSent = false, completed = false, completionOk = false;
  uint32_t completedAt = 0U;
  char completionMessage[64] = "";
  uint32_t transactionId = 0U;
  uint32_t queuedAt = 0;
  uint32_t revision = 0;
  char requestId[REALTIME_REQUEST_ID_CAPACITY] = "";
  uint8_t ackKey[32] = {};
  bool signedAck = false;
};
static PendingConfigSave pendingConfigSave;

// ------------------------------- Hop thu phat ACK -------------------------------
// mayapRealtimeConfirmCommand/mayapRealtimeConfirmConfigSave chay tren controlTask va
// KHONG duoc goi thang vao network transport (I/O mang) - chung chi day ket qua vao
// day, realtime owner se rut ra va publish that su.
struct AckOutboxItem {
  bool used = false;
  char requestId[REALTIME_REQUEST_ID_CAPACITY] = "";
  char result[16] = "";
  char message[64] = "";
  char operation[40] = "";
  uint32_t receivedAt = 0U;
  uint32_t completedAt = 0U;
  uint8_t ackKey[32] = {};
  bool signedAck = false;
};
static AckOutboxItem ackOutbox[COMMAND_QUEUE_SIZE + 2U];

inline bool enqueueAckLocked(const char *requestId, const char *result,
                             const char *message, const char *operation = "",
                             uint32_t receivedAt = 0U, const uint8_t *ackKey = nullptr,
                             uint32_t completedAt = 0U) {
  if (!requestId || !requestId[0]) return false;
  for (AckOutboxItem &slot : ackOutbox) {
    if (slot.used) continue;
    slot.used = true;
    snprintf(slot.requestId, sizeof(slot.requestId), "%s", requestId);
    snprintf(slot.result, sizeof(slot.result), "%s", result ? result : "");
    snprintf(slot.message, sizeof(slot.message), "%s", message ? message : "");
    snprintf(slot.operation, sizeof(slot.operation), "%s", operation ? operation : "");
    slot.receivedAt = receivedAt;
    slot.completedAt = completedAt ? completedAt : millis();
    slot.signedAck = ackKey != nullptr;
    if (ackKey) memcpy(slot.ackKey, ackKey, sizeof(slot.ackKey));
    return true;
  }
  return false; // Caller keeps the completed result in its bounded pending slot.
}

// -------------------------- Hop thu nhat ky (event log) -------------------------
// mayapRealtimePushEventLog() chay tren controlTask; chi sao chep snapshot vao day,
// realtime owner moi thuc su lap va publish tung muc (co I/O mang).
static HmiEventSnapshot pendingEventSnapshot{};
static bool eventSnapshotDirty = false;
static uint32_t lastPublishedEventSequence = 0U;

// --------------------- Lich su nhiet do AT24C32 -> Web ----------------------
// Chi doc EEPROM khi web yeu cau; moi vong realtime owner chi phat toi da 12
// bucket de khong chiem I2C/realtime lau. Request duoc HMAC giong command/config.
static bool historyResponsePending = false;
static uint16_t historyWindowMinutes = 30U;
static uint16_t historyCursor = 0U;
static uint16_t historyCandidateCount = 0U;
static uint32_t historySnapshotEpoch = 0U;
static char historyRequestId[REALTIME_REQUEST_ID_CAPACITY] = "";
static uint8_t historyAckKey[32] = {};
static bool historySignedAck = false;
static bool historyReadError = false;
static uint16_t historySampleCount = 0U;

// -------------------------------- Publish -------------------------------------
// Tat ca ham publishXxx() ben duoi chi duoc goi tu realtime owner.
inline bool publishJson(const char *channel, const JsonDocument &doc, bool /* cacheHint */) {
  if (!publishCallback || doc.overflowed()) return false;
  char buffer[MayapProtocol::FRAME_NORMAL_CAP];
  const size_t length = serializeJson(doc, buffer, sizeof(buffer));
  return length > 0U && length < sizeof(buffer) && publishCallback(channel, buffer, length);
}

inline void handleHistoryRequestMessage(const JsonDocument &doc) {
  const char *requestId = doc["requestId"] | "";
  if (!requestId[0]) return;
  if (historyResponsePending) {
    publishAck(requestId, "busy", "HISTORY_BUSY");
    return;
  }
  uint16_t minutes = static_cast<uint16_t>(doc["minutes"] | 30U);
  if (minutes < 5U) minutes = 5U;
  if (minutes > 1440U) minutes = 1440U;

  const uint32_t epoch = mayapTemperatureHistoryLatestEpoch();
  const uint32_t interval = TEMP_HISTORY_SAMPLE_SEC;
  uint32_t candidates = (static_cast<uint32_t>(minutes) * 60UL + interval - 1UL) / interval + 1UL;
  if (candidates > TEMP_HISTORY_SLOT_COUNT) candidates = TEMP_HISTORY_SLOT_COUNT;

  historyWindowMinutes = minutes;
  historyCursor = 0U;
  historyCandidateCount = static_cast<uint16_t>(candidates);
  historySnapshotEpoch = epoch;
  snprintf(historyRequestId, sizeof(historyRequestId), "%s", requestId);
  historyResponsePending = true;
  historySignedAck = activeAckKeyValid;
  if (historySignedAck) memcpy(historyAckKey, activeAckKey, sizeof(historyAckKey));
  historyReadError = false;
  historySampleCount = 0U;
  publishAck(requestId, "accepted", "HISTORY_ACCEPTED");
}

inline void serviceHistoryResponse() {
  if (!historyResponsePending || !publishCallback) return;

  JsonDocument doc;
  doc["v"] = 1;
  doc["bootId"] = bootId;
  doc["requestId"] = historyRequestId;
  doc["windowMin"] = historyWindowMinutes;
  doc["intervalSec"] = TEMP_HISTORY_SAMPLE_SEC;
  doc["cursor"] = historyCursor;
  JsonArray samples = doc["samples"].to<JsonArray>();

  if (historySnapshotEpoch == 0U || historyCandidateCount == 0U) {
    doc["done"] = true;
    if (!publishJson("history/reported", doc, false)) return;
    historyResponsePending = false;
    publishAck(historyRequestId, "applied", "HISTORY_EMPTY", "history.read",
               0U, 0U, historySignedAck ? historyAckKey : nullptr);
    return;
  }

  const uint32_t nowBucket = historySnapshotEpoch / TEMP_HISTORY_SAMPLE_SEC;
  const uint32_t firstBucket = nowBucket >= historyCandidateCount - 1U
      ? nowBucket - (historyCandidateCount - 1U) : 0U;
  const uint16_t end = static_cast<uint16_t>(
      min<uint32_t>(historyCandidateCount, static_cast<uint32_t>(historyCursor) + 12U));

  uint16_t chunkSamples = 0U;
  bool chunkReadError = false;
  for (uint16_t i = historyCursor; i < end; ++i) {
    const uint32_t absoluteBucket = firstBucket + i;
    MayapTemperatureHistoryPoint point{};
    const uint8_t status = mayapTemperatureHistoryReadStatus(absoluteBucket, point);
    if (status == 0U) { chunkReadError = true; continue; }
    if (status != 2U) continue;
    ++chunkSamples;
    JsonArray row = samples.add<JsonArray>();
    row.add(point.epoch);
    row.add(static_cast<float>(point.temperatureX10) / 10.0f);
  }

  const bool done = end >= historyCandidateCount;
  doc["done"] = done;
  if (!publishJson("history/reported", doc, false)) return;
  historyCursor = end;
  historySampleCount += chunkSamples;
  historyReadError = historyReadError || chunkReadError;
  if (done) {
    historyResponsePending = false;
    publishAck(historyRequestId, historyReadError ? "rejected" : "applied",
               historyReadError ? "HISTORY_EEPROM_ERROR" :
               (historySampleCount ? "HISTORY_DONE" : "HISTORY_EMPTY"), "history.read",
               0U, 0U, historySignedAck ? historyAckKey : nullptr);
  }
}

inline bool publishPresence(bool online) {
  JsonDocument doc;
  doc["online"] = online;
  doc["bootId"] = bootId;
  const NetworkStatus status = mayapGetNetworkStatus();
  doc["ip"] = status.connected ? mayapNetworkLocalIp().toString() : "";
  doc["rssi"] = status.connected ? status.rssiDbm : 0;
  doc["fw"] = MAYAP_FIRMWARE_VERSION;
  doc["firmware"] = MAYAP_FIRMWARE_VERSION;
  doc["proto"] = 2;
  doc["maxPacket"] = MayapProtocol::FRAME_NORMAL_CAP;
  JsonArray caps = doc["caps"].to<JsonArray>();
  caps.add("transactions"); caps.add("config.patch");
  caps.add("control.session"); caps.add("history.chunk");
  doc["hw"] = MAYAP_HARDWARE_REVISION;
  return publishJson("presence", doc, true);
}

inline bool publishConfigReport(const MachineConfig &cfg, uint32_t revision) {
  char verifiedId[REALTIME_REQUEST_ID_CAPACITY] = "";
  portENTER_CRITICAL(&realtimeMux);
  if (revision == lastVerifiedConfigRevision)
    snprintf(verifiedId, sizeof(verifiedId), "%s", lastVerifiedConfigRequestId);
  portEXIT_CRITICAL(&realtimeMux);
  JsonDocument doc;
  doc["v"] = 1;
  doc["bootId"] = bootId;
  doc["revision"] = revision;
  JsonObject c = doc["config"].to<JsonObject>();
  c["targetTemp"] = cfg.targetTemp;
  c["tempHysteresis"] = cfg.tempHysteresis;
  c["lowTempAlarm"] = cfg.lowTempAlarm;
  c["highTempAlarm"] = cfg.highTempAlarm;
  c["emergencyTemp"] = cfg.emergencyTemp;
  c["kp"] = cfg.kp;
  c["ki"] = cfg.ki;
  c["kd"] = cfg.kd;
  c["lowHumidityAlarm"] = cfg.lowHumidityAlarm;
  c["humidifierInstalled"] = cfg.humidifierInstalled;
  c["humidifierEnabled"] = cfg.humidifierEnabled;
  c["targetHumidity"] = cfg.targetHumidity;
  c["humidifierHysteresisRh"] = cfg.humidifierHysteresisRh;
  c["ventOnTemp"] = cfg.ventOnTemp;
  c["ventOffTemp"] = cfg.ventOffTemp;
  c["ventScheduleEnabled"] = cfg.ventScheduleEnabled;
  c["ventScheduleCount"] = cfg.ventScheduleCount;
  c["ventScheduleDurationMin"] = cfg.ventScheduleDurationMin;
  c["ventScheduleHour1"] = cfg.ventScheduleHour1;
  c["ventScheduleHour2"] = cfg.ventScheduleHour2;
  c["ventScheduleHour3"] = cfg.ventScheduleHour3;
  c["ventScheduleHour4"] = cfg.ventScheduleHour4;
  c["ventScheduleHour5"] = cfg.ventScheduleHour5;
  c["ventScheduleHour6"] = cfg.ventScheduleHour6;
  c["ventAutoEnabled"] = cfg.ventAutoEnabled;
  c["ventProfileLevel"] = cfg.ventProfileLevel;
  c["ventCycleMinutes"] = cfg.ventCycleMinutes;
  c["ventDutyDay1To3"] = cfg.ventDutyDay1To3;
  c["ventDutyDay4To7"] = cfg.ventDutyDay4To7;
  c["ventDutyDay8To11"] = cfg.ventDutyDay8To11;
  c["ventDutyDay12To15"] = cfg.ventDutyDay12To15;
  c["ventDutyDay16To18"] = cfg.ventDutyDay16To18;
  c["ventDutyDay19To21"] = cfg.ventDutyDay19To21;
  c["tempOffset"] = cfg.tempOffset;
  c["humidityOffset"] = cfg.humidityOffset;
  c["pidCycleSec"] = cfg.pidCycleSec;
  c["humidityAlarmDelaySec"] = cfg.humidityAlarmDelaySec;
  c["turnIntervalMin"] = cfg.turnIntervalMin;
  c["turnMaxRunSec"] = cfg.turnMaxRunSec;
  c["powerRestoreDelaySec"] = cfg.powerRestoreDelaySec;
  c["sensorTimeoutSec"] = cfg.sensorTimeoutSec;
  c["maxHeaterPower"] = cfg.maxHeaterPower;
  c["adaptiveThermalBalanceEnabled"] = cfg.adaptiveThermalBalanceEnabled;
  c["totalIncubationDays"] = cfg.totalIncubationDays;
  c["circulationFanEnabled"] = cfg.circulationFanEnabled;
  c["turningEnabled"] = cfg.turningEnabled;
  c["manualTurnReanchorsSchedule"] = cfg.manualTurnReanchorsSchedule;
  c["sirenSelfTestEnabled"] = cfg.sirenSelfTestEnabled;
  // Ten field khac firmware (autoResumeOnPowerLoss) vi web da dung ten nay
  // truoc: giu nguyen giao thuc web, chi anh xa ten trong firmware.
  c["autoResumeAfterPower"] = cfg.autoResumeOnPowerLoss;
  c["allowHeatWithoutBatch"] = cfg.allowHeatWithoutBatch;
  c["alarmEnabled"] = cfg.alarmEnabled;
  c["lightAfterBatchAlarmEnabled"] = cfg.lightAfterBatchAlarmEnabled;
  c["highTempAlarmWithoutBatch"] = cfg.highTempAlarmWithoutBatch;
  c["controlMode"] = static_cast<uint8_t>(cfg.controlMode);
  c["nextDirection"] = static_cast<uint8_t>(cfg.nextDirection);
  // 8 truong "Nang cao" (schema 8, xem config.h) - THIEU o day tu luc them
  // tinh nang "Nang cao" la LOI GOC gay web KHONG BAO GIO dong bo duoc: web
  // (CONFIG_KEYS trong app.js) doi hoi DU CA 38 truong moi coi 1 goi config/
  // reported la hop le (validateFullConfig), thieu dung 8 truong nay khien
  // MOI lan bao cau hinh tu ESP32 bi web tu choi vinh vien - khong lien quan
  // gi den mang/broker, day la loi giao thuc that su.
  c["heaterStuckMinRiseC"] = cfg.heaterStuckMinRiseC;
  c["heaterStuckDurationSec"] = cfg.heaterStuckDurationSec;
  c["tempRateLimitC"] = cfg.tempRateLimitC;
  c["tempRateWindowSec"] = cfg.tempRateWindowSec;
  c["tempOscillationCrossLimit"] = cfg.tempOscillationCrossLimit;
  c["tempOscillationWindowSec"] = cfg.tempOscillationWindowSec;
  c["autotuneRelayPowerPercent"] = cfg.autotuneRelayPowerPercent;
  c["autotuneBandC"] = cfg.autotuneBandC;
  if (doc.overflowed()) return false;
  // Stream a full report in bounded chunks. Never retain a partial config.
  JsonDocument chunk;
  uint8_t part = 0U;
  auto beginChunk = [&]() {
    chunk.clear();
    chunk["v"] = 2;
    chunk["bootId"] = bootId;
    chunk["revision"] = revision;
    if (verifiedId[0]) chunk["requestId"] = verifiedId;
    chunk["part"] = part;
    chunk["done"] = false;
    chunk["config"].to<JsonObject>();
  };
  beginChunk();
  for (JsonPair field : c) {
    const char *key = field.key().c_str();
    chunk["config"][key] = field.value();
    if (measureJson(chunk) > 850U) {
      chunk["config"].as<JsonObject>().remove(key);
      if (!publishJson("config/reported", chunk, false)) return false;
      ++part;
      beginChunk();
      chunk["config"][key] = field.value();
    }
  }
  chunk["done"] = true;
  return publishJson("config/reported", chunk, false);
}


inline bool publishSnapshot(const MachineRuntime &rt, uint32_t revision) {
  JsonDocument doc;
  doc["bootId"] = bootId;
  doc["revision"] = revision;
  JsonObject r = doc["runtime"].to<JsonObject>();
  r["temperature"] = rt.temperature;
  r["humidity"] = rt.humidity;
  r["machineState"] = rt.machineState;
  r["batchRunning"] = rt.batchRunning;
  r["currentDay"] = rt.currentDay;
  r["heaterOn"] = rt.heaterOn;
  r["heaterPower"] = rt.heaterPower;
  r["circulationFanOn"] = rt.circulationFanOn;
  r["ventFanOn"] = rt.ventFanOn;
  r["humidifierOn"] = rt.humidifierOn;
  r["lightOn"] = rt.lightOn;
  r["sirenOn"] = rt.sirenOn;
  r["turnState"] = static_cast<uint8_t>(rt.turnState);
  r["nextTurnMinutes"] = rt.nextTurnMinutes;
  r["autoTuneState"] = static_cast<uint8_t>(rt.autoTuneState);
  r["autoTuneProgress"] = rt.autoTuneProgress;
  JsonObject adapt=r["adaptiveThermal"].to<JsonObject>();
  adapt["enabled"]=rt.adaptiveEnabled;adapt["state"]=rt.adaptiveState;
  adapt["confidence"]=rt.adaptiveConfidence;adapt["loadIndex"]=rt.adaptiveLoadIndex;
  adapt["coastRiseC"]=rt.adaptiveCoastRiseC;adapt["coastTimeSec"]=rt.adaptiveCoastTimeSec;
  adapt["holdPowerPct"]=rt.adaptiveHoldPowerPct;adapt["effectiveMaxPowerPct"]=rt.effectiveMaxPowerPct;
  adapt["approachBandC"]=rt.adaptiveApproachBandC;adapt["selfHeating"]=rt.adaptiveSelfHeating;
  adapt["coolingDemand"]=rt.adaptiveCoolingDemand;adapt["validWindows"]=rt.observerValidWindows;
  adapt["reason"]=rt.lastAdaptiveReason;
  r["resumeConfirmationRequired"] = rt.resumeConfirmationRequired;
  r["batchOverdueConfirmationPending"] = rt.batchOverdueConfirmationPending;
  // Danh sach loi dang active, da sap xep theo displayPriority giam dan boi
  // FaultManager::copyActiveForHmi() - phan tu [0] la loi quan trong nhat.
  // Web dung de to mau o Trang thai + hien popup chi tiet khi bam vao.
  JsonArray faults = r["activeFaults"].to<JsonArray>();
  for (uint8_t i = 0U; i < rt.activeFaultDisplayCount; ++i) {
    JsonObject f = faults.add<JsonObject>();
    f["code"] = rt.activeFaults[i].code;
    f["severity"] = rt.activeFaults[i].severity;
  }
  return publishJson("snapshot", doc, false);
}

// Compact retained hints never contain config or authorize a command.
inline bool publishBootstrap(const MachineRuntime &rt, uint32_t revision) {
  JsonDocument doc;
  doc["v"] = 1;
  doc["proto"] = 2;
  doc["fw"] = MAYAP_FIRMWARE_VERSION;
  doc["bootId"] = bootId;
  doc["revision"] = revision;
  const time_t epoch = time(nullptr);
  doc["publishedAt"] = epoch > 1700000000 ? static_cast<uint32_t>(epoch) : 0U;
  doc["temperature"] = rt.temperature;
  doc["humidity"] = rt.humidity;
  doc["machineState"] = rt.machineState;
  doc["batchRunning"] = rt.batchRunning;
  doc["heaterOn"] = rt.heaterOn;
  doc["circulationFanOn"] = rt.circulationFanOn;
  doc["ventFanOn"] = rt.ventFanOn;
  doc["humidifierOn"] = rt.humidifierOn;
  doc["lightOn"] = rt.lightOn;
  doc["sirenOn"] = rt.sirenOn;
  portENTER_CRITICAL(&realtimeMux);
  const bool humidifierInstalled = knownConfigValid && knownConfig.humidifierInstalled;
  portEXIT_CRITICAL(&realtimeMux);
  doc["humidifierInstalled"] = humidifierInstalled;
  doc["alarmMask"] = rt.alarmMask;
  doc["faultCode"] = rt.primaryFaultCode;
  doc["faultCount"] = rt.activeFaultCount;
  doc["faultSeverity"] = rt.activeFaultDisplayCount ? rt.activeFaults[0].severity : 0U;
  // Include the channel envelope and RFC6455 framing in the small-packet budget.
  if (measureJson(doc) + 64U >
      MayapRealtimePublish::BOOTSTRAP_PACKET_BUDGET) return false;
  return publishJson("bootstrap", doc, true);
}

struct TerminalResult {
  bool used = false;
  char requestId[REALTIME_REQUEST_ID_CAPACITY] = "";
  char operation[40] = "";
  char result[16] = "";
  char message[64] = "";
  uint8_t ackKey[32] = {};
  bool signedAck = false;
};
static TerminalResult terminalCache[16];
static uint8_t terminalCursor = 0;
inline bool replayTerminal(const char *id) {
  if (!id || !id[0]) return false;
  for (const auto &item : terminalCache) {
    if (!item.used || strcmp(item.requestId, id)) continue;
    // Replayed terminal result never executes the controller again.
    publishAck(item.requestId, item.result, item.message, item.operation,
               0U, 0U, item.signedAck ? item.ackKey : nullptr);
    return true;
  }
  return false;
}

inline const char *ackCode(const char *result, const char *message) {
  if (message && !strncmp(message, "HISTORY_", 8U)) return message;
  if (message && !strncmp(message, "CONFIG_", 7U)) return message;
  if (!strcmp(result, "applied")) return "APPLIED";
  if (!strcmp(result, "accepted")) return "RECEIVED";
  if (!strcmp(result, "unauthorized")) return "AUTH_ERROR";
  if (!strcmp(result, "stale")) return "STALE_REQUEST";
  if (!strcmp(result, "busy")) return "CONTROLLER_BUSY";
  if (!strcmp(result, "expired")) return "CONTROLLER_TIMEOUT";
  if (!strcmp(result, "invalid")) return "INVALID_REQUEST";
  if (!strcmp(result, "unsupported")) return "UNSUPPORTED_OPERATION";
  struct Reason { const char *raw; const char *code; };
  static constexpr Reason reasons[] = {
    {"HAY CHUYEN SANG AUTO", "BATCH_AUTO_OFF"},
    {"HAY BAT CONG TAC NHIET", "BATCH_HEATER_SWITCH_OFF"},
    {"CAM BIEN CHUA SAN SANG", "BATCH_SENSOR_ERROR"},
    {"RTC CHUA HOP LE", "BATCH_RTC_INVALID"},
    {"LOI 2 HANH TRINH", "BATCH_LIMIT_SWITCH_FAULT"},
    {"DANG CO LOI DAO", "BATCH_TURNING_FAULT"},
    {"NHIET DANG QUA CAO", "BATCH_OVERHEAT"},
    {"DANG QUA NHIET KHAN CAP", "BATCH_EMERGENCY_OVERHEAT"},
    {"ME DANG CHAY", "BATCH_ALREADY_RUNNING"},
    {"DUNG ME CU TRUOC", "BATCH_ALREADY_RUNNING"},
    {"DANG XOA DU LIEU ME CU", "BATCH_STORAGE_BUSY"},
    {"LOI LUU TRANG THAI ME", "BATCH_EEPROM_ERROR"},
    {"LOI BO NHO CAU HINH", "CONFIG_EEPROM_ERROR"},
    {"LUU CAU HINH BI TU CHOI", "CONFIG_SAVE_REJECTED"},
    {"KHONG CO ME DANG CHAY", "BATCH_NOT_RUNNING"},
    {"COI KHAN CAP CAN ACK TAI MAY", "ALARM_PHYSICAL_ACK_REQUIRED"},
    {"LOI DAO CAN ACK TAI MAY", "TURN_PHYSICAL_ACK_REQUIRED"},
    {"HAY BAT TU DONG DAO", "BATCH_TURNING_DISABLED"},
    {"HAY XAC NHAN RESET LOI", "BATCH_RESET_ACK_REQUIRED"},
    {"LOI NHAT KY AN TOAN", "BATCH_SAFETY_JOURNAL_ERROR"}
  };
  for (const Reason &reason : reasons) if (!strcmp(message, reason.raw)) return reason.code;
  return "CONTROLLER_REJECTED";
}

inline const char *ackFriendlyMessage(const char *code, const char *raw) {
  if (raw && !strcmp(raw, "SESSION_EXPIRED"))
    return "Phiên điều khiển đã hết hạn; hãy thử lại";
  if (raw && !strcmp(raw, "INVALID_CONFIG_PATCH"))
    return "Thông số cấu hình không hợp lệ";
  if (raw && !strcmp(raw, "INVALID_FULL_CONFIG"))
    return "Cấu hình tổng thể không hợp lệ";
  if (raw && !strcmp(raw, "REPLAY SEQUENCE"))
    return "Yêu cầu cũ đã được gửi trước đó";
  if (raw && !strcmp(raw, "STALE_BOOT"))
    return "Máy vừa khởi động lại; hãy đồng bộ rồi thử lại";
  struct Text { const char *code; const char *message; };
  static constexpr Text texts[] = {
    {"BATCH_AUTO_OFF", "Hãy chuyển công tắc sang AUTO trước"},
    {"BATCH_HEATER_SWITCH_OFF", "Hãy bật công tắc thanh nhiệt trước"},
    {"BATCH_SENSOR_ERROR", "Cảm biến chưa sẵn sàng"},
    {"BATCH_RTC_INVALID", "Đồng hồ RTC chưa hợp lệ"},
    {"BATCH_LIMIT_SWITCH_FAULT", "Lỗi hai công tắc hành trình"},
    {"BATCH_TURNING_FAULT", "Cơ cấu đảo trứng đang lỗi"},
    {"BATCH_OVERHEAT", "Nhiệt độ đang quá cao"},
    {"BATCH_EMERGENCY_OVERHEAT", "Đang quá nhiệt khẩn cấp"},
    {"BATCH_ALREADY_RUNNING", "Mẻ ấp đang chạy"},
    {"BATCH_EEPROM_ERROR", "Không lưu được trạng thái mẻ"},
    {"CONFIG_EEPROM_ERROR", "Không ghi/đọc lại được EEPROM cấu hình"},
    {"CONFIG_BATCH_LOCKED", "Thông số này bị khóa khi mẻ đang chạy"},
    {"CONFIG_SAFETY_BLOCK", "Máy đang có lỗi an toàn; chưa thể lưu"},
    {"HISTORY_EEPROM_ERROR", "Không đọc được EEPROM lịch sử"},
    {"HISTORY_EMPTY", "EEPROM chưa có lịch sử nhiệt"},
    {"HISTORY_DONE", "Đã đọc xong lịch sử nhiệt"},
    {"ALARM_PHYSICAL_ACK_REQUIRED", "Cần xác nhận còi khẩn cấp tại máy"},
    {"TURN_PHYSICAL_ACK_REQUIRED", "Cần xác nhận lỗi đảo tại máy"},
    {"UNSUPPORTED_OPERATION", "Firmware chưa hỗ trợ thao tác này"},
  };
  for (const Text &text : texts) if (!strcmp(code, text.code)) return text.message;
  return raw && raw[0] ? raw : (!strcmp(code, "APPLIED") ? "Máy đã thực hiện" :
      !strcmp(code, "RECEIVED") ? "Máy đã nhận yêu cầu" : "Máy từ chối yêu cầu");
}

inline bool publishAck(const char *requestId, const char *result,
                       const char *message, const char *operation,
                       uint32_t receivedAt, uint32_t completedAt,
                       const uint8_t *ackKey) {
  if (!requestId || !requestId[0]) return false;
  const char *op = operation && operation[0] ? operation : activeOperation;
  const bool received = !strcmp(result, "accepted");
  const bool uncertain = !strcmp(result, "expired");
  const bool ok = !strcmp(result, "applied");
  const uint8_t *key = ackKey ? ackKey : (activeAckKeyValid ? activeAckKey : nullptr);
  if (!received && !uncertain) {
    TerminalResult *existing = nullptr;
    for (auto &item : terminalCache) if (item.used && !strcmp(item.requestId, requestId)) { existing = &item; break; }
    TerminalResult slot{}; // Separate copy avoids aliasing replayTerminal() input.
    slot.used = true;
    snprintf(slot.requestId, sizeof(slot.requestId), "%s", requestId);
    snprintf(slot.operation, sizeof(slot.operation), "%s", op);
    snprintf(slot.result, sizeof(slot.result), "%s", result);
    snprintf(slot.message, sizeof(slot.message), "%s", message ? message : "");
    slot.signedAck = key != nullptr;
    if (key) memcpy(slot.ackKey, key, sizeof(slot.ackKey));
    (existing ? *existing : terminalCache[terminalCursor++ % 16U]) = slot;
    lastSnapshotPublishAt = 0U;
    forceSnapshotPublish = true;
    lastDeviceCompletedAt = millis();
  }
  JsonDocument doc;
  doc["v"] = 2;
  doc["requestId"] = requestId;
  doc["operation"] = op;
  doc["phase"] = received ? "received" : uncertain ? "uncertain" : "completed";
  doc["ok"] = ok;
  const char *code = ackCode(result, message ? message : "");
  doc["code"] = code;
  doc["bootId"] = bootId;
  doc["result"] = result;
  doc["message"] = ackFriendlyMessage(code, message);
  doc["revision"] = realtimeConfigRevision;
  doc["tDeviceReceived"] = receivedAt ? receivedAt : millis();
  doc["tDeviceCompleted"] = completedAt ? completedAt : lastDeviceCompletedAt;
  if (key) {
    const char *friendly = doc["message"] | "";
    const char *phase = doc["phase"] | "";
    const uint32_t revision = doc["revision"] | 0UL;
    char signedText[512];
    const int n = snprintf(signedText, sizeof(signedText),
        "mayap-mqtt-ack:v2\n%s\n%s\n%s\n%s\n%d\n%s\n%lu\n%lu\n%s",
        deviceId, requestId, op, phase, ok ? 1 : 0, code,
        static_cast<unsigned long>(bootId), static_cast<unsigned long>(revision), friendly);
    uint8_t digest[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info || n < 0 || static_cast<size_t>(n) >= sizeof(signedText) ||
        mbedtls_md_hmac(info, key, 32,
            reinterpret_cast<const uint8_t *>(signedText), n, digest) != 0) return false;
    char hex[65];
    static constexpr char alphabet[] = "0123456789abcdef";
    for (uint8_t i = 0; i < 32U; ++i) {
      hex[i * 2U] = alphabet[digest[i] >> 4U];
      hex[i * 2U + 1U] = alphabet[digest[i] & 15U];
    }
    hex[64] = '\0';
    doc["sig"] = hex;
  }
  return publishJson("ack", doc, false);
}

inline bool publishLogEntry(const HmiEventItem &item) {
  JsonDocument doc;
  doc["sequence"] = item.sequence;
  doc["epoch"] = item.epoch;
  doc["code"] = item.code;
  doc["value"] = item.value;
  doc["type"] = item.type;
  return publishJson("log", doc, false);
}

// --------------------------- Xu ly ban tin den (realtime owner) --------------------
inline HmiCommandType mapCommandAction(const char *action) {
  if (!action) return HmiCommandType::None;
  if (!strcmp(action, "batch_start")) return HmiCommandType::BatchStart;
  if (!strcmp(action, "batch_stop")) return HmiCommandType::BatchStop;
  if (!strcmp(action, "resume_yes")) return HmiCommandType::ResumeYes;
  if (!strcmp(action, "resume_no")) return HmiCommandType::ResumeNo;
  if (!strcmp(action, "autotune_start")) return HmiCommandType::AutoTuneStart;
  if (!strcmp(action, "autotune_cancel")) return HmiCommandType::AutoTuneCancel;
  // Nut "Cap nhat" tren web CHI yeu cau may kiem tra ngay (bo qua nhip 6h),
  // KHONG tu tai ve/nap - van phai xac nhan vat ly tren HMI (xem ota_web_
  // update.h + hmi.h::openFirmwareWebConfirm()).
  if (!strcmp(action, "firmware_check_now")) return HmiCommandType::FirmwareWebCheckNow;
  // Quay lai firmware truoc do (xem ota_rollback.h) - KHAC voi cap nhat
  // (chi "kiem tra", phai xac nhan vat ly tren HMI), lenh nay ap dung
  // NGAY qua web vi ban chat la doi ve firmware DA TUNG chay on dinh
  // truoc do (khong phai 1 ban hoan toan moi/chua kiem chung), va nguoi
  // dung can quay lai duoc TU XA dung luc may dang gap loi sau khi cap
  // nhat, khong phai luc nao cung o canh may that.
  if (!strcmp(action, "firmware_rollback")) return HmiCommandType::FirmwareRollback;
  // Nut den/coi tren dashboard (outputStrip) - xem handleCommandMessage() ve
  // cach truyen alarmMask rieng cho alarm_ack (LightToggle khong can tham so).
  if (!strcmp(action, "light_toggle")) return HmiCommandType::LightToggle;
  if (!strcmp(action, "alarm_ack")) return HmiCommandType::AlarmAck;
  // F-06: xac nhan "tiep tuc u am" khi me qua han ngay du kien - khong bi
  // han che nguon (F-09) vi day la quyet dinh van hanh, khong phai su co
  // can kiem tra vat ly.
  if (!strcmp(action, "batch_overdue_continue")) return HmiCommandType::BatchOverdueContinue;
  return HmiCommandType::None;
}

static uint32_t lastCommandSequence = 0U;
static char lastCommandRequestId[REALTIME_REQUEST_ID_CAPACITY] = "";

inline bool realtimeCommandChannelTrusted() {
  return mayapCommandKey() && strlen(mayapCommandKey()) == 64U;
}

inline int realtimeHexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline bool realtimeDecodeHex32(const char *text, uint8_t out[32]) {
  if (!text || strlen(text) != 64U) return false;
  for (size_t i = 0; i < 32U; ++i) {
    const int hi = realtimeHexNibble(text[i * 2U]);
    const int lo = realtimeHexNibble(text[i * 2U + 1U]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

struct ClientReplayWindow {
  char id[40] = "";
  uint64_t lastSeq = 0;
  uint32_t expiresAt = 0;
};
static ClientReplayWindow replayWindows[8];

inline bool realtimeVerifyV2(const char *channel, const JsonDocument &wire,
                         JsonDocument &bodyDoc, bool &expired) {
  expired = false;
  const char *grant = wire["grant"] | "";
  const char *grantSig = wire["grantSig"] | "";
  const char *body = wire["body"] | "";
  const char *signature = wire["sig"] | "";
  if (!channel || strlen(grant) > 96U || strlen(body) >= MayapProtocol::FRAME_SIGNED_BODY_CAP ||
      !grant[0] || !body[0]) return false;
  char clientId[40] = "";
  unsigned long expiry = 0;
  char grantNonce[25] = "";
  int consumed = 0;
  if (sscanf(grant, "%39[A-Za-z0-9_-]|%lu|%24[0-9a-f]%n",
             clientId, &expiry, grantNonce, &consumed) != 3 ||
      static_cast<size_t>(consumed) != strlen(grant) ||
      strlen(clientId) < 8U || strlen(grantNonce) != 24U) return false;
  const time_t now = time(nullptr);
  uint8_t key[32], suppliedGrant[32], suppliedBody[32], actual[32], sessionKey[32];
  if (!realtimeDecodeHex32(mayapCommandKey(), key) ||
      !realtimeDecodeHex32(grantSig, suppliedGrant) ||
      !realtimeDecodeHex32(signature, suppliedBody)) return false;
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;
  char header[220];
  int len = snprintf(header, sizeof(header), "mayap-control-grant:v2\n%s\n%s",
                     deviceId, grant);
  if (len < 0 || static_cast<size_t>(len) >= sizeof(header) ||
      mbedtls_md_hmac(info, key, 32, reinterpret_cast<const uint8_t *>(header),
                      len, actual) != 0) return false;
  uint8_t diff = 0U;
  for (size_t i = 0; i < 32; ++i) diff |= actual[i] ^ suppliedGrant[i];
  if (diff) return false;
  len = snprintf(header, sizeof(header), "mayap-control-session:v2\n%s\n%s",
                 deviceId, grant);
  if (len < 0 || static_cast<size_t>(len) >= sizeof(header) ||
      mbedtls_md_hmac(info, key, 32, reinterpret_cast<const uint8_t *>(header),
                      len, sessionKey) != 0) return false;
  len = snprintf(header, sizeof(header), "mayap-mqtt-write:v2\n%s\n%s\n%s\n",
                 deviceId, channel, grant);
  if (len < 0 || static_cast<size_t>(len) >= sizeof(header)) return false;
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  bool ok = mbedtls_md_setup(&ctx, info, 1) == 0 &&
      mbedtls_md_hmac_starts(&ctx, sessionKey, 32) == 0 &&
      mbedtls_md_hmac_update(&ctx, reinterpret_cast<const uint8_t *>(header), len) == 0 &&
      mbedtls_md_hmac_update(&ctx, reinterpret_cast<const uint8_t *>(body), strlen(body)) == 0 &&
      mbedtls_md_hmac_finish(&ctx, actual) == 0;
  mbedtls_md_free(&ctx);
  if (!ok) return false;
  diff = 0U;
  for (size_t i = 0; i < 32; ++i) diff |= actual[i] ^ suppliedBody[i];
  if (diff || deserializeJson(bodyDoc, body) != DeserializationError::Ok) return false;
  memcpy(activeAckKey, sessionKey, sizeof(activeAckKey));
  activeAckKeyValid = true;
  const bool validBody = bodyDoc["v"].as<int>() == 2 &&
      !strcmp(bodyDoc["clientId"] | "", clientId) &&
      strlen(bodyDoc["requestId"] | "") > 0U &&
      strlen(bodyDoc["nonce"] | "") >= 16U &&
      bodyDoc["seq"].as<uint64_t>() > 0U;
  if (!validBody) return false;
  expired = now < 1700000000 || expiry < static_cast<unsigned long>(now) ||
      expiry > static_cast<unsigned long>(now) + 300UL;
  return !expired;
}

inline bool checkReplaySequence(const JsonDocument &doc) {
  const char *client = doc["clientId"] | "";
  const uint64_t seq = doc["seq"].as<uint64_t>();
  const uint32_t now = millis();
  ClientReplayWindow *slot = nullptr;
  for (auto &candidate : replayWindows) {
    if (!strcmp(candidate.id, client)) { slot = &candidate; break; }
  }
  if (!slot) for (auto &candidate : replayWindows) {
    if (!candidate.id[0] || timeReached(now, candidate.expiresAt)) {
      slot = &candidate; break;
    }
  }
  if (!slot) return false;  // bounded clients; do not evict an active replay fence
  if (!strcmp(slot->id, client) && seq <= slot->lastSeq) return false;
  snprintf(slot->id, sizeof(slot->id), "%s", client);
  slot->lastSeq = seq;
  slot->expiresAt = now + 330000U;
  return true;
}

inline void handleCommandMessage(const JsonDocument &doc) {
  const char *requestId = doc["requestId"] | "";
  if (!realtimeCommandChannelTrusted()) {
    publishAck(requestId, "unauthorized", "REALTIME CHUA XAC THUC");
    return;
  }
  const uint32_t sequence = doc["sequence"] | 0UL;
  const uint32_t messageBootId = doc["bootId"] | 0UL;
  const char *action = doc["action"] | "";

  // Lenh dieu khien PHAI co requestId, sequence va bootId hop le.
  // bootId thay doi moi lan khoi dong, nen packet cua boot cu bi vo hieu.
  if (!requestId[0] || (doc["v"].as<int>() != 2 && sequence == 0U)) {
    publishAck(requestId, "invalid", "");
    return;
  }
  if (messageBootId == 0U || messageBootId != bootId) {
    publishAck(requestId, "stale", "");
    return;
  }
  if (doc["v"].as<int>() == 2) {
    const uint32_t expiresAt = doc["expiresAt"] | 0UL;
    const time_t nowEpoch = time(nullptr);
    if (nowEpoch < 1700000000 || expiresAt < static_cast<uint32_t>(nowEpoch) ||
        expiresAt > static_cast<uint32_t>(nowEpoch) + 30U) {
      publishAck(requestId, "expired", "EXPIRED_REQUEST");
      return;
    }
  }

  // Chong lap trong cung boot: requestId khong duoc lap va sequence phai tang.
  if ((doc["v"].as<int>() != 2) && lastCommandSequence != 0U && sequence <= lastCommandSequence) {
    publishAck(requestId, "stale", "");
    return;
  }

  const HmiCommandType type = mapCommandAction(action);
  if (type == HmiCommandType::None) {
    publishAck(requestId, "unsupported", "");
    return;
  }

  // Rollback thay doi firmware dang boot. Kenh realtime hien dung credential
  // chung, nen rollback tu xa bi khoa. Rollback van dung duoc tren HMI
  // voi man xac nhan CO/HUY da co san.
  if (type == HmiCommandType::FirmwareRollback) {
    publishAck(requestId, "rejected", "QUAY LAI CAN XAC NHAN TAI MAY");
    return;
  }

  // "Coi bao" tren dashboard tat tam giong het nut ACK tren HMI (xem
  // requestAlarmAcknowledge() trong hmi.h) - can dung mat na canh bao
  // DANG active, khong phai AlarmNone, neu khong AlarmAck se khong xoa/tat
  // duoc gi ca (xem case HmiCommandType::AlarmAck trong machine_control.h).
  uint32_t alarmMaskParam = AlarmNone;
  if (type == HmiCommandType::AlarmAck) {
    portENTER_CRITICAL(&realtimeMux);
    alarmMaskParam = knownRuntimeValid
        ? (knownRuntime.alarmMask & ALARM_KNOWN_MASK) : AlarmNone;
    portEXIT_CRITICAL(&realtimeMux);
  }

  uint32_t commandId = 0U;
  const uint16_t validForMs = type == HmiCommandType::AutoTuneStart
      ? COMMAND_AUTOTUNE_VALID_MS : COMMAND_DEFAULT_VALID_MS;
  // F-09: danh dau lenh nay den tu realtime (tu xa) - AlarmAck se tu choi rieng
  // 2 hanh dong can xac nhan vat ly (xoa loi dao/tat coi khan cap) neu nguon
  // la Remote, xem case HmiCommandType::AlarmAck trong processHmiTransactions().
  // Reserve tracking before queue admission. A full tracker must never execute
  // an uncorrelated remote command, even if the HMI queue has room.
  PendingCommand *reserved = nullptr;
  portENTER_CRITICAL(&realtimeMux);
  for (PendingCommand &slot : pendingCommands) {
    if (slot.used) continue;
    reserved = &slot;
    slot = PendingCommand{};
    slot.used = true;
    slot.queuedAt = millis();
    snprintf(slot.requestId, sizeof(slot.requestId), "%s", requestId);
    snprintf(slot.operation, sizeof(slot.operation), "%s", activeOperation);
    slot.signedAck = activeAckKeyValid;
    if (slot.signedAck) memcpy(slot.ackKey, activeAckKey, sizeof(slot.ackKey));
    break;
  }
  portEXIT_CRITICAL(&realtimeMux);
  if (!reserved) { publishAck(requestId, "busy", "TRACKING_FULL"); return; }
  const bool queued = queueCommand(type, validForMs, 0U, alarmMaskParam, &commandId,
      HmiCommandSource::Remote, [](uint32_t id, void *context) {
        // Lock order is HMI -> Web; callers never hold Web while entering HMI.
        portENTER_CRITICAL(&realtimeMux);
        static_cast<PendingCommand *>(context)->commandId = id;
        portEXIT_CRITICAL(&realtimeMux);
      }, reserved);
  if (!queued) {
    portENTER_CRITICAL(&realtimeMux);
    reserved->used = false;
    portEXIT_CRITICAL(&realtimeMux);
    publishAck(requestId, "busy", "");
    return;
  }
  if (doc["v"].as<int>() != 2) lastCommandSequence = sequence;
  snprintf(lastCommandRequestId, sizeof(lastCommandRequestId), "%s", requestId);
  publishAck(requestId, "accepted", "");
}

inline void handleConfigSetMessage(const JsonDocument &doc) {
  const char *requestId = doc["requestId"] | "";
  if (!realtimeCommandChannelTrusted()) {
    publishAck(requestId, "unauthorized", "REALTIME CHUA XAC THUC");
    return;
  }
  const uint32_t revision = doc["revision"] | 0UL;
  if (!requestId[0] || revision == 0U) {
    publishAck(requestId, "invalid", "");
    return;
  }

  portENTER_CRITICAL(&realtimeMux);
  const bool busy = pendingConfigSave.used;
  const bool haveBase = knownConfigValid;
  const uint32_t currentRevision = realtimeConfigRevision;
  MachineConfig candidate = knownConfig;
  portEXIT_CRITICAL(&realtimeMux);

  if (currentRevision != 0U && revision <= currentRevision) {
    publishAck(requestId, "stale", "");
    return;
  }
  if (busy) {
    publishAck(requestId, "busy", "");
    return;
  }
  if (!haveBase) {
    publishAck(requestId, "invalid", "CHUA CO CAU HINH GOC");
    return;
  }
  JsonVariantConst configObj = doc["config"];
  if (configObj.isNull()) {
    publishAck(requestId, "invalid", "THIEU CONFIG");
    return;
  }

  // Reject unknown and malformed patch fields before touching the controller.
  static constexpr const char *patchKeys[] = {
    "alarmEnabled", "allowHeatWithoutBatch", "autoResumeAfterPower", "autotuneBandC", "autotuneRelayPowerPercent",
    "circulationFanEnabled", "controlMode", "emergencyTemp", "heaterStuckDurationSec", "heaterStuckMinRiseC",
    "highTempAlarm", "highTempAlarmWithoutBatch", "humidifierEnabled", "humidifierHysteresisRh", "humidityAlarmDelaySec", "humidityOffset",
    "kd", "ki", "kp", "lightAfterBatchAlarmEnabled", "lowHumidityAlarm",
    "lowTempAlarm", "manualTurnReanchorsSchedule", "adaptiveThermalBalanceEnabled", "maxHeaterPower", "nextDirection", "pidCycleSec",
    "powerRestoreDelaySec", "sensorTimeoutSec", "sirenSelfTestEnabled", "targetHumidity", "targetTemp",
    "tempHysteresis", "tempOffset", "tempOscillationCrossLimit", "tempOscillationWindowSec", "tempRateLimitC",
    "tempRateWindowSec", "totalIncubationDays", "turnIntervalMin", "turnMaxRunSec", "turningEnabled",
    "ventOffTemp", "ventOnTemp", "ventScheduleCount", "ventScheduleDurationMin", "ventScheduleEnabled",
    "ventScheduleHour1", "ventScheduleHour2", "ventScheduleHour3", "ventScheduleHour4", "ventScheduleHour5",
    "ventScheduleHour6",
    "ventAutoEnabled", "ventProfileLevel", "ventCycleMinutes",
    "ventDutyDay1To3", "ventDutyDay4To7", "ventDutyDay8To11",
    "ventDutyDay12To15", "ventDutyDay16To18", "ventDutyDay19To21",
  };
  JsonObjectConst fields = configObj.as<JsonObjectConst>();
  if (fields.size() == 0U) { publishAck(requestId, "invalid", "EMPTY_PATCH"); return; }
  for (JsonPairConst field : fields) {
    bool known = false;
    for (const char *key : patchKeys) if (!strcmp(field.key().c_str(), key)) { known = true; break; }
    const JsonVariantConst value = field.value();
    if (!known || !(value.is<bool>() || value.is<int>() || value.is<float>() || value.is<double>()) ||
        (value.is<float>() && !isfinite(value.as<float>()))) {
      publishAck(requestId, "invalid", "INVALID_CONFIG_PATCH");
      return;
    }
  }
  candidate.targetTemp = configObj["targetTemp"] | candidate.targetTemp;
  candidate.tempHysteresis = configObj["tempHysteresis"] | candidate.tempHysteresis;
  candidate.lowTempAlarm = configObj["lowTempAlarm"] | candidate.lowTempAlarm;
  candidate.highTempAlarm = configObj["highTempAlarm"] | candidate.highTempAlarm;
  candidate.emergencyTemp = configObj["emergencyTemp"] | candidate.emergencyTemp;
  candidate.kp = configObj["kp"] | candidate.kp;
  candidate.ki = configObj["ki"] | candidate.ki;
  candidate.kd = configObj["kd"] | candidate.kd;
  candidate.lowHumidityAlarm = configObj["lowHumidityAlarm"] | candidate.lowHumidityAlarm;
  candidate.humidifierEnabled = configObj["humidifierEnabled"] | candidate.humidifierEnabled;
  candidate.targetHumidity = configObj["targetHumidity"] | candidate.targetHumidity;
  candidate.humidifierHysteresisRh = configObj["humidifierHysteresisRh"] | candidate.humidifierHysteresisRh;
  candidate.ventOnTemp = configObj["ventOnTemp"] | candidate.ventOnTemp;
  candidate.ventOffTemp = configObj["ventOffTemp"] | candidate.ventOffTemp;
  candidate.ventScheduleEnabled = configObj["ventScheduleEnabled"] | candidate.ventScheduleEnabled;
  candidate.ventScheduleCount = configObj["ventScheduleCount"] | candidate.ventScheduleCount;
  candidate.ventScheduleDurationMin = configObj["ventScheduleDurationMin"] | candidate.ventScheduleDurationMin;
  candidate.ventScheduleHour1 = configObj["ventScheduleHour1"] | candidate.ventScheduleHour1;
  candidate.ventScheduleHour2 = configObj["ventScheduleHour2"] | candidate.ventScheduleHour2;
  candidate.ventScheduleHour3 = configObj["ventScheduleHour3"] | candidate.ventScheduleHour3;
  candidate.ventScheduleHour4 = configObj["ventScheduleHour4"] | candidate.ventScheduleHour4;
  candidate.ventScheduleHour5 = configObj["ventScheduleHour5"] | candidate.ventScheduleHour5;
  candidate.ventScheduleHour6 = configObj["ventScheduleHour6"] | candidate.ventScheduleHour6;
  candidate.adaptiveThermalBalanceEnabled = configObj["adaptiveThermalBalanceEnabled"] | candidate.adaptiveThermalBalanceEnabled;
  candidate.ventAutoEnabled = configObj["ventAutoEnabled"] | candidate.ventAutoEnabled;
  candidate.ventProfileLevel = configObj["ventProfileLevel"] | candidate.ventProfileLevel;
  candidate.ventCycleMinutes = configObj["ventCycleMinutes"] | candidate.ventCycleMinutes;
  candidate.ventDutyDay1To3 = configObj["ventDutyDay1To3"] | candidate.ventDutyDay1To3;
  candidate.ventDutyDay4To7 = configObj["ventDutyDay4To7"] | candidate.ventDutyDay4To7;
  candidate.ventDutyDay8To11 = configObj["ventDutyDay8To11"] | candidate.ventDutyDay8To11;
  candidate.ventDutyDay12To15 = configObj["ventDutyDay12To15"] | candidate.ventDutyDay12To15;
  candidate.ventDutyDay16To18 = configObj["ventDutyDay16To18"] | candidate.ventDutyDay16To18;
  candidate.ventDutyDay19To21 = configObj["ventDutyDay19To21"] | candidate.ventDutyDay19To21;
  candidate.tempOffset = configObj["tempOffset"] | candidate.tempOffset;
  candidate.humidityOffset = configObj["humidityOffset"] | candidate.humidityOffset;
  candidate.pidCycleSec = configObj["pidCycleSec"] | candidate.pidCycleSec;
  candidate.humidityAlarmDelaySec =
      configObj["humidityAlarmDelaySec"] | candidate.humidityAlarmDelaySec;
  candidate.turnIntervalMin = configObj["turnIntervalMin"] | candidate.turnIntervalMin;
  candidate.turnMaxRunSec = configObj["turnMaxRunSec"] | candidate.turnMaxRunSec;
  candidate.powerRestoreDelaySec =
      configObj["powerRestoreDelaySec"] | candidate.powerRestoreDelaySec;
  candidate.sensorTimeoutSec = configObj["sensorTimeoutSec"] | candidate.sensorTimeoutSec;
  candidate.maxHeaterPower = configObj["maxHeaterPower"] | candidate.maxHeaterPower;
  candidate.totalIncubationDays =
      configObj["totalIncubationDays"] | candidate.totalIncubationDays;
  candidate.circulationFanEnabled =
      configObj["circulationFanEnabled"] | candidate.circulationFanEnabled;
  candidate.turningEnabled = configObj["turningEnabled"] | candidate.turningEnabled;
  candidate.manualTurnReanchorsSchedule =
      configObj["manualTurnReanchorsSchedule"] | candidate.manualTurnReanchorsSchedule;
  candidate.sirenSelfTestEnabled =
      configObj["sirenSelfTestEnabled"] | candidate.sirenSelfTestEnabled;
  candidate.autoResumeOnPowerLoss =
      configObj["autoResumeAfterPower"] | candidate.autoResumeOnPowerLoss;
  candidate.allowHeatWithoutBatch =
      configObj["allowHeatWithoutBatch"] | candidate.allowHeatWithoutBatch;
  candidate.alarmEnabled = configObj["alarmEnabled"] | candidate.alarmEnabled;
  candidate.lightAfterBatchAlarmEnabled =
      configObj["lightAfterBatchAlarmEnabled"] | candidate.lightAfterBatchAlarmEnabled;
  candidate.highTempAlarmWithoutBatch =
      configObj["highTempAlarmWithoutBatch"] | candidate.highTempAlarmWithoutBatch;
  candidate.controlMode = static_cast<ControlMode>(
      configObj["controlMode"] | static_cast<uint8_t>(candidate.controlMode));
  candidate.nextDirection = static_cast<TurnDirection>(
      configObj["nextDirection"] | static_cast<uint8_t>(candidate.nextDirection));
  // 8 truong "Nang cao" (schema 8) - cung bi THIEU o day tu luc them tinh
  // nang, khien luu tu form "Nang cao" tren web ROI VAO IM LANG (khong loi,
  // nhung khong truong nao trong 8 truong nay thuc su duoc ap dung).
  candidate.heaterStuckMinRiseC =
      configObj["heaterStuckMinRiseC"] | candidate.heaterStuckMinRiseC;
  candidate.heaterStuckDurationSec =
      configObj["heaterStuckDurationSec"] | candidate.heaterStuckDurationSec;
  candidate.tempRateLimitC = configObj["tempRateLimitC"] | candidate.tempRateLimitC;
  candidate.tempRateWindowSec =
      configObj["tempRateWindowSec"] | candidate.tempRateWindowSec;
  candidate.tempOscillationCrossLimit =
      configObj["tempOscillationCrossLimit"] | candidate.tempOscillationCrossLimit;
  candidate.tempOscillationWindowSec =
      configObj["tempOscillationWindowSec"] | candidate.tempOscillationWindowSec;
  candidate.autotuneRelayPowerPercent =
      configObj["autotuneRelayPowerPercent"] | candidate.autotuneRelayPowerPercent;
  candidate.autotuneBandC = configObj["autotuneBandC"] | candidate.autotuneBandC;

  sanitizeConfig(candidate);
  if (!mayapPidHasAuthority(candidate)) {
    publishAck(requestId, "invalid", "INVALID_PID_GAINS");
    return;
  }
  if (!isfinite(candidate.targetTemp) || !isfinite(candidate.highTempAlarm) ||
      !isfinite(candidate.emergencyTemp) ||
      candidate.lowTempAlarm >= candidate.targetTemp ||
      candidate.highTempAlarm <= candidate.targetTemp ||
      candidate.emergencyTemp <= candidate.highTempAlarm) {
    publishAck(requestId, "invalid", "INVALID_FULL_CONFIG");
    return;
  }

  // Register the transaction before the control task can observe readyForHost.
  portENTER_CRITICAL(&realtimeMux);
  pendingConfigSave = PendingConfigSave{};
  pendingConfigSave.used = true;
  pendingConfigSave.queuedAt = millis();
  pendingConfigSave.revision = revision;
  pendingConfigSave.signedAck = activeAckKeyValid;
  if (activeAckKeyValid) memcpy(pendingConfigSave.ackKey, activeAckKey,
                                sizeof(pendingConfigSave.ackKey));
  snprintf(pendingConfigSave.requestId, sizeof(pendingConfigSave.requestId), "%s",
           requestId);
  portEXIT_CRITICAL(&realtimeMux);
  uint32_t transactionId = 0U;
  if (!startConfigSave(candidate, true, &transactionId)) {
    portENTER_CRITICAL(&realtimeMux);
    pendingConfigSave.used = false;
    portEXIT_CRITICAL(&realtimeMux);
    publishAck(requestId, "busy", "");
    return;
  }
  portENTER_CRITICAL(&realtimeMux);
  pendingConfigSave.transactionId = transactionId;
  portEXIT_CRITICAL(&realtimeMux);
  portENTER_CRITICAL(&hmiApiMux);
  configSave.readyForHost = true;
  portEXIT_CRITICAL(&hmiApiMux);
  publishAck(requestId, "accepted", "");
}


// ------------------------- Web session (foreground tabs) ---------------------
// `session` only tunes snapshot cadence and asks for a resync; it never changes a
// setpoint or actuator, so (as in V2) it is unsigned. Owner task only.
constexpr uint32_t REALTIME_SESSION_MAX_TTL_MS = 60000UL;
static bool webSessionActive = false;
struct SessionLease { char id[40] = ""; uint32_t expiresAt = 0U; };
static SessionLease sessionLeases[8];

inline void handleSessionMessage(const JsonDocument &doc) {
  const char *client = doc["clientId"] | "";
  if (strlen(client) < 8U || strlen(client) >= sizeof(sessionLeases[0].id)) return;
  const bool active = doc["active"] | false;
  uint32_t ttlMs = doc["ttlMs"] | 0UL;
  const bool sync = doc["sync"] | false;
  const uint32_t now = millis();
  SessionLease *slot = nullptr;
  for (auto &lease : sessionLeases) if (!strcmp(lease.id, client)) { slot = &lease; break; }
  if (!slot) for (auto &lease : sessionLeases)
    if (!lease.id[0] || timeReached(now, lease.expiresAt)) { slot = &lease; break; }
  if (!slot) return;  // bounded clients: never evict an active lease
  if (active) {
    if (ttlMs == 0U || ttlMs > REALTIME_SESSION_MAX_TTL_MS) ttlMs = REALTIME_SESSION_MAX_TTL_MS;
    snprintf(slot->id, sizeof(slot->id), "%s", client);
    slot->expiresAt = now + ttlMs;
  } else {
    slot->id[0] = '\0';
    slot->expiresAt = now;
  }
  webSessionActive = false;
  for (const auto &lease : sessionLeases)
    if (lease.id[0] && !timeReached(now, lease.expiresAt)) webSessionActive = true;
  if (!sync) return;
  // Republish from the owner loop, not from inside the incoming message.
  portENTER_CRITICAL(&realtimeMux);
  if (knownConfigValid) configDirty = true;
  eventSnapshotDirty = true;
  portEXIT_CRITICAL(&realtimeMux);
  lastSnapshotPublishAt = 0U;
  forceSnapshotPublish = true;
  lastPublishedEventSequence = 0U;
}

inline void dispatchApplicationMessage(const char *channel, const uint8_t *payload, size_t length) {
  if (!channel || length > MayapProtocol::FRAME_NORMAL_CAP) return;
  JsonDocument wireDoc;
  if (deserializeJson(wireDoc, payload, length) != DeserializationError::Ok) return;
  // Any Web contact (an open tab's session, a command, a config/history request) keeps the radio awake and
  // returns it to PERFORMANCE at once if it had gone to modem sleep after 15 idle minutes.
  if (strcmp(channel, "session") != 0 || (wireDoc["active"] | false)) MayapWifiPower::noteWebActivity(millis());
  if (!strcmp(channel, "session")) {
    handleSessionMessage(wireDoc);
    return;
  }

  auto verifyAndDispatch = [&](const char *channel, auto handler) {
    activeAckKeyValid = false;
    JsonDocument bodyDoc;
    const bool v2 = wireDoc["v"].as<int>() == 2;
    bool expired = false;
    if (!v2 || !realtimeVerifyV2(channel, wireDoc, bodyDoc, expired)) {
      if (expired) {
        const char *op = (!strcmp(channel, "command")) ? (bodyDoc["action"] | "")
            : !strcmp(channel, "config/set") ? "config.save"
            : "history.read";
        char normalized[40];
        snprintf(normalized, sizeof(normalized), "%s", op);
        for (char *c = normalized; *c; ++c) if (*c == '_') *c = '.';
        publishAck(bodyDoc["requestId"] | "", "unauthorized", "SESSION_EXPIRED", normalized);
        activeAckKeyValid = false;
        return;
      }
      const char *legacyRequestId = wireDoc["requestId"] | "";
      if (legacyRequestId[0]) publishAck(legacyRequestId, "unauthorized", "CHU KY LENH KHONG HOP LE");
      return;
    }
    const char *id = bodyDoc["requestId"] | "";
    const char *op = (!strcmp(channel, "command")) ? (bodyDoc["action"] | "")
                    : !strcmp(channel, "config/set") ? "config.save"
                    : "history.read";
    snprintf(activeOperation, sizeof(activeOperation), "%s", op);
    for (char *c = activeOperation; *c; ++c) if (*c == '_') *c = '.';
    if (v2 && bodyDoc["bootId"].as<uint32_t>() != bootId) {
      publishAck(id, "stale", "STALE_BOOT");
      activeOperation[0] = '\0';
      activeAckKeyValid = false;
      return;
    }
    if (replayTerminal(id)) { activeOperation[0] = '\0'; activeAckKeyValid = false; return; }
    bool inFlight = (pendingConfigSave.used && !strcmp(id, pendingConfigSave.requestId)) ||
                    (historyResponsePending && !strcmp(id, historyRequestId));
    for (const auto &pending : pendingCommands)
      if (pending.used && !strcmp(id, pending.requestId)) inFlight = true;
    if (inFlight) { publishAck(id, "accepted", ""); activeOperation[0] = '\0'; activeAckKeyValid = false; return; }
    if (v2 && !checkReplaySequence(bodyDoc)) {
      publishAck(id, "stale", "REPLAY SEQUENCE");
      activeOperation[0] = '\0';
      activeAckKeyValid = false;
      return;
    }
    handler(bodyDoc);
    activeOperation[0] = '\0';
    activeAckKeyValid = false;
  };

  if (!strcmp(channel, "config/set")) {
    verifyAndDispatch("config/set", [](const JsonDocument &doc) { handleConfigSetMessage(doc); });
    return;
  }

  if (!strcmp(channel, "history/request")) {
    verifyAndDispatch("history/request", [](const JsonDocument &doc) { handleHistoryRequestMessage(doc); });
    return;
  }
  if (!strcmp(channel, "command")) {
    verifyAndDispatch("command", [](const JsonDocument &doc) { handleCommandMessage(doc); });
  }
}

// ------------------------------ Vong doi ket noi -------------------------------
inline void flushCompletedTransactions() {
  portENTER_CRITICAL(&realtimeMux);
  for (PendingCommand &slot : pendingCommands) {
    if (slot.used && slot.completed && enqueueAckLocked(slot.requestId,
        slot.completionOk ? "applied" : "rejected", slot.completionMessage,
        slot.operation, slot.queuedAt, slot.signedAck ? slot.ackKey : nullptr, slot.completedAt))
      slot.used = false;
  }
  PendingConfigSave *saves[] = {&pendingConfigSave};
  for (PendingConfigSave *slot : saves) {
    if (slot->used && slot->completed && enqueueAckLocked(slot->requestId,
        slot->completionOk ? "applied" : "rejected", slot->completionMessage,
        "config.save", slot->queuedAt,
        slot->signedAck ? slot->ackKey : nullptr, slot->completedAt)) slot->used = false;
  }
  portEXIT_CRITICAL(&realtimeMux);
}

inline void expirePendingCommands(uint32_t now) {
  flushCompletedTransactions();
  char requestIdsToExpire[COMMAND_QUEUE_SIZE][REALTIME_REQUEST_ID_CAPACITY];
  char operationsToExpire[COMMAND_QUEUE_SIZE][40];
  uint8_t keysToExpire[COMMAND_QUEUE_SIZE][32];
  bool signedToExpire[COMMAND_QUEUE_SIZE] = {};
  uint8_t expireCount = 0U;
  bool configExpired = false;
  char configRequestId[REALTIME_REQUEST_ID_CAPACITY] = "";
  uint8_t configKey[32] = {};
  bool configSigned = false;

  portENTER_CRITICAL(&realtimeMux);
  for (PendingCommand &slot : pendingCommands) {
    if (!slot.used || slot.completed) continue;
    if (slot.uncertainSent) {
      if (timeReached(now, slot.queuedAt) && elapsedMs(now, slot.queuedAt) >= 120000U) slot.used = false;
      continue;
    }
    // An input owner can create a slot after the caller captured `now`. Without
    // this ordering guard, now - queuedAt underflows and a fresh request looks
    // roughly 49 days old, so it is expired immediately.
    if (!timeReached(now, slot.queuedAt)) continue;
    if (elapsedMs(now, slot.queuedAt) < REALTIME_COMMAND_ACK_TIMEOUT_MS) continue;
    snprintf(requestIdsToExpire[expireCount], REALTIME_REQUEST_ID_CAPACITY, "%s",
             slot.requestId);
    snprintf(operationsToExpire[expireCount], sizeof(operationsToExpire[0]), "%s",
             slot.operation);
    signedToExpire[expireCount] = slot.signedAck;
    if (slot.signedAck) memcpy(keysToExpire[expireCount], slot.ackKey, 32U);
    ++expireCount;
    slot.uncertainSent = true;
  }
  if (pendingConfigSave.uncertainSent && !pendingConfigSave.completed && timeReached(now, pendingConfigSave.queuedAt) && elapsedMs(now, pendingConfigSave.queuedAt) >= 120000U) pendingConfigSave.used = false;
  if (pendingConfigSave.used && !pendingConfigSave.completed && !pendingConfigSave.uncertainSent && timeReached(now, pendingConfigSave.queuedAt) &&
      elapsedMs(now, pendingConfigSave.queuedAt) >= REALTIME_CONFIG_SAVE_ACK_TIMEOUT_MS) {
    configExpired = true;
    snprintf(configRequestId, sizeof(configRequestId), "%s",
             pendingConfigSave.requestId);
    configSigned = pendingConfigSave.signedAck;
    if (configSigned) memcpy(configKey, pendingConfigSave.ackKey, 32U);
    pendingConfigSave.uncertainSent = true;
  }
  portEXIT_CRITICAL(&realtimeMux);

  for (uint8_t i = 0; i < expireCount; ++i)
    publishAck(requestIdsToExpire[i], "expired", "", operationsToExpire[i],
               0U, 0U, signedToExpire[i] ? keysToExpire[i] : nullptr);
  if (configExpired) publishAck(configRequestId, "expired", "", "config.save",
                                0U, 0U, configSigned ? configKey : nullptr);

}

// Terminal ACKs wait here until the transport accepted them. A refusal (in-flight window full, link closing) is retried with a
// short exponential back-off (50 ms .. 1 s) instead of every loop pass, and logged at most once per 2 s with the count, so a
// busy moment neither spins the CPU nor floods the serial buffer. An ACK that was accepted by the socket but whose PUBACK never
// comes is covered one level up: the Web re-sends the command and replayTerminal() answers it WITHOUT executing it again.
static uint8_t ackRefusals = 0U;
static uint32_t ackRetryAt = 0U, ackLogAt = 0U, ackRefusedTotal = 0U, ackRefusedLogged = 0U;
inline void drainAckOutbox() {
  const uint32_t startedAt = millis();
  if (ackRefusals > 0U && static_cast<int32_t>(startedAt - ackRetryAt) < 0) return;
  for (uint8_t i = 0U; i < COMMAND_QUEUE_SIZE + 2U; ++i) {
    AckOutboxItem item;
    portENTER_CRITICAL(&realtimeMux);
    item = ackOutbox[i];
    portEXIT_CRITICAL(&realtimeMux);
    if (!item.used) continue;
    const bool sent = publishAck(item.requestId, item.result, item.message,
                                 item.operation, item.receivedAt, item.completedAt,
                                 item.signedAck ? item.ackKey : nullptr);
    if (!sent) {
      const uint32_t now = millis();
      if (ackRefusals < 255U) ++ackRefusals;
      ++ackRefusedTotal;
      const uint8_t shift = ackRefusals < 5U ? ackRefusals : 5U;
      ackRetryAt = now + (50UL << shift > 1000UL ? 1000UL : (50UL << shift));
      if (ackLogAt == 0U || static_cast<uint32_t>(now - ackLogAt) >= 2000UL) {
        mayapSerialPrintf(false, "[CFG-TX] ACK PUB FAIL id=%s (refused %lu since last log, retry in %lums)\n", item.requestId,
                          static_cast<unsigned long>(ackRefusedTotal - ackRefusedLogged), static_cast<unsigned long>(ackRetryAt - now));
        ackLogAt = now ? now : 1U;
        ackRefusedLogged = ackRefusedTotal;
      }
      break; // Keep the ACK and retry after the back-off.
    }
    ackRefusals = 0U;
    portENTER_CRITICAL(&realtimeMux);
    if (ackOutbox[i].used && ackOutbox[i].completedAt == item.completedAt &&
        !strcmp(ackOutbox[i].requestId, item.requestId))
      ackOutbox[i].used = false;
    portEXIT_CRITICAL(&realtimeMux);
  }
}

inline void serviceConfigPublish() {
  portENTER_CRITICAL(&realtimeMux);
  const bool dirty = configDirty;
  configDirty = false;
  const MachineConfig cfg = knownConfig;
  const uint32_t revision = realtimeConfigRevision;
  portEXIT_CRITICAL(&realtimeMux);
  if (dirty && !publishConfigReport(cfg, revision)) {
    portENTER_CRITICAL(&realtimeMux); configDirty = true; portEXIT_CRITICAL(&realtimeMux);
  }
}



inline void serviceSessionTimeout(uint32_t now) {
  bool active = false;
  for (auto &lease : sessionLeases) {
    if (lease.id[0] && timeReached(now, lease.expiresAt)) lease.id[0] = '\0';
    if (lease.id[0]) active = true;
  }
  webSessionActive = active;
}

// Full live `snapshot` (V2 contract): fast while a Web tab holds a session lease,
// slow heartbeat otherwise. A completed command forces an immediate sample.
inline void serviceLiveSnapshot(uint32_t now) {
  const uint32_t interval = webSessionActive ? REALTIME_SNAPSHOT_ACTIVE_INTERVAL_MS
                                             : REALTIME_SNAPSHOT_IDLE_INTERVAL_MS;
  if (!forceSnapshotPublish && !timeReached(now, lastSnapshotPublishAt + interval)) return;
  portENTER_CRITICAL(&realtimeMux);
  const bool valid = knownRuntimeValid;
  const MachineRuntime rt = knownRuntime;
  const uint32_t revision = realtimeConfigRevision;
  portEXIT_CRITICAL(&realtimeMux);
  if (valid && publishSnapshot(rt, revision)) {
    forceSnapshotPublish = false;
    lastSnapshotPublishAt = millis();
  }
}

inline void serviceSnapshotPublish(uint32_t now) {
  // Future publisher decides when to call this. Preserve generic idle cadence.
  static MayapRealtimePublish::BootstrapState last{};
  static bool known = false;
  portENTER_CRITICAL(&realtimeMux);
  const bool valid = knownRuntimeValid; const MachineRuntime rt = knownRuntime;
  const uint32_t revision = realtimeConfigRevision;
  portEXIT_CRITICAL(&realtimeMux);
  if (!valid || !publishCallback) return;
  auto hint = MayapRealtimePublish::bootstrapState(rt, revision);
  hint.temperature = hint.humidity = 0;
  hint.outputs &= 65U;
  if (!forceSnapshotPublish && known && hint == last &&
      elapsedMs(now, lastSnapshotPublishAt) < REALTIME_SNAPSHOT_IDLE_INTERVAL_MS) return;
  if (publishBootstrap(rt, revision)) {
    known = true; last = hint; forceSnapshotPublish = false; lastSnapshotPublishAt = millis();
  }
}

inline void serviceEventLogPublish() {
  portENTER_CRITICAL(&realtimeMux);
  const bool dirty = eventSnapshotDirty;
  const HmiEventSnapshot snapshot = pendingEventSnapshot;
  portEXIT_CRITICAL(&realtimeMux);
  if (!dirty || snapshot.sourceSequence == lastPublishedEventSequence) return;
  // Oldest first; advance the cursor ONLY after successful publication.
  // Keep dirty until drained so a failed send or >5-item backlog is retried.
  uint8_t published = 0U;
  for (uint8_t i = snapshot.count; i > 0U && published < 5U; --i) {
    const HmiEventItem &item = snapshot.items[i - 1U];
    if (item.sequence <= lastPublishedEventSequence) continue;
    if (!publishLogEntry(item)) return;
    lastPublishedEventSequence = item.sequence;
    ++published;
  }
  if (lastPublishedEventSequence >= snapshot.sourceSequence) {
    portENTER_CRITICAL(&realtimeMux);
    if (pendingEventSnapshot.sourceSequence == snapshot.sourceSequence) eventSnapshotDirty = false;
    portEXIT_CRITICAL(&realtimeMux);
  }
}

}  // namespace MayapRealtimeInternal

// ================================ API cong khai ================================

inline void mayapRealtimeBegin() { MayapRealtimeInternal::ensureIdentity(); }
inline void mayapMqttRecover(uint32_t) {}
inline void mayapRealtimeUpdate(uint32_t now) {
  using namespace MayapRealtimeInternal;
  // The owner drains inbound packets before this call and passes a fresh clock
  // sample, so no incoming callback can stamp a time newer than `now`.
  serviceSessionTimeout(now);
  expirePendingCommands(now);
  drainAckOutbox();
  // Terminal ACKs above never wait; bulk reports defer while another TLS
  // operation holds its transient working set.
  MayapNetworkBatchOperation batch;
  if (!batch) return;
  serviceLiveSnapshot(now);
  serviceConfigPublish();
  serviceEventLogPublish();
  serviceHistoryResponse();
}

// ------------------------- Hooks goi tu controlTask (machine_control.h) --------
// Tat ca cac ham duoi day CHI ghi vao hop thu realtimeMux-protected, khong bao gio
// goi vao network transport/WiFiClient (I/O mang phai o lai realtime owner).

inline void mayapRealtimeSetRuntime(const MachineRuntime &runtime) {
  using namespace MayapRealtimeInternal;
  portENTER_CRITICAL(&realtimeMux);
  knownRuntime = runtime;
  knownRuntimeValid = true;
  portEXIT_CRITICAL(&realtimeMux);
}

inline void mayapRealtimeSetConfig(const MachineConfig &config) {
  using namespace MayapRealtimeInternal;
  portENTER_CRITICAL(&realtimeMux);
  const bool changed = !knownConfigValid ||
      memcmp(&config, &knownConfig, sizeof(MachineConfig)) != 0;
  knownConfig = config;
  knownConfigValid = true;
  if (changed) {
    configDirty = true;
    // pendingConfigSave.used == true nghia la thay doi nay la ket qua truc
    // tiep cua mot config/set tu web: revision da duoc gan san trong
    // handleConfigSetMessage(), khong tang them de khop dung "revision" ma
    // web dang cho trong pending.revision. Ngoai ra (HMI sua tay...) thi tang.
    if (realtimeConfigRevision == 0U) realtimeConfigRevision = 1U;
    else if (!pendingConfigSave.used) ++realtimeConfigRevision;
  }
  portEXIT_CRITICAL(&realtimeMux);
}



inline void mayapRealtimeConfirmCommand(uint32_t commandId, bool ok,
                                   const char *message) {
  using namespace MayapRealtimeInternal;
  portENTER_CRITICAL(&realtimeMux);
  for (PendingCommand &slot : pendingCommands) {
    if (!slot.used || slot.completed || slot.commandId != commandId) continue;
    slot.completed = true; slot.completionOk = ok; slot.completedAt = millis();
    snprintf(slot.completionMessage, sizeof(slot.completionMessage), "%s", message ? message : "");
    break;
  }
  portEXIT_CRITICAL(&realtimeMux);
}

inline void mayapRealtimeConfirmConfigSave(uint32_t transactionId, bool ok,
                                      const MachineConfig *stored,
                                      const char *failureCode = "CONFIG_SAVE_REJECTED") {
  using namespace MayapRealtimeInternal;
  (void)stored;  // config moi da/se toi qua mayapRealtimeSetConfig() tu cung noi goi
  portENTER_CRITICAL(&realtimeMux);
  if (pendingConfigSave.used && !pendingConfigSave.completed && pendingConfigSave.transactionId == transactionId) {
    if (ok) {
      realtimeConfigRevision = pendingConfigSave.revision > realtimeConfigRevision
          ? pendingConfigSave.revision : realtimeConfigRevision + 1U;
      lastVerifiedConfigRevision = realtimeConfigRevision;
      snprintf(lastVerifiedConfigRequestId, sizeof(lastVerifiedConfigRequestId), "%s",
               pendingConfigSave.requestId);
      configDirty = true; // Publish a complete verified report even if a prior report raced.
    }
    pendingConfigSave.completed = true; pendingConfigSave.completionOk = ok;
    pendingConfigSave.completedAt = millis();
    snprintf(pendingConfigSave.completionMessage, sizeof(pendingConfigSave.completionMessage), "%s", ok ? "" : failureCode);
  }
  portEXIT_CRITICAL(&realtimeMux);
}



inline void mayapRealtimePushEventLog(const HmiEventSnapshot &snapshot) {
  using namespace MayapRealtimeInternal;
  portENTER_CRITICAL(&realtimeMux);
  pendingEventSnapshot = snapshot;
  eventSnapshotDirty = true;
  portEXIT_CRITICAL(&realtimeMux);
}

// Local diagnostic for the dormant application bridge.
inline void mayapPrintWebStatus() {
  mayapSerialPrintf(false, "[TX] publisher=disabled pending=local-only\n");
}
