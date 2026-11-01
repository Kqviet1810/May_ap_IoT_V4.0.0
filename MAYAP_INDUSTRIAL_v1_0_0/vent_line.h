#pragma once
#include <cmath>
#include <cstdint>

// The vent line: when the exhaust fans of the OVERHEAT run. Pure C++ (no Arduino types): MachineController calls it every cycle and the host
// tests (tests/vent-line.cpp) drive the very same code.
//
// The two small fans of this machine take little heat away and the contactor that switches them is rated for far fewer operations than the
// heater, so the line is a LAST RESORT, not a regulator, and it never cuts the heater: the controller lowers its own power from the error and the
// line only watches whether that is enough.
//
//   Reading: the sensor gives 0.1 degC steps, so a temperature that stands between two steps flickers (37.5 / 37.6 / 37.5 ...). Every decision is
//   taken on the MEAN of the last seconds, never on "the reading stayed over the threshold without a single dip". A reading R means the true
//   temperature is in R +- step/2, so the threshold in true terms is onTemp - step/2 ("over"): the mean of the readings crosses it exactly where
//   the true temperature does.
//
//   WATCH   starts when the mean is over. The heater has had its chance; the watch lasts W = clamp(1.2 x transit time, 150 s, 260 s), longer than
//           the delay of the oven, because a cut of the heater cannot show before that. A fall of the mean by fallC restarts the watch (the
//           controller is bringing it down: no fan).
//   FAN ON  (any one, and the fans have rested for minOffMs):
//           - STUCK:  the watch ran out and the mean did not fall;
//           - FAST:   the mean is a further fastC over the threshold for confirmMs (a decisive overshoot: no waiting);
//           - PRED:   the controller's own prediction says predMarginC more is on its way, while the mean is over (heat in flight);
//           - FLOOR:  the heater average is at/below floorPct for floorHoldMs and the mean has been over for floorOverMs (nothing left to cut).
//   FAN OFF the mean is back at/below the release threshold (+ epsC) for confirmMs and the fans ran for minOnMs; at the latest after maxOnMs (a
//           reading that is still high after that is for the High / Emergency lines, which force the fans through their own path and are not
//           limited by any of this).
//           The controller's own prediction can hold STUCK and FLOOR back: when it says the temperature will be under the threshold (the cut of the
//           heater is still on its way to the sensor) the line waits, for at most predVetoWindows windows; FAST and PRED are never held back.
//   Contactor: a starts budget (token bucket: bucketCap starts at once, one more every refillMs). A start is a contactor operation only when the
//           fans were stopped; when they already run (the day profile) the line takes over without spending a token, and a profile block that is
//           due within coalesceMs is waited for instead of starting the fans a minute earlier (a decisive FAST / PRED does not wait).
class VentLine {
 public:
  enum Why : uint8_t { WhyNone = 0U, WhyStuck = 1U, WhyFast = 2U, WhyFloor = 3U, WhyPredicted = 4U };
  enum : uint8_t { BitWatching = 1U, BitActive = 2U, BitBudget = 4U, BitDeferred = 8U, BitMaxRun = 16U };

  struct Params {
    uint32_t confirmMs = 20000UL, minOnMs = 180000UL, maxOnMs = 600000UL, minOffMs = 300000UL;
    uint32_t windowMinMs = 150000UL, windowMaxMs = 260000UL;
    float windowTauK = 1.2f;
    float defaultTauS = 180.0f;               // when the controller has not measured the transit time
    uint32_t floorHoldMs = 60000UL, floorOverMs = 150000UL;
    float floorPct = 3.0f;
    uint8_t bucketCap = 3U;
    uint32_t refillMs = 1800000UL;
    uint32_t coalesceMs = 300000UL;
    float epsC = 0.005f, fallC = 0.05f, fastC = 0.1f, predMarginC = 0.2f;
    float predVetoC = 0.03f;                  // the controller predicts the mean this far under the threshold: heat is on its way DOWN, wait
    float predVetoWindows = 2.0f;             // ... for at most this many watch windows
    float stepC = 0.1f;                       // the step of the reading (0.1 or 0.01 degC)
  };
  struct Ctx {
    float tauS = NAN;                         // transit time of the oven [s]
    float heaterAvgPct = NAN;                 // mean heater power over the last minute [%]
    float predictedC = NAN;                   // the controller's predicted temperature
    bool fanRunning = false;                  // the exhaust fan is running now, for any reason
    float profileOnInS = NAN;                 // seconds until the day profile starts the fans (NAN: none due)
  };
  static constexpr uint8_t RING = 30U;

  void reset() {
    clearWatch();
    active_ = false;
    why_ = WhyNone;
    bits_ = 0U;
    notBefore_ = 0U;
    haveNotBefore_ = false;
    tokens_ = -1.0f;
    started_ = false;
  }

