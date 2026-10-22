#pragma once
// Thermal V2.0.0 - model-based heating law with a predictor and a PI trim.  Pure C++ (no Arduino types) so the host rig compiles this exact text.
// Spec and the origin of every constant: doc/THERMAL_V2_CONSTANTS.md.  RAM only, nothing stored in NVS (2.0.0).
//
// What the controller believes about the oven (integrating air node, heater path = transport delay + first-order lag):
//     d(PV)/dt = k*P_air - [ c0 + cp*(PV-P0) - ce*(Te-P0) - c3*w(t) ]            P0 = 35 degC (centring only)
//     demand D(PV) = 100*[ c0 + cp*(PV-P0) - ce*(Te-P0) - c3*w ] / (100*k)       [% of bank]
//   which is  [ UA*(PV-Tamb) + hA*(PV-Te) ] / Pmax * 100  : loss to the room grows with PV (cp = k*(UA+hA)/Pmax), exchange with the eggs with PV-Te (ce = k*hA/Pmax).
//   P_air = delivered power after delay and lag, rebuilt from what the arbiter really gave.
//   Te = one slow egg state, Te' = (PV-Te)/tau_egg, kept for three candidate tau_egg; Te_true = Te + delta*w(t), w = exp(-t/tau_egg), delta = c3/ce
//   (the unknown start offset of the egg temperature, e.g. warm eggs after a power cut).
// What it learns, and when
//   * 90 s look with the heater OFF (P_air = 0): the slope is minus the demand times k; a rise means warm eggs.
//   * identification climb at a power that is safe for the fastest oven of the family while the delay is still unknown (time of the first 2*dQ rise).
//   * BATCH fit (resumable, one candidate per call): search of transport delay, lag and egg time constant on the stored history, ridge least squares for
//     (k, c0, cp, ce, c3); first fit 600 s after the first power, second at 1500 s (the first one is then its prior).
//   * a recursive learner follows the coefficients after that; k is frozen between batch fits (a steady hold cannot separate k from the losses).
// Law
//     u = clamp( FF + K*(SP - PV_pred) + I , 0, cap )            K = Kc below SP, 2*Kc above;  Kc = 1/(3*k*theta)   (SIMC, tau_c = 2*theta)
//     FF = D(SP) - (1-BETA)*ha*(SP-Te)                           PV_pred = PV + k*(E_transit - D(PV_mid)*T_transit)
//     far mode: predicted deficit > Z*sigma_pred => full power until the deficit is < half of that (sigma_pred from the learner's covariance)
// Nothing here can heat by itself: the output goes through the production OutputArbiter / HeaterBurstScheduler / protection.
#include <cmath>
#include <cstdint>

