#pragma once
#include <math.h>
#include <stdint.h>

// Pure rules for a firmware update while the machine is (or is not) incubating, and for the spacing of remote commands. Header-only and
// free of Arduino types so the host tests drive exactly this code (tests/firmware-check.cpp).
namespace MayapMaintenance {

// A batch may be interrupted for an update only in developer mode, only while the temperature is inside this band around SV, and never
// while the machine waits to resume a batch or to record a stop. The band follows SV (a 30 degC hatcher is judged like a 37.5 degC one).
constexpr float BATCH_UPDATE_MAX_DEV_C = 1.0f;
constexpr uint32_t RETRY_AFTER_FAIL_MS = 600000UL;      // a remote "update now" is refused for this long after a failed attempt
constexpr uint32_t REMOTE_OUTPUT_GAP_MS = 2000UL;       // the same remote output toggle is not repeated faster than this

struct ReadyInputs {
  bool batchRunning = false, resumePending = false, batchClearPending = false;
  bool testMode = false, turning = false, emergency = false, highTemp = false;
  bool developerMode = false, sensorUsable = false;
  float temperature = NAN, setpoint = NAN;
};

inline bool temperatureWithinBand(const ReadyInputs &in) {
  return in.sensorUsable && isfinite(in.temperature) && isfinite(in.setpoint) &&
         fabsf(in.temperature - in.setpoint) <= BATCH_UPDATE_MAX_DEV_C;
}

// The machine may start the update now (the HMI path and the automatic path both ask this).
inline bool ready(const ReadyInputs &in) {
  if (in.testMode || in.turning || in.emergency) return false;
  if (in.resumePending || in.batchClearPending) return false;          // a safety-journal state: never interrupted, developer mode or not
  if (!in.batchRunning) return true;
  return in.developerMode && !in.highTemp && temperatureWithinBand(in);
}

// The same remote output toggle is not repeated faster than `gapMs`; a safety action (`restores`) is never delayed.
inline bool commandGapOk(uint32_t now, uint32_t lastAt, uint32_t gapMs, bool restores) {
  if (restores || lastAt == 0U) return true;
  return static_cast<uint32_t>(now - lastAt) >= gapMs;
}

// A remote "update now" is refused while one is running and for RETRY_AFTER_FAIL_MS after a failed one.
inline bool applyAllowed(uint32_t now, bool downloading, uint32_t lastFailedAt) {
  if (downloading) return false;
  if (lastFailedAt == 0U) return true;
  return static_cast<uint32_t>(now - lastFailedAt) >= RETRY_AFTER_FAIL_MS;
}

}  // namespace MayapMaintenance
