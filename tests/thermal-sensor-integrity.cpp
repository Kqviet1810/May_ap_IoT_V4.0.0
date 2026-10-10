// Sensor-integrity study of the heating route (Phase 1 of the Smart Thermal programme). Simulation only.
//
// Why this exists. tests/thermal-adaptive-sim.h sets highTemperatureActive_/emergencyActive_ from the PLANT'S TRUE temperature and
// passes the true temperature as the raw sample, so every "0 High/Emergency" result there assumes a protection layer that can see the
// truth. The firmware sees only its sensor. This harness closes that gap WITHOUT touching the existing oracle:
//
//   plant truth -> sensor (resolution, bias, lag, FAULT) -> production IIR filter -> controller
//   safety trip : the REAL High/Emergency block of MachineController::updateAlarms (sliced), fed rawTemperature_/temperature_
//                 as the sensor path reports them
//   heater-not-heating (E115): the REAL energy-accounting block (sliced)
//   frozen sensor: the production rule (20 min unchanged AND >= max(60 s, heaterStuckDuration/2) of heater ON time)
//
// For each sensor fault it reports what the PLANT really did (true peak, time above High/Emergency, heater energy delivered while the
// plant was above Emergency) next to what the firmware knew and when. Nothing here asserts that a case is safe: its job is to measure
// how far software alone gets, so the hardware requirement (an independent limiter) is evidence-based.
#include "thermal-adaptive-sim.h"
#include <fstream>
#include <iomanip>

using namespace sim;

#include "actual-alarm-constants.inc"
#include "actual-condition-timer.inc"

