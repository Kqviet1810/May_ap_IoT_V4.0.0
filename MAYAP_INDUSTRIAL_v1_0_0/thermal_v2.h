#pragma once
// Thermal V2.0.0 - one predictor-PI heating law with ONE learner.  Pure C++ (no Arduino types) so the host rig compiles this exact text.
// Spec and the origin of every constant: doc/THERMAL_V2_CONSTANTS.md.  RAM only, nothing stored in NVS (2.0.0).
//
// Plant seen by the controller (integrating air node, heater path = transport delay + first-order lag):
//     d(PV)/dt = k * P_air(t)  -  c1  -  c2 * x(t)          x = PV - Te   (Te: one slow egg state, Te' = (PV - Te)/tau_egg)
//     demand D(x) = (c1 + c2*x)/k   [% of bank]      P_air = delivered power after delay and lag (reconstructed from what the arbiter really gave)
// One recursive least-squares learner estimates (k, c1, c2) from the 60 s slope of PV, from the first climb on.
//
//     u = clamp( FF + K*(SP - PV_pred) + I , 0, cap )             K = Kc below SP, 2*Kc above;  Kc = 1/(3*k*theta)   (SIMC, tau_c = 2*theta)
//     FF = D(0) + BETA*(D(SP-Te) - D(0))                           PV_pred = PV + k*(E_transit - D(PV-Te)*T_transit)
//     far mode: predicted deficit > E_FAR_ON => full power until the deficit is < E_FAR_OFF (decided by PV_pred, not PV)
// Nothing here can heat by itself: the output goes through the production OutputArbiter / HeaterBurstScheduler / protection.
#include <cmath>
#include <cstdint>