namespace thermalv2 {

constexpr uint16_t BIN_N = 1280;           // 1 s bins of delivered power (0.5 % steps): a 960 s fit window + the longest delay of the search
constexpr uint8_t HIST_N = 96;             // 10 s points of PV and of the egg states kept for the batch fit (960 s)
constexpr uint8_t RING_N = 12;             // 10 s samples of PV and of P_air (120 s)
constexpr uint8_t SLOPE_N = 6;             // 60 s least-squares slope
constexpr uint8_t NP = 5;                  // learned coefficients: a = 100*k, c0, cp, ce, c3
constexpr float TAU_LAG_S = 40.0f;         // heater-path lag prior: middle of the 25..55 s range of plants.json
constexpr float THETA_PRIOR_S = 180.0f;    // upper edge of the RG144 range: conservative until the first climb is measured
constexpr float THETA_PICKUP_S = 6.0f;     // sensor filter delay
constexpr float TAU_E_C[3] = {1000.0f, 1500.0f, 2200.0f};   // Ce/hA of 10 000 eggs: 1.9 MJ over 900..1800 W/K is 1050..2100 s
constexpr float TAU_L_C[4] = {25.0f, 35.0f, 45.0f, 55.0f}; // heater lag candidates (plants.json 25..55 s)
constexpr uint8_t TAU_E_PRIOR = 1;
// ---- learner (scaled so every number is O(1): a = 100*k in mK/s, c's in mK/s, slope in mK/s) ----
constexpr float P0 = 35.0f;
constexpr float A0 = 30.0f, A0_STD = 20.0f;      // k = 3e-4 +- 2e-4 degC/s per %  (1.3e-4 .. 6.7e-4 over the prior range)
constexpr float C0_0 = 1.5f, C0_STD = 1.5f;      // mK/s : loss at PV = Te = P0
constexpr float CP_0 = 3.0f, CP_STD = 2.0f;      // mK/s per degC : k*(UA+hA)/Pmax*100
constexpr float CE_0 = 2.0f, CE_STD = 1.5f;      // mK/s per degC : k*hA/Pmax*100
constexpr float C3_0 = 0.0f, C3_STD = 3.0f;      // mK/s : ce*delta, delta = 0 +- ~1.5 degC
constexpr float SLOPE_STD_MKS = 0.9f;            // one 60 s slope of a 0.1 degC staircase: 0.7 mK/s + model error
constexpr float GATE_SIGMAS = 4.0f;              // innovation gate: a door opening is not a slope of this plant
constexpr float P_FLOOR_B = 0.3f;                // mK/s : the running learner stays slowly adaptive (ambient drift)
constexpr float K_MIN = 8e-5f, K_MAX_EST = 9e-4f;
constexpr float D_MIN = -60.0f, D_MAX = 100.0f;  // % of bank
constexpr float C3_MAX = 12.0f;
// ---- first-climb identification ----
constexpr float ID_ROOM_C = 3.0f;          // identification climb needs this much room below SP
constexpr float K_FAST = 6.7e-4f;          // 20 kW on 0.3 MJ/degC: fastest oven of the prior range (only for the safe probe power)
constexpr float T_MAX_S = 240.0f;          // longest transit of the prior range: 200 s delay + 40 s lag
constexpr float ID_SAFETY = 0.8f;
constexpr float ID_RISE_C = 1.0f;
constexpr float ID_P_MIN = 20.0f;
constexpr float H_LO = 12.0f;
constexpr float ID_MAX_PROBE_S = 900.0f;   // longest the safe-power probe may run after the first rise
constexpr float ID_TIMEOUT_S = 600.0f;
constexpr uint8_t LOOK_RING = 9;           // first look: 90 s with the heater OFF
constexpr float BATCH1_S = 600.0f, BATCH2_S = 1500.0f;   // batch fits this long after the first power
constexpr float TD_MIN_S = 20.0f, TD_MAX_S = 260.0f, TD_STEP_S = 10.0f;
constexpr uint8_t TD_N = 25;               // (260-20)/10 + 1
constexpr uint8_t BATCH_MIN_POINTS = 40;
// ---- law ----
constexpr bool EGG_OBSERVER = false;
constexpr bool RLS_NEEDS_BATCH = false;
constexpr float TAUI_FACTOR = 6.0f;        // tau_I = factor * theta (SIMC: 4*(tau_c+theta) = 12 theta; 6 after the sweep on set A)
constexpr float ASYM = 3.0f;               // gain above SP relative to below (G0: 2)
constexpr float DEADZONE_FRAC = 0.0f;     // of the sensor step dQ
constexpr float OBS_GAIN = 0.15f;          // fraction of the innovation applied per 10 s (time constant ~70 s)
constexpr float OBS_DEADBAND_MKS = 0.6f;   // mK/s: below this the 0.1 degC staircase explains the innovation
constexpr float OBS_GATE_MKS = 8.0f;       // mK/s: larger innovations are disturbances of the air
constexpr float OBS_STEP_MAX_C = 0.5f;     // degC per update
constexpr float OBS_MAX_LA_PRIOR = 5.0f;   // % : 'almost no heater power' before the first batch fit
constexpr float Z_SIGMA = 1.5f;            // far mode ends when the predicted deficit is below Z sigma_pred (overshoot budget 0.2 degC by construction)
constexpr float E_ON_MIN = 0.25f;          // degC: never switch to full power for a smaller predicted deficit
constexpr float BETA = 0.85f;
constexpr float LEVEL_FLAT_C = 0.15f;      // degC rise over the 110 s ring window below which the PV counts as flat
constexpr float LEVEL_GAIN = 0.05f;        // per 10 s
constexpr float I_MAX = 30.0f;
constexpr float E115_CLIMB_FRAC = 0.4f;
constexpr float E115_BOOST_MAX = 25.0f;
// ---- independent last-resort limiter (fixed worst-case numbers from the RG144 study, NOT tuned to this controller; it can only lower u) ----
constexpr float LIM_K_HI = 2.5e-4f;        // degC/s per % : 0.02 degC/s of net rise at ~80 % net power, the steepest of RG144
constexpr float LIM_THETA_S = 180.0f;      // longest delay of RG144: energy delivered in this window may still be on its way
constexpr float LIM_H_LO = 12.0f;          // lowest hold power seen
constexpr float LIM_NEW_S = 60.0f;         // horizon over which a newly commanded % is priced
constexpr float LIM_MARGIN_C = 0.4f;       // below the Emergency line
constexpr float PERMIT_RECOMPUTE_S = 120.0f;

enum State : uint8_t { StIdle = 0, StClimb = 1, StTrack = 2, StFrozen = 3, StE115 = 4 };

struct Input {
  float pv = NAN, sp = 37.5f, dQ = 0.1f, headroomC = 0.7f, alarmHeadroomC = 0.7f,   // headroomC: to the vent line; alarmHeadroomC: to the Emergency line (control sensor's frame)
  capPct = 100.0f, deliveredPct = 0.0f, e115Frac = 0.0f, dtS = 0.1f;
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
    for (auto &b : binU_) b = 0;
    for (auto &x : pvRing_) x = 0.0f;
    for (auto &x : paRing_) x = 0.0f;
    binHead_ = 0; ringCount_ = 0; secT_ = 0; secAcc_ = 0; ring10_ = 0; lag_ = 0; absS_ = 0;
    tau_ = TAU_LAG_S; setDelay(THETA_PRIOR_S - THETA_PICKUP_S);
    histCount_ = 0; histHead_ = 0; firstPowerAbs_ = -1.0f; batchStage_ = 0; batchDue_ = false; batchActive_ = false; kFrozen_ = false; kStdB_ = 0; dBias_ = 0;
    I_ = 0; Te_ = NAN; pv_ = NAN; spNow_ = 37.5f; far_ = false; tauEsel_ = TAU_E_PRIOR; sigPred_ = 1.0f;
    idDone_ = false; idPhase_ = 0; rlsHold_ = SLOPE_N + 1; thetaSeen_ = false;
    th_[0] = A0; th_[1] = C0_0; th_[2] = CP_0; th_[3] = CE_0; th_[4] = C3_0;
    for (auto &r : P_) for (auto &v : r) v = 0.0f;
    P_[0][0] = A0_STD * A0_STD; P_[1][1] = C0_STD * C0_STD; P_[2][2] = CP_STD * CP_STD; P_[3][3] = CE_STD * CE_STD; P_[4][4] = C3_STD * C3_STD;
    lostS_ = 0; lastU_ = 0; haveSample_ = false; pvPred_ = 0;
  }
  // read-outs (telemetry and tests)
  float kEst() const { return th_[0] * 1e-5f; }
  float kStd() const { return kFrozen_ ? kStdB_ : std::sqrt(P_[0][0] > 0 ? P_[0][0] : 0) * 1e-5f; }
  float kAir() const { return kUsed(); }
  float thetaHat() const { return theta_; }
  float tdHat() const { return tdEst_; }
  float tauHat() const { return tau_; }
  float tauEggHat() const { return TAU_E_C[tauEsel_]; }
  float haPct() const { return haOf(); }
  float dLoss() const { return lossOf(); }
  float integral() const { return I_; }
  float predicted() const { return pvPred_; }
  float sigmaPred() const { return sigPred_; }
  float teModel() const { return Te_; }
  float dNow() const { return demandAt(pv_); }
  uint8_t batchesDone() const { return batchStage_; }
  bool batchRunning() const { return batchActive_; }