namespace {

struct SafetyCore {  // production names, so the sliced block compiles unmodified
  struct { float emergencyTemp = 39.0f, highTempAlarm = 38.2f; bool highTempAlarmWithoutBatch = true; } config_;
  struct { bool running() const { return false; } } autotune_;
  bool batchRunning_ = true, testModeActive_ = false, safetySampleValid_ = true;
  bool emergencyActive_ = false, highTemperatureActive_ = false;
  float rawTemperature_ = NAN, temperature_ = NAN;
  ConditionTimer highTripTimer_{}, highClearTimer_{}, emergencyClearTimer_{};
  void update(uint32_t now) {
#include "actual-alarm-trip.inc"
    (void)wasHigh; (void)wasEmergency;
  }
};

struct EvidenceCore {  // production names for the E115 energy accounting block
  struct { float targetTemp = 37.5f, tempHysteresis = 0.2f, heaterStuckMinRiseC = 0.3f; uint16_t heaterStuckDurationSec = 900; } config_;
  struct Input { bool heaterEnable = true; };
  struct Inputs { Input input; const Input &state() const { return input; } } inputs_;
  OutputArbiter *arbiter = nullptr;
  bool batchRunning_ = true, sensorUsable_ = true, heaterStuckTracking_ = false, heaterNotHeatingActive_ = false;
  float temperature_ = 30, heaterStuckStartTemp_ = NAN;
  uint32_t heaterStuckSinceAt_ = 0, heaterStuckAccumOnMs_ = 0;
  void update(uint32_t now) {
    struct { OutputArbiter *a; const decltype(OutputArbiter().state()) state() const { return a->state(); } } outputs_{arbiter};
#include "actual-heater-evidence.inc"
  }
};

enum class Fault : uint8_t { None, StuckLow, StuckAtReading, OffsetLow, DriftLow, StuckHigh };
const char *faultName(Fault f) {
  static const char *n[] = {"NONE", "STUCK_LOW_30C", "STUCK_AT_LAST_READING", "OFFSET_LOW_3C", "DRIFT_LOW_1C_PER_MIN", "STUCK_HIGH_45C"};
  return n[static_cast<uint8_t>(f)];
}

struct Out {
  double peakTrue = 0, tHighTrue = -1, tEmergencyTrue = -1, secAboveEmergency = 0, secAboveHigh = 0;
  double energyAboveEmergencyKJ = 0;       // heater energy delivered while the PLANT was above Emergency
  double tFirmwareSafe = -1;               // first moment the firmware cut heat for a safety reason
  const char *firstCut = "NONE";
  double firmwareMaxSeen = 0;              // highest value the firmware ever saw
  bool trueOverEmergencyBeforeCut = false;
};

Out simulate(const Plant &base, Fault fault, double faultAtS, double sp, double durationS) {
  Preferences::records().clear();
  MayapThermal::profileStorage.resetForTest();
  clockMs = 0;
  std::fill(levels, levels + 64, LOW);
  bootReady = true; trip = maintenance = false;
  Plant p = base;
  std::unique_ptr<TuneHarness> hp(new TuneHarness());
  TuneHarness *h = hp.get();
  Scenario sc; sc.sp = static_cast<float>(sp); sc.mode = Mode::Adaptive;
  configure(*h, sc);
  ActualSensorFilter filter;
  SafetyCore safety;
  EvidenceCore evidence;
  evidence.arbiter = &h->outputs_;
  evidence.config_.targetTemp = static_cast<float>(sp);
  Out out;
  constexpr unsigned stepMs = 100;
  constexpr double dt = stepMs / 1000.0;
  const unsigned ticks = static_cast<unsigned>(durationS * 1000 / stepMs);
  const unsigned lagTicks = static_cast<unsigned>(p.dead * 1000 / stepMs);
  std::vector<double> pipe(lagTicks + 1U, 0.0);
  size_t cursor = 0;
  double temp = p.ambient, heater = 0, heldFiltered = temp, lastReported = temp, frozenValue = 0;
  bool faultActive = false;
  unsigned samples = 0;
  uint32_t frozenSince = 0;
  float frozenRef = NAN;
  safety.rawTemperature_ = static_cast<float>(temp);
  safety.temperature_ = static_cast<float>(temp);
  for (unsigned tick = 0; tick < ticks; ++tick) {
    const double t = tick * dt;
    const uint32_t now = 1000U + tick * stepMs;
    const bool sampleTick = (tick % 20U) == 0U;
    if (sampleTick) {
      double sensed = temp + p.bias;
      if (fault != Fault::None && t >= faultAtS) {
        if (!faultActive) { faultActive = true; frozenValue = lastReported; }
        switch (fault) {
          case Fault::StuckLow: sensed = 30.0; break;
          case Fault::StuckAtReading: sensed = frozenValue; break;
          case Fault::OffsetLow: sensed -= 3.0; break;
          case Fault::DriftLow: sensed -= std::min(8.0, (t - faultAtS) / 60.0); break;
          case Fault::StuckHigh: sensed = 45.0; break;
          case Fault::None: break;
        }
      }
      const float sampled = static_cast<float>(std::round(sensed / p.resolution) * p.resolution);
      lastReported = sampled;
      filter.updateFilter(sampled, 60);
      heldFiltered = filter.value();
      ++samples;
      safety.rawTemperature_ = sampled;
      safety.temperature_ = static_cast<float>(heldFiltered);
      h->sensorUsable_ = samples >= 6;
      if (!std::isfinite(frozenRef) || std::fabs(sampled - frozenRef) > SENSOR_FROZEN_EPSILON_C) { frozenRef = sampled; frozenSince = now; }
    }
    const double fed = heldFiltered;
    // ---- what the FIRMWARE decides, on the sensor path only ------------------------------------------------------------
    safety.update(now);
    evidence.sensorUsable_ = h->sensorUsable_;
    evidence.temperature_ = static_cast<float>(fed);
    evidence.update(now);
    const uint32_t frozenEvidenceOnMs = std::max<uint32_t>(60000UL, (static_cast<uint32_t>(evidence.config_.heaterStuckDurationSec) * 1000UL) / 2UL);
    const bool frozenFault = h->sensorUsable_ && std::isfinite(frozenRef) &&
        static_cast<uint32_t>(now - frozenSince) >= SENSOR_FROZEN_TIMEOUT_MS && evidence.heaterStuckAccumOnMs_ >= frozenEvidenceOnMs;
    h->highTemperatureActive_ = safety.highTemperatureActive_;
    h->emergencyActive_ = safety.emergencyActive_;
    const bool stop = h->highTemperatureActive_ || h->emergencyActive_ || evidence.heaterNotHeatingActive_ || frozenFault;
    h->faults_.cooling = stop;
    h->faults_.inhibit = stop;
    h->faults_.drop = stop;
    if (stop && out.tFirmwareSafe < 0) {
      out.tFirmwareSafe = t;
      out.firstCut = h->emergencyActive_ ? "EMERGENCY_TRIP" : h->highTemperatureActive_ ? "HIGH_TRIP"
                   : evidence.heaterNotHeatingActive_ ? "E115_HEATER_NOT_HEATING" : "SENSOR_FROZEN";
    }
    h->sample(safety.rawTemperature_, static_cast<float>(fed));
    h->cycle(now, (tick % 20U) == 0U && h->sensorUsable_);
    out.firmwareMaxSeen = std::max<double>(out.firmwareMaxSeen, safety.rawTemperature_);
    const auto st = h->outputs_.state();
    const bool on = st.heaterSsr && st.heatMaster;
    // ---- plant ------------------------------------------------------------------------------------------------------------
    const double delivered = on ? 16000.0 * p.eff : 0.0;
    const double delayed = pipe[cursor];
    pipe[cursor] = delivered;
    cursor = (cursor + 1U) % pipe.size();
    heater += (delayed - heater) * dt / std::max(0.1, p.lag);
    temp += (heater - p.loss * (temp - p.ambient)) * dt / p.capacity;
    out.peakTrue = std::max(out.peakTrue, temp);
    if (temp >= HighC) { out.secAboveHigh += dt; if (out.tHighTrue < 0) out.tHighTrue = t; }
    if (temp >= EmergencyC) {
      out.secAboveEmergency += dt;
      if (out.tEmergencyTrue < 0) { out.tEmergencyTrue = t; out.trueOverEmergencyBeforeCut = out.tFirmwareSafe < 0; }
      out.energyAboveEmergencyKJ += delivered * dt / 1000.0;
    }
  }
  return out;
}

}  // namespace

