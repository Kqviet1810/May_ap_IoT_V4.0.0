#pragma once
// Thermal V2.0.0 - one predictor-PI heating law with ONE learner.  Pure C++ (no Arduino types) so the host rig compiles this exact text.
// Spec and the origin of every constant: doc/THERMAL_V2_CONSTANTS.md.  RAM only, nothing stored in NVS (2.0.0).
//
// Plant seen by the controller (integrating air node, heater path = transport delay + first-order lag):
//     d(PV)/dt = k*P_air - [ c0 + cp*(PV-P0) - ce*(Te-P0) ]          P0 = 35 degC (centring only)
//     demand D(PV,Te) = [ c0 + cp*(PV-P0) - ce*(Te-P0) ] / k   [% of bank]
//   = [ UA*(PV-Tamb) + hA*(PV-Te) ] / Pmax * 100 : loss to the room grows with PV (cp = k*(UA+hA)/Pmax), egg exchange with PV-Te (ce = k*hA/Pmax).
//   P_air = delivered power after delay and lag, rebuilt from what the arbiter really gave.  Te: one slow egg state, Te' = (PV-Te)/tau_egg.
// One recursive least-squares learner estimates (k, c0, cp, ce) from the 60 s slope of PV, from the first look on.  The first 90 s the heater is
// OFF (P_air = 0): the slope is then the demand itself, independent of k, and a rise with the heater off means warm eggs.
//
//     u = clamp( FF + K*(SP - PV_pred) + I , 0, cap )             K = Kc below SP, 2*Kc above;  Kc = 1/(3*k*theta)   (SIMC, tau_c = 2*theta)
//     FF = D(SP,Te) - (1-BETA)*ha*(SP-Te)                          PV_pred = PV + k*(E_transit - D(PV_mid,Te)*T_transit)
//     far mode: predicted deficit > E_FAR_ON => full power until the deficit is < E_FAR_OFF (decided by PV_pred, not PV)
// Nothing here can heat by itself: the output goes through the production OutputArbiter / HeaterBurstScheduler / protection.
#include <cmath>
#include <cstdint>
#ifdef V2_DUMP
#include <cstdio>
#endif

