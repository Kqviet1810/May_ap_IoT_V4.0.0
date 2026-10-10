// Smart Thermal startup (ThermalStartupController::setSmart) -- unit and property tests of the REAL class.
//   * the self-calibrated gain tracks a strong plant and never fires on a weak one;
//   * the unobserved-start prior is bounded in energy AND in time (no permanent stall), and ignores small start errors;
//   * safety property: on an identical input stream the Smart ceiling is never above the legacy ceiling (it only adds braking);
//   * millis() rollover and non-finite inputs are safe.
#include "thermal-fixture.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_control.h"
#include <vector>

struct Plant { double kh, dead, lag, loss, cap, amb; };   // kh: degC/s at 100 % bank; loss/cap: 1/s cooling coefficient = loss/cap
struct Step { uint32_t now; float pv, sp; float cap, requested; };

// Closed loop at 100 ms: the controller sets a ceiling every 2 s, the SSR is switched by a sigma-delta of that ceiling.
// Returns the per-decision record so that the same inputs can be replayed into the legacy controller.
static std::vector<Step> run(bool smart, Plant p, float sp, float pv0, double seconds, uint32_t t0, double *khObs, double *onUntilFirstRiseS) {
  p.amb = pv0;                                       // the oven starts at ambient equilibrium
  ThermalStartupController c; c.setSmart(smart);
  std::vector<Step> rec;
  const int lagN = static_cast<int>(p.dead * 10) + 1;
  std::vector<double> pipe(lagN, 0.0); size_t cur = 0;
  double temp = pv0, heater = 0, sigma = 0, onS = 0, filt = pv0;
  float req = 0, cap = 0;
  if (onUntilFirstRiseS) *onUntilFirstRiseS = -1;
  bool risen = false;
  for (int i = 0; i < static_cast<int>(seconds * 10); ++i) {
    const uint32_t now = t0 + static_cast<uint32_t>(i) * 100U;
    bool on = false;
    if (req > 0) { sigma += req / 100.0; if (sigma >= 1.0) { sigma -= 1.0; on = true; } }
    c.observe(now, on);
    if (on) onS += 0.1;
    const double d = pipe[cur]; pipe[cur] = on ? 1.0 : 0.0; cur = (cur + 1) % pipe.size();
    heater += (d * p.kh - heater) * 0.1 / std::max(0.1, p.lag);
    temp += (heater - (p.loss / p.cap) * (temp - p.amb)) * 0.1;
    filt += (temp - filt) * 0.1 / 4.0;                               // 4 s probe/filter lag
    if (i % 20 == 0) {
      const float pv = static_cast<float>(std::round(filt * 10.0) / 10.0);   // 0.1 degC probe
      const auto dec = c.decide(now, sp, pv, 100.0f);
      cap = dec.ceiling; req = std::min(cap, 100.0f);
      c.requested(req);
      rec.push_back({now, pv, sp, cap, req});
      if (!risen && pv > pv0 + 0.25f) { risen = true; if (onUntilFirstRiseS) *onUntilFirstRiseS = onS; }
    }
  }
  if (khObs) *khObs = c.observedGain();
  return rec;
}

