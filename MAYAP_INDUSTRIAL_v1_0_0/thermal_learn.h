#pragma once
// What the thermal controller learned about THIS oven, kept across a restart. Pure C++ (no Arduino types): MachineController keeps the bytes in the
// internal NVS and tests/thermal-learn.cpp drives this very code.
//
// Only a state that proved itself is kept: the snapshot is written after the oven has stood within +-0.2 degC of SV for two hours with no door,
// no heater-response suspicion, no High / Emergency line and a hold level that has stopped moving (StableTracker), then again only after two more
// stable hours if the hold level moved by more than 1.5 %. At most one write every two hours, so the flash is touched a handful of times a day.
//
// What comes back at the next start, and why:
//   heater gain, transit time, horizon   properties of the oven; they only start the next first climb, which measures them again.
//                                        Rejected when the heater cap (maxHeaterPower) is not the one they were learned with.
//   hold level                           a property of the load and the room: it is applied only when the batch that is resumed is the very batch
//                                        the snapshot belongs to (same start time), because a hold level learned on day 18 is wrong on day 1.
// A snapshot that fails its checksum, its version or any range is ignored as a whole: the controller then starts from its priors, as it always did.
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace thermallearn {

constexpr uint32_t MAGIC = 0x314C4854UL;      // "THL1"
constexpr uint8_t VERSION = 1U;
constexpr size_t BLOB_SIZE = 32U;

// The ranges the controller itself accepts (thermal_v3.h: KAIR_MIN/MAX, TAU_MIN_S/MAX_S, HOLD_MAX_PCT, MAX_PIPE_LEN).
constexpr float KAIR_MIN = 5.0e-5f, KAIR_MAX = 7.5e-4f;
constexpr float TAU_MIN = 90.0f, TAU_MAX = 220.0f;
constexpr float HOLD_MAX = 85.0f;
constexpr uint16_t PIPE_MIN = 15U, PIPE_MAX = 319U;

struct Snapshot {
  float pHold = 0.0f;          // learned hold level [%]
  float kAir = 0.0f;           // heater gain [degC/s per %]
  float tau = 0.0f;            // transit time [s]
  uint16_t pipeLen = 0U;       // prediction horizon [s]
  float sp = 0.0f;             // SV when it was saved
  uint8_t capPct = 0U;         // heater cap (maxHeaterPower) when it was saved
  uint32_t batchEpoch = 0U;    // start time of the batch it was learned in (0: none)
  uint32_t savedEpoch = 0U;    // wall clock when it was saved (0: unknown)
};

inline uint32_t crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0U; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0U; b < 8U; ++b) crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
  }
  return ~crc;
}

namespace detail {
inline void put16(uint8_t *p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
inline void put32(uint8_t *p, uint32_t v) { put16(p, static_cast<uint16_t>(v)); put16(p + 2, static_cast<uint16_t>(v >> 16)); }
inline uint16_t get16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t get32(const uint8_t *p) { return static_cast<uint32_t>(get16(p)) | (static_cast<uint32_t>(get16(p + 2)) << 16); }
inline uint16_t scaled16(float v, float scale, float maxValue) {
  if (!std::isfinite(v) || v < 0.0f) return 0U;
  if (v > maxValue) v = maxValue;
  return static_cast<uint16_t>(v * scale + 0.5f);
}
}  // namespace detail

inline bool inRange(const Snapshot &s) {
  return std::isfinite(s.pHold) && s.pHold >= 0.0f && s.pHold <= HOLD_MAX &&
         std::isfinite(s.kAir) && s.kAir >= KAIR_MIN && s.kAir <= KAIR_MAX &&
         std::isfinite(s.tau) && s.tau >= TAU_MIN && s.tau <= TAU_MAX &&
         s.pipeLen >= PIPE_MIN && s.pipeLen <= PIPE_MAX &&
         std::isfinite(s.sp) && s.sp > 20.0f && s.sp < 50.0f &&
         s.capPct > 0U && s.capPct <= 100U;
}

// 32 bytes: magic(4) version(1) cap(1) sp*100(2) hold*100(2) kAir*1e8(4) tau*10(2) pipe(2) batchEpoch(4) savedEpoch(4) reserved(2) crc(4)
inline bool encode(const Snapshot &s, uint8_t *out) {
  if (!inRange(s)) return false;
  for (size_t i = 0U; i < BLOB_SIZE; ++i) out[i] = 0U;
  detail::put32(out + 0, MAGIC);
  out[4] = VERSION;
  out[5] = s.capPct;
  detail::put16(out + 6, detail::scaled16(s.sp, 100.0f, 600.0f));
  detail::put16(out + 8, detail::scaled16(s.pHold, 100.0f, 600.0f));
  detail::put32(out + 10, static_cast<uint32_t>(s.kAir * 1.0e8f + 0.5f));
  detail::put16(out + 14, detail::scaled16(s.tau, 10.0f, 6000.0f));
  detail::put16(out + 16, s.pipeLen);
  detail::put32(out + 18, s.batchEpoch);
  detail::put32(out + 22, s.savedEpoch);
  detail::put32(out + 28, crc32(out, 28U));
  return true;
}

inline bool decode(const uint8_t *in, size_t length, Snapshot &s) {
  if (!in || length != BLOB_SIZE) return false;
  if (detail::get32(in + 0) != MAGIC || in[4] != VERSION) return false;
  if (detail::get32(in + 28) != crc32(in, 28U)) return false;
  Snapshot t;
  t.capPct = in[5];
  t.sp = static_cast<float>(detail::get16(in + 6)) / 100.0f;
  t.pHold = static_cast<float>(detail::get16(in + 8)) / 100.0f;
  t.kAir = static_cast<float>(detail::get32(in + 10)) / 1.0e8f;
  t.tau = static_cast<float>(detail::get16(in + 14)) / 10.0f;
  t.pipeLen = detail::get16(in + 16);
  t.batchEpoch = detail::get32(in + 18);
  t.savedEpoch = detail::get32(in + 22);
  if (!inRange(t)) return false;
  s = t;
  return true;
}

// The oven constants may start the next first climb only for the same heater cap.
inline bool ovenUsable(const Snapshot &s, uint8_t capPct) { return inRange(s) && s.capPct == capPct; }

// The hold level may be applied only to the batch it was learned in (a resumed batch), at about the same SV.
inline bool holdUsable(const Snapshot &s, uint8_t capPct, uint32_t batchEpoch, float sp) {
  return ovenUsable(s, capPct) && batchEpoch != 0U && s.batchEpoch == batchEpoch && std::isfinite(sp) && std::fabs(sp - s.sp) <= 1.0f;
}

class StableTracker {
 public:
  struct Params {
    uint32_t needMs = 7200000UL;          // two hours of stable running before the first snapshot, and between two snapshots
    float bandC = 0.2f;                   // |PV - SV| at most this
    float spreadPct = 1.0f;               // the hold level moved by at most this in the last window ...
    uint32_t windowMs = 1800000UL;        // ... 30 minutes
    float resaveDeltaPct = 1.5f;          // a later snapshot only when the hold level is this far from the saved one
    uint32_t retryMs = 600000UL;          // a write that failed is tried again after this
    uint32_t maxGapMs = 5000UL;           // a longer silence between two calls is a break
  };
  static constexpr uint8_t SAMPLES = 31U;   // one hold sample a minute over the window (+1)

