#pragma once
// Thermal V2 digital-twin simulation loop + metrics (G1).  SIMULATION ONLY.  See rig.h for what is production text.
#include <random>
#include "rig.h"
#include <fstream>
#include <functional>
#include <map>

namespace v2 {

struct Metrics {
  double reach_s = -1;            // first time the TRUE air reaches SP-0.3 (from the scenario start / event)
  double over_c = -1000;          // max (air - SP) from the reference time on
  double under_c = 1000;          // min (air - SP) after the reach / event recovery
  double settle_0p3_s = -1, settle_0p1_s = -1;     // from reach to the last exit from the band (0 = never left)
  double recov_s = -1;            // event scenarios: from the event to the last time outside +-0.3
  double tail_in_0p1_pct_avg60 = 0, tail_in_0p3_pct = 0, tail_in_0p1_pct_sensor = 0;   // last 3600 s
  double peak_air = -1000, energy_kWh = 0, max_cmd = 0, mean_cmd = 0;
  double sec_high = 0, sec_emergency = 0;
  int high_samples = 0, emergency_samples = 0;
  bool e115 = false;
  uint32_t unsafe_ticks = 0;
  double first_on_s = -1;
  double slot_calls = 0;
};

class Sim {
 public:
  Sim(const Plant &p, const Scenario &s, Controller &c) : p_(p), s_(s), ctl_(c) {}
  std::ostream *trace = nullptr;      // optional CSV trace (every 10 s)
  bool stopAtReach = false;           // used for the physical reference
  float tempOffsetCfg = NAN;

