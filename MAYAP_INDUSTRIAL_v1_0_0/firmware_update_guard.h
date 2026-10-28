#pragma once
#include <stdint.h>
namespace MayapFirmwareGuardInternal {
static uint8_t ready = 0U;
static uint8_t active = 0U;
static volatile uint32_t heaterHoldUntil = 0U;   // 0 = no hold; otherwise millis() until which the heater request is held off
}
inline bool mayapFirmwareMaintenanceReady() {
  return __atomic_load_n(&MayapFirmwareGuardInternal::ready, __ATOMIC_ACQUIRE) != 0U;
}
inline void mayapSetFirmwareMaintenanceReady(bool ready) {
  __atomic_store_n(&MayapFirmwareGuardInternal::ready, ready ? 1U : 0U, __ATOMIC_RELEASE);
}
inline bool mayapFirmwareMaintenanceActive() {
  return __atomic_load_n(&MayapFirmwareGuardInternal::active, __ATOMIC_ACQUIRE) != 0U;
}
inline void mayapSetFirmwareMaintenanceActive(bool active) {
  __atomic_store_n(&MayapFirmwareGuardInternal::active, active ? 1U : 0U, __ATOMIC_RELEASE);
}
// The heater is held off only for the final flash write of an update. The hold expires by itself and is dropped whenever the
// maintenance scope below ends (success, failure, early return), so no path can leave the heater off.
inline void mayapFirmwareHeaterHoldFor(uint32_t now, uint32_t ms) {
  uint32_t until = now + ms;
  if (until == 0U) until = 1U;
  __atomic_store_n(&MayapFirmwareGuardInternal::heaterHoldUntil, until, __ATOMIC_RELEASE);
}
inline void mayapFirmwareHeaterHoldClear() {
  __atomic_store_n(&MayapFirmwareGuardInternal::heaterHoldUntil, 0U, __ATOMIC_RELEASE);
}
inline bool mayapFirmwareHeaterHoldActive(uint32_t now) {
  const uint32_t until = __atomic_load_n(&MayapFirmwareGuardInternal::heaterHoldUntil, __ATOMIC_ACQUIRE);
  return until != 0U && static_cast<int32_t>(until - now) > 0;
}
class MayapFirmwareMaintenance {
 public:
  MayapFirmwareMaintenance() {
    mayapSetFirmwareMaintenanceActive(true);
  }
  ~MayapFirmwareMaintenance() {
    mayapFirmwareHeaterHoldClear();
    mayapSetFirmwareMaintenanceActive(false);
  }
  MayapFirmwareMaintenance(const MayapFirmwareMaintenance &) = delete;
  MayapFirmwareMaintenance &operator=(const MayapFirmwareMaintenance &) = delete;
};
