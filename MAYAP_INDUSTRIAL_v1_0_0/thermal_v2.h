#pragma once
// Thermal V2.0.0 - one predictor-PI heating law.  Pure C++ (no Arduino types) so the host rig compiles this exact text.
// Spec and the origin of every constant: doc/THERMAL_V2_CONSTANTS.md.  RAM only, nothing stored in NVS (2.0.0).
//
//   u = clamp( u_ff + K*(SP - PV_pred) + I , 0, cap )
//   u_ff    = d_loss + beta*hA_pct*(SP - Te)                   power that holds the air at SP while the eggs are still warming
//   PV_pred = PV + k*(E_transit - d_now*T_transit)         air temperature once the energy already delivered has arrived and the demand has been paid
//   d_loss  = learned from the energy balance of the last WINDOW_S seconds:  mean(P_air) - slope/k - egg flow
// Nothing here can heat by itself: the output goes through the production OutputArbiter / HeaterBurstScheduler / protection.
#include <cmath>
#include <cstdint>

namespace thermalv2 {

constexpr uint16_t BIN_N = 400;            // 1 s bins of delivered power: > max transport delay + margin
constexpr uint8_t RING_N = 36;             // 10 s samples of PV and of P_air: WINDOW_S = 360 s balance window
constexpr float WINDOW_S = RING_N * 10.0f;
constexpr float TAU_LAG_S = 40.0f;         // heater-path lag prior: middle of the 25..55 s range of plants.json
constexpr float THETA_PRIOR_S = 180.0f;    // upper edge of the RG144 range: conservative until the first climb is measured
constexpr float THETA_PICKUP_S = 6.0f;     // measured "first 2*dQ rise" minus the filter delay ~6 s = transport delay + the short lag-to-2dQ (checked on A1..A12: td' + tau' within 5 % of td + tau)
constexpr float K_PRIOR = 5.3e-4f;         // degC/s per % of bank: 16 kW on 0.3 MJ/degC (lightest oven => most cautious controller)
constexpr float HA_PCT_PRIOR = 8.0f;       // % of bank per degC air-egg difference: 1300 W/K on 16 kW
constexpr float TAU_EGG_S = 1500.0f;       // Ce/hA of 10 000 eggs: 1.9 MJ / 1300 W/K ~ 1460 s
constexpr float BETA = 1.0f;               // trust in the egg feed-forward (doc section 3)
constexpr float I_MAX = 30.0f;             // trim limit: the balance estimate carries the hold, I only removes its bias
constexpr float H_LO = 12.0f;              // demand used before the balance is known (same number as the independent limiter)
constexpr float ID_ROOM_C = 3.0f;         // identification climb needs this much room below SP: 0.2 (theta) + 1.0 (k) + energy still in flight
constexpr float K_MAX = 6.7e-4f;           // 20 kW on 0.3 MJ/degC: fastest oven of the prior range
constexpr float T_MAX_S = 240.0f;          // longest transit of the prior range: 200 s delay + 40 s lag
constexpr float ID_SAFETY = 0.8f;
constexpr float ID_RISE_C = 1.0f;          // the 10*dQ rise the k estimate needs
constexpr float ID_P_MIN = 20.0f;          // % of bank: below this the identification climb would take longer than the climb it protects
constexpr uint8_t ID_FLAT_RING = 3;
constexpr float E_FAR_ON = 1.0f;          // degC of predicted deficit that switches to full power: eps*E_FAR <= 0.2 degC overshoot budget for eps <= 20 %
constexpr float E_FAR_OFF = 0.7f;
constexpr float BAL_FLAT_SLOPE = 5e-4f;   // degC/s: 0.1 degC over 200 s
constexpr uint8_t STEP_N = 6;             // 60 s windows for the power-step estimate of k
constexpr float STEP_FLAT_PCT = 15.0f;     // % of bank
constexpr float STEP_GIVEUP_S = 900.0f;
constexpr float STEP_MIN_DP = 25.0f;       // % of bank: the step must be at least this large for D to cancel inside 10 %
constexpr float BAL_MAX_NET = 25.0f;       // % of bank
constexpr bool TRANSFER_I = true;
constexpr float B_GAIN = 0.0f;             // per 10 s balance, at a flat PV
constexpr float BAL_SIGMA_PCT = 3.0f;      // % of bank: noise of one balance at a flat PV (0.1 degC steps over a 180 s half-window, k ~ 1.3e-4)
constexpr float BAL_NET_SCALE = 15.0f;     // % of bank: extra variance grows with |slope/k| (a 15 % error of k on that term)
constexpr float BAL_FORGET = 0.99f;       // per 10 s: ~80 min memory
constexpr float HA_MIN = 4.0f, HA_MAX = 13.0f;   // 900..1800 W/K on 14..22 kW (plants.json B ranges)
constexpr float P0_L = 100.0f, P0_HA = 16.0f;    // prior variances: L 12+-10 %, ha 8+-4 %/degC
constexpr float ID_TIMEOUT_S = 600.0f;
constexpr float E115_CLIMB_FRAC = 0.4f;
constexpr float E115_BOOST_MAX = 25.0f;    // % of bank added at the trip point; PV must rise 0.3 degC inside the remaining 60 % of 900 s
constexpr float PERMIT_RECOMPUTE_S = 120.0f;

enum State : uint8_t { StIdle = 0, StClimb = 1, StTrack = 2, StFrozen = 3, StE115 = 4 };

struct Input {
  float pv = NAN, sp = 37.5f, dQ = 0.1f, headroomC = 0.7f, capPct = 100.0f, deliveredPct = 0.0f, e115Frac = 0.0f, dtS = 0.1f;
  bool newSample = false, permit = false;
};
struct Output {
  float powerPct = 0.0f;
  uint8_t state = StIdle;
  bool powerLimited = false;
};

class Controller {
 public:
  Controller() { reset(); }
  void reset() {
    for (auto &b : bins_) b = 0.0f;
    for (auto &x : pvRing_) x = 0.0f;
    for (auto &x : paRing_) x = 0.0f;
    binHead_ = 0; ringCount_ = 0; secT_ = 0; secAcc_ = 0; ring10_ = 0; lag_ = 0; absS_ = 0;
    k_ = K_PRIOR; kKnown_ = false; theta_ = THETA_PRIOR_S; thetaKnown_ = false; haPct_ = HA_PCT_PRIOR;
    dLoss_ = H_LO; bias_ = 0; dKnown_ = false; p00_ = P0_L; p01_ = 0; p11_ = P0_HA; I_ = 0; Te_ = NAN; pv_ = NAN; idDone_ = false; idPhase_ = 0; stepPhase_ = 0; stepSince_ = -1;
    lostS_ = 0; lastU_ = 0; state_ = StIdle; haveSample_ = false; prevSeq_ = 0;
  }
  float kAir() const { return k_; }
  float thetaHat() const { return theta_; }
  float dLoss() const { return dLoss_; }
  float integral() const { return I_; }
  float predicted() const { return pvPred_; }
  float teModel() const { return Te_; }
  float dNow() const { return dLoss_ + haPct_ * (pv_ - Te_) + bias_; }
  bool identified() const { return kKnown_ && thetaKnown_; }