  void reset() {
    stableMs_ = 0U; started_ = false; samples_ = 0U; head_ = 0U; lastSampleMs_ = 0U;
    savedValid_ = false; savedHold_ = 0.0f; stableAtSave_ = 0U; retryAt_ = 0U; haveRetry_ = false;
  }
  // The stable stretch is over (a door, an alarm ...): count from zero. The saved snapshot stays what it is.
  void breakStreak() { stableMs_ = 0U; started_ = false; samples_ = 0U; head_ = 0U; stableAtSave_ = 0U; }
  // The stored snapshot is gone (cleared by hand, heater fault): the next stable two hours write a new one.
  void invalidate() { savedValid_ = false; breakStreak(); }
  // The stored snapshot is `hold` (loaded at boot, or written just now).
  void saved(float hold) { savedValid_ = true; savedHold_ = hold; stableAtSave_ = stableMs_; haveRetry_ = false; }

  // Call every cycle. `ok` = everything that must hold for the oven to count as stable (batch running, heater allowed, no door, no heater-response
  // suspicion, no High / Emergency, no test mode, sensor usable). Returns true when a snapshot should be written now.
  bool update(uint32_t now, bool ok, float pv, float sp, float hold, const Params &p) {
    if (!started_) { started_ = true; lastMs_ = now; }
    const uint32_t dt = static_cast<uint32_t>(now - lastMs_);
    lastMs_ = now;
    if (!ok || !std::isfinite(pv) || !std::isfinite(sp) || !std::isfinite(hold) || std::fabs(pv - sp) > p.bandC || dt > p.maxGapMs) {
      breakStreak();
      return false;
    }
    stableMs_ += dt;
    if (samples_ == 0U || static_cast<uint32_t>(now - lastSampleMs_) >= 60000UL) {
      ring_[head_] = hold;
      head_ = static_cast<uint8_t>((head_ + 1U) % SAMPLES);
      if (samples_ < SAMPLES) ++samples_;
      lastSampleMs_ = now;
    }
    if (stableMs_ < p.needMs) return false;
    // the hold level has stopped moving: spread of the samples that cover the window
    uint8_t n = static_cast<uint8_t>(p.windowMs / 60000UL + 1UL);
    if (n > SAMPLES) n = SAMPLES;
    if (samples_ < n) return false;
    float lo = hold, hi = hold;
    for (uint8_t i = 0U; i < n; ++i) {
      const float v = ring_[(head_ + SAMPLES - 1U - i) % SAMPLES];
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    if (hi - lo > p.spreadPct) return false;
    if (haveRetry_ && static_cast<int32_t>(now - retryAt_) < 0) return false;
    bool want = false;
    if (!savedValid_) want = true;
    else if (stableMs_ - stableAtSave_ >= p.needMs && std::fabs(hold - savedHold_) > p.resaveDeltaPct) want = true;
    if (!want) return false;
    haveRetry_ = true;                                     // if the caller does not confirm with saved(), try again later
    retryAt_ = now + p.retryMs;
    return true;
  }

  uint32_t stableMs() const { return stableMs_; }
  bool savedValid() const { return savedValid_; }

 private:
  uint32_t stableMs_ = 0U, lastMs_ = 0U, lastSampleMs_ = 0U, stableAtSave_ = 0U, retryAt_ = 0U;
  bool started_ = false, savedValid_ = false, haveRetry_ = false;
  float savedHold_ = 0.0f;
  float ring_[SAMPLES] = {};
  uint8_t samples_ = 0U, head_ = 0U;
};

}  // namespace thermallearn
