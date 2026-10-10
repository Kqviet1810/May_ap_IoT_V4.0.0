// Smart Thermal phase 1: sensor-path integrity suite.
//
// Same closed loop as thermal-adaptive-v1.cpp (real heating route, real arbiter), but High/Emergency/E115/E104 and the
// controller's inhibit come from the temperature the SENSOR REPORTS (probe lag, quantisation, injected faults), while
// the plant TRUE temperature is only an observer. For every case three truths are reported separately:
//   plant true      truePeak, trueHighS, trueEmergencyS, onWhileTrueHighS
//   firmware saw    fwHigh/fwEmergency (+latency), e115S, e104S, sensorLostS
//   command         unsafeCommandTicks (SSR ON while the firmware's own inhibit/drop was asserted)
// Nothing here changes a threshold. A PASS of this suite is NEVER a statement that the sensor fault was survived:
// see "verdict" -- UNDETECTED_TRUE_VIOLATION means only an independent hardware protection can stop that fault.
#include "thermal-adaptive-sim.h"
#include <cstring>
#include <fstream>
#include <iomanip>
using namespace sim;

struct PlantDef { const char *name; double cap, dead; };
static const PlantDef PLANTS[] = {{"light_d15", 180000, 15}, {"medium_d15", 600000, 15}, {"medium_d60", 600000, 60}, {"heavy_d30", 1600000, 30}};

struct Case { std::string label; Plant p; Scenario sc; };

static Plant makePlant(const PlantDef &d) {
  Plant p; p.eff = 1.0; p.capacity = d.cap; p.loss = 180; p.dead = d.dead; p.lag = 8; p.ambient = 20; p.resolution = 0.1;
  return p;
}

static std::vector<Case> cases(bool smart) {
  std::vector<Case> out;
  auto add = [&](const PlantDef &d, const std::string &tag, SensorFault f, double lagS, double value, double dur, float offset) {
    Case c; c.p = makePlant(d); c.sc.sp = 37.5f; c.sc.durationS = 10800; c.sc.mode = smart ? Mode::Smart : Mode::Adaptive;
    c.sc.path.enabled = true; c.sc.path.lagS = lagS; c.sc.path.fault = f; c.sc.path.faultAtS = 5400; c.sc.path.faultValue = value;
    c.sc.path.faultDurationS = dur; c.sc.path.tempOffset = offset; c.sc.metricsFromS = 3600;
    c.label = std::string(d.name) + "/" + tag;
    out.push_back(c);
  };
  for (const PlantDef &d : PLANTS) {
    for (double lag : {0.0, 10.0, 30.0, 60.0}) add(d, "nofault_lag" + std::to_string(static_cast<int>(lag)), SensorFault::None, lag, 0, 0, 0.0f);
    add(d, "offset_neg0.5", SensorFault::None, 10, 0, 0, -0.5f);
    add(d, "disconnect120s", SensorFault::Disconnect, 10, 0, 120, 0.0f);
    add(d, "stuck_low_30.0", SensorFault::StuckLow, 10, 30.0, 1e9, 0.0f);     // step > 1.5 C: plausibility filter sees a drop
    add(d, "stuck_low_36.5", SensorFault::StuckLow, 10, 36.5, 1e9, 0.0f);     // inside the 1.5 C window: accepted at once
    add(d, "stuck_high_40.0", SensorFault::StuckHigh, 10, 40.0, 1e9, 0.0f);
    add(d, "frozen_value", SensorFault::Frozen, 10, 0, 1e9, 0.0f);
    add(d, "spike_plus3", SensorFault::Spike, 10, 3.0, 1.0, 0.0f);
    add(d, "drift_low_0.5Cph", SensorFault::Drift, 10, 0.5, 1e9, 0.0f);
    add(d, "drift_low_2Cph", SensorFault::Drift, 10, 2.0, 1e9, 0.0f);
    add(d, "ssr_stuck_on", SensorFault::SsrStuckOn, 10, 0, 1e9, 0.0f);
    add(d, "ssr_and_contactor_stuck", SensorFault::SsrContactorStuck, 10, 0, 1e9, 0.0f);
  }
  return out;
}

