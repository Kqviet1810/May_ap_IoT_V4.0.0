#pragma once
// Periodic egg cooling: independent, OFF-by-default module (Smart Thermal phase 6).
//
// DESIGN ONLY, NOT WIRED INTO THE FIRMWARE and NOT TO BE ENABLED in a firmware that goes to a real oven before (a) the thermal
// stabilisation work is qualified, (b) the biological procedure (species, days, duration, minimum temperature) is approved,
// (c) a sensor/commissioning plan shows the cooling actually happens in the eggs. Nothing includes this header.
//
// The module only produces a REQUEST for the caller; it never touches an output and it can never keep a protection quiet:
//   heaterInhibit   hold the heater OFF during the approved window (the caller ANDs it into the heater permit, i.e. it can only remove heat)
//   lowTempSuppress the caller may defer the "temperature low" WARNING while this is true; never High/Emergency, never a sensor fault
// Preconditions are checked on EVERY update; losing any of them ends the cycle at once (heater permitted again) and the slot is
// consumed, so a fault cannot be followed by an immediate restart in the same slot.
//   batch running, RTC valid, sensor valid, no safety latch, no AutoTune, no test mode, temperature above the minimum guard,
//   elapsed cooling below the maximum-duration guard.
// After a reset / power loss the default is to CANCEL (heater permitted at once). Resuming the remaining time is opt-in and only with a
// valid CRC record, a valid RTC and a remaining time that still fits the maximum duration.
#include <cmath>
#include <cstdint>

namespace MayapProgram {

#ifndef MAYAP_EGG_COOLING
#define MAYAP_EGG_COOLING 0       // 0: no production integration is compiled in (there is none yet)
#endif

constexpr uint16_t CoolingAbsoluteMaxMin = 90;    // no policy may ask for more than this, whatever the species
constexpr int16_t CoolingAbsoluteMinTempX10 = 300;  // 30.0 degC: no policy may allow the chamber to fall below this

enum class Resume : uint8_t { Cancel = 0, ResumeRemaining = 1 };
enum class CoolPhase : uint8_t { Off, Waiting, Cooling, Cancelled };
enum class CancelReason : uint8_t { None, Done, MinTemp, MaxDuration, Rtc, Sensor, Safety, Batch, Tune, Test, Reset, BadRecord };

struct CoolingPolicy {
  bool enabled = false;                 // default OFF
  uint8_t firstDay = 0, lastDay = 0;    // inclusive, 1-based incubation days
  uint16_t periodMin = 0;               // slot length; a slot starts at batchStart + k * period
  uint16_t durationMin = 0;             // requested cooling at the start of each slot
  uint16_t maxDurationMin = 0;          // hard guard (>= duration)
  int16_t minTempX10 = 0;               // cooling ends if the chamber falls to this (0.1 degC)
  Resume resume = Resume::Cancel;
};

inline bool validPolicy(const CoolingPolicy &p) {
  if (!p.enabled) return true;
  return p.firstDay >= 1 && p.lastDay >= p.firstDay && p.lastDay <= 28 &&
         p.durationMin >= 1 && p.durationMin <= p.maxDurationMin && p.maxDurationMin <= CoolingAbsoluteMaxMin &&
         p.periodMin > p.maxDurationMin && p.periodMin <= 24 * 60 && p.minTempX10 >= CoolingAbsoluteMinTempX10;
}

struct CoolingInput {
  bool batchRunning = false, rtcValid = false, sensorValid = false, safetyLatched = false, autoTune = false, testMode = false;
  uint32_t epoch = 0, batchStartEpoch = 0;
  float tempC = NAN;
};

struct CoolingOutput {
  bool heaterInhibit = false, lowTempSuppress = false;
  CoolPhase phase = CoolPhase::Off;
  CancelReason lastCancel = CancelReason::None;
  uint16_t slot = 0;
  uint32_t coolingSec = 0;
};

// Persisted state (so a reset can follow the resume policy). Packed, 24 bytes incl. sequence and CRC; written by the caller on phase change only.
#pragma pack(push, 1)
struct CoolingRecord {
  uint32_t magic = 0x4C4F4F43U;          // 'COOL'
  uint8_t version = 1, active = 0;
  uint16_t slot = 0;
  uint32_t slotStartEpoch = 0;           // start of the slot whose window was open
  uint32_t coolStartEpoch = 0;           // when cooling really began (the maximum-duration guard counts from here)
  uint32_t seq = 0;                      // A/B store sequence (program_store.h)
  uint32_t crc = 0;
};
#pragma pack(pop)
inline uint32_t crc32(const uint8_t *d, uint32_t n) {
  uint32_t c = 0xFFFFFFFFU;
  for (uint32_t i = 0; i < n; ++i) { c ^= d[i]; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320U & (0U - (c & 1U))); }
  return ~c;
}
inline uint32_t recordCrc(const CoolingRecord &r) {
  return crc32(reinterpret_cast<const uint8_t *>(&r), static_cast<uint32_t>(sizeof(CoolingRecord) - sizeof(uint32_t)));
}
inline void seal(CoolingRecord &r) { r.crc = recordCrc(r); }
inline bool validRecord(const CoolingRecord &r) { return r.magic == 0x4C4F4F43U && r.version == 1 && r.crc == recordCrc(r); }

