#pragma once
// Predictive thermal controller (Thermal Predictive V1).
//
// WHY: the oven is two thermal masses, not one. The sensor sees the AIR (fast, a few hundred kJ/degC); the eggs (about 1.9 MJ/degC for a
// full load) exchange heat with it through the airflow only. A controller that models one mass thinks the oven is warm as soon as the air
// is, cuts the heat, and the eggs then pull the air down again (cold start); after an overshoot the eggs keep the surplus for hours
// because the oven can only lose a few hundred watts. See doc/THERMAL_REDESIGN_RESEARCH.md.
//
// WHAT: everything is in "% of the heater bank" units, so the heater power (16..20 kW) never has to be known:
//     cf * dTa/dt = hl - a*(Ta - Tref) - k*(Ta - Te) + d - vp*vent        (air, hl = delayed and lagged heater power)
//     ce * dTe/dt = k*(Ta - Te) + w                                       (eggs; w = embryo heat)
//   * a 4-state Kalman filter [Ta, Te, w, d] tracks the egg temperature and the two slow disturbances from the air temperature alone;
//   * a background identifier (a (1+1)-evolution search, one simulation per call, no Jacobian) fits cf, ce, k, a, delay, lag, vent and the
//     starting state to the last 90 minutes of (heater, PV) data, with log-normal priors, so a wrong first guess is corrected within minutes;
//   * a planner simulates a few hundred candidate power profiles two hours ahead and applies the first action of the cheapest one
//     (overshoot costs more than undershoot, and an energy-balance tail law keeps the air at SP while the eggs absorb heat);
//   * a supervisor decides when the model is trustworthy; otherwise the legacy controller keeps control (this class only PROPOSES power).
// Nothing here allocates; heavy work is chunked (service()) so the control cycle never stalls. Pure C++11, host-testable.
#include <cmath>
#include <cstdint>
#include <cstring>

