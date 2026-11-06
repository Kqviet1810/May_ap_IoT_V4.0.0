#pragma once
// The load ledger: what the oven needed to hold SV, as a mean over 30 minutes, kept as a short ring of checkpoints. It ADVISES, it never controls: the caller asks
// advise() and decides what to do with the answer (the hold controller takes it as its starting level, the climb controller as a hint); nothing in here writes into a controller.
//
// Why a 30 minute mean (log data of 31/10 - 05/11, 676 settled minutes): the hold level of one moment predicts the level 30 minutes later within 3 points in 80 % of the cases, after 60 minutes
// only in 29 %, after two hours never. A mean over 30 minutes is quieter than a single reading, and a trend between two checkpoints helps a little (within 5 points at 60 minutes: 69 % -> 85 %).
// So the advice is trusted fully for 30 minutes after the last checkpoint, less and less until 90 minutes, and not at all after that.
// The advice is SHADED low (x0.85): rig, oven needing 55 %: advice 20 % too high -> +0.31 degC over SV, 20 % too low -> +0.07 degC and still 2.5 times faster than the 12 % start.
//
// Pure C++ (no Arduino types). MachineController keeps one, feeds it once a second and stores it (pack/unpack, 64 bytes, CRC) in the EEPROM; tests/thermal-load-ledger.cpp drives the same code.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "thermal_gain_record.h"

namespace thermalload {

constexpr uint32_t WINDOW_S = 1800UL;       // one checkpoint = the mean of a 30 minute window
constexpr uint32_t MIN_COVER_S = 600UL;     // ... of which at least 10 minutes were settled
constexpr uint32_t GAP_RESET_S = 120UL;     // a pause longer than this in the feed (a restart): the window in progress is thrown away
constexpr uint32_t FULL_AGE_S = 1800UL;     // advice at full confidence up to this age of the newest checkpoint
constexpr uint32_t MAX_AGE_S = 5400UL;      // ... none after this
constexpr uint32_t TREND_MAX_SPAN_S = 3600UL;
constexpr float SP_TOL_C = 0.10f;           // the set-point must be the one of the checkpoint
constexpr float SHADE = 0.85f;
constexpr float TREND_DAMP = 0.5f;
constexpr float TREND_MAX_PCT = 5.0f;
constexpr float HOLD_MAX_PCT = 60.0f;
constexpr uint8_t RING = 6;                 // three hours of checkpoints
constexpr uint32_t MAGIC = 0x4D4C4C31UL;    // "MLL1"

struct Checkpoint { uint32_t endS = 0U; float hold = 0.0f; float sp = 0.0f; };
struct Advice { bool valid = false; float holdPct = 0.0f; float confidence = 0.0f; uint32_t ageS = 0U; };

#pragma pack(push, 1)
struct PackedCheckpoint { uint32_t endS; uint16_t hold100; uint16_t sp100; };
struct Record {
  uint32_t magic;
  uint32_t sequence;
  uint8_t count;
  uint8_t pad[3];
  PackedCheckpoint cp[RING];      // oldest first
  uint32_t crc;
};
#pragma pack(pop)
static_assert(sizeof(Record) == 64, "the ledger record is one 64 byte slot");

class Ledger {
 public:
  void reset() { *this = Ledger(); }

  // Once per second while the controller runs. `nowS` = the clock in seconds (the RTC epoch, or any monotonic clock that keeps counting over a restart).
  // `settled` = tracking near SV with no door and no fan (the controller says so), `holdPct` = its hold estimate.
  void feed(uint32_t nowS, bool settled, float holdPct, float sp) {
    if (!std::isfinite(holdPct) || !std::isfinite(sp)) return;
    if (haveFeed_ && static_cast<uint32_t>(nowS - lastFeedS_) > GAP_RESET_S) dropWindow();
    lastFeedS_ = nowS; haveFeed_ = true;
    if (!winOpen_) { winStartS_ = nowS; winSp_ = sp; winOpen_ = true; sum_ = 0.0; cover_ = 0U; }
    if (std::fabs(sp - winSp_) > 0.05f) { dropWindow(); winStartS_ = nowS; winSp_ = sp; winOpen_ = true; }
    if (settled) { sum_ += static_cast<double>(holdPct); ++cover_; }
    if (static_cast<uint32_t>(nowS - winStartS_) + 1U >= WINDOW_S) {
      if (cover_ >= MIN_COVER_S) push(nowS, static_cast<float>(sum_ / static_cast<double>(cover_)), winSp_);
      dropWindow();
    }
  }