  Output step(const Input &in) {
    Output out;
    const float dt = in.dtS < 0.001f ? 0.001f : (in.dtS > 1.0f ? 1.0f : in.dtS);
    const float cap = (std::isfinite(in.capPct) && in.capPct > 0.0f) ? (in.capPct > 100.0f ? 100.0f : in.capPct) : 0.0f;
    advanceClock(in, dt);
    if (batchDue_ && !batchActive_) batchBegin();
    if (batchActive_) batchStep();
    if (std::isfinite(in.pv)) {
      if (!haveSample_) { for (auto &t : teJ_) t = in.pv; Te_ = in.pv; haveSample_ = true; }
      if (in.newSample) { for (uint8_t j = 0; j < 3; ++j) teJ_[j] += (in.pv - teJ_[j]) * (2.0f / TAU_E_C[j]); Te_ = teJ_[tauEsel_]; identify(in); }
      pv_ = in.pv;
    }
    if (!in.permit || !haveSample_ || !std::isfinite(pv_) || cap <= 0.0f || !std::isfinite(in.sp)) {
      lostS_ += dt;
      if (lostS_ > PERMIT_RECOMPUTE_S) { I_ = 0; if (!idDone_) idPhase_ = 0; }
      lastU_ = 0.0f; out.state = StFrozen; return out;
    }
    lostS_ = 0; spNow_ = in.sp;
    const float k = kUsed();
    const float Ttr = tdPrime() + tau_;
    // the demand grows with the air while it climbs (egg flow ~ PV - Te): pay for it at the middle of the predicted trajectory, not at the present PV
    const float Etr = transitEnergy();
    const float p0 = pv_ + k * (Etr - clampPos(demandAt(pv_)) * Ttr);
    const float pMid = 0.5f * (pv_ + (p0 < in.sp ? p0 : in.sp));
    const float dMid = clampPos(demandAt(pMid));
    pvPred_ = pv_ + k * (Etr - dMid * Ttr);
    const float e = in.sp - pvPred_;
    // uncertainty of that prediction: k (relative), and the demand model (learner covariance) paid over the transit
    sigPred_ = predictionSigma(k, Etr, dMid, Ttr, pMid);
    const float eOn = std::fmax(E_ON_MIN, Z_SIGMA * sigPred_);
    const float Kc = 1.0f / (k * 3.0f * theta_);                 // SIMC for an integrating plant, tau_c = 2*theta
    const float tauI = TAUI_FACTOR * theta_;
    const float K = (e >= 0.0f) ? Kc : ASYM * Kc;
    if (e > eOn) far_ = true; else if (e < 0.5f * eOn) far_ = false;
    // The sensor reports in steps of dQ: the air can sit anywhere within +-dQ/2 of a reading, so a one-step change of the reading is not a full dQ of error.
    // The proportional action works on the error minus the dead band; the integral (which averages over time) keeps the full error.
    const float dz = DEADZONE_FRAC * in.dQ;
    const float eP = e > dz ? e - dz : (e < -dz ? e + dz : 0.0f);
    const float uRaw = far_ ? cap : feedForward() + K * eP + I_;
    float u = uRaw < 0.0f ? 0.0f : (uRaw > cap ? cap : uRaw);
    const bool satHigh = uRaw >= cap, satLow = uRaw < 0.0f;
    if (!((satHigh && e > 0.0f) || (satLow && e < 0.0f))) {      // conditional integration
      I_ += Kc / tauI * e * dt;
      if (I_ > I_MAX) I_ = I_MAX; else if (I_ < -I_MAX) I_ = -I_MAX;
    }
    // Last-resort limiter, independent of the learner and of the law: only the measured PV, its measured 120 s rise carried over one worst-case delay,
    // and the fixed constants above.  It can only lower u, and it only acts near the Emergency line (the controller's own prediction guards High).
    {
      float inflight = 0.0f;
      if (ringCount_ >= RING_N) {
        const float rise = pvRing_[RING_N - 1] - pvRing_[0];
        inflight = rise / (10.0f * (RING_N - 1)) * LIM_THETA_S;
        if (inflight < 0.0f) inflight = 0.0f;
      }
      const float ceilC = in.sp + (in.alarmHeadroomC > LIM_MARGIN_C ? in.alarmHeadroomC - LIM_MARGIN_C : 0.0f);
      const float uLim = LIM_H_LO + (ceilC - pv_ - inflight) / (LIM_K_HI * LIM_NEW_S);
      if (pv_ >= in.sp && uLim < u) { u = uLim > 0.0f ? uLim : 0.0f; far_ = false; }       // above SP only: a deep dip must be allowed to climb back at full power
    }
    out.state = StTrack;
    if (!idDone_ && idPhase_ == 0 && ringCount_ < LOOK_RING) { u = 0.0f; far_ = false; }       // first look: nothing is commanded
    if (idPhase_ == 1 || idPhase_ == 2) { u = idPower_ < cap ? idPower_ : cap; out.state = StClimb; }
    if (in.e115Frac >= E115_CLIMB_FRAC && pv_ < in.sp) {          // heater on for long without the air rising 0.3: ask for more, in proportion
      const float boost = E115_BOOST_MAX * (in.e115Frac - E115_CLIMB_FRAC) / (1.0f - E115_CLIMB_FRAC);
      const float ub = u + boost; u = ub > cap ? cap : ub; out.state = StE115;
    } else if (u >= cap && e > 0.0f) out.state = StClimb;
    out.powerLimited = (u >= cap) && cap < 100.0f;
    out.powerPct = u; lastU_ = u;
    return out;
  }