static const char *verdict(const Result &r, const Scenario &sc) {
  if (r.unsafeCommandTicks) return "UNSAFE_COMMAND";
  const bool trueViolation = r.high > 0 || r.emergency > 0;
  // A trip/stop with NO injected fault is a nuisance trip (availability loss: the heater is cut for the rest of the batch).
  if (sc.path.fault == SensorFault::None && !trueViolation && (r.e115S >= 0 || r.e104S >= 0 || r.sensorLostS >= 0 || r.fwHigh > 0))
    return "FALSE_TRIP_NO_FAULT";
  if (trueViolation && sc.path.fault == SensorFault::SsrContactorStuck) return "ACTUATOR_UNSTOPPABLE_BY_FIRMWARE";
  if (!trueViolation) return sc.path.fault == SensorFault::None ? "OK" : "SAFE_UNDER_FAULT";
  if (r.fwHigh > 0 && r.firstFwHighS >= 0 && r.firstFwHighS - r.firstTrueHighS <= 5.0) return "TRUE_VIOLATION_DETECTED";
  if (r.e115S >= 0 || r.e104S >= 0) return "TRUE_VIOLATION_STOPPED_LATE_BY_E115_E104";
  if (r.fwHigh > 0) return "TRUE_VIOLATION_DETECTED_LATE";
  return "UNDETECTED_TRUE_VIOLATION";
}

int main(int argc, char **argv) {
  const char *cmd = argc > 1 ? argv[1] : "run";
  const bool smart = argc > 5 && std::strcmp(argv[5], "smart") == 0;
  if (!std::strcmp(cmd, "count")) { std::cout << cases(smart).size() << "\n"; return 0; }
  if (!std::strcmp(cmd, "debug")) {   // debug <label> <fromS>: per-sample controller internals on stderr
    for (const Case &c : cases(smart)) if (c.label == argv[2]) {
      Scenario sc = c.sc; sc.label = c.label; sc.debugFrom = std::atof(argv[3]); sc.debugTo = sc.debugFrom + 600; if (sc.debugFrom >= 0) sc.debugOut = &std::cerr; else { sc.trace = true; sc.traceOut = &std::cerr; }
      const Result r = run(c.p, sc);
      std::cout << "peak=" << r.truePeak << " e115=" << r.e115S << " mae=" << r.mae << "\n";
      return 0;
    }
    return 2;
  }
  const int shard = argc > 2 ? std::atoi(argv[2]) : 0, shards = argc > 3 ? std::atoi(argv[3]) : 1;
  std::ofstream f; std::ostream *o = &std::cout;
  if (argc > 4 && std::strcmp(argv[4], "-") != 0) { f.open(argv[4]); o = &f; }
  *o << std::fixed << std::setprecision(4);
  *o << "label,mode,fault,lag_s,offset,truePeak,trueHighS,trueEmergencyS,onWhileTrueHighS,fwHigh,fwEmergency,latencyHighS,e115S,e104S,"
        "sensorLostS,unsafeCommandTicks,heaterOnAfterFaultS,sensorErrMax,overshoot,mae,verdict\n";
  const std::vector<Case> all = cases(smart);
  for (size_t i = static_cast<size_t>(shard); i < all.size(); i += static_cast<size_t>(shards)) {
    Scenario sc = all[i].sc; sc.label = all[i].label;
    const Result r = run(all[i].p, sc);
    const double latency = (r.firstTrueHighS >= 0 && r.firstFwHighS >= 0) ? r.firstFwHighS - r.firstTrueHighS : -1.0;
    *o << all[i].label << ',' << modeName(sc.mode) << ',' << sensorFaultName(sc.path.fault) << ',' << sc.path.lagS << ',' << sc.path.tempOffset << ','
       << r.truePeak << ',' << r.trueHighS << ',' << r.trueEmergencyS << ',' << r.onWhileTrueHighS << ',' << r.fwHigh << ',' << r.fwEmergency << ','
       << latency << ',' << r.e115S << ',' << r.e104S << ',' << r.sensorLostS << ',' << r.unsafeCommandTicks << ',' << r.heaterOnAfterFaultS << ','
       << r.sensorErrMax << ',' << r.overshoot << ',' << r.mae << ',' << verdict(r, sc) << '\n';
  }
  return 0;
}
