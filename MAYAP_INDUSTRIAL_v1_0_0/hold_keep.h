#pragma once
// The hold the oven needed a few minutes ago, kept so that a restart (an update, a power blip) does not have to learn it again. Pure C++11, host tested.
//
// One value, one reader at the start of the controller (thermalv3::Controller::seedHold), one writer (the keeper below, from a quiet oven).
// The observer of the controller stays the only thing that changes hold while the controller runs.
//
//  * Record: 16 bytes in a ring of 512 slots of the external EEPROM (config.h: EEPROM_ADDR_HOLD_RING). Newest = highest sequence with a valid CRC.
//    A torn write has a bad CRC and is ignored; nothing is erased, a slot is simply overwritten after 512 writes.
//  * Writing: every 5 minutes while the oven is quiet (steady()), at most once a minute and 400 times a day. Over 20 years that is at most about
//    5,700 writes per cell (an AT24C512 is rated for 1,000,000; 100,000 is the cautious figure used here).
//  * Using: the record is at most 20 minutes old by the RTC, the set point and the heater ceiling are the same, the value is in range.
//  * Once: the update that brings this feature finds no record. If it restarts a batch that was checkpointed less than 20 minutes ago and the oven is near SV,
//    the controller starts from ONE_TIME_SEED_PCT instead of the prior (remove in 3.1.2).
#include <cmath>
#include <stdint.h>
#include <string.h>

