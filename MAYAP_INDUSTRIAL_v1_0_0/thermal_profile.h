#pragma once
// Adaptive Thermal V1: the per-oven thermal profile.
//
// A small, versioned, CRC-protected record of the EFFECTIVE thermal behaviour the
// firmware has learned about this oven. It deliberately contains no heater kW, no
// chamber volume and no fan CFM: only what the single temperature probe can observe.
// Pure C++ (no Arduino/FreeRTOS types): the host simulation links the same code.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace MayapThermal {

constexpr uint32_t ProfileMagic = 0x54505631;  // 'TPV1'
constexpr uint16_t ProfileVersion = 1;
constexpr uint32_t ProfileModelVersion = 1;     // learning-algorithm revision
constexpr uint32_t ProfileMaxAgeSec = 30UL * 86400UL;  // older: loaded, but trust capped

enum class LearnState : uint8_t { Unlearned, Learning, Qualified, Adapting, Degraded };
inline const char *learnStateName(LearnState s) {
  static const char *const names[] = {"UNLEARNED", "LEARNING", "QUALIFIED", "ADAPTING", "DEGRADED"};
  const uint8_t i = static_cast<uint8_t>(s);
  return i < 5 ? names[i] : "UNKNOWN";
}

// Bounds shared by sanitize/validate and by the learner (nothing learned may leave them).
namespace Limits {
constexpr float GainMin = 0.0005f, GainMax = 0.5f;      // degC/s at 100 % actual duty
constexpr float DelayMin = 0.0f, DelayMax = 240.0f;     // s, SSR -> sensor response
constexpr float CoastRiseMax = 20.0f, CoastTimeMax = 300.0f;
constexpr float HoldMax = 100.0f;
constexpr float VentGainMax = 0.2f;                     // degC/s of cooling, fan ON
}  // namespace Limits

#pragma pack(push, 1)
struct ThermalProfile {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  float heaterGain;       // degC/s produced by 100 % ACTUAL heater duty (effective plant gain)
  float heaterDelaySec;   // apparent delay: SSR on -> reliable sensor response
  float coastRiseC;       // continued rise after a 100 % -> 0 % cut
  float coastTimeSec;     // time from that cut to the peak
  float holdPowerPct;     // average ACTUAL duty needed near a stable setpoint
  float ventCoolingGain;  // degC/s cooling effect of the exhaust fan at the operating point
  uint8_t confidence;     // 0..100
  uint8_t state;          // LearnState (informational)
  uint8_t ventConfidence; // 0..100
  uint8_t reserved;
  uint32_t validLearningSec;
  uint32_t modelVersion;
  uint32_t signature;     // hardware/config compatibility
  uint32_t epoch;         // RTC epoch at save (0 = unknown)
  uint32_t sequence;
  uint32_t crc;
};
#pragma pack(pop)
static_assert(sizeof(ThermalProfile) == 60, "profile layout is persistent: bump ProfileVersion to change it");

inline uint32_t profileCrc(const ThermalProfile &p) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&p);
  uint32_t c = 0xffffffffU;
  for (size_t n = 0; n < offsetof(ThermalProfile, crc); ++n) {
    c ^= bytes[n];
    for (unsigned b = 0; b < 8; ++b) c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U)));
  }
  return ~c;
}

// Safe defaults: confidence 0, so nothing in the controller consumes them.
inline ThermalProfile defaultProfile() {
  ThermalProfile p;
  std::memset(&p, 0, sizeof(p));
  p.magic = ProfileMagic;
  p.version = ProfileVersion;
  p.size = static_cast<uint16_t>(sizeof(ThermalProfile));
  p.heaterGain = 0.03f;
  p.heaterDelaySec = 45.0f;
  p.coastRiseC = 0.0f;
  p.coastTimeSec = 0.0f;
  p.holdPowerPct = 0.0f;
  p.ventCoolingGain = 0.0f;
  p.confidence = 0;
  p.state = static_cast<uint8_t>(LearnState::Unlearned);
  p.modelVersion = ProfileModelVersion;
  p.crc = profileCrc(p);
  return p;
}

inline bool finiteIn(float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }

// Field-level range check (no CRC): is this a profile the controller may use?
inline bool profileRangesValid(const ThermalProfile &p) {
  return finiteIn(p.heaterGain, Limits::GainMin, Limits::GainMax) &&
         finiteIn(p.heaterDelaySec, Limits::DelayMin, Limits::DelayMax) &&
         finiteIn(p.coastRiseC, 0.0f, Limits::CoastRiseMax) &&
         finiteIn(p.coastTimeSec, 0.0f, Limits::CoastTimeMax) &&
         finiteIn(p.holdPowerPct, 0.0f, Limits::HoldMax) &&
         finiteIn(p.ventCoolingGain, 0.0f, Limits::VentGainMax) &&
         p.confidence <= 100 && p.ventConfidence <= 100 && p.state <= 4;
}

