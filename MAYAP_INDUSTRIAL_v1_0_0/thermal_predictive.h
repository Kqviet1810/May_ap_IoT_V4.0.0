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
constexpr int NP = 10;                  // identified parameters

struct Model {            // % units, seconds
  float cf = 2500.0f;     // air + steel + trays: % * s / degC
  float ce = 9000.0f;     // eggs
  float k = 6.0f;         // air <-> eggs coupling, % / degC
  float a = 1.2f;         // loss to ambient, % / degC
  float dead = 60.0f;     // transport delay of the heater to the sensor
  float tau = 40.0f;      // heater / element lag
  float vp = 10.0f;       // power removed while the exhaust fan runs, %
};

// Wide priors (any oven of this class) and hard bounds. Index order is the identifier's parameter vector.
//   0 ln cf, 1 ln ce, 2 ln k, 3 ln a, 4 ln dead, 5 ln tau, 6 vp, 7 d0, 8 off (Ta0 - Te0), 9 w0
struct Prior {
  static constexpr float mu[NP] = {7.824f, 9.105f, 1.792f, 0.0f, 4.094f, 3.689f, 10.0f, -10.0f, 0.0f, 0.0f};
  static constexpr float sd[NP] = {0.9f, 0.9f, 0.9f, 0.6f, 0.7f, 0.7f, 8.0f, 8.0f, 1.5f, 3.0f};
  static constexpr float lo[NP] = {6.0f, 7.6f, 0.2f, -3.0f, 1.6f, 2.3f, 0.0f, -60.0f, -1.5f, -2.0f};
  static constexpr float hi[NP] = {9.6f, 11.1f, 3.4f, 1.8f, 5.5f, 4.8f, 45.0f, 15.0f, 14.0f, 12.0f};
};
constexpr float Prior::mu[NP];
constexpr float Prior::sd[NP];
constexpr float Prior::lo[NP];
constexpr float Prior::hi[NP];