 private:
  static float clampPos(float v) { return v > 0.0f ? v : 0.0f; }
  float kRaw() const { const float k = th_[0] * 1e-5f; return k < K_MIN ? K_MIN : (k > K_MAX_EST ? K_MAX_EST : k); }
  // cautious k: estimate plus one standard deviation (a larger k means a smaller gain and an earlier stop)
  float kUsed() const { const float k = kRaw() + kStd(); return k > K_MAX_EST ? K_MAX_EST : k; }
  float aSafe() const { return th_[0] > 1.0f ? th_[0] : 1.0f; }
  float wEgg() const { return std::exp(-absS_ / TAU_E_C[tauEsel_]); }
  float haOf() const { return 100.0f * th_[3] / aSafe(); }                                   // egg exchange, % of bank per degC
  float demandAt(float pv) const {                                                            // demand if the air were at pv
    const float d = 100.0f * (th_[1] + th_[2] * (pv - P0) - th_[3] * (Te_ - P0) - th_[4] * wEgg()) / aSafe() + dBias_;
    return d < D_MIN ? D_MIN : (d > D_MAX ? D_MAX : d);
  }
  float teEff() const { const float b = th_[3] > 0.3f ? th_[3] : 0.3f; float d = th_[4] / b; d = d > 6.0f ? 6.0f : (d < -6.0f ? -6.0f : d); return Te_ + d * wEgg(); }
  float lossOf() const { return demandAt(teEff()); }                                          // demand with no air-egg difference
  float feedForward() const { return demandAt(spNow_) - (1.0f - BETA) * haOf() * (spNow_ - teEff()); }
  float tdPrime() const { return tdEst_; }
  // transport delay -> effective delay of the gains (SIMC half rule: + half the lag, + the sensor filter)
  void setDelay(float td) {
    tdEst_ = td < 10.0f ? 10.0f : (td > BIN_N - 20 ? static_cast<float>(BIN_N - 20) : td);
    theta_ = tdEst_ + 0.5f * tau_ + THETA_PICKUP_S;
  }
  // the first 2*dQ rise comes the delay + the ramp-up of the lag later: t = sqrt(2*tau*2dQ / (k*net)); take it out to get the pure delay
  float lagRiseS(float k, float net, float dQ) const {
    const float n = net > 8.0f ? net : 8.0f;
    return std::sqrt(2.0f * tau_ * 2.0f * dQ / (k * n));
  }
  // standard deviation of PV_pred: relative error of k acting on the net in-flight energy, and the demand-model covariance acting over the transit
  float predictionSigma(float k, float Etr, float dMid, float Ttr, float pvMid) const {
    const float kRel = kEst() > 1e-6f ? kStd() / kEst() : 1.0f;
    const float a = aSafe(), w = wEgg();
    const float g[NP] = {-(dMid - dBias_) / a, 100.0f / a, 100.0f * (pvMid - P0) / a, -100.0f * (Te_ - P0) / a, -100.0f * w / a};
    float var = 0.0f;
    for (uint8_t i = 1; i < NP; ++i) for (uint8_t j = 1; j < NP; ++j) var += g[i] * P_[i][j] * g[j];     // k is carried by kRel (its row is cleared once frozen)
    if (var < 0.0f) var = 0.0f;
    const float sK = kRel * k * std::fabs(Etr - dMid * Ttr), sD = k * Ttr * std::sqrt(var);
    return std::sqrt(sK * sK + sD * sD + 0.0004f);                                                // + 0.02 degC (sensor step)
  }

  void advanceClock(const Input &in, float dt) {
    secAcc_ += (in.permit && std::isfinite(in.deliveredPct) ? in.deliveredPct : 0.0f) * dt; secT_ += dt;
    while (secT_ >= 1.0f) {
      const float bin = secAcc_ / secT_; secAcc_ = 0; secT_ -= 1.0f; absS_ += 1.0f;
      { const float bq = bin * 2.0f + 0.5f; binU_[binHead_] = static_cast<uint8_t>(bq > 255.0f ? 255.0f : (bq < 0.0f ? 0.0f : bq)); }
      if (firstPowerAbs_ < 0.0f && bin > 1.0f) firstPowerAbs_ = absS_;
      const uint16_t td = static_cast<uint16_t>(tdPrime() + 0.5f);
      const uint16_t idx = static_cast<uint16_t>((binHead_ + BIN_N - td) % BIN_N);   // the bin that leaves the transport delay now
      binHead_ = static_cast<uint16_t>((binHead_ + 1) % BIN_N);
      lag_ += (binU_[idx] * 0.5f - lag_) * (1.0f - std::exp(-1.0f / tau_));
      if (ring10_ >= 9) {
        ring10_ = 0;
        for (uint8_t i = 0; i + 1 < RING_N; ++i) { pvRing_[i] = pvRing_[i + 1]; paRing_[i] = paRing_[i + 1]; }
        pvRing_[RING_N - 1] = std::isfinite(pv_) ? pv_ : 0.0f; paRing_[RING_N - 1] = lag_;
        pvH_[histHead_] = pvRing_[RING_N - 1];
        for (uint8_t j = 0; j < 3; ++j) teH_[j][histHead_] = std::isfinite(teJ_[j]) ? teJ_[j] : pvRing_[RING_N - 1];
        histHead_ = static_cast<uint8_t>((histHead_ + 1) % HIST_N); if (histCount_ < HIST_N) ++histCount_; lastPushAbs_ = absS_;
        if (ringCount_ < RING_N) ++ringCount_;
        if (in.permit && firstPowerAbs_ >= 0.0f && batchStage_ < 2 && absS_ - firstPowerAbs_ >= (batchStage_ == 0 ? BATCH1_S : BATCH2_S)) batchDue_ = true;
        if (!in.permit) { ringCount_ = 0; rlsHold_ = SLOPE_N + 1; histCount_ = 0; batchDue_ = false; }
        else if (rlsHold_ > 0) --rlsHold_;
        else { eggObserver(); rlsUpdate(); levelUpdate(); }
      } else ++ring10_;
    }
  }

