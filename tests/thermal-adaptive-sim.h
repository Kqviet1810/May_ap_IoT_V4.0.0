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
#include "thermal-sensor-path.h"
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

namespace sim {

enum class Mode : uint8_t { Baseline = 0, LearnNoVent = 1, Adaptive = 2, Smart = 3 };
inline const char *modeName(Mode m) {
  return m == Mode::Baseline ? "BASELINE_V4" : m == Mode::LearnNoVent ? "V1_NO_VENT_COORD" : m == Mode::Smart ? "SMART_THERMAL" : "ADAPTIVE_V1";
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
  double metricsFromS = 3600;  // vent events before this are warm-up and not scored
  bool trace = false;
  std::ostream *traceOut = nullptr;
  std::ostream *debugOut = nullptr;  // per-sample controller internals (development)
  double debugFrom = 0, debugTo = 0;
  std::string label;
  // Smart AutoTune qualification: run the post-tune closed loop with the ACCEPTED gains and profile seed.
  bool gainsSet = false;
  float kp = 18.0f, ki = 0.8f, kd = 45.0f;
  bool seedSet = false;
  MayapThermal::ThermalProfile seed{};
  // Sensor-path integrity (opt-in): High/Emergency/E115/E104 derived from the REPORTED temperature.
  SensorPath path;
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
  int ventEvents = 0, ventDropEvents = 0;
  // safety / learning integrity
  uint32_t unsafeHeatTicks = 0;
  uint32_t faultUpdates = 0;       // learner regression updates while a fault/disturbance window was open
  double timeReconvergeS = -1;     // after a hardware change: first time conf>=60 and gain error <= 25 %
  double minHintAfterChange = 1.0; // lowest startup-hint strength seen after the change (authority)
  double minFfAfterChange = 1e9, maxFfAfterChange = 0;
  double confAfterReboot = -1;
  uint32_t overshootGuards = 0;
  uint32_t mismatchKh = 0, mismatchHold = 0;
  // sensor-path report (only filled when Scenario::path.enabled)
  double truePeak = -1000, trueHighS = 0, trueEmergencyS = 0, onWhileTrueHighS = 0, sensorErrMax = 0;
  double firstTrueHighS = -1, firstFwHighS = -1, firstFwEmergencyS = -1, e115S = -1, e104S = -1, sensorLostS = -1;
  int fwHigh = 0, fwEmergency = 0;
  uint32_t unsafeCommandTicks = 0;
  double heaterOnAfterFaultS = 0;
  // long-run report: |PV - SP| per 6 h block (mean, max)
  std::vector<double> blockMae, blockMax;
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

inline void configure(PathHarness &h, const Scenario &sc) {
  h.config_.tempOffset = sc.path.tempOffset;
  h.batchRunning_ = true;
  h.config_.targetTemp = sc.sp;
  h.thermalV1_.setSmartLearning(sc.mode == Mode::Smart);
  h.startupHeat_.setSmart(sc.mode == Mode::Smart);   // Smart Thermal startup (feature flag); every other mode is the legacy controller
  h.config_.adaptiveThermalBalanceEnabled = sc.mode != Mode::Baseline;
  h.thermalV1_.setVentCoordination(sc.mode == Mode::Adaptive || sc.mode == Mode::Smart);
  h.config_.ventAutoEnabled = sc.ventProfile;
  h.config_.ventCycleMinutes = static_cast<uint8_t>(sc.ventCycleMin);
  h.config_.ventDutyDay1To3 = static_cast<uint8_t>(sc.ventDutyPct);
  h.config_.ventProfileLevel = 1;
  // The real firmware holds a sensor start-up grace window; without it the first 6 samples
  // (sensor not yet usable) would force the exhaust fan on for its 120 s minimum.
  h.sensorStartupGraceUntil_ = sc.clockOffset + 1000U + 60000U;
  if (sc.gainsSet) { h.config_.kp = sc.kp; h.config_.ki = sc.ki; h.config_.kd = sc.kd; }
  if (sc.seedSet) h.thermalV1_.learner().seed(sc.seed, 75, sc.seed.holdPowerPct > 0.5f);
}

inline Result run(const Plant &base, const Scenario &sc) {
  Preferences::records().clear();
  MayapThermal::profileStorage.resetForTest();
  clockMs = 0;
  std::fill(levels, levels + 64, LOW);
  bootReady = true; trip = maintenance = false;
  const double sp = sc.sp;
  Plant p = base;
  std::unique_ptr<PathHarness> hp(new PathHarness());
  PathHarness *h = hp.get();
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
  double temp = p.ambient, heater = 0, ventAir = 0, sensPhys = p.ambient;
  bool prevFwHigh = false, prevFwEmergency = false;
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
  uint32_t lastUpdates = 0;
  bool frozenActive = false;
  float frozenValue = 0;
  bool jumped = false;
  bool rebooted = false;
  unsigned sampleCounter = 0;
  std::vector<double> blkSum, blkMax; std::vector<unsigned> blkN;
  double pathFrozenValue = 0;
  bool pathSpiked = false;

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
      hp.reset(new PathHarness());
      h = hp.get();
      configure(*h, sc);
      MayapThermal::profileStorage.resetForTest();
      h->heatRestartNotBefore_ = 0;
      lastUpdates = 0;
    }
    h->abnormalResetLatched_ = powerLoss;
    h->testModeActive_ = testMode;
    h->inputs_.in.heaterEnable = !heaterOff;
    h->batchElapsedSec_ = static_cast<uint32_t>(t) + sc.ventOffsetSec;

    // ---- sensor ---------------------------------------------------------------------------------
    bool sampleTick = (tick % 20U) == 0U;
    if (jitter) sampleTick = (tick % 20U) == (static_cast<unsigned>(t / 7.0) % 3U) && (static_cast<unsigned>(t / 2.0) % 11U) != 0U;
    if (missing) sampleTick = false;
    double fed = 0;
    bool controlSample = false;
    if (!sc.path.enabled) {
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
    fed = frozenActive ? frozenValue : heldFiltered;
    h->highTemperatureActive_ = temp >= HighC;
    h->emergencyActive_ = temp >= EmergencyC;
    h->faults_.cooling = h->highTemperatureActive_ || h->emergencyActive_;
    h->faults_.inhibit = cut || h->highTemperatureActive_ || h->emergencyActive_;
    h->faults_.drop = h->highTemperatureActive_ || h->emergencyActive_;
    h->sample(static_cast<float>(temp), static_cast<float>(fed));
    controlSample = sampleTick && h->sensorUsable_;
    } else {
      // ---- sensor-path mode: the controller and the alarms see ONLY what the probe reports ----------------
      const SensorPath &sp_ = sc.path;
      if (sp_.lagS <= 0.0) sensPhys = temp; else sensPhys += (temp - sensPhys) * dt / std::max(0.05, sp_.lagS);
      const bool faultOn = t >= sp_.faultAtS && t < sp_.faultAtS + sp_.faultDurationS;
      if (sp_.fault == SensorFault::Disconnect) h->pathDataValid_ = !faultOn;
      else h->pathDataValid_ = true;
      h->latestFrameValid_ = h->pathDataValid_;
      bool accepted = false;
      if (sampleTick && h->pathDataValid_) {
        double sensed = sensPhys + p.bias + (p.noise > 0 ? p.noise * std::sin(t * 1.7) : 0.0);
        if (faultOn) {
          switch (sp_.fault) {
            case SensorFault::StuckLow: case SensorFault::StuckHigh: sensed = sp_.faultValue; break;
            case SensorFault::Frozen: sensed = pathFrozenValue; break;
            case SensorFault::Spike: if (!pathSpiked) { sensed += sp_.faultValue; pathSpiked = true; } break;
            case SensorFault::Drift: sensed -= sp_.faultValue * (t - sp_.faultAtS) / 3600.0; break;
            default: break;
          }
        } else pathFrozenValue = std::round(sensed / p.resolution) * p.resolution;
        const float reported = static_cast<float>(std::round(sensed / p.resolution) * p.resolution);
        filter.updateFilter(reported, 60);
        const float filtered = filter.value();
        h->rawTemperature_ = std::max(reported, reported + sp_.tempOffset);  // offset can only RAISE the safety value
        h->newSensorSample_ = false;
        h->pathAccept(now, filtered + sp_.tempOffset, true);
        accepted = h->newSensorSample_;
        r.sensorErrMax = std::max(r.sensorErrMax, std::fabs(static_cast<double>(reported) - temp));
      }
      h->pathUsable();
      h->pathAlarms(now);
      h->pathEvidence(now);
      h->pathApplyFaults(false);
      controlSample = accepted;
      fed = h->temperature_;
    }
    h->cycle(now, controlSample);
    if ((tick % 200U) == 0U) MayapThermal::profileStorage.service(now);
    if (sc.debugOut && sampleTick && t >= sc.debugFrom && t < sc.debugTo) {
      const auto st0 = h->outputs_.state();
      (*sc.debugOut) << static_cast<int>(t) << " pv=" << temp << " fed=" << fed << " err=" << (sp - fed) << " req=" << h->pidPower_
                     << " I=" << h->pid_.integral() << " ff=" << h->pid_.feedForwardApplied() << " cap=" << h->startupHeat_.lastCeiling()
                     << " peak=" << h->startupHeat_.predictedPeak() << " slope=" << h->startupHeat_.slope()
                     << " khObs=" << h->startupHeat_.observedGain() << " phase=" << static_cast<int>(h->startupHeat_.phase()) << " eff=" << h->adaptiveThermal_.decision().effective
                     << " vent=" << st0.ventFan << ' ' << MayapThermal::ventPhaseName(h->thermalV1_.plan().ventPhase) << " vff=" << h->thermalV1_.plan().ventFF
                     << " hff=" << h->thermalV1_.plan().holdFF << " ventPct=" << h->thermalV1_.plan().hint.ventPct
                     << " ventConf=" << static_cast<int>(h->thermalV1_.learner().profile().ventConfidence)
                     << " kv=" << h->thermalV1_.learner().profile().ventCoolingGain << '\n';
    }

    const auto st = h->outputs_.state();
    const bool on = st.heaterSsr && st.heatMaster;
    // ---- safety invariants: heat must be absent in every unsafe context ----------------------
    const bool unsafe = sc.path.enabled
        ? (h->faults_.inhibit || h->faults_.drop || powerLoss || heaterOff || testMode)
        : (cut || lossSensor || badSample || frozen || powerLoss || heaterOff ||
           h->highTemperatureActive_ || h->emergencyActive_ || testMode);
    if (unsafe && on) {
      ++r.unsafeHeatTicks;
      if (sc.path.enabled) ++r.unsafeCommandTicks;   // reported, not aborted: this is what the sensor-path suite measures
      else {
        std::fprintf(stderr, "UNSAFE HEAT %s t=%.1f ev=%s mode=%s\n", sc.label.c_str(), t, ev.c_str(), modeName(sc.mode));
        std::abort();
      }
    }
    // ---- plant ---------------------------------------------------------------------------------------
    // Actuator faults (sensor-path mode only): the SSR conducts although commanded OFF; the master contactor is the
    // independent series element. The firmware has no feedback for either, so `on` (the command) stays what it believes.
    bool actualOn = on;
    if (sc.path.enabled && t >= sc.path.faultAtS) {
      if (sc.path.fault == SensorFault::SsrStuckOn) actualOn = st.heatMaster;
      else if (sc.path.fault == SensorFault::SsrContactorStuck) actualOn = true;
    }
    const double delivered = actualOn ? 16000.0 * p.eff : 0.0;
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
    if (sc.path.enabled) {
      r.truePeak = std::max(r.truePeak, temp);
      if (ch) { r.trueHighS += dt; if (r.firstTrueHighS < 0) r.firstTrueHighS = t; if (actualOn) r.onWhileTrueHighS += dt; }
      if (ce) r.trueEmergencyS += dt;
      if (h->highTemperatureActive_ && !prevFwHigh) { ++r.fwHigh; if (r.firstFwHighS < 0) r.firstFwHighS = t; }
      if (h->emergencyActive_ && !prevFwEmergency) { ++r.fwEmergency; if (r.firstFwEmergencyS < 0) r.firstFwEmergencyS = t; }
      prevFwHigh = h->highTemperatureActive_; prevFwEmergency = h->emergencyActive_;
      if (h->heaterNotHeatingActive_ && r.e115S < 0) r.e115S = t;
      if (h->sensorFrozenLatched_ && r.e104S < 0) r.e104S = t;
      if (!h->sensorUsable_ && h->pathDataValid_ == false && r.sensorLostS < 0) r.sensorLostS = t;
      if (actualOn && t >= sc.path.faultAtS && sc.path.fault != SensorFault::None) r.heaterOnAfterFaultS += dt;
    }
    r.overshoot = std::max(r.overshoot, temp - sp);
    if (!reached && temp >= sp - 0.15) { reached = true; r.riseS = t; }
    if (std::fabs(temp - sp) > 0.15) lastOut = static_cast<int>(t);
    if (tick % 10U == 0U) {
      sec_temp.push_back(static_cast<float>(temp));
      sec_integral.push_back(h->pid_.integral());
      sec_on.push_back(0);
    }
    if (on && !sec_on.empty()) sec_on.back() = 1;
    if (tick % 10U == 0U) {
      const size_t b = static_cast<size_t>(t / 21600.0);
      if (b >= blkSum.size()) { blkSum.resize(b + 1, 0.0); blkN.resize(b + 1, 0U); blkMax.resize(b + 1, 0.0); }
      const double e = std::fabs(temp - sp);
      blkSum[b] += e; ++blkN[b]; blkMax[b] = std::max(blkMax[b], e);
    }
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
    // ---- learning integrity / hardware-change tracking ------------------------------------------
    {
      const bool faultWin = (lossSensor || badSample || frozen || testMode || heaterOff || cut || powerLoss || missing) ||
                            (ev != "none" && t >= e0 && t < e0 + 100 &&
                             (ev == "sensor_loss" || ev == "sensor_invalid" || ev == "sensor_frozen" || ev == "manual_test" ||
                              ev == "heater_off" || ev == "safety_cut" || ev == "power_loss" || ev == "missing_samples" ||
                              ev == "power_recovery"));
      const uint32_t u = h->thermalV1_.learner().updates();
      if (faultWin && u > lastUpdates) r.faultUpdates += (u - lastUpdates);
      lastUpdates = u;
      if (changed && sc.mode != Mode::Baseline) {
        const auto &pf = h->thermalV1_.learner().profile();
        const double truthKh = base.khTrue() * sc.effScale / sc.capScale;
        if (r.timeReconvergeS < 0 && t > sc.changeAtS + 60 && pf.confidence >= 60 &&
            std::fabs(pf.heaterGain - truthKh) / truthKh <= 0.25) r.timeReconvergeS = t - sc.changeAtS;
        r.minHintAfterChange = std::min<double>(r.minHintAfterChange, h->thermalV1_.plan().hint.valid ? h->thermalV1_.plan().hint.strength : 0.0);
        r.minFfAfterChange = std::min<double>(r.minFfAfterChange, (h->thermalV1_.plan().assist.feedForward + h->thermalV1_.plan().assist.addForward));
        r.maxFfAfterChange = std::max<double>(r.maxFfAfterChange, (h->thermalV1_.plan().assist.feedForward + h->thermalV1_.plan().assist.addForward));
      }
      if (rebooted && r.confAfterReboot < 0 && t >= sc.eventAtS + 120) r.confAfterReboot = h->thermalV1_.learner().profile().confidence;
    }
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
                     << ',' << (h->thermalV1_.plan().assist.feedForward + h->thermalV1_.plan().assist.addForward) << ',' << h->pid_.integral() << ','
                     << MayapThermal::gateReasonName(L.gate()) << ',' << h->runtime_.heaterPower << ',' << L.holdWindows() << ',' << L.infoSlow() << '\n';
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
  for (size_t b = 0; b < blkSum.size(); ++b) { r.blockMae.push_back(blkN[b] ? blkSum[b] / blkN[b] : 0.0); r.blockMax.push_back(blkMax[b]); }
  r.energyJ = energy;
  r.pass = r.high == 0 && r.emergency == 0 && r.overshoot <= 0.3 && r.mae <= 0.1 && r.p95 <= 0.15 &&
           r.ripple <= 0.25 && r.settling >= 0;
  const auto &prof = hp->thermalV1_.learner().profile();
  r.confidence = prof.confidence; r.state = prof.state; r.ventConf = prof.ventConfidence;
  r.gainEst = prof.heaterGain; r.delayEst = prof.heaterDelaySec; r.holdEst = prof.holdPowerPct;
  r.coastEst = prof.coastRiseC; r.ventEst = prof.ventCoolingGain; r.predErr = hp->thermalV1_.learner().predictionError();
  r.mismatchEvents = hp->thermalV1_.learner().mismatchEvents(); r.outliers = hp->thermalV1_.learner().outliers();
  r.overshootGuards = hp->thermalV1_.overshootEvents();
  r.mismatchKh = hp->thermalV1_.learner().mismatchBy(1); r.mismatchHold = hp->thermalV1_.learner().mismatchBy(2);
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
  // per-vent metrics from the 1 s record. The DROP is measured against PV just before the vent
  // and only for events that start near SP (a vent during heat-up has no "deviation" to score).
  const double settleFrom = sc.metricsFromS;  // vent events before this are warm-up
  double dropSum = 0, postSum = 0;
  int dropEvents = 0;
  for (auto &v : r.vents) {
    if (v.start < settleFrom) continue;
    const size_t s0 = static_cast<size_t>(v.start), s1 = std::min(sec_temp.size() - 1, static_cast<size_t>(v.end));
    if (s0 >= sec_temp.size()) continue;
    const double tStart = sec_temp[s0];
    const bool nearSp = std::fabs(tStart - sp) <= 0.5;
    double minT = tStart, onS = 0;
    for (size_t i = s0; i <= s1 && i < sec_temp.size(); ++i) { minT = std::min<double>(minT, sec_temp[i]); onS += sec_on[i]; }
    const size_t pe = std::min(sec_temp.size() - 1, s1 + 900);
    double postMax = -1000;
    for (size_t i = s1; i <= pe; ++i) { postMax = std::max<double>(postMax, sec_temp[i] - sp); }
    for (size_t i = s1 + 1; i <= std::min(sec_temp.size() - 1, s1 + 300); ++i) onS += sec_on[i];
    double rec = 900;
    for (size_t i = s1; i <= pe; ++i) {
      bool ok = true;
      for (size_t j = i; j <= std::min(pe, i + 60); ++j) if (std::fabs(sec_temp[j] - sp) > 0.15) { ok = false; break; }
      if (ok) { rec = static_cast<double>(i - s1); break; }
    }
    v.minDev = tStart - minT; v.postMax = postMax; v.recoverS = rec; v.heaterOnS = onS;
    ++r.ventEvents;
    r.postVentOvershootMax = std::max(r.postVentOvershootMax, postMax);
    r.ventRecoveryMax = std::max(r.ventRecoveryMax, rec);
    r.ventWindupMax = std::max(r.ventWindupMax, v.windup);
    postSum += postMax; r.ventEnergyMean += onS;
    if (nearSp) { ++dropEvents; dropSum += v.minDev; r.ventDevMax = std::max(r.ventDevMax, v.minDev); }
  }
  r.ventDropEvents = dropEvents;
  if (r.ventEvents) { r.postVentOvershootMean = postSum / r.ventEvents; r.ventEnergyMean /= r.ventEvents; }
  if (dropEvents) r.ventDevMean = dropSum / dropEvents;
  return r;
}

}  // namespace sim
