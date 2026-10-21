#pragma once
// Launch plan (accelerate-then-brake prior) and its persistent owner.
//
// What the startup controller measured on the LAST good warm-up of this oven: heater gain, apparent delay and the
// drift calibration. The next batch launches at full power at once instead of probing; every launch still re-measures
// the gain (a different number of egg trolleys changes the thermal mass), so the plan is a prior and never evidence.
//
// Same discipline as MayapThermal::ProfileStorage (and the same NVS namespace, own keys "la"/"lb"):
//   * the sole flash writer is Arduino loop(), never the control task;
//   * the control task only copies a fixed-size record into a mailbox;
//   * two alternating slots, each written then read back and verified, so a power cut leaves the previous valid plan;
//   * at most one save per `minIntervalMs` (the controller already offers at most once per batch warm-up, and only when
//     the plan differs materially from the one it started with).
// Pure C++ apart from Preferences/FreeRTOS (stubbed by the host tests).
#include "thermal_assist.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace MayapThermal {

constexpr uint32_t LaunchPlanMagic = 0x4C504C31;   // 'LPL1'
constexpr uint16_t LaunchPlanVersion = 1;

#pragma pack(push, 1)
struct LaunchPlanRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  float gain;        // degC/s at 100 % duty
  float delaySec;    // apparent delay
  float coastCal;    // measured drift / modelled heat in flight
  uint8_t calMeasured;   // 1 = coastCal was measured on a real cut
  uint8_t idleOverCc;    // idleOverC in 0.01 C (0 = never seen; older records have 0 here)
  uint8_t reserved[2];
  float holdPct;     // duty that holds PV at SP (0 = unknown)
  uint32_t signature;  // hardware compatibility (same as the thermal profile)
  uint32_t epoch;      // RTC epoch at save (0 = unknown)
  uint32_t sequence;
  uint32_t crc;
};
#pragma pack(pop)
constexpr uint32_t MinIntervalMs = 600000UL;
static_assert(sizeof(LaunchPlanRecord) == 44, "launch plan layout is persistent: bump LaunchPlanVersion to change it");

namespace LaunchLimits {
constexpr float GainMin = 0.0005f, GainMax = 0.5f;
constexpr float DelayMax = 240.0f;
constexpr float CalMin = 0.4f, CalMax = 3.5f;
}  // namespace LaunchLimits

inline uint32_t launchPlanCrc(const LaunchPlanRecord &r) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&r);
  uint32_t c = 0xffffffffU;
  for (size_t n = 0; n < offsetof(LaunchPlanRecord, crc); ++n) {
    c ^= bytes[n];
    for (unsigned b = 0; b < 8; ++b) c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U)));
  }
  return ~c;
}
inline bool launchPlanRangesValid(float gain, float delaySec, float cal) {
  return std::isfinite(gain) && gain >= LaunchLimits::GainMin && gain <= LaunchLimits::GainMax &&
         std::isfinite(delaySec) && delaySec >= 0.0f && delaySec <= LaunchLimits::DelayMax &&
         std::isfinite(cal) && cal >= LaunchLimits::CalMin && cal <= LaunchLimits::CalMax;
}
inline bool validLaunchPlanRecord(const LaunchPlanRecord &r) {
  return r.magic == LaunchPlanMagic && r.version == LaunchPlanVersion && r.size == sizeof(LaunchPlanRecord) &&
         r.crc == launchPlanCrc(r) && launchPlanRangesValid(r.gain, r.delaySec, r.coastCal) &&
         std::isfinite(r.holdPct) && r.holdPct >= 0.0f && r.holdPct <= 100.0f;
}
inline LaunchPlanRecord makeLaunchPlanRecord(const LaunchPlan &p, uint32_t signature, uint32_t epoch) {
  LaunchPlanRecord r;
  std::memset(&r, 0, sizeof(r));
  r.magic = LaunchPlanMagic; r.version = LaunchPlanVersion; r.size = sizeof(LaunchPlanRecord);
  r.gain = p.gain; r.delaySec = p.delaySec; r.coastCal = p.coastCal; r.calMeasured = p.calMeasured ? 1U : 0U; r.holdPct = p.holdPct;
  r.idleOverCc = static_cast<uint8_t>(std::fmin(255.0f, std::fmax(0.0f, std::floor(p.idleOverC * 100.0f + 0.5f))));
  r.signature = signature; r.epoch = epoch;
  r.crc = launchPlanCrc(r);
  return r;
}
// A stored plan is used only on the same hardware signature and never after an abnormal reset.
inline bool launchPlanCompatible(const LaunchPlanRecord &r, uint32_t signature, bool abnormalReset) {
  return !abnormalReset && validLaunchPlanRecord(r) && r.signature == signature;
}
// Flash-wear policy (pure so it is unit-tested): save only a plan that differs materially from the stored one.
inline bool shouldPersistLaunchPlan(const LaunchPlanRecord &live, const LaunchPlanRecord &stored, bool storedValid,
                                    uint32_t sinceLastSaveMs, uint32_t minIntervalMs = MinIntervalMs) {
  if (!validLaunchPlanRecord(live) || sinceLastSaveMs < minIntervalMs) return false;
  if (!storedValid || live.signature != stored.signature) return true;
  if (std::fabs(live.gain - stored.gain) > 0.10f * stored.gain) return true;
  if (std::fabs(live.delaySec - stored.delaySec) > 10.0f) return true;
  if (live.calMeasured != stored.calMeasured) return true;
  if (std::fabs(live.holdPct - stored.holdPct) > 3.0f) return true;
  if (std::abs(static_cast<int>(live.idleOverCc) - static_cast<int>(stored.idleOverCc)) > 5) return true;   // 0.05 C
  return std::fabs(live.coastCal - stored.coastCal) > 0.15f;
}