  // energy delivered but not yet reflected in the air temperature: bins still inside the delay + what is stored in the lag
  float transitEnergy() const {
    float sum = 0; const uint16_t td = static_cast<uint16_t>(tdPrime() + 0.5f);
    for (uint16_t i = 1; i <= td; ++i) sum += binU_[(binHead_ + BIN_N - i) % BIN_N] * 0.5f;
    return sum + tau_ * lag_;
  }

  // Level anchor: in a FLAT window the power that arrives equals the demand whatever k is, so the model's level is pulled to it (slowly).  The same amount
  // is taken out of I, so the pair (model level, I) changes by nothing but where the number lives.
  void levelUpdate() {
    if (!idDone_ || ringCount_ < RING_N || batchActive_) return;
    const float rise = pvRing_[RING_N - 1] - pvRing_[0];
    if (std::fabs(rise) > LEVEL_FLAT_C) return;
    float pa = 0, pvm = 0; for (uint8_t i = 0; i < RING_N; ++i) { pa += paRing_[i]; pvm += pvRing_[i]; }
    pa /= RING_N; pvm /= RING_N;
    const float step = LEVEL_GAIN * (pa - demandAt(pvm));
    dBias_ += step; I_ -= step;
    if (dBias_ > 40.0f) dBias_ = 40.0f; else if (dBias_ < -40.0f) dBias_ = -40.0f;
    if (I_ > I_MAX) I_ = I_MAX; else if (I_ < -I_MAX) I_ = -I_MAX;
  }

  // Egg observer: Te is a STATE (a tray swap, warm eggs after a power cut move it in one step), the coefficients are parameters.  The innovation of the
  // 60 s slope against the model is turned into a correction of Te: slope has +ce*Te, so a slope lower than predicted means colder eggs than modelled.
  // It runs only where the innovation does not depend on an uncertain k: after the first batch fit, or while (almost) no heater power arrives.
  void eggObserver() {
    if (!EGG_OBSERVER || batchActive_ || ringCount_ < SLOPE_N || !haveSample_) return;
    const uint8_t n = SLOPE_N; float sx = 0, sy = 0, sxx = 0, sxy = 0, pa = 0, pvm = 0;
    for (uint8_t i = 0; i < n; ++i) {
      const float t = i * 10.0f, y = pvRing_[RING_N - n + i];
      sx += t; sy += y; sxx += t * t; sxy += t * y; pa += paRing_[RING_N - n + i]; pvm += y;
    }
    pa /= n; pvm /= n;
    if (!(batchStage_ > 0 || pa < OBS_MAX_LA_PRIOR)) return;
    const float slopeMks = 1000.0f * (n * sxy - sx * sy) / (n * sxx - sx * sx);
    const float pred = th_[0] * pa * 0.01f - (th_[1] + th_[2] * (pvm - P0) - th_[3] * (Te_ - P0) - th_[4] * wEgg());
    float nu = slopeMks - pred;                                       // mK/s
    const float ce = th_[3] > 0.5f ? th_[3] : 0.5f;
    if (std::fabs(nu) < OBS_DEADBAND_MKS || std::fabs(nu) > OBS_GATE_MKS) return;      // a step of the AIR (door, vent) is not an egg change
    float dTe = OBS_GAIN * nu / ce;
    if (dTe > OBS_STEP_MAX_C) dTe = OBS_STEP_MAX_C; else if (dTe < -OBS_STEP_MAX_C) dTe = -OBS_STEP_MAX_C;
    for (auto &t : teJ_) t += dTe;
    Te_ = teJ_[tauEsel_];
  }

