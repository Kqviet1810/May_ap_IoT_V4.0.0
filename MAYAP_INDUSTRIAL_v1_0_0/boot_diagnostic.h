#pragma once

#include "config.h"
#include "boot_policy.h"
#include <esp_attr.h>
#include <esp_system.h>

namespace MayapBootInternal {
// RTC_DATA_ATTR alone is reinitialized on software reset in ESP-IDF.
// Keep the requested live RTC snapshot AND a validated noinit backing record.
RTC_DATA_ATTR static MayapBoot::Diagnostic diagnostic = {};
RTC_NOINIT_ATTR static MayapBoot::Diagnostic retained[2];
static uint8_t retainedSlot = 0U;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t currentStage = 0U;
static volatile uint8_t homeReleased = 0U;
static volatile uint8_t operationsReady = 0U;
static volatile uint8_t readyStatus = 0U;
// Boot diagnostic screen (shown instead of the logo when the Home gate is still closed 30 s after power-up).
static volatile uint8_t diagActive = 0U;
static volatile uint16_t diagCode = 0U;
static volatile uint8_t diagReason = 0U;
static volatile uint32_t diagAgeMs = 0U;
static volatile uint8_t homeRequested = 0U;

inline void persistUnlocked() {
  ++diagnostic.sequence;
  diagnostic.checksum = MayapBoot::checksum(diagnostic);
  const uint8_t nextSlot = retainedSlot ^ 1U;
  // Preserve the previous valid slot if panic/WDT interrupts this copy.
  retained[nextSlot].checksum = 0U;
  retained[nextSlot] = diagnostic;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  retainedSlot = nextSlot;
}
}  // namespace MayapBootInternal