  Metrics run() {
    Rig rig; rig_ = &rig; rig.ctl_ = &ctl_;
    configure(rig);
    rig.sensorStartupGraceUntil_ = 1000U + SENSOR_STARTUP_GRACE_MS;      // production sets this at boot; without it the rig forces a 120 s vent at t=0
    ActualSensorFilter filter;
    const double dt = 0.1; rig.dt_ = dt;
    const unsigned stepMs = 100;
    const unsigned ticks = static_cast<unsigned>(s_.duration * 1000 / stepMs);
    const unsigned dly = std::max(1u, static_cast<unsigned>(p_.td * 1000 / stepMs));
    std::vector<double> pipe(dly, 0.0); size_t cur = 0;
    const bool settledInit = s_.warm && !s_.coldStart;
    double Ta = settledInit ? s_.SP : s_.startAir, Te = settledInit ? s_.SP : s_.startEgg, Ts = Ta;
    double heater = 0, heater2 = 0, vent = 0, sensPhys = Ta;
    const double ventG = p_.ventW / 12.5;
    double Tamb = p_.Tamb;
    if (settledInit) {          // settled, so the controller starts as after a reboot of a running incubator
      const double hold = p_.UA * (s_.SP - Tamb);
      heater = hold; heater2 = hold; std::fill(pipe.begin(), pipe.end(), hold);
    }
    Metrics m; std::vector<float> airTrace, sensTrace;       // 1 Hz
    airTrace.reserve(static_cast<size_t>(s_.duration) + 2);
    bool prevHigh = false, prevEmerg = false, pathLost = false;
    double cmdSum = 0; unsigned cmdN = 0; uint32_t lastCmdTick = 0;
    double deliveredNow = 0, d60 = 0;
    unsigned filterSamples = 0;
    float held = static_cast<float>(Ta), frozenValue = NAN;
    rig.temperature_ = held; rig.rawTemperature_ = held;
    for (unsigned k = 0; k < ticks; ++k) {
      const double t = k * dt;
      const uint32_t now = 1000U + k * stepMs;
      clockMs = now;
      rig.rtc_.e = 1800000000U + static_cast<uint32_t>(t);
      rig.batchElapsedSec_ = static_cast<uint32_t>(t);
      // ---- scenario events ----
      if (k == static_cast<unsigned>(s_.eventAt / dt)) {
        if (s_.door != 0) Ta += s_.door;
        if (s_.trayFrac > 0) Te -= s_.trayFrac * (Te - s_.trayTemp);
        if (s_.spStep != 0) {            // the HMI / production shift the whole alarm envelope with the target (machine_control.h legacyDefaultEnvelope, hmi.h)
          const float d = static_cast<float>(s_.spStep);
          rig.config_.targetTemp += d; rig.config_.lowTempAlarm += d; rig.config_.highTempAlarm += d; rig.config_.emergencyTemp += d;
          rig.config_.ventOnTemp += d; rig.config_.ventOffTemp += d;
        }
      }
      pathLost = s_.permitLossAt >= 0 && t >= s_.permitLossAt && t < s_.permitLossAt + s_.permitLossFor;
      const bool faultOn = s_.fault > 0 && s_.faultAt >= 0 && t >= s_.faultAt;
      if (faultOn && s_.fault == 5) pathLost = true;
      // S7 is only meaningful where the oven CAN reject the embryo heat: cap it at 60 % of the loss capacity at the end ambient (documented in acceptance.json)
      const double endAmb = isfinite(s_.ambientTo) ? s_.ambientTo : p_.Tamb;
      const double embryoCap = std::min(s_.embryoMax, 0.6 * p_.UA * std::max(0.0, s_.SP - endAmb));
      double embryo = embryoCap * std::min(1.0, t / s_.duration);
      if (isfinite(s_.ambientTo)) Tamb = p_.Tamb + (s_.ambientTo - p_.Tamb) * std::min(1.0, t / s_.duration);
      // ---- sensor chain at 0.5 Hz ----
      const bool sampleTick = (k % 20U) == 0U;
      if (p_.sensorLagS <= 0) sensPhys = Ta; else sensPhys += (Ta - sensPhys) * dt / p_.sensorLagS;
      rig.pathDataValid_ = !pathLost; rig.latestFrameValid_ = !pathLost;
      rig.newSensorSample_ = false;
      if (sampleTick && !pathLost) {
        const double stepC = std::getenv("V2_DQ") ? std::atof(std::getenv("V2_DQ")) : p_.stepC;
        static std::mt19937 noiseRng(12345);
        static const double noiseC = std::getenv("V2_NOISE") ? std::atof(std::getenv("V2_NOISE")) : 0.0;   // robustness runs only: sensor noise sigma, C
        const double noise = noiseC > 0 ? std::normal_distribution<double>(0.0, noiseC)(noiseRng) : 0.0;
        double fo = 0;
        if (faultOn) { if (s_.fault == 2) fo = -std::min(3.0, 3.0 * (t - s_.faultAt) / 1800.0); else if (s_.fault == 3) fo = -4.0; else if (s_.fault == 4) fo = 4.0; }
        float reported = static_cast<float>(std::round((sensPhys + p_.sensorOffset + noise + fo) / stepC) * stepC);   // C8: the SENSOR reads low, the controller is not told
        if (faultOn && s_.fault == 1) { if (!isfinite(frozenValue)) frozenValue = reported; reported = frozenValue; }       // S10: the frame is valid but the value never changes
        filter.updateFilter(reported, 60);
        const float filtered = filter.value();
        rig.rawTemperature_ = std::max(reported, reported + rig.config_.tempOffset);
        rig.pathAccept(now, filtered + rig.config_.tempOffset, true);
        ++filterSamples;
      }
      rig.pathUsable(now);
      rig.pathAlarms(now);
      rig.pathEvidence(now);
      rig.applyFaults();
      if (rig.heaterNotHeatingActive_) m.e115 = true;
      rig.deliveredPct_ = static_cast<float>(deliveredNow);
      rig.updateHeatingAndOutputs(now);
      const auto st = rig.outputs_.state();
      const bool on = st.heaterSsr && st.heatMaster;
      if (on && (rig.faults_.inhibit || rig.faults_.drop)) ++m.unsafe_ticks;
      if (on && m.first_on_s < 0) m.first_on_s = t;
      deliveredNow = (on ? 100.0 : 0.0);          // what the arbiter THINKS it gave (a welded SSR is invisible to it)        // instantaneous 0/100 of the last step; the controller integrates it with its own window
      d60 += ((on ? 1.0 : 0.0) - d60) * dt / 60.0;
      cmdSum += rig.lastCommand_; ++cmdN; m.max_cmd = std::max<double>(m.max_cmd, rig.lastCommand_);
      // ---- plant ----
      double sag = 1.0;
      const double sagD = std::max(p_.sagPerDuty, s_.sagPerDuty);
      if (sagD > 0) sag -= sagD * d60;          // supply sag follows the DELIVERED duty over ~60 s, as plants.json C7 says
      if (p_.sagStepAtS >= 0 && t >= p_.sagStepAtS) sag *= (1.0 - p_.sagStep);
      bool plantOn = on;
      if (faultOn && s_.fault == 6) plantOn = st.heatMaster;          // the SSR is welded: only the master contactor can stop the heater
      if (faultOn && s_.fault == 7) plantOn = false;                  // the SSR never conducts
      const double delivered = plantOn ? p_.Pmax * p_.gainScale * sag : 0.0;
      const double delayed = pipe[cur]; pipe[cur] = delivered; cur = (cur + 1) % pipe.size();
      heater += (delayed - heater) * dt / std::max(0.1, p_.tl);
      if (p_.tailFrac > 0) heater2 += (delayed - heater2) * dt / std::max(0.1, p_.tailTauS);
      const double heaterEff = p_.tailFrac > 0 ? (1.0 - p_.tailFrac) * heater + p_.tailFrac * heater2 : heater;
      vent += ((st.ventFan ? 1.0 : 0.0) - vent) * dt / 8.0;
      double extraLoss = 0;
      if (s_.ventAt >= 0 && s_.ventKnown == 0 && t >= s_.ventAt && t < s_.ventAt + s_.ventFor) extraLoss = ventG;   // unscheduled loss
      double hA = p_.hA;
      if (p_.fanCoupling && std::fmod(t, 7200.0) < 1200.0) hA *= 0.6;
      const double q = hA * (Ta - Te);
      if (s_.doorG > 0 && s_.doorAt >= 0 && t >= s_.doorAt && t < s_.doorAt + s_.doorFor) extraLoss += s_.doorG;           // S11: door held open
      if (s_.doorFrac > 0 && s_.doorAt >= 0 && t >= s_.doorAt && t < s_.doorAt + s_.doorFor) extraLoss += s_.doorFrac * p_.Pmax * p_.gainScale / std::max(5.0, s_.SP - p_.Tamb);   // S13
      double dTa = heaterEff - p_.UA * (Ta - Tamb) - q - ventG * vent * (Ta - Tamb) - extraLoss * (Ta - Tamb) + p_.fanW;
      if (p_.thirdC > 0) { const double qs = p_.thirdG * (Ta - Ts); dTa -= qs; Ts += qs * dt / p_.thirdC; }
      g_truthDemandPct = 100.0 * (p_.UA * (Ta - Tamb) + q + ventG * vent * (Ta - Tamb) + extraLoss * (Ta - Tamb) - p_.fanW) / (p_.Pmax * p_.gainScale);
      Ta += dTa * dt / p_.Cf;
      Te += (q + embryo) * dt / p_.Ce;
      if (!isfinite(Ta) || Ta > 200) { std::fprintf(stderr, "plant blew up %s %s\n", p_.id.c_str(), s_.id.c_str()); std::abort(); }
      m.energy_kWh += delivered * dt / 3.6e6;
      // ---- 1 Hz records ----
      if ((k % 10U) == 0U) {
        airTrace.push_back(static_cast<float>(Ta)); sensTrace.push_back(rig.temperature_);   // scored on the TRUE air (acceptance.json); a calibration offset (set C8) is the controller's job through config tempOffset
        const bool hi = Ta >= rig.config_.highTempAlarm, em = Ta >= rig.config_.emergencyTemp;
        if (hi) { m.sec_high += 1; if (!prevHigh) ++m.high_samples; }
        if (em) { m.sec_emergency += 1; if (!prevEmerg) ++m.emergency_samples; }
        prevHigh = hi; prevEmerg = em; m.peak_air = std::max<double>(m.peak_air, Ta);
        if (trace && (k % 100U) == 0U)
          (*trace) << t << ',' << Ta << ',' << Te << ',' << rig.temperature_ << ',' << rig.lastCommand_ << ',' << (on ? 1 : 0) << ',' << rig.highTemperatureActive_
                   << ',' << rig.heaterNotHeatingActive_ << ',' << st.ventFan << '\n';
        if (stopAtReach && Ta >= s_.SP - 0.3 && t > s_.eventAt * (s_.warm ? 1 : 0)) { k = ticks; break; }
      }
    }
    m.mean_cmd = cmdN ? cmdSum / cmdN : 0;
    score(m, airTrace, sensTrace);
    return m;
  }