  // ---- the running learner: slope(mK/s) = a*La/100 - c0 - cp*(PV-P0) + ce*(Te-P0) + c3*w   (theta = [a, c0, cp, ce, c3]) ----
  void rlsUpdate() {
    if (RLS_NEEDS_BATCH && batchStage_ == 0) return;                                              // before the first batch fit the data cannot separate the unknowns: priors only
    if (batchActive_ || ringCount_ < SLOPE_N || (!idDone_ && idPhase_ == 1)) return;              // the delay must be known before P_air means anything
    const uint8_t n = SLOPE_N; float sx = 0, sy = 0, sxx = 0, sxy = 0, pa = 0, pvm = 0;
    for (uint8_t i = 0; i < n; ++i) {
      const float t = i * 10.0f, y = pvRing_[RING_N - n + i];
      sx += t; sy += y; sxx += t * t; sxy += t * y; pa += paRing_[RING_N - n + i]; pvm += y;
    }
    const float slopeMks = 1000.0f * (n * sxy - sx * sy) / (n * sxx - sx * sx);
    pa /= n; pvm /= n;
    const float phi[NP] = {pa * 0.01f, -1.0f, -(pvm - P0), (Te_ - P0), wEgg()};
    float Pphi[NP], s2 = SLOPE_STD_MKS * SLOPE_STD_MKS;
    for (int i = 0; i < NP; ++i) { Pphi[i] = 0; for (int j = 0; j < NP; ++j) Pphi[i] += P_[i][j] * phi[j]; }
    for (int i = 0; i < NP; ++i) s2 += phi[i] * Pphi[i];
    float err = slopeMks; for (int i = 0; i < NP; ++i) err -= phi[i] * th_[i];
    if (err * err > GATE_SIGMAS * GATE_SIGMAS * s2) return;           // outlier (door, tray, vent): not a slope of this plant
    float g[NP]; for (int i = 0; i < NP; ++i) g[i] = Pphi[i] / s2;
    for (int i = 0; i < NP; ++i) th_[i] += g[i] * err;
    for (int i = 0; i < NP; ++i) for (int j = 0; j < NP; ++j) P_[i][j] -= g[i] * Pphi[j];
    for (int i = 0; i < NP; ++i) for (int j = i + 1; j < NP; ++j) { const float m = 0.5f * (P_[i][j] + P_[j][i]); P_[i][j] = P_[j][i] = m; }
    for (int i = 1; i < NP; ++i) if (P_[i][i] < P_FLOOR_B * P_FLOOR_B) P_[i][i] = P_FLOOR_B * P_FLOOR_B;
    if (!kFrozen_ && P_[0][0] < 1.0f) P_[0][0] = 1.0f;
    clampTheta();
  }
  void clampTheta() {
    if (th_[2] < 0.5f) th_[2] = 0.5f; else if (th_[2] > 9.0f) th_[2] = 9.0f;                  // physical: loss + exchange coefficient
    if (th_[3] < 0.2f) th_[3] = 0.2f; else if (th_[3] > th_[2]) th_[3] = th_[2];               // exchange cannot exceed the total coefficient
    if (th_[4] > C3_MAX) th_[4] = C3_MAX; else if (th_[4] < -C3_MAX) th_[4] = -C3_MAX;
    if (th_[0] < K_MIN * 1e5f) th_[0] = K_MIN * 1e5f; else if (th_[0] > K_MAX_EST * 1e5f) th_[0] = K_MAX_EST * 1e5f;
  }