  Output step(const Input &in) {
    Output out;
    const float dt = in.dtS < 0.001f ? 0.001f : (in.dtS > 1.0f ? 1.0f : in.dtS);
    const float cap = (std::isfinite(in.capPct) && in.capPct > 0.0f) ? (in.capPct > 100.0f ? 100.0f : in.capPct) : 0.0f;
    advanceClock(in, dt);
    if (std::isfinite(in.pv)) {
      if (!haveSample_) { Te_ = in.pv; haveSample_ = true; }
      if (in.newSample) onSample(in, dt);
      pv_ = in.pv;
    }
    if (!in.permit || !haveSample_ || !std::isfinite(pv_) || cap <= 0.0f || !std::isfinite(in.sp)) {
      lostS_ += dt;
      if (lostS_ > PERMIT_RECOMPUTE_S) { I_ = 0; idPhase_ = 0; }
      lastU_ = 0.0f; out.state = StFrozen; return out;
    }
    lostS_ = 0; spNow_ = in.sp;
    const float Ttr = tdPrime() + TAU_LAG_S;
    const float Etr = transitEnergy();
    // Air temperature once the energy already delivered has arrived and the demand (loss + egg flow at the present PV) has been paid for the same time.
    const float dNow = dLoss_ + haPct_ * (pv_ - Te_) + bias_;
    pvPred_ = pv_ + k_ * (Etr - (dNow > 0.0f ? dNow : 0.0f) * Ttr);
    const float e = in.sp - pvPred_;
    const float thEff = theta_;
    const float Kc = 1.0f / (k_ * 3.0f * thEff);                 // SIMC for an integrating plant, tau_c = 2*theta_eff
    const float tauI = 12.0f * thEff;                            // 4*(tau_c + theta_eff)
    const float K = (e >= 0.0f) ? Kc : 2.0f * Kc;
    const float dFF = dLoss_ + bias_ + BETA * haPct_ * (in.sp - Te_);
    // Dual mode on the PREDICTED error: far below SP (e > E_FAR_ON) full power, decided by PV_pred and not by PV; once e < E_FAR_OFF the plain SIMC law
    // runs (small-signal loop = G0 margins).  The hysteresis stops the mode from chattering when the prediction wobbles by the model error.
    if (e > E_FAR_ON) far_ = true; else if (e < E_FAR_OFF) far_ = false;
    float uRaw = far_ ? cap : dFF + K * e + I_;
    float u = uRaw < 0.0f ? 0.0f : (uRaw > cap ? cap : uRaw);
    const bool satHigh = uRaw >= cap, satLow = uRaw < 0.0f;
    if (!((satHigh && e > 0.0f) || (satLow && e < 0.0f))) {      // conditional integration
      I_ += Kc / tauI * e * dt;
      if (I_ > I_MAX) I_ = I_MAX; else if (I_ < -I_MAX) I_ = -I_MAX;
    }
    out.state = StTrack;
    if (idPhase_ == 1 || idPhase_ == 2) { u = idPower_ < cap ? idPower_ : cap; out.state = StClimb; }
    if (in.e115Frac >= E115_CLIMB_FRAC && pv_ < in.sp) {          // the heater has been on for a long time without the air rising 0.3: ask for more, in proportion
      const float boost = E115_BOOST_MAX * (in.e115Frac - E115_CLIMB_FRAC) / (1.0f - E115_CLIMB_FRAC);
      const float ub = u + boost; u = ub > cap ? cap : ub; out.state = StE115;
    }
    else if (u >= cap && e > 0.0f) out.state = StClimb;
    out.powerLimited = (u >= cap) && cap < 100.0f;
    out.powerPct = u; lastU_ = u; state_ = out.state;
    return out;
  }

