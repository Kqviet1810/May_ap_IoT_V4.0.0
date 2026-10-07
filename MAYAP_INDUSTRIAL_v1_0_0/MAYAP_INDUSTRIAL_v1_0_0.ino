#include "config.h"
#include "boot_diagnostic.h"
#include <esp_heap_caps.h>
#include <esp_timer.h>

static volatile bool gMayapSystemTripLatched = false;

bool mayapSystemTripLatched() {
  return __atomic_load_n(&gMayapSystemTripLatched, __ATOMIC_ACQUIRE);
}

void mayapLatchSystemTrip() {
  __atomic_store_n(&gMayapSystemTripLatched, true, __ATOMIC_RELEASE);
}

static StaticSemaphore_t i2cMutexBuffer;
static SemaphoreHandle_t i2cMutex = nullptr;

bool mayapI2cLock(uint32_t timeoutMs) {
  if (!i2cMutex) return false;
  const TickType_t ticks = timeoutMs ? pdMS_TO_TICKS(timeoutMs) : 0;
  return xSemaphoreTake(i2cMutex, ticks) == pdTRUE;
}

void mayapI2cUnlock() {
  if (i2cMutex) xSemaphoreGive(i2cMutex);
}

#include "device_identity.h"
#include "i2c_supervisor.h"
#include "service_recovery.h"
#include "network_service.h"
#include "ota_update.h"
#include "ota_web_update.h"
#include "ota_rollback.h"
#include "hmi.h"
#include "history_store.h"
#include "transaction_bridge.h"
#include "mqtt_transport.h"
#include "cloud_alert_link.h"
#include "attiny_bus.h"
#include "machine_control.h"

using namespace Mayap;

// Core 1 chay dieu khien an toan va HMI (uu tien thap hon control). Core 0 tach cac task
// I/O doc lap de HTTPS/NTP/OTA khong chan dieu khien. Tat ca stack tinh,
// khong tao/xoa task trong runtime.
constexpr uint32_t NETWORK_FAST_TASK_PERIOD_MS = 50UL;
constexpr uint32_t MQTT_TASK_PERIOD_MS = 20UL;
constexpr uint32_t CLOUD_TASK_PERIOD_MS = 100UL;
// Controller availability outranks connectivity. Heap pressure remains visible
// through E401/E402, but an online workload may not reboot the machine.
// Same budget as the V2 MQTT owner: publishAck/publishJson alone keep a 2 KiB
// frame plus signing buffers on this stack.
constexpr size_t MQTT_TASK_STACK_BYTES = 12288U;
constexpr size_t CLOUD_TASK_STACK_BYTES = 12288U;

static StaticTask_t controlTaskTcb;
static StackType_t controlTaskStack[
    (CONTROL_TASK_STACK_BYTES + sizeof(StackType_t) - 1U) / sizeof(StackType_t)];
static TaskHandle_t controlTaskHandle = nullptr;

static StaticTask_t hmiTaskTcb;
static StackType_t hmiTaskStack[
    (HMI_TASK_STACK_BYTES + sizeof(StackType_t) - 1U) / sizeof(StackType_t)];
static TaskHandle_t hmiTaskHandle = nullptr;

static StaticTask_t supervisorTaskTcb;
static StackType_t supervisorTaskStack[
    (SUPERVISOR_TASK_STACK_BYTES + sizeof(StackType_t) - 1U) / sizeof(StackType_t)];
static TaskHandle_t supervisorTaskHandle = nullptr;

static StaticTask_t networkTaskTcb;
static StackType_t networkTaskStack[
    (NETWORK_TASK_STACK_BYTES + sizeof(StackType_t) - 1U) / sizeof(StackType_t)];
static TaskHandle_t networkTaskHandle = nullptr;

static StaticTask_t mqttTaskTcb;
static StackType_t mqttTaskStack[
    (MQTT_TASK_STACK_BYTES + sizeof(StackType_t) - 1U) / sizeof(StackType_t)];
static TaskHandle_t mqttTaskHandle = nullptr;

static StaticTask_t cloudTaskTcb;
static StackType_t cloudTaskStack[
    (CLOUD_TASK_STACK_BYTES + sizeof(StackType_t) - 1U) / sizeof(StackType_t)];
static TaskHandle_t cloudTaskHandle = nullptr;

static StaticTask_t otaTaskTcb;
static StackType_t otaTaskStack[
    (OTA_TASK_STACK_BYTES + sizeof(StackType_t) - 1U) / sizeof(StackType_t)];
static TaskHandle_t otaTaskHandle = nullptr;

// Hai co nay chi dung de portal cho mot I/O mang dang block ket thuc truoc
// khi doi mode radio. Task ghi co cua chinh no, networkTask chi doc.
static volatile uint8_t mqttIoBusy = 0U;
static volatile uint8_t cloudIoBusy = 0U;

