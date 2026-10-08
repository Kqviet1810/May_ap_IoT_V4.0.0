#pragma once
// Smart AutoTune closed-loop harness: one plant, the production AutoTune route, optional fault / power-cut / save-failure injection.
// SOFTWARE / SIMULATION ONLY (see thermal-adaptive-sim.h for the plant and honesty notes).
#ifdef LEGACY_ENGINE
#define MAYAP_AUTOTUNE_LEGACY 1
#endif
#include "thermal-adaptive-sim.h"

namespace smarttune {
using namespace sim;

struct Case { Plant p; float sp = 37.5f; std::string label; };

struct TuneOut {
  bool accepted = false, started = false, modelDone = false, candidate = false, validated = false;
  AutoTuneReason reason = AutoTuneReason::None, rejection = AutoTuneReason::None;
  AutoTunePhase lastPhase = AutoTunePhase::Idle;
  double tuneS = 0, peak = 0;
  bool high = false, emergency = false;
  MachineConfig cfg;
  MayapThermal::ThermalProfile seed{};
  float gain = 0, delay = 0, coast100 = 0, hold = 0, ku = 0, pu = 0, kp = 0, ki = 0, kd = 0;
  uint8_t confidence = 0;
  float evOver = 0, evMae = 0, evP95 = 0, evRipple = 0;
  unsigned saves = 0;
  uint32_t profileSaves = 0;
  bool oldKept = true;
};

// A disturbance injected into the tune (immediate-abort qualification).
enum class Fault { None, SensorLost, SensorInvalid, SensorFrozen, AutoOff, HeaterOff, Batch, Inhibit, Trip, Maintenance, Cancel, HighTemp, StorageFault };

struct TuneOpts {
  Fault fault = Fault::None;
  double faultAtS = 0;          // seconds after the tune started
  double maxS = 17000;
  int saveBudget = -1;          // >= 0: every EEPROM write is truncated to this many bytes (power cut / write failure)
  double rebootAtS = -1;        // >= 0: controller RAM is lost here; the new boot must see the OLD config/profile, heater off, tune idle
  double phaseFirstS[16];
  bool rebootChecked = false;
  TuneOpts() { for (double &v : phaseFirstS) v = -1; }
};

static TuneOut tune(const Plant &base, float sp, TuneOpts &opt) {
  const Fault fault = opt.fault;
  const double faultAtS = opt.faultAtS;
  const double maxS = opt.maxS;
  TuneOut out;
  Preferences::records().clear();
  MayapThermal::profileStorage.resetForTest();
  clockMs = 0;
  std::fill(levels, levels + 64, LOW);
  bootReady = true; trip = maintenance = false;
  std::unique_ptr<TuneHarness> hp(new TuneHarness());
  TuneHarness *h = hp.get();
  h->config_.targetTemp = sp;
  h->config_.adaptiveThermalBalanceEnabled = true;
  h->sensorStartupGraceUntil_ = 1000U + 60000U;
  const MachineConfig before = h->config_;
  MachineConfig saved; MachineConfig back;
  bool ok = h->store_.saveConfig(h->config_, back); (void)ok; h->store_.saves = 0;
  h->store_.writeBudget = opt.saveBudget;
  Plant p = base;
  ActualSensorFilter filter;
  constexpr unsigned stepMs = 100;
  constexpr double dt = stepMs / 1000.0;
  const unsigned lagTicks = static_cast<unsigned>(p.dead * 1000 / stepMs);
  std::vector<double> pipe(lagTicks + 1U, 0.0);
  size_t cursor = 0;
  double temp = std::getenv("START_TEMP") ? std::atof(std::getenv("START_TEMP")) : p.ambient, heater = 0, heldFiltered = temp;
  unsigned samples = 0, sampleCounter = 0;
  bool started = false, frozenActive = false; float frozenValue = 0;
  const unsigned total = static_cast<unsigned>(maxS * 1000 / stepMs) + 1500U;
  unsigned startTick = 0;
  uint32_t lastModel = 0, lastValid = 0;
  (void)lastModel; (void)lastValid;
  for (unsigned tick = 0; tick < total; ++tick) {
    const uint32_t now = 1000U + tick * stepMs;
    const bool sampleTick = (tick % 20U) == 0U;
    const double since = started ? (tick - startTick) * dt : -1.0;
    const bool fa = fault != Fault::None && started && since >= faultAtS;
    const bool lost = fa && fault == Fault::SensorLost, invalid = fa && fault == Fault::SensorInvalid, frozen = fa && fault == Fault::SensorFrozen;
    if (sampleTick) {
      ++sampleCounter;
      if (!lost && !invalid && !frozen) {
        const float sampled = static_cast<float>(std::round((temp + p.bias) / p.resolution) * p.resolution);
        filter.updateFilter(sampled, 60);
        heldFiltered = filter.value();
        ++samples;
      }
      if (frozen && !frozenActive) { frozenActive = true; frozenValue = static_cast<float>(heldFiltered); }
      if (!frozen) frozenActive = false;
      h->sensorUsable_ = !lost && !invalid && !frozen && samples >= 6;
    }
    if (lost || invalid || frozen) h->sensorUsable_ = false;
    const double fed = frozenActive ? frozenValue : heldFiltered;
    h->highTemperatureActive_ = temp >= HighC || (fa && fault == Fault::HighTemp);
    h->emergencyActive_ = temp >= EmergencyC;
    h->faults_.cooling = h->highTemperatureActive_ || h->emergencyActive_;
    h->faults_.inhibit = (fa && fault == Fault::Inhibit) || h->highTemperatureActive_ || h->emergencyActive_;
    h->faults_.drop = h->highTemperatureActive_ || h->emergencyActive_;
    h->inputs_.in.autoMode = !(fa && fault == Fault::AutoOff);
    h->inputs_.in.heaterEnable = !(fa && fault == Fault::HeaterOff);
    h->batchRunning_ = fa && fault == Fault::Batch;
    trip = fa && fault == Fault::Trip;
    maintenance = fa && fault == Fault::Maintenance;
    h->storageFaultLatched_ = fa && fault == Fault::StorageFault;
    h->sample(static_cast<float>(temp), static_cast<float>(fed));
    if (!started && tick == 200U) {   // 20 s of sampling first, like a powered-up controller
      const char *message = nullptr;
      started = h->startAutoTune(now, message);
      out.started = started;
      if (!started) { out.reason = AutoTuneReason::None; out.rejection = AutoTuneReason::NoHeadroom; break; }
      startTick = tick;
    }
    if (fa && fault == Fault::Cancel && h->autotune_.running()) {
      const char *message = nullptr;
      h->cancelAutoTune(now, message);
    }
    h->cycle(now, sampleTick && h->sensorUsable_);
    const auto st = h->outputs_.state();
    const bool on = st.heaterSsr && st.heatMaster;
    const bool unsafe = (fa && (fault != Fault::None) && fault != Fault::Cancel) || h->highTemperatureActive_ || h->emergencyActive_;
    if (unsafe && on && fault != Fault::None && fault != Fault::SensorFrozen) {
      // After the cut cycle the heater must be OFF; one control cycle of grace for the cut itself.
      if (since >= faultAtS + 0.35) { std::fprintf(stderr, "UNSAFE HEAT during %d at %.1f\n", static_cast<int>(fault), since); std::abort(); }
    }
    const double delivered = on ? 16000.0 * p.eff : 0.0;
    const double delayed = pipe[cursor];
    pipe[cursor] = delivered;
    cursor = (cursor + 1U) % pipe.size();
    heater += (delayed - heater) * dt / std::max(0.1, p.lag);
    temp += (heater - p.loss * (temp - p.ambient)) * dt / p.capacity;
    if (!std::isfinite(temp)) { std::fprintf(stderr, "NONFINITE\n"); std::abort(); }
    if (started && std::getenv("TUNE_DEBUG") && (tick % 300U) == 0U && h->autotune_.phase() == AutoTunePhase::Validating)
      std::fprintf(stderr, "  t=%.0f pv=%.3f fed=%.3f pid=%.2f I=%.2f heater=%.1f ssr=%d master=%d cap=%.1f phase=%d peak=%.3f slope=%.4f\n", since, temp, fed, h->pidPower_,
                   h->pid_.integral(), h->runtime_.heaterPower, st.heaterSsr, st.heatMaster, h->startupHeat_.lastCeiling(), static_cast<int>(h->startupHeat_.phase()), h->startupHeat_.predictedPeak(), h->startupHeat_.slope());
    out.peak = std::max(out.peak, temp);
    out.high = out.high || temp >= HighC;
    out.emergency = out.emergency || temp >= EmergencyC;
    if (started) {
      out.lastPhase = h->autotune_.phase();
      { const unsigned pi = static_cast<unsigned>(out.lastPhase); if (pi < 16U && opt.phaseFirstS[pi] < 0) opt.phaseFirstS[pi] = (tick - startTick) * dt; }
      if (opt.rebootAtS >= 0 && !opt.rebootChecked && since >= opt.rebootAtS) {
        // Power cut: RAM is gone, the EEPROM image survives. The next boot starts idle with the OLD, valid configuration.
        opt.rebootChecked = true;
        TuneStore image = h->store_;
        hp.reset(new TuneHarness());
        h = hp.get();
        h->store_ = image;
        MachineConfig again;
        const bool loadedAgain = h->store_.loadConfig(again);
        out.oldKept = loadedAgain && again.kp == before.kp && again.ki == before.ki && again.kd == before.kd && !h->autotune_.running() &&
                      !h->outputs_.state().heaterSsr;
        out.accepted = false;
        out.lastPhase = AutoTunePhase::Idle;
        return out;
      }
#ifndef LEGACY_ENGINE
      if (h->autotune_.modelSerial() != lastModel) { lastModel = h->autotune_.modelSerial(); out.modelDone = true; }
      if (h->autotune_.validationSerial() != lastValid) { lastValid = h->autotune_.validationSerial(); out.candidate = true; out.validated = true; }
#endif
      if ((tick % 200U) == 0U) MayapThermal::profileStorage.service(now);
      if (!h->autotune_.running()) {
        out.tuneS = (tick - startTick) * dt;
        // let the post-tune handling (save, cool-down) run for a few seconds
        for (unsigned n = 0; n < 100; ++n) { h->cycle(now + (n + 1U) * stepMs, false); }
        MayapThermal::profileStorage.service(now + 20000U);
        MayapThermal::profileStorage.service(now + 40000U);
        break;
      }
    }
  }
  if (started && h->autotune_.running()) { out.tuneS = maxS; out.reason = AutoTuneReason::TotalTimeout; }
  else if (started) { out.reason = h->autotune_.reason(); out.rejection = h->autotune_.rejection(); }
  out.accepted = started && h->autotune_.state() == AutoTuneState::Success;
  out.saves = h->store_.saves;
  out.profileSaves = MayapThermal::profileStorage.saves();
  MachineConfig stored;
  const bool loaded = h->store_.loadConfig(stored);
  if (out.accepted) {
    out.cfg = h->config_;
    out.kp = h->config_.kp; out.ki = h->config_.ki; out.kd = h->config_.kd;
    out.oldKept = loaded && stored.kp == h->config_.kp && stored.ki == h->config_.ki && stored.kd == h->config_.kd;
    out.seed = h->thermalV1_.learner().profile();
  } else {
    // Rollback contract: nothing was written, config and profile are exactly what they were.
    // A SAVE_FAILED tune attempted one (truncated) write; every other rejection must not have touched the store at all.
    out.oldKept = (out.saves == 0 || h->autotune_.reason() == AutoTuneReason::SaveFailed) && h->config_.kp == before.kp && h->config_.ki == before.ki && h->config_.kd == before.kd &&
                  (!loaded || (stored.kp == before.kp && stored.ki == before.ki && stored.kd == before.kd)) &&
                  out.profileSaves == 0 && h->thermalV1_.learner().profile().confidence == 0;
    if (started && !out.oldKept) { std::fprintf(stderr, "ROLLBACK VIOLATION saves=%u psaves=%u\n", out.saves, out.profileSaves); std::abort(); }
  }
#ifndef LEGACY_ENGINE
  const AutoTuneModelView m = h->autotune_.model();
  out.gain = m.gain; out.delay = m.delaySec; out.coast100 = m.coast100C; out.hold = m.holdPct;
  out.ku = m.ku; out.pu = m.periodSec; out.confidence = m.confidence;
  out.evOver = h->autotune_.eval().overshoot; out.evMae = h->autotune_.eval().mae;
  out.evP95 = h->autotune_.eval().p95; out.evRipple = h->autotune_.eval().ripple;
#else
  out.ku = h->autotune_.result().ku; out.pu = h->autotune_.result().periodSec;
#endif
  return out;
}

}  // namespace smarttune