  bool update(uint32_t now, bool valid, float temp, float onTemp, float offTemp, const Params &p) {
    return update(now, valid, temp, onTemp, offTemp, p, Ctx());
  }
  bool update(uint32_t now, bool valid, float temp, float onTemp, float offTemp, const Params &p, const Ctx &c) {
    if (!valid || !std::isfinite(temp)) {
      active_ = false;
      why_ = WhyNone;
      bits_ = 0U;
      clearWatch();
      return false;
    }
    if (tokens_ < 0.0f) { tokens_ = static_cast<float>(p.bucketCap); lastRefill_ = now; }
    if (!started_) { started_ = true; lastTick_ = now; }
    const uint32_t gap = static_cast<uint32_t>(now - lastTick_);
    if (gap < 1000UL && cnt_ != 0U) return active_;            // decisions are taken once a second
    uint32_t n = gap / 1000UL;
    if (n == 0U || cnt_ == 0U) n = 1U;
    if (n > RING) n = RING;
    for (uint32_t i = 0U; i < n; ++i) push(temp);
    lastTick_ = now;
    refill(now, p);
    tick(now, onTemp, offTemp, p, c);
    uint8_t b = 0U;
    if (overActive_) b = static_cast<uint8_t>(b | BitWatching);
    if (active_) b = static_cast<uint8_t>(b | BitActive);
    if (budgetBlocked_) b = static_cast<uint8_t>(b | BitBudget);
    if (deferred_) b = static_cast<uint8_t>(b | BitDeferred);
    if (maxRun_) b = static_cast<uint8_t>(b | BitMaxRun);
    bits_ = b;
    return active_;
  }

  bool active() const { return active_; }
  uint8_t why() const { return why_; }
  uint8_t bits() const { return bits_; }
  float tokens() const { return tokens_ < 0.0f ? 0.0f : tokens_; }
  float mean30() const { return mean(RING); }

 private:
  void clearWatch() {
    cnt_ = 0U; head_ = 0U;
    overActive_ = false;
    fastOn_ = predOn_ = floorOn_ = offOn_ = false;
    budgetBlocked_ = deferred_ = maxRun_ = false;
    lastTick_ = 0U;
    started_ = false;
  }
  void push(float t) {
    ring_[head_] = t;
    head_ = static_cast<uint8_t>((head_ + 1U) % RING);
    if (cnt_ < RING) ++cnt_;
  }
  float mean(uint8_t k) const {
    const uint8_t m = k < cnt_ ? k : cnt_;
    if (m == 0U) return NAN;
    float s = 0.0f;
    for (uint8_t i = 0U; i < m; ++i) s += ring_[(head_ + RING - 1U - i) % RING];
    return s / static_cast<float>(m);
  }
  void refill(uint32_t now, const Params &p) {
    const uint32_t dt = static_cast<uint32_t>(now - lastRefill_);
    lastRefill_ = now;
    if (p.refillMs == 0U) return;
    tokens_ += static_cast<float>(dt) / static_cast<float>(p.refillMs);
    if (tokens_ > static_cast<float>(p.bucketCap)) tokens_ = static_cast<float>(p.bucketCap);
  }
  static uint32_t since(uint32_t now, uint32_t t) { return static_cast<uint32_t>(now - t); }