static volatile uint32_t controlHeartbeatMs = 0U;
static volatile uint32_t hmiHeartbeatMs = 0U;
static volatile uint32_t controlLastCycleUs = 0U;
static volatile uint32_t controlMaxCycleUs = 0U;
static volatile uint8_t controlTripCycleCount = 0U;
// Where control was last seen, so a heartbeat trip can tell "not scheduled" from
// "stuck inside Machine.update": phase 1=update, 2=post-update/WDT, 3=waiting for next period.
static volatile uint8_t controlPhase = 0U;
static volatile uint8_t controlInUpdate = 0U;
static volatile uint32_t controlCycleStartMs = 0U;
static volatile uint32_t hmiLastCycleUs = 0U;
static volatile uint32_t hmiMaxCycleUs = 0U;
static volatile uint8_t hmiTripCycleCount = 0U;
static volatile uint32_t supervisorHeartbeatMs = 0U;
static volatile uint8_t controlStarted = 0U;
static volatile uint8_t hmiStarted = 0U;
static volatile uint8_t sensorHealthy = 0U;
static volatile uint8_t displayHealthy = 0U;
static volatile uint8_t networkReady = 0U;
static volatile uint8_t mqttReady = 0U;
static volatile uint8_t mqttConnected = 0U;
static volatile uint8_t cloudReady = 0U;
static volatile uint8_t otaReady = 0U;
static MayapBoot::Sequencer bootSequence;
static MayapBoot::Stability localTaskStability;
static MayapBoot::Stability localSuccessStability;
static bool bootStageEntered = false;
static bool bootSuccessMarked = false;
static bool bootFailuresCleared = false;
static bool bootCoordinatorWdt = false;
static bool bootReadyShown = false;
static uint32_t bootReadyAt = 0U;

static_assert(sizeof(controlTaskStack) >= CONTROL_TASK_STACK_BYTES,
              "Control stack buffer qua nho");
static_assert(sizeof(hmiTaskStack) >= HMI_TASK_STACK_BYTES,
              "HMI stack buffer qua nho");
static_assert(sizeof(supervisorTaskStack) >= SUPERVISOR_TASK_STACK_BYTES,
              "Supervisor stack buffer qua nho");
static_assert(sizeof(networkTaskStack) >= NETWORK_TASK_STACK_BYTES,
              "Network stack buffer qua nho");
static_assert(sizeof(mqttTaskStack) >= MQTT_TASK_STACK_BYTES,
              "MQTT stack buffer qua nho");
static_assert(sizeof(cloudTaskStack) >= CLOUD_TASK_STACK_BYTES,
              "Cloud stack buffer qua nho");
static_assert(sizeof(otaTaskStack) >= OTA_TASK_STACK_BYTES,
              "OTA stack buffer qua nho");

static void fatalRestart(const char *stage, esp_err_t error,
                         MayapBoot::RestartReason reason = MayapBoot::RestartReason::FatalInit) {
  mayapLatchSystemTrip();
  mayapSafeOutputsEarly();
  mayapSerialPrintf(false, "[FATAL] %s err=%d -> RESTART\n",
                    stage ? stage : "SYSTEM", static_cast<int>(error));
  mayapRestart(reason, stage);
}

static void subscribeCurrentTaskToWdt(const char *name) {
  const esp_err_t result = esp_task_wdt_add(nullptr);
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
    fatalRestart(name, result, MayapBoot::RestartReason::WdtApi);
  }
}

void controlTask(void *parameter) {
  (void)parameter;
  subscribeCurrentTaskToWdt("CTRL WDT ADD");
  TickType_t lastWake = xTaskGetTickCount();
#if MAYAP_DIAGNOSTIC_SERIAL
  uint32_t lastStackReportAt = millis();
#endif

  for (;;) {
    const uint32_t now = millis();
    const int64_t cycleStartedUs = esp_timer_get_time();
    __atomic_store_n(&controlCycleStartMs, now, __ATOMIC_RELEASE);
    __atomic_store_n(&controlInUpdate, 1U, __ATOMIC_RELEASE);
    __atomic_store_n(&controlPhase, 1U, __ATOMIC_RELEASE);
    Machine.update(now);
    __atomic_store_n(&controlInUpdate, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&controlPhase, 2U, __ATOMIC_RELEASE);
    const MachineRuntime &runtime = Machine.runtime();
    __atomic_store_n(&sensorHealthy,
        runtime.sensorOnline && !runtime.sensorStartupGrace && isfinite(runtime.temperature) ? 1U : 0U,
        __ATOMIC_RELEASE);
    const uint32_t cycleUs = static_cast<uint32_t>(
        std::min<int64_t>(UINT32_MAX, esp_timer_get_time() - cycleStartedUs));
    __atomic_store_n(&controlLastCycleUs, cycleUs, __ATOMIC_RELEASE);
    uint32_t previousMax = __atomic_load_n(&controlMaxCycleUs, __ATOMIC_ACQUIRE);
    while (cycleUs > previousMax &&
           !__atomic_compare_exchange_n(&controlMaxCycleUs, &previousMax, cycleUs,
                                        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
    uint8_t slowCount = __atomic_load_n(&controlTripCycleCount, __ATOMIC_ACQUIRE);
    if (cycleUs >= CONTROL_CYCLE_TRIP_US) {
      if (slowCount < UINT8_MAX) ++slowCount;
    } else {
      slowCount = 0U;
    }
    __atomic_store_n(&controlTripCycleCount, slowCount, __ATOMIC_RELEASE);
    __atomic_store_n(&controlHeartbeatMs, millis(), __ATOMIC_RELEASE);

#if MAYAP_DIAGNOSTIC_SERIAL
    if (elapsedMs(now, lastStackReportAt) >= TASK_STACK_MONITOR_MS) {
      lastStackReportAt = now;
      mayapSerialPrintf(false,
          "[TASK] stack ctrl=%u hmi=%u sup=%u net=%u mqtt=%u cloud=%u ota=%u bytes ctrl=%lu/%luus hmi=%lu/%luus\n",
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(controlTaskHandle)),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(hmiTaskHandle)),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(supervisorTaskHandle)),
          networkTaskHandle ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(networkTaskHandle)) : 0U,
          mqttTaskHandle ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(mqttTaskHandle)) : 0U,
          cloudTaskHandle ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(cloudTaskHandle)) : 0U,
          otaTaskHandle ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(otaTaskHandle)) : 0U,
          static_cast<unsigned long>(__atomic_load_n(&controlLastCycleUs, __ATOMIC_ACQUIRE)),
          static_cast<unsigned long>(__atomic_load_n(&controlMaxCycleUs, __ATOMIC_ACQUIRE)),
          static_cast<unsigned long>(__atomic_load_n(&hmiLastCycleUs, __ATOMIC_ACQUIRE)),
          static_cast<unsigned long>(__atomic_load_n(&hmiMaxCycleUs, __ATOMIC_ACQUIRE)));
      mayapSerialPrintf(false,
          "[HEAP] free=%lu min=%lu largest=%lu\n",
          static_cast<unsigned long>(ESP.getFreeHeap()),
          static_cast<unsigned long>(ESP.getMinFreeHeap()),
          static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    }