  Advice advise(uint32_t nowS, float sp) const {
    Advice a;
    if (count_ == 0U || !std::isfinite(sp)) return a;
    const Checkpoint &last = cp_[count_ - 1U];
    if (static_cast<int32_t>(nowS - last.endS) < 0) return a;                  // a clock that went backwards: not usable
    const uint32_t age = nowS - last.endS;
    if (age > MAX_AGE_S || std::fabs(sp - last.sp) > SP_TOL_C) return a;
    float pred = last.hold;
    if (count_ >= 2U) {
      const Checkpoint &prev = cp_[count_ - 2U];
      const uint32_t span = last.endS - prev.endS;
      if (span > 0U && span <= TREND_MAX_SPAN_S && std::fabs(prev.sp - last.sp) <= SP_TOL_C) {
        const float perWindow = (last.hold - prev.hold) * (static_cast<float>(WINDOW_S) / static_cast<float>(span));
        const float step = TREND_DAMP * perWindow * (static_cast<float>(age) / static_cast<float>(WINDOW_S));
        pred += std::max(-TREND_MAX_PCT, std::min(TREND_MAX_PCT, step));
      }
    }
    pred = std::max(0.0f, std::min(HOLD_MAX_PCT, pred));
    a.valid = true;
    a.ageS = age;
    a.holdPct = SHADE * pred;
    a.confidence = age <= FULL_AGE_S ? 1.0f : std::max(0.0f, 1.0f - static_cast<float>(age - FULL_AGE_S) / static_cast<float>(MAX_AGE_S - FULL_AGE_S));
    return a;
  }

  // What the caller should start from: its own prior, moved toward the advice by the confidence.
  static float blend(float prior, const Advice &a) { return a.valid ? prior + a.confidence * (a.holdPct - prior) : prior; }

  uint8_t count() const { return count_; }
  const Checkpoint &checkpoint(uint8_t i) const { return cp_[i]; }
  void clear() { count_ = 0U; dropWindow(); haveFeed_ = false; }

  // Persistence. The sequence number lets two alternating slots tell which one is newer (as thermal_gain_record.h).
  Record pack(uint32_t sequence) const {
    Record r;
    std::memset(&r, 0, sizeof r);
    r.magic = MAGIC; r.sequence = sequence; r.count = count_;
    for (uint8_t i = 0U; i < count_; ++i) {
      r.cp[i].endS = cp_[i].endS;
      r.cp[i].hold100 = static_cast<uint16_t>(std::lround(std::max(0.0f, std::min(HOLD_MAX_PCT, cp_[i].hold)) * 100.0f));
      r.cp[i].sp100 = static_cast<uint16_t>(std::lround(std::max(0.0f, std::min(600.0f, cp_[i].sp)) * 100.0f));
    }
    r.crc = thermalgain::crc32(reinterpret_cast<const uint8_t *>(&r), offsetof(Record, crc));
    return r;
  }
  static bool valid(const Record &r) {
    return r.magic == MAGIC && r.count <= RING && r.crc == thermalgain::crc32(reinterpret_cast<const uint8_t *>(&r), offsetof(Record, crc));
  }
  bool unpack(const Record &r) {
    if (!valid(r)) return false;
    clear();
    for (uint8_t i = 0U; i < r.count; ++i) {
      cp_[i].endS = r.cp[i].endS;
      cp_[i].hold = static_cast<float>(r.cp[i].hold100) * 0.01f;
      cp_[i].sp = static_cast<float>(r.cp[i].sp100) * 0.01f;
    }
    count_ = r.count;
    return true;
  }
  // true when a checkpoint was added since the last call: the caller writes the record then (at most one write per 30 minutes).
  bool takeDirty() { const bool d = dirty_; dirty_ = false; return d; }

 private:
  void push(uint32_t endS, float hold, float sp) {
    if (count_ == RING) { for (uint8_t i = 1U; i < RING; ++i) cp_[i - 1U] = cp_[i]; count_ = RING - 1U; }
    cp_[count_].endS = endS; cp_[count_].hold = std::max(0.0f, std::min(HOLD_MAX_PCT, hold)); cp_[count_].sp = sp;
    ++count_;
    dirty_ = true;
  }
  void dropWindow() { winOpen_ = false; sum_ = 0.0; cover_ = 0U; }

  Checkpoint cp_[RING];
  uint8_t count_ = 0U;
  bool winOpen_ = false, haveFeed_ = false, dirty_ = false;
  uint32_t winStartS_ = 0U, lastFeedS_ = 0U, cover_ = 0U;
  float winSp_ = 0.0f;
  double sum_ = 0.0;
};

}  // namespace thermalload