class EggCooling {
 public:
  void configure(const CoolingPolicy &p) { policy_ = validPolicy(p) ? p : CoolingPolicy{}; phase_ = CoolPhase::Off; booted_ = false; }
  const CoolingPolicy &policy() const { return policy_; }

  // Call once after a reset with whatever record was read back (nullptr = none). A cycle that was active at the reset is
  // CANCELLED unless the policy opts into ResumeRemaining AND the record is valid AND the RTC is valid AND the window is still open.
  void restore(const CoolingRecord *rec, const CoolingInput &in) {
    resumeFrom_ = false;
    if (!rec) return;
    if (!validRecord(*rec)) { lastCancel_ = CancelReason::BadRecord; return; }
    if (rec->active == 0) return;
    lastCancel_ = CancelReason::Reset;
    if (policy_.resume != Resume::ResumeRemaining || !policy_.enabled || !in.rtcValid) return;
    if (in.epoch < rec->slotStartEpoch || rec->coolStartEpoch < rec->slotStartEpoch) return;
    if (in.epoch - rec->slotStartEpoch >= static_cast<uint32_t>(policy_.durationMin) * 60U) return;   // the approved window is over
    resumeFrom_ = true; resumeSlot_ = rec->slot; resumeSlotStart_ = rec->slotStartEpoch; resumeCoolStart_ = rec->coolStartEpoch;
  }