  // ---- batch identification, resumable: one (delay, lag, egg time constant) candidate per call of step() ----
  float binAt(float sec) const {                                  // delivered % in absolute second `sec`, as of the moment the batch started; 0 outside the history
    const float age = absRef_ - sec;
    if (age < 0.0f || age >= BIN_N) return 0.0f;
    return binU_[(binHeadRef_ + BIN_N - 1 - static_cast<uint16_t>(age)) % BIN_N] * 0.5f;
  }
  // replay the heater path (delay td, lag tau) over the stored points; fills bla_ with P_air at every point, returns P_air at absRef_
  float replayWindow(float td, float tau, uint8_t ns) {
    const float t0 = lastPushRef_ - 10.0f * (ns - 1), alpha = 1.0f - std::exp(-1.0f / tau);
    float y = binAt(t0 - td); bla_[0] = y;
    const int total = 10 * (ns - 1);
    for (int s = 1; s <= total; ++s) { y += (binAt(t0 + s - td) - y) * alpha; if (s % 10 == 0) bla_[s / 10] = y; }
    for (float t = lastPushRef_ + 1.0f; t <= absRef_ + 0.5f; t += 1.0f) y += (binAt(t - td) - y) * alpha;
    return y;
  }
  // NPxNP ridge solve + inverse (double, Gauss-Jordan with partial pivoting); false if singular
  static bool solveNP(double A[NP][NP], const double b[NP], double x[NP], double inv[NP][NP]) {
    double M[NP][2 * NP];
    for (int i = 0; i < NP; ++i) { for (int j = 0; j < NP; ++j) { M[i][j] = A[i][j]; M[i][NP + j] = (i == j) ? 1.0 : 0.0; } }
    for (int c = 0; c < NP; ++c) {
      int piv = c; for (int r = c + 1; r < NP; ++r) if (std::fabs(M[r][c]) > std::fabs(M[piv][c])) piv = r;
      if (std::fabs(M[piv][c]) < 1e-12) return false;
      if (piv != c) for (int j = 0; j < 2 * NP; ++j) { const double t = M[c][j]; M[c][j] = M[piv][j]; M[piv][j] = t; }
      const double d = M[c][c]; for (int j = 0; j < 2 * NP; ++j) M[c][j] /= d;
      for (int r = 0; r < NP; ++r) if (r != c) { const double f = M[r][c]; for (int j = 0; j < 2 * NP; ++j) M[r][j] -= f * M[c][j]; }
    }
    for (int i = 0; i < NP; ++i) { for (int j = 0; j < NP; ++j) inv[i][j] = M[i][NP + j]; }
    for (int i = 0; i < NP; ++i) { x[i] = 0; for (int j = 0; j < NP; ++j) x[i] += inv[i][j] * b[j]; }
    return true;
  }
  void batchBegin() {
    batchDue_ = false;
    if (histCount_ < BATCH_MIN_POINTS || !idDoneOrProbe()) { return; }
    bns_ = histCount_;
    bfirst_ = static_cast<uint8_t>((histHead_ + HIST_N - bns_) % HIST_N);
    absRef_ = absS_; binHeadRef_ = binHead_; lastPushRef_ = lastPushAbs_;
    // first fit: the wide prior of the oven family; later fits: the previous result is the prior (a settled hold cannot overturn what a climb measured)
    const double th0[NP] = {A0, C0_0, CP_0, CE_0, C3_0}, sd0[NP] = {A0_STD, C0_STD, CP_STD, CE_STD, C3_STD};
    for (int p = 0; p < NP; ++p) { bth0_[p] = th0[p]; bsd_[p] = sd0[p]; }
    if (batchStage_ > 0) {
      for (int p = 0; p < NP; ++p) bth0_[p] = th_[p];
      bsd_[0] = kStdB_ * 1e5f > 1.0f ? kStdB_ * 1e5f : 1.0f;
      for (int p = 1; p < NP; ++p) { const double v = P_[p][p] > P_FLOOR_B * P_FLOOR_B ? P_[p][p] : P_FLOOR_B * P_FLOOR_B; bsd_[p] = std::sqrt(v) * 2.0; }
    }
    bestRms_ = 1e30; bestFound_ = false; bidx_ = 0; bsub_ = 0; batchActive_ = true;
  }
  bool idDoneOrProbe() const { return true; }
  // One candidate = three calls, so that no call of step() does more than a few hundred microseconds of work on the target:
  //   sub 0: replay of the heater path (float);  sub 1: regression rows and normal equations;  sub 2: 5x5 ridge solve (double) and residual.
  void batchStep() {
    const uint16_t total = static_cast<uint16_t>(TD_N * 4 * 3);
    if (bidx_ >= total) { batchApply(); return; }
    const uint16_t i = bidx_;
    const uint8_t ti = static_cast<uint8_t>(i % 3), li = static_cast<uint8_t>((i / 3) % 4), di = static_cast<uint8_t>(i / 12);
    const float td = TD_MIN_S + TD_STEP_S * di, tau = TAU_L_C[li];
    if (bsub_ == 0) { replayWindow(td, tau, bns_); bsub_ = 1; return; }
    if (bsub_ == 1) {
      for (int p = 0; p < NP; ++p) { bb_[p] = 0; for (int q = 0; q < NP; ++q) bA_[p][q] = 0; }
      brows_ = 0;
      for (uint8_t j = 5; j < bns_; ++j) {
        float pa = 0, pvm = 0, num = 0;
        for (int q = 0; q < 6; ++q) { const uint8_t h = static_cast<uint8_t>((bfirst_ + j - 5 + q) % HIST_N); pa += bla_[j - 5 + q]; pvm += pvH_[h]; num += (q - 2.5f) * pvH_[h]; }
        pa /= 6.0f; pvm /= 6.0f;
        const float slope = num / 17.5f / 10.0f * 1000.0f;
        const float tj = lastPushRef_ - 10.0f * (bns_ - 1 - j);
        float* r = bx_[brows_];
        r[0] = pa * 0.01f; r[1] = -1.0f; r[2] = -(pvm - P0); r[3] = teH_[ti][(bfirst_ + j) % HIST_N] - P0; r[4] = std::exp(-tj / TAU_E_C[ti]); r[NP] = slope;
        for (int p = 0; p < NP; ++p) { for (int q = p; q < NP; ++q) bA_[p][q] += r[p] * r[q]; bb_[p] += r[p] * slope; }   // float sums, upper triangle only
        ++brows_;
      }
      bsub_ = 2; return;
    }
    // sub 2
    bsub_ = 0; ++bidx_;
    const double s2 = static_cast<double>(SLOPE_STD_MKS) * SLOPE_STD_MKS;
    double A[NP][NP], b[NP];
    for (int p = 0; p < NP; ++p) { for (int q = 0; q < NP; ++q) A[p][q] = (q >= p) ? bA_[p][q] : bA_[q][p]; b[p] = bb_[p]; }
    for (int p = 0; p < NP; ++p) { A[p][p] += s2 / (bsd_[p] * bsd_[p]); b[p] += s2 / (bsd_[p] * bsd_[p]) * bth0_[p]; }
    double x[NP], inv[NP][NP];
    if (!solveNP(A, b, x, inv)) return;
    if (x[0] < K_MIN * 1e5f || x[0] > K_MAX_EST * 1e5f) return;
    float ss = 0; for (uint8_t r = 0; r < brows_; ++r) { float e = bx_[r][NP]; for (int p = 0; p < NP; ++p) e -= static_cast<float>(x[p]) * bx_[r][p]; ss += e * e; }
    const double rms = std::sqrt(static_cast<double>(ss) / brows_);
    if (rms < bestRms_) {
      bestRms_ = rms; bestTd_ = td; bestTau_ = tau; bestTe_ = ti; bestFound_ = true;
      for (int p = 0; p < NP; ++p) { bestTh_[p] = x[p]; for (int q = 0; q < NP; ++q) bestInv_[p][q] = inv[p][q]; }
    }
  }
  void batchApply() {
    batchActive_ = false; ++batchStage_;
    if (!bestFound_) return;
    const double s2 = static_cast<double>(SLOPE_STD_MKS) * SLOPE_STD_MKS;
    tau_ = bestTau_; setDelay(bestTd_);
    tauEsel_ = bestTe_; Te_ = teJ_[tauEsel_];
    for (int p = 0; p < NP; ++p) th_[p] = static_cast<float>(bestTh_[p]);
    for (int p = 0; p < NP; ++p) for (int q = 0; q < NP; ++q) P_[p][q] = static_cast<float>(s2 * bestInv_[p][q]);
    clampTheta();
    // the arriving-power state and the last ring entries now follow the identified path (history replayed up to the moment the batch began, then to now)
    lag_ = replayWindow(bestTd_, bestTau_, bns_);
    { const float alpha = 1.0f - std::exp(-1.0f / tau_); const uint16_t tdI = static_cast<uint16_t>(bestTd_ + 0.5f);
      for (float t = absRef_ + 1.0f; t <= absS_ + 0.5f; t += 1.0f) { const uint16_t age = static_cast<uint16_t>(absS_ - t + tdI); lag_ += ((age < BIN_N ? binU_[(binHead_ + BIN_N - 1 - age) % BIN_N] * 0.5f : 0.0f) - lag_) * alpha; } }
    for (int m = 0; m < RING_N && m < bns_; ++m) paRing_[RING_N - 1 - m] = bla_[bns_ - 1 - m];
    // k is a property of the oven measured with excitation; a steady hold cannot separate it from the loss coefficients, so the running learner may not move it
    kStdB_ = static_cast<float>(std::sqrt(s2 * bestInv_[0][0])) * 1e-5f;
    if (kStdB_ < 0.03f * kRaw()) kStdB_ = 0.03f * kRaw();
    kFrozen_ = true; dBias_ = 0.0f;                                   // the fitted model carries the level now
    for (int q = 0; q < NP; ++q) { P_[0][q] = 0.0f; P_[q][0] = 0.0f; }
    rlsHold_ = 0; idDone_ = true; idPhase_ = 3;
  }