enum class State : uint8_t { Off = 0, Learning = 1, Active = 2, Fallback = 3 };

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
class History {
 public:
  void reset() { n_ = 0; std::memset(u_, 0, sizeof(u_)); std::memset(y_, 0, sizeof(y_)); std::memset(v_, 0, sizeof(v_)); }
  void push(float uPct, float pv, float vent) {
    const uint32_t i = n_ % CapBins;
    u_[i] = static_cast<uint8_t>(clampf(uPct, 0.0f, 100.0f) * 2.0f + 0.5f);
    y_[i] = static_cast<int16_t>((clampf(pv, 0.0f, 80.0f) - 20.0f) * 100.0f + 0.5f);
    v_[i] = static_cast<uint8_t>(clampf(vent, 0.0f, 1.0f) * 255.0f + 0.5f);
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
    return v_[static_cast<uint32_t>(bin) % CapBins] * (1.0f / 255.0f);
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
  uint8_t v_[CapBins] = {};
};

// ---------------------------------------------------------------------------------------------------------------------------------------
// One integration step of the two-node model (used by the Kalman filter, the identifier and the planner).
struct Sim {
  float ta, te, hl;
};
inline void simStep(const Model &m, float dt, float uDelayed, float d, float w, float vent, Sim &s) {
  // substeps keep the explicit scheme stable for stiff (small cf, large k) models
  const float stiff = (m.a + m.k) * dt / m.cf;
  const int sub = stiff > 0.8f ? static_cast<int>(stiff / 0.8f) + 1 : 1;
  const float h = dt / static_cast<float>(sub);
  const float ex = 1.0f - std::exp(-h / m.tau);
  for (int i = 0; i < sub; ++i) {
    s.hl += ex * (uDelayed - s.hl);
    const float q = m.k * (s.ta - s.te);
    s.ta += (h / m.cf) * (s.hl - m.a * (s.ta - Tref) - q + d - m.vp * vent);
    s.te += (h / m.ce) * (q + w);
  }
}

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
      cost_ = evaluate(x_, h, &rms_, &end_, offTr_, &recent_, &lastErr);
      // a sudden miss of the last bin after a quiet stretch is an event, not a worse model
      if (std::fabs(lastErr) > 0.6f && quiet_ >= 6U && !isEvent(n - 1U)) { addEvent(n - 1U); dirty_ = true; quiet_ = 0; }
      else if (std::fabs(lastErr) < 0.3f) { if (quiet_ < 100U) ++quiet_; }
      else quiet_ = 0;
      return true;
    }
    float cand[NP];
    std::memcpy(cand, x_, sizeof(cand));
    int j = coord_;
    coord_ = (coord_ + 1) % (NP + 3);
    // Poor data (hold, no events) cannot teach the physics: only the slow nuisances (vent, d, off, w) may move
    if (!rich_ && !frozen_) { if (j < NP) j = 6 + (j % 4); else j = 6 + (j % 4); }
    if (j < NP) {
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
    const float c = evaluate(cand, h, &rms, &endCand, offScratch_);
    ++evals_;
    if (c < cost_) {
      std::memcpy(x_, cand, sizeof(x_)); cost_ = c; rms_ = rms; ++accepted_; end_ = endCand;
      std::memcpy(offTr_, offScratch_, sizeof(offTr_));
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
  float recentRms() const { return recent_; }     // residual over the last two minutes (a model that stopped fitting shows here first)
  uint32_t evals() const { return evals_; }
  uint32_t accepted() const { return accepted_; }
  bool atBound() const {
    for (int i = 0; i < NP; ++i) {
      const float span = Prior::hi[i] - Prior::lo[i];
      if (i < 7 && (x_[i] - Prior::lo[i] < 0.02f * span || Prior::hi[i] - x_[i] < 0.02f * span)) return true;
    }
    return false;
  }
  Model model() const { return toModel(x_); }
  // Simulated state at the end of the window under the incumbent (the moving-horizon estimate of the hidden egg temperature).
  const Sim &endState() const { return end_; }
  float d0() const { return x_[7]; }
  float off0() const { return x_[8]; }
  float w0() const { return x_[9]; }
  static Model toModel(const float *x) {
    Model m;
    m.cf = std::exp(x[0]); m.ce = std::exp(x[1]); m.k = std::exp(x[2]); m.a = std::exp(x[3]);
    m.dead = std::exp(x[4]); m.tau = std::exp(x[5]); m.vp = x[6];
    return m;
  }
  // Residual RMS of the model `m` (with the given nuisance values) on the window; used by tests and by the supervisor.
  float score(const float *x, const History &h) const { float r = 0; evaluate(x, h, &r); return r; }
  const float *vector() const { return x_; }

 private:
  uint32_t nextRand() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return rng_; }
  float uniform() { return static_cast<float>(nextRand() >> 8) * (1.0f / 16777216.0f); }
  static void clampVector(float *x) { for (int i = 0; i < NP; ++i) x[i] = clampf(x[i], Prior::lo[i], Prior::hi[i]); }
  // When the window has slid, the eggs' temperature at its first bin is what the previous incumbent simulated for that bin (the prior of
  // `off` is re-centred there); at the very start of a batch the eggs are assumed to be at the air temperature.
  void anchor(const History &h) {
    const uint32_t s = h.windowStart();
    if (s == 0U) return;
    const float a = 0.1f * offTr_[s % CapBins];
    mu_[8] = a; sd_[8] = 0.7f;
    x_[8] = clampf(a, Prior::lo[8], Prior::hi[8]);
  }
  float evaluate(const float *x, const History &h, float *rmsOut, Sim *endOut = nullptr, int8_t *trace = nullptr, float *recentOut = nullptr, float *lastErrOut = nullptr) const {
    const Model m = toModel(x);
    const uint32_t n = h.count(), s = h.windowStart();
    const float d0 = x[7], w0 = x[9];
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
    float chi = 0.0f, chiRecent = 0.0f;
    const float inv = 1.0f / (0.07f * 0.07f);
    uint32_t cnt = 0, cntRecent = 0;
    for (uint32_t b = s; b < n; ++b) {
      const float tMid = (static_cast<float>(b) + 0.5f) * BinSec;
      const float ud = h.uAtTime(tMid - m.dead);
      simStep(m, static_cast<float>(BinSec), ud, d0, w0, h.v(b), sim);
      const float e = sim.ta - h.y(b);
      if (lastErrOut && b + 1U == n) *lastErrOut = e;
      if (isEvent(b)) {      // an air-side step the model cannot explain (door, a glitch): re-synchronise the air to the measurement, no scoring
        sim.ta = h.y(b);
        if (trace) trace[b % CapBins] = static_cast<int8_t>(clampf((sim.ta - sim.te) * 10.0f, -127.0f, 127.0f));
        continue;
      }
      if (trace) trace[b % CapBins] = static_cast<int8_t>(clampf((sim.ta - sim.te) * 10.0f, -127.0f, 127.0f));
      if (b > s) { chi += e * e * inv; ++cnt; }
      { const float over = sim.te - sim.ta - 3.0f; if (over > 0.0f) chi += over * over * 400.0f; }   // the eggs are not hotter than the air by degrees
      if (recentOut && b + RecentBins >= n) { chiRecent += e * e; ++cntRecent; }
      if (!finitef(sim.ta) || sim.ta > 80.0f || sim.ta < -20.0f) { *rmsOut = 99.0f; return 1e30f; }
    }
    float prior = 0.0f;
    for (int i = 0; i < NP; ++i) { const float z = (x[i] - mu_[i]) / sd_[i]; prior += z * z; }
    *rmsOut = cnt ? std::sqrt(chi / inv / static_cast<float>(cnt)) : 0.0f;
    if (endOut) *endOut = sim;
    if (recentOut) *recentOut = cntRecent ? std::sqrt(chiRecent / static_cast<float>(cntRecent)) : 0.0f;
    return chi + prior;
  }
  float x_[NP], mu_[NP], sd_[NP], step_[NP];
  int8_t offTr_[CapBins] = {};      // (Ta - Te) of the incumbent per bin, 0.1 C: the arrival state of the next window
  int8_t offScratch_[CapBins] = {};
  static constexpr uint32_t RecentBins = 12;
  static constexpr int MaxEvents = 6;
  uint32_t events_[MaxEvents] = {};
  int nEvents_ = 0;
  uint32_t quiet_ = 0;
  float cost_ = -1.0f, rms_ = 0.0f, recent_ = 0.0f;
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
struct PlanChoice {
  float u1 = 0.0f;      // % for the first T1 minutes
  float t1Min = 0.0f;
  float tau = 300.0f;   // tail law time constant, s
  float ventMin = 0.0f; // exhaust fan on for this many minutes
  float cost = 1e30f;
  bool valid = false;
};

class Planner {
 public:
  struct State { float ta, te, w, d, hl; };
  void start(const Model &m, const State &st, float sp, float limitPct, bool ventAllowed, const History &h, uint32_t nowBin) {
    m_ = m; ta_ = st.ta; te_ = st.te; w_ = st.w; d_ = st.d; hl0_ = st.hl; sp_ = sp;
    limit_ = limitPct; ventAllowed_ = ventAllowed; hist_ = &h; nowBin_ = nowBin;
    idx_ = 0; best_ = PlanChoice{}; running_ = true;
  }
  bool running() const { return running_; }
  // Evaluate up to `n` candidates; returns true when the sweep has finished.
  bool service(int n) {
    for (int i = 0; i < n && running_; ++i) {
      PlanChoice c;
      if (!candidate(idx_, c)) { running_ = false; best_.valid = best_.cost < 1e29f; break; }
      c.cost = evaluate(c);
      if (c.cost < best_.cost) best_ = c;
      ++idx_;
    }
    return !running_;
  }
  const PlanChoice &best() const { return best_; }
  // Energy-balance tail law: the power that holds the air while the eggs absorb heat, plus a pull toward SP with time constant tau.
  static float tail(const Model &m, float ta, float te, float d, float vent, float sp, float tau, float limit) {
    const float p = m.a * (ta - Tref) + m.k * (ta - te) - d + m.vp * vent + m.cf * (sp - ta) / tau;
    return clampf(p, 0.0f, limit);
  }
 private:
  static constexpr int NU = 8, NT = 6, NTAU = 4, NV = 3;
  bool candidate(int i, PlanChoice &c) const {
    static const float U1[NU] = {0.0f, 10.0f, 20.0f, 30.0f, 45.0f, 60.0f, 80.0f, 100.0f};
    static const float T1[NT] = {0.0f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f};
    static const float TAUK[NTAU] = {2.0f, 3.5f, 7.0f, 15.0f};      // x (delay + lag): SIMC wants tau_c >= theta, i.e. a closed loop no faster than 2 theta
    static const float TV[NV] = {0.0f, 10.0f, 30.0f};
    const int nv = ventAllowed_ ? NV : 1;
    const int total = NU * NT * NTAU * nv;
    if (i >= total) return false;
    int r = i;
    const int iv = r % nv; r /= nv;
    const int it = r % NTAU; r /= NTAU;
    const int i1 = r % NT; r /= NT;
    const int iu = r;
    c.t1Min = T1[i1]; c.u1 = c.t1Min <= 0.0f ? 0.0f : fminf(U1[iu], limit_);
    if (c.t1Min <= 0.0f && iu != 0) { c.cost = 1e30f; c.u1 = -1.0f; }   // duplicates of the pure tail law are skipped by the cost below
    c.tau = TAUK[it] * (m_.dead + m_.tau); c.ventMin = TV[iv];
    return true;
  }
  float evaluate(const PlanChoice &c) {
    if (c.u1 < 0.0f) return 1e30f;
    Sim s; s.ta = ta_; s.te = te_; s.hl = hl0_;
    const float dt = static_cast<float>(PlanDtSec);
    const float tNow = static_cast<float>(nowBin_) * BinSec;
    float cost = 0.0f;
    for (int j = 0; j < PlanSteps; ++j) {
      const float t = j * dt;
      const float vent = (ventAllowed_ && t < c.ventMin * 60.0f) ? 1.0f : 0.0f;
      float u;
      if (t < c.t1Min * 60.0f) u = c.u1;
      else u = tail(m_, s.ta, s.te, d_, vent, sp_, c.tau, limit_);
      cmd_[j] = u;
      // heater power delivered `dead` seconds ago: the past from the history, the future from this very plan
      const float td = t + 0.5f * dt - m_.dead;
      float ud;
      if (td < 0.0f) ud = hist_->uAtTime(tNow + td);
      else { int k = static_cast<int>(td / dt); if (k >= j) k = j; ud = cmd_[k]; }
      simStep(m_, dt, ud, d_, w_, vent, s);
      const float e = s.ta - sp_;
      cost += (e > 0.03f ? 12.0f * e * e : e * e) + (e > 0.35f ? 400.0f * (e - 0.35f) * (e - 0.35f) : 0.0f) + (vent > 0.0f ? 0.0005f : 0.0f);
      if (!finitef(s.ta)) return 1e30f;
    }
    const float eT = s.te - (sp_ + w_ / m_.k);
    return cost + 8.0f * eT * eT;
  }
  Model m_;
  float ta_ = 0, te_ = 0, w_ = 0, d_ = 0, hl0_ = 0, sp_ = 37.5f, limit_ = 100.0f;
  bool ventAllowed_ = false, running_ = false;
  const History *hist_ = nullptr;
  uint32_t nowBin_ = 0;
  int idx_ = 0;
  PlanChoice best_;
  float cmd_[PlanSteps];
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
    fallbackUntilBin_ = 0; healthyBins_ = 0; badBins_ = 0; plan_ = PlanChoice{}; planner_ = Planner{}; planStartBin_ = 0; pendingStartBin_ = -1000;
    readySince_ = 0; model_ = stored.valid ? stored.m : Model{}; modelOk_ = false; sensorGapMs_ = 0; firstHeatBin_ = -1; lastSampleMs_ = 0;
    planOnTop_ = false; trim_ = 0.0f; lastUpdateMs_ = 0; lastPinned_ = false;
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
    if (!sensorOk || !finitef(pv)) return;
    pvSum_ += pv; ++pvN_; lastPv_ = pv;
  }
  // Chunked heavy work, one kind per call so a control cycle never carries both: even calls advance the identifier, odd calls the planner.
  // `budget` is the number of identifier evaluations (a planner candidate costs about a third of one, so it gets three times as many).
  void service(const Inputs &in, int budget) {
    if (!enabled_) return;
    (void)in;
    phase_ ^= 1U;
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
    else if (ident_.recentRms() > 0.30f) why = 2;
    else if (!sensorFresh_(in.now)) why = 3;
    else if (finitef(in.pv) && in.pv > in.sp + 0.7f && request_ > 0.5f) why = 4;
    const bool unhealthy = why != 0;
    if (unhealthy) lastWhy_ = why;
    if (state_ == State::Learning) {
      if (modelOk_ && !unhealthy) { state_ = State::Active; readySince_ = bins; badBins_ = 0; trim_ = 0.0f; }
    } else if (state_ == State::Active) {
      badBins_ = unhealthy ? badBins_ + 1U : 0U;
      if (!modelOk_ && bins > 120U) { badBins_ += 1U; lastWhy_ = 5; }
      if (badBins_ >= 30U) { state_ = State::Fallback; fallbackUntilBin_ = bins + 120U; badBins_ = 0; }
    } else if (state_ == State::Fallback) {
      if (bins >= fallbackUntilBin_) state_ = State::Learning;
    }
    if (state_ != State::Active) { request_ = 0.0f; cooling_ = false; return; }
    // proposal ------------------------------------------------------------------------------------------------------------------
    const float limit = clampf(in.limitPct, 0.0f, 100.0f);
    const float vent = in.ventOn ? 1.0f : 0.0f;
    float u;
    const float sinceSec = static_cast<float>(static_cast<int64_t>(bins) - planStartBin_) * BinSec;
    const Planner::State st = currentState(in);
    // slow trim of what the model gets wrong (a biased disturbance or egg temperature): an integral on the air error, frozen when the
    // output is pinned or the error is large (events are the planner's job)
    const float dtSec = lastUpdateMs_ ? clampf(static_cast<float>(in.now - lastUpdateMs_) * 0.001f, 0.0f, 10.0f) : 0.0f;
    lastUpdateMs_ = in.now;
    const float err = in.sp - st.ta;
    const float theta = model_.dead + model_.tau;
    const float ki = model_.cf / (16.0f * theta * theta);          // SIMC: Kc = cf / (2 theta), Ti = 8 theta
    if (std::fabs(err) < 0.4f && !lastPinned_) trim_ = clampf(trim_ + ki * err * dtSec, -8.0f, 8.0f);
    // Exhaust fan: it removes kilowatts, so it is a large disturbance of its own. Ask for it only when the air really is above SP and
    // the plan wants it; hold the request until the air is back within 0.03 C of SP. The heater is idle meanwhile.
    if (!in.ventAllowed || !finitef(in.pv)) cooling_ = false;
    else if (cooling_) cooling_ = in.pv > in.sp + CoolOffC;
    else cooling_ = plan_.valid && plan_.ventMin > 0.0f && in.pv >= in.sp + CoolOnC;
    if (plan_.valid && sinceSec < plan_.t1Min * 60.0f) u = fminf(plan_.u1, limit);
    else u = Planner::tail(model_, st.ta, st.te, st.d, cooling_ ? 0.0f : vent, in.sp, plan_.valid ? plan_.tau : 300.0f, limit + 20.0f) + trim_;
    // guard rails: never ask for heat well above SP; never more than the limit
    if (finitef(in.pv) && in.pv > in.sp + 0.3f) u = 0.0f;
    if (cooling_) u = 0.0f;
    request_ = clampf(u, 0.0f, limit);
    lastPinned_ = request_ <= 0.0f || request_ >= limit;
  }
  State state() const { return state_; }
  bool active() const { return enabled_ && state_ == State::Active; }
  float request() const { return request_; }
  bool coolingRequest() const { return cooling_; }
  Telemetry telemetry() const {
    Telemetry t;
    t.state = state_; t.rms = ident_.rms(); t.innov = ident_.recentRms(); t.te = ident_.endState().te; t.w = ident_.w0(); t.d = ident_.d0();
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
    return enabled_ && hist_.count() >= 24U && !planner_.running() && static_cast<int64_t>(hist_.count()) - pendingStartBin_ >= PlanDtSec / BinSec;
  }
  void beginPlan(const Inputs &in) {
    planner_.start(model_, currentState(in), in.sp, clampf(in.limitPct, 0.0f, 100.0f), in.ventAllowed, hist_, hist_.count());
    pendingStartBin_ = static_cast<int64_t>(hist_.count());   // the plan is aged from the moment its sweep started
  }
  const PlanChoice &plan() const { return plan_; }
  Identifier &identifierForTest() { return ident_; }
  const Identifier &identifier() const { return ident_; }
  const History &history() const { return hist_; }
  float heaterLag() const { return hl_; }
  uint8_t lastWhy() const { return lastWhy_; }

  static constexpr float RmsOk = 0.10f;
  static constexpr float RmsKeep = 0.20f;
  static constexpr float TrimTauSec = 900.0f;
  static constexpr float CoolOnC = 0.50f, CoolOffC = 0.10f;

 private:
  // The state the planner starts from: the moving-horizon estimate while the identifier fits well (it is consistent with the fitted
  // physics), the Kalman filter otherwise. The air temperature is always the measurement.
  Planner::State currentState(const Inputs &in) const {
    Planner::State st;
    const bool mhe = ident_.rms() > 0.0f && ident_.rms() <= 0.12f && hist_.count() >= 24U;
    st.ta = finitef(in.pv) ? in.pv : ident_.endState().ta;
    st.te = clampf(ident_.endState().te, st.ta - 15.0f, st.ta + 3.0f); st.w = ident_.w0(); st.d = ident_.d0(); st.hl = ident_.endState().hl;
    (void)mhe;
    return st;
  }
  void closeBin(uint32_t now) {
    const float frac = elapsedMs_ > 0U ? static_cast<float>(onMs_) / static_cast<float>(elapsedMs_) : 0.0f;
    const float vfrac = elapsedMs_ > 0U ? static_cast<float>(ventMs_) / static_cast<float>(elapsedMs_) : 0.0f;
    const float uPct = 100.0f * frac;
    const float pv = pvN_ > 0 ? pvSum_ / static_cast<float>(pvN_) : lastPv_;
    onMs_ = 0; elapsedMs_ = 0; ventMs_ = 0; pvSum_ = 0.0f; pvN_ = 0;
    binStartMs_ = now;
    if (!finitef(pv)) { hist_.push(uPct, hist_.count() ? hist_.y(hist_.count() - 1) : 0.0f, vfrac); return; }
    const int64_t bin = hist_.count();
    hist_.push(uPct, pv, vfrac);
    if (firstHeatBin_ < 0 && uPct > 5.0f) { firstHeatBin_ = bin; ident_.setOnset(bin); }
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
  PlanChoice plan_;
  StoredModel stored_;
  Model model_;
  bool started_ = false, modelOk_ = false, planOnTop_ = false;
  uint32_t binStartMs_ = 0, lastTick_ = 0, onMs_ = 0, ventMs_ = 0, elapsedMs_ = 0, lastSampleMs_ = 0;
  uint32_t fallbackUntilBin_ = 0, healthyBins_ = 0, badBins_ = 0, readySince_ = 0, sensorGapMs_ = 0;
  int64_t firstHeatBin_ = -1, planStartBin_ = 0, pendingStartBin_ = -1000;
  float pvSum_ = 0, lastPv_ = NAN, ventSum_ = 0, hl_ = 0, request_ = 0, trim_ = 0;
  uint32_t lastUpdateMs_ = 0;
  bool lastPinned_ = false;
  uint8_t lastWhy_ = 0, phase_ = 0;
  int pvN_ = 0;
  bool cooling_ = false;
};

}  // namespace MayapPredictive