int main() {
  const Plant strong{0.178, 60, 3, 120, 180000, 10};      // eff 2.0 on the lightest mass, 60 s dead time (m1824)
  const Plant strong120{0.178, 120, 8, 120, 180000, 25};  // dead time 120 s: nothing is visible for two minutes (m1836)
  const Plant weak{0.0089, 120, 8, 300, 1600000, 10};     // heavy and slow
  const Plant medium{0.0267, 15, 8, 180, 600000, 20};

  // ---- 1. the default build is the legacy controller ------------------------------------------------
  { ThermalStartupController c; assert(!c.smart()); c.setSmart(true); assert(c.smart()); c.reset(); assert(c.smart()); /* a flag, not a state */ }

  // ---- 2. gain observation ---------------------------------------------------------------------------
  { double kh = 0; run(true, strong, 37.5f, 10.0f, 400, 1000, &kh, nullptr);
    assert(kh > 0.8 * 0.178 && kh < 1.4 * 0.178);
    kh = 0; run(true, weak, 37.5f, 10.0f, 900, 1000, &kh, nullptr);
    assert(kh < 0.05);                                      // never above the legacy 0.104 trigger on a weak plant
    kh = 0; run(false, strong, 37.5f, 10.0f, 400, 1000, &kh, nullptr);
    assert(kh == 0.0);                                      // flag off: nothing is observed
  }

  // ---- 3. safety property: Smart ceiling <= legacy ceiling on identical inputs --------------------------
  struct Case { Plant p; float sp, pv0; };
  const Case cases[] = {{strong, 37.5f, 10}, {strong120, 37.5f, 25}, {weak, 37.5f, 10}, {medium, 37.5f, 20}, {strong, 30.0f, 28}, {medium, 35.0f, 33}, {weak, 32.5f, 30}};
  int steps = 0;
  for (const Case &cs : cases) {
    const std::vector<Step> smart = run(true, cs.p, cs.sp, cs.pv0, 1800, 5000, nullptr, nullptr);
    // replay: the legacy controller sees the SAME pv/ON history/requested values
    ThermalStartupController legacy; legacy.setSmart(false);
    ThermalStartupController smartAgain; smartAgain.setSmart(true);
    // The ON history is part of the input; regenerate it deterministically from the recorded requests.
    double sigma = 0; float req = 0;
    size_t k = 0;
    for (int i = 0; i < 18000 && k < smart.size(); ++i) {
      const uint32_t now = 5000U + static_cast<uint32_t>(i) * 100U;
      bool on = false;
      if (req > 0) { sigma += req / 100.0; if (sigma >= 1.0) { sigma -= 1.0; on = true; } }
      legacy.observe(now, on); smartAgain.observe(now, on);
      if (i % 20 == 0) {
        const Step &s = smart[k++];
        const auto a = legacy.decide(now, s.sp, s.pv, 100.0f);
        const auto b = smartAgain.decide(now, s.sp, s.pv, 100.0f);
        assert(std::fabs(b.ceiling - s.cap) < 1e-3f);        // replay reproduces the recorded smart run
        assert(b.ceiling <= a.ceiling + 1e-3f);              // Smart only ever adds braking
        assert(b.peak >= a.peak - 1e-3f);
        legacy.requested(s.requested); smartAgain.requested(s.requested);
        req = s.requested; ++steps;
      }
    }
  }
  assert(steps > 5000);

  // ---- 4. unobserved prior: bounded in energy, bounded in time, ignores small start errors ----------------
  { double onS = 0; const std::vector<Step> r = run(true, strong120, 37.5f, 25.0f, 600, 1000, nullptr, &onS);
    assert(onS > 0 && onS < 150.0);                          // heat committed before the first visible rise (legacy: ~205 s)
    // not a permanent stall: heat resumes
    float maxCap = 0; for (const Step &s : r) if (s.now > 1000U + 200000U) maxCap = std::max(maxCap, s.cap);
    assert(maxCap > 5.0f);
    double onLegacy = 0; run(false, strong120, 37.5f, 25.0f, 600, 1000, nullptr, &onLegacy);
    assert(onS < onLegacy);                                  // and strictly less than the legacy model commits
  }
  { // small start error (2 C): identical to legacy while nothing is observed
    const std::vector<Step> a = run(true, medium, 30.0f, 28.0f, 40, 1000, nullptr, nullptr);
    const std::vector<Step> b = run(false, medium, 30.0f, 28.0f, 40, 1000, nullptr, nullptr);
    assert(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) assert(a[i].cap == b[i].cap);
  }

  // ---- 5. millis() rollover and non-finite inputs -----------------------------------------------------------
  { const std::vector<Step> r = run(true, strong, 37.5f, 10.0f, 600, UINT32_MAX - 60000U, nullptr, nullptr);
    for (const Step &s : r) assert(std::isfinite(s.cap) && s.cap >= 0 && s.cap <= 100);
    ThermalStartupController c; c.setSmart(true); c.observe(1000, true);
    const auto d = c.decide(2000, 37.5f, NAN, 100.0f); assert(d.ceiling == 0.0f);
  }
  std::puts("Smart startup: gain observation, never-more-heat property (7 plants), bounded prior, rollover PASS");
}