int main(int argc, char **argv) {
  const char *csv = argc > 1 ? argv[1] : "sensor-integrity.csv";
  std::ofstream o(csv);
  o << "plant,eff,capacity_kj_per_c,loss,dead,lag,fault,fault_at_s,sp,true_peak_c,t_true_high_s,t_true_emergency_s,s_above_emergency,"
       "kj_delivered_above_emergency,firmware_first_cut,t_firmware_cut_s,firmware_max_seen_c,true_over_emergency_before_cut\n";
  struct P { const char *name; double eff, cap, loss, dead, lag; };
  const P plants[] = {
      {"light_1x", 1.0, 180000, 180, 15, 8},   {"medium_1x", 1.0, 600000, 180, 15, 8},   {"heavy_1x", 1.0, 1600000, 180, 15, 8},
      {"light_half", 0.5, 180000, 180, 15, 8}, {"medium_half", 0.5, 600000, 180, 15, 8}, {"heavy_half", 0.5, 1600000, 180, 15, 8},
      {"light_2x", 2.0, 180000, 180, 15, 8},   {"medium_2x", 2.0, 600000, 180, 15, 8},   {"heavy_2x", 2.0, 1600000, 180, 15, 8},
  };
  const Fault faults[] = {Fault::None, Fault::StuckLow, Fault::StuckAtReading, Fault::OffsetLow, Fault::DriftLow, Fault::StuckHigh};
  const double faultAt[] = {600.0, 5400.0};   // during warm-up and at steady state
  unsigned rows = 0;
  for (const P &pl : plants) for (Fault f : faults) for (double at : faultAt) {
    if (f == Fault::None && at != faultAt[0]) continue;
    Plant p; p.eff = pl.eff; p.capacity = pl.cap; p.loss = pl.loss; p.dead = pl.dead; p.lag = pl.lag; p.ambient = 25;
    const Out r = simulate(p, f, at, 37.5, 5400.0 + 5400.0);
    o << pl.name << ',' << pl.eff << ',' << pl.cap / 1000.0 << ',' << pl.loss << ',' << pl.dead << ',' << pl.lag << ',' << faultName(f) << ','
      << (f == Fault::None ? -1.0 : at) << ',' << 37.5 << ',' << std::fixed << std::setprecision(2) << r.peakTrue << ',' << r.tHighTrue << ','
      << r.tEmergencyTrue << ',' << r.secAboveEmergency << ',' << r.energyAboveEmergencyKJ << ',' << r.firstCut << ',' << r.tFirmwareSafe << ','
      << r.firmwareMaxSeen << ',' << (r.trueOverEmergencyBeforeCut ? 1 : 0) << std::defaultfloat << '\n';
    ++rows;
  }
  std::printf("sensor-integrity: %u scenarios written to %s\n", rows, csv);
  return 0;
}