namespace MayapPredictive {

constexpr float Tref = 37.5f;           // the model's reference temperature (loss is linear around it); independent of the set point
constexpr int BinSec = 10;              // estimator / identifier time base
constexpr int HistBins = 540;           // 90 minutes: the fit window
constexpr int PreBins = 60;             // 10 minutes of heater history before the window (delay and lag need it)
constexpr int CapBins = HistBins + PreBins;
constexpr int PlanDtSec = 30;           // planner step
constexpr int PlanSteps = 240;          // two hours
static_assert(CapBins >= 2 * PlanSteps, "the shared scratch buffer must hold a plan (16 bit per step)");
constexpr int NP = 12;                  // identified parameters
constexpr int RecentDBins = 90;         // the disturbance estimate that may differ in the last 15 minutes (a few times the longest delay + lag)

struct Model {            // % units, seconds
  float cf = 2500.0f;     // air + steel + trays: % * s / degC
  float ce = 9000.0f;     // eggs
  float k = 6.0f;         // air <-> eggs coupling, % / degC
  float a = 1.2f;         // loss to ambient, % / degC
  float dead = 60.0f;     // transport delay of the heater to the sensor
  float tau = 40.0f;      // heater / element lag
  float vp = 10.0f;       // power removed while the exhaust fan runs with the air 12.5 degC above the room, %
  float tamb = 25.0f;     // the room (what the exhaust fan blows in): its power scales with the air-to-room difference
  float ventPower(float ta) const { const float dT = ta - tamb; return dT > 0.0f ? vp * dT * (1.0f / 12.5f) : 0.0f; }
};

// Wide priors (any oven of this class) and hard bounds. Index order is the identifier's parameter vector.
//   0 ln cf, 1 ln ce, 2 ln k, 3 ln a, 4 ln dead, 5 ln tau, 6 vp, 7 d0, 8 off (Ta0 - Te0), 9 w0, 10 dr (extra air-side power over the last 15 min), 11 room temperature
struct Prior {
  static constexpr float mu[NP] = {7.824f, 9.105f, 1.792f, 0.0f, 4.094f, 3.689f, 10.0f, -10.0f, 0.0f, 0.0f, 0.0f, 25.0f};
  static constexpr float sd[NP] = {0.9f, 0.9f, 0.9f, 0.6f, 0.7f, 0.7f, 8.0f, 8.0f, 1.5f, 3.0f, 4.0f, 6.0f};
  static constexpr float lo[NP] = {6.0f, 7.6f, 0.2f, -3.0f, 1.6f, 2.3f, 1.0f, -60.0f, -5.0f, -2.0f, -15.0f, 5.0f};
  static constexpr float hi[NP] = {9.6f, 11.1f, 3.4f, 1.8f, 5.5f, 4.8f, 45.0f, 15.0f, 14.0f, 12.0f, 15.0f, 36.0f};
};
constexpr float Prior::mu[NP];
constexpr float Prior::sd[NP];
constexpr float Prior::lo[NP];
constexpr float Prior::hi[NP];

// Learning: no trusted model yet (the launch controller of the machine drives). Active: the planner drives. Degraded (value 3): the model is not
// trusted for planning any more, but THIS controller still drives with a robust PI built on the hold power of the model; the legacy PID never
// takes power back once a batch has been under this controller.
enum class State : uint8_t { Off = 0, Learning = 1, Active = 2, Degraded = 3 };

struct Telemetry {
  State state = State::Off;
  float rms = 0;            // identifier residual, degC
  float innov = 0;          // Kalman innovation RMS (recent), degC
  float te = 0, w = 0, d = 0;
  float request = 0;
  Model m;
  uint16_t windowBins = 0;
  bool cooling = false;
  bool modelOk = false;
};

// Persistent summary of the model fitted on the last batch (a prior for the next one).
struct StoredModel {
  bool valid = false;
  Model m;
  float rms = 0;
};

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline bool finitef(float v) { return std::isfinite(v); }

// ---------------------------------------------------------------------------------------------------------------------------------------
#ifndef MAYAP_VENT_BIT_THRESHOLD
#define MAYAP_VENT_BIT_THRESHOLD 0.5f
#endif
class History {
 public:
  static constexpr float VentBitThreshold = MAYAP_VENT_BIT_THRESHOLD;
  void reset() { n_ = 0; std::memset(u_, 0, sizeof(u_)); std::memset(y_, 0, sizeof(y_)); std::memset(vb_, 0, sizeof(vb_)); }
  void push(float uPct, float pv, float vent) {
    const uint32_t i = n_ % CapBins;
    u_[i] = static_cast<uint8_t>(clampf(uPct, 0.0f, 100.0f) * 2.0f + 0.5f);
    y_[i] = static_cast<int16_t>((clampf(pv, 0.0f, 80.0f) - 20.0f) * 100.0f + 0.5f);
    if (vent >= VentBitThreshold) vb_[i >> 3] = static_cast<uint8_t>(vb_[i >> 3] | (1U << (i & 7U))); else vb_[i >> 3] = static_cast<uint8_t>(vb_[i >> 3] & ~(1U << (i & 7U)));      // the fan was on for most of the bin
    ++n_;
  }
  uint32_t count() const { return n_; }
  uint32_t first() const { return n_ > static_cast<uint32_t>(CapBins) ? n_ - CapBins : 0U; }          // oldest bin still stored
  uint32_t windowStart() const { return n_ > static_cast<uint32_t>(CapBins) ? n_ - HistBins : 0U; }   // first bin the fit scores
  float u(int64_t bin) const {      // % (0 before the data starts)
    if (bin < static_cast<int64_t>(first()) || bin >= static_cast<int64_t>(n_)) return 0.0f;
    return 0.5f * u_[static_cast<uint32_t>(bin) % CapBins];
  }
  float y(int64_t bin) const { return 20.0f + 0.01f * y_[static_cast<uint32_t>(bin) % CapBins]; }
  float v(int64_t bin) const {
    if (bin < static_cast<int64_t>(first()) || bin >= static_cast<int64_t>(n_)) return 0.0f;
    const uint32_t i = static_cast<uint32_t>(bin) % CapBins;
    return (vb_[i >> 3] >> (i & 7U)) & 1U ? 1.0f : 0.0f;
  }
  // Heater power delivered at time `t` (seconds, bin 0 starts at 0), linear between bin centres; before the data: 0.
  float uAtTime(float t) const {
    const float f = t / BinSec - 0.5f;
    if (f < 0.0f) return f <= -1.0f ? 0.0f : u(0) * (1.0f + f);
    const int64_t i = static_cast<int64_t>(f);
    const float fr = f - static_cast<float>(i);
    return u(i) * (1.0f - fr) + u(i + 1) * fr;
  }
 private:
  uint32_t n_ = 0;
  uint8_t u_[CapBins] = {};
  int16_t y_[CapBins] = {};
  uint8_t vb_[(CapBins + 7) / 8] = {};
};

// ---------------------------------------------------------------------------------------------------------------------------------------
// One integration step of the two-node model (used by the Kalman filter, the identifier and the planner).
struct Sim {
  float ta, te, hl;
};
// The constants of a step depend only on the model and on dt: computed once per simulation, not per step (the exponential dominates the cost).
struct StepCoef { float h; float ex; int sub; };
inline StepCoef stepCoef(const Model &m, float dt) {
  StepCoef c;
  // substeps keep the explicit scheme stable for stiff (small cf, large k) models
  const float stiff = (m.a + m.k) * dt / m.cf;
  c.sub = stiff > 0.8f ? static_cast<int>(stiff / 0.8f) + 1 : 1;
  c.h = dt / static_cast<float>(c.sub);
  c.ex = 1.0f - std::exp(-c.h / m.tau);
  return c;
}
inline void simStep(const Model &m, const StepCoef &c, float uDelayed, float d, float w, float vent, Sim &s) {
  for (int i = 0; i < c.sub; ++i) {
    s.hl += c.ex * (uDelayed - s.hl);
    const float q = m.k * (s.ta - s.te);
    s.ta += (c.h / m.cf) * (s.hl - m.a * (s.ta - Tref) - q + d - m.ventPower(s.ta) * vent);
    s.te += (c.h / m.ce) * (q + w);
  }
}
inline void simStep(const Model &m, float dt, float uDelayed, float d, float w, float vent, Sim &s) { simStep(m, stepCoef(m, dt), uDelayed, d, w, vent, s); }

// ---------------------------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------------------------
// Background identifier: (1+1)-evolution search with per-coordinate steps over the last HistBins bins.
class Identifier {
 public:
  void reset(const StoredModel &stored) {
    for (int i = 0; i < NP; ++i) x_[i] = Prior::mu[i];
    for (int i = 0; i < NP; ++i) mu_[i] = Prior::mu[i];
    for (int i = 0; i < NP; ++i) sd_[i] = Prior::sd[i];
    if (stored.valid) {          // the last batch's physics is a much tighter prior for the next one (the eggs' state is not)
      const float v[7] = {std::log(stored.m.cf), std::log(stored.m.ce), std::log(stored.m.k), std::log(stored.m.a),
                          std::log(stored.m.dead), std::log(stored.m.tau), stored.m.vp};
      for (int i = 0; i < 7; ++i) { mu_[i] = x_[i] = clampf(v[i], Prior::lo[i], Prior::hi[i]); sd_[i] = Prior::sd[i] * (i == 6 ? 0.6f : 0.45f); }
    }
    for (int i = 0; i < NP; ++i) step_[i] = 0.35f * sd_[i];
    cost_ = -1.0f; evals_ = 0; accepted_ = 0; rng_ = 0x9E3779B9u; coord_ = 0; dirty_ = true; rms_ = 0.0f; nEvents_ = 0; quiet_ = 0; onset_ = -1; badBins_ = 0;
  }
  void bind(int8_t *scratch) { scratch_ = scratch; }
  // Test hook: pin the parameter vector (and stop searching).
  void freeze(const float *x) { std::memcpy(x_, x, sizeof(x_)); frozen_ = true; dirty_ = true; }
  // Test hook: pin the coordinates in `mask` to `x` and keep searching the others.
  void pin(const float *x, uint32_t mask) { pinMask_ = mask; for (int i = 0; i < NP; ++i) if (mask & (1u << i)) x_[i] = pinVal_[i] = x[i]; dirty_ = true; }
  // One evaluation. Returns true when it did work.
  bool service(const History &h) {
    const uint32_t n = h.count();
    if (n < 24U) return false;
    if (frozen_) {
      if (n != lastN_ || dirty_) { lastN_ = n; dirty_ = false; cost_ = evaluate(x_, h, &rms_, &end_, offTr_, &recent_); return true; }
      return false;
    }
    if (dirty_ || n != lastN_) {      // the window moved: re-score the incumbent first
      lastN_ = n; dirty_ = false;
      anchor(h);
      rich_ = physicsLearnable(h);
      float lastErr = 0.0f;
      cost_ = evaluate(x_, h, &rms_, &end_, offTr_, &recent_, &lastErr, &slope_, &peak_);
      // a sudden miss of the last bin after a quiet stretch is an event, not a worse model
      if (std::fabs(lastErr) > 0.6f && quiet_ >= 6U && !isEvent(n - 1U)) {
        addEvent(n - 1U); quiet_ = 0;
        cost_ = evaluate(x_, h, &rms_, &end_, offTr_, &recent_, &lastErr, &slope_, &peak_);     // score again without the event at once: the supervisor must not see the raw miss
      }
      else if (std::fabs(lastErr) < 0.3f) { if (quiet_ < 100U) ++quiet_; }
      else quiet_ = 0;
      return true;
    }
    float cand[NP];
    std::memcpy(cand, x_, sizeof(cand));
    int j = coord_;
    coord_ = (coord_ + 1) % (NP + 3);
    // A power the model lacks (a door held open, the room warming up, the load changing) shows as a steady drift of the residual: the size
    // of the missing power is -cf * slope. Propose it directly every fourth evaluation instead of waiting for the random search to find it.
    const float missing = -std::exp(x_[0]) * slope_;
    const bool newton = (evals_ & 3U) == 3U && std::fabs(missing) >= 0.3f;
    // Poor data (hold, no events) cannot teach the physics: only the slow nuisances (vent, d, off, w) may move
    if (!rich_ && !frozen_) j = 6 + (j % 6);
    if (newton) {
      j = 10; cand[10] += clampf(0.8f * missing, -10.0f, 10.0f);
    } else if (j < NP) {
      const float dir = (nextRand() & 1U) ? 1.0f : -1.0f;
      cand[j] += dir * step_[j] * (0.5f + uniform());
    } else if (j == NP) {          // heat capacities move together (the data fix their ratio to the heater power much better than each)
      const float s = (uniform() - 0.5f) * 0.5f;
      cand[0] += s; cand[1] += s * 0.7f;
    } else if (j == NP + 1) {      // coupling and capacity of the eggs trade off (time constant)
      const float s = (uniform() - 0.5f) * 0.5f;
      cand[1] += s; cand[2] += s;
    } else {                       // delay and lag trade off against the air capacity
      const float s = (uniform() - 0.5f) * 0.4f;
      cand[4] += s; cand[0] -= 0.4f * s;
    }
    clampVector(cand);
    for (int i = 0; i < NP; ++i) if (pinMask_ & (1u << i)) cand[i] = pinVal_[i];
    float rms = 0.0f;
    Sim endCand{0, 0, 0};
    const float c = evaluate(cand, h, &rms, &endCand, scratch_);
    ++evals_;
    if (c < cost_) {
      std::memcpy(x_, cand, sizeof(x_)); cost_ = c; rms_ = rms; ++accepted_; end_ = endCand;
      if (scratch_) std::memcpy(offTr_, scratch_, sizeof(offTr_));
      if (j < NP) step_[j] = std::fmin(step_[j] * 1.4f, 1.0f * sd_[j]);
    } else if (j < NP) {
      step_[j] = std::fmax(step_[j] * 0.8f, 0.01f * sd_[j]);
    }
    return true;
  }
  // The physics (capacities, coupling, delay, lag) is learnable only from the warm-up: while the window still contains the first heating
  // (or the model keeps failing). Afterwards the values are kept and only the slow nuisances move.
  void setOnset(int64_t bin) { onset_ = bin; }
  bool physicsLearnable(const History &h) {
    if (onset_ < 0) return true;
    if (static_cast<int64_t>(h.windowStart()) <= onset_ + 6) { badBins_ = 0; return true; }
    badBins_ = recent_ > 0.18f ? badBins_ + 1U : 0U;
    return badBins_ > 120U;                       // the model has not fitted for 20 minutes: let the physics move again
  }
  bool rich() const { return rich_; }
  bool isEvent(uint32_t bin) const { for (int i = 0; i < nEvents_; ++i) if (events_[i] == bin) return true; return false; }
  void addEvent(uint32_t bin) {
    if (nEvents_ == MaxEvents) { for (int i = 1; i < nEvents_; ++i) events_[i - 1] = events_[i]; --nEvents_; }
    events_[nEvents_++] = bin;
  }
  int eventCount() const { return nEvents_; }
  float rms() const { return rms_; }
  float recentPeak() const { return peak_; }      // largest miss of the last half minute, degC
  float recentRms() const { return recent_; }     // residual over the last two minutes (a model that stopped fitting shows here first)
  uint32_t evals() const { return evals_; }
  uint32_t accepted() const { return accepted_; }
  bool atBound() const {
    for (int i = 0; i < NP; ++i) {
      const float span = Prior::hi[i] - Prior::lo[i];
      if (i < 6 && (x_[i] - Prior::lo[i] < 0.02f * span || Prior::hi[i] - x_[i] < 0.02f * span)) return true;
    }
    return false;
  }
  Model model() const { return toModel(x_); }
  // Simulated state at the end of the window under the incumbent (the moving-horizon estimate of the hidden egg temperature).
  const Sim &endState() const { return end_; }
  float d0() const { return x_[7]; }
  float dEffective() const { return x_[7] + x_[10]; }      // the air-side power to assume from now on
  float off0() const { return x_[8]; }
  float w0() const { return x_[9]; }
  static Model toModel(const float *x) {
    Model m;
    m.cf = std::exp(x[0]); m.ce = std::exp(x[1]); m.k = std::exp(x[2]); m.a = std::exp(x[3]);
    m.dead = std::exp(x[4]); m.tau = std::exp(x[5]); m.vp = x[6]; m.tamb = x[11];
    return m;
  }
  // Residual RMS of the model `m` (with the given nuisance values) on the window; used by tests and by the supervisor.
  float score(const float *x, const History &h) const { float r = 0; evaluate(x, h, &r); return r; }
  const float *vector() const { return x_; }