 private:
  float tdPrime() const { const float t = theta_ - THETA_PICKUP_S; return t < 10.0f ? 10.0f : (t > BIN_N - 20 ? static_cast<float>(BIN_N - 20) : t); }

  void advanceClock(const Input &in, float dt) {
    secAcc_ += (in.permit && std::isfinite(in.deliveredPct) ? in.deliveredPct : 0.0f) * dt; secT_ += dt;
    while (secT_ >= 1.0f) {
      const float bin = secAcc_ / secT_; secAcc_ = 0; secT_ -= 1.0f; absS_ += 1.0f;
      bins_[binHead_] = bin;
      const uint16_t td = static_cast<uint16_t>(tdPrime() + 0.5f);
      const uint16_t idx = static_cast<uint16_t>((binHead_ + BIN_N - td) % BIN_N);   // the bin that leaves the transport delay now
      binHead_ = static_cast<uint16_t>((binHead_ + 1) % BIN_N);
      lag_ += (bins_[idx] - lag_) * (1.0f - std::exp(-1.0f / TAU_LAG_S));
      if (ring10_ >= 9) {
        ring10_ = 0;
        for (uint8_t i = 0; i + 1 < RING_N; ++i) { pvRing_[i] = pvRing_[i + 1]; paRing_[i] = paRing_[i + 1]; }
        pvRing_[RING_N - 1] = std::isfinite(pv_) ? pv_ : 0.0f; paRing_[RING_N - 1] = lag_;
        if (ringCount_ < RING_N) ++ringCount_;
        stepRefine();
        if (in.permit) balance(in); else ringCount_ = 0;
      } else ++ring10_;
    }
  }

