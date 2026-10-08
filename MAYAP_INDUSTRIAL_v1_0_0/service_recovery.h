#pragma once
#include "config.h"
#include "runtime_recovery_policy.h"
#include "online_isolation.h"

namespace MayapServiceInternal {
struct Slot {
  uint32_t beat = 0U, ack = 0U, admitted = 0U, request = 0U;
  uint32_t isolateAt = 0U, isolated = 0U, degraded = 0U;
};
static Slot slots[4];
static MayapOnline::RadioGate radioGate;
static uint32_t radioQuiesce = 0U;
static uint32_t memoryPressure = 0U;
static uint32_t realtimeOnline = 0U;
static uint32_t otaQuiesced = 1U;
static const char *const names[] = {"NETWORK", "MQTT", "CLOUD", "OTA"};
}
inline void mayapServiceBeat(MayapRecovery::Service service) {
  __atomic_store_n(&MayapServiceInternal::slots[static_cast<uint8_t>(service)].beat, millis(), __ATOMIC_RELEASE);
}
inline void mayapServiceAdmit(MayapRecovery::Service service) {
  mayapServiceBeat(service);
  __atomic_store_n(&MayapServiceInternal::slots[static_cast<uint8_t>(service)].admitted, 1U, __ATOMIC_RELEASE);
}
inline bool mayapServiceRecoveryRequested(MayapRecovery::Service service) {
  return __atomic_load_n(&MayapServiceInternal::slots[static_cast<uint8_t>(service)].request, __ATOMIC_ACQUIRE) != 0U;
}
inline void mayapServiceRecoveryComplete(MayapRecovery::Service service) {
  auto &slot = MayapServiceInternal::slots[static_cast<uint8_t>(service)];
  __atomic_store_n(&slot.request, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&slot.degraded, 0U, __ATOMIC_RELEASE);
  __atomic_add_fetch(&slot.ack, 1U, __ATOMIC_ACQ_REL);
  mayapServiceBeat(service);
}
inline bool mayapServiceIsolated(MayapRecovery::Service service, uint32_t now) {
  auto &slot = MayapServiceInternal::slots[static_cast<uint8_t>(service)];
  if (!__atomic_load_n(&slot.isolated, __ATOMIC_ACQUIRE)) return false;
  if (MayapRecovery::age(now, __atomic_load_n(&slot.isolateAt, __ATOMIC_ACQUIRE)) < MayapRecovery::ISOLATE_PAUSE_MS) return true;
  __atomic_store_n(&slot.isolated, 0U, __ATOMIC_RELEASE);
  return false;
}
inline uint8_t mayapServiceDegradedMask() {
  uint8_t mask = __atomic_load_n(&MayapServiceInternal::memoryPressure, __ATOMIC_ACQUIRE) ? 0x0EU : 0U;
  for (uint8_t i = 0U; i < 4U; ++i)
    if (__atomic_load_n(&MayapServiceInternal::slots[i].degraded, __ATOMIC_ACQUIRE))
      mask |= static_cast<uint8_t>(1U << i);
  return mask;
}
inline void mayapSetRealtimeOnline(bool connected) {
  __atomic_store_n(&MayapServiceInternal::realtimeOnline, connected ? 1U : 0U, __ATOMIC_RELEASE);
}
inline bool mayapRealtimeOnline() {
  return __atomic_load_n(&MayapServiceInternal::realtimeOnline, __ATOMIC_ACQUIRE) != 0U;
}
inline void mayapSetOnlineMemoryPressure(bool paused) {
  __atomic_store_n(&MayapServiceInternal::memoryPressure, paused ? 1U : 0U, __ATOMIC_RELEASE);
}
inline bool mayapOnlineMemoryPressure() {
  return __atomic_load_n(&MayapServiceInternal::memoryPressure, __ATOMIC_ACQUIRE) != 0U;
}
inline bool mayapOnlineIoEnter(MayapRecovery::Service owner) {
  return !mayapOnlineMemoryPressure() && MayapServiceInternal::radioGate.enter(static_cast<uint8_t>(owner));
}
inline void mayapOnlineIoLeave(MayapRecovery::Service owner) {
  MayapServiceInternal::radioGate.leave(static_cast<uint8_t>(owner));
}
inline void mayapOnlineOwnerQuiet(MayapRecovery::Service owner) {
  MayapServiceInternal::radioGate.acknowledge(static_cast<uint8_t>(owner));
}
inline void mayapRadioQuiesceBegin() { MayapServiceInternal::radioGate.closeAdmission(); }
inline bool mayapOnlineOwnersDrained() {
  uint8_t owners = 0U;
  for (uint8_t i = 1U; i < 4U; ++i)
    if (__atomic_load_n(&MayapServiceInternal::slots[i].admitted, __ATOMIC_ACQUIRE)) owners |= 1U << i;
  return MayapServiceInternal::radioGate.drained(owners);
}
inline void mayapRadioQuiesceEnd() { MayapServiceInternal::radioGate.release(); }
inline void mayapOnlineUnavailable(MayapRecovery::Service owner) {
  __atomic_store_n(&MayapServiceInternal::slots[static_cast<uint8_t>(owner)].degraded, 1U, __ATOMIC_RELEASE);
}
inline bool mayapRadioRecoveryRequested() {
  return MayapServiceInternal::radioGate.closed() ||
      __atomic_load_n(&MayapServiceInternal::radioQuiesce, __ATOMIC_ACQUIRE) != 0U;
}
inline bool mayapRadioOtaQuiesced() {
  return __atomic_load_n(&MayapServiceInternal::otaQuiesced, __ATOMIC_ACQUIRE) != 0U;
}
inline void mayapSetRadioOtaQuiesced(bool quiet) {
  __atomic_store_n(&MayapServiceInternal::otaQuiesced, quiet ? 1U : 0U, __ATOMIC_RELEASE);
}
inline void mayapServiceSupervisorUpdate(uint32_t now) {
  static MayapRecovery::ServiceWatch watches[4];
  for (uint8_t i = 0U; i < 4U; ++i) {
    auto &slot = MayapServiceInternal::slots[i];
    const bool admitted = __atomic_load_n(&slot.admitted, __ATOMIC_ACQUIRE) != 0U;
    const uint32_t beat = __atomic_load_n(&slot.beat, __ATOMIC_ACQUIRE);
    const uint32_t ack = __atomic_load_n(&slot.ack, __ATOMIC_ACQUIRE);
    if (admitted && MayapRecovery::silence(now, beat) <= MayapRecovery::SERVICE_TIMEOUT_MS[i])
      __atomic_store_n(&slot.degraded, 0U, __ATOMIC_RELEASE);
    const auto action = watches[i].update(now, admitted, beat, ack,
        MayapRecovery::SERVICE_TIMEOUT_MS[i]);
    if (action == MayapRecovery::Action::Reinit || action == MayapRecovery::Action::Isolate) {
      __atomic_store_n(&slot.request, 1U, __ATOMIC_RELEASE);
      if (action == MayapRecovery::Action::Isolate) {
        __atomic_store_n(&slot.isolateAt, now, __ATOMIC_RELEASE);
        __atomic_store_n(&slot.isolated, 1U, __ATOMIC_RELEASE);
      }
      mayapSerialPrintf(false, "[SERVICE-RECOVERY] %s %s silent=%lums now=%lu beat=%lu\n", MayapServiceInternal::names[i],
          action == MayapRecovery::Action::Reinit ? "OWNER REINIT REQUEST" : "ISOLATE",
          static_cast<unsigned long>(MayapRecovery::silence(now, beat)), static_cast<unsigned long>(now), static_cast<unsigned long>(beat));
    } else if (action == MayapRecovery::Action::Degraded) {
      __atomic_store_n(&slot.degraded, 1U, __ATOMIC_RELEASE);
      mayapSerialPrintf(false,
          "[SERVICE-DEGRADED] %s unresponsive - LOCAL CONTROL CONTINUES, NO RESTART\n",
          MayapServiceInternal::names[i]);
    }
  }
}