namespace thermalv2 {

constexpr uint16_t BIN_N = 1280;           // 1 s bins of delivered power (0.5 % steps): a 960 s fit window + the longest delay of the search
constexpr uint8_t HIST_N = 96;             // 10 s points of PV and of the egg state kept for the batch fit (960 s)
constexpr uint8_t RING_N = 12;             // 10 s samples of PV and of P_air (120 s)
constexpr uint8_t SLOPE_N = 6;             // 60 s least-squares slope
constexpr float TAU_LAG_S = 40.0f;         // heater-path lag prior: middle of the 25..55 s range of plants.json
constexpr float THETA_PRIOR_S = 180.0f;    // upper edge of the RG144 range: conservative until the first climb is measured
constexpr float THETA_PICKUP_S = 6.0f;     // measured "first 2*dQ rise" minus the filter delay ~6 s
constexpr float TAU_EGG_S = 1500.0f;       // Ce/hA of 10 000 eggs: 1.9 MJ / 1300 W/K ~ 1460 s
// ---- learner (scaled so every number is O(1): a = 100*k in mK/s, b1 = c1 and b2 = c2 in mK/s, slope in mK/s) ----
constexpr float P0 = 35.0f;                      // centring of PV and Te in the regression
constexpr float A0 = 30.0f, A0_STD = 20.0f;      // k = 3e-4 +- 2e-4 degC/s per %  (1.3e-4 .. 6.7e-4 over the prior range)
constexpr float C0_0 = 1.5f, C0_STD = 1.5f;      // mK/s : loss at PV = Te = P0, UA*(P0-Tamb)/Cf
constexpr float CP_0 = 3.0f, CP_STD = 2.0f;      // mK/s per degC : k*(UA+hA)/Pmax*100 -> (UA+hA)/Pmax 6..13 %/degC at k = 3e-4
constexpr float CE_0 = 2.0f, CE_STD = 1.5f;      // mK/s per degC : k*hA/Pmax*100
constexpr float SLOPE_STD_MKS = 0.9f;            // one 60 s slope of a 0.1 degC staircase: 0.7 mK/s + model error
constexpr float FORGET = 1.0f;                   // no exponential forgetting (it winds the covariance up in a steady hold); the floors below keep it adaptive
constexpr float GATE_SIGMAS = 4.0f;              // innovation gate: a door opening is not a heater/plant slope
constexpr float P_FLOOR_A = 1.0f, P_FLOOR_B1 = 0.3f, P_FLOOR_B2 = 0.3f;   // std 1.0 / 0.3 / 0.3 mK/s: parameters stay slowly adaptive (ambient drift)
constexpr float K_MIN = 8e-5f, K_MAX_EST = 9e-4f;
constexpr float D_MIN = -60.0f, D_MAX = 100.0f;  // % of bank
// ---- first-climb identification (delay) ----
constexpr float ID_ROOM_C = 3.0f;          // identification climb needs this much room below SP
constexpr float K_FAST = 6.7e-4f;          // 20 kW on 0.3 MJ/degC: fastest oven of the prior range (only for the safe probe power)
constexpr float T_MAX_S = 240.0f;          // longest transit of the prior range: 200 s delay + 40 s lag
constexpr float ID_SAFETY = 0.8f;
constexpr float ID_RISE_C = 1.0f;
constexpr float ID_P_MIN = 20.0f;
constexpr float ID_K_REL_STD = 0.2f;         // sigma_k/k below which k is trusted for the delay correction
constexpr float BATCH1_S = 600.0f, BATCH2_S = 1500.0f;   // batch fit of (delay, lag, k, loss, exchange) this long after the first power
constexpr float TD_MIN_S = 20.0f, TD_MAX_S = 260.0f, TD_STEP_S = 10.0f;
constexpr float BATCH_MIN_POINTS = 40;
constexpr uint8_t RLS_OFF_AFTER_STAGE = 255;   // the running learner serves only until the second batch fit
constexpr float ID_MAX_PROBE_S = 900.0f;   // longest the safe-power probe may run after the first rise
constexpr float H_LO = 12.0f;
constexpr float ID_TIMEOUT_S = 600.0f;
constexpr uint8_t LOOK_RING = 9;           // first look: 90 s with the heater OFF; the slope then is the demand itself (hot eggs show as a rise), independent of k
// ---- law ----
constexpr float E_FAR_ON = 0.5f;           // degC of predicted deficit that switches to full power: eps*E_FAR <= 0.2 degC overshoot budget for eps <= 20 %
constexpr float E_FAR_OFF = 0.25f;
constexpr float BETA = 0.85f;
constexpr bool DEMAND_INCLUDES_I = false;
constexpr float LEVEL_FLAT_C = 0.15f;      // degC rise over the 110 s ring window below which the PV counts as flat
constexpr float LEVEL_GAIN = 0.05f;        // per 10 s
constexpr float I_MAX = 30.0f;
constexpr float E115_CLIMB_FRAC = 0.4f;
constexpr float E115_BOOST_MAX = 25.0f;
// ---- independent in-flight limiter (fixed worst-case numbers from the RG144 study, NOT tuned to this controller; it can only lower u) ----
constexpr float LIM_K_HI = 2.5e-4f;        // degC/s per % : 0.02 degC/s of net rise at ~80 % net power, the steepest of RG144
constexpr float LIM_THETA_S = 180.0f;      // longest delay of RG144: energy delivered in this window may still be on its way
constexpr float LIM_H_LO = 12.0f;          // lowest hold power seen: what the oven loses while that energy arrives
constexpr float LIM_NEW_S = 60.0f;         // horizon over which a newly commanded % is priced
constexpr float LIM_MARGIN_C = 0.4f;       // below the Emergency line
constexpr float PERMIT_RECOMPUTE_S = 120.0f;

enum State : uint8_t { StIdle = 0, StClimb = 1, StTrack = 2, StFrozen = 3, StE115 = 4 };

struct Input {
  float pv = NAN, sp = 37.5f, dQ = 0.1f, headroomC = 0.7f, alarmHeadroomC = 0.7f,   // headroomC: to the vent line; alarmHeadroomC: to the Emergency line (both in the control sensor's frame)
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
    tau_ = TAU_LAG_S; setDelay(THETA_PRIOR_S - THETA_PICKUP_S); histCount_ = 0; histHead_ = 0; firstPowerAbs_ = -1.0f; batchStage_ = 0; batchDue_ = false; kFrozen_ = false; kStdB_ = 0; dBias_ = 0; I_ = 0; Te_ = NAN; pv_ = NAN; spNow_ = 37.5f; far_ = false;
    idDone_ = false; idPhase_ = 0; rlsHold_ = SLOPE_N + 1; thetaSeen_ = false;
    th_[0] = A0; th_[1] = C0_0; th_[2] = CP_0; th_[3] = CE_0;
    for (auto &r : P_) for (auto &v : r) v = 0.0f;
    P_[0][0] = A0_STD * A0_STD; P_[1][1] = C0_STD * C0_STD; P_[2][2] = CP_STD * CP_STD; P_[3][3] = CE_STD * CE_STD;
    lostS_ = 0; lastU_ = 0; haveSample_ = false; pvPred_ = 0;
  }
  // read-outs
  float kEst() const { return th_[0] * 1e-5f; }
  float kStd() const { return kFrozen_ ? kStdB_ : std::sqrt(P_[0][0] > 0 ? P_[0][0] : 0) * 1e-5f; }
  float kAir() const { return kUsed(); }
  float thetaHat() const { return theta_; }
  float haPct() const { return haOf(); }
  float dLoss() const { return lossOf(); }
  float integral() const { return I_; }
  float predicted() const { return pvPred_; }
  float teModel() const { return Te_; }
  float dNow() const { return demandAt(pv_); }