  // energy delivered but not yet reflected in the air temperature: bins still inside the delay + what is stored in the lag
  float transitEnergy() const {
    float sum = 0; const uint16_t td = static_cast<uint16_t>(tdPrime() + 0.5f);
    for (uint16_t i = 1; i <= td; ++i) sum += bins_[(binHead_ + BIN_N - i) % BIN_N];
    return sum + TAU_LAG_S * lag_;
  }

  void balance(const Input &in) {
    if (ringCount_ < RING_N || !idDone_) return;
    const uint8_t h = RING_N / 2; float a = 0, b = 0, pa = 0;
    for (uint8_t i = 0; i < h; ++i) { a += pvRing_[i]; b += pvRing_[h + i]; }
    // difference of half-window means = k * triangle-weighted mean of (P_air - demand) over the window: weight P_air the same way
    float wsum = 0;
    for (uint8_t i = 0; i < RING_N; ++i) { const float w = static_cast<float>(i < RING_N / 2 ? i + 1 : RING_N - i); pa += w * paRing_[i]; wsum += w; }
    a /= h; b /= h; pa /= wsum;
    const float slope = (b - a) / (h * 10.0f);
    if (!kKnown_ && std::fabs(slope) > BAL_FLAT_SLOPE) return;               // an unmeasured k only counts for a flat PV
    // total demand over the window (% of bank): mean air power minus what the slope says went into the air.  The k-error of that second term is
    // eps*|slope/k|, so the measurement variance grows with it (BAL_SIGMA_PCT at a flat PV).
    const float net = slope / k_;
    if (std::fabs(net) > BAL_MAX_NET) return;                                // climbing hard: the k-error of that term would swamp the demand
    const float dMeas = pa - net;
    const float x = 0.5f * (a + b) - Te_;                                     // air - egg difference at the window centre
    const float sig = BAL_SIGMA_PCT * (1.0f + std::fabs(net) / BAL_NET_SCALE);
    // Bayesian RLS on  dMeas = L + ha*x  : L = loss of the air, ha = egg coupling in % of bank per degC.  Forgetting keeps it following slow change.
    const float ffBefore = dLoss_ + bias_ + BETA * haPct_ * (spNow_ - Te_);
    const float lam = BAL_FORGET;
    float p00 = p00_ / lam, p01 = p01_ / lam, p11 = p11_ / lam;
    const float eAll = dMeas - (dLoss_ + haPct_ * x + bias_);
    bias_ += B_GAIN * eAll * (BAL_SIGMA_PCT / sig);                       // fast residual tracker: the structure (L, ha) is learned slowly, the residual quickly
    const float e = dMeas - bias_ - (dLoss_ + haPct_ * x);
    const float s2 = p00 + 2.0f * p01 * x + p11 * x * x + sig * sig;
    const float g0 = (p00 + p01 * x) / s2, g1 = (p01 + p11 * x) / s2;
    dLoss_ += g0 * e; haPct_ += g1 * e;
    p00_ = p00 - g0 * (p00 + p01 * x); p01_ = p01 - g0 * (p01 + p11 * x); p11_ = p11 - g1 * (p01 + p11 * x);
    if (dLoss_ < 0.0f) dLoss_ = 0.0f; else if (dLoss_ > 60.0f) dLoss_ = 60.0f;
    if (haPct_ < HA_MIN) haPct_ = HA_MIN; else if (haPct_ > HA_MAX) haPct_ = HA_MAX;
    dKnown_ = true; (void)in;
    // Two learners, one job each: the model (L, ha) owns the demand, the integral owns what the model does not know yet.  Whatever the model just
    // absorbed is taken out of I, so u does not jump at the update and I does not double-count the same bias.
    const float ffAfter = dLoss_ + bias_ + BETA * haPct_ * (spNow_ - Te_);
    if (TRANSFER_I) I_ -= (ffAfter - ffBefore);
    if (I_ > I_MAX) I_ = I_MAX; else if (I_ < -I_MAX) I_ = -I_MAX;
  }