namespace HoldKeep {

constexpr uint8_t MAGIC = 0xB1U;
constexpr uint16_t SLOTS = 512U;
constexpr uint16_t RECORD_BYTES = 16U;
constexpr uint32_t SAVE_PERIOD_S = 300U, MIN_GAP_S = 60U, MAX_PER_DAY = 400U, DAY_S = 86400U;
constexpr uint32_t MAX_AGE_S = 1200U;
constexpr float STEADY_BAND_C = 0.15f, STEADY_HOLD_STEP = 1.0f;
constexpr uint32_t STEADY_S = 600U;
constexpr float SP_TOL_C = 0.3f;
constexpr float ONE_TIME_SEED_PCT = 43.0f, SEED_NEAR_C = 0.5f;
constexpr uint8_t FAILS_TO_DISABLE = 3U;

struct Record {
  uint8_t cap = 100;       // heater ceiling [%]
  uint32_t seq = 0, epoch = 0;
  float hold = 0.0f;       // [%]
  float sp = 0.0f;         // [degC]
};

inline uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t c = 0xFFFFU;
  for (size_t i = 0; i < n; ++i) {
    c ^= static_cast<uint16_t>(d[i]) << 8U;
    for (uint8_t b = 0; b < 8U; ++b) c = (c & 0x8000U) ? static_cast<uint16_t>((c << 1U) ^ 0x1021U) : static_cast<uint16_t>(c << 1U);
  }
  return c;
}
inline void put32(uint8_t *p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF; }
inline uint32_t get32(const uint8_t *p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
inline uint16_t clampU16(float v) { return v <= 0.0f ? 0U : (v >= 65535.0f ? 65535U : static_cast<uint16_t>(v + 0.5f)); }

inline void encode(const Record &r, uint8_t out[RECORD_BYTES]) {
  out[0] = MAGIC; out[1] = r.cap;
  put32(out + 2, r.seq); put32(out + 6, r.epoch);
  const uint16_t h = clampU16(r.hold * 10.0f), s = clampU16(r.sp * 100.0f);
  out[10] = h & 0xFF; out[11] = h >> 8; out[12] = s & 0xFF; out[13] = s >> 8;
  const uint16_t c = crc16(out, 14U);
  out[14] = c & 0xFF; out[15] = c >> 8;
}
inline bool decode(const uint8_t in[RECORD_BYTES], Record &r) {
  if (in[0] != MAGIC) return false;
  const uint16_t c = static_cast<uint16_t>(in[14] | (in[15] << 8));
  if (c != crc16(in, 14U)) return false;
  r.cap = in[1]; r.seq = get32(in + 2); r.epoch = get32(in + 6);
  r.hold = static_cast<float>(in[10] | (in[11] << 8)) / 10.0f; r.sp = static_cast<float>(in[12] | (in[13] << 8)) / 100.0f;
  return true;
}

// S: bool read(uint16_t addr, void *dst, size_t n), bool write(uint16_t addr, const void *src, size_t n)
template <class S>
class Ring {
 public:
  Ring(S &s, uint16_t base) : s_(s), base_(base) {}
  bool begin() {
    have_ = false; ready_ = false; fails_ = 0U;
    uint32_t best = 0U; uint16_t bestSlot = 0U; uint8_t buf[RECORD_BYTES * 8U];
    for (uint16_t slot = 0U; slot < SLOTS; slot = static_cast<uint16_t>(slot + 8U)) {
      if (!s_.read(static_cast<uint16_t>(base_ + slot * RECORD_BYTES), buf, sizeof(buf))) return false;
      for (uint8_t i = 0U; i < 8U; ++i) {
        Record r;
        if (decode(buf + i * RECORD_BYTES, r) && (!have_ || static_cast<int32_t>(r.seq - best) > 0)) { have_ = true; best = r.seq; bestSlot = static_cast<uint16_t>(slot + i); latest_ = r; }
      }
    }
    seq_ = have_ ? best + 1U : 1U; slot_ = have_ ? static_cast<uint16_t>((bestSlot + 1U) % SLOTS) : 0U;
    ready_ = true; return true;
  }
  bool ready() const { return ready_; }
  bool hasRecord() const { return have_; }
  const Record &latest() const { return latest_; }
  bool append(Record r) {
    if (!ready_) return false;
    r.seq = seq_;
    uint8_t buf[RECORD_BYTES]; encode(r, buf);
    if (!s_.write(static_cast<uint16_t>(base_ + slot_ * RECORD_BYTES), buf, RECORD_BYTES)) {
      if (++fails_ >= FAILS_TO_DISABLE) ready_ = false;       // quietly off until the next start
      return false;
    }
    fails_ = 0U; latest_ = r; have_ = true; ++seq_; slot_ = static_cast<uint16_t>((slot_ + 1U) % SLOTS);
    return true;
  }
  // "Clear the thermal learning": every slot gets an invalid record.
  bool clear() {
    uint8_t zero[128]; memset(zero, 0, sizeof(zero));
    for (uint16_t off = 0U; off < SLOTS * RECORD_BYTES; off = static_cast<uint16_t>(off + sizeof(zero)))
      if (!s_.write(static_cast<uint16_t>(base_ + off), zero, sizeof(zero))) return false;
    have_ = false; seq_ = 1U; slot_ = 0U; return true;
  }
 private:
  S &s_; uint16_t base_;
  bool have_ = false, ready_ = false; Record latest_; uint32_t seq_ = 1U; uint16_t slot_ = 0U; uint8_t fails_ = 0U;
};

// When may a record be written? Call tick() once a second.
class Keeper {
 public:
  void tick(float e, bool door, bool fan, bool judgeIdle, float holdPct, bool heaterFault, bool rtcValid) {
    const bool ok = rtcValid && !door && !fan && judgeIdle && !heaterFault && std::isfinite(e) && std::isfinite(holdPct) && std::fabs(e) <= STEADY_BAND_C;
    if (!ok) { steadyS_ = 0U; return; }
    if (steadyS_ == 0U || std::fabs(holdPct - ref_) > STEADY_HOLD_STEP) { ref_ = holdPct; steadyS_ = 0U; }
    if (steadyS_ < 0xFFFFFFF0U) ++steadyS_;
  }
  bool steady() const { return steadyS_ >= STEADY_S; }
  bool due(uint32_t nowS, bool planned) {
    if (!steady()) return false;
    if (haveSaved_) {
      const uint32_t gap = nowS - lastS_;
      if (gap < MIN_GAP_S || (!planned && gap < SAVE_PERIOD_S)) return false;
    }
    if (nowS - dayStartS_ >= DAY_S) { dayStartS_ = nowS; dayCount_ = 0U; }
    return dayCount_ < MAX_PER_DAY;
  }
  void saved(uint32_t nowS) { haveSaved_ = true; lastS_ = nowS; ++dayCount_; }
  void restarted() { steadyS_ = 0U; }
 private:
  uint32_t steadyS_ = 0U, lastS_ = 0U, dayStartS_ = 0U, dayCount_ = 0U; float ref_ = 0.0f; bool haveSaved_ = false;
};

enum class Why : uint8_t { Ok = 0, NoRecord, RtcInvalid, TooOld, Future, SpChanged, CapChanged, Range };

inline Why decideRestore(bool have, const Record &r, bool rtcValid, uint32_t nowEpoch, float sp, uint8_t cap, float &holdOut) {
  if (!have) return Why::NoRecord;
  if (!rtcValid || nowEpoch == 0U || r.epoch == 0U) return Why::RtcInvalid;
  if (nowEpoch < r.epoch) return Why::Future;
  if (nowEpoch - r.epoch > MAX_AGE_S) return Why::TooOld;
  if (std::fabs(sp - r.sp) > SP_TOL_C) return Why::SpChanged;
  if (cap != r.cap) return Why::CapChanged;
  if (!(r.hold >= 3.0f && r.hold <= 90.0f)) return Why::Range;
  holdOut = r.hold; return Why::Ok;
}

// The update that brings this feature: no record yet, a batch checkpointed a few minutes ago, the oven near SV.
inline bool oneTimeSeed(bool ringHasRecord, bool batchRunning, bool rtcValid, uint32_t nowEpoch, uint32_t checkpointEpoch, float errC) {
  if (ringHasRecord || !batchRunning || !rtcValid || checkpointEpoch == 0U || nowEpoch < checkpointEpoch) return false;
  return nowEpoch - checkpointEpoch <= MAX_AGE_S && std::isfinite(errC) && std::fabs(errC) <= SEED_NEAR_C;
}

}  // namespace HoldKeep