inline MayapBoot::Diagnostic mayapBootDiagnosticSnapshot() {
  portENTER_CRITICAL(&MayapBootInternal::mux);
  const MayapBoot::Diagnostic copy = MayapBootInternal::diagnostic;
  portEXIT_CRITICAL(&MayapBootInternal::mux);
  return copy;
}
inline void mayapBootDiagnosticBegin() {
  __atomic_store_n(&MayapBootInternal::homeReleased, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&MayapBootInternal::operationsReady, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&MayapBootInternal::readyStatus, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&MayapBootInternal::diagActive, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&MayapBootInternal::homeRequested, 0U, __ATOMIC_RELEASE);
  using namespace MayapBoot;
  const esp_reset_reason_t reason = esp_reset_reason();
  const ResetKind kind = reason == ESP_RST_POWERON ? ResetKind::PowerOn :
      reason == ESP_RST_BROWNOUT ? ResetKind::Brownout :
      reason == ESP_RST_SW ? ResetKind::Software :
      reason == ESP_RST_DEEPSLEEP ? ResetKind::DeepSleep : ResetKind::Unexpected;
  portENTER_CRITICAL(&MayapBootInternal::mux);
  MayapBootInternal::retainedSlot = newestSlot(MayapBootInternal::retained[0], MayapBootInternal::retained[1]);
  const Diagnostic &old = MayapBootInternal::retained[MayapBootInternal::retainedSlot];
  MayapBootInternal::diagnostic = beginDiagnostic(old, reason, kind);
  if (kind == ResetKind::PowerOn || !valid(old)) {
    MayapBootInternal::retained[0].checksum = 0U;
    MayapBootInternal::retained[1].checksum = 0U;
  }
  MayapBootInternal::persistUnlocked();
  portEXIT_CRITICAL(&MayapBootInternal::mux);
}
inline MayapBoot::Stage mayapBootStage() {
  return static_cast<MayapBoot::Stage>(__atomic_load_n(&MayapBootInternal::currentStage, __ATOMIC_ACQUIRE));
}
inline void mayapBootSetStage(MayapBoot::Stage stage) {
  portENTER_CRITICAL(&MayapBootInternal::mux);
  MayapBootInternal::diagnostic.lastBootStage = stage;
  MayapBootInternal::persistUnlocked();
  __atomic_store_n(&MayapBootInternal::currentStage, static_cast<uint32_t>(stage), __ATOMIC_RELEASE);
  portEXIT_CRITICAL(&MayapBootInternal::mux);
  mayapSerialPrintf(false, "[BOOT-STAGE] %s\n", MayapBoot::stageText(stage));
}
inline void mayapBootPlanRestart(MayapBoot::RestartReason reason, const char *detail) {
  portENTER_CRITICAL(&MayapBootInternal::mux);
  MayapBootInternal::diagnostic.plannedRestartReason = reason;
  memset(MayapBootInternal::diagnostic.restartDetail, 0, sizeof(MayapBootInternal::diagnostic.restartDetail));
  if (detail) strncpy(MayapBootInternal::diagnostic.restartDetail, detail,
                      sizeof(MayapBootInternal::diagnostic.restartDetail) - 1U);
  MayapBootInternal::persistUnlocked();
  portEXIT_CRITICAL(&MayapBootInternal::mux);
}
[[noreturn]] inline void mayapRestart(MayapBoot::RestartReason reason, const char *detail) {
  mayapBootPlanRestart(reason, detail);
  esp_restart();
  abort();
}
inline void mayapBootMarkSuccess() {
  portENTER_CRITICAL(&MayapBootInternal::mux);
  MayapBootInternal::diagnostic.bootCompleted = 1U;
  MayapBootInternal::persistUnlocked();
  portEXIT_CRITICAL(&MayapBootInternal::mux);
  mayapSerialPrintf(false, "[BOOT_SUCCESS] local healthy continuously for 25s\n");
}
inline void mayapBootClearFailures() {
  portENTER_CRITICAL(&MayapBootInternal::mux);
  MayapBootInternal::diagnostic.consecutiveFailedBoots = 0U;
  MayapBootInternal::diagnostic.recoveryLevel = 0U;
  MayapBootInternal::persistUnlocked();
  portEXIT_CRITICAL(&MayapBootInternal::mux);
  mayapSerialPrintf(false, "[BOOT-RECOVERY] stable 10min -> level=0 failed=0\n");
}
inline bool mayapBootHomeReleased() {
  return __atomic_load_n(&MayapBootInternal::homeReleased, __ATOMIC_ACQUIRE) != 0U;
}
inline void mayapBootReleaseHome() {
  __atomic_store_n(&MayapBootInternal::homeReleased, 1U, __ATOMIC_RELEASE);
}
// Coordinator release only requests the UI transition. Outputs are permitted
// AFTER HMI has sent a non-splash frame, never while a busy I2C bus delays it.
inline bool mayapBootOperationsReady() {
  return __atomic_load_n(&MayapBootInternal::operationsReady, __ATOMIC_ACQUIRE) != 0U;
}
inline void mayapBootAcknowledgeHomeFrame() {
  __atomic_store_n(&MayapBootInternal::operationsReady, 1U, __ATOMIC_RELEASE);
}
// Coordinator -> HMI: the gate is still closed after the 30 s deadline; show WHY (code + reason), never "ready".
inline void mayapBootPublishDiagnostic(uint16_t code, MayapBoot::BlockReason reason, uint32_t ageMs) {
  __atomic_store_n(&MayapBootInternal::diagCode, code, __ATOMIC_RELEASE);
  __atomic_store_n(&MayapBootInternal::diagReason, static_cast<uint8_t>(reason), __ATOMIC_RELEASE);
  __atomic_store_n(&MayapBootInternal::diagAgeMs, ageMs, __ATOMIC_RELEASE);
  __atomic_store_n(&MayapBootInternal::diagActive, 1U, __ATOMIC_RELEASE);
}
inline void mayapBootClearDiagnostic() { __atomic_store_n(&MayapBootInternal::diagActive, 0U, __ATOMIC_RELEASE); }
inline bool mayapBootDiagnosticActive() { return __atomic_load_n(&MayapBootInternal::diagActive, __ATOMIC_ACQUIRE) != 0U; }
inline uint16_t mayapBootDiagnosticCode() { return __atomic_load_n(&MayapBootInternal::diagCode, __ATOMIC_ACQUIRE); }
inline MayapBoot::BlockReason mayapBootDiagnosticReason() {
  return static_cast<MayapBoot::BlockReason>(__atomic_load_n(&MayapBootInternal::diagReason, __ATOMIC_ACQUIRE));
}
inline uint32_t mayapBootDiagnosticAgeMs() { return __atomic_load_n(&MayapBootInternal::diagAgeMs, __ATOMIC_ACQUIRE); }
// Operator pressed the button on the diagnostic screen: continue to Home (alarms there still show the real fault).
inline void mayapBootRequestHome() { __atomic_store_n(&MayapBootInternal::homeRequested, 1U, __ATOMIC_RELEASE); }
inline bool mayapBootHomeRequested() { return __atomic_load_n(&MayapBootInternal::homeRequested, __ATOMIC_ACQUIRE) != 0U; }
inline void mayapBootShowReady() {
  __atomic_store_n(&MayapBootInternal::readyStatus, 1U, __ATOMIC_RELEASE);
}
inline MayapBoot::Status mayapBootStatus() {
  return __atomic_load_n(&MayapBootInternal::readyStatus, __ATOMIC_ACQUIRE)
      ? MayapBoot::Status::Ready : MayapBoot::statusFor(mayapBootStage());
}