 private:
  uint32_t nextRand() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return rng_; }
  float uniform() { return static_cast<float>(nextRand() >> 8) * (1.0f / 16777216.0f); }
  // First bin of the fit. A bin that a door (or any step) completed inside is not the level the model starts from: begin after it.
  uint32_t startBin(const History &h) const {
    uint32_t s = h.windowStart();
    while (s + 1U < h.count() && (isEvent(s) || (s > 0U && isEvent(s - 1U)))) ++s;
    return s;
  }
  static void clampVector(float *x) { for (int i = 0; i < NP; ++i) x[i] = clampf(x[i], Prior::lo[i], Prior::hi[i]); }
  // When the window has slid, the eggs' temperature at its first bin is what the previous incumbent simulated for that bin (the prior of
  // `off` is re-centred there); at the very start of a batch the eggs are assumed to be at the air temperature.
  void anchor(const History &h) {
    const uint32_t s = startBin(h);
    if (s == 0U) return;
    const float a = 0.1f * offTr_[s % CapBins];
    mu_[8] = a; sd_[8] = 0.7f;
    x_[8] = clampf(a, Prior::lo[8], Prior::hi[8]);
  }
  float evaluate(const float *x, const History &h, float *rmsOut, Sim *endOut = nullptr, int8_t *trace = nullptr, float *recentOut = nullptr, float *lastErrOut = nullptr, float *slopeOut = nullptr, float *peakOut = nullptr) const {
    const Model m = toModel(x);
    const uint32_t n = h.count(), s = startBin(h);
    const float d0 = x[7], w0 = x[9], dr = x[10];
    const uint32_t recentFrom = n > static_cast<uint32_t>(RecentDBins) ? n - RecentDBins : 0U;
    Sim sim;
    sim.ta = h.y(s);
    sim.te = sim.ta - x[8];
    // the heater state when the window opens: run the delay and the lag over the history that precedes it
    sim.hl = 0.0f;
    if (s > 0U) {
      const uint32_t p0 = s > static_cast<uint32_t>(PreBins) ? s - PreBins : 0U;
      const float ex = 1.0f - std::exp(-static_cast<float>(BinSec) / m.tau);
      for (uint32_t b = p0; b < s; ++b) sim.hl += ex * (h.uAtTime((static_cast<float>(b) + 0.5f) * BinSec - m.dead) - sim.hl);
    }
    if (trace) trace[s % CapBins] = static_cast<int8_t>(clampf(x[8] * 10.0f, -127.0f, 127.0f));
    const StepCoef sc = stepCoef(m, static_cast<float>(BinSec));
    float chi = 0.0f, chiRecent = 0.0f;
    const float inv = 1.0f / (0.07f * 0.07f);
    uint32_t cnt = 0, cntRecent = 0;
    // slope of the residual over the last SlopeBins bins: a constant power the model lacks makes (model - measurement) drift at -power/cf
    float peak = 0.0f;
    float sn = 0.0f, st = 0.0f, se = 0.0f, stt = 0.0f, ste = 0.0f;
    for (uint32_t b = s; b < n; ++b) {
      const float tMid = (static_cast<float>(b) + 0.5f) * BinSec;
      const float ud = h.uAtTime(tMid - m.dead);
      simStep(m, sc, ud, b >= recentFrom ? d0 + dr : d0, w0, h.v(b), sim);
      const float e = sim.ta - h.y(b);
      if (lastErrOut && b + 1U == n) *lastErrOut = e;
      if (isEvent(b) || (b > 0U && isEvent(b - 1U))) {   // an air-side step the model cannot explain (door, a glitch) completes inside its bin (the bin mean is not the new level): re-synchronise on that bin and the next, no scoring
        sim.ta = h.y(b);
        if (trace) trace[b % CapBins] = static_cast<int8_t>(clampf((sim.ta - sim.te) * 10.0f, -127.0f, 127.0f));
        continue;
      }
      if (trace) trace[b % CapBins] = static_cast<int8_t>(clampf((sim.ta - sim.te) * 10.0f, -127.0f, 127.0f));
      if (b > s) { chi += e * e * inv; ++cnt; }
      { const float over = sim.te - sim.ta - 3.0f; if (over > 0.0f) chi += over * over * 400.0f; }   // the eggs are not hotter than the air by degrees
      if (recentOut && b + RecentBins >= n) { chiRecent += e * e; ++cntRecent; }
      if (peakOut && b + PeakBins >= n && std::fabs(e) > peak) peak = std::fabs(e);
      if (slopeOut && b + SlopeBins >= n) { const float tt = static_cast<float>(b + SlopeBins - n); sn += 1.0f; st += tt; se += e; stt += tt * tt; ste += tt * e; }
      if (!finitef(sim.ta) || sim.ta > 80.0f || sim.ta < -20.0f) { *rmsOut = 99.0f; return 1e30f; }
    }
    float prior = 0.0f;
    for (int i = 0; i < NP; ++i) { const float z = (x[i] - mu_[i]) / sd_[i]; prior += z * z; }
    *rmsOut = cnt ? std::sqrt(chi / inv / static_cast<float>(cnt)) : 0.0f;
    if (endOut) *endOut = sim;
    if (recentOut) *recentOut = cntRecent ? std::sqrt(chiRecent / static_cast<float>(cntRecent)) : 0.0f;
    if (peakOut) *peakOut = peak;
    if (slopeOut) { const float den = sn * stt - st * st; *slopeOut = (sn >= 8.0f && den > 1e-3f) ? (sn * ste - st * se) / den / static_cast<float>(BinSec) : 0.0f; }
    return chi + prior;
  }
  float x_[NP], mu_[NP], sd_[NP], step_[NP];
  int8_t offTr_[CapBins] = {};      // (Ta - Te) of the incumbent per bin, 0.1 C: the arrival state of the next window
  int8_t *scratch_ = nullptr;       // shared with the planner (never both at once): the controller owns the buffer
  static constexpr uint32_t RecentBins = 12;
  static constexpr uint32_t SlopeBins = 18;
  static constexpr uint32_t PeakBins = 3;
  static constexpr int MaxEvents = 6;
  uint32_t events_[MaxEvents] = {};
  int nEvents_ = 0;
  uint32_t quiet_ = 0;
  float cost_ = -1.0f, rms_ = 0.0f, recent_ = 0.0f, slope_ = 0.0f, peak_ = 0.0f;
  Sim end_{0, 0, 0};
  uint32_t evals_ = 0, accepted_ = 0, lastN_ = 0, rng_ = 1;
  int coord_ = 0;
  bool dirty_ = true, dirtyEnd_ = false, frozen_ = false, rich_ = true;
  int64_t onset_ = -1;
  uint32_t badBins_ = 0;
  uint32_t pinMask_ = 0;
  float pinVal_[NP] = {};
};