class LaunchPlanStorage {
 public:
  void offer(const LaunchPlanRecord &record) {
    portENTER_CRITICAL(&mux_); pending_ = record; hasPending_ = true; portEXIT_CRITICAL(&mux_);
  }
  bool takeSeed(LaunchPlanRecord &record) {
    portENTER_CRITICAL(&mux_);
    const bool ready = seedReady_;
    if (ready) { record = stored_; seedReady_ = false; }
    portEXIT_CRITICAL(&mux_);
    return ready;
  }
  uint32_t saves() const { return saves_; }
  void resetForTest() {
    portENTER_CRITICAL(&mux_); hasPending_ = seedReady_ = false; portEXIT_CRITICAL(&mux_);
    initialized_ = available_ = activeA_ = storedValid_ = false;
    sequence_ = lastSave_ = saves_ = 0;
    pending_ = LaunchPlanRecord{}; stored_ = LaunchPlanRecord{};
  }
  void service(uint32_t now) {
    if (!initialized_) {
      initialized_ = true;
      available_ = prefs_.begin("mayap_thermal", false);
      if (!available_) return;
      LaunchPlanRecord a{}, b{};
      const bool va = prefs_.isKey("la") && prefs_.getBytes("la", &a, sizeof(a)) == sizeof(a) && validLaunchPlanRecord(a);
      const bool vb = prefs_.isKey("lb") && prefs_.getBytes("lb", &b, sizeof(b)) == sizeof(b) && validLaunchPlanRecord(b);
      if (va || vb) {
        const bool useA = !vb || (va && static_cast<int32_t>(a.sequence - b.sequence) >= 0);
        activeA_ = useA;
        stored_ = useA ? a : b;
        storedValid_ = true;
        sequence_ = stored_.sequence;
        portENTER_CRITICAL(&mux_); seedReady_ = true; portEXIT_CRITICAL(&mux_);
      }
      lastSave_ = now - MinIntervalMs;   // the first offer of this run may be saved at once
      return;
    }
    if (!available_) return;
    LaunchPlanRecord record{};
    portENTER_CRITICAL(&mux_);
    const bool pending = hasPending_;
    if (pending) record = pending_;
    portEXIT_CRITICAL(&mux_);
    if (!pending) return;
    if (!validLaunchPlanRecord(record) || !shouldPersistLaunchPlan(record, stored_, storedValid_, UINT32_MAX)) {
      portENTER_CRITICAL(&mux_); hasPending_ = false; portEXIT_CRITICAL(&mux_);   // nothing material to save: drop it
      return;
    }
    if (now - lastSave_ < MinIntervalMs) return;   // wear bound: keep it pending until the interval has passed
    portENTER_CRITICAL(&mux_); hasPending_ = false; portEXIT_CRITICAL(&mux_);
    lastSave_ = now;   // a failed write is wear/backoff bounded as well
    record.sequence = sequence_ + 1U;
    record.crc = launchPlanCrc(record);
    if (!validLaunchPlanRecord(record)) return;
    const char *key = activeA_ ? "lb" : "la";
    LaunchPlanRecord verify{};
    if (prefs_.putBytes(key, &record, sizeof(record)) == sizeof(record) &&
        prefs_.getBytes(key, &verify, sizeof(verify)) == sizeof(verify) && validLaunchPlanRecord(verify) &&
        std::memcmp(&record, &verify, sizeof(record)) == 0) {
      activeA_ = !activeA_; sequence_ = record.sequence; stored_ = record; storedValid_ = true; ++saves_;
    }
  }

 private:
  Preferences prefs_;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  LaunchPlanRecord pending_{}, stored_{};
  bool hasPending_ = false, seedReady_ = false;
  bool initialized_ = false, available_ = false, activeA_ = false, storedValid_ = false;
  uint32_t sequence_ = 0, lastSave_ = 0, saves_ = 0;
};
static LaunchPlanStorage launchPlanStorage;
}  // namespace MayapThermal