  CoolingOutput update(const CoolingInput &in) {
    CoolingOutput o;
    if (!policy_.enabled) { phase_ = CoolPhase::Off; o.phase = phase_; o.lastCancel = lastCancel_; return o; }
    const CancelReason pre = precondition(in);
    if (pre != CancelReason::None) {
      if (phase_ == CoolPhase::Cooling) endCycle(pre);
      phase_ = pre == CancelReason::Batch ? CoolPhase::Off : CoolPhase::Waiting;
      return finish(o);
    }
    const uint32_t elapsedSec = in.epoch >= in.batchStartEpoch ? in.epoch - in.batchStartEpoch : 0U;
    const uint32_t day = elapsedSec / 86400U + 1U;
    const uint32_t periodSec = static_cast<uint32_t>(policy_.periodMin) * 60U;
    const uint32_t windowSec = static_cast<uint32_t>(policy_.durationMin) * 60U;
    const uint32_t k = elapsedSec / periodSec;
    const uint32_t slotStart = in.batchStartEpoch + k * periodSec;
    const uint32_t into = elapsedSec - k * periodSec;
    slot_ = static_cast<uint16_t>(k > 0xFFFFU ? 0xFFFFU : k);
    const bool due = day >= policy_.firstDay && day <= policy_.lastDay && into < windowSec;
    const bool resumable = resumeFrom_ && resumeSlot_ == slot_ && resumeSlotStart_ == slotStart;
    // First look after boot: a window that is already open is NOT started from scratch (default: cancel), only resumed.
    if (!booted_) { booted_ = true; if (due && !resumable) consume(slot_); }
    if (phase_ == CoolPhase::Off || phase_ == CoolPhase::Cancelled) phase_ = CoolPhase::Waiting;
    if (phase_ == CoolPhase::Cooling) {
      if (in.epoch < coolStartEpoch_) endCycle(CancelReason::Rtc);                                        // clock moved backwards
      else if (in.epoch - coolStartEpoch_ >= static_cast<uint32_t>(policy_.maxDurationMin) * 60U) endCycle(CancelReason::MaxDuration);
      else if (in.tempC <= policy_.minTempX10 * 0.1f) endCycle(CancelReason::MinTemp);
      else if (slot_ != coolSlot_ || into >= windowSec) { phase_ = CoolPhase::Waiting; lastCancel_ = CancelReason::Done; consume(coolSlot_); }
    } else if (due && !(consumedValid_ && consumed_ == slot_) && in.tempC > policy_.minTempX10 * 0.1f + 0.2f) {
      phase_ = CoolPhase::Cooling; coolSlot_ = slot_;
      coolStartEpoch_ = resumable ? resumeCoolStart_ : in.epoch;
      coolSlotStart_ = slotStart;
      if (resumable) resumeFrom_ = false;
    }
    return finish(o, &in);
  }

  // Record to persist when the caller sees a phase change (never in the control loop's hot path).
  CoolingRecord record(const CoolingInput &in) const {
    CoolingRecord r; r.active = phase_ == CoolPhase::Cooling ? 1 : 0; r.slot = coolSlot_;
    r.slotStartEpoch = phase_ == CoolPhase::Cooling ? coolSlotStart_ : in.epoch;
    r.coolStartEpoch = phase_ == CoolPhase::Cooling ? coolStartEpoch_ : in.epoch;
    seal(r); return r;
  }

 private:
  CancelReason precondition(const CoolingInput &in) const {
    if (!in.batchRunning) return CancelReason::Batch;
    if (!in.rtcValid) return CancelReason::Rtc;
    if (!in.sensorValid || !std::isfinite(in.tempC)) return CancelReason::Sensor;
    if (in.safetyLatched) return CancelReason::Safety;
    if (in.autoTune) return CancelReason::Tune;
    if (in.testMode) return CancelReason::Test;
    return CancelReason::None;
  }
  void consume(uint16_t slot) { consumed_ = slot; consumedValid_ = true; }
  void endCycle(CancelReason r) { phase_ = CoolPhase::Cancelled; lastCancel_ = r; consume(coolSlot_); }
  CoolingOutput finish(CoolingOutput &o, const CoolingInput *in = nullptr) const {
    o.phase = phase_; o.lastCancel = lastCancel_; o.slot = slot_;
    if (phase_ == CoolPhase::Cooling) {
      o.heaterInhibit = true; o.lowTempSuppress = true;
      if (in && in->epoch >= coolStartEpoch_) o.coolingSec = in->epoch - coolStartEpoch_;
    }
    return o;
  }

  CoolingPolicy policy_{};
  CoolPhase phase_ = CoolPhase::Off;
  CancelReason lastCancel_ = CancelReason::None;
  uint16_t slot_ = 0, coolSlot_ = 0, consumed_ = 0, resumeSlot_ = 0;
  bool consumedValid_ = false, resumeFrom_ = false, booted_ = false;
  uint32_t coolStartEpoch_ = 0, coolSlotStart_ = 0, resumeSlotStart_ = 0, resumeCoolStart_ = 0;
};

}  // namespace MayapProgram
