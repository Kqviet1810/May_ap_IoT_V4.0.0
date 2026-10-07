#pragma once
// Adaptive Thermal V1: coordinator + planner + facade.
//
//   Sensor -> validation/filter -> ThermalObserver (existing) -> ThermalLearner (profile)
//          -> ThermalStartupController (hint) -> PID + feed-forward (this plan)
//          -> adaptive authority limiter (existing supervisor) -> VentCoordinator
//          -> HeaterBurstScheduler -> OutputArbiter -> SSR/master
//
// The plan only ever PROPOSES a bounded feed-forward and integral policy. It cannot grant
// heat: every number it produces still passes the startup ceiling, the supervisor's
// authority limit, the scheduler and the OutputArbiter, and every safety path (High,
// Emergency, E115, inhibit, master drop, sensor loss) resets it by resetting the PID.
#include "thermal_assist.h"
#include "thermal_learner.h"
#include <algorithm>
#include <cmath>

namespace MayapThermal {

enum class VentPhase : uint8_t { Idle, Prepare, Active, Recovery };
inline const char *ventPhaseName(VentPhase p) {
  static const char *const n[] = {"VENT_IDLE", "VENT_PREPARE", "VENT_ACTIVE", "VENT_RECOVERY"};
  return n[static_cast<uint8_t>(p) & 3U];
}

// Ventilation OWNS when and for how long. This is only what thermal is told about it.
struct VentInfo {
  bool active = false;       // exhaust fan ACTUALLY on (post-arbiter)
  bool forced = false;       // safety / temperature / sensor-fault vent: never compensated
  float startsInSec = -1;    // known schedule: seconds to the next start (<0 unknown)
  float remainingSec = -1;   // known schedule: seconds to the end of the current run
};

struct Plan {
  Assist assist;
  StartupHint hint;
  VentPhase ventPhase = VentPhase::Idle;
  float holdFF = 0, ventFF = 0;
  uint8_t ventTrustPct = 0;
};

class VentCoordinator {
 public:
  void reset() { *this = VentCoordinator(); }
  VentPhase phase() const { return phase_; }
  float ff() const { return ff_; }
  float integralAtStart() const { return integralAtStart_; }
  // `targetPct`: bounded compensation computed from the learned profile (0 = none).
  void update(uint32_t now, const VentInfo &v, const ThermalProfile &p, float targetPct,
              float pv, float sp, float integralNow) {
    const float lead = std::min(90.0f, std::max(0.0f, p.heaterDelaySec - 8.0f));
    const float recovery = std::min(180.0f, std::max(40.0f, p.heaterDelaySec + 0.5f * p.coastTimeSec));
    const uint32_t dtMs = seen_ ? now - at_ : 0U;
    at_ = now; seen_ = true;
    const float dt = std::min(5.0f, dtMs * 0.001f);
    switch (phase_) {
      case VentPhase::Idle:
        if (v.active) { enter(VentPhase::Active, now, integralNow); }
        else if (!v.forced && v.startsInSec >= 0 && v.startsInSec <= lead && targetPct > 0) {
          enter(VentPhase::Prepare, now, integralNow);
        }
        break;
      case VentPhase::Prepare:
        if (v.active) enter(VentPhase::Active, now, integralNow);
        else if (v.startsInSec < 0 || v.startsInSec > lead + 10.0f) { phase_ = VentPhase::Idle; }
        break;
      case VentPhase::Active:
        if (!v.active) { enter(VentPhase::Recovery, now, integralNow); recoveryStartFF_ = ff_; recoverySec_ = recovery; }
        break;
      case VentPhase::Recovery:
        if (v.active) enter(VentPhase::Active, now, integralNow);
        else if (static_cast<float>(now - phaseAt_) * 0.001f >= recoverySec_ && ff_ <= 0.01f) phase_ = VentPhase::Idle;
        break;
    }
    // ---- feed-forward shape -----------------------------------------------------------
    float want = 0;
    const float target = v.forced ? 0.0f : targetPct;
    switch (phase_) {
      case VentPhase::Idle: want = 0; break;
      case VentPhase::Prepare: {
        const float frac = lead > 1.0f ? 1.0f - std::min(1.0f, std::max(0.0f, v.startsInSec) / lead) : 1.0f;
        want = target * frac;
        break;
      }
      case VentPhase::Active: {
        want = target;
        // known end: fade the boost out so the delayed heat ends with the cooling
        if (v.remainingSec >= 0 && lead > 1.0f && v.remainingSec < lead) want = target * std::max(0.0f, v.remainingSec / lead);
        break;
      }
      case VentPhase::Recovery: {
        const float e = static_cast<float>(now - phaseAt_) * 0.001f;
        want = recoveryStartFF_ * std::max(0.0f, 1.0f - e / std::max(1.0f, recoverySec_));
        break;
      }
    }
    // Never add heat while PV is already above the setpoint.
    if (pv > sp + 0.10f) want *= std::max(0.0f, 1.0f - (pv - sp - 0.10f) / 0.20f);
    // Bounded slew: no step in either direction (bumpless).
    const float maxStep = 25.0f * dt;  // % per s
    ff_ = std::min(want, ff_ + maxStep);
    if (want < ff_) ff_ = std::max(want, ff_ - 2.0f * maxStep);
    ff_ = std::min(100.0f, std::max(0.0f, ff_));
  }
 private:
  void enter(VentPhase p, uint32_t now, float integralNow) {
    if (phase_ == VentPhase::Idle) integralAtStart_ = integralNow;
    phase_ = p; phaseAt_ = now;
  }
  VentPhase phase_ = VentPhase::Idle;
  uint32_t at_ = 0, phaseAt_ = 0;
  bool seen_ = false;
  float ff_ = 0, recoveryStartFF_ = 0, recoverySec_ = 60, integralAtStart_ = 0;
};

// Authority policy: confidence is the single dial; low confidence is always more conservative.
struct Authority {
  static float ramp(float x, float lo, float hi) {
    return x <= lo ? 0.0f : x >= hi ? 1.0f : (x - lo) / (hi - lo);
  }
};

namespace Policy {
constexpr float GuardOvershootC = 0.35f;       // PV above SP by this while V1 is in control: fall back
constexpr uint32_t GuardLockoutMs = 1800000UL; // 30 min, doubled per repeat (max x8)
constexpr float HoldFfMinPct = 3.0f;           // no hold feed-forward for a hold below the output resolution
}  // namespace Policy
constexpr float VentCapPct = 35.0f;           // vent feed-forward is bounded
constexpr float VentIntegralRisePct = 12.0f;  // integral growth allowed during a vent with no knowledge

class AdaptiveV1 {
 public:
  void reset() { learner_.reset(); vent_.reset(); plan_ = Plan{}; stickyKiLimit_ = 0.0f; }
  // Ablation/diagnostic switch (default ON): learning and hold feed-forward stay active,
  // only the ventilation compensation and its integral policy are removed.
  void setVentCoordination(bool on) { ventCoordination_ = on; }
  bool ventCoordination() const { return ventCoordination_; }
  ThermalLearner &learner() { return learner_; }
  const ThermalLearner &learner() const { return learner_; }
  const VentCoordinator &vent() const { return vent_; }
  const Plan &plan() const { return plan_; }

