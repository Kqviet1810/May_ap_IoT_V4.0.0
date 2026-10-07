#pragma once
// Adaptive Thermal V1 closed-loop simulation.
//
// The controller under test is the REAL production heating route (updateHeatingAndOutputs,
// extracted from machine_control.h), the real ThermalController / ThermalStartupController,
// the real adaptive supervisor, the real ThermalLearner, the real HeaterBurstScheduler and the
// real OutputArbiter. The plant below only closes the loop. The controller sees only what the
// firmware sees: the (filtered, quantized) sensor, the setpoint, the ACTUAL SSR state, the
// fan states, timing and faults. It never reads plant parameters.
//
// This is software evidence on an uncalibrated first-order model with transport delay. It is
// NOT physical qualification.
#include "../MAYAP_INDUSTRIAL_v1_0_0/adaptive_thermal_balance.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/adaptive_persistence.h"
#define MAYAP_TEST_ADAPTIVE 1
#define MAYAP_SENSOR_PROFILE 0
#define MAYAP_ADAPTIVE_OBSERVER_ONLY 0
#include "thermal-autotune-harness.h"
#include "actual-filter.inc"
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

namespace sim {

enum class Mode : uint8_t { Baseline = 0, LearnNoVent = 1, Adaptive = 2 };
inline const char *modeName(Mode m) {
  return m == Mode::Baseline ? "BASELINE_V4" : m == Mode::LearnNoVent ? "V1_NO_VENT_COORD" : "ADAPTIVE_V1";
}
enum class Reach : uint8_t { Reachable, HeatLimited, CoolingRequired, SafetyLimited };
inline const char *reachName(Reach r) {
  static const char *n[] = {"REACHABLE", "HEAT_LIMITED", "COOLING_REQUIRED", "SAFETY_LIMITED"};
  return n[static_cast<uint8_t>(r)];
}

struct Plant {
  double eff = 1.0;          // heater effectiveness vs the nominal 16 kW bank
  double capacity = 600000;  // J/degC
  double loss = 180;         // W/degC to ambient
  double dead = 15;          // s transport delay
  double lag = 8;            // s heater lag
  double ambient = 25;
  double ventG = 0;          // W/degC removed while the exhaust fan is fully on
  double resolution = 0.1;
  double bias = 0;
  double noise = 0;
  double khTrue() const { return 16000.0 * eff / capacity; }  // degC/s at 100 % duty
};

struct Scenario {
  float sp = 37.5f;
  Mode mode = Mode::Adaptive;
  double durationS = 10800;
  // ventilation owned by the real profile logic (ventAutoEnabled)
  bool ventProfile = false;
  unsigned ventCycleMin = 40;
  unsigned ventDutyPct = 10;
  uint32_t ventOffsetSec = 0;  // shifts the batch clock so vents land where we want them
  // mid-run hardware change
  double changeAtS = -1;
  double effScale = 1, ventScale = 1, lossScale = 1, capScale = 1;
  // disturbance / fault event
  std::string event = "none";
  double eventAtS = 5400;
  double eventValue = 2.0;       // door drop (degC)
  double ambientDelta = 0;       // applied at changeAtS
  double extraLossW = 0, extraLossUntilS = 0;  // legacy "vent" event: extra W/degC while t in [changeAtS, until)
  float spBefore = NAN;          // setpoint_step: SP before eventAtS
  uint32_t clockOffset = 0;  // millis() starts here (rollover tests)
  bool trace = false;
  std::ostream *traceOut = nullptr;
  std::ostream *debugOut = nullptr;  // per-sample controller internals (development)
  double debugFrom = 0, debugTo = 0;
  std::string label;
};

struct VentRecord { double start, end, minDev, postMax, recoverS, heaterOnS, windup; };

struct Result {
  Reach reach = Reach::Reachable;
  double overshoot = -1000, mae = 0, p95 = 0, ripple = 0, riseS = -1, energyJ = 0;
  int settling = -1;
  int high = 0, emergency = 0;
  bool pass = false;
  // learning
  int confidence = 0, state = 0, ventConf = 0;
  double khEstErrPct = NAN, delayErrS = NAN, holdErrPp = NAN, coastErrC = NAN, ventErrPct = NAN;
  double timeQualifiedS = -1, timeMismatchS = -1, minConfAfterChange = 100, maxPredErr = 0;
  double gainEst = NAN, delayEst = NAN, holdEst = NAN, coastEst = NAN, ventEst = NAN, predErr = NAN;
  uint32_t mismatchEvents = 0, outliers = 0, saves = 0;
  // vent
  std::vector<VentRecord> vents;
  double ventDevMax = 0, postVentOvershootMax = 0, ventRecoveryMax = 0, ventEnergyMean = 0, ventWindupMax = 0;
  double ventDevMean = 0, postVentOvershootMean = 0;
  int ventEvents = 0;
  // safety
  uint32_t unsafeHeatTicks = 0;
};

constexpr double HighC = 38.2, EmergencyC = 39.0;

inline Reach classify(const Plant &p, double sp) {
  if (p.ambient >= sp - 1.0) return Reach::CoolingRequired;
  const double need = p.loss * (sp - p.ambient) / (16000.0 * p.eff);  // steady duty at SP
  const double heatSec = p.capacity * (sp - p.ambient) / std::max(1.0, 16000.0 * p.eff - 0.5 * p.loss * (sp - p.ambient));
  if (need > 0.80 || heatSec > 5400.0) return Reach::HeatLimited;
  // Full-power coast so large that approach must be slow: overshoot target is controller-bound.
  if (p.khTrue() * (p.dead + p.lag) > 3.0) return Reach::SafetyLimited;
  return Reach::Reachable;
}

inline double holdTruthPct(const Plant &p, double sp) {
  return std::min(100.0, std::max(0.0, 100.0 * p.loss * (sp - p.ambient) / (16000.0 * p.eff)));
}
inline double coastTruthC(const Plant &p) { return p.khTrue() * (p.dead + p.lag); }
inline double ventTruth(const Plant &p, double sp) { return p.ventG * (sp - p.ambient) / p.capacity; }

inline void configure(TuneHarness &h, const Scenario &sc) {
  h.batchRunning_ = true;
  h.config_.targetTemp = sc.sp;
  h.config_.adaptiveThermalBalanceEnabled = sc.mode != Mode::Baseline;
  h.thermalV1_.setVentCoordination(sc.mode == Mode::Adaptive);
  h.config_.ventAutoEnabled = sc.ventProfile;
  h.config_.ventCycleMinutes = static_cast<uint8_t>(sc.ventCycleMin);
  h.config_.ventDutyDay1To3 = static_cast<uint8_t>(sc.ventDutyPct);
  h.config_.ventProfileLevel = 1;
  // The real firmware holds a sensor start-up grace window; without it the first 6 samples
  // (sensor not yet usable) would force the exhaust fan on for its 120 s minimum.
  h.sensorStartupGraceUntil_ = sc.clockOffset + 1000U + 60000U;
}

inline Result run(const Plant &base, const Scenario &sc) {
  Preferences::records().clear();
  MayapThermal::profileStorage.resetForTest();
  clockMs = 0;
  std::fill(levels, levels + 64, LOW);
  bootReady = true; trip = maintenance = false;
  const double sp = sc.sp;
  Plant p = base;
  std::unique_ptr<TuneHarness> hp(new TuneHarness());
  TuneHarness *h = hp.get();
  configure(*h, sc);
  ActualSensorFilter filter;
  Result r;
  r.reach = classify(base, sp);

  constexpr unsigned stepMs = 100;
  constexpr double dt = stepMs / 1000.0;
  const unsigned ticks = static_cast<unsigned>(sc.durationS * 1000 / stepMs);
  const unsigned lagTicks = static_cast<unsigned>(p.dead * 1000 / stepMs);
  std::vector<double> pipe(lagTicks + 1U, 0.0);
  size_t cursor = 0;
  double temp = p.ambient, heater = 0, ventAir = 0;
  double energy = 0;
  bool prevHigh = false, prevEmergency = false, ventPrev = false, reached = false;
  int lastOut = 0;
  std::vector<double> tail;
  double lo = 1000, hi = -1000, maeSum = 0;
  double heldFiltered = temp;
  unsigned samples = 0;
  std::vector<float> sec_temp, sec_integral;
  std::vector<uint8_t> sec_on;
  double duty60 = 0;
  unsigned onTicks60 = 0;
  bool changed = false;
  VentRecord vr{};
  bool ventOpen = false;
  double integralAtVent = 0, integralPeak = 0;
  std::vector<std::pair<double, double>> ventSpans;
  bool frozenActive = false;
  float frozenValue = 0;
  bool jumped = false;
  bool rebooted = false;
  unsigned sampleCounter = 0;

  for (unsigned tick = 0; tick < ticks; ++tick) {
    const double t = tick * dt;
    const uint32_t now = sc.clockOffset + 1000U + tick * stepMs;
    const double elapsedBefore = t;

    if (!changed && sc.changeAtS >= 0 && t >= sc.changeAtS) {
      changed = true;
      p.eff *= sc.effScale; p.ventG *= sc.ventScale; p.loss *= sc.lossScale; p.capacity *= sc.capScale;
      p.ambient += sc.ambientDelta;
    }
    if (std::isfinite(sc.spBefore)) h->config_.targetTemp = t >= sc.eventAtS ? sc.sp : sc.spBefore;
    // ---- scheduled disturbance / fault events -----------------------------------------------
    const double e0 = sc.eventAtS;
    const std::string &ev = sc.event;
    const bool lossSensor = ev == "sensor_loss" && t >= e0 && t < e0 + 40;
    const bool badSample = ev == "sensor_invalid" && t >= e0 && t < e0 + 10;
    const bool frozen = ev == "sensor_frozen" && t >= e0 && t < e0 + 60;
    const bool testMode = ev == "manual_test" && t >= e0 && t < e0 + 300;
    const bool heaterOff = ev == "heater_off" && t >= e0 && t < e0 + 300;
    const bool cut = (ev == "safety_cut" && t >= e0 && t < e0 + 90) || (ev == "power_recovery" && t >= e0 && t < e0 + 120);
    const bool powerLoss = ev == "power_loss" && t >= e0 && t < e0 + 180;
    const bool noisy = ev == "noisy";
    const bool jitter = ev == "jitter";
    const bool missing = ev == "missing_samples" && t >= e0 && t < e0 + 14;
    if (ev == "door" && tick == static_cast<unsigned>(e0 / dt)) temp -= sc.eventValue;
    if (ev == "reboot" && !rebooted && t >= e0) {
      rebooted = true;  // controller RAM is lost; the profile in NVS survives
      hp.reset(new TuneHarness());
      h = hp.get();
      configure(*h, sc);
      MayapThermal::profileStorage.resetForTest();
      h->heatRestartNotBefore_ = 0;
    }
    h->abnormalResetLatched_ = powerLoss;
    h->testModeActive_ = testMode;
    h->inputs_.in.heaterEnable = !heaterOff;
    h->batchElapsedSec_ = static_cast<uint32_t>(t) + sc.ventOffsetSec;

    // ---- sensor ---------------------------------------------------------------------------------
    bool sampleTick = (tick % 20U) == 0U;
    if (jitter) sampleTick = (tick % 20U) == (static_cast<unsigned>(t / 7.0) % 3U) && (static_cast<unsigned>(t / 2.0) % 11U) != 0U;
    if (missing) sampleTick = false;
    if (sampleTick) {
      ++sampleCounter;
      if (!lossSensor && !badSample && !frozen) {
        double noise = p.noise;
        double n = noisy ? 0.06 * std::sin(t * 0.73) + 0.03 * std::sin(t * 0.131) : 0.0;
        double sensed = temp + p.bias + n + (noise > 0 ? noise * std::sin(t * 1.7) : 0.0);
        if (ev == "sudden_jump" && !jumped && t >= e0) { sensed += 1.2; jumped = true; }
        const float sampled = static_cast<float>(std::round(sensed / p.resolution) * p.resolution);
        filter.updateFilter(sampled, 60);
        heldFiltered = filter.value();
        ++samples;
      }
      if (frozen && !frozenActive) { frozenActive = true; frozenValue = static_cast<float>(heldFiltered); }
      if (!frozen) frozenActive = false;
      h->sensorUsable_ = !lossSensor && !badSample && !frozen && samples >= 6;
    }
    if (lossSensor || badSample || frozen) h->sensorUsable_ = false;
    const double fed = frozenActive ? frozenValue : heldFiltered;
    h->highTemperatureActive_ = temp >= HighC;
    h->emergencyActive_ = temp >= EmergencyC;
    h->faults_.cooling = h->highTemperatureActive_ || h->emergencyActive_;
    h->faults_.inhibit = cut || h->highTemperatureActive_ || h->emergencyActive_;
    h->faults_.drop = h->highTemperatureActive_ || h->emergencyActive_;
    h->sample(static_cast<float>(temp), static_cast<float>(fed));
    h->cycle(now, sampleTick && h->sensorUsable_);
    if ((tick % 200U) == 0U) MayapThermal::profileStorage.service(now);
    if (sc.debugOut && sampleTick && t >= sc.debugFrom && t < sc.debugTo) {
      (*sc.debugOut) << static_cast<int>(t) << " pv=" << temp << " fed=" << fed << " err=" << (sp - fed) << " req=" << h->pidPower_
                     << " I=" << h->pid_.integral() << " ff=" << h->pid_.feedForwardApplied() << " cap=" << h->startupHeat_.lastCeiling()
                     << " peak=" << h->startupHeat_.predictedPeak() << " slope=" << h->startupHeat_.slope()
                     << " phase=" << static_cast<int>(h->startupHeat_.phase()) << " eff=" << h->adaptiveThermal_.decision().effective << '\n';
    }

    const auto st = h->outputs_.state();
    const bool on = st.heaterSsr && st.heatMaster;
    // ---- safety invariants: heat must be absent in every unsafe context ----------------------
    const bool unsafe = cut || lossSensor || badSample || frozen || powerLoss || heaterOff ||
                        h->highTemperatureActive_ || h->emergencyActive_ || testMode;
    if (unsafe && on) {
      ++r.unsafeHeatTicks;
      std::fprintf(stderr, "UNSAFE HEAT %s t=%.1f ev=%s mode=%s\n", sc.label.c_str(), t, ev.c_str(), modeName(sc.mode));
      std::abort();
    }
    // ---- plant ---------------------------------------------------------------------------------------
    const double delivered = on ? 16000.0 * p.eff : 0.0;
    const double delayed = pipe[cursor];
    pipe[cursor] = delivered;
    cursor = (cursor + 1U) % pipe.size();
    heater += (delayed - heater) * dt / std::max(0.1, p.lag);
    ventAir += ((st.ventFan ? 1.0 : 0.0) - ventAir) * dt / 8.0;
    const double ventW = p.ventG * ventAir * (temp - p.ambient);
    const double extraLoss = (sc.extraLossW > 0 && t >= sc.changeAtS && t < sc.extraLossUntilS) ? sc.extraLossW : 0.0;
    temp += (heater - (p.loss + extraLoss) * (temp - p.ambient) - ventW) * dt / p.capacity;
    energy += delivered * dt;
    if (!std::isfinite(temp) || !std::isfinite(h->runtime_.heaterPower) || h->runtime_.heaterPower < 0 ||
        h->runtime_.heaterPower > 100.0001) {
      std::fprintf(stderr, "NONFINITE/UNBOUNDED %s t=%.1f\n", sc.label.c_str(), t);
      std::abort();
    }
    // ---- metrics ----------------------------------------------------------------------------------------
    const bool ch = temp >= HighC, ce = temp >= EmergencyC;
    if (ch && !prevHigh) ++r.high;
    if (ce && !prevEmergency) ++r.emergency;
    prevHigh = ch; prevEmergency = ce;
    r.overshoot = std::max(r.overshoot, temp - sp);
    if (!reached && temp >= sp - 0.15) { reached = true; r.riseS = t; }
    if (std::fabs(temp - sp) > 0.15) lastOut = static_cast<int>(t);
    if (tick % 10U == 0U) {
      sec_temp.push_back(static_cast<float>(temp));
      sec_integral.push_back(h->pid_.integral());
      sec_on.push_back(0);
    }
    if (on && !sec_on.empty()) sec_on.back() = 1;
    if (t >= sc.durationS - 1800 && tick % 10U == 0U) {
      const double e = temp - sp;
      tail.push_back(std::fabs(e));
      maeSum += std::fabs(e);
      lo = std::min(lo, temp); hi = std::max(hi, temp);
    }
    // ---- vent events (ACTUAL fan output) ----------------------------------------------------------
    const bool ventNow = st.ventFan;
    if (ventNow && !ventPrev) {
      ventOpen = true; vr = VentRecord{}; vr.start = t; integralAtVent = h->pid_.integral(); integralPeak = integralAtVent;
    }
    if (ventOpen) integralPeak = std::max(integralPeak, static_cast<double>(h->pid_.integral()));
    if (!ventNow && ventPrev && ventOpen) {
      vr.end = t; vr.windup = integralPeak - integralAtVent; ventSpans.push_back({vr.start, vr.end});
      r.vents.push_back(vr); ventOpen = false;
    }
    ventPrev = ventNow;
    // ---- learning trace ------------------------------------------------------------------------------
    const auto &prof = h->thermalV1_.learner().profile();
    if (sc.mode != Mode::Baseline) {
      if (r.timeQualifiedS < 0 && prof.confidence >= 60) r.timeQualifiedS = t;
      if (changed && t >= sc.changeAtS) r.minConfAfterChange = std::min<double>(r.minConfAfterChange, prof.confidence);
      if (h->thermalV1_.learner().mismatch() && r.timeMismatchS < 0 && changed) r.timeMismatchS = t - sc.changeAtS;
      r.maxPredErr = std::max<double>(r.maxPredErr, h->thermalV1_.learner().predictionError());
    }
    if (on) onTicks60++;
    if (sc.trace && sc.traceOut && tick % 600U == 599U) {
      duty60 = onTicks60 / 600.0; onTicks60 = 0;
      const auto &L = h->thermalV1_.learner();
      (*sc.traceOut) << sc.label << ',' << modeName(sc.mode) << ',' << static_cast<int>(t) << ',' << temp << ',' << fed << ','
                     << sp << ',' << duty60 << ',' << st.ventFan << ',' << static_cast<int>(prof.confidence) << ','
                     << MayapThermal::learnStateName(L.state()) << ',' << prof.heaterGain << ',' << prof.heaterDelaySec << ','
                     << prof.holdPowerPct << ',' << prof.coastRiseC << ',' << prof.ventCoolingGain << ','
                     << L.predictionError() << ',' << L.mismatch() << ',' << MayapThermal::ventPhaseName(h->thermalV1_.plan().ventPhase)
                     << ',' << h->thermalV1_.plan().assist.feedForward << ',' << h->pid_.integral() << ','
                     << MayapThermal::gateReasonName(L.gate()) << ',' << h->runtime_.heaterPower << '\n';
    }
    (void)elapsedBefore;
  }
  // ---- final metrics ----------------------------------------------------------------------------------------
  if (!tail.empty()) {
    std::sort(tail.begin(), tail.end());
    r.mae = maeSum / tail.size();
    r.p95 = tail[static_cast<size_t>(0.95 * (tail.size() - 1))];
    r.ripple = hi - lo;
  }
  r.settling = lastOut < static_cast<int>(sc.durationS - 1800) ? lastOut + 1 : -1;
  r.energyJ = energy;
  r.pass = r.high == 0 && r.emergency == 0 && r.overshoot <= 0.3 && r.mae <= 0.1 && r.p95 <= 0.15 &&
           r.ripple <= 0.25 && r.settling >= 0;
  const auto &prof = hp->thermalV1_.learner().profile();
  r.confidence = prof.confidence; r.state = prof.state; r.ventConf = prof.ventConfidence;
  r.gainEst = prof.heaterGain; r.delayEst = prof.heaterDelaySec; r.holdEst = prof.holdPowerPct;
  r.coastEst = prof.coastRiseC; r.ventEst = prof.ventCoolingGain; r.predErr = hp->thermalV1_.learner().predictionError();
  r.mismatchEvents = 0; r.outliers = hp->thermalV1_.learner().outliers();
  r.saves = MayapThermal::profileStorage.saves();
  // truth is the plant AFTER any scheduled change
  Plant truth = base;
  if (sc.changeAtS >= 0 && sc.changeAtS < sc.durationS) {
    truth.eff *= sc.effScale; truth.ventG *= sc.ventScale; truth.loss *= sc.lossScale; truth.capacity *= sc.capScale;
    truth.ambient += sc.ambientDelta;
  }
  if (sc.mode != Mode::Baseline) {
    r.khEstErrPct = 100.0 * (prof.heaterGain - truth.khTrue()) / truth.khTrue();
    r.delayErrS = prof.heaterDelaySec - (truth.dead + truth.lag);
    r.holdErrPp = prof.holdPowerPct - holdTruthPct(truth, sp);
    r.coastErrC = prof.coastRiseC - coastTruthC(truth);
    if (truth.ventG > 0 && ventTruth(truth, sp) > 0) r.ventErrPct = 100.0 * (prof.ventCoolingGain - ventTruth(truth, sp)) / ventTruth(truth, sp);
  }
  // per-vent metrics from the 10 Hz-decimated record (1 s resolution)
  const double settleFrom = 3600;  // ignore the first hour (heat-up / learning)
  double devSum = 0, postSum = 0;
  for (auto &v : r.vents) {
    if (v.start < settleFrom) continue;
    const size_t s0 = static_cast<size_t>(v.start), s1 = std::min(sec_temp.size() - 1, static_cast<size_t>(v.end));
    double minDev = 0, postMax = -1000, onS = 0;
    for (size_t i = s0; i <= s1 && i < sec_temp.size(); ++i) { minDev = std::min(minDev, sec_temp[i] - sp); onS += sec_on[i]; }
    size_t pe = std::min(sec_temp.size() - 1, s1 + 900);
    for (size_t i = s1; i <= pe; ++i) { postMax = std::max(postMax, sec_temp[i] - sp); if (i > s1) onS += sec_on[i] * 0.0; }
    double rec = 900;
    for (size_t i = s1; i <= pe; ++i) {
      bool ok = true;
      for (size_t j = i; j <= std::min(pe, i + 60); ++j) if (std::fabs(sec_temp[j] - sp) > 0.15) { ok = false; break; }
      if (ok) { rec = static_cast<double>(i - s1); break; }
    }
    v.minDev = minDev; v.postMax = postMax; v.recoverS = rec; v.heaterOnS = onS;
    ++r.ventEvents;
    r.ventDevMax = std::max(r.ventDevMax, -minDev);
    r.postVentOvershootMax = std::max(r.postVentOvershootMax, postMax);
    r.ventRecoveryMax = std::max(r.ventRecoveryMax, rec);
    r.ventWindupMax = std::max(r.ventWindupMax, v.windup);
    devSum += -minDev; postSum += postMax; r.ventEnergyMean += onS;
  }
  if (r.ventEvents) { r.ventDevMean = devSum / r.ventEvents; r.postVentOvershootMean = postSum / r.ventEvents; r.ventEnergyMean /= r.ventEvents; }
  return r;
}

}  // namespace sim
