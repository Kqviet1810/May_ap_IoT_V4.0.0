#pragma once
// The hold level of the oven (percent of the heater that keeps it at SV), kept in the external EEPROM so that a restart does not throw it away: after a restart
// (an update, a power cut) the controller used to start at 12 % whatever the oven needed, and on a hot noon that is 8-10 points too much (rig: +0.30 degC over SV, 18 minutes
// over +0.1). Same layout as the heater gain record (thermal_gain_record.h): two slots, one CRC record each, a save writes the slot that does NOT hold the newest valid
// record and reads it back, so a power cut in the middle of a write leaves the other slot readable.
// Pure C++ (no Arduino types): MachineController keeps and restores it, tests/thermal-hold-record.cpp drives the same code.
//
//   Saved only when the oven is settled (tracking, PV near SV, no door) for 10 minutes, at most every 10 minutes, and when the level moved by 0.5 points or the batch is another one.
//   Restored only when ALL of these hold: a valid record of THIS batch; SV unchanged; PV near SV and near the PV of the record (the oven did not cool or heat meanwhile);
//   the clock says the stop was short (30 minutes) - or, with no usable clock, the PV says so. The level given back is 0.9 of the saved one (a little under is safer than a little over:
//   the controller raises it if it is too low). Anything else: the 12 % start, as before.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "thermal_gain_record.h"

namespace thermalhold {

constexpr uint32_t MAGIC = 0x4D4B4831UL;                 // "MKH1"
constexpr uint32_t SETTLED_MIN_MS = 600000UL;            // settled this long before a save
constexpr uint32_t SAVE_MIN_MS = 600000UL;               // at most one write every 10 minutes
constexpr float SAVE_MOVE_PCT = 0.5f;                    // ... and only when the level moved by this much (or the batch changed)
constexpr uint32_t RESTORE_MAX_GAP_S = 1800UL;           // a stop longer than this: the oven is a different one (sun, eggs, room)
constexpr float RESTORE_NEAR_C = 0.5f;                   // PV within this of SV at the restart ...
constexpr float RESTORE_PV_MOVE_C = 0.5f;                // ... and within this of the PV saved
constexpr float RESTORE_SP_MOVE_C = 0.05f;               // the set-point must be the one of the record
constexpr float RESTORE_FACTOR = 0.9f;
constexpr float HOLD_MAX_PCT = 40.0f;

struct Record {
  uint32_t magic;
  uint32_t sequence;
  float hold;
  float pv;
  float sp;
  uint32_t epoch;       // RTC epoch at the save, 0 = no usable clock
  uint32_t batchId;     // the batch start epoch: another batch is another set of eggs
  uint32_t crc;
};

inline uint32_t recordCrc(const Record &r) { return thermalgain::crc32(reinterpret_cast<const uint8_t *>(&r), offsetof(Record, crc)); }
inline bool valid(const Record &r) {
  return r.magic == MAGIC && r.crc == recordCrc(r) && std::isfinite(r.hold) && r.hold >= 0.0f && r.hold <= HOLD_MAX_PCT &&
         std::isfinite(r.pv) && r.pv > 20.0f && r.pv < 50.0f && std::isfinite(r.sp) && r.sp > 20.0f && r.sp < 50.0f && r.batchId != 0U;
}
inline Record make(uint32_t sequence, float hold, float pv, float sp, uint32_t epoch, uint32_t batchId) {
  Record r{};
  r.magic = MAGIC; r.sequence = sequence; r.hold = hold; r.pv = pv; r.sp = sp; r.epoch = epoch; r.batchId = batchId;
  r.crc = recordCrc(r);
  return r;
}
// Of the two slots, the newest valid record. Returns 0 (none), 1 (A) or 2 (B).
inline uint8_t pick(const Record &a, bool readA, const Record &b, bool readB, Record &out) {
  const bool va = readA && valid(a), vb = readB && valid(b);
  if (va && (!vb || !thermalgain::newer(b.sequence, a.sequence))) { out = a; return 1U; }
  if (vb) { out = b; return 2U; }
  return 0U;
}
inline Record nextToWrite(const Record &a, bool readA, const Record &b, bool readB, float hold, float pv, float sp, uint32_t epoch, uint32_t batchId, bool &toA) {
  Record newest{};
  const uint8_t have = pick(a, readA, b, readB, newest);
  toA = have != 1U;
  return make(have ? newest.sequence + 1UL : 1UL, hold, pv, sp, epoch, batchId);
}

// Should the level be written now? `settledMs` = how long the oven has been settled, `sinceLastSaveMs` since the last write of this run.
inline bool wanted(uint32_t settledMs, uint32_t sinceLastSaveMs, bool haveSaved, float savedHold, uint32_t savedBatch, float hold, uint32_t batchId) {
  if (settledMs < SETTLED_MIN_MS || sinceLastSaveMs < SAVE_MIN_MS || batchId == 0U) return false;
  if (!std::isfinite(hold) || hold < 0.0f || hold > HOLD_MAX_PCT) return false;
  if (!haveSaved || savedBatch != batchId) return true;
  return std::fabs(hold - savedHold) >= SAVE_MOVE_PCT;
}

// The level to start the controller at after a restart, or a negative number: no restore (the 12 % start).
inline float restored(const Record &r, bool recordValid, uint32_t batchId, bool rtcValid, uint32_t nowEpoch, float pv, float sp) {
  if (!recordValid || batchId == 0U || r.batchId != batchId || !std::isfinite(pv) || !std::isfinite(sp)) return -1.0f;
  if (std::fabs(r.sp - sp) > RESTORE_SP_MOVE_C) return -1.0f;
  if (std::fabs(pv - sp) > RESTORE_NEAR_C || std::fabs(pv - r.pv) > RESTORE_PV_MOVE_C) return -1.0f;
  if (rtcValid && r.epoch != 0U) {
    if (nowEpoch < r.epoch || nowEpoch - r.epoch > RESTORE_MAX_GAP_S) return -1.0f;
  }
  const float h = r.hold * RESTORE_FACTOR;
  return h > HOLD_MAX_PCT ? HOLD_MAX_PCT : (h < 0.0f ? 0.0f : h);
}

}  // namespace thermalhold