#endif

    const esp_err_t result = esp_task_wdt_reset();
    if (result != ESP_OK) fatalRestart("CTRL WDT RESET", result, MayapBoot::RestartReason::WdtApi);
    __atomic_store_n(&controlPhase, 3U, __ATOMIC_RELEASE);
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(CONTROL_TASK_PERIOD_MS));
  }
}

void hmiTask(void *parameter) {
  (void)parameter;
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    const uint32_t now = millis();
    const int64_t cycleStartedUs = esp_timer_get_time();
    hmiUpdate(now);
    __atomic_store_n(&displayHealthy, lcdReady ? 1U : 0U, __ATOMIC_RELEASE);
    const uint32_t cycleUs = static_cast<uint32_t>(
        std::min<int64_t>(UINT32_MAX, esp_timer_get_time() - cycleStartedUs));
    __atomic_store_n(&hmiLastCycleUs, cycleUs, __ATOMIC_RELEASE);
    uint32_t previousMax = __atomic_load_n(&hmiMaxCycleUs, __ATOMIC_ACQUIRE);
    while (cycleUs > previousMax &&
           !__atomic_compare_exchange_n(&hmiMaxCycleUs, &previousMax, cycleUs,
                                        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
    uint8_t slowCount = __atomic_load_n(&hmiTripCycleCount, __ATOMIC_ACQUIRE);
    if (cycleUs >= HMI_CYCLE_TRIP_US) {
      if (slowCount < UINT8_MAX) ++slowCount;
    } else {
      slowCount = 0U;
    }
    __atomic_store_n(&hmiTripCycleCount, slowCount, __ATOMIC_RELEASE);
    __atomic_store_n(&hmiHeartbeatMs, millis(), __ATOMIC_RELEASE);
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(HMI_TASK_PERIOD_MS));
  }
}

void networkTask(void *parameter) {
  (void)parameter;
  mayapServiceAdmit(MayapRecovery::Service::Network);
  // A delayed optional task may start after other owners were admitted.
  // Even initial WIFI_OFF must obey the same socket-closure transaction.
  mayapRadioQuiesceBegin();
  while (!mayapOnlineOwnersDrained()) {
    mayapServiceBeat(MayapRecovery::Service::Network);
    vTaskDelay(pdMS_TO_TICKS(NETWORK_FAST_TASK_PERIOD_MS));
  }
  mayapNetworkBegin();
  mayapPrintNetworkConfig();
  __atomic_store_n(&networkReady, 1U, __ATOMIC_RELEASE);
  TickType_t lastWake = xTaskGetTickCount();
  bool memoryPaused = false;
  for (;;) {
    const uint32_t now = millis();
    const bool portalRequested = mayapWifiPortalExclusiveRequested();
    if (portalRequested) mayapRadioQuiesceBegin();
    // A deferred OTA task has no sockets to quiesce. Keep the portal usable
    // during admission without touching the runtime OTA handshake/interlock.
    if (portalRequested && mayapBootStage() < MayapBoot::Stage::Ota) {
      mayapSetWifiPortalOtaQuiesced(true);
    }
    const bool externalIoBusy =
        __atomic_load_n(&mqttIoBusy, __ATOMIC_ACQUIRE) != 0U ||
        __atomic_load_n(&cloudIoBusy, __ATOMIC_ACQUIRE) != 0U;
    if (mayapServiceRecoveryRequested(MayapRecovery::Service::Network)) {
      mayapRequestWifiDeepRecovery();
    }
    if (mayapOnlineMemoryPressure()) {
      memoryPaused = true;
      mayapRadioQuiesceBegin();
      MayapNetworkInternal::publish(NetworkStateCode::Connecting, false);
      mayapServiceBeat(MayapRecovery::Service::Network);
      vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(NETWORK_FAST_TASK_PERIOD_MS));
      continue;
    }
    if (memoryPaused) {
      if (!mayapOnlineOwnersDrained()) {
        mayapServiceBeat(MayapRecovery::Service::Network);
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(NETWORK_FAST_TASK_PERIOD_MS));
        continue;
      }
      if (!portalRequested) mayapRadioQuiesceEnd();
      memoryPaused = false;
    }
    MayapNetworkInternal::tickStableWifi();
    const bool recovering = mayapNetworkDeepRecoveryUpdate(now, externalIoBusy);
    if (!recovering && !(portalRequested && externalIoBusy) &&
        !mayapServiceIsolated(MayapRecovery::Service::Network, now)) {
      mayapNetworkUpdate(now);
    }
    if (!recovering && !portalRequested && mayapServiceRecoveryRequested(MayapRecovery::Service::Network)) {
      mayapServiceRecoveryComplete(MayapRecovery::Service::Network);
    }
    mayapServiceBeat(MayapRecovery::Service::Network);
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(NETWORK_FAST_TASK_PERIOD_MS));
  }
}

