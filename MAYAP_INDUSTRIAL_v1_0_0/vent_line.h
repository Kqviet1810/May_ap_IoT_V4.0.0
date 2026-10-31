#pragma once
#include <cmath>
#include <cstdint>

// The vent line: when the exhaust fans of the overheat run. Pure C++ (no Arduino types): MachineController calls it every cycle and the host
// tests (tests/vent-line.cpp) drive the very same code.
//
// The two small fans of this machine take only a little heat away, and the contactor that switches them is rated for far fewer operations than
// the heater. So the line is a last resort, not a regulator, and it never cuts the heater (the controller lowers its own power from the error):
//   ON : the reading is at/over the vent threshold and (a) has stayed there for trendMs without any sign of falling (a fall of fallC below where
//        the wait began restarts it), or (b) is a further fastC over the threshold for confirmMs (a decisive overshoot) - and the fans have rested
//        for minOffMs and have not been started maxStartsPerHour times in the last hour;
//   OFF: the reading is back at/below the release threshold (+ epsC) for confirmMs and the fans have run for minOnMs.
// High / Emergency force the fans through their own path and are not limited by any of this.
class VentLine {
 public:
  struct Params {
    uint32_t confirmMs = 20000UL, trendMs = 60000UL, minOnMs = 180000UL, minOffMs = 300000UL;
    uint8_t maxStartsPerHour = 6U;
    float epsC = 0.005f, fallC = 0.05f, fastC = 0.1f;
  };
  static constexpr uint8_t MAX_STARTS = 12U;

  void reset() {
    active_ = false;
    aboveOn_ = fastOn_ = offOn_ = false;
    aboveSince_ = fastSince_ = offSince_ = 0U;
    notBefore_ = 0U;
    haveNotBefore_ = false;
    for (uint8_t i = 0U; i < MAX_STARTS; ++i) { startAt_[i] = 0U; startUsed_[i] = false; }
    startIdx_ = 0U;
  }

  bool update(uint32_t now, bool valid, float temp, float onTemp, float offTemp, const Params &p) {
    if (!valid || !std::isfinite(temp)) {
      active_ = false;
      aboveOn_ = fastOn_ = offOn_ = false;
      return false;
    }
    const bool onPast = temp >= onTemp;
    if (!onPast) {
      aboveOn_ = false;
    } else if (!aboveOn_ || temp < abovePv_ - p.fallC) {
      aboveOn_ = true;
      aboveSince_ = now;
      abovePv_ = temp;
    }
    const bool trend = aboveOn_ && static_cast<uint32_t>(now - aboveSince_) >= p.trendMs;
    const bool fastPast = temp >= onTemp + p.fastC;
    const bool offPast = temp <= offTemp + p.epsC;
    if (!active_ && fastPast) { if (!fastOn_) { fastOn_ = true; fastSince_ = now; } } else { fastOn_ = false; }
    if (active_ && offPast) { if (!offOn_) { offOn_ = true; offSince_ = now; } } else { offOn_ = false; }
    const bool fastConfirmed = fastOn_ && static_cast<uint32_t>(now - fastSince_) >= p.confirmMs;
    const bool offConfirmed = offOn_ && static_cast<uint32_t>(now - offSince_) >= p.confirmMs;
    const bool rested = !haveNotBefore_ || static_cast<int32_t>(now - notBefore_) >= 0;
    if (!active_) {
      uint8_t startsLastHour = 0U;
      const uint8_t cap = p.maxStartsPerHour < MAX_STARTS ? p.maxStartsPerHour : MAX_STARTS;
      for (uint8_t i = 0U; i < cap; ++i)
        if (startUsed_[i] && static_cast<uint32_t>(now - startAt_[i]) < 3600000UL) ++startsLastHour;
      if ((trend || fastConfirmed) && rested && startsLastHour < cap) {
        active_ = true;
        notBefore_ = now + p.minOnMs;
        haveNotBefore_ = true;
        startAt_[startIdx_] = now;
        startUsed_[startIdx_] = true;
        startIdx_ = static_cast<uint8_t>((startIdx_ + 1U) % (cap ? cap : 1U));
        fastOn_ = false;
      }
    } else if (offConfirmed && rested) {
      active_ = false;
      notBefore_ = now + p.minOffMs;
      haveNotBefore_ = true;
      offOn_ = false;
    }
    return active_;
  }

  bool active() const { return active_; }

 private:
  bool active_ = false;
  uint32_t aboveSince_ = 0U, fastSince_ = 0U, offSince_ = 0U, notBefore_ = 0U;
  bool haveNotBefore_ = false;
  bool aboveOn_ = false, fastOn_ = false, offOn_ = false;
  float abovePv_ = 0.0f;
  uint32_t startAt_[MAX_STARTS] = {};
  bool startUsed_[MAX_STARTS] = {};
  uint8_t startIdx_ = 0U;
};