// ---------------------------------------------------------------------------------------------------------------------------------------
// The exhaust fan the machine itself has scheduled (ventilation owns WHEN and HOW LONG): a known disturbance of tens of percent of the bank
// that the plan must pre-compensate by the delay of the heater, not wait to see in the air temperature.
struct VentSchedule {
  float startsIn = -1.0f;    // s to the next run (>= 0 when the fan is not running and the next run is known)
  float remaining = -1.0f;   // s to the end of the run in progress (>= 0 when it is running and its end is known)
  float run = 0.0f;          // length of a run, s
  float cycle = 0.0f;        // period of the runs, s (0: no repeat inside the horizon)
  // 1 when the scheduled fan runs `t` seconds from now
  float at(float t) const {
    if (t < 0.0f) return 0.0f;
    float first = -1.0f;
    if (remaining >= 0.0f) { if (t < remaining) return 1.0f; first = cycle > run && run > 0.0f ? remaining + (cycle - run) : -1.0f; }
    else if (startsIn >= 0.0f && run > 0.0f) first = startsIn;
    if (first < 0.0f || t < first) return 0.0f;
    const float x = t - first;
    return (cycle > run ? std::fmod(x, cycle) : x) < run ? 1.0f : 0.0f;
  }
  bool known() const { return remaining >= 0.0f || (startsIn >= 0.0f && run > 0.0f); }
};

struct PlanChoice {
  float du = 0.0f;      // offset (% of the bank) added to the energy-balance law for the first T1 minutes: the deviation the plan commits to
  float t1Min = 0.0f;
  float tau = 300.0f;   // tail law time constant, s
  float ventMin = 0.0f; // exhaust fan on for this many minutes
  float cost = 1e30f;
  float peak = 0.0f;    // highest predicted air temperature above SP, degC
  bool valid = false;
};