// MQTT service: the sole owner of the native MQTT/TLS client and of every Transaction V2
// bridge call. The controller never waits on it (see mqtt_transport.h).
void mqttTask(void *parameter) {
  (void)parameter;
  mayapServiceAdmit(MayapRecovery::Service::Mqtt);
  mayapRealtimeBegin();
  mayapMqttTransportBegin();
  __atomic_store_n(&mqttReady, 1U, __ATOMIC_RELEASE);
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    const uint32_t now = millis();
    if (mayapServiceRecoveryRequested(MayapRecovery::Service::Mqtt)) {
      mayapMqttTransportRecover(now);
      mayapServiceRecoveryComplete(MayapRecovery::Service::Mqtt);
    }
    mayapMqttTransportUpdate(now);
    const bool online = mayapMqttTransportConnected();
    __atomic_store_n(&mqttConnected, online ? 1U : 0U, __ATOMIC_RELEASE);
    mayapSetRealtimeOnline(online);
    mayapServiceBeat(MayapRecovery::Service::Mqtt);
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(MQTT_TASK_PERIOD_MS));
  }
}

// HTTPS Cloudflare co the block vai giay luc TLS/HTTP cham. Cho no mot task
// rieng de canh bao van hoat dong day du nhung realtime MQTT khong bi dong bang.
void cloudTask(void *parameter) {
  (void)parameter;
  mayapServiceAdmit(MayapRecovery::Service::Cloud);
  mayapCloudAlertBegin();
  __atomic_store_n(&cloudReady, 1U, __ATOMIC_RELEASE);
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    const uint32_t now = millis();
    if (mayapServiceRecoveryRequested(MayapRecovery::Service::Cloud)) {
      // HTTP/TLS sessions are scoped and closed before update returns here.
      mayapCloudRecover(now);
      mayapServiceRecoveryComplete(MayapRecovery::Service::Cloud);
    }
    if (mayapWifiPortalExclusiveRequested() || mayapRadioRecoveryRequested() ||
        mayapServiceIsolated(MayapRecovery::Service::Cloud, now)) {
      __atomic_store_n(&cloudIoBusy, 0U, __ATOMIC_RELEASE);
      mayapOnlineOwnerQuiet(MayapRecovery::Service::Cloud);
      mayapServiceBeat(MayapRecovery::Service::Cloud);
      vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(CLOUD_TASK_PERIOD_MS));
      continue;
    }

    if (!mayapOnlineIoEnter(MayapRecovery::Service::Cloud)) {
      mayapServiceBeat(MayapRecovery::Service::Cloud);
      vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(CLOUD_TASK_PERIOD_MS));
      continue;
    }
    __atomic_store_n(&cloudIoBusy, 1U, __ATOMIC_RELEASE);
    if (!mayapWifiPortalExclusiveRequested() && !mayapRadioRecoveryRequested()) {
      mayapCloudAlertUpdate(now);
    }
    __atomic_store_n(&cloudIoBusy, 0U, __ATOMIC_RELEASE);
    mayapOnlineIoLeave(MayapRecovery::Service::Cloud);
    mayapServiceBeat(MayapRecovery::Service::Cloud);
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(CLOUD_TASK_PERIOD_MS));
  }
}

void otaTask(void *parameter) {
  (void)parameter;
  mayapSetRadioOtaQuiesced(false);
  mayapServiceAdmit(MayapRecovery::Service::Ota);
  mayapOtaBegin();
  __atomic_store_n(&otaReady, 1U, __ATOMIC_RELEASE);
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    const uint32_t now = millis();
    if (mayapServiceRecoveryRequested(MayapRecovery::Service::Ota) && mayapOtaRuntimeRecover(now)) {
      mayapServiceRecoveryComplete(MayapRecovery::Service::Ota);
    }
    if (mayapWifiPortalExclusiveRequested() || mayapRadioRecoveryRequested() ||
        mayapServiceIsolated(MayapRecovery::Service::Ota, now)) {
      const bool quiesced = mayapOtaQuiesceForWifiPortal();
      mayapSetWifiPortalOtaQuiesced(quiesced);
      mayapSetRadioOtaQuiesced(quiesced);
      if (quiesced) mayapOnlineOwnerQuiet(MayapRecovery::Service::Ota);
      if (!quiesced) mayapOtaUpdate(now);
      mayapServiceBeat(MayapRecovery::Service::Ota);
      vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(OTA_TASK_PERIOD_MS));
      continue;
    }
    if (!mayapOnlineIoEnter(MayapRecovery::Service::Ota)) {
      mayapServiceBeat(MayapRecovery::Service::Ota);
      vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(OTA_TASK_PERIOD_MS));
      continue;
    }
    mayapSetRadioOtaQuiesced(false);
    mayapSetWifiPortalOtaQuiesced(false);
    mayapOtaUpdate(now);
    mayapFirmwareWebUpdate(now);
    mayapFirmwareRollbackUpdate(now);
    mayapOnlineIoLeave(MayapRecovery::Service::Ota);
    mayapServiceBeat(MayapRecovery::Service::Ota);
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(OTA_TASK_PERIOD_MS));
  }
}