namespace thermalv2 {

constexpr uint16_t BIN_N = 400;            // 1 s bins of delivered power: > max transport delay + margin
constexpr uint8_t RING_N = 12;             // 10 s samples of PV and of P_air (120 s)
constexpr uint8_t SLOPE_N = 6;             // 60 s least-squares slope
constexpr float TAU_LAG_S = 40.0f;         // heater-path lag prior: middle of the 25..55 s range of plants.json
constexpr float THETA_PRIOR_S = 180.0f;    // upper edge of the RG144 range: conservative until the first climb is measured
constexpr float THETA_PICKUP_S = 6.0f;     // measured "first 2*dQ rise" minus the filter delay ~6 s
constexpr float TAU_EGG_S = 1500.0f;       // Ce/hA of 10 000 eggs: 1.9 MJ / 1300 W/K ~ 1460 s
// ---- learner (scaled so every number is O(1): a = 100*k in mK/s, b1 = c1 and b2 = c2 in mK/s, slope in mK/s) ----
constexpr float A0 = 30.0f, A0_STD = 20.0f;      // k = 3e-4 +- 2e-4 degC/s per %  (1.3e-4 .. 6.7e-4 over the prior range)
constexpr float B1_0 = 3.6f, B1_STD = 3.0f;      // c1 = k*L, L = 12 +- 10 %
constexpr float B2_0 = 2.4f, B2_STD = 2.5f;      // c2 = k*ha, ha = 8 +- 4 %/degC
constexpr float SLOPE_STD_MKS = 0.9f;            // one 60 s slope of a 0.1 degC staircase: 0.7 mK/s + model error
constexpr float FORGET = 1.0f;                   // no exponential forgetting (it winds the covariance up in a steady hold); the floors below keep it adaptive
constexpr float GATE_SIGMAS = 4.0f;              // innovation gate: a door opening is not a heater/plant slope
constexpr float P_FLOOR_A = 1.0f, P_FLOOR_B1 = 0.3f, P_FLOOR_B2 = 0.3f;   // std 1.0 / 0.3 / 0.3 mK/s: parameters stay slowly adaptive (ambient drift)
constexpr float K_MIN = 8e-5f, K_MAX_EST = 9e-4f;
constexpr float HA_MIN = 3.0f, HA_MAX = 15.0f;   // %/degC: 900..1800 W/K on 14..22 kW with margin
constexpr float L_MAX = 60.0f;
// ---- first-climb identification (delay) ----
constexpr float ID_ROOM_C = 3.0f;          // identification climb needs this much room below SP
constexpr float K_FAST = 6.7e-4f;          // 20 kW on 0.3 MJ/degC: fastest oven of the prior range (only for the safe probe power)
constexpr float T_MAX_S = 240.0f;          // longest transit of the prior range: 200 s delay + 40 s lag
constexpr float ID_SAFETY = 0.8f;
constexpr float ID_RISE_C = 1.0f;
constexpr float ID_P_MIN = 20.0f;
constexpr float ID_AFTER_THETA_S = 60.0f;  // probe power is kept this long after the delay was seen, so the learner sees the step
constexpr float H_LO = 12.0f;
constexpr float ID_TIMEOUT_S = 600.0f;
constexpr uint8_t ID_FLAT_RING = 3;
// ---- law ----
constexpr float E_FAR_ON = 0.5f;           // degC of predicted deficit that switches to full power: eps*E_FAR <= 0.2 degC overshoot budget for eps <= 20 %
constexpr float E_FAR_OFF = 0.25f;
constexpr float BETA = 0.7f;
constexpr float I_MAX = 30.0f;
constexpr float E115_CLIMB_FRAC = 0.4f;
constexpr float E115_BOOST_MAX = 25.0f;
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
    theta_ = THETA_PRIOR_S; I_ = 0; Te_ = NAN; pv_ = NAN; spNow_ = 37.5f; far_ = false;
    idDone_ = false; idPhase_ = 0; rlsHold_ = SLOPE_N + 1;
    th_[0] = A0; th_[1] = B1_0; th_[2] = B2_0;
    for (auto &r : P_) for (auto &v : r) v = 0.0f;
    P_[0][0] = A0_STD * A0_STD; P_[1][1] = B1_STD * B1_STD; P_[2][2] = B2_STD * B2_STD;
    lostS_ = 0; lastU_ = 0; haveSample_ = false; pvPred_ = 0;
  }
  // read-outs
  float kEst() const { return th_[0] * 1e-5f; }
  float kStd() const { return std::sqrt(P_[0][0] > 0 ? P_[0][0] : 0) * 1e-5f; }
  float kAir() const { return kUsed(); }
  float thetaHat() const { return theta_; }
  float haPct() const { return haOf(); }
  float dLoss() const { return lossOf(); }
  float integral() const { return I_; }
  float predicted() const { return pvPred_; }
  float teModel() const { return Te_; }
  float dNow() const { return demandAt(pv_ - Te_); }

  Output step(const Input &in) {
    Output out;
    const float dt = in.dtS < 0.001f ? 0.001f : (in.dtS > 1.0f ? 1.0f : in.dtS);
    const float cap = (std::isfinite(in.capPct) && in.capPct > 0.0f) ? (in.capPct > 100.0f ? 100.0f : in.capPct) : 0.0f;
    advanceClock(in, dt);
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
    const float Ttr = tdPrime() + TAU_LAG_S;
    // the demand grows with the air while it climbs (egg flow ~ PV - Te): pay for it at the middle of the predicted trajectory, not at the present PV
    const float Etr = transitEnergy();
    const float p0 = pv_ + k * (Etr - clampPos(demandAt(pv_ - Te_)) * Ttr);
    const float pMid = 0.5f * (pv_ + (p0 < in.sp ? p0 : in.sp));
    pvPred_ = pv_ + k * (Etr - clampPos(demandAt(pMid - Te_)) * Ttr);
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
    out.state = StTrack;
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
  float haOf() const { const float a = th_[0] > 1.0f ? th_[0] : 1.0f; const float h = 100.0f * th_[2] / a; return h < HA_MIN ? HA_MIN : (h > HA_MAX ? HA_MAX : h); }
  float lossOf() const { const float a = th_[0] > 1.0f ? th_[0] : 1.0f; const float l = 100.0f * th_[1] / a; return l < 0.0f ? 0.0f : (l > L_MAX ? L_MAX : l); }
  float demandAt(float x) const { return lossOf() + haOf() * x; }
  float feedForward() const { return lossOf() + BETA * haOf() * (spNow_ - Te_); }
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
        if (!in.permit) { ringCount_ = 0; rlsHold_ = SLOPE_N + 1; }
        else if (rlsHold_ > 0) --rlsHold_;
        else rlsUpdate();
      } else ++ring10_;
    }
  }

  // energy delivered but not yet reflected in the air temperature: bins still inside the delay + what is stored in the lag
  float transitEnergy() const {
    float sum = 0; const uint16_t td = static_cast<uint16_t>(tdPrime() + 0.5f);
    for (uint16_t i = 1; i <= td; ++i) sum += bins_[(binHead_ + BIN_N - i) % BIN_N];
    return sum + TAU_LAG_S * lag_;
  }

  // ---- the single learner: slope(mK/s) = a*La/100 - b1 - b2*x   with theta = [a, b1, b2] ----
  void rlsUpdate() {
    if (ringCount_ < SLOPE_N || !idDone_ && idPhase_ == 1) return;               // the delay must be known before P_air means anything
    const uint8_t n = SLOPE_N; float sx = 0, sy = 0, sxx = 0, sxy = 0, pa = 0, xm = 0;
    for (uint8_t i = 0; i < n; ++i) {
      const float t = i * 10.0f, y = pvRing_[RING_N - n + i];
      sx += t; sy += y; sxx += t * t; sxy += t * y; pa += paRing_[RING_N - n + i]; xm += y - Te_;
    }
    const float den = n * sxx - sx * sx;
    const float slopeMks = 1000.0f * (n * sxy - sx * sy) / den;
    pa /= n; xm /= n;
    const float phi[3] = {pa * 0.01f, -1.0f, -xm};
    float Pphi[3], s2 = SLOPE_STD_MKS * SLOPE_STD_MKS;
    for (int i = 0; i < 3; ++i) { Pphi[i] = 0; for (int j = 0; j < 3; ++j) Pphi[i] += P_[i][j] * phi[j] / FORGET; }
    for (int i = 0; i < 3; ++i) s2 += phi[i] * Pphi[i];
    float err = slopeMks; for (int i = 0; i < 3; ++i) err -= phi[i] * th_[i];
    if (err * err > GATE_SIGMAS * GATE_SIGMAS * s2) return;           // outlier (door, tray, vent): not a slope of this plant
    float g[3]; for (int i = 0; i < 3; ++i) g[i] = Pphi[i] / s2;
    for (int i = 0; i < 3; ++i) th_[i] += g[i] * err;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) P_[i][j] = P_[i][j] / FORGET - g[i] * Pphi[j];
    for (int i = 0; i < 3; ++i) for (int j = i + 1; j < 3; ++j) { const float m = 0.5f * (P_[i][j] + P_[j][i]); P_[i][j] = P_[j][i] = m; }
    if (P_[0][0] < P_FLOOR_A * P_FLOOR_A) P_[0][0] = P_FLOOR_A * P_FLOOR_A;
    if (P_[1][1] < P_FLOOR_B1 * P_FLOOR_B1) P_[1][1] = P_FLOOR_B1 * P_FLOOR_B1;
    if (P_[2][2] < P_FLOOR_B2 * P_FLOOR_B2) P_[2][2] = P_FLOOR_B2 * P_FLOOR_B2;
    if (th_[0] < K_MIN * 1e5f) th_[0] = K_MIN * 1e5f; else if (th_[0] > K_MAX_EST * 1e5f) th_[0] = K_MAX_EST * 1e5f;
  }

  // ---- first climb: transport delay from the first 2*dQ rise (power is held low enough to be safe while blind) ----
  void replayLag() {
    const uint16_t td = static_cast<uint16_t>(tdPrime() + 0.5f); lag_ = 0;
    const float a = 1.0f - std::exp(-1.0f / TAU_LAG_S);
    for (uint16_t j = BIN_N - 1; j >= 1; --j) {                    // oldest first; bin age j seconds
      const uint16_t ageIn = static_cast<uint16_t>(j + td);
      const float x = ageIn < BIN_N ? bins_[(binHead_ + BIN_N - ageIn) % BIN_N] : 0.0f;
      lag_ += (x - lag_) * a;
    }
  }
  void identify(const Input &in) {
    if (idDone_) return;
    if (!in.permit) { idPhase_ = 0; return; }
    if (idPhase_ == 0) {
      if (ringCount_ < ID_FLAT_RING) return;
      const bool flat = std::fabs(in.pv - pvRing_[RING_N - ID_FLAT_RING]) <= 2.0f * in.dQ;
      if (!flat || in.sp - in.pv < ID_ROOM_C) { idDone_ = true; idPhase_ = 3; return; }   // already moving, or no room: priors
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
        theta_ = th < 30.0f ? 30.0f : (th > 320.0f ? 320.0f : th);
        replayLag(); idPhase_ = 2; idTTheta_ = absS_; rlsHold_ = SLOPE_N + 1;
      } else if (absS_ - idT0_ > ID_TIMEOUT_S) { idDone_ = true; idPhase_ = 3; }
      return;
    }
    if (idPhase_ == 2 && (absS_ - idTTheta_ >= ID_AFTER_THETA_S || in.sp - in.pv < ID_ROOM_C * 0.5f)) { idDone_ = true; idPhase_ = 3; }
  }

  float idPower_ = 100.0f, idT0_ = 0, idPv0_ = 0, idTTheta_ = 0;
  bool idDone_ = false; uint8_t idPhase_ = 0, rlsHold_ = 0;
  float bins_[BIN_N];
  float pvRing_[RING_N], paRing_[RING_N];
  uint16_t binHead_; uint8_t ringCount_, ring10_;
  float secT_, secAcc_, lag_, absS_;
  float th_[3], P_[3][3];
  bool far_ = false;
  float theta_, I_, Te_, pv_, spNow_, pvPred_ = 0, lastU_, lostS_;
  bool haveSample_;
};

}  // namespace thermalv2