class Planner {
 public:
  struct State { float ta, te, w, d, hl; };
  void start(const Model &m, const State &st, float sp, float limitPct, bool ventAllowed, const History &h, uint32_t nowBin, const VentSchedule &vs) {
    vs_ = vs;
    m_ = m; ta_ = st.ta; te_ = st.te; w_ = st.w; d_ = st.d; hl0_ = st.hl; sp_ = sp;
    limit_ = limitPct; ventAllowed_ = ventAllowed; hist_ = &h; nowBin_ = nowBin;
    idx_ = 0; best_ = PlanChoice{}; running_ = true;
  }
  void bind(uint16_t *scratch) { cmd_ = scratch; }       // PlanSteps entries
  bool running() const { return running_; }
  // Evaluate up to `n` candidates; returns true when the sweep has finished.
  bool service(int n) {
    for (int i = 0; i < n && running_; ++i) {
      PlanChoice c;
      // The exhaust fan wastes heat and the air exchange is the machine's to schedule: it is a candidate only when the best the heater alone
      // can do still lets the air rise past the knee of the cost (what idling the elements cannot absorb)
      if (idx_ == NU * NT * NTAU && best_.peak <= OvershootKnee) { running_ = false; best_.valid = best_.cost < 1e29f; break; }
      if (!candidate(idx_, c)) { running_ = false; best_.valid = best_.cost < 1e29f; break; }
      c.cost = evaluate(c);
      c.peak = peak_;
      if (c.cost < best_.cost) best_ = c;
      ++idx_;
    }
    return !running_;
  }
  const PlanChoice &best() const { return best_; }
  // Diagnostics (host tests): the cost of a candidate and the air temperature it predicts for the planner's last start state.
  float replay(const PlanChoice &c, float *taOut) { return evaluate(c, taOut); }
  // Energy-balance tail law: the power that holds the air while the eggs absorb heat, plus a pull toward SP with time constant tau.
  static float tail(const Model &m, float ta, float te, float d, float vent, float sp, float tau, float limit) {
    const float p = m.a * (ta - Tref) + m.k * (ta - te) - d + m.ventPower(ta) * vent + m.cf * (sp - ta) / tau;
    return clampf(p, 0.0f, limit);
  }
 private:
  static constexpr int NU = 10, NT = 6, NTAU = 4, NV = 5;
  // A candidate is the energy-balance law (the exact feedback law of the model) plus a committed deviation du for t1 minutes, then the law
  // again with time constant tau. du = 0 is the pure law; large |du| are the cutback / full-power moves of a cold start or a door.
  // The last candidates open the exhaust fan for a few minutes with the heater idle (what the controller does while it is cooling).
  bool candidate(int i, PlanChoice &c) const {
    static const float DU[NU] = {-100.0f, -60.0f, -30.0f, -15.0f, -6.0f, 0.0f, 6.0f, 15.0f, 30.0f, 100.0f};
    static const float T1[NT] = {0.0f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f};
    static const float TAUK[NTAU] = {2.0f, 3.5f, 7.0f, 15.0f};      // x (delay + lag): SIMC wants tau_c >= theta, i.e. a closed loop no faster than 2 theta
    static const float TV[NV] = {1.0f, 2.0f, 4.0f, 8.0f, 16.0f};
    const int heaterTotal = NU * NT * NTAU;
    const int total = heaterTotal + (ventAllowed_ ? NV * NTAU : 0);
    if (i >= total) return false;
    c = PlanChoice{};
    if (i >= heaterTotal) {
      const int r = i - heaterTotal;
      c.ventMin = TV[r / NTAU]; c.tau = TAUK[r % NTAU] * (m_.dead + m_.tau);
      return true;
    }
    int r = i;
    const int it = r % NTAU; r /= NTAU;
    const int i1 = r % NT; r /= NT;
    const int iu = r;
    c.t1Min = T1[i1]; c.du = DU[iu];
    c.tau = TAUK[it] * (m_.dead + m_.tau);
    const bool pure = c.du == 0.0f, none = c.t1Min <= 0.0f;
    if (pure != none) c.du = -1000.0f;       // (du = 0) == (t1 = 0): the other combinations repeat the pure law; the cost marks them as skipped
    return true;
  }
  float evaluate(const PlanChoice &c, float *taOut = nullptr) {
    peak_ = -99.0f;
    if (c.du < -500.0f || !cmd_) return 1e30f;
    Sim s; s.ta = ta_; s.te = te_; s.hl = hl0_;
    const float dt = static_cast<float>(PlanDtSec);
    const float tNow = static_cast<float>(nowBin_) * BinSec;
    const StepCoef sc = stepCoef(m_, dt);
    float cost = 0.0f;
    for (int j = 0; j < PlanSteps; ++j) {
      const float t = j * dt;
      const float planned = (ventAllowed_ && t < c.ventMin * 60.0f) ? 1.0f : 0.0f;
      const float vent = fmaxf(planned, vs_.at(t + 0.5f * dt));
      const float ventAhead = fmaxf(planned, vs_.at(t + m_.dead));     // what the heater power commanded now has to face when it arrives
      float u;
      if (planned > 0.0f) u = 0.0f;
      else u = clampf(tail(m_, s.ta, s.te, d_, ventAhead, sp_, c.tau, limit_ + 20.0f) + (t < c.t1Min * 60.0f ? c.du : 0.0f), 0.0f, limit_);
      cmd_[j] = static_cast<uint16_t>(u * 100.0f + 0.5f);     // 0.01 %: a hold of a few percent integrated over two hours does not forgive coarser steps
      // heater power delivered `dead` seconds ago: the past from the history, the future from this very plan
      const float td = t + 0.5f * dt - m_.dead;
      float ud;
      if (td < 0.0f) ud = hist_->uAtTime(tNow + td);
      else { int k = static_cast<int>(td / dt); if (k >= j) k = j; ud = 0.01f * cmd_[k]; }
      simStep(m_, sc, ud, d_, w_, vent, s);
      if (taOut) taOut[j] = s.ta;
      const float e = s.ta - sp_;
      if (e > peak_) peak_ = e;
      cost += (e > 0.03f ? 12.0f * e * e : e * e) + (e > OvershootKnee ? 400.0f * (e - OvershootKnee) * (e - OvershootKnee) : 0.0f) + (planned > 0.0f ? 0.0005f : 0.0f);
      if (!finitef(s.ta)) return 1e30f;
    }
    const float eT = s.te - (sp_ + w_ / m_.k);
    return cost + 8.0f * eT * eT;
  }
  Model m_;
  static constexpr float OvershootKnee = 0.35f;
  float peak_ = 0.0f;
  VentSchedule vs_;
  float ta_ = 0, te_ = 0, w_ = 0, d_ = 0, hl0_ = 0, sp_ = 37.5f, limit_ = 100.0f;
  bool ventAllowed_ = false, running_ = false;
  const History *hist_ = nullptr;
  uint32_t nowBin_ = 0;
  int idx_ = 0;
  PlanChoice best_;
  uint16_t *cmd_ = nullptr;
};

// ---------------------------------------------------------------------------------------------------------------------------------------
struct Inputs {
  uint32_t now = 0;          // ms
  float pv = NAN;            // filtered air temperature
  bool sensorOk = false;
  float sp = 37.5f;
  float high = 38.2f;
  float limitPct = 100.0f;   // the effective heater limit
  bool heaterPermit = false; // the heater may actually run
  bool ventOn = false;       // the exhaust fan is running
  bool ventAllowed = false;  // the controller may ask for it
  VentSchedule vent;         // the fan the machine has scheduled
  bool disturbed = false;
};