// Stored record: magic/version/size/CRC and ranges. Anything else is discarded.
inline bool validProfile(const ThermalProfile &p) {
  return p.magic == ProfileMagic && p.version == ProfileVersion &&
         p.size == sizeof(ThermalProfile) && p.crc == profileCrc(p) && profileRangesValid(p);
}

// Clamp a live (in-RAM) profile into range; NaN/Inf fields fall back to defaults and
// zero the confidence, so a corrupted value can never raise controller authority.
inline void sanitizeProfile(ThermalProfile &p) {
  const ThermalProfile d = defaultProfile();
  bool repaired = false;
  auto fix = [&](float &v, float lo, float hi, float fallback) {
    if (!std::isfinite(v)) { v = fallback; repaired = true; }
    else if (v < lo) v = lo;
    else if (v > hi) v = hi;
  };
  fix(p.heaterGain, Limits::GainMin, Limits::GainMax, d.heaterGain);
  fix(p.heaterDelaySec, Limits::DelayMin, Limits::DelayMax, d.heaterDelaySec);
  fix(p.coastRiseC, 0.0f, Limits::CoastRiseMax, 0.0f);
  fix(p.coastTimeSec, 0.0f, Limits::CoastTimeMax, 0.0f);
  fix(p.holdPowerPct, 0.0f, Limits::HoldMax, 0.0f);
  fix(p.ventCoolingGain, 0.0f, Limits::VentGainMax, 0.0f);
  if (p.confidence > 100) p.confidence = 100;
  if (p.ventConfidence > 100) p.ventConfidence = 100;
  if (p.state > 4) p.state = static_cast<uint8_t>(LearnState::Unlearned);
  if (repaired) { p.confidence = 0; p.ventConfidence = 0; }
  p.magic = ProfileMagic;
  p.version = ProfileVersion;
  p.size = static_cast<uint16_t>(sizeof(ThermalProfile));
  p.modelVersion = ProfileModelVersion;
}

inline void sealProfile(ThermalProfile &p) { p.crc = profileCrc(p); }

// A stored profile may seed learning only on the same hardware signature, never after an
// abnormal reset, and with a trust ceiling that falls with age. The seed is not evidence:
// the caller must still qualify it with fresh clean windows.
inline bool profileCompatible(const ThermalProfile &p, uint32_t signature, bool abnormalReset) {
  return !abnormalReset && validProfile(p) && p.signature == signature && p.modelVersion == ProfileModelVersion;
}
inline uint8_t seedConfidenceCap(const ThermalProfile &p, uint32_t nowEpoch) {
  if (p.epoch == 0U || nowEpoch == 0U || nowEpoch < p.epoch) return 20;
  const uint32_t age = nowEpoch - p.epoch;
  return age > ProfileMaxAgeSec ? 15 : 40;
}

// Flash-wear policy (pure so it is unit-tested): persist only a qualified profile, at most
// once per `minIntervalMs`, and only when it differs materially from what is stored or
// the stored copy is old.
inline bool shouldPersistProfile(const ThermalProfile &live, const ThermalProfile &stored,
                                 bool storedValid, uint32_t sinceLastSaveMs,
                                 uint32_t minIntervalMs = 3600000UL) {
  if (live.confidence < 60 || !profileRangesValid(live)) return false;
  if (sinceLastSaveMs < minIntervalMs) return false;
  if (!storedValid) return true;
  if (live.signature != stored.signature || live.modelVersion != stored.modelVersion) return true;
  // Age refresh: an unchanged but healthy profile is rewritten weekly so its epoch (and with it
  // the seed trust after a reboot) never ages out while the oven keeps running.
  if (live.epoch != 0U && stored.epoch != 0U && live.epoch > stored.epoch && live.epoch - stored.epoch >= 7UL * 86400UL) return true;
  auto rel = [](float a, float b) { return std::fabs(a - b) / (std::fabs(b) > 1e-6f ? std::fabs(b) : 1e-6f); };
  if (rel(live.heaterGain, stored.heaterGain) > 0.10f) return true;
  if (std::fabs(live.heaterDelaySec - stored.heaterDelaySec) > 10.0f) return true;
  if (std::fabs(live.holdPowerPct - stored.holdPowerPct) > 3.0f) return true;
  if (std::fabs(live.coastRiseC - stored.coastRiseC) > 0.15f) return true;
  if (std::fabs(live.ventCoolingGain - stored.ventCoolingGain) > 0.25f * stored.ventCoolingGain + 0.0005f &&
      live.ventConfidence >= 50) return true;
  if (std::abs(static_cast<int>(live.confidence) - static_cast<int>(stored.confidence)) >= 20) return true;
  return false;
}

}  // namespace MayapThermal
