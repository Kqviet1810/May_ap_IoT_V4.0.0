#pragma once
// PID Monitor log ring (Thermal V2): one compact sample every 10 s, the last two hours, kept in RAM.  The Web asks nothing: while its PID Monitor card is
// open every snapshot carries the next few samples of the ring (see transaction_bridge.h), so the page collects the whole ring in about two minutes and
// can save it as CSV.  Pure C++ (host tested).  The control task is the only writer; the network task reads.  A read that races a write can see one
// half-updated sample at the oldest end of the ring: diagnostic data only, nothing here feeds a control decision.
#include <stdint.h>
#include <math.h>

namespace MayapPidLog {
constexpr uint16_t CAPACITY = 180U;      // 10 s per sample: 30 min (the owner asked for the whole climb + one disturbance; also keeps RAM for the network stack)
constexpr uint8_t CHUNK = 10U;           // samples per snapshot
constexpr uint32_t SAMPLE_PERIOD_MS = 10000U;

enum : uint8_t { FlPhaseMask = 0x03, FlSuspect = 0x04, FlVentFan = 0x08, FlSsr = 0x10, FlMaster = 0x20, FlBatch = 0x40, FlSensorOk = 0x80 };

// ctl byte: bit0 door guard, bit1 transit time measured, bits2-3 whole fan-budget tokens (0..3), bits4-7 the reason the exhaust fan runs (ventReason).
enum : uint8_t { CtlDoor = 0x01, CtlIdentified = 0x02 };

struct Sample {
  uint16_t pv100 = 0, sp100 = 0, pred100 = 0;   // degC * 100
  uint8_t req2 = 0, act2 = 0;                   // percent * 2 (0.5 % steps)
  uint8_t flags = 0;
  uint8_t spare = 0;
  // Diagnostics that explain a decision (sent in a second, separate chunk: an older Web ignores it).
  uint16_t safe100 = 0;                         // the reading the safety lines use, degC * 100
  uint8_t hold2 = 0;                            // learned hold level, percent * 2
  uint8_t kair = 0;                             // heater gain, 1e-5 degC/s per %
  uint8_t tau = 0;                              // transit time [s]
  int8_t integ4 = 0;                            // integral term, percent * 4
  uint8_t vent = 0;                             // vent line: bits0-2 why (VentLine::Why), bits3-7 its state bits (VentLine::Bit*)
  uint8_t ctl = 0;
  uint8_t learn = 0;                            // bits0-1 where the controller's numbers came from (0 priors, 1 oven restored, 2 oven + hold restored), bit2 a snapshot is stored, bits3-7 stable minutes / 4 (0..31)
};

inline uint16_t toC100(float v) {
  if (!isfinite(v) || v <= 0.0f) return 0U;
  const float x = v * 100.0f + 0.5f;
  return x >= 65535.0f ? 65535U : static_cast<uint16_t>(x);
}
inline uint8_t toU8(float v) {
  if (!isfinite(v) || v <= 0.0f) return 0U;
  const float x = v + 0.5f;
  return x >= 255.0f ? 255U : static_cast<uint8_t>(x);
}
inline int8_t toI8x4(float v) {
  if (!isfinite(v)) return 0;
  float x = v * 4.0f;
  x += x >= 0.0f ? 0.5f : -0.5f;
  return x >= 127.0f ? static_cast<int8_t>(127) : (x <= -127.0f ? static_cast<int8_t>(-127) : static_cast<int8_t>(x));
}
inline uint8_t toPct2(float v) {
  if (!isfinite(v) || v <= 0.0f) return 0U;
  const float x = v * 2.0f + 0.5f;
  return x >= 255.0f ? 255U : static_cast<uint8_t>(x);
}

class Ring {
 public:
  void push(const Sample &s) { buf_[head_ % CAPACITY] = s; head_ = head_ + 1U; }
  uint32_t head() const { return head_; }                              // index of the NEXT sample = how many were ever pushed
  uint32_t oldest() const { return head_ > CAPACITY ? head_ - CAPACITY : 0U; }
  void clear() { head_ = 0U; }
  // Up to CHUNK samples starting at `cursor` (moved up to the oldest if it fell off the ring; `start` = the index of the first one returned); `cursor` advances, and wraps to the oldest after the newest.
  uint8_t fetch(uint32_t &cursor, Sample *out, uint32_t &start) const {
    const uint32_t h = head_, o = h > CAPACITY ? h - CAPACITY : 0U;
    if (h == 0U) { cursor = 0U; start = 0U; return 0U; }
    if (cursor < o || cursor >= h) cursor = o;
    start = cursor;
    uint8_t n = 0U;
    while (n < CHUNK && cursor < h) { out[n++] = buf_[cursor % CAPACITY]; ++cursor; }
    return n;
  }
  const Sample &at(uint32_t index) const { return buf_[index % CAPACITY]; }
 private:
  Sample buf_[CAPACITY];
  volatile uint32_t head_ = 0U;
};

inline Ring &ring() { static Ring r; return r; }
}  // namespace MayapPidLog