  void setEnabled(bool on) {
    if (!on && learner_.enabled()) { vent_.reset(); plan_ = Plan{}; }
    learner_.setEnabled(on);
  }
  void tick(uint32_t now, bool actualOn) { learner_.tick(now, actualOn); }

  // Called every control cycle after the PID decision inputs are known.
  uint32_t overshootEvents() const { return overshootEvents_; }
  bool lockedOut(uint32_t now) const { return lockoutUntil_ != 0U && static_cast<int32_t>(now - lockoutUntil_) < 0; }

  const Plan &update(uint32_t now, bool permit, bool tuneRunning, const VentInfo &v, float pv, float sp,
                     float integralNow, float highC = INFINITY) {
    const ThermalProfile &p = learner_.profile();
    plan_ = Plan{};
    // ---- overshoot guard: the profile must never be trusted past a real overshoot ---------------
    if (std::isfinite(sp)) {
      if (std::isfinite(lastSp_) && std::fabs(sp - lastSp_) > 0.01f) spChangedAt_ = now;
      lastSp_ = sp;
    }
    const bool spSettled = spChangedAt_ == 0U || now - spChangedAt_ > 600000UL;
    if (learner_.enabled() && permit && !tuneRunning && std::isfinite(pv) && std::isfinite(sp) && spSettled &&
        pv > sp + Policy::GuardOvershootC && p.confidence >= 30 && !lockedOut(now)) {
      ++overshootEvents_;
      const uint32_t shift = std::min<uint32_t>(3U, overshootEvents_ - 1U);
      lockoutUntil_ = now + (Policy::GuardLockoutMs << shift);
      learner_.degrade();
    }
    const bool usable = learner_.enabled() && permit && !tuneRunning && std::isfinite(pv) && std::isfinite(sp) &&
                        profileRangesValid(p) && !learner_.mismatch() && !lockedOut(now);
    const float conf = static_cast<float>(p.confidence);
    // --- hold feed-forward: 30..60 ramps in, never above the learned duty, trimmed 5 % ---
    float hold = 0;
    // Below ~3 % duty the hold is smaller than what the burst scheduler and a 0.1 C probe can
    // resolve: the estimate wanders and a feed-forward there only perturbs a limit cycle.
    if (usable && learner_.holdTrust() >= 0.5f && p.holdPowerPct >= Policy::HoldFfMinPct)
      hold = p.holdPowerPct * 0.95f * Authority::ramp(conf, 30.0f, 60.0f);
    // --- startup hint: 45..75 ramps in, margin shrinks with confidence ---
    if (usable) {
      const float s = Authority::ramp(conf, 45.0f, 75.0f);
      if (s > 0) {
        plan_.hint.valid = true;
        plan_.hint.strength = s;
        // Margin: shrinks with confidence, grows with the coast measured by the observer and with
        // how large the full-power coast is compared with the room left under the High alarm.
        const float headroom = std::isfinite(highC) ? std::max(0.1f, highC - sp) : 0.7f;
        const float coastFull = p.heaterGain * (p.heaterDelaySec + 8.0f);
        const float headroomExtra = 1.0f * std::min(1.0f, coastFull / (8.0f * headroom));
        const float margin = (1.30f - 0.20f * Authority::ramp(conf, 60.0f, 90.0f)) + headroomExtra;
        plan_.hint.coastPerOnMs = p.heaterGain * 0.001f * margin * learner_.coastScale();
        plan_.hint.gainPerSec = p.heaterGain;
        plan_.hint.slopeValid = learner_.slopeValid();
        plan_.hint.slopePerSec = learner_.slope90();
        plan_.hint.delaySec = p.heaterDelaySec;
        plan_.hint.holdPct = p.holdPowerPct;
      }
    }
    // --- vent feed-forward target: heater duty that offsets Kv, bounded -------------------
    float ventTarget = 0;
    const float vt = learner_.ventTrust();
    if (usable && vt >= 0.3f && p.heaterGain > 0.0005f)
      ventTarget = std::min(VentCapPct, 100.0f * p.ventCoolingGain / p.heaterGain) *
                   Authority::ramp(conf, 30.0f, 60.0f) * Authority::ramp(vt * 100.0f, 25.0f, 70.0f);
    vent_.update(now, usable ? v : VentInfo{}, p, ventTarget, pv, sp, integralNow);
    plan_.ventPhase = vent_.phase();
    plan_.ventFF = usable ? vent_.ff() : 0.0f;
    plan_.holdFF = hold;
    // The startup ceiling must know that a vent is (about to be) removing heat, or it clips the
    // compensation at "hold + 3 %". Only PREPARE/ACTIVE count: in RECOVERY the heat that is
    // still in flight is real surplus and must be braked.
    if (plan_.hint.valid && ventCoordination_ &&
        (vent_.phase() == VentPhase::Prepare || vent_.phase() == VentPhase::Active))
      plan_.hint.ventPct = plan_.ventFF;
    plan_.ventTrustPct = static_cast<uint8_t>(std::min(100.0f, vt * 100.0f));
    if (!ventCoordination_) plan_.ventFF = 0.0f;
    plan_.assist.feedForward = plan_.holdFF;
    plan_.assist.addForward = plan_.ventFF;
    // Correction-authority limiter: ripple budget / (Kh * D). Only with a trusted hold and gain,
    // so a plant without a known hold keeps the full legacy range.
    if (usable && hold > 0.0f && learner_.gainTrust() >= 0.5f && learner_.delayTrust() >= 0.4f) {
      const float theta = std::max(3.0f, p.heaterDelaySec);
      const float a0 = std::min(60.0f, std::max(5.0f, 10.0f / (std::max(p.heaterGain, 0.0005f) * theta)));
      const float w = Authority::ramp(conf, 60.0f, 80.0f);
      if (w > 0) plan_.assist.corrBase = a0 + (1.0f - w) * 100.0f;
    }
    // Integral-gain ceiling from the learned plant (SIMC integrating+delay, tau_c = theta, x2 margin).
    if (usable) {
      const float theta = std::max(3.0f, p.heaterDelaySec);
      const float kiLimit = 100.0f / (16.0f * std::max(p.heaterGain, 0.0005f) * theta * theta);
      const float w = Authority::ramp(conf, 45.0f, 75.0f);
      if (stickyKiLimit_ > 0.0f) {
        // Once a plant has been qualified, the stability limit is never relaxed again just because
        // confidence later falls (a plant change drops it): lifting it to the legacy Ki on a plant
        // that may now be many times more sensitive is what turns a change into a limit cycle.
        plan_.assist.kiMax = std::min(stickyKiLimit_, kiLimit + (1.0f - w) * 1000.0f);
      } else if (w > 0) {
        plan_.assist.kiMax = kiLimit + (1.0f - w) * 1000.0f;  // never qualified: w=0 means no limit
      }
      if (conf >= 75.0f) stickyKiLimit_ = stickyKiLimit_ > 0.0f ? std::min(stickyKiLimit_, kiLimit) : kiLimit;
    } else if (learner_.enabled() && permit && !tuneRunning && stickyKiLimit_ > 0.0f) {
      // A mismatch/lock-out withdraws feed-forward and startup authority, but the loop STABILITY
      // limit (a function of delay and gain, which a loss change does not move) stays: dropping
      // back to the unlimited legacy Ki on a long-delay plant would limit-cycle.
      plan_.assist.kiMax = stickyKiLimit_;
    }
    // During any vent phase the disturbance is handled by feed-forward; I only unwinds, and
    // is capped when nothing is known about the vent so it cannot wind up and overshoot later.
    if (!ventCoordination_) {
      // no coordination: legacy integral behaviour during vents
    } else if (usable && vent_.phase() != VentPhase::Idle) {
      plan_.assist.freezeIntegral = true;
      plan_.assist.integralCeiling = vent_.integralAtStart() + VentIntegralRisePct;
    } else if (!usable && vent_.phase() != VentPhase::Idle) {
      plan_.assist.integralCeiling = vent_.integralAtStart() + VentIntegralRisePct;
    }
    return plan_;
  }

 private:
  ThermalLearner learner_;
  VentCoordinator vent_;
  Plan plan_;
  bool ventCoordination_ = true;
  uint32_t lockoutUntil_ = 0, spChangedAt_ = 0, overshootEvents_ = 0;
  float lastSp_ = NAN;
  float stickyKiLimit_ = 0.0f;
};

}  // namespace MayapThermal
