#pragma once
// The hold controller with the load ledger (thermal_load_ledger.h) beside it, as MachineController runs them: the ledger is fed once a second with the hold estimate while the
// oven is settled; when the controller starts live it asks the ledger for advice and takes it as its starting hold level (blended with the 12 % prior by the confidence).
// The ledger lives in the EEPROM on the machine, so a restart keeps it (carryOver) while the clock keeps counting through the stop (skipClock).
#include <cmath>
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_load_ledger.h"
struct AdvisedCtl {
  static constexpr bool kFineInput = true;
  thermalv3::Controller c;
  thermalload::Ledger led;
  double clockS = 100000.0, acc = 0.0;
  bool asked = false;
  double liveS = 0.0;
#ifndef ADOPT_AFTER
#define ADOPT_AFTER 400.0
#endif
  static constexpr double ADOPT_AFTER_S = ADOPT_AFTER;
  bool useAdvice = true;
  float lastAdviceHold = -1.0f, lastConfidence = 0.0f;
  thermalv3::Output step(const thermalv3::Input &in) {
    thermalv3::Output o = c.step(in);
    clockS += in.dtS;
    if (in.permit && in.pv > 0.0f) liveS += in.dtS;
    // The advice is taken once the power history is full (the observer reads the power of one transit time ago: with an empty history it reads zero power and a hold level it
    // was just given looks too high to it - on a slow oven that starts a swing of 0.8 degC that lasts for hours).
    if (!asked && in.permit && liveS >= ADOPT_AFTER_S) {
      asked = true;
      const thermalload::Advice a = led.advise(static_cast<uint32_t>(clockS), in.sp);
      if (useAdvice && a.valid && std::fabs(in.sp - in.pv) < 0.5f) {
        const float start = thermalload::Ledger::blend(c.pHold(), a);
        c.adoptHold(start);
        lastAdviceHold = a.holdPct; lastConfidence = a.confidence;
      }
    }
    acc += in.dtS;
    if (acc >= 1.0) {
      acc -= 1.0;
      led.feed(static_cast<uint32_t>(clockS), c.settled() && !in.ventFan && !in.shadow, c.pHold(), in.sp);
    }
    return o;
  }
  float pHold() const { return c.pHold(); }
  float kAir() const { return c.kAir(); }
  float tauTransit() const { return c.tauTransit(); }
  void restore(float k, float t) { c.restore(k, t); }
  void skipClock(double s) { clockS += s; }
  static void carryOver(AdvisedCtl &fresh, const AdvisedCtl &old) { fresh.led = old.led; fresh.clockS = old.clockS; fresh.useAdvice = old.useAdvice; }
};