void supervisorTask(void *parameter) {
  (void)parameter;
  subscribeCurrentTaskToWdt("SUP WDT ADD");
  TickType_t lastWake = xTaskGetTickCount();
  bool previousHmiHealthy = true;

  for (;;) {
    const uint32_t now = millis();
    const uint32_t ctrlBeat = __atomic_load_n(&controlHeartbeatMs, __ATOMIC_ACQUIRE);
    mayapSerialDrain(); // bounded partial writes; never block controlTask for logging
    const uint32_t hmiBeat = __atomic_load_n(&hmiHeartbeatMs, __ATOMIC_ACQUIRE);

    // Do not supervise a task before staged startup has admitted it. Once
    // admitted, retain EVERY original heartbeat/deadline trip threshold.
    const bool localAdmitted = mayapBootStage() >= MayapBoot::Stage::ControlSafety;
    const bool controlExpected = localAdmitted && __atomic_load_n(&controlStarted, __ATOMIC_ACQUIRE);
    const bool hmiExpected = localAdmitted && __atomic_load_n(&hmiStarted, __ATOMIC_ACQUIRE);
    const bool controlHealthy = !controlExpected || (ctrlBeat != 0U &&
        elapsedMs(now, ctrlBeat) <= CONTROL_HEARTBEAT_TIMEOUT_MS);
    const bool hmiHealthy = !hmiExpected || (hmiBeat != 0U &&
        elapsedMs(now, hmiBeat) <= HMI_HEARTBEAT_TIMEOUT_MS);
    const uint8_t hmiSlowCycles = __atomic_load_n(&hmiTripCycleCount, __ATOMIC_ACQUIRE);
    const bool hmiFatal = hmiExpected &&
        (hmiBeat == 0U || elapsedMs(now, hmiBeat) >= HMI_FATAL_HEARTBEAT_TIMEOUT_MS ||
         hmiSlowCycles >= HMI_CYCLE_TRIP_COUNT);

    const uint8_t slowCycles = __atomic_load_n(&controlTripCycleCount, __ATOMIC_ACQUIRE);
    const bool deadlineTrip = controlExpected && slowCycles >= CONTROL_CYCLE_TRIP_COUNT;

    if (!controlHealthy || deadlineTrip) {
      // Snapshot the evidence first: it is what the next boot reports.
      const char *reasonText = !controlHealthy ? "HEARTBEAT" : "DEADLINE";
      const uint32_t heartbeatAgeMs = ctrlBeat != 0U ? elapsedMs(now, ctrlBeat) : 0U;
      const uint32_t cycleUs = __atomic_load_n(&controlLastCycleUs, __ATOMIC_ACQUIRE);
      const char *stageText = MayapBoot::stageText(mayapBootStage());
      // Sampled BEFORE the suspend below: is control Running/Ready (starved or
      // spinning) or Blocked (waiting on a lock/delay)?
      const unsigned phase = __atomic_load_n(&controlPhase, __ATOMIC_ACQUIRE);
      const unsigned inUpdate = __atomic_load_n(&controlInUpdate, __ATOMIC_ACQUIRE);
      const uint32_t cycleStartMs = __atomic_load_n(&controlCycleStartMs, __ATOMIC_ACQUIRE);
      const uint32_t cycleAgeMs = cycleStartMs != 0U ? elapsedMs(now, cycleStartMs) : 0U;
      char taskState = '?';
      if (controlTaskHandle) {
        switch (eTaskGetState(controlTaskHandle)) {
          case eRunning: taskState = 'R'; break;
          case eReady: taskState = 'Y'; break;
          case eBlocked: taskState = 'B'; break;
          case eSuspended: taskState = 'S'; break;
          default: taskState = 'D'; break;
        }
      }
      const MayapBoot::RestartReason tripReason = !controlHealthy ?
          MayapBoot::RestartReason::ControlHeartbeat : MayapBoot::RestartReason::ControlDeadline;
      mayapLatchSystemTrip();
      if (controlTaskHandle) vTaskSuspend(controlTaskHandle);
      mayapSafeOutputsEarly();
      char detail[sizeof(MayapBoot::Diagnostic::restartDetail)];
      // Persist reason/age/cycle/slow/stage BEFORE any heap walk or Serial work, so
      // even a TWDT reset leaves the cause in RTC for [BOOT-DIAG].
      snprintf(detail, sizeof(detail), "%s age=%lums cyc=%luus slow=%u stg=%s",
               reasonText, static_cast<unsigned long>(heartbeatAgeMs),
               static_cast<unsigned long>(cycleUs), static_cast<unsigned>(slowCycles), stageText);
      mayapBootPlanRestart(tripReason, detail);
      const unsigned long heapFree = ESP.getFreeHeap();
      const unsigned long heapMin = ESP.getMinFreeHeap();
      const unsigned long heapLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      snprintf(detail, sizeof(detail), "%s age=%lums cyc=%luus slow=%u stg=%s h=%lu/%lu/%lu ph=%u in=%u ca=%lums ts=%c",
               reasonText, static_cast<unsigned long>(heartbeatAgeMs),
               static_cast<unsigned long>(cycleUs), static_cast<unsigned>(slowCycles), stageText,
               heapFree, heapMin, heapLargest, phase, inUpdate,
               static_cast<unsigned long>(cycleAgeMs), taskState);
      mayapBootPlanRestart(tripReason, detail);
      mayapSerialPrintf(true,
          "[SUPERVISOR] TRIP reason=%s hbAge=%lums cycleUs=%lu slow=%u stage=%s heap=%lu/%lu/%lu "
          "phase=%u inUpdate=%u currentCycleAge=%lums ctrlState=%c\n",
          reasonText, static_cast<unsigned long>(heartbeatAgeMs), static_cast<unsigned long>(cycleUs),
          static_cast<unsigned>(slowCycles), stageText, heapFree, heapMin, heapLargest,
          phase, inUpdate, static_cast<unsigned long>(cycleAgeMs), taskState);
      // Print now: the fallback below outlives the 5 s TWDT, so a queued line would
      // otherwise be lost. Bounded (<150 ms); TWDT timeout and membership unchanged.
      mayapSerialDrainFor(150U);
      const uint32_t tripAt = now;
      while (elapsedMs(millis(), tripAt) < SUPERVISOR_RESTART_FALLBACK_MS) {
        mayapSafeOutputsEarly();
        mayapSerialDrain();
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      mayapRestart(tripReason, detail);  // keep the evidence, not a generic label
    }

    if (hmiBeat != 0U && hmiHealthy != previousHmiHealthy) {
      previousHmiHealthy = hmiHealthy;
      mayapSerialPrintf(false, "[SUPERVISOR] HMI %s cycle=%luus slow=%u\n",
                        hmiHealthy ? "RECOVERED" : "HEARTBEAT SLOW",
                        static_cast<unsigned long>(__atomic_load_n(&hmiLastCycleUs, __ATOMIC_ACQUIRE)),
                        static_cast<unsigned>(hmiSlowCycles));
    }

    if (hmiFatal) {
      mayapLatchSystemTrip();
      if (controlTaskHandle) vTaskSuspend(controlTaskHandle);
      if (hmiTaskHandle) vTaskSuspend(hmiTaskHandle);
      mayapSafeOutputsEarly();
      mayapSerialPrintf(false,
          "[SUPERVISOR] HMI FATAL heartbeatAge=%lums cycle=%luus slow=%u -> RESTART\n",
          static_cast<unsigned long>(elapsedMs(now, hmiBeat)),
          static_cast<unsigned long>(__atomic_load_n(&hmiLastCycleUs, __ATOMIC_ACQUIRE)),
          static_cast<unsigned>(hmiSlowCycles));
      mayapRestart(MayapBoot::RestartReason::HmiFatal, "Supervisor HMI fatal");
    }

    MayapRecovery::Service failedService = MayapRecovery::Service::Network;
    (void)failedService;
    // Network/Realtime/Cloud/OTA may degrade, but never own controller reset.
    mayapServiceSupervisorUpdate(now);
    const esp_err_t result = esp_task_wdt_reset();
    if (result != ESP_OK) fatalRestart("SUP WDT RESET", result, MayapBoot::RestartReason::WdtApi);
    __atomic_store_n(&supervisorHeartbeatMs, millis(), __ATOMIC_RELEASE);
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SUPERVISOR_TASK_PERIOD_MS));
  }
}