class Controller {
 public:
  Controller() { reset(StoredModel{}); }
  void setEnabled(bool on) { enabled_ = on; if (!on) state_ = State::Off; }
  bool enabled() const { return enabled_; }
  void reset(const StoredModel &stored) {
    hist_.reset(); ident_.reset(stored);
    stored_ = stored; started_ = false; binStartMs_ = 0; onMs_ = 0; elapsedMs_ = 0; pvSum_ = 0; pvN_ = 0; ventSum_ = 0;
    lastPv_ = NAN; hl_ = 0; request_ = 0; cooling_ = false; state_ = enabled_ ? State::Learning : State::Off;
    healthyBins_ = 0; degStart_ = false; iDeg_ = 0.0f; det_ = 1.0f; osc_ = 0; lastSign_ = 0; ampSince_ = 0.0f; lastCrossMs_ = 0; badBins_ = 0; plan_ = PlanChoice{}; planner_ = Planner{}; planStartBin_ = 0; pendingStartBin_ = -1000;
    readySince_ = 0; model_ = stored.valid ? stored.m : Model{}; modelOk_ = false; sensorGapMs_ = 0; firstHeatBin_ = -1; lastSampleMs_ = 0;
    planOnTop_ = false; trim_ = 0.0f; lastUpdateMs_ = 0; coolUntilMs_ = 0; slopeValid_ = false; slope_ = 0.0f; needMove_ = false; lastPinned_ = false; lastJudgedBin_ = 0;
  }
  // Called every control cycle with the ACTUAL state of the heater SSR (and master) and of the exhaust fan.
  void tick(uint32_t now, bool heaterOn, bool ventOn) {
    if (!started_) { started_ = true; binStartMs_ = now; lastTick_ = now; }
    const uint32_t dtMs = now - lastTick_;
    lastTick_ = now;
    if (dtMs > 0U && dtMs < 2000U) { elapsedMs_ += dtMs; if (heaterOn) onMs_ += dtMs; if (ventOn) ventMs_ += dtMs; }
    if (now - binStartMs_ >= static_cast<uint32_t>(BinSec) * 1000U) closeBin(now);
  }
  // Called on every new filtered sensor sample.
  void sample(uint32_t now, float pv, bool sensorOk) {
    lastSampleMs_ = now;
    if (!sensorOk || !finitef(pv)) { slopeValid_ = false; return; }
    pvSum_ += pv; ++pvN_; lastPv_ = pv;
    // Observed air-temperature slope (K/s), smoothed over about a minute. It needs no model, so it keeps working when the model is wrong.
    if (slopeValid_ && now - slopeMs_ >= 1000U && now - slopeMs_ < 30000U) {
      const float dt = static_cast<float>(now - slopeMs_) * 0.001f;
      slope_ += (clampf((pv - slopePv_) / dt, -0.5f, 0.5f) - slope_) * clampf(dt / 60.0f, 0.0f, 1.0f);
      slopePv_ = pv; slopeMs_ = now;
    } else if (!slopeValid_ || now - slopeMs_ >= 30000U) { slopeValid_ = true; slopePv_ = pv; slopeMs_ = now; slope_ = 0.0f; }
  }
  // Chunked heavy work, one kind per call so a control cycle never carries both: even calls advance the identifier, odd calls the planner.
  // `budget` is the number of identifier evaluations (a planner candidate costs about a third of one, so it gets three times as many).
  void service(const Inputs &in, int budget) {
    if (!enabled_) return;
    (void)in;
    phase_ ^= 1U;
    ident_.bind(scratch_); planner_.bind(reinterpret_cast<uint16_t *>(scratch_));
    if (phase_ == 0U) {
      // the first twenty minutes at full rate (the physics is learned from the warm-up), afterwards a quarter of it
      const int n = hist_.count() < 120U ? budget : (budget > 3 ? budget / 4 : 1);
      for (int i = 0; i < n; ++i) if (!ident_.service(hist_)) break;
      return;
    }
    if (planner_.running() && planner_.service(budget * 3)) {
      if (planner_.best().valid) { plan_ = planner_.best(); planStartBin_ = pendingStartBin_; }
    }
  }
  // Decide state and the power proposal. Call on every new sensor sample (cheap).
  void update(const Inputs &in) {
    if (!enabled_) { state_ = State::Off; request_ = 0; cooling_ = false; return; }
    const uint32_t bins = hist_.count();
    // supervisor ---------------------------------------------------------------------------------------------------------------
    const float rms = ident_.rms();
    const bool excited = excited_();
    const float rmsLimit = state_ == State::Active ? RmsKeep : RmsOk;      // enter at 0.10, stay until 0.20 (the window slides past the warm-up)
    const bool fitOk = bins >= 54U && excited && rms > 0.0f && rms <= rmsLimit && !ident_.atBound();
    const bool storedGood = stored_.valid && bins >= 18U && ident_.recentRms() <= 0.12f;
    modelOk_ = fitOk || (storedGood && !fitBad_());
    if (bins >= 24U && rms > 0.0f && rms <= 0.30f) model_ = ident_.model();   // the best model so far drives the filter and the planner
    uint8_t why = 0;
    if (bins < 24U) why = 1;
    else if (ident_.recentRms() > 0.30f || ident_.recentPeak() > SeverePeak) why = 2;
    else if (!sensorFresh_(in.now)) why = 3;
    else if (finitef(in.pv) && in.pv > in.sp + 0.7f && request_ > 0.5f) why = 4;
    const bool unhealthy = why != 0;
    if (unhealthy) lastWhy_ = why;
    const bool newBin = bins != lastJudgedBin_;     // the verdict advances once per 10 s bin, whatever the sensor rate
    lastJudgedBin_ = bins;
    if (state_ == State::Learning || state_ == State::Degraded) {
      // The model must have been checked against a real move of the heater inside its window (a quiet hold cannot tell one oven from another:
      // plants agree in steady state and differ only in how they answer a change of power), and after a stand-down against a recent one
      if (modelOk_ && !unhealthy && heaterSwing_(bins, static_cast<uint32_t>(HistBins)) >= ProbeSwingPct && (!needMove_ || heaterSwing_(bins, 90U) >= ProbeSwingPct)) { needMove_ = false; state_ = State::Active; readySince_ = bins; badBins_ = 0; trim_ = 0.0f; }
    } else if (state_ == State::Active) {
      if (newBin) {
        // severity x duration: a model that misses by more than 0.6 degC (the structure of the oven just changed) is stood down within a minute
        const bool untrusted = !modelOk_ && bins > 120U;      // the fit is not good enough to keep driving (e.g. a physical parameter at its bound)
        if (untrusted && !unhealthy) lastWhy_ = 5;
        badBins_ = (unhealthy || untrusted) ? badBins_ + (unhealthy && (ident_.recentRms() > SevereMiss || ident_.recentPeak() > SeverePeak) ? BadBinsToFallback : 1U) : 0U;
      }
      if (badBins_ >= BadBinsToFallback) { state_ = State::Degraded; badBins_ = 0; needMove_ = true; degStart_ = true; }
    }
    if (state_ != State::Active && state_ != State::Degraded) { request_ = 0.0f; cooling_ = false; return; }
    // proposal ------------------------------------------------------------------------------------------------------------------
    const float limit = clampf(in.limitPct, 0.0f, 100.0f);
    const float vent = fmaxf(in.ventOn ? 1.0f : 0.0f, in.vent.at(model_.dead));   // the fan the heater power sent now will meet
    float u;
    const float sinceSec = static_cast<float>(static_cast<int64_t>(bins) - planStartBin_) * BinSec;
    const Planner::State st = currentState(in);
    holdPct_ = clampf(model_.a * (in.sp - Tref) + model_.k * (in.sp - st.te) - st.d, 0.0f, limit);     // what holds SP with the eggs where they are
    // slow trim of what the model gets wrong (a biased disturbance or egg temperature): an integral on the air error, frozen when the
    // output is pinned or the error is large (events are the planner's job)
    const float dtSec = lastUpdateMs_ ? clampf(static_cast<float>(in.now - lastUpdateMs_) * 0.001f, 0.0f, 10.0f) : 0.0f;
    lastUpdateMs_ = in.now;
    const float err = in.sp - st.ta;
    const float theta = model_.dead + model_.tau;
    if (state_ == State::Degraded) { degradedStep_(in, st, err, theta, limit, dtSec); return; }
    // Integral gain of the SIMC rule: Ti = tau1 with tau1 = cf / (a + k) the air's own time constant, Kc = cf / tau_c with tau_c >= 2 theta
    // -> ki = Kc / Ti = (a + k) / tau_c; the planner's own pull uses tau_c = 2 theta at least, so ki = (a + k) / (4 theta) keeps a 2x margin.
    const float ki = (model_.a + model_.k) / (4.0f * theta);
    if (std::fabs(err) < 0.4f && !lastPinned_) trim_ = clampf(trim_ + ki * err * dtSec, -8.0f, 8.0f);
    // Exhaust fan: it removes kilowatts, so it is a large disturbance of its own. Ask for it only when the air really is above SP and
    // the plan wants it; hold the request until the air is back within 0.03 C of SP. The heater is idle meanwhile.
    // The plan says for how long (heater idle meanwhile); venting never helps while the air is below SP.
    if (!in.ventAllowed || !finitef(in.pv)) cooling_ = false;
    else if (cooling_ && static_cast<int32_t>(in.now - coolUntilMs_) < 0) cooling_ = in.pv >= in.sp;     // once started it runs at least a minute (relay)
    else {
      const bool want = plan_.valid && plan_.ventMin > 0.0f && sinceSec < plan_.ventMin * 60.0f && in.pv >= in.sp;
      if (want && !cooling_) coolUntilMs_ = in.now + 60000U;
      cooling_ = want;
    }
    u = Planner::tail(model_, st.ta, st.te, st.d, cooling_ ? 0.0f : vent, in.sp, plan_.valid ? plan_.tau : 300.0f, limit + 20.0f) + trim_;
    if (plan_.valid && sinceSec < plan_.t1Min * 60.0f) u += plan_.du;
    // guard rails: never ask for heat well above SP; never more than the limit
    if (finitef(in.pv) && in.pv > in.sp + 0.3f) u = 0.0f;
    if (cooling_) u = 0.0f;
    // Model-free overshoot guard: whatever the model believes, heat already on its way keeps raising the air for about delay + lag after the
    // power is cut. If the observed slope carries the air past SP by that time, no more power (the same logic as a cutback / fuzzy overshoot
    // suppression, but it acts on what is measured, so a model that has just become wrong cannot defeat it).
    // (only while the model is missing: when it forecasts well the planner already schedules the cutback, and a linear extrapolation of the
    // slope over-cuts it: the eggs and the loss absorb part of the heat in flight)
    if (slopeValid_ && finitef(in.pv) && (ident_.recentRms() > GuardRms || ident_.recentPeak() > GuardPeak) &&
        in.pv + slope_ * (model_.dead + model_.tau) > in.sp + OvershootKnee) u = 0.0f;
    request_ = clampf(u, 0.0f, limit);
    lastPinned_ = request_ <= 0.0f || request_ >= limit;
  }
  State state() const { return state_; }
  bool active() const { return enabled_ && (state_ == State::Active || state_ == State::Degraded); }       // drives the heater
  bool planning() const { return enabled_ && state_ == State::Active; }
  float request() const { return request_; }
  float holdEstimate() const { return holdPct_; }       // the power the model says holds SP now (for a bumpless hand-back)
  bool coolingRequest() const { return cooling_; }
  Telemetry telemetry() const {
    Telemetry t;
    t.state = state_; t.rms = ident_.rms(); t.innov = ident_.recentRms(); t.te = ident_.endState().te; t.w = ident_.w0(); t.d = ident_.dEffective();
    t.request = request_; t.m = model_; t.windowBins = static_cast<uint16_t>(hist_.count() > HistBins ? HistBins : hist_.count());
    t.cooling = cooling_; t.modelOk = modelOk_;
    return t;
  }
  // What to keep for the next batch (only a model that fitted well).
  bool exportModel(StoredModel &out) const {
    if (!modelOk_ || ident_.rms() <= 0.0f || ident_.rms() > RmsOk || hist_.count() < 120U) return false;
    out.valid = true; out.m = ident_.model(); out.rms = ident_.rms();
    return true;
  }
  bool wantsPlan() const {
    return enabled_ && state_ != State::Degraded && hist_.count() >= 24U && !planner_.running() && static_cast<int64_t>(hist_.count()) - pendingStartBin_ >= PlanDtSec / BinSec;
  }
  void beginPlan(const Inputs &in) {
    planner_.bind(reinterpret_cast<uint16_t *>(scratch_));
    planner_.start(model_, currentState(in), in.sp, clampf(in.limitPct, 0.0f, 100.0f), in.ventAllowed, hist_, hist_.count(), in.vent);
    pendingStartBin_ = static_cast<int64_t>(hist_.count());   // the plan is aged from the moment its sweep started
  }
  const PlanChoice &plan() const { return plan_; }
  Planner &plannerForTest() { return planner_; }
  Identifier &identifierForTest() { return ident_; }
  const Identifier &identifier() const { return ident_; }
  const History &history() const { return hist_; }
  float heaterLag() const { return hl_; }
  uint8_t lastWhy() const { return lastWhy_; }

