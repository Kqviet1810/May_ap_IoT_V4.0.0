#pragma once
// =========================================================================================
// Thermal V3 - a measured stopping distance, a proportional band as wide as the oven coasts, and a hold level taken from an energy balance.
// Pure C++11 (no Arduino types), header-only.  Nothing in here predicts the PV: every number is the PV, its slope over 90 s,
// or the power this block itself commanded.  What the block believes about the oven is four numbers (struct Model):
//   k      [degC/s per %]  gain of the heater          measured once by a gentle probe pulse:  (slope(P) - slope(0)) / P
//   delay  [s]             age of the power that makes the slope the air shows now     measured at the same pulse (time to the first answer)
//   Tc     [s]             coast time                  measured at every cut of the power:  (rise after the cut) / (slope at the cut)
//   hold   [%]             power that holds the air    -slope(0)/k at the start, then an energy balance:  power behind the slope - slope / k
// Law (u in %):
//   below SP   u = hold + (SP - PV - margin) / (k * Tc) * BAND        the power that, held, lands the air on SP
//   above SP   u = hold + DOWN_GAIN * ...                              and nothing at all while it is still climbing
//   stop rule  the slope times Tc is how far the air still goes; once that reaches the room left under SP the power is faded out
// Start-up (cold, no model yet, 2 degC or more under SP):  look 90 s with the heater off -> probe at PROBE_PCT -> cut -> measure the coast -> law.
// The model lives in RAM for the life of the controller (the machine never resets it); exportModel()/importModel() are for a later NVS record.
// The independent layers (High, Emergency, E115, vent line) are outside this block and can only lower or block its output.
// =========================================================================================
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace thermalv3 {

// ---- measured-model priors (conservative: a fast oven's gain, a long coast) ----
constexpr float K_PRIOR = 6.0e-4f;       // degC/s per %
constexpr float K_MIN = 8.0e-5f, K_MAX = 9.0e-4f;
constexpr float TC_PRIOR_S = 240.0f, TC_MIN_S = 60.0f, TC_MAX_S = 450.0f;
constexpr float D_PRIOR = 6.0f, D_MAX = 60.0f;       // a low prior: a hold level that is too high overshoots (a lossless oven cannot cool), too low only climbs slower
constexpr float DELAY_PRIOR_S = 150.0f, DELAY_MIN_S = 20.0f, DELAY_MAX_S = 320.0f;   // age of the power that makes the slope the air shows now
constexpr float RESP_C = 0.2f;           // the air has answered the probe when it is this far above its own baseline
// ---- start-up ----
constexpr float LOOK_S = 90.0f;          // heater off: baseline slope
constexpr float PROBE_PCT = 25.0f;       // gentle pulse, safe for the fastest oven of the family
constexpr float PROBE_ROOM_C = 2.0f;     // a probe needs this much room under SP
constexpr float PROBE_RISE_C = 0.6f;     // the pulse ends when the air rose this much above its baseline...
constexpr float PROBE_MAX_S = 420.0f;    // ...or after this long
constexpr float COAST_MAX_S = 420.0f;
// ---- law ----
constexpr float BAND = 0.8f;             // proportional gain = BAND / (k * Tc)
constexpr float KAPPA = 1.25f;           // stopping distance is priced this much long
constexpr float MARGIN_C = 0.05f;        // the air is expected to end this far under SP
constexpr float DOWN_GAIN = 3.0f;        // above SP the gain is this much larger
constexpr float TAUH_FACTOR = 3.0f;      // the hold level follows the energy balance with this many coast times
constexpr float HOLD_SLOPE_MKS = 6.0f;   // ...only while the air moves slower than this (the balance is exact when it stands still)
constexpr float HOLD_STEADY_PCT = 12.0f; // ...and the power has not moved by more than this since the power behind the slope was sent
constexpr float HOLD_BAND_C = 1.5f;      // ...and is this close to SP
constexpr float FULL_C = 1.2f;           // (telemetry) a climb further than this under SP is reported as Climb
constexpr float FLOOR_FRAC = 0.5f;       // the stop rule fades the power down to this share of the hold level
constexpr float SLOPE_FLOOR_MKS = 0.8f;  // a 90 s slope under this (or under 3 standard errors) is the sensor's staircase, not a climb
constexpr float R1_MIN_MKS = 3.0f;       // the stop rule acts only on a climb faster than this (0.18 degC/min); a slower one is the proportional band's job
constexpr uint8_t RING_N = 18;           // 10 s samples (180 s)

enum State : uint8_t {          // numbering shared with the Web (PID Monitor): 0 idle, 1 climb, 2 track, 3 frozen
  StIdle = 0,    // not used while live
  StClimb = 1,   // start-up (look / probe / coast) or a climb far under SP
  StTrack = 2,   // the law is running near SP
  StFrozen = 3   // no permit, bad sensor, or the vent line holds the heater
};

struct Input {
  float pv = NAN;                // control temperature [degC]
  float sp = 37.5f;              // target [degC]
  float dQ = 0.1f;               // sensor step [degC]
  float capPct = 100.0f;         // configured heater ceiling [%]
  float deliveredPct = 0.0f;     // (unused: the block keeps its own account of what it commanded)
  float dtS = 0.1f;              // step interval [s]
  bool newSample = false;        // (unused: the PV is read on a 10 s grid)
  bool permit = false;           // all protections green
  bool shadow = false;           // the vent line holds the heater: keep the history alive, command 0
  bool ventFan = false;          // (unused)
  float ventOnInS = 1.0e6f;      // (unused)
  float ventOffInS = 1.0e6f;     // (unused)
  float headroomC = 0.7f;        // (unused: High / Emergency are independent layers)
  float alarmHeadroomC = 0.7f;   // (unused)
  float e115Frac = 0.0f;         // (unused)
};

struct Output {
  float powerPct = 0.0f;
  uint8_t state = StIdle;
  bool powerLimited = false;
  uint8_t trustQ = 15;           // telemetry only: 15 = the oven's gain was measured, 8 = priors
};

struct Model {                   // what survives (RAM now; an NVS record later)
  float k = K_PRIOR, tc = TC_PRIOR_S, hold = D_PRIOR, delay = DELAY_PRIOR_S;
  bool known = false;
};

class Controller {
 public:
  Controller() { reset(); }

  void reset() {
    model_ = Model();
    resetRun();
  }

  bool importModel(const Model &m) {
    if (!(m.k >= K_MIN && m.k <= K_MAX && m.tc >= TC_MIN_S && m.tc <= TC_MAX_S && m.hold >= 0.0f && m.hold <= D_MAX && m.delay >= DELAY_MIN_S && m.delay <= DELAY_MAX_S)) return false;
    model_ = m; model_.known = true; resetRun();
    return true;
  }
  Model exportModel() const { return model_; }

  Output step(const Input &in) {
    Output out;
    const float dt = !std::isfinite(in.dtS) ? 0.1f : (in.dtS < 0.001f ? 0.001f : (in.dtS > 1.0f ? 1.0f : in.dtS));
    const bool pvOk = std::isfinite(in.pv) && in.pv > 0.0f && in.pv < 60.0f;
    const bool spOk = std::isfinite(in.sp) && in.sp > 20.0f && in.sp < 50.0f;
    const float cap = (std::isfinite(in.capPct) && in.capPct > 0.0f) ? std::min(100.0f, in.capPct) : 0.0f;
    const bool live = (in.permit || in.shadow) && pvOk && spOk && cap > 0.0f;
    if (!live) {                               // history is stale after a gap; the model is kept
      if (stage_ != StageRun && stage_ != StageLook) stage_ = StageLook;
      ringN_ = 0; accT_ = 0.0f; uAcc_ = 0.0f; coastOn_ = false; lastU_ = 0.0f;
      out.state = StFrozen;
      return out;
    }
    pv_ = in.pv; sp_ = in.sp;
    const float room0 = sp_ - pv_;

    tick(dt);                                  // 10 s grid of PV and of the commanded power
    if (coastOn_) cEl_ += dt;
    if (!started_) {
      started_ = true;
      stage_ = (model_.known || room0 < PROBE_ROOM_C) ? StageRun : StageLook;
      stageT_ = 0.0f;
    }
    stageT_ += dt;
    float slope = 0.0f, se = 0.0f;
    const bool slopeOk = fit(9, slope, se);    // mK/s over 90 s
    slopeMks_ = slopeOk ? slope : 0.0f;

    float u = 0.0f;
    uint8_t st = StClimb;
    if (in.shadow && !in.permit) {
      u = 0.0f; st = StFrozen;
    } else if (stage_ == StageLook) {
      u = 0.0f;
      float s9 = 0.0f, e9 = 0.0f;
      if (stageT_ >= LOOK_S && fit(9, s9, e9)) { base_ = std::min(s9, 0.0f) * 1.0e-3f; startPv_ = pv_; tDet_ = -1.0f; stage_ = StageProbe; stageT_ = 0.0f; }
    } else if (stage_ == StageProbe) {
      u = std::min(PROBE_PCT, cap);
      float s9 = 0.0f, e9 = 0.0f;
      const bool have = fit(9, s9, e9);
      const float rise = pv_ - startPv_;
      if (tDet_ < 0.0f && pv_ - (startPv_ + base_ * stageT_) >= RESP_C) tDet_ = stageT_;
      const bool done = (rise >= PROBE_RISE_C && have) || stageT_ >= PROBE_MAX_S || room0 < 1.0f;
      if (done) {
        const float resp = have ? s9 * 1.0e-3f - base_ : 0.0f;
        if (have && resp > 3.0f * e9 * 1.0e-3f && resp > 0.8e-3f) {
          model_.k = clampf(resp / u, K_MIN, K_MAX);
          model_.known = true;
          if (tDet_ > 0.0f) model_.delay = clampf(tDet_, DELAY_MIN_S, DELAY_MAX_S);
          const float d = -base_ / model_.k;
          if (base_ < 0.0f && d >= 0.0f && d <= D_MAX) model_.hold = d;
        }
        beginCoast(s9 * 1.0e-3f);
        stage_ = StageCoast; stageT_ = 0.0f; u = 0.0f;
      }
    } else if (stage_ == StageCoast) {
      u = 0.0f;
      trackCoast(slopeOk ? slope : 0.0f, slopeOk, se, true);
      if (!coastOn_ || stageT_ >= COAST_MAX_S) { stage_ = StageRun; stageT_ = 0.0f; coastOn_ = false; }
    } else {
      u = law(dt, cap, slopeOk, slope, se, st);
      trackCoast(slopeOk ? slope : 0.0f, slopeOk, se, false);
    }

    u = clampf(u, 0.0f, cap);
    commit(u, dt);
    // response watchdog (telemetry): a lot of power for a long time and the air did not move
    if (u > 40.0f) {
      hiT_ += dt;
      if (hiT_ > 360.0f && std::isfinite(hiPv_) && pv_ - hiPv_ < 0.20f) suspect_ = true;
    } else { hiT_ = 0.0f; hiPv_ = pv_; suspect_ = false; }
    out.powerPct = u;
    out.state = (stage_ == StageRun) ? st : static_cast<uint8_t>(in.shadow && !in.permit ? StFrozen : StClimb);
    out.powerLimited = (u >= cap);
    out.trustQ = model_.known ? 15U : 8U;
    lastU_ = u;
    return out;
  }

  // read-outs (telemetry and tests)
  float predicted() const { return pv_; }                 // no prediction exists: the measured PV
  bool responseSuspect() const { return suspect_; }
  bool identified() const { return model_.known; }
  uint8_t batchesDone() const { return model_.known ? 2U : 0U; }
  float kAir() const { return model_.k; }
  float tauTransit() const { return model_.tc; }
  float tdHat() const { return model_.tc; }
  float tauHat() const { return model_.hold; }
  float pHold() const { return model_.hold; }
  float dNow() const { return model_.hold; }
  float dLoss() const { return 0.0f; }
  float haPct() const { return 0.0f; }
  float sigmaPred() const { return 0.0f; }
  float integral() const { return 0.0f; }
  float slopeMks() const { return slopeMks_; }
  bool doorDetected() const { return false; }          // (kept for the telemetry glue: a fall after a cut looks like a door, the law recovers a real one by itself)
  uint8_t stage() const { return stage_; }

 private:
  enum Stage : uint8_t { StageLook = 0, StageProbe = 1, StageCoast = 2, StageRun = 3 };

  static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

  void resetRun() {
    stage_ = StageLook; stageT_ = 0.0f; started_ = false;
    pv_ = NAN; sp_ = 37.5f; ringN_ = 0; accT_ = 0.0f; uAcc_ = 0.0f; uSec_ = 0.0f; lastU_ = 0.0f;
    base_ = 0.0f; startPv_ = 0.0f; tDet_ = -1.0f; flatT_ = 0.0f; coastOn_ = false; slopeMks_ = 0.0f;
    suspect_ = false; hiT_ = 0.0f; hiPv_ = NAN;
    for (uint8_t i = 0; i < RING_N; ++i) { pvR_[i] = 0.0f; uR_[i] = 0.0f; }
  }

  // PV and commanded power on a 10 s grid (independent of how often the machine calls and of when the sensor value changes)
  void tick(float dt) {
    accT_ += dt;
    if (accT_ >= 10.0f) {
      accT_ -= 10.0f;
      for (uint8_t i = 0; i + 1 < RING_N; ++i) { pvR_[i] = pvR_[i + 1]; uR_[i] = uR_[i + 1]; }
      pvR_[RING_N - 1] = pv_; uR_[RING_N - 1] = uSec_ > 0.0f ? uAcc_ / uSec_ : lastU_;
      uAcc_ = 0.0f; uSec_ = 0.0f;
      if (ringN_ < RING_N) ++ringN_;
    }
  }
  void commit(float u, float dt) { uAcc_ += u * dt; uSec_ += dt; }

  // least-squares slope [mK/s] of the newest n grid samples and its standard error
  bool fit(uint8_t n, float &slope, float &se) const {
    if (ringN_ < n || n < 3) return false;
    float sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (uint8_t i = 0; i < n; ++i) { const float t = i * 10.0f, y = pvR_[RING_N - n + i]; sx += t; sy += y; sxx += t * t; sxy += t * y; }
    const float d = n * sxx - sx * sx;
    const float b = (n * sxy - sx * sy) / d, a = (sy - b * sx) / n;
    float sse = 0;
    for (uint8_t i = 0; i < n; ++i) { const float r = pvR_[RING_N - n + i] - (a + b * i * 10.0f); sse += r * r; }
    const float sdd = sxx - sx * sx / n;
    slope = 1000.0f * b;
    se = sdd > 0.0f ? 1000.0f * std::sqrt(sse / (n - 2) / sdd) : 0.0f;
    return std::isfinite(slope);
  }
  // mean commanded power over the newest `sec` seconds of the grid
  float powerAvg(float sec) const {
    uint8_t n = static_cast<uint8_t>(sec / 10.0f + 0.5f);
    if (n < 1) n = 1;
    if (n > ringN_) n = ringN_;
    if (n == 0) return 0.0f;
    float s = 0.0f; for (uint8_t i = 0; i < n; ++i) s += uR_[RING_N - 1 - i];
    return s / n;
  }

  // mean commanded power around `ageSec` ago (3 grid samples): the power that produces the slope the air shows now
  float powerAt(float ageSec) const {
    int c = static_cast<int>(ageSec / 10.0f + 0.5f);
    float s = 0.0f; int n = 0;
    for (int j = c - 1; j <= c + 1; ++j) { if (j >= 0 && j < static_cast<int>(ringN_)) { s += uR_[static_cast<int>(RING_N) - 1 - j]; ++n; } }
    return n ? s / n : 0.0f;
  }

  // ---- coast: how far the air still goes, per unit of slope, after the power was cut ----
  void beginCoast(float sCut) {
    coastOn_ = true; cEl_ = 0.0f; cPv0_ = pv_; cPeak_ = pv_; cS0_ = sCut > 0.0f ? sCut : 0.0f;
  }
  void trackCoast(float slopeMks, bool slopeOk, float se, bool forced) {
    if (!coastOn_) {                           // a cut by anything: >= 15 % for a minute, <= 5 % for the last 20 s, and the air is climbing
      const float aNow = powerAvg(20.0f);
      float before = 0.0f;
      if (ringN_ >= 9) { for (uint8_t i = 0; i < 6; ++i) before += uR_[RING_N - 3 - i]; before /= 6.0f; }
      if (aNow <= 5.0f && before >= 15.0f && slopeOk && slopeMks >= 3.0f * se && slopeMks >= SLOPE_FLOOR_MKS) beginCoast(slopeMks * 1.0e-3f);
      return;
    }
    if (pv_ > cPeak_) cPeak_ = pv_;
    const float el = cEl_;
    if (!forced && powerAvg(20.0f) >= 10.0f && el < 90.0f) { coastOn_ = false; return; }       // the power came back too early: not a clean coast
    const float sNow = slopeOk ? slopeMks * 1.0e-3f : 0.0f;
    const bool ended = el >= 360.0f || (el >= 90.0f && sNow <= 0.35f * cS0_) || pv_ < cPeak_ - 0.15f;
    if (!ended) return;
    coastOn_ = false;
    if (!(cS0_ > 0.0f)) return;
    float obs = (cPeak_ - cPv0_ + (sNow > 0.0f ? sNow * 60.0f : 0.0f)) / cS0_;
    obs = clampf(obs, TC_MIN_S, TC_MAX_S);
    model_.tc = (obs >= model_.tc) ? obs : model_.tc + 0.5f * (obs - model_.tc);
  }

  float law(float dt, float cap, bool slopeOk, float slopeMks, float se, uint8_t &state) {
    const float k = model_.k, tc = std::max(model_.tc, model_.delay);       // the air cannot stop sooner than the delay the probe measured
    const float e = sp_ - pv_;
    const float room = e - MARGIN_C;
    state = (e > FULL_C) ? StClimb : StTrack;
    const float sn = std::max(R1_MIN_MKS, 3.0f * se);
    const float sMeas = (slopeOk && slopeMks > sn) ? slopeMks * 1.0e-3f : 0.0f;          // K/s, only a climb that is more than the staircase

    // nominal: the power that, held, would land the air on SP (the band is as wide as the oven coasts); SP + a larger gain above
    const float kp = BAND / (k * tc);
    float u = (e > 0.0f) ? model_.hold + kp * std::max(0.0f, room) : model_.hold + DOWN_GAIN * kp * e;

    // (R1) momentum: the slope the air shows times the coast time is how far it still goes; the power above a floor is faded out
    // between "distance = room" and "distance = 2 x room"; at or above SP a climbing air gets nothing
    if (sMeas > 0.0f) {
      const float dist = KAPPA * sMeas * tc;
      if (e <= 0.0f) u = 0.0f;
      else if (dist > room) {
        const float fl = FLOOR_FRAC * model_.hold;
        if (u > fl) u = fl + (u - fl) * clampf(2.0f * std::max(0.0f, room) / dist - 1.0f, 0.0f, 1.0f);
      }
    }
    u = clampf(u, 0.0f, cap);

    // the hold level: the power behind the slope the air shows, minus what that slope took for itself (an energy balance, followed with 3 coast times)
    const float pBehind = powerAt(model_.delay);
    if (slopeOk && std::fabs(slopeMks) <= HOLD_SLOPE_MKS && std::fabs(e) <= HOLD_BAND_C && std::fabs(pBehind - powerAvg(30.0f)) <= HOLD_STEADY_PCT) {
      const float dm = pBehind - slopeMks * 1.0e-3f / k;
      model_.hold = clampf(model_.hold + (dm - model_.hold) * dt / (TAUH_FACTOR * tc), 0.0f, D_MAX);
    }
    // flat under SP for two minutes: the hold level is too low (a heavier load, a lower room)
    if (e >= 0.08f && e <= 0.6f && slopeOk && std::fabs(slopeMks) < 1.5f) {
      flatT_ += dt;
      if (flatT_ > 120.0f) model_.hold = clampf(model_.hold + 0.025f * dt, 0.0f, D_MAX);
    } else flatT_ = 0.0f;
    return u;
  }

  Model model_;
  uint8_t stage_ = StageLook;
  float stageT_ = 0.0f;
  bool started_ = false;
  float pv_ = NAN, sp_ = 37.5f, lastU_ = 0.0f, slopeMks_ = 0.0f;
  float pvR_[RING_N], uR_[RING_N];
  uint8_t ringN_ = 0;
  float accT_ = 0.0f, uAcc_ = 0.0f, uSec_ = 0.0f;
  float base_ = 0.0f, startPv_ = 0.0f, tDet_ = -1.0f;
  float flatT_ = 0.0f;
  bool coastOn_ = false;
  float cEl_ = 0.0f, cPv0_ = 0.0f, cPeak_ = 0.0f, cS0_ = 0.0f;
  bool suspect_ = false;
  float hiT_ = 0.0f, hiPv_ = NAN;
};

}  // namespace thermalv3