  void onSample(const Input &in, float dt) {
    (void)dt;
    // egg model: one state, driven by the measured air
    Te_ += (in.pv - Te_) * (2.0f / TAU_EGG_S);
    identify(in);
  }

  // Identification climb (once, from a flat PV with room to climb): full power until the first 2*dQ rise (theta) and a 10*dQ rise after it
  // (k = slope / mean P_air).  With less than ID_ROOM_C to climb the priors are kept; nothing is learned that the climb cannot afford.
  // the delay is known now: recompute the lag state from the delivered bins with the measured transport delay
  void replayLag() {
    const uint16_t td = static_cast<uint16_t>(tdPrime() + 0.5f); lag_ = 0;
    const float a = 1.0f - std::exp(-1.0f / TAU_LAG_S);
    for (uint16_t j = BIN_N - 1; j >= 1; --j) {                    // oldest first; bin age j seconds
      const uint16_t ageIn = static_cast<uint16_t>(j + td);
      const float x = ageIn < BIN_N ? bins_[(binHead_ + BIN_N - ageIn) % BIN_N] : 0.0f;
      lag_ += (x - lag_) * a;
    }
  }
  void finishId() { idDone_ = true; idPhase_ = 3; snap0(); }
  // k from a POWER STEP: the demand D cancels in  (slope1 - slope0) = k*(P_air1 - P_air0)  as long as D moves little between the two windows.
  // Snapshot 0 is taken when the identification climb ends (low power), snapshot 1 once the full-power climb has settled (delay + 3 lags later).
  void lastWindow(float &slope, float &pa) const {
    const uint8_t n = STEP_N; float sx = 0, sy = 0, sxx = 0, sxy = 0, sp = 0;
    for (uint8_t i = 0; i < n; ++i) { const float x = i * 10.0f, y = pvRing_[RING_N - n + i]; sx += x; sy += y; sxx += x * x; sxy += x * y; sp += paRing_[RING_N - n + i]; }
    slope = (n * sxy - sx * sy) / (n * sxx - sx * sx); pa = sp / n;
  }
  void snap0() { if (ringCount_ >= STEP_N) { lastWindow(slope0_, pa0_); stepPhase_ = 1; stepSince_ = -1; } }
  void stepRefine() {
    if (stepPhase_ != 1 || ringCount_ < STEP_N) return;
    if (stepSince_ < 0) stepSince_ = absS_;                      // measured from the end of the identification climb
    if (absS_ - stepSince_ < tdPrime() + TAU_LAG_S + 10.0f * STEP_N) return;      // P_air moves inside the window; the model La is what is compared, so no need to wait for it to settle
    float sl1, pa1; lastWindow(sl1, pa1);
    if (std::fabs(paRing_[RING_N - 1] - paRing_[RING_N - STEP_N]) > STEP_FLAT_PCT) { if (absS_ - stepSince_ > STEP_GIVEUP_S) stepPhase_ = 2; return; }
    stepPhase_ = 2;
    const float dLa = paRing_[RING_N - 1] - paRing_[RING_N - STEP_N];
    if (pa1 - pa0_ < STEP_MIN_DP || sl1 <= slope0_ || std::fabs(dLa) > STEP_FLAT_PCT) return;   // P_air must be (nearly) constant inside the window
    const float kNew = (sl1 - slope0_) / (pa1 - pa0_);
    if (kNew > 0.4f * k_ && kNew < 2.5f * k_) { k_ = kNew; kKnown_ = true; }
  }
  void identify(const Input &in) {
    if (idDone_) return;
    if (!in.permit) { idPhase_ = 0; return; }
    if (idPhase_ == 0) {
      if (ringCount_ < ID_FLAT_RING) return;            // 30 s of history first
      const bool flat = std::fabs(in.pv - pvRing_[RING_N - ID_FLAT_RING]) <= 2.0f * in.dQ;
      if (!flat) { finishId(); return; }       // already moving when the controller started: priors
      if (in.sp - in.pv < ID_ROOM_C) { finishId(); return; }
      idPhase_ = 1; idT0_ = absS_; idPv0_ = in.pv; idSumLa_ = 0; idN_ = 0;
      // Power of the identification climb: blind until the first rise, so what is delivered meanwhile must not be able to push the air past SP even in
      // the fastest oven of the prior range (k_max over the whole transit T_max): H_LO + 0.8*(room - ID_RISE)/(K_MAX*T_MAX)
      idPower_ = H_LO + ID_SAFETY * (in.sp - in.pv - ID_RISE_C) / (K_MAX * T_MAX_S);
      if (idPower_ < ID_P_MIN) idPower_ = ID_P_MIN;
      return;
    }
    if (idPhase_ == 1 && in.pv < idPv0_) idPv0_ = in.pv;                     // running minimum: the air may still be cooling while the first heat is in the delay
    const float rise = in.pv - idPv0_;
    if (idPhase_ == 1) {
      if (rise >= 2.0f * in.dQ) {
        const float th = absS_ - idT0_;
        theta_ = th < 30.0f ? 30.0f : (th > 320.0f ? 320.0f : th); thetaKnown_ = true;
        idPhase_ = 2; idSumX_ = 0; idTTheta_ = absS_; idPvTheta_ = in.pv; replayLag();
      } else if (absS_ - idT0_ > ID_TIMEOUT_S) finishId();
      return;
    }
    idSumLa_ += lag_; idSumX_ += in.pv - Te_; ++idN_;
    const float span = absS_ - idTTheta_;
    if (in.pv - idPvTheta_ >= 10.0f * in.dQ && span >= 60.0f && idN_ > 20) {
      // demand during the probe: the prior loss plus the egg flow that grows with the air-egg difference built up so far
      const float net = idSumLa_ / idN_ - H_LO - HA_PCT_PRIOR * (idSumX_ / idN_);
      const float kNew = (in.pv - idPvTheta_) / (span * (net > 30.0f ? net : 30.0f));
      k_ = kNew < 5e-5f ? 5e-5f : (kNew > 1.2e-3f ? 1.2e-3f : kNew); kKnown_ = true; finishId();
    } else if (in.sp - in.pv < ID_ROOM_C * 0.5f) finishId();          // ran out of room: keep theta, prior k
  }

  float idPower_ = 100.0f;
  bool idDone_ = false; uint8_t idPhase_ = 0; float idT0_ = 0, idPv0_ = 0, idTTheta_ = 0, idPvTheta_ = 0, idSumLa_ = 0, idSumX_ = 0; uint32_t idN_ = 0;
  float bins_[BIN_N];
  float pvRing_[RING_N], paRing_[RING_N];
  uint16_t binHead_; uint8_t ringCount_, ring10_;
  float secT_, secAcc_, lag_, absS_;
  uint8_t stepPhase_ = 0; float slope0_ = 0, pa0_ = 0, stepSince_ = -1;
  float bias_ = 0, spNow_ = 37.5f; bool far_ = false; float p00_ = P0_L, p01_ = 0, p11_ = P0_HA;
  float k_, theta_, haPct_, dLoss_, I_, Te_, pv_, pvPred_ = 0, lastU_, lostS_;
  bool kKnown_, thetaKnown_, dKnown_, haveSample_;
  uint32_t prevSeq_; uint8_t state_;
};

}  // namespace thermalv2
