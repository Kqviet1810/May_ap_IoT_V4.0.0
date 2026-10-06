#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>

namespace MayapRealtimePublish {
constexpr uint32_t BOOTSTRAP_MIN_INTERVAL_MS = 2000U;
constexpr uint32_t BOOTSTRAP_HEARTBEAT_MS = 30000U;
constexpr size_t BOOTSTRAP_PACKET_BUDGET = 512U;

struct BootstrapState {
  int32_t temperature = 0, humidity = 0;
  uint32_t revision = 0, alarmMask = 0;
  uint16_t fault = 0;
  uint8_t outputs = 0, faultCount = 0, severity = 0;
  char machineState[20] = "";
  bool operator==(const BootstrapState &other) const {
    return temperature == other.temperature && humidity == other.humidity &&
      revision == other.revision && alarmMask == other.alarmMask && fault == other.fault &&
      outputs == other.outputs && faultCount == other.faultCount && severity == other.severity &&
      !strcmp(machineState, other.machineState);
  }
};
inline int32_t quantize(float value, float scale) {
  const float scaled = value * scale;
  return isfinite(scaled) && scaled > -2147483648.0f && scaled < 2147483647.0f
    ? static_cast<int32_t>(roundf(scaled)) : INT32_MIN;
}
template <class Runtime>
BootstrapState bootstrapState(const Runtime &rt, uint32_t revision) {
  BootstrapState s;
  s.temperature = quantize(rt.temperature, 10.0f);
  s.humidity = quantize(rt.humidity, 1.0f);
  s.revision = revision;
  s.alarmMask = rt.alarmMask;
  s.fault = rt.primaryFaultCode;
  s.faultCount = rt.activeFaultCount;
  s.severity = rt.activeFaultDisplayCount ? rt.activeFaults[0].severity : 0U;
  s.outputs = (rt.batchRunning ? 1U : 0U) | (rt.heaterOn ? 2U : 0U) |
    (rt.circulationFanOn ? 4U : 0U) | (rt.ventFanOn ? 8U : 0U) |
    (rt.humidifierOn ? 16U : 0U) | (rt.lightOn ? 32U : 0U) | (rt.sirenOn ? 64U : 0U);
  strncpy(s.machineState, rt.machineState, sizeof(s.machineState) - 1U);
  return s;
}
class BootstrapCadence {
 public:
  void reset() { published_ = attempted_ = false; }
  bool due(uint32_t now, const BootstrapState &s) const {
    if (attempted_ && static_cast<uint32_t>(now - lastAttempt_) < BOOTSTRAP_MIN_INTERVAL_MS) return false;
    return !published_ || !(s == last_) ||
      static_cast<uint32_t>(now - lastPublish_) >= BOOTSTRAP_HEARTBEAT_MS;
  }
  void attempted(uint32_t now, const BootstrapState &s, bool success) {
    attempted_ = true; lastAttempt_ = now;
    if (success) { published_ = true; lastPublish_ = now; last_ = s; }
  }
 private:
  bool published_ = false, attempted_ = false;
  uint32_t lastAttempt_ = 0, lastPublish_ = 0;
  BootstrapState last_;
};
} // namespace MayapRealtimePublish