  Output step(const Input &in) {
    Output out;
    const float dt = in.dtS < 0.001f ? 0.001f : (in.dtS > 1.0f ? 1.0f : in.dtS);
    const float cap = (std::isfinite(in.capPct) && in.capPct > 0.0f) ? (in.capPct > 100.0f ? 100.0f : in.capPct) : 0.0f;
    advanceClock(in, dt);
    if (batchDue_) batchIdentify();
    if (std::isfinite(in.pv)) {
      if (!haveSample_) { Te_ = in.pv; haveSample_ = true; }
      if (in.newSample) { Te_ += (in.pv - Te_) * (2.0f / TAU_EGG_S); identify(in); }
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
    const float iDem = DEMAND_INCLUDES_I ? I_ : 0.0f;                  // I is the part of the demand the model does not know: the prediction must pay for it too
    const float p0 = pv_ + k * (Etr - clampPos(demandAt(pv_) + iDem) * Ttr);
    const float pMid = 0.5f * (pv_ + (p0 < in.sp ? p0 : in.sp));
    pvPred_ = pv_ + k * (Etr - clampPos(demandAt(pMid) + iDem) * Ttr);
    const float e = in.sp - pvPred_;
    const float Kc = 1.0f / (k * 3.0f * theta_);                 // SIMC for an integrating plant, tau_c = 2*theta
    const float tauI = 6.0f * theta_;                           // 4*(tau_c + theta)
    const float K = (e >= 0.0f) ? Kc : 2.0f * Kc;
    if (e > E_FAR_ON) far_ = true; else if (e < E_FAR_OFF) far_ = false;
    const float uRaw = far_ ? cap : feedForward() + K * e + I_;
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
  float haOf() const { return 100.0f * th_[3] / aSafe(); }                                   // egg exchange, % of bank per degC
  float demandAt(float pv) const {                                                            // demand if the air were at pv (egg temperature as modelled)
    const float d = 100.0f * (th_[1] + th_[2] * (pv - P0) - th_[3] * (Te_ - P0)) / aSafe() + dBias_;
    return d < D_MIN ? D_MIN : (d > D_MAX ? D_MAX : d);
  }
  float lossOf() const { return demandAt(Te_); }                                              // demand with no air-egg difference
  float feedForward() const { return demandAt(spNow_) - (1.0f - BETA) * haOf() * (spNow_ - Te_); }
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
  void advanceClock(const Input &in, float dt) {
    secAcc_ += (in.permit && std::isfinite(in.deliveredPct) ? in.deliveredPct : 0.0f) * dt; secT_ += dt;
    while (secT_ >= 1.0f) {
      const float bin = secAcc_ / secT_; secAcc_ = 0; secT_ -= 1.0f; absS_ += 1.0f;
      { const float bq = bin * 2.0f + 0.5f; binU_[binHead_] = static_cast<uint8_t>(bq > 255.0f ? 255.0f : (bq < 0.0f ? 0.0f : bq)); }
      if (firstPowerAbs_ < 0.0f && bin > 1.0f) firstPowerAbs_ = absS_;
#ifdef V2_DUMP
      std::printf("BIN %.0f %.2f %.3f\n", absS_, bin, std::isfinite(pv_) ? pv_ : 0.0f);
#endif
      const uint16_t td = static_cast<uint16_t>(tdPrime() + 0.5f);
      const uint16_t idx = static_cast<uint16_t>((binHead_ + BIN_N - td) % BIN_N);   // the bin that leaves the transport delay now
      binHead_ = static_cast<uint16_t>((binHead_ + 1) % BIN_N);
      lag_ += (binU_[idx] * 0.5f - lag_) * (1.0f - std::exp(-1.0f / tau_));
      if (ring10_ >= 9) {
        ring10_ = 0;
        for (uint8_t i = 0; i + 1 < RING_N; ++i) { pvRing_[i] = pvRing_[i + 1]; paRing_[i] = paRing_[i + 1]; }
        pvRing_[RING_N - 1] = std::isfinite(pv_) ? pv_ : 0.0f; paRing_[RING_N - 1] = lag_;
        pvH_[histHead_] = pvRing_[RING_N - 1]; teH_[histHead_] = std::isfinite(Te_) ? Te_ : pvRing_[RING_N - 1];
        histHead_ = static_cast<uint8_t>((histHead_ + 1) % HIST_N); if (histCount_ < HIST_N) ++histCount_; lastPushAbs_ = absS_;
        if (in.permit && firstPowerAbs_ >= 0.0f && batchStage_ < 2 && absS_ - firstPowerAbs_ >= (batchStage_ == 0 ? BATCH1_S : BATCH2_S)) batchDue_ = true;
        if (ringCount_ < RING_N) ++ringCount_;
        if (!in.permit) { ringCount_ = 0; rlsHold_ = SLOPE_N + 1; histCount_ = 0; }
        else if (rlsHold_ > 0) --rlsHold_;
        else { rlsUpdate(); levelUpdate(); }
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
    if (!idDone_ || ringCount_ < RING_N) return;
    const float rise = pvRing_[RING_N - 1] - pvRing_[0];
    if (std::fabs(rise) > LEVEL_FLAT_C) return;
    float pa = 0, pvm = 0; for (uint8_t i = 0; i < RING_N; ++i) { pa += paRing_[i]; pvm += pvRing_[i]; }
    pa /= RING_N; pvm /= RING_N;
    const float err = pa - demandAt(pvm);
    const float step = LEVEL_GAIN * err;
    dBias_ += step; I_ -= step;
    if (dBias_ > 40.0f) dBias_ = 40.0f; else if (dBias_ < -40.0f) dBias_ = -40.0f;
    if (I_ > I_MAX) I_ = I_MAX; else if (I_ < -I_MAX) I_ = -I_MAX;
  }

  // ---- the single learner: slope(mK/s) = a*La/100 - b1 - b2*x   with theta = [a, b1, b2] ----
  void rlsUpdate() {
    if (kFrozen_ && batchStage_ >= RLS_OFF_AFTER_STAGE) return;                       // after the batch fits the structure is fixed; the integral carries the residual
    if (ringCount_ < SLOPE_N || (!idDone_ && idPhase_ == 1)) return;               // the delay must be known before P_air means anything
    const uint8_t n = SLOPE_N; float sx = 0, sy = 0, sxx = 0, sxy = 0, pa = 0, xm = 0;
    for (uint8_t i = 0; i < n; ++i) {
      const float t = i * 10.0f, y = pvRing_[RING_N - n + i];
      sx += t; sy += y; sxx += t * t; sxy += t * y; pa += paRing_[RING_N - n + i]; xm += y - Te_;
    }
    const float den = n * sxx - sx * sx;
    const float slopeMks = 1000.0f * (n * sxy - sx * sy) / den;
    pa /= n; xm /= n;
    const float pvm = xm + Te_;
    const float phi[4] = {pa * 0.01f, -1.0f, -(pvm - P0), (Te_ - P0)};
#ifdef V2_DUMP
    std::printf("DUMP %.0f %.4f %.4f %.4f %.4f %.0f %d\n", absS_, slopeMks, pa, pvm, Te_, theta_, int(idPhase_));
#endif
    float Pphi[4], s2 = SLOPE_STD_MKS * SLOPE_STD_MKS;
    for (int i = 0; i < 4; ++i) { Pphi[i] = 0; for (int j = 0; j < 4; ++j) Pphi[i] += P_[i][j] * phi[j] / FORGET; }
    for (int i = 0; i < 4; ++i) s2 += phi[i] * Pphi[i];
    float err = slopeMks; for (int i = 0; i < 4; ++i) err -= phi[i] * th_[i];
    if (err * err > GATE_SIGMAS * GATE_SIGMAS * s2) return;           // outlier (door, tray, vent): not a slope of this plant
    float g[4]; for (int i = 0; i < 4; ++i) g[i] = Pphi[i] / s2;
    for (int i = 0; i < 4; ++i) th_[i] += g[i] * err;
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) P_[i][j] = P_[i][j] / FORGET - g[i] * Pphi[j];
    for (int i = 0; i < 4; ++i) for (int j = i + 1; j < 4; ++j) { const float m = 0.5f * (P_[i][j] + P_[j][i]); P_[i][j] = P_[j][i] = m; }
    if (!kFrozen_ && P_[0][0] < P_FLOOR_A * P_FLOOR_A) P_[0][0] = P_FLOOR_A * P_FLOOR_A;
    if (P_[1][1] < P_FLOOR_B1 * P_FLOOR_B1) P_[1][1] = P_FLOOR_B1 * P_FLOOR_B1;
    if (P_[2][2] < P_FLOOR_B2 * P_FLOOR_B2) P_[2][2] = P_FLOOR_B2 * P_FLOOR_B2;
    if (P_[3][3] < P_FLOOR_B2 * P_FLOOR_B2) P_[3][3] = P_FLOOR_B2 * P_FLOOR_B2;
    if (th_[2] < 0.5f) th_[2] = 0.5f; else if (th_[2] > 9.0f) th_[2] = 9.0f;                  // physical: loss + exchange coefficient
    if (th_[3] < 0.2f) th_[3] = 0.2f; else if (th_[3] > th_[2]) th_[3] = th_[2];               // exchange cannot exceed the total coefficient
    if (th_[0] < K_MIN * 1e5f) th_[0] = K_MIN * 1e5f; else if (th_[0] > K_MAX_EST * 1e5f) th_[0] = K_MAX_EST * 1e5f;
  }

  // ---- batch identification: search the delay and the lag on the stored history, solve the rest by (ridge) least squares ----
  float binAbs(float sec) const {                                 // delivered % in second `sec` (absolute); 0 outside the stored history
    const float age = absS_ - sec;
    if (age < 0.0f || age >= BIN_N) return 0.0f;
    return binU_[(binHead_ + BIN_N - 1 - static_cast<uint16_t>(age)) % BIN_N] * 0.5f;
  }
  // replay the heater path (delay td, lag tau) over the history window; fills bla_ with P_air at every stored point, returns P_air at absS_
  float replayWindow(float td, float tau, uint8_t ns) {
    const float t0 = lastPushAbs_ - 10.0f * (ns - 1), alpha = 1.0f - std::exp(-1.0f / tau);
    float y = binAbs(t0 - td); bla_[0] = y;
    const int total = 10 * (ns - 1);
    for (int sidx = 1; sidx <= total; ++sidx) { y += (binAbs(t0 + sidx - td) - y) * alpha; if (sidx % 10 == 0) bla_[sidx / 10] = y; }
    float yEnd = y;
    for (float t = lastPushAbs_ + 1.0f; t <= absS_ + 0.5f; t += 1.0f) yEnd += (binAbs(t - td) - yEnd) * alpha;
    return yEnd;
  }
  // 4x4 ridge solve + inverse (double, Gauss-Jordan with partial pivoting); false if singular
  static bool solve4(double A[4][4], const double b[4], double x[4], double inv[4][4]) {
    double M[4][8];
    for (int i = 0; i < 4; ++i) { for (int j = 0; j < 4; ++j) { M[i][j] = A[i][j]; M[i][4 + j] = (i == j) ? 1.0 : 0.0; } }
    for (int c = 0; c < 4; ++c) {
      int piv = c; for (int r = c + 1; r < 4; ++r) if (std::fabs(M[r][c]) > std::fabs(M[piv][c])) piv = r;
      if (std::fabs(M[piv][c]) < 1e-12) return false;
      if (piv != c) for (int j = 0; j < 8; ++j) { const double t = M[c][j]; M[c][j] = M[piv][j]; M[piv][j] = t; }
      const double d = M[c][c]; for (int j = 0; j < 8; ++j) M[c][j] /= d;
      for (int r = 0; r < 4; ++r) if (r != c) { const double f = M[r][c]; for (int j = 0; j < 8; ++j) M[r][j] -= f * M[c][j]; }
    }
    for (int i = 0; i < 4; ++i) { for (int j = 0; j < 4; ++j) inv[i][j] = M[i][4 + j]; }
    for (int i = 0; i < 4; ++i) { x[i] = 0; for (int j = 0; j < 4; ++j) x[i] += inv[i][j] * b[j]; }
    return true;
  }
  void batchIdentify() {
    batchDue_ = false;
    const uint8_t ns = histCount_;
    if (ns < BATCH_MIN_POINTS) return;
    const uint8_t first = static_cast<uint8_t>((histHead_ + HIST_N - ns) % HIST_N);
    const double s2 = static_cast<double>(SLOPE_STD_MKS) * SLOPE_STD_MKS;
    // first batch: the wide prior of the oven family; later batches: the previous result is the prior (a settled hold cannot overturn what a climb measured)
    double th0[4] = {A0, C0_0, CP_0, CE_0}, sd[4] = {A0_STD, C0_STD, CP_STD, CE_STD};
    if (batchStage_ > 0) {
      for (int p = 0; p < 4; ++p) th0[p] = th_[p];
      sd[0] = kStdB_ * 1e5f > 1.0f ? kStdB_ * 1e5f : 1.0f;
      for (int p = 1; p < 4; ++p) { const double v = P_[p][p] > P_FLOOR_B1 * P_FLOOR_B1 ? P_[p][p] : P_FLOOR_B1 * P_FLOOR_B1; sd[p] = std::sqrt(v) * 2.0; }
    }
    const float taus[4] = {25.0f, 35.0f, 45.0f, 55.0f};
    double bestRms = 1e30, bestTh[4] = {0, 0, 0, 0}, bestInv[4][4]; float bestTd = -1, bestTau = tau_; bool have = false;
    for (float td = TD_MIN_S; td <= TD_MAX_S + 0.1f; td += TD_STEP_S) {
      for (int ti = 0; ti < 4; ++ti) {
        replayWindow(td, taus[ti], ns);
        double A[4][4] = {{0}}, b[4] = {0, 0, 0, 0}; int rows = 0;
        for (uint8_t j = 5; j < ns; ++j) {
          float pa = 0, pvm = 0, num = 0;
          for (int i = 0; i < 6; ++i) { const uint8_t h = static_cast<uint8_t>((first + j - 5 + i) % HIST_N); pa += bla_[j - 5 + i]; pvm += pvH_[h]; num += (i - 2.5f) * pvH_[h]; }
          pa /= 6.0f; pvm /= 6.0f;
          const float slope = num / 17.5f / 10.0f * 1000.0f;
          const float tem = teH_[(first + j) % HIST_N];
          bx_[rows][0] = pa * 0.01f; bx_[rows][1] = -1.0f; bx_[rows][2] = -(pvm - P0); bx_[rows][3] = tem - P0; bx_[rows][4] = slope;
          for (int p = 0; p < 4; ++p) { for (int q = 0; q < 4; ++q) A[p][q] += static_cast<double>(bx_[rows][p]) * bx_[rows][q]; b[p] += static_cast<double>(bx_[rows][p]) * slope; }
          ++rows;
        }
        for (int p = 0; p < 4; ++p) { A[p][p] += s2 / (sd[p] * sd[p]); b[p] += s2 / (sd[p] * sd[p]) * th0[p]; }
        double x[4], inv[4][4];
        if (!solve4(A, b, x, inv)) continue;
        if (x[0] < K_MIN * 1e5f || x[0] > K_MAX_EST * 1e5f) continue;
        double ss = 0; for (int r = 0; r < rows; ++r) { double e = bx_[r][4]; for (int p = 0; p < 4; ++p) e -= x[p] * bx_[r][p]; ss += e * e; }
        const double rms = std::sqrt(ss / rows);
        if (rms < bestRms) { bestRms = rms; bestTd = td; bestTau = taus[ti]; for (int p = 0; p < 4; ++p) { bestTh[p] = x[p]; for (int q = 0; q < 4; ++q) bestInv[p][q] = inv[p][q]; } have = true; }
      }
    }
    ++batchStage_;
    if (!have) return;
    tau_ = bestTau; setDelay(bestTd);
    for (int p = 0; p < 4; ++p) th_[p] = static_cast<float>(bestTh[p]);
    for (int p = 0; p < 4; ++p) for (int q = 0; q < 4; ++q) P_[p][q] = static_cast<float>(s2 * bestInv[p][q]);
    if (th_[2] < 0.5f) th_[2] = 0.5f; else if (th_[2] > 9.0f) th_[2] = 9.0f;
    if (th_[3] < 0.2f) th_[3] = 0.2f; else if (th_[3] > th_[2]) th_[3] = th_[2];
    lag_ = replayWindow(bestTd, bestTau, ns);                       // P_air state and the last ring entries now follow the identified path
    for (int m = 0; m < RING_N && m < ns; ++m) paRing_[RING_N - 1 - m] = bla_[ns - 1 - m];
    // k and the path are properties of the oven measured with excitation; a steady hold cannot separate k from the loss coefficients, so the
    // running learner is not allowed to move k (its covariance row/column is cleared); the next batch may.
    kStdB_ = static_cast<float>(std::sqrt(s2 * bestInv[0][0])) * 1e-5f;
    if (kStdB_ < 0.03f * kRaw()) kStdB_ = 0.03f * kRaw();
    kFrozen_ = true;
    for (int q = 0; q < 4; ++q) { P_[0][q] = 0.0f; P_[q][0] = 0.0f; }
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
  float pvH_[HIST_N], teH_[HIST_N], bla_[HIST_N], bx_[HIST_N][5];
  uint8_t histCount_ = 0, histHead_ = 0, batchStage_ = 0; bool batchDue_ = false;
  bool kFrozen_ = false; float kStdB_ = 0, dBias_ = 0;
  float lastPushAbs_ = 0, firstPowerAbs_ = -1.0f, tau_ = TAU_LAG_S;
  float pvRing_[RING_N], paRing_[RING_N];
  uint16_t binHead_; uint8_t ringCount_, ring10_;
  float secT_, secAcc_, lag_, absS_;
  float th_[4], P_[4][4];
  bool far_ = false;
  float tdEst_ = 150.0f, thetaMeas_ = 0, pvAtTheta_ = 0; bool thetaSeen_ = false;
  float theta_, I_, Te_, pv_, spNow_, pvPred_ = 0, lastU_, lostS_;
  bool haveSample_;
};

}  // namespace thermalv2