static void advanceBootStage() {
  bootSequence.advance(millis());
  bootStageEntered = false;
  mayapBootSetStage(bootSequence.stage());
}

static bool localTasksHealthy(uint32_t now) {
  const uint32_t ctrl = __atomic_load_n(&controlHeartbeatMs, __ATOMIC_ACQUIRE);
  const uint32_t hmi = __atomic_load_n(&hmiHeartbeatMs, __ATOMIC_ACQUIRE);
  const uint32_t sup = __atomic_load_n(&supervisorHeartbeatMs, __ATOMIC_ACQUIRE);
  return __atomic_load_n(&controlStarted, __ATOMIC_ACQUIRE) &&
      __atomic_load_n(&hmiStarted, __ATOMIC_ACQUIRE) && ctrl && hmi && sup &&
      elapsedMs(now, ctrl) <= CONTROL_HEARTBEAT_TIMEOUT_MS &&
      elapsedMs(now, hmi) <= HMI_HEARTBEAT_TIMEOUT_MS &&
      elapsedMs(now, sup) <= CONTROL_HEARTBEAT_TIMEOUT_MS &&
      __atomic_load_n(&controlTripCycleCount, __ATOMIC_ACQUIRE) == 0U &&
      __atomic_load_n(&hmiTripCycleCount, __ATOMIC_ACQUIRE) == 0U &&
      !mayapSystemTripLatched();
}

