#pragma once
#include <stdint.h>
namespace MayapFirmwareGuardInternal {
static uint8_t ready = 0U;
static uint8_t active = 0U;
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
class MayapFirmwareMaintenance {
 public:
  MayapFirmwareMaintenance() {
    mayapSetFirmwareMaintenanceActive(true);
  }
  ~MayapFirmwareMaintenance() {
    mayapSetFirmwareMaintenanceActive(false);
  }
  MayapFirmwareMaintenance(const MayapFirmwareMaintenance &) = delete;
  MayapFirmwareMaintenance &operator=(const MayapFirmwareMaintenance &) = delete;
};