  void score(Metrics &m, const std::vector<float> &air, const std::vector<float> &sens) const {
    const double SP = s_.SP + s_.spStep, t0 = s_.warm ? s_.eventAt : 0.0;
    const size_t n = air.size(), i0 = static_cast<size_t>(t0);
    size_t reach = n;
    if (!s_.warm) { for (size_t i = i0; i < n; ++i) if (air[i] >= SP - 0.3) { reach = i; break; } }
    else reach = i0;
    if (!s_.warm && reach < n) m.reach_s = static_cast<double>(reach);
    if (s_.warm) {          // disturbance scenarios: seconds from the event until the air is back at SP-0.3 (0 if it never dipped that far)
      size_t dip = n; for (size_t i = i0; i < n; ++i) if (air[i] < SP - 0.3) { dip = i; break; }
      if (dip >= n) m.reach_s = 0;
      else { m.reach_s = -1; for (size_t i = dip; i < n; ++i) if (air[i] >= SP - 0.3) { m.reach_s = static_cast<double>(i - i0); break; } }
    }
    const size_t from = (s_.warm ? i0 : std::min(reach, n));
    for (size_t i = from; i < n; ++i) { m.over_c = std::max<double>(m.over_c, air[i] - SP); }
    size_t under_from = s_.warm ? i0 : reach;
    if (s_.door != 0 || s_.trayFrac > 0) under_from = n;   // the dip itself is the disturbance
    if (s_.door != 0 || s_.trayFrac > 0) {
      // recovery: last time outside +-0.3 after the event
      size_t last = i0; for (size_t i = i0; i < n; ++i) if (std::fabs(air[i] - SP) > 0.3) last = i + 1;
      m.recov_s = (last >= n) ? -1 : static_cast<double>(last - i0);
    }
    for (size_t i = under_from; i < n; ++i) m.under_c = std::min<double>(m.under_c, air[i] - SP);
    auto settle = [&](double band) {
      if (reach >= n && !s_.warm) return -1.0;
      size_t last = from; for (size_t i = from; i < n; ++i) if (std::fabs(air[i] - SP) > band) last = i + 1;
      return last >= n ? -1.0 : static_cast<double>(last - from);
    };
    m.settle_0p3_s = settle(0.3); m.settle_0p1_s = settle(0.1);
    const size_t W = 3600, a = n > W ? n - W : 0;
    auto avg60 = [&](const std::vector<float> &v, size_t i) { size_t s0 = i >= 59 ? i - 59 : 0; double sum = 0; for (size_t j = s0; j <= i; ++j) sum += v[j]; return sum / (i - s0 + 1); };
    unsigned c1 = 0, c3 = 0, c1s = 0, tot = 0;
    for (size_t i = a; i < n; ++i) {
      ++tot;
      if (std::fabs(avg60(air, i) - SP) <= 0.1) ++c1;
      if (std::fabs(air[i] - SP) <= 0.3) ++c3;
      if (std::fabs(avg60(sens, i) - (SP + 0)) <= 0.1) ++c1s;
    }
    if (tot) { m.tail_in_0p1_pct_avg60 = 100.0 * c1 / tot; m.tail_in_0p3_pct = 100.0 * c3 / tot; m.tail_in_0p1_pct_sensor = 100.0 * c1s / tot; }
  }

 private:
  void configure(Rig &rig) {
    rig.config_.targetTemp = static_cast<float>(s_.SP);
    rig.config_.tempOffset = isfinite(tempOffsetCfg) ? tempOffsetCfg : 0.0f;
    rig.batchRunning_ = s_.batchRunning;
    rig.inputs_.in.heaterEnable = true;
    if (s_.ventAt >= 0 && s_.ventKnown != 0) {              // scheduled vent through the production schedule logic
      rig.config_.ventScheduleEnabled = true; rig.config_.ventAutoEnabled = false; rig.config_.ventScheduleCount = 1;
      rig.config_.ventScheduleDurationMin = static_cast<uint8_t>(std::max(1.0, s_.ventFor / 60.0));
      rig.config_.ventScheduleHour1 = static_cast<uint8_t>((((1800000000U + static_cast<uint32_t>(s_.ventAt)) % 86400U) / 3600U));
    }
  }
  Plant p_; Scenario s_; Controller &ctl_; Rig *rig_ = nullptr;
};

}  // namespace v2