  // ---- first climb: transport delay from the first 2*dQ rise (power is held low enough to be safe while blind) ----
  void replayLag() {
    const uint16_t td = static_cast<uint16_t>(tdPrime() + 0.5f); lag_ = 0;
    const float a = 1.0f - std::exp(-1.0f / tau_);
    for (uint16_t j = BIN_N - 1; j >= 1; --j) {                    // oldest first; bin age j seconds
      const uint16_t ageIn = static_cast<uint16_t>(j + td);
      const float x = ageIn < BIN_N ? binU_[(binHead_ + BIN_N - ageIn) % BIN_N] * 0.5f : 0.0f;
      lag_ += (x - lag_) * a;
    }
  }
  // The look found the air rising with the heater off (warm eggs) ... handled by the batch fit (c3); nothing is assumed here.
  void identify(const Input &in) {
    if (idDone_) return;
    if (!in.permit) { idPhase_ = 0; return; }
    if (idPhase_ == 0) {
      if (ringCount_ < LOOK_RING) return;
      const bool risingAlone = in.pv - pvRing_[RING_N - LOOK_RING] > 2.0f * in.dQ;       // heater off and the air still climbs: something else warms it
      if (risingAlone || in.sp - in.pv < ID_ROOM_C) { idDone_ = true; idPhase_ = 3; return; }   // no clean start or no room: priors
      idPhase_ = 1; idT0_ = absS_; idPv0_ = in.pv;
      // blind until the first rise: what is delivered meanwhile must not be able to push the air past SP even in the fastest oven of the prior range
      idPower_ = H_LO + ID_SAFETY * (in.sp - in.pv - ID_RISE_C) / (K_FAST * T_MAX_S);
      if (idPower_ < ID_P_MIN) idPower_ = ID_P_MIN;
      return;
    }
    if (idPhase_ == 1) {
      if (in.pv < idPv0_) idPv0_ = in.pv;                          // running minimum: the air may still be cooling while the first heat is in the delay
      if (in.pv - idPv0_ >= 2.0f * in.dQ) {
        const float th = absS_ - idT0_;
        thetaMeas_ = th; thetaSeen_ = true; pvAtTheta_ = in.pv;
        setDelay(th - THETA_PICKUP_S - lagRiseS(A0 * 1e-5f, idPower_ - demandAt(in.pv), in.dQ));
        replayLag(); idPhase_ = 2; idTTheta_ = absS_; rlsHold_ = SLOPE_N + 1;
      } else if (absS_ - idT0_ > ID_TIMEOUT_S) { idDone_ = true; idPhase_ = 3; }
      return;
    }
    // keep the safe power until the batch fit has measured the oven (it sets idDone_ itself), or the room / time budget is spent
    if (idPhase_ == 2 && (absS_ - idTTheta_ >= ID_MAX_PROBE_S || in.sp - in.pv < ID_ROOM_C * 0.5f)) { idDone_ = true; idPhase_ = 3; }
  }

  float idPower_ = 100.0f, idT0_ = 0, idPv0_ = 0, idTTheta_ = 0;
  bool idDone_ = false; uint8_t idPhase_ = 0, rlsHold_ = 0;
  uint8_t binU_[BIN_N];
  float pvH_[HIST_N], teH_[3][HIST_N], bla_[HIST_N], bx_[HIST_N][NP + 1], teJ_[3];
  uint8_t histCount_ = 0, histHead_ = 0, batchStage_ = 0, tauEsel_ = TAU_E_PRIOR; bool batchDue_ = false, batchActive_ = false;
  uint8_t bns_ = 0, bfirst_ = 0, bsub_ = 0, brows_ = 0; float bA_[NP][NP], bb_[NP]; uint16_t bidx_ = 0; uint16_t binHeadRef_ = 0; float absRef_ = 0, lastPushRef_ = 0;
  double bth0_[NP], bsd_[NP], bestRms_ = 1e30, bestTh_[NP], bestInv_[NP][NP]; float bestTd_ = 0, bestTau_ = TAU_LAG_S; uint8_t bestTe_ = TAU_E_PRIOR; bool bestFound_ = false;
  bool kFrozen_ = false; float kStdB_ = 0, dBias_ = 0, sigPred_ = 1.0f;
  float lastPushAbs_ = 0, firstPowerAbs_ = -1.0f, tau_ = TAU_LAG_S;
  float pvRing_[RING_N], paRing_[RING_N];
  uint16_t binHead_; uint8_t ringCount_, ring10_;
  float secT_, secAcc_, lag_, absS_;
  float th_[NP], P_[NP][NP];
  bool far_ = false;
  float tdEst_ = 150.0f, thetaMeas_ = 0, pvAtTheta_ = 0; bool thetaSeen_ = false;
  float theta_, I_, Te_, pv_, spNow_, pvPred_ = 0, lastU_, lostS_;
  bool haveSample_;
};

}  // namespace thermalv2