static void updateBootStability(uint32_t now) {
  const bool tasksHealthy = localTasksHealthy(now);
  // Task stability permits networking even while the operator diagnoses a
  // sensor/display fault. Such a fault still prevents BOOT_SUCCESS/level clear.
  localTaskStability.update(now, tasksHealthy);
  localSuccessStability.update(now, tasksHealthy &&
      __atomic_load_n(&sensorHealthy, __ATOMIC_ACQUIRE) &&
      __atomic_load_n(&displayHealthy, __ATOMIC_ACQUIRE));
  if (!bootSuccessMarked && localSuccessStability.held(now, MayapBoot::SUCCESS_STABLE_MS)) {
    mayapBootMarkSuccess();
    bootSuccessMarked = true;
  }
  if (!bootFailuresCleared && bootSequence.stage() == MayapBoot::Stage::Running &&
      localSuccessStability.held(now, MayapBoot::FAILURES_CLEAR_MS)) {
    mayapBootClearFailures();
    bootFailuresCleared = true;
  }
  // Home has its own LOCAL deadline. Connection success/timeouts do not enter
  // this decision. Levels 2/3 release Home before any network task exists.
  const uint32_t homeDelay = bootSequence.homeBeforeNetwork()
      ? MayapBoot::LOCAL_SETTLE_MS : MayapBoot::LOCAL_SETTLE_MS + 1500U;
  if (!bootReadyShown && localTaskStability.held(now, homeDelay)) {
    mayapBootShowReady();
    bootReadyShown = true;
    bootReadyAt = now;
  }
  if (bootReadyShown && elapsedMs(now, bootReadyAt) >= MayapBoot::READY_DISPLAY_MS) {
    mayapBootReleaseHome();
  }
}

static void stagedStartupUpdate(uint32_t now) {
  using MayapBoot::Stage;
  updateBootStability(now);
  const Stage stage = bootSequence.stage();
  if (!bootStageEntered) {
    // Set before calls that may fail, so retained breadcrumbs name the operation.
    bootStageEntered = true;
    switch (stage) {
      case Stage::SafeOutputs: break;  // Already applied before all startup I/O.
      case Stage::Storage:
        mayapDeviceIdentityBegin();
        Machine.beginStorage();
        break;
      case Stage::SensorMachine:
        Machine.begin();
        break;
      case Stage::Hmi:
        hmiSetConfig(Machine.config());
        hmiSetRuntime(Machine.runtime());
        mayapRealtimeSetConfig(Machine.config());
        mayapRealtimeSetRuntime(Machine.runtime());
        mayapCloudSetRuntime(Machine.runtime());
        hmiBegin();
        __atomic_store_n(&hmiHeartbeatMs, millis(), __ATOMIC_RELEASE);
        __atomic_store_n(&hmiStarted, 1U, __ATOMIC_RELEASE);
        hmiTaskHandle = xTaskCreateStaticPinnedToCore(
            hmiTask, "mayap_hmi", sizeof(hmiTaskStack), nullptr, 2,
            hmiTaskStack, &hmiTaskTcb, 1);
        if (!hmiTaskHandle) fatalRestart("HMI TASK CREATE", ESP_ERR_NO_MEM);
        break;
      case Stage::ControlSafety:
        // Rollback capability is local flash metadata; keep the existing HMI
        // recovery action available even while Internet OTA is deferred.
        mayapOtaRollbackBegin();
        __atomic_store_n(&controlHeartbeatMs, millis(), __ATOMIC_RELEASE);
        __atomic_store_n(&controlStarted, 1U, __ATOMIC_RELEASE);
        controlTaskHandle = xTaskCreateStaticPinnedToCore(
            controlTask, "mayap_ctrl", sizeof(controlTaskStack), nullptr, 5,
            controlTaskStack, &controlTaskTcb, 1);
        if (!controlTaskHandle) fatalRestart("CTRL TASK CREATE", ESP_ERR_NO_MEM);
        supervisorTaskHandle = xTaskCreateStaticPinnedToCore(
            supervisorTask, "mayap_supervisor", sizeof(supervisorTaskStack), nullptr, 6,
            supervisorTaskStack, &supervisorTaskTcb, 1);
        if (!supervisorTaskHandle) fatalRestart("SUP TASK CREATE", ESP_ERR_NO_MEM);
        break;
      case Stage::LocalSettle: break;
      case Stage::Wifi:
        networkTaskHandle = xTaskCreateStaticPinnedToCore(
            networkTask, "mayap_network", sizeof(networkTaskStack), nullptr, 1,
            networkTaskStack, &networkTaskTcb, 0);
        if (!networkTaskHandle) {
          mayapOnlineUnavailable(MayapRecovery::Service::Network);
          mayapSerialPrintf(false, "[ONLINE] WIFI task unavailable - LOCAL CONTROL CONTINUES\n");
        }
        break;
      case Stage::Mqtt:
        mqttTaskHandle = xTaskCreateStaticPinnedToCore(
            mqttTask, "mayap_mqtt", sizeof(mqttTaskStack), nullptr, 2,
            mqttTaskStack, &mqttTaskTcb, 0);
        if (!mqttTaskHandle) {
          mayapOnlineUnavailable(MayapRecovery::Service::Mqtt);
          mayapSerialPrintf(false, "[ONLINE] MQTT task unavailable - LOCAL CONTROL CONTINUES\n");
        }
        break;
      case Stage::Cloud:
        cloudTaskHandle = xTaskCreateStaticPinnedToCore(
            cloudTask, "mayap_cloud", sizeof(cloudTaskStack), nullptr, 1,
            cloudTaskStack, &cloudTaskTcb, 0);
        if (!cloudTaskHandle) {
          mayapOnlineUnavailable(MayapRecovery::Service::Cloud);
          mayapSerialPrintf(false, "[ONLINE] CLOUD task unavailable - LOCAL CONTROL CONTINUES\n");
        }
        break;
      case Stage::Ota:
        otaTaskHandle = xTaskCreateStaticPinnedToCore(
            otaTask, "mayap_ota", sizeof(otaTaskStack), nullptr, 1,
            otaTaskStack, &otaTaskTcb, 0);
        if (!otaTaskHandle) {
          mayapOnlineUnavailable(MayapRecovery::Service::Ota);
          mayapSerialPrintf(false, "[ONLINE] OTA task unavailable - LOCAL CONTROL CONTINUES\n");
        }
        break;
      case Stage::Running:
        // Preserve the runtime WDT membership (control + Supervisor). The
        // coordinator is watched only while staged startup is still in flight.
        if (bootCoordinatorWdt) {
          const esp_err_t result = esp_task_wdt_delete(nullptr);
          if (result != ESP_OK) fatalRestart("BOOT WDT DELETE", result, MayapBoot::RestartReason::WdtApi);
          bootCoordinatorWdt = false;
        }
        break;
    }
  }
  // Short cooperative dwell lets the LCD show every local step. No blocking
  // delay and no network calls execute on the boot/control/HMI coordinator.
  if (stage <= Stage::ControlSafety) {
    if (bootSequence.age(millis()) >= 150U) advanceBootStage();
    return;
  }
  switch (stage) {
    case Stage::LocalSettle:
      if (bootSequence.releaseNetwork(now, localTaskStability)) advanceBootStage();
      break;
    case Stage::Wifi:
      if (bootSequence.wifiDone(now, __atomic_load_n(&networkReady, __ATOMIC_ACQUIRE) ||
          bootSequence.age(now) >= MayapBoot::WIFI_WAIT_MS,
          mayapGetNetworkStatus().connected)) advanceBootStage();
      break;
    case Stage::Mqtt:
      if (bootSequence.mqttDone(now, __atomic_load_n(&mqttReady, __ATOMIC_ACQUIRE) ||
          bootSequence.age(now) >= MayapBoot::MQTT_WAIT_MS,
          __atomic_load_n(&mqttConnected, __ATOMIC_ACQUIRE))) advanceBootStage();
      break;
    case Stage::Cloud:
      if (bootSequence.age(now) >= bootSequence.serviceGap())
        advanceBootStage();
      break;
    case Stage::Ota:
      if (bootSequence.age(now) >= bootSequence.serviceGap())
        advanceBootStage();
      break;
    default: break;
  }
}