  void tick(uint32_t now, float onTemp, float offTemp, const Params &p, const Ctx &c) {
    const float half = 0.5f * p.stepC;
    const float overLvl = onTemp - half;
    const bool haveMean = cnt_ >= 20U;
    const float m30 = mean(RING), m10 = mean(10U);

    // ---- the fans are on: when do they go off
    if (active_) {
      const uint32_t run = since(now, activeSince_);
      const bool offPast = haveMean && m10 <= offTemp + p.epsC;
      if (offPast) { if (!offOn_) { offOn_ = true; offSince_ = now; } } else offOn_ = false;
      const bool offConfirmed = offOn_ && since(now, offSince_) >= p.confirmMs;
      if (run >= p.maxOnMs || (run >= p.minOnMs && offConfirmed)) {
        maxRun_ = run >= p.maxOnMs && !(run >= p.minOnMs && offConfirmed);
        active_ = false;
        why_ = WhyNone;
        offOn_ = false;
        notBefore_ = now + p.minOffMs;
        haveNotBefore_ = true;
      }
      deferred_ = false;
      budgetBlocked_ = false;
    }

    const bool rested = !haveNotBefore_ || static_cast<int32_t>(now - notBefore_) >= 0;      // after a stop of this very tick: it has just begun to rest

    // ---- is the reading over
    bool over = haveMean && m30 >= overLvl;
    if (overActive_ && haveMean) over = m30 >= overLvl - 0.02f;     // a little hysteresis: the watch does not flap on the edge
    if (!over) {
      overActive_ = false;
      fastOn_ = predOn_ = floorOn_ = false;
      deferred_ = budgetBlocked_ = false;
      return;
    }
    if (!overActive_) {
      overActive_ = true;
      overSince_ = watchSince_ = now;
      p0_ = m30;
      fastOn_ = predOn_ = floorOn_ = false;
      if (!active_) maxRun_ = false;
    }
    if (m30 > p0_) p0_ = m30;                                          // the watch is measured from the peak the mean has reached
    if (m30 <= p0_ - p.fallC) { p0_ = m30; watchSince_ = now; }      // it is coming down: a new watch from the lower level

    if (active_) return;

    const float tau = std::isfinite(c.tauS) && c.tauS > 30.0f ? c.tauS : p.defaultTauS;
    uint32_t windowMs = static_cast<uint32_t>(p.windowTauK * tau * 1000.0f);
    if (windowMs < p.windowMinMs) windowMs = p.windowMinMs;
    if (windowMs > p.windowMaxMs) windowMs = p.windowMaxMs;

    // FAST: a decisive overshoot
    if (m10 >= overLvl + p.fastC) { if (!fastOn_) { fastOn_ = true; fastSince_ = now; } } else fastOn_ = false;
    const bool fast = fastOn_ && since(now, fastSince_) >= p.confirmMs;
    // PRED: heat is on its way
    const bool predNow = std::isfinite(c.predictedC) && c.predictedC >= overLvl + p.predMarginC;
    if (predNow) { if (!predOn_) { predOn_ = true; predSince_ = now; } } else predOn_ = false;
    const bool pred = predOn_ && since(now, predSince_) >= p.confirmMs;
    // FLOOR: the heater has nothing left to give up
    const bool atFloor = std::isfinite(c.heaterAvgPct) && c.heaterAvgPct <= p.floorPct;
    if (atFloor) { if (!floorOn_) { floorOn_ = true; floorSince_ = now; } } else floorOn_ = false;
    const bool floorHit = floorOn_ && since(now, floorSince_) >= p.floorHoldMs && since(now, overSince_) >= p.floorOverMs;
    // STUCK: the watch ran out
    const bool stuck = since(now, watchSince_) >= windowMs;
    // the prediction of the controller says it is on its way down: STUCK and FLOOR wait (a bounded time)
    const bool predDown = std::isfinite(c.predictedC) && c.predictedC <= overLvl - p.predVetoC &&
                          static_cast<float>(since(now, watchSince_)) < p.predVetoWindows * static_cast<float>(windowMs);

    uint8_t want = WhyNone;
    if (fast) want = WhyFast;
    else if (pred) want = WhyPredicted;
    else if (floorHit && !predDown) want = WhyFloor;
    else if (stuck && !predDown) want = WhyStuck;
    deferred_ = false;
    budgetBlocked_ = false;
    if (want == WhyNone || !rested) return;

    const bool decisive = want == WhyFast || want == WhyPredicted;
    if (!c.fanRunning && !decisive && std::isfinite(c.profileOnInS) && c.profileOnInS >= 0.0f &&
        c.profileOnInS * 1000.0f <= static_cast<float>(p.coalesceMs)) {
      deferred_ = true;                                                 // the day profile starts the fans soon: wait for it
      return;
    }
    if (!c.fanRunning) {                                                // a contactor operation: it is paid for from the budget
      if (tokens_ < 1.0f) { budgetBlocked_ = true; return; }
      tokens_ -= 1.0f;
    }
    active_ = true;
    why_ = want;
    activeSince_ = now;
    offOn_ = false;
    fastOn_ = predOn_ = floorOn_ = false;
  }

  bool active_ = false;
  uint8_t why_ = WhyNone, bits_ = 0U;
  float ring_[RING] = {};
  uint8_t cnt_ = 0U, head_ = 0U;
  bool started_ = false;
  uint32_t lastTick_ = 0U, lastRefill_ = 0U;
  float tokens_ = -1.0f;
  bool overActive_ = false;
  uint32_t overSince_ = 0U, watchSince_ = 0U, activeSince_ = 0U, fastSince_ = 0U, predSince_ = 0U, floorSince_ = 0U, offSince_ = 0U;
  float p0_ = 0.0f;
  bool fastOn_ = false, predOn_ = false, floorOn_ = false, offOn_ = false;
  bool budgetBlocked_ = false, deferred_ = false, maxRun_ = false;
  uint32_t notBefore_ = 0U;
  bool haveNotBefore_ = false;
};