  static constexpr float GuardRms = 0.10f, GuardPeak = 0.25f;
  static constexpr float SeverePeak = 0.5f;
  static constexpr float ProbeSwingPct = 15.0f;
  static constexpr float SevereMiss = 0.6f;            // a miss this large is stood down at once
  static constexpr uint32_t BadBinsToFallback = 30U;    // 5 minutes of lesser trouble
  static constexpr float RmsOk = 0.10f;
  static constexpr float RmsKeep = 0.20f;
  static constexpr float TrimTauSec = 900.0f;

 private:
  // The state the planner starts from: the moving-horizon estimate while the identifier fits well (it is consistent with the fitted
  // physics), the Kalman filter otherwise. The air temperature is always the measurement.
  Planner::State currentState(const Inputs &in) const {
    Planner::State st;
    const bool mhe = ident_.rms() > 0.0f && ident_.rms() <= 0.12f && hist_.count() >= 24U;
    st.ta = finitef(in.pv) ? in.pv : ident_.endState().ta;
    st.te = clampf(ident_.endState().te, st.ta - 15.0f, st.ta + 3.0f); st.w = ident_.w0(); st.d = ident_.dEffective(); st.hl = ident_.endState().hl;
    (void)mhe;
    return st;
  }
  // Degraded: an own PI that does not trust the model beyond the steady-state gain. Bumpless from the last request, integrator in absolute %
  // (the model's hold power is exactly what is wrong when we get here). The loop gain Kc*K is 0.6 (K = 1/(a+k)); Ti = max(3 (delay + lag), 300 s).
  // Sustained oscillation (gain or delay wrong) detunes it by half each time, quiet time retunes it slowly: the same adaptive-on-oscillation idea
  // as the vendors' controllers. No cut-back on the slope here: with a long transport delay a hard cut is itself the oscillator (relay + delay),
  // so only a far-above-SP safety cut remains (High/Emergency layers sit below).
  void degradedStep_(const Inputs &in, const Planner::State &st, float err, float theta, float limit, float dtSec) {
    cooling_ = false;
    const float K = 1.0f / fmaxf(model_.a + model_.k, 0.3f);
    if (degStart_) {            // bumpless from the last request
      degStart_ = false; det_ = 1.0f; osc_ = 0; lastSign_ = 0; ampSince_ = 0.0f;
      iDeg_ = request_ - 0.6f * det_ / K * err;
    }
    const float Kc = 0.6f * det_ / K, Ti = fmaxf(3.0f * theta, 300.0f);
    // oscillation monitor: error sign flips with a real amplitude
    const int sign = err > 0.0f ? 1 : -1;
    if (std::fabs(err) > ampSince_) ampSince_ = std::fabs(err);
    if (sign != lastSign_ && lastSign_ != 0) {
      if (ampSince_ > 0.25f) { if (in.now - lastCrossMs_ > 1200000U) osc_ = 0; ++osc_; lastCrossMs_ = in.now; if (osc_ >= 3) { det_ = fmaxf(0.15f, det_ * 0.5f); osc_ = 0; } }
      ampSince_ = 0.0f;
    }
    lastSign_ = sign;
    if (det_ < 1.0f) det_ = fminf(1.0f, det_ + 0.002f * dtSec / 60.0f);
    float u = Kc * err + iDeg_;
    const bool pinned = (u >= limit && err > 0.0f) || (u <= 0.0f && err < 0.0f);
    if (!pinned) iDeg_ = clampf(iDeg_ + Kc / Ti * err * dtSec, 0.0f, limit);
    u = Kc * err + iDeg_;
    if (finitef(in.pv) && in.pv > in.sp + 0.6f) u = 0.0f;
    request_ = clampf(u, 0.0f, limit);
    lastPinned_ = request_ <= 0.0f || request_ >= limit;
    (void)st;
  }
  void closeBin(uint32_t now) {
    const float frac = elapsedMs_ > 0U ? static_cast<float>(onMs_) / static_cast<float>(elapsedMs_) : 0.0f;
    const float vfrac = elapsedMs_ > 0U ? static_cast<float>(ventMs_) / static_cast<float>(elapsedMs_) : 0.0f;
    const float uPct = 100.0f * frac;
    const float pv = pvN_ > 0 ? pvSum_ / static_cast<float>(pvN_) : lastPv_;
    onMs_ = 0; elapsedMs_ = 0; ventMs_ = 0; pvSum_ = 0.0f; pvN_ = 0;
    binStartMs_ = now;
    if (!finitef(pv)) {   // no valid air temperature yet: the leading bins are dropped, a gap later repeats the last value
      if (hist_.count()) hist_.push(uPct, hist_.y(hist_.count() - 1), vfrac);
      return;
    }
    const int64_t bin = hist_.count();
    hist_.push(uPct, pv, vfrac);
    if (firstHeatBin_ < 0 && uPct > 5.0f) { firstHeatBin_ = bin; ident_.setOnset(bin); }
  }
  float heaterSwing_(uint32_t bins, uint32_t span) const {       // largest minus smallest heater duty over the last `span` bins
    float lo = 100.0f, hi = 0.0f;
    const uint32_t from = bins > span ? bins - span : 0U;
    if (from == 0U) lo = 0.0f;       // the batch began from rest: switching the heater on is itself the move
    for (uint32_t b = from; b < bins; ++b) { const float u = hist_.u(b); if (u < lo) lo = u; if (u > hi) hi = u; }
    return hi > lo ? hi - lo : 0.0f;
  }
  bool excited_() const {
    if (firstHeatBin_ < 0) return false;
    const uint32_t bins = hist_.count();
    return bins >= static_cast<uint32_t>(firstHeatBin_) + 36U;       // at least 6 minutes of data after the heater first ran
  }
  bool fitBad_() const { return ident_.rms() > 0.18f && hist_.count() > 90U; }
  bool sensorFresh_(uint32_t now) const { return now - lastSampleMs_ < 30000U; }

