#pragma once
#include <Arduino.h>

// MQTT handshake, Cloud HTTPS and OTA HTTPS formerly allocated TLS working
// sets concurrently on the no-PSRAM board. Keep one transient TLS operation
// at a time; the established MQTT connection continues pumping independently.
namespace MayapNetworkIoInternal {
static uint8_t tlsBusy = 0U;
static uint32_t deferred = 0U;
static uint32_t cloudYieldUntil = 0U, cloudYieldRetryAt = 0U;
static uint8_t cloudActive = 0U;
static uint32_t cloudUrgentRetryAt=0U;
// ONE TLS context at a time, ever. The resident MQTT/WSS socket (~43 kB) counts as a context; an HTTPS
// operation (alarm fallback, registration, OTA) may only start after MQTT has closed and given its memory back.
// `contextsMax` is the high-water mark the diagnostics print and the tests assert on (must stay <= 1).
static uint8_t mqttResident = 0U;
static uint8_t contexts = 0U, contextsMax = 0U;
static uint32_t overlapDenied = 0U, overlapViolations = 0U;
}
inline void mayapTlsContextEnter() {
  using namespace MayapNetworkIoInternal;
  const uint8_t now = __atomic_add_fetch(&contexts, 1U, __ATOMIC_ACQ_REL);
  uint8_t peak = __atomic_load_n(&contextsMax, __ATOMIC_ACQUIRE);
  while (now > peak && !__atomic_compare_exchange_n(&contextsMax, &peak, now, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
  if (now > 1U) __atomic_fetch_add(&overlapViolations, 1U, __ATOMIC_RELAXED);
}
inline void mayapTlsContextLeave() {
  using namespace MayapNetworkIoInternal;
  if (__atomic_load_n(&contexts, __ATOMIC_ACQUIRE) > 0U) __atomic_sub_fetch(&contexts, 1U, __ATOMIC_ACQ_REL);
}
// Owner (mqttTask) only: the WSS socket exists / no longer exists.
inline void mayapSetMqttTlsResident(bool resident) {
  using namespace MayapNetworkIoInternal;
  const uint8_t was = __atomic_exchange_n(&mqttResident, resident ? 1U : 0U, __ATOMIC_ACQ_REL);
  if (resident && !was) mayapTlsContextEnter();
  else if (!resident && was) mayapTlsContextLeave();
}
inline bool mayapMqttTlsResident() {
  return __atomic_load_n(&MayapNetworkIoInternal::mqttResident, __ATOMIC_ACQUIRE) != 0U;
}
inline uint8_t mayapTlsContextsMax() { return __atomic_load_n(&MayapNetworkIoInternal::contextsMax, __ATOMIC_RELAXED); }
inline uint32_t mayapTlsOverlapViolations() { return __atomic_load_n(&MayapNetworkIoInternal::overlapViolations, __ATOMIC_RELAXED); }
inline uint32_t mayapTlsOverlapDenied() { return __atomic_load_n(&MayapNetworkIoInternal::overlapDenied, __ATOMIC_RELAXED); }
// Cloud requests a bounded RAM handoff; only the realtime owner closes its
// socket. A failed Cloud task cannot keep realtime paused indefinitely.
inline bool mayapRequestCloudTlsYield(uint32_t now, bool urgent = false) {
  using namespace MayapNetworkIoInternal;
  const uint32_t retry = __atomic_load_n(&cloudYieldRetryAt, __ATOMIC_ACQUIRE);
  const uint32_t until=__atomic_load_n(&cloudYieldUntil,__ATOMIC_ACQUIRE);
  if(until!=0U && static_cast<int32_t>(now-until)<0) return true; // same transaction
  if (!urgent && retry != 0U && static_cast<int32_t>(now - retry) < 0) return false;
  const uint32_t urgentRetry=__atomic_load_n(&cloudUrgentRetryAt,__ATOMIC_ACQUIRE);
  if(urgent && urgentRetry!=0U && static_cast<int32_t>(now-urgentRetry)<0) return false;
  __atomic_store_n(&cloudUrgentRetryAt,now+20000U,__ATOMIC_RELEASE);
  __atomic_store_n(&cloudYieldUntil, now + 15000U, __ATOMIC_RELEASE);
  __atomic_store_n(&cloudYieldRetryAt, now + 60000U, __ATOMIC_RELEASE);
  return true;
}
inline bool mayapCloudTlsYieldRequested(uint32_t now) {
  using namespace MayapNetworkIoInternal;
  const uint32_t until = __atomic_load_n(&cloudYieldUntil, __ATOMIC_ACQUIRE);
  return until != 0U && (static_cast<int32_t>(now - until) < 0 ||
      __atomic_load_n(&cloudActive, __ATOMIC_ACQUIRE) != 0U);
}
inline void mayapReleaseCloudTlsYield() {
  __atomic_store_n(&MayapNetworkIoInternal::cloudYieldUntil, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&MayapNetworkIoInternal::cloudUrgentRetryAt, 0U, __ATOMIC_RELEASE);
}
enum class MayapTlsKind : uint8_t { Mqtt, Cloud, Ota };
inline bool mayapTlsBusy() {
  return __atomic_load_n(&MayapNetworkIoInternal::tlsBusy, __ATOMIC_ACQUIRE) != 0U;
}
inline uint32_t mayapTlsDeferredCount() {
  return __atomic_load_n(&MayapNetworkIoInternal::deferred, __ATOMIC_RELAXED);
}
// Bulk MQTT JSON/chunk publication shares admission with transient TLS.
// It never blocks the MQTT pump or terminal ACKs, and Cloud cannot start a
// handshake in the check-then-allocate gap of a large config/history report.
class MayapNetworkBatchOperation {
 public:
  MayapNetworkBatchOperation() {
    uint8_t expected = 0U;
    acquired_ = __atomic_compare_exchange_n(&MayapNetworkIoInternal::tlsBusy,
        &expected, 2U, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    if (acquired_ && ESP.getFreeHeap() < 24576U) {
      __atomic_store_n(&MayapNetworkIoInternal::tlsBusy, 0U, __ATOMIC_RELEASE);
      acquired_ = false;
    }
  }
  ~MayapNetworkBatchOperation() {
    if (acquired_) __atomic_store_n(&MayapNetworkIoInternal::tlsBusy, 0U, __ATOMIC_RELEASE);
  }
  explicit operator bool() const { return acquired_; }
  MayapNetworkBatchOperation(const MayapNetworkBatchOperation &) = delete;
  MayapNetworkBatchOperation &operator=(const MayapNetworkBatchOperation &) = delete;
 private:
  bool acquired_ = false;
};
class MayapTlsOperation {
 public:
  // `urgent` (an alarm that could not go over MQTT) may ask the realtime owner to close more often.
  explicit MayapTlsOperation(MayapTlsKind kind = MayapTlsKind::Mqtt, bool urgent = false) {
    kind_ = kind;
    uint8_t expected = 0U;
    acquired_ = __atomic_compare_exchange_n(&MayapNetworkIoInternal::tlsBusy,
        &expected, 1U, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    // HTTPS (Cloud/OTA) never coexists with the MQTT socket: while MQTT is resident the operation is
    // refused and the realtime owner is asked to close; only after it released its ~43 kB may the HTTPS
    // session start (and only with enough free heap and one large block for the handshake).
    const uint32_t freeBudget = kind == MayapTlsKind::Mqtt ? 49152U : 73728U;
    const bool https = kind != MayapTlsKind::Mqtt;
    const bool overlap = acquired_ && https && mayapMqttTlsResident();
    if (acquired_ && (overlap || ESP.getFreeHeap() < freeBudget || ESP.getMaxAllocHeap() < 24576U)) {
      __atomic_store_n(&MayapNetworkIoInternal::tlsBusy, 0U, __ATOMIC_RELEASE);
      acquired_ = false;
      if (overlap) __atomic_fetch_add(&MayapNetworkIoInternal::overlapDenied, 1U, __ATOMIC_RELAXED);
      if (https) mayapRequestCloudTlsYield(millis(), urgent);
    }
    if (acquired_ && https) mayapTlsContextEnter();
    if (acquired_ && kind == MayapTlsKind::Cloud)
      __atomic_store_n(&MayapNetworkIoInternal::cloudActive, 1U, __ATOMIC_RELEASE);
    if (!acquired_) __atomic_fetch_add(&MayapNetworkIoInternal::deferred, 1U, __ATOMIC_RELAXED);
  }
  ~MayapTlsOperation() {
    if (acquired_ && kind_ == MayapTlsKind::Cloud) {
      // Keep the bounded lease between registration and queued alarms;
      // otherwise WS reconnect wins the required HTTP send gap every time.
      __atomic_store_n(&MayapNetworkIoInternal::cloudActive, 0U, __ATOMIC_RELEASE);
    }
    if (acquired_ && kind_ != MayapTlsKind::Mqtt) mayapTlsContextLeave();
    if (acquired_) __atomic_store_n(&MayapNetworkIoInternal::tlsBusy, 0U, __ATOMIC_RELEASE);
  }
  explicit operator bool() const { return acquired_; }
  MayapTlsOperation(const MayapTlsOperation &) = delete;
  MayapTlsOperation &operator=(const MayapTlsOperation &) = delete;
 private:
  bool acquired_ = false;
  MayapTlsKind kind_ = MayapTlsKind::Mqtt;
};