void setup() {
  mayapSafeOutputsEarly();
  mayapBootDiagnosticBegin();
  Serial.begin(115200);

  const MayapBoot::Diagnostic diagnostic = mayapBootDiagnosticSnapshot();
  mayapSerialPrintf(true,
      "[BOOT-DIAG] reset=%lu previousReset=%lu previousStage=%s planned=%s detail=%s failed=%lu level=%lu\n",
      static_cast<unsigned long>(diagnostic.resetReason),
      static_cast<unsigned long>(diagnostic.previousResetReason),
      MayapBoot::stageText(diagnostic.previousBootStage),
      MayapBoot::restartText(diagnostic.previousPlannedRestartReason), diagnostic.previousRestartDetail,
      static_cast<unsigned long>(diagnostic.consecutiveFailedBoots),
      static_cast<unsigned long>(diagnostic.recoveryLevel));
  bootSequence.begin(diagnostic.recoveryLevel, millis());
  mayapBootSetStage(MayapBoot::Stage::SafeOutputs);

  i2cMutex = xSemaphoreCreateMutexStatic(&i2cMutexBuffer);
  if (!i2cMutex) fatalRestart("I2C MUTEX", ESP_ERR_NO_MEM);

  if (!Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_CLOCK_HZ)) {
    fatalRestart("I2C BEGIN", ESP_FAIL);
  }
  Wire.setTimeOut(I2C_TIMEOUT_MS);
  hmiSetI2cLockCallbacks(mayapI2cLock, mayapI2cUnlock);

  esp_task_wdt_config_t wdtConfig{};
  wdtConfig.timeout_ms = CONTROL_WDT_TIMEOUT_MS;
  wdtConfig.idle_core_mask = 0U;
  wdtConfig.trigger_panic = true;
  esp_err_t result = esp_task_wdt_reconfigure(&wdtConfig);
  if (result == ESP_ERR_INVALID_STATE) result = esp_task_wdt_init(&wdtConfig);
  if (result != ESP_OK) fatalRestart("WDT INIT", result);

  subscribeCurrentTaskToWdt("BOOT WDT ADD");
  bootCoordinatorWdt = true;
  hmiBootDisplayBegin();
}

void loop() {
  if (bootCoordinatorWdt) {
    const esp_err_t result = esp_task_wdt_reset();
    if (result != ESP_OK) fatalRestart("BOOT WDT RESET", result, MayapBoot::RestartReason::WdtApi);
  }
  // Before hmiTask creation this loop is the sole LCD owner.
  if (!hmiTaskHandle) hmiBootDisplayUpdate(millis());
  stagedStartupUpdate(millis());
  if(mayapBootStage() >= MayapBoot::Stage::LocalSettle && !mayapFirmwareMaintenanceActive())
    MayapAdaptive::modelStorage.service(millis());
  if(mayapBootStage() >= MayapBoot::Stage::LocalSettle && !mayapFirmwareMaintenanceActive())
  if (mayapBootStage() >= MayapBoot::Stage::LocalSettle) mayapI2cSupervisorUpdate(millis());
  vTaskDelay(pdMS_TO_TICKS(10));
}
