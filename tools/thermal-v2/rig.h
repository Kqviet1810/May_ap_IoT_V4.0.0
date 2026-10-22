#pragma once
// Thermal V2 digital-twin rig (G1).  SIMULATION ONLY: nothing here is evidence about a real oven (lesson L6).
//
// What is REAL production text (compiled, not re-implemented; see make_includes.py):
//   updateHeatingAndOutputs() with its permits, vent/fan logic, HeaterBurstScheduler, OutputArbiter,
//   the sensor filter, sensor acceptance/usable, High/Emergency alarm, E115 accounting, ConditionTimer, MachineConfig.
// What is host code: the plant, the sensor chain (probe lag, quantisation, offset, faults), the scenario script, the metrics, and the
// controller object plugged into the single slot `rigSlot()` (the production line `float commandedPower = 0.0f;`).
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <deque>
#include <memory>
#include <string>
#include <vector>
using std::isfinite;

static uint32_t clockMs = 0;
inline uint32_t millis() { return clockMs; }
inline uint32_t elapsedMs(uint32_t now, uint32_t then) { return now - then; }
inline bool timeReached(uint32_t now, uint32_t then) { return static_cast<int32_t>(now - then) >= 0; }
inline int constrain(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
inline float clampFloat(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
constexpr float PI = 3.14159265358979323846f;
#include "actual-config.inc"
#include "../../MAYAP_INDUSTRIAL_v1_0_0/thermal_v2.h"
#include "../../MAYAP_INDUSTRIAL_v1_0_0/heater_burst_scheduler.h"
#include "../../MAYAP_INDUSTRIAL_v1_0_0/startup_output_policy.h"
constexpr int LOW = 0, HIGH = 1, OUTPUT = 2;
static int levels[64]{};
static bool bootReady = true, trip = false, maintenance = false;
inline bool mayapBootOperationsReady() { return bootReady; }
inline bool mayapSystemTripLatched() { return trip; }
inline bool mayapFirmwareMaintenanceActive() { return maintenance; }
inline void digitalWrite(uint8_t pin, int v) { assert(pin < 64); levels[pin] = v; }
inline void pinMode(uint8_t pin, int mode) { assert(pin < 64 && mode == OUTPUT && levels[pin] == LOW); }
template <class T, unsigned N> class FixedRing {
 public:
  void push(const T &v) { if (q.size() == N) { q.pop_front(); ++overflow; } q.push_back(v); }
  bool pop(T &v) { if (q.empty()) return false; v = q.front(); q.pop_front(); return true; }
  uint32_t overflowCount() const { return overflow; }
 private:
  std::deque<T> q; uint32_t overflow = 0;
};
#include "actual-output.inc"
#include "actual-heating-constants.inc"
#include "actual-sensor-constants.inc"
#include "actual-condition-timer.inc"
#include "actual-filter.inc"

constexpr uint8_t HEATER_GROUP_COUNT = 1;
enum class TurnPhase { Idle, MovingLeft, MovingRight };

namespace v2 {
static double g_truthDemandPct = 0;

// ------------------------------------------------------------------------------------------------------------------------------------------
// Plant: two thermal nodes (air+steel Cf, eggs Ce) coupled by hA, loss UA to ambient, transport delay td and first-order lag tl on the heater
// path, optional third node (C1), supply sag (C7), heater gain error (C4/C5), fan-dependent coupling (C6), embryo heat, vent loss.
// ------------------------------------------------------------------------------------------------------------------------------------
struct Plant {
  std::string id;
  double Pmax = 16000, Cf = 500000, Ce = 1900000, hA = 1300, UA = 250, td = 100, tl = 40, Tamb = 28;
  double ventW = 2500;          // W/ (full fan) removed through the vent at (Ta-Tamb)=12.5 C  => ventG = ventW/12.5
  double fanW = 0;              // circulation motor heat into the air (W)
  // structural modifiers (set C)
  double thirdC = 0, thirdG = 0;                 // C1
  double sensorLagS = 0;                         // C2/C3 (extra, besides the production filter)
  double gainScale = 1.0;                        // C4/C5 (controller is not told)
  bool fanCoupling = false;                      // C6
  double sagPerDuty = 0, sagStepAtS = -1, sagStep = 0;   // C7
  double sensorOffset = 0;                       // C8 (calibration offset, applied like config_.tempOffset)
  double stepC = 0.1;                            // sensor quantisation
};

struct Scenario {
  std::string id;
  double SP = 37.5, startAir = 33, startEgg = 33, duration = 6 * 3600;
  bool warm = false;                  // start settled: air=egg=SP, scored from eventAt
  double eventAt = 5400;
  double door = 0;                    // S4: air step (negative)
  double trayFrac = 0, trayTemp = 28; // S5: fraction of egg mass replaced by cold eggs
  double ventAt = -1, ventFor = 300, ventKnown = 0;       // S6
  double embryoMax = 0, ambientTo = NAN;                  // S7 (ramped over the whole run)
  double spStep = 0;                                      // S8
  double permitLossAt = -1, permitLossFor = 600;          // S9
  double sagPerDuty = 0;                                  // S7 supply sag
  bool batchRunning = true;
  bool coldStart = false;                                // event scenarios run after a real cold start (identification climb), not from a settled state
};

// ------------------------------------------------------------------------------------------------------------------------------------------
// Controller plugged into the slot.  `delivered` is the REAL energy the arbiter gave in the last step (not the command).
// ------------------------------------------------------------------------------------------------------------------------------------------
struct CtlIn {
  uint32_t now = 0;
  float pv = NAN, sp = 37.5f;
  bool newSample = false;
  uint32_t sampleSeq = 0;
  bool permit = false, fanStable = false, fanVent = false, ventHold = false;
  float capPct = 100, headroomC = 0.7f, alarmHeadroomC = 0.7f, deliveredPct = 0, e115Frac = 0;
  double dt = 0.1;
};
struct Controller {
  virtual ~Controller() {}
  virtual const char *name() const = 0;
  virtual float step(const CtlIn &in) = 0;
};
struct Fastest : Controller {          // 100 % whenever the production route allows: the physical reference
  const char *name() const override { return "fastest"; }
  float step(const CtlIn &) override { return 100.0f; }
};
struct Zero : Controller {
  const char *name() const override { return "zero"; }
  float step(const CtlIn &) override { return 0.0f; }
};
struct HoldSteady : Controller {       // constant percent (self-tests)
  float pct;
  explicit HoldSteady(float p) : pct(p) {}
  const char *name() const override { return "const"; }
  float step(const CtlIn &) override { return pct; }
};
struct V2Ctl : Controller {            // the V2 law; the cap / NaN / negative clamp below is what the production adapter does
  thermalv2::Controller c;
  thermalv2::Output last;
  const char *name() const override { return "v2"; }
  float step(const CtlIn &in) override {
    thermalv2::Input i;
    i.pv = in.pv; i.sp = in.sp; i.dQ = std::getenv("V2_DQ") ? static_cast<float>(std::atof(std::getenv("V2_DQ"))) : 0.1f; i.headroomC = in.headroomC; i.alarmHeadroomC = in.alarmHeadroomC; i.capPct = in.capPct; i.deliveredPct = in.deliveredPct;
    i.e115Frac = in.e115Frac; i.dtS = static_cast<float>(in.dt); i.newSample = in.newSample; i.permit = in.permit;
    last = c.step(i);
    static const bool dbg = std::getenv("V2DBG") != nullptr;
    if (dbg && (in.now % 30000U) < 100U)
      std::fprintf(stderr, "t=%6.0f pv=%.2f pred=%.2f u=%.1f k=%.2e th=%.0f dloss=%.1f I=%.1f Te=%.2f st=%d dnow=%.1f truth=%.1f td=%.0f tl=%.0f te=%.0f ha=%.1f sg=%.2f\n", (in.now - 1000) / 1000.0, in.pv, c.predicted(), last.powerPct, c.kAir(), c.thetaHat(), c.dLoss(), c.integral(), c.teModel(), last.state, c.dNow(), g_truthDemandPct, c.tdHat(), c.tauHat(), c.tauEggHat(), c.haPct(), c.sigmaPred());
    float u = last.powerPct;
    if (!(u > 0.0f)) u = 0.0f;
    return std::min(u, in.capPct);
  }
};
struct BangBang : Controller {         // tiny hysteresis controller, only to exercise the rig end to end
  bool on = true;
  const char *name() const override { return "bangbang"; }
  float step(const CtlIn &in) override {
    if (isfinite(in.pv)) { if (in.pv > in.sp - 0.2f) on = false; else if (in.pv < in.sp - 0.8f) on = true; }
    return on ? 100.0f : 0.0f;
  }
};

// ------------------------------------------------------------------------------------------------------------------------------------------
// Firmware rig
// ------------------------------------------------------------------------------------------------------------------------------------------
struct InputState { bool light = false, autoMode = true, circulationFan = true, heaterEnable = true; };
struct FakeInputs { InputState in; const InputState &state() const { return in; } };
struct FakeRtc { uint32_t e = 1800000000U; bool valid() const { return true; } uint32_t epoch() const { return e; } };
struct FakeFaults {
  bool drop = false, inhibit = false, cooling = false;
  bool masterDropRequired() const { return drop; } bool ssrInhibited() const { return inhibit; }
  bool circulationForced() const { return cooling; } bool ventForced() const { return cooling; }
};

struct Rig {
  FakeInputs inputs_; FakeRtc rtc_; FakeFaults faults_; MachineConfig config_; OutputArbiter outputs_;
#include "actual-burst-member.inc"
  struct { float heaterPower = 0; } runtime_;
  ConditionTimer highTripTimer_{}, highClearTimer_{}, emergencyClearTimer_{};
  bool testModeActive_ = false, lightWebOverrideActive_ = false, lightWebOverrideRefInput_ = false, lightWebOverrideValue_ = false;
  bool resumeConfirmationRequired_ = false, resumePending_ = false, batchRunning_ = true, prevBatchRunningForOutputs_ = false;
  bool sensorUsable_ = false, humidityLowActive_ = false, previousFanCommand_ = false;
  bool emergencyActive_ = false, highTemperatureActive_ = false, ventTemperatureActive_ = false;
  bool batchClearPending_ = false, safetyJournalFaultLatched_ = false, storageFaultLatched_ = false, storageDegraded_ = false, abnormalResetLatched_ = false;
  bool newSensorSample_ = false, batchOverdueSirenActive_ = false, sirenSelfTestActive_ = false;
  bool warmupVentHold_ = false, warmupVentChecked_ = false; uint32_t warmupVentHoldSince_ = 0;
  uint32_t circFanStaggerUntil_ = 0, postCoolUntil_ = 0, sensorStartupGraceUntil_ = 0, fanOnSince_ = 0, heatRestartNotBefore_ = 0, sirenMutedUntil_ = 0;
  float temperature_ = 25, rawTemperature_ = 25, humidity_ = 60;
  TurnPhase turnPhase_ = TurnPhase::Idle;
  uint32_t batchElapsedSec_ = 0;
  uint32_t elapsedBatchSec(uint32_t) const { return batchElapsedSec_; }
  void updateTestModeOutputs(uint32_t now) { outputs_.forceSafe(now); }
  // sensor path state (production names)
  bool safetySampleValid_ = false, latestFrameValid_ = false, pathDataValid_ = false, sensorSuspect_ = false;
  uint8_t sensorPlausibilityStreak_ = 0, goodSensorStreak_ = 0;
  float lastSuspectCandidate_ = NAN, lastAcceptedTemperature_ = NAN, sensorFrozenRefTemp_ = NAN;
  uint32_t sensorFrozenSince_ = 0;
  bool heaterStuckTracking_ = false, heaterNotHeatingActive_ = false;
  uint32_t heaterStuckSinceAt_ = 0, heaterStuckAccumOnMs_ = 0;
  float heaterStuckStartTemp_ = NAN;
  // controller slot
  Controller *ctl_ = nullptr; CtlIn last_{}; uint32_t sampleSeq_ = 0; float deliveredPct_ = 0; double dt_ = 0.1;
  float lastCommand_ = 0, lastUnclamped_ = 0; bool lastPermit_ = false;

  float rigSlot(uint32_t now, bool normalSsrPermit, bool actuatorReady, bool fanStable, const OutputRequest &req) {
    CtlIn in;
    in.now = now; in.pv = temperature_; in.sp = config_.targetTemp; in.newSample = newSensorSample_;
    if (newSensorSample_) ++sampleSeq_;
    in.sampleSeq = sampleSeq_; in.permit = normalSsrPermit && actuatorReady; in.fanStable = fanStable;
    in.fanVent = req.ventFan; in.ventHold = warmupVentHold_; in.capPct = config_.maxHeaterPower;
    in.headroomC = std::min(config_.highTempAlarm, config_.ventOnTemp) - std::max(0.0f, -config_.tempOffset) - config_.targetTemp;
    in.alarmHeadroomC = config_.emergencyTemp - std::max(0.0f, -config_.tempOffset) - config_.targetTemp;
    in.deliveredPct = deliveredPct_;
    in.e115Frac = static_cast<float>(heaterStuckAccumOnMs_) / (static_cast<float>(config_.heaterStuckDurationSec) * 1000.0f);
    in.dt = dt_;
    last_ = in; lastPermit_ = in.permit;
    // No clamp here on purpose: the cap / NaN / negative handling is the job of the production adapter (G2 puts it in machine_control.h).
    const float u = ctl_ ? ctl_->step(in) : 0.0f;
    lastUnclamped_ = u; lastCommand_ = u; return u;
  }
#include "actual-heating.inc"
  void pathAccept(uint32_t now, float candidateTemp, bool frameValid) {
#include "actual-sensor-accept.inc"
  }
  void pathUsable(uint32_t now) {
    const bool wasUsable = sensorUsable_;
#include "actual-sensor-usable.inc"
    if (wasUsable != sensorUsable_) {      // production processSensor(): checked verbatim by make_includes.py
      heatRestartNotBefore_ = now + HEAT_RESTART_LOCKOUT_MS;
      if (!sensorUsable_) postCoolUntil_ = now + POST_COOL_MS;
    }
  }
  void pathAlarms(uint32_t now) {
#include "actual-safety-alarm.inc"
#include "actual-vent-temp.inc"
#include "actual-alarm-edges.inc"
  }
  void pathEvidence(uint32_t now) {
#include "actual-heater-evidence.inc"
  }
  void applyFaults() {
    const bool stop = !sensorUsable_ || heaterNotHeatingActive_ || highTemperatureActive_ || emergencyActive_;
    faults_.inhibit = stop; faults_.drop = stop; faults_.cooling = highTemperatureActive_ || emergencyActive_;
  }
  Rig() {
    outputs_.begin();
    config_.maxHeaterPower = 100;
    bootReady = true; trip = false; maintenance = false;
    std::memset(levels, 0, sizeof(levels));
  }
};

}  // namespace v2