  bool enabled_ = true;
  State state_ = State::Off;
  History hist_;
  Identifier ident_;
  Planner planner_;
  alignas(4) int8_t scratch_[CapBins] = {};       // one working buffer for whichever of the two is evaluating (they alternate, never overlap)
  PlanChoice plan_;
  StoredModel stored_;
  Model model_;
  bool started_ = false, modelOk_ = false, planOnTop_ = false;
  uint32_t binStartMs_ = 0, lastTick_ = 0, onMs_ = 0, ventMs_ = 0, elapsedMs_ = 0, lastSampleMs_ = 0;
  uint32_t healthyBins_ = 0, badBins_ = 0, readySince_ = 0, sensorGapMs_ = 0;
  int64_t firstHeatBin_ = -1, planStartBin_ = 0, pendingStartBin_ = -1000;
  float holdPct_ = 0, pvSum_ = 0, lastPv_ = NAN, ventSum_ = 0, hl_ = 0, request_ = 0, trim_ = 0;
  uint32_t lastUpdateMs_ = 0, coolUntilMs_ = 0;
  bool lastPinned_ = false, slopeValid_ = false, needMove_ = false, degStart_ = false;
  float iDeg_ = 0.0f, det_ = 1.0f, ampSince_ = 0.0f;
  int osc_ = 0, lastSign_ = 0;
  uint32_t lastCrossMs_ = 0;
  uint32_t slopeMs_ = 0;
  float slope_ = 0.0f, slopePv_ = 0.0f;
  static constexpr float OvershootKnee = 0.35f;
  uint8_t lastWhy_ = 0, phase_ = 0;
  uint32_t lastJudgedBin_ = 0;
  int pvN_ = 0;
  bool cooling_ = false;
};

}  // namespace MayapPredictive
