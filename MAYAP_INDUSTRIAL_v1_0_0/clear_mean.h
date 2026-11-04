#pragma once
#include <cmath>
#include <cstdint>

// The way out of High (E111) and Emergency (E112): decided on the MEAN of the last seconds of the reading, not on "every sample under the level".
// Pure C++ (no Arduino types): MachineController calls it every cycle and tests/clear-mean.cpp drives the very same code.
//
// Why: the sensor gives 0.1 degC steps, so an oven that stands at the set-point flickers between two steps. With the lines set close together (High 37.7, Emergency 37.8
// at SV 37.5, field photos of 04/11) the level to leave E112 is 37.5 = SV, and "every sample <= 37.5 for 30 s" is restarted by every 37.6 reading: the alarm stayed up with
// PV at 37.5 and the heater stayed blocked. The mean of a dithered reading is an unbiased estimate of the true temperature, so the mean at/under the level for the whole
// window is the same statement as "the oven is at/under the level" - and a sample far over the level still refuses it for the whole window (guardC).
//
// The way IN is not touched (a single reading at/over the line trips it).
class MeanClear {
 public:
  static constexpr uint8_t RING = 32U;

  void reset() { n_ = 0U; head_ = 0U; lastAt_ = 0U; }

  // The level to leave a line that trips at `trip`: `hysteresisC` under it, but never so low that a controller holding SV could not reach it
  // (SV + 0.05) and never closer than 0.1 to the line itself.
  static float level(float trip, float hysteresisC, float setpoint) {
    const float floorLevel = std::fmin(setpoint + 0.05f, trip - 0.1f);
    return std::fmax(trip - hysteresisC, floorLevel);
  }

  // One call per control cycle. A sample is taken once a second. True when the window is full, its mean is at/under `levelC` and no sample of it is more than
  // `guardC` over it (a spike keeps the alarm up until it has left the window). Any invalid reading starts the window again.
  bool update(uint32_t now, bool valid, float value, float levelC, uint32_t windowMs, float guardC) {
    if (!valid || !std::isfinite(value)) { reset(); return false; }
    if (n_ == 0U || static_cast<uint32_t>(now - lastAt_) >= 1000UL) {
      ring_[head_] = value;
      head_ = static_cast<uint8_t>((head_ + 1U) % RING);
      if (n_ < RING) ++n_;
      lastAt_ = now;
    }
    uint32_t need = windowMs / 1000UL;
    if (need < 1U) need = 1U;
    if (need > RING) need = RING;
    if (n_ < need) return false;
    float sum = 0.0f, top = ring_[(head_ + RING - 1U) % RING];
    for (uint32_t i = 0U; i < need; ++i) {
      const float v = ring_[(head_ + RING - 1U - i) % RING];
      sum += v;
      if (v > top) top = v;
    }
    return sum / static_cast<float>(need) <= levelC && top <= levelC + guardC;
  }

 private:
  float ring_[RING] = {};
  uint8_t n_ = 0U, head_ = 0U;
  uint32_t lastAt_ = 0U;
};

// The way IN to High (E111), on the same principle: the sensor gives 0.1 degC steps, so a temperature a little under the line flickers onto it for a second now and then, and a
// trip on "one raw reading at/over the line for 1 s" (2.9.1) reported E111 with the screen showing 37.6 and the line set at 37.7. It trips when the MEAN of the last `windowMs` is
// at/over `line - toleranceC` (the true temperature is at the line) with no sample more than `guardC` under it, or at once when a reading is `immediateC` or more over the line.
class MeanTrip {
 public:
  static constexpr uint8_t RING = 16U;
  void reset() { n_ = 0U; head_ = 0U; lastAt_ = 0U; }
  bool update(uint32_t now, bool valid, float value, float line, uint32_t windowMs, float toleranceC, float guardC, float immediateC) {
    if (!valid || !std::isfinite(value)) { reset(); return false; }
    if (value >= line + immediateC) return true;
    if (n_ == 0U || static_cast<uint32_t>(now - lastAt_) >= 1000UL) {
      ring_[head_] = value;
      head_ = static_cast<uint8_t>((head_ + 1U) % RING);
      if (n_ < RING) ++n_;
      lastAt_ = now;
    }
    uint32_t need = windowMs / 1000UL;
    if (need < 1U) need = 1U;
    if (need > RING) need = RING;
    if (n_ < need) return false;
    float sum = 0.0f, low = ring_[(head_ + RING - 1U) % RING];
    for (uint32_t i = 0U; i < need; ++i) {
      const float v = ring_[(head_ + RING - 1U - i) % RING];
      sum += v;
      if (v < low) low = v;
    }
    return sum / static_cast<float>(need) >= line - toleranceC && low >= line - toleranceC - guardC;
  }
 private:
  float ring_[RING] = {};
  uint8_t n_ = 0U, head_ = 0U;
  uint32_t lastAt_ = 0U;
};
