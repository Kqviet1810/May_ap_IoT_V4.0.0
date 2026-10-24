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
constexpr uint8_t BATCH_GUARD_POINTS = 12;  // newest points pushed while a batch runs overwrite this many of the oldest: they are not used
// ---- law ----
constexpr bool EGG_OBSERVER_FINE = true;   // the egg observer needs a slope that is not a 0.1 degC staircase: it runs only when the sensor step is <= 0.02
constexpr bool RLS_NEEDS_BATCH = false;
constexpr float TAUI_FACTOR = 8.0f;        // tau_I = factor * theta (SIMC: 4*(tau_c+theta) = 12 theta; 6 after the sweep on set A)
constexpr float KUP = 1.0f;                // proportional gain below SP, in units of Kc
constexpr float ASYM = 20.0f;               // gain above SP relative to below (G0: 2)
constexpr float DEADZONE_FRAC = 0.0f;     // of the sensor step dQ
constexpr float OBS_GAIN = 0.15f;          // fraction of the innovation applied per 10 s (time constant ~70 s)
constexpr float OBS_DEADBAND_MKS = 0.5f;   // mK/s: below this the 0.1 degC staircase explains the innovation
constexpr uint8_t OBS_PERSIST_N = 12;          // 10 s updates a large innovation must last before it is an egg change
constexpr float OBS_GATE_MKS = 8.0f;       // mK/s: larger innovations are disturbances of the air
constexpr float OBS_STEP_MAX_C = 1.0f;     // degC per update
constexpr float OBS_MAX_LA_PRIOR = 5.0f;   // % : 'almost no heater power' before the first batch fit
constexpr float Z_SIGMA = 1.5f;            // far mode ends when the predicted deficit is below Z sigma_pred (overshoot budget 0.2 degC by construction)
constexpr float E_ON_MIN = 0.25f;          // degC: never switch to full power for a smaller predicted deficit
constexpr float BETA = 0.85f;
constexpr float LEVEL_FLAT_C = 0.15f;      // degC rise over the 110 s ring window below which the PV counts as flat
constexpr bool LEVEL_SLOPE_MODE = true;      // G3c: the anchor also works while the PV moves (demand = arriving power - slope/k)
constexpr float LEVEL_SLOPE_GAIN = 0.12f;    // per 10 s
constexpr float LEVEL_SLOPE_GATE = 25.0f;    // % : a larger mismatch is a door/vent, not the demand
constexpr float LEVEL_GAIN = 0.05f;        // per 10 s
constexpr float I_MAX = 30.0f;
constexpr float E115_CLIMB_FRAC = 0.4f;
constexpr float E115_BOOST_MAX = 25.0f;
constexpr float E115_QUIET_FRAC = 0.2f;     // above this share of the E115 limit the G7 limiters (chase, guard) step aside
// ---- independent last-resort limiter (fixed worst-case numbers from the RG144 study, NOT tuned to this controller; it can only lower u) ----
constexpr float LIM_K_HI = 2.5e-4f;        // degC/s per % : 0.02 degC/s of net rise at ~80 % net power, the steepest of RG144
constexpr float LIM_THETA_S = 180.0f;      // longest delay of RG144: energy delivered in this window may still be on its way
// Response watchdog (G4): power that has had time to arrive (older than the longest transit) must move the PV; otherwise the sensor, the heater or the
// path is not what the law believes and the request is withdrawn.  It uses only fixed worst-case constants.
constexpr bool RESP_WATCHDOG = true;
constexpr uint16_t RESP_MATURE_S = 260;           // = T_MAX_S: energy younger than this has not arrived yet
constexpr float RESP_MOVE_C = 0.3f;               // a PV move this large restarts the account (rise or fall)
constexpr float RESP_LOSS_PCT = 85.0f;            // highest loss the heater can be fighting while the air is below SP - RESP_DEFICIT_C
constexpr float RESP_DEFICIT_C = 1.0f;
constexpr float LIM_H_LO = 12.0f;          // lowest hold power seen
constexpr float LIM_NEW_S = 60.0f;         // horizon over which a newly commanded % is priced
constexpr float LIM_MARGIN_C = 0.4f;       // below the Emergency line
// host experiments only (-DV2_Fn=0 switches one G7 feature off); the firmware never defines them
#ifndef V2_F1
#define V2_F1 1
#endif
#ifndef V2_F2
#define V2_F2 1
#endif
#ifndef V2_F3
#define V2_F3 1
#endif
#ifndef V2_F4A
#define V2_F4A 1
#endif
#ifndef V2_F4B
#define V2_F4B 0
#endif
#ifndef V2_F5
#define V2_F5 1
#endif
#ifndef V2_F4C
#define V2_F4C 1
#endif
#ifndef V2_F6
#define V2_F6 1
#endif
#ifndef V2_CHASE_PCT
#define V2_CHASE_PCT 8.0f
#endif
#ifndef V2_CHASE_MODE
#define V2_CHASE_MODE 3
#endif
#ifndef V2_SURPLUS_C
#define V2_SURPLUS_C 0.5f
#endif
#ifndef V2_CHASE_DEF
#define V2_CHASE_DEF 0.6f
#endif
#ifndef V2_CHASE_FALL
#define V2_CHASE_FALL 1
#endif
#ifndef V2_CHASE_ARR
#define V2_CHASE_ARR 15.0f
#endif
#ifndef V2_LEVELCLEAN
#define V2_LEVELCLEAN 0
#endif
#ifndef V2_BRAKE_RESUME
#define V2_BRAKE_RESUME 0.0f
#endif
#ifndef V2_SLOWUP
#define V2_SLOWUP 0.1f
#endif
#ifndef V2_MONO
#define V2_MONO 0
#endif
#ifndef V2_GUARDDEF
#define V2_GUARDDEF 0.3f
#endif
#ifndef V2_GUARDH
#define V2_GUARDH 60.0f
#endif
#ifndef V2_GUARDK
#define V2_GUARDK 1.25f
#endif
#ifndef V2_F7
#define V2_F7 1
#endif
#ifndef V2_F8
#define V2_F8 1
#endif
#ifndef V2_F9
#define V2_F9 1
#endif
// ---- G8 (the owner's log of 24 Oct, doc/THERMAL_V2_G8_DESIGN.md): the air sat 0.1..0.3 degC under SP for 20 minutes with 0..15 % requested because the prediction said "arriving"
#ifndef V2_F10A
#define V2_F10A 1                          // a deficit that does not close is integrated on the MEASURED error, faster
#endif
#ifndef V2_F10B
#define V2_F10B 0                          // the integral does not leak while the air itself is still below SP
#endif
#ifndef V2_F10C
#define V2_F10C 1                          // the measured-slope crest uses the slope's lower confidence bound and a bounded negative demand
#endif
#ifndef V2_F12
#define V2_F12 1                           // the exhaust fan is an input: its loss is learned apart from the oven's own level and paid for in advance
#endif
#ifndef V2_F11
#define V2_F11 1                           // the law trusts its prediction only as far as the prediction agrees with the air when the air stands still
#endif
#ifndef V2_GAP_TAU
#define V2_GAP_TAU 300.0f
#endif
#ifndef V2_GAP_G0
#define V2_GAP_G0 0.04f
#endif
#ifndef V2_GAP_SPAN
#define V2_GAP_SPAN 0.10f
#endif
#ifndef V2_F10B_FLAT
#define V2_F10B_FLAT 1                     // ... only while the air stands still (a rebound after a door still needs the leak)
#endif
#ifndef V2_STUCK_NOARR
#define V2_STUCK_NOARR 1                   // a deficit that the arriving power already fights (a door ajar, a cold tray) is a real loss, not a stuck law
#endif
#ifndef V2_STUCK_GAIN
#define V2_STUCK_GAIN 8.0f
#endif
#ifndef V2_STUCK_AFTER
#define V2_STUCK_AFTER 420.0f
#endif
#ifndef V2_STUCK_REC
#define V2_STUCK_REC 900.0f
#endif
#ifndef V2_STUCK_DEF
#define V2_STUCK_DEF 0.10f
#endif
#ifndef V2_F1_DMF
#define V2_F1_DMF 60.0f
#endif
#ifndef V2_SLOPE_Z
#define V2_SLOPE_Z 3.0f
#endif
#ifndef V2_F10D
#define V2_F10D 0                          // the level anchor corrects the arriving power for the slope also in a "flat" window
#endif
#ifndef V2_F10E
#define V2_F10E 0                          // the slow-up gate of G7 holds only for a mismatch that can be a door (a large one)
#endif
#ifndef V2_SLOWUP_ERR
#define V2_SLOWUP_ERR 8.0f
#endif
#ifndef V2_ASYM_HOLD
#define V2_ASYM_HOLD 20.0f
#endif
// ---- G7 (the owner's real-oven log of 23 Oct, doc/THERMAL_V2_G7_DESIGN.md): the predicted crest is tied to the MEASURED slope, the coast is learned from every
// cut of the power, the first approach brakes at the predicted crest, the integral is released when the air has overtaken it, a far-mode that cannot flicker,
// a guard below SP, a patience measured on the climb, and a shadow mode while the vent line blocks the heater.  Each one is a switch.
constexpr bool F1_KIN_PRED = V2_F1;         // crest = max(model, PV + measured slope * coast time)
constexpr float KIN_SLOPE_MIN_MKS = 1.0f;  // floor of the slope used (a coarse sensor needs 25*dQ, see kinMinSlope())
constexpr float GAIN_LO = 0.3f, GAIN_HI = 2.0f;   // bounds of the learned ratio (coast shown / coast expected)
constexpr bool F2_COAST_LEARN = V2_F2;
constexpr float CUT_AVG_HI = 30.0f, CUT_AVG_LO = 5.0f;   // % delivered: a CUT = >= 30 % over the 60 s before and <= 5 % over the last 10 s
constexpr float CUT_MIN_SLOPE_MKS = 3.0f;
constexpr bool F3_BRAKE = V2_F3;
constexpr float BRAKE_BELOW_C = 0.2f;      // the first approach is stopped when the predicted crest reaches SP minus this
#ifndef V2_BRAKE_MIN
#define V2_BRAKE_MIN 0.3f
#endif
constexpr float BRAKE_MIN_COAST_C = V2_BRAKE_MIN;  // a slower oven has nothing worth measuring: no brake, the law eases in
constexpr float BRAKE_WINDOW_MAX_S = 300.0f;
constexpr bool F4_UNWIND = V2_F4A;         // the integral leaks while the crest is above SP
constexpr bool F4_PERSIST = V2_F4B;        // a big level mismatch that repeats is learned
constexpr float UNWIND_TAU_FACTOR = 2.0f;  // integral leak time constant = factor * theta
constexpr uint8_t LEVEL_PERSIST_N = 3;     // a level mismatch above LEVEL_SLOPE_GATE that repeats this many 10 s steps with one sign is not a one-off
constexpr bool F10A_STUCK = V2_F10A, F10B_LEAK = V2_F10B, F10C_PRED = V2_F10C;
constexpr float STUCK_GAIN = V2_STUCK_GAIN, STUCK_AFTER_S = V2_STUCK_AFTER, STUCK_REC_S = V2_STUCK_REC, STUCK_DEF_C = V2_STUCK_DEF, UNWIND_PV_C = 0.10f;
constexpr bool F10B_FLAT = V2_F10B_FLAT, F11_TRUST = V2_F11, F12_VENT = V2_F12;
#ifndef V2_FAN_SETTLE
#define V2_FAN_SETTLE 60.0f
#endif
constexpr float FAN_LAG_S = 10.0f, FAN_MAX_PCT = 60.0f, FAN_SETTLE_S = V2_FAN_SETTLE;
constexpr float STEADY_PA_PCT = 6.0f, TRUST_OVER_C = 0.15f, STEADY_FAN_S = 300.0f;
constexpr float GAP_TAU_S = V2_GAP_TAU, GAP_G0_C = V2_GAP_G0, GAP_SPAN_C = V2_GAP_SPAN;
constexpr bool F10D_FLATSLOPE = V2_F10D, F10E_SLOWGATE = V2_F10E;
constexpr float SLOWUP_ERR_PCT = V2_SLOWUP_ERR;
constexpr bool STUCK_NOARR = V2_STUCK_NOARR;
constexpr float C_BAND_C = 0.3f;
constexpr float F1_DM_FLOOR = V2_F1_DMF, SLOPE_Z = V2_SLOPE_Z, ASYM_HOLD = V2_ASYM_HOLD;
constexpr bool F5_FAR_SMOOTH = V2_F5;
constexpr float FAR_SMOOTH_S = 30.0f, FAR_MIN_DWELL_S = 30.0f, FAR_OFF_FRAC = 0.25f;
constexpr bool F8_GUARD = V2_F8;            // below SP: the power may not carry the crest over SP + GUARD_CEIL_C; above SP the request never rises while the air still climbs
constexpr float GUARD_CEIL_C = 0.1f;
constexpr float GUARD_MIN_DEF_C = V2_GUARDDEF;      // the guard only works on the approach from a deficit this deep; in the last stretch the law itself rules (a guard there makes the air hunt)
constexpr float GUARD_HORIZON_S = V2_GUARDH;       // a command is priced over this long: the crest is re-evaluated every step, so only the next moments are committed
constexpr bool F9_PATIENCE = V2_F9;         // the longest stall seen on the climb is the patience; the probe uses the MEASURED delay
constexpr float PATIENCE_MIN_S = 40.0f, PATIENCE_MAX_S = 300.0f, STALL_MIN_C = 0.04f;
constexpr bool F4C_ASYM = V2_F4C;          // the model's level may be pulled DOWN at once by a big mismatch (a level too high hides the coast), but is not pulled UP while a deficit stands still / falls (that is a door: the integral carries it)
constexpr float LEVEL_DOWN_STEP_MAX = 8.0f;
constexpr bool F6_CHASE = V2_F6;           // a deficit that does not move (a door, a wet tray) is not chased with more than what arrives + CHASE_PCT: the power committed now lands after the delay, when the cause may be gone
constexpr float CHASE_PCT = V2_CHASE_PCT, CHASE_AFTER_S = 60.0f;
constexpr int CHASE_MODE = V2_CHASE_MODE;     // 1: arriving + CHASE_PCT; 2: hold + the surplus that cannot overshoot more than SURPLUS_C if the cause vanishes; 3: the lower of both
constexpr float SURPLUS_C = V2_SURPLUS_C;
constexpr float CHASE_MIN_DEF_C = V2_CHASE_DEF;
constexpr float FALL_STEEP_MKS = 20.0f, FALL_AFTER_S = 40.0f;
constexpr int CHASE_FALL = V2_CHASE_FALL;         // 1: a falling air also counts (a door opening); 0: only a flat one
constexpr float CHASE_ARR_PCT = V2_CHASE_ARR;      // a flat deficit counts only while the power that ARRIVES is this much above the hold: a step of the air (tray, quick door) is no loss   // only a deficit this deep is a disturbance; a shallow one is the integral's job
constexpr float GUARD_K_FACTOR = V2_GUARDK;   // the below-SP guard uses the LEARNED k times this (the above-SP limiter keeps its fixed worst-case constant)
constexpr bool F7_SHADOW = V2_F7;           // the vent line blocks the heater: keep predicting and anchoring the level, command nothing
constexpr float PERMIT_RECOMPUTE_S = 120.0f;
constexpr float PV_BAD_FREEZE_S = 10.0f;   // a missing / absurd sensor value for longer than this: no power (the arbiter has its own sensor supervision; this block does not heat on a stale reading)
constexpr float PV_MIN_C = -20.0f, PV_MAX_C = 100.0f, SP_MIN_C = 20.0f, SP_MAX_C = 50.0f;
constexpr float FIT_RMS_MAX_MKS = 1.5f;    // mK/s: a fit that explains the slopes worse than this is not accepted (typical good fit 0.3..0.7)
constexpr float FIT_RMS_WORSE = 2.0f;      // a later fit worse than this factor times the previous one is not accepted
constexpr uint8_t FIT_RETRIES = 3;         // rejected first fits are retried this many times, every FIT_RETRY_S
constexpr float FIT_RETRY_S = 300.0f;
constexpr float GAP_KEEP_S = 0.0f;         // gaps in permit / sensor restart the learning window (0 = any gap). Keeping short gaps with held values made S3 worse (set A): left at 0, see G2 report
constexpr bool SOFT_CEILING = false;
constexpr bool UNIDENTIFIED_CAP = true;     // before the first accepted fit the power is capped by what is safe for the fastest oven of the family
constexpr float LIM_SOFT_MARGIN_C = 0.1f;  // soft ceiling for the limiter: the nearer of the vent and High lines, minus this
constexpr float BATCH_MAX_SPAN_S = 150.0f; // a batch fit that takes longer than this (slow call rate) is discarded: the history it reads would have moved
constexpr float TIME_REBASE_S = 1.0e6f;    // absolute seconds are rebased before float resolution (1 s at 2^24) becomes a problem

enum State : uint8_t { StIdle = 0, StClimb = 1, StTrack = 2, StFrozen = 3, StE115 = 4, StShadow = 5, StBrake = 6, StRecover = 7, StDegraded = 8 };

struct Input {
  float pv = NAN, sp = 37.5f, dQ = 0.1f, headroomC = 0.7f, alarmHeadroomC = 0.7f,   // headroomC: to the vent line; alarmHeadroomC: to the Emergency line (control sensor's frame)
  capPct = 100.0f, deliveredPct = 0.0f, e115Frac = 0.0f, dtS = 0.1f;
  bool newSample = false, permit = false;
  bool shadow = false;      // permit is false ONLY because the vent line blocks the heater (everything else is fine): the controller keeps running, commands 0
  bool ventFan = false;     // the exhaust fan is running (any reason): it takes heat out of the box
  float ventOnInS = 1.0e6f, ventOffInS = 1.0e6f;   // what the vent schedule KNOWS: seconds until the fan starts (0 = running) / until it stops; 1e6 = unknown
};
struct Output {
  float powerPct = 0.0f;
  uint8_t state = StIdle;
  bool powerLimited = false;
  uint8_t trustQ = 15;     // telemetry only: how far the law trusts its own prediction, 0..15 (never read back by anything)
};

// What survives a restart: the oven's identified path and loss model (the last accepted batch fit).  Nothing about the eggs or the room is kept.
struct Prior {
  float th[NP] = {0, 0, 0, 0, 0};     // a (= 100k, scaled), c0, cp, ce, c3 of the fit
  float td = 0, tau = 0, kStd = 0, rms = 0;
  uint8_t tauEsel = 0;
};

class Controller {
 public:
  Controller() { reset(); }
  void reset() {
    for (auto &b : binU_) b = 0;
    for (auto &x : pvH_) x = 0.0f;
    for (auto &b : histOk_) b = false;
    for (auto &x : bla_) x = 0.0f;
    for (auto &t : teJ_) t = 0.0f;
    for (auto &r : teH_) for (auto &x : r) x = 0.0f;
    for (auto &r : bx_) for (auto &x : r) x = 0.0f;
    idPower_ = 100.0f; bidx_ = 0; bsub_ = 0; brows_ = 0; pvBadS_ = 0.0f; idT0_ = idPv0_ = idTTheta_ = 0.0f; lastPushAbs_ = 0.0f;
    for (auto &x : pvRing_) x = 0.0f;
    for (auto &x : paRing_) x = 0.0f;
    binHead_ = 0; ringCount_ = 0; secT_ = 0; secAcc_ = 0; ring10_ = 0; lag_ = 0; absS_ = 0;
    tau_ = TAU_LAG_S; setDelay(THETA_PRIOR_S - THETA_PICKUP_S);
    histCount_ = 0; histHead_ = 0; firstPowerAbs_ = -1.0f; batchStage_ = 0; batchDue_ = false; batchActive_ = false; kFrozen_ = false; kStdB_ = 0; dBias_ = 0; fitRejects_ = 0; prevFitRms_ = 0.0f;
    lastFitOk_ = false; trusted_ = false; wDead_ = false; fitSerial_ = 0;
    respPv_ = NAN; respT_ = 0.0f; respE_ = 0.0f; respSuspect_ = false;
    I_ = 0; Te_ = NAN; pv_ = NAN; spNow_ = 37.5f; far_ = false; tauEsel_ = TAU_E_PRIOR; sigPred_ = 1.0f;
    idDone_ = false; idPhase_ = 0; rlsHold_ = SLOPE_N + 1; thetaSeen_ = false;
    th_[0] = A0; th_[1] = C0_0; th_[2] = CP_0; th_[3] = CE_0; th_[4] = C3_0;
    for (auto &r : P_) for (auto &v : r) v = 0.0f;
    P_[0][0] = A0_STD * A0_STD; P_[1][1] = C0_STD * C0_STD; P_[2][2] = CP_STD * CP_STD; P_[3][3] = CE_STD * CE_STD; P_[4][4] = C3_STD * C3_STD;
    lostS_ = 0; lastU_ = 0; haveSample_ = false; pvPred_ = 0;
    dQ_ = 0.1f; slopeMks_ = 0.0f; pvPredModel_ = 0.0f; eSm_ = NAN; farAgeS_ = 0.0f; coastGain_ = 1.0f; predMeas_ = 0.0f; slopeOk_ = false; shadowNow_ = false; cutActive_ = false;
    brakeActive_ = false; brakeDone_ = false; atSpOnce_ = false; tcN_ = 0; gateCnt_ = 0; gateSign_ = 0; patienceObs_ = 0.0f; stallRef_ = NAN; stallRunS_ = 0.0f; flatDeficitS_ = 0.0f; fallS_ = 0.0f; stuckS_ = 0.0f; slopeSeMks_ = 0.0f; gapEma_ = 0.0f; trust_ = 1.0f; dFan_ = 0.0f; fanF_ = fanFut_ = fanW_ = fanWin_ = 0.0f; fanNow_ = false; fanPrev_ = false; fanFutPrev_ = 0.0f; fanChgS_ = 1.0e6f;
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
  float predictedModel() const { return pvPredModel_; }
  float trust() const { return trust_; }
  float fanLoss() const { return dFan_; }
  float gapEma() const { return gapEma_; }
  float slopeMks() const { return slopeOk_ ? slopeMks_ : 0.0f; }
  float coastGain() const { return coastGain_; }
  uint8_t coastSamples() const { return tcN_; }
  float patienceSeconds() const { return patienceS(); }
  bool brakeRunning() const { return brakeActive_; }
  bool brakeFinished() const { return brakeDone_; }
  bool inShadow() const { return shadowNow_; }
  float teModel() const { return Te_; }
  float dNow() const { return demandAt(pv_); }
  uint8_t batchesDone() const { return batchStage_; }
  float clockS() const { return absS_; }
  uint8_t fitRejects() const { return fitRejects_; }
  float fitRms() const { return prevFitRms_; }
  float lastCandRms() const { return static_cast<float>(bestRms_); }
  bool batchRunning() const { return batchActive_; }
  bool responseSuspect() const { return respSuspect_; }
  bool identified() const { return idDone_; }
  uint8_t fitSerial() const { return fitSerial_; }                 // +1 per accepted batch fit: the signal to store a new Prior
  bool exportPrior(Prior &p) const {
    if (!lastFitOk_) return false;
    for (int i = 0; i < NP; ++i) p.th[i] = th_[i];
    p.td = tdEst_; p.tau = tau_; p.kStd = kStdB_; p.rms = prevFitRms_; p.tauEsel = tauEsel_;
    return true;
  }
  // Only before anything was learned in this run.  A restored path is treated as less certain than a fresh fit (k uncertainty >= 15 %, egg offset forgotten),
  // the power cap of the unidentified state is lifted, and the second batch fit (1500 s after the first power) re-checks it against the oven as it is now.
  bool importPrior(const Prior &p) {
    if (batchStage_ != 0 || idPhase_ != 0 || absS_ > 60.0f) return false;
    for (int i = 0; i < NP; ++i) if (!std::isfinite(p.th[i])) return false;
    if (!std::isfinite(p.td) || !std::isfinite(p.tau) || !std::isfinite(p.kStd) || !std::isfinite(p.rms)) return false;
    if (p.th[0] < K_MIN * 1e5f * 1.02f || p.th[0] > K_MAX_EST * 1e5f * 0.98f) return false;
    if (p.th[2] < 0.5f || p.th[2] > 9.0f || p.th[3] < 0.2f || p.th[3] > p.th[2]) return false;
    if (p.th[1] < -10.0f || p.th[1] > 40.0f || p.kStd > K_MAX_EST || p.rms > 20.0f) return false;
    if (p.td < TD_MIN_S + 0.1f || p.td > TD_MAX_S - 0.1f || p.tau < 10.0f || p.tau > 120.0f || p.kStd < 0.0f || p.rms < 0.0f || p.tauEsel >= 3) return false;
    for (int i = 0; i < NP; ++i) th_[i] = p.th[i];
    th_[4] = 0.0f;                                                  // the egg start offset belonged to that start
    tau_ = p.tau; setDelay(p.td); tauEsel_ = p.tauEsel;
    kStdB_ = p.kStd; if (kStdB_ < 0.15f * kRaw()) kStdB_ = 0.15f * kRaw();
    kFrozen_ = true; for (int q = 0; q < NP; ++q) { P_[0][q] = 0.0f; P_[q][0] = 0.0f; }
    prevFitRms_ = p.rms; batchStage_ = 1; trusted_ = true; dBias_ = 0.0f; idDone_ = true; idPhase_ = 3; rlsHold_ = SLOPE_N + 1;
    return true;
  }

  Output step(const Input &in) {
    Output out;
    const float dt = !std::isfinite(in.dtS) ? 0.1f : (in.dtS < 0.001f ? 0.001f : (in.dtS > 1.0f ? 1.0f : in.dtS));
    const float cap = (std::isfinite(in.capPct) && in.capPct > 0.0f) ? (in.capPct > 100.0f ? 100.0f : in.capPct) : 0.0f;
    const bool pvOk = std::isfinite(in.pv) && in.pv > PV_MIN_C && in.pv < PV_MAX_C;
    const bool spOk = std::isfinite(in.sp) && in.sp > SP_MIN_C && in.sp < SP_MAX_C;
    const float dQ = (std::isfinite(in.dQ) && in.dQ >= 0.001f && in.dQ <= 1.0f) ? in.dQ : 0.1f;
    fineSensor_ = dQ <= 0.02f; dQ_ = dQ;
    shadowNow_ = F7_SHADOW && in.shadow && !in.permit && idDone_;      // shadow only after the identification: nothing to learn about the delay while the heater cannot run
    const bool live = in.permit || shadowNow_;
    if (pvOk) pvBadS_ = 0.0f; else pvBadS_ += dt;
    if (F12_VENT) {                                                  // what the fan does now (for the learner) and what it will do when the power asked now lands (for the law)
      const bool fanKnown = in.ventOnInS < 1.0e5f || in.ventOffInS < 1.0e5f;        // only a fan whose run the vent schedule announces is modelled: one unannounced run (the vent line, the Web, an hour-of-day list) is too little to learn a loss from
      fanNow_ = in.ventFan && fanKnown;
      fanF_ += ((fanNow_ ? 1.0f : 0.0f) - fanF_) * (dt >= FAN_LAG_S ? 1.0f : dt / FAN_LAG_S);
      const float lead = tdPrime(), horizon = lead + tau_;
      const float fa = fanNow_ ? 0.0f : in.ventOnInS, fb = fanNow_ ? in.ventOffInS : in.ventOffInS;     // the next (or the present) run of the fan: [fa, fb] seconds from now
      fanFut_ = (fa <= lead && lead < fb) ? 1.0f : 0.0f;                                                 // at the moment a power asked now lands
      const float ov = std::fmin(fb, horizon) - std::fmax(fa, 0.0f);                                     // the share of the prediction horizon in which the fan runs
      fanWin_ = ov > 0.0f ? (ov >= horizon ? 1.0f : ov / horizon) : 0.0f;
      if (fanNow_ != fanPrev_ || fanFut_ != fanFutPrev_) fanChgS_ = 0.0f; else fanChgS_ += dt;
      fanPrev_ = fanNow_; fanFutPrev_ = fanFut_;
    } else { fanNow_ = false; fanF_ = fanFut_ = fanWin_ = 0.0f; }
    fanW_ = fanFut_;
    advanceClock(in, dt);
    if (!live && lostS_ > GAP_KEEP_S) batchActive_ = false;   // a long loss of permit: the history is not a fit window
    if (batchDue_ && !batchActive_ && in.permit && histCount_ >= BATCH_MIN_POINTS) batchBegin();
    if (batchActive_) { if (absS_ - absRef_ > BATCH_MAX_SPAN_S) batchActive_ = false; else batchStep(); }
    if (pvOk) {
      if (!haveSample_) { for (auto &t : teJ_) t = in.pv; Te_ = in.pv; haveSample_ = true; }
      if (in.newSample) {
        for (uint8_t j = 0; j < 3; ++j) teJ_[j] += (in.pv - teJ_[j]) * (2.0f / TAU_E_C[j]);
        Te_ = teJ_[tauEsel_];
        if (spOk && in.permit) { Input ci = in; ci.dQ = dQ; identify(ci); }
      }
      pv_ = in.pv;
    }
    if (!live || !haveSample_ || !pvOk || pvBadS_ > PV_BAD_FREEZE_S || cap <= 0.0f || !spOk) {
      lostS_ += dt;
      if (lostS_ > PERMIT_RECOMPUTE_S) { I_ = 0; if (!idDone_) idPhase_ = 0; }
      if (lostS_ > 1.0e6f) lostS_ = 1.0e6f;
      lastU_ = 0.0f; out.state = StFrozen; return out;
    }
    lostS_ = 0; spNow_ = in.sp;
    if (trusted_ && pv_ >= in.sp - 0.3f) atSpOnce_ = true;
    const float k = kUsed();
    const float Ttr = tdPrime() + tau_;
    // the demand grows with the air while it climbs (egg flow ~ PV - Te): pay for it at the middle of the predicted trajectory, not at the present PV
    const float Etr = transitEnergy();
    fanW_ = fanWin_;                                             // the prediction looks over the whole horizon, the feed-forward only at the moment a power asked now lands
    const float p0 = pv_ + k * (Etr - clampPos(demandAt(pv_)) * Ttr);
    const float pMid = 0.5f * (pv_ + (p0 < in.sp ? p0 : in.sp));
    const float dMid = clampPos(demandAt(pMid));
    pvPredModel_ = pv_ + k * (Etr - dMid * Ttr);
    pvPred_ = pvPredModel_;
    // G7/F1: the model's loss level can be wrong (a door, a wet tray, a level learned in a climb); the air's own measured slope cannot.  What the air will still
    // gain if the power were cut now is at least its slope times the coast time (learned from every cut), so the crest is the higher of the two.
    if (F1_KIN_PRED && trusted_ && slopeOk_ && ringCount_ >= SLOPE_N) {
      // the same energy balance with the demand the air itself just showed: D_meas = arriving power - slope / k (what levelUpdate() uses), over the energy really in flight
      float pam = 0.0f; for (uint8_t i = RING_N - SLOPE_N; i < RING_N; ++i) pam += paRing_[i]; pam *= 1.0f / SLOPE_N;
      // G8/F10C: the crest is raised only by a rise the slope shows beyond its own noise (a 0.1 degC step of the sensor looks like 2 mK/s over 60 s), and a negative demand is bounded
      const bool holdBand = F10C_PRED && std::fabs(in.sp - pv_) < C_BAND_C;      // the noise bound is for the hold band; a rebound from a deep dip (a door) is a real signal
      const float slEff = holdBand ? slopeMks_ - SLOPE_Z * slopeSeMks_ : slopeMks_;
      float dm = pam - 100.0f * slEff / th_[0];
      const float dmLo = holdBand ? -F1_DM_FLOOR : D_MIN;
      if (dm < dmLo) dm = dmLo; else if (dm > D_MAX) dm = D_MAX;
      const float margin = 0.5f * kinMinSlope() * 1e-3f * Ttr;       // the slope's own noise over the transit: the model is trusted unless the air disagrees by more than this
      const float meas = pv_ + coastGain_ * k * (Etr - dm * Ttr);
      predMeas_ = meas;
      if (meas - margin > pvPred_) pvPred_ = meas - margin;
    }
    const bool predSaysOver = pvPred_ > in.sp + TRUST_OVER_C;                   // an expected overshoot is always respected
    const float wTrust = (F11_TRUST && !predSaysOver && in.sp - pv_ <= 0.5f && in.sp - pv_ >= -0.3f) ? trust_ : 1.0f;     // only inside the band where a stuck air lives: a climb or an overshoot is the prediction's job
    fanW_ = fanFut_;
    const float predUsed = pv_ + wTrust * (pvPred_ - pv_);      // G8: the supervisor's weight: 1 = the prediction as it is, 0 = the measured PV
    const float e = in.sp - predUsed;
    const float eFar = in.sp - pvPred_;                                           // far mode and the brake work on the prediction itself
    // uncertainty of that prediction: k (relative), and the demand model (learner covariance) paid over the transit
    sigPred_ = predictionSigma(k, Etr, dMid, Ttr, pMid);
    const float eOn = std::fmax(E_ON_MIN, Z_SIGMA * sigPred_);
    const float Kc = 1.0f / (k * 3.0f * theta_);                 // SIMC for an integrating plant, tau_c = 2*theta
    const float tauI = TAUI_FACTOR * theta_;
    const bool airUnderSp = atSpOnce_ && in.sp - pv_ > -0.05f;          // G8: the crest is predicted above SP but the air is not (the prediction wobbles +-0.15 degC, a gain of ASYM would rectify it into a lower mean)
    const float K = (e >= 0.0f) ? KUP * Kc : (airUnderSp ? ASYM_HOLD : ASYM) * Kc;
    if (shadowNow_) {                                            // the vent line blocks the heater: the model keeps running (predicting, anchoring the level), nothing is commanded, the integral waits
      brakeActive_ = false; far_ = false; farAgeS_ = 0.0f; eSm_ = eFar;
      out.state = StShadow; lastU_ = 0.0f; return out;
    }
    if (F5_FAR_SMOOTH) {
      // far mode on the SMOOTHED deficit, a dwell, and a wider way out: the prediction wobbles by +-0.05 degC and used to flip 20 <-> 100 % every 10 s
      if (!std::isfinite(eSm_)) eSm_ = eFar;
      eSm_ += (eFar - eSm_) * (dt >= FAR_SMOOTH_S ? 1.0f : dt / FAR_SMOOTH_S);
      farAgeS_ += dt;
      if (!far_) { if (eSm_ > eOn && eFar > eOn) { far_ = true; farAgeS_ = 0.0f; } }
      else if (eFar <= 0.0f || (eFar < FAR_OFF_FRAC * eOn && farAgeS_ >= FAR_MIN_DWELL_S)) far_ = false;
    } else if (eFar > eOn) far_ = true; else if (eFar < 0.5f * eOn) far_ = false;
    // G7/F3: the first approach is stopped at the predicted crest and the coast is measured (the oven may drift on by itself: remember it)
    if (brakeStep(in)) {
      far_ = false; out.state = StBrake; out.powerPct = 0.0f; lastU_ = 0.0f; return out;
    }
    // The sensor reports in steps of dQ: the air can sit anywhere within +-dQ/2 of a reading, so a one-step change of the reading is not a full dQ of error.
    // The proportional action works on the error minus the dead band; the integral (which averages over time) keeps the full error.
    const bool e115Quiet = in.e115Frac < E115_QUIET_FRAC;       // the heater-not-heating watchdog is accumulating: nothing may hold the power down
    const bool chaseOn = F6_CHASE && e115Quiet && trusted_ && idDone_ && idPhase_ != 1 && idPhase_ != 2 && (flatDeficitS_ >= CHASE_AFTER_S || (CHASE_MODE >= 2 && CHASE_FALL && fallS_ >= FALL_AFTER_S)) && pv_ < in.sp - CHASE_MIN_DEF_C;
    const float dz = DEADZONE_FRAC * in.dQ;
    const float eP = e > dz ? e - dz : (e < -dz ? e + dz : 0.0f);
    const float uRaw = far_ ? cap : feedForward() + K * eP + I_;
    float u = uRaw < 0.0f ? 0.0f : (uRaw > cap ? cap : uRaw);
    const bool satHigh = uRaw >= cap, satLow = uRaw < 0.0f;
    if (!chaseOn && !((satHigh && e > 0.0f) || (satLow && e < 0.0f && !(F4_UNWIND && I_ > 0.0f)))) {      // conditional integration (a positive integral may still unwind)
      float eI = e, gI = 1.0f;
      if (F10A_STUCK && stuckS_ >= STUCK_AFTER_S && in.sp - pv_ > e) { eI = in.sp - pv_; gI = STUCK_GAIN; }     // G8: the measured deficit, faster
      I_ += Kc / tauI * gI * eI * dt;
      if (I_ > I_MAX) I_ = I_MAX; else if (I_ < -I_MAX) I_ = -I_MAX;
    }
    // G7/F4: the air has overtaken the integral (the crest is above SP): it leaks away instead of waiting to be integrated out, which takes tens of minutes
    if (F4_UNWIND && e < -0.05f && I_ > 0.0f && !(F10B_LEAK && pv_ < in.sp - UNWIND_PV_C && (!F10B_FLAT || (slopeOk_ && std::fabs(slopeMks_) <= flatLimit())))) { I_ -= I_ * dt / (UNWIND_TAU_FACTOR * theta_); if (I_ < 0.0f) I_ = 0.0f; }
    if (idPhase_ == 1 || idPhase_ == 2) {                           // blind / probe phase: power from the CURRENT room below SP, never above the safe level
      const float pp = probePower(in.sp, pv_);
      idPower_ = pp; u = pp < cap ? pp : cap; out.state = StClimb; far_ = false;
    }
    if (UNIDENTIFIED_CAP && !trusted_ && idPhase_ != 1 && idPhase_ != 2) {   // no accepted fit yet and no probe running (priors path, e.g. a restart): never above the family-safe power
      const float pp = probePower(in.sp, pv_);
      if (u > pp) { u = pp; far_ = false; }
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
      float ceilC = in.sp + (in.alarmHeadroomC > LIM_MARGIN_C ? in.alarmHeadroomC - LIM_MARGIN_C : 0.0f);
      const float soft = in.sp + (in.headroomC > LIM_SOFT_MARGIN_C + 0.1f ? in.headroomC - LIM_SOFT_MARGIN_C : 0.1f);     // vent / High line (control sensor frame)
      if (SOFT_CEILING && std::isfinite(in.headroomC) && soft < ceilC) ceilC = soft;
      const float uLim = LIM_H_LO + (ceilC - pv_ - inflight) / (LIM_K_HI * LIM_NEW_S);
      if (pv_ >= in.sp && uLim < u) { u = uLim > 0.0f ? uLim : 0.0f; far_ = false; }       // above SP only: a deep dip must be allowed to climb back at full power
    }
    if (chaseOn) {                                                   // G7/F6: flat below SP: the request may exceed what arrives by CHASE_PCT only
      const float pa = (paRing_[RING_N - 1] + paRing_[RING_N - 2] + paRing_[RING_N - 3]) * (1.0f / 3.0f);
      float cl = pa + CHASE_PCT;
      const float sur = SURPLUS_C / (k * (Ttr + 60.0f));                 // % over the hold that, if the cause vanished now, would still gain only SURPLUS_C degC while it lands
      const float c2 = clampPos(feedForward()) + sur;
      if (CHASE_MODE == 2) cl = c2; else if (CHASE_MODE == 3 && c2 < cl) cl = c2;
      if (u > cl) u = cl;
    }
    // G7/F8: the same kind of limiter BELOW SP, on the (learned) crest and SP + GUARD_CEIL_C, never below the power that holds SP; and above SP the request never rises while the air climbs
    if (F8_GUARD && e115Quiet && trusted_ && idDone_ && idPhase_ != 1 && idPhase_ != 2) {
      if (pv_ < in.sp - GUARD_MIN_DEF_C) {
        const float rem = in.sp + GUARD_CEIL_C - (pvPred_ > pv_ ? pvPred_ : pv_);
        const float kG = GUARD_K_FACTOR * k;
        const float hold = clampPos(feedForward());
        float uG = hold + rem / (kG * GUARD_HORIZON_S);                  // the power that, held for GUARD_HORIZON_S, brings the crest to SP + GUARD_CEIL_C
        if (uG < 0.0f) uG = 0.0f;
        if (uG < u) u = uG;
      } else if (V2_MONO && pv_ >= in.sp && slopeOk_ && slopeMks_ >= 0.0f && u > lastU_) u = lastU_;
    }
    out.state = StTrack;
    if (F11_TRUST && trust_ < 0.5f) out.state = StDegraded;
    if (F10A_STUCK && stuckS_ >= STUCK_AFTER_S) out.state = StRecover;
    if (!idDone_ && idPhase_ == 0 && ringCount_ < LOOK_RING) { u = 0.0f; far_ = false; }       // first look: nothing is commanded
    if (in.e115Frac >= E115_CLIMB_FRAC && pv_ < in.sp) {          // heater on for long without the air rising 0.3: ask for more, in proportion
      const float boost = E115_BOOST_MAX * (in.e115Frac - E115_CLIMB_FRAC) / (1.0f - E115_CLIMB_FRAC);
      const float ub = u + boost; u = ub > cap ? cap : ub; out.state = StE115;
    } else if (u >= cap && e > 0.0f) out.state = StClimb;
    if (respSuspect_) { u = 0.0f; far_ = false; }                    // power that arrived did nothing: withdraw the request until the PV moves
    if (!(u >= 0.0f)) u = 0.0f;                                     // also catches NaN
    if (u > cap) u = cap;
    out.powerLimited = (u >= cap) && cap < 100.0f;
    out.powerPct = u; lastU_ = u;
    out.trustQ = static_cast<uint8_t>(trust_ * 15.0f + 0.5f);
    return out;
  }

 private:
  // ---- G7 helpers ----
  float flatLimit() const { const float k = kinMinSlope(); return k > 1.5f ? k : 1.5f; }
  float kinMinSlope() const { const float f = 25.0f * dQ_; return f > KIN_SLOPE_MIN_MKS ? f : KIN_SLOPE_MIN_MKS; }    // mK/s: a 60 s slope must be at least ~2.5 sensor steps per 100 s to count
  float patienceS() const {
    float p = patienceObs_ > PATIENCE_MIN_S ? patienceObs_ : PATIENCE_MIN_S;
    return p > PATIENCE_MAX_S ? PATIENCE_MAX_S : p;
  }
  float coastWindowS() const {
    float w = 1.5f * (tdPrime() + tau_) + (F9_PATIENCE ? patienceS() : 60.0f);
    return w > BRAKE_WINDOW_MAX_S ? BRAKE_WINDOW_MAX_S : w;
  }
  // power of the blind / probe climb: safe for the fastest oven of the family over the transit it can still hide.  Once the delay has been MEASURED the transit
  // is td + the longest lag + 10 s, not the worst case of the prior range, so the climb does not crawl at the power of the slowest-to-be-measured oven.
  float probePower(float sp, float pv) const {
    float T = T_MAX_S;
    if (F9_PATIENCE && thetaSeen_ && idPhase_ == 2) { const float t2 = tdEst_ + TAU_L_C[3] + 10.0f; if (t2 < T) T = t2; }
    float pp = H_LO + ID_SAFETY * (sp - pv - ID_RISE_C) / (K_FAST * T);
    if (!(pp >= ID_P_MIN)) pp = ID_P_MIN;
    return pp;
  }
  // the coast the air really showed after a cut, against the rise the law expected at that moment: the ratio scales the measured-demand prediction from now on
  void learnCoast(float coastC, float expectedC, float w) {
    if (!(expectedC >= 0.15f) || !std::isfinite(coastC)) return;
    float ratio = (coastC > 0.0f ? coastC : 0.0f) / expectedC;
    if (ratio < GAIN_LO) ratio = GAIN_LO; else if (ratio > GAIN_HI) ratio = GAIN_HI;
    coastGain_ = (tcN_ == 0) ? ratio : coastGain_ + w * (ratio - coastGain_);
    if (tcN_ < 255) ++tcN_;
  }
  // average delivered power [%] over the bins (age 1 = the newest second) from..to inclusive
  float binAvg(uint16_t from, uint16_t to) const {
    float sum = 0.0f; for (uint16_t i = from; i <= to; ++i) sum += binU_[(binHead_ + BIN_N - i) % BIN_N] * 0.5f;
    return sum / static_cast<float>(to - from + 1);
  }
  // 60 s least-squares slope of the PV ring (every 10 s)
  void slopeUpdate(bool gap) {
    slopeOk_ = false;
    if (gap || ringCount_ < SLOPE_N) return;
    const uint8_t n = SLOPE_N; float sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (uint8_t i = 0; i < n; ++i) { const float t = i * 10.0f, y = pvRing_[RING_N - n + i]; sx += t; sy += y; sxx += t * t; sxy += t * y; }
    const float sdd = sxx - sx * sx / n, b = (n * sxy - sx * sy) / (n * sxx - sx * sx), a0 = (sy - b * sx) / n;
    float sse = 0.0f; for (uint8_t i = 0; i < n; ++i) { const float r = pvRing_[RING_N - n + i] - (a0 + b * i * 10.0f); sse += r * r; }
    slopeSeMks_ = sdd > 0.0f ? 1000.0f * std::sqrt(sse / (n - 2) / sdd) : 0.0f;
    slopeMks_ = 1000.0f * b;
    slopeOk_ = std::isfinite(slopeMks_);
  }
  // every cut of the power, whoever made it (this law, the vent line, a safety layer), is a free measurement of the coast
  void coastTrack() {
    if (!F2_COAST_LEARN || !trusted_) { cutActive_ = false; return; }
    const float a10 = binAvg(1, 10), a60 = binAvg(11, 70);
    if (!cutActive_) {
      if (!brakeActive_ && a10 <= CUT_AVG_LO && a60 >= CUT_AVG_HI && slopeOk_ && slopeMks_ >= CUT_MIN_SLOPE_MKS) {
        cutActive_ = true; cutT0_ = absS_; cutPv0_ = pv_; cutRise_ = pvPred_ - pv_; cutPeak_ = pv_;
      }
      return;
    }
    if (pv_ > cutPeak_) cutPeak_ = pv_;
    const float el = absS_ - cutT0_;
    if (a10 >= 15.0f || brakeActive_) { cutActive_ = false; return; }          // the power came back: not a clean coast
    if (el >= coastWindowS() || (el >= 40.0f && slopeOk_ && slopeMks_ <= 0.5f) || pv_ < cutPeak_ - 0.15f) {
      learnCoast(cutPeak_ - cutPv0_, cutRise_, 0.5f);
      cutActive_ = false;
    }
  }
  // the longest time the PV stood still while the heater was working hard far below SP: the oven's own patience, measured on the way up
  void flatTrack() {
    const float flatLim = kinMinSlope() > 1.5f ? kinMinSlope() : 1.5f;
    const float pam = (paRing_[RING_N - 1] + paRing_[RING_N - 2] + paRing_[RING_N - 3]) * (1.0f / 3.0f);
    const bool arrives = pam >= clampPos(feedForward()) + CHASE_ARR_PCT;
    if (trusted_ && slopeOk_ && std::fabs(slopeMks_) < flatLim && pv_ < spNow_ - 0.15f && arrives) flatDeficitS_ += 10.0f; else flatDeficitS_ = 0.0f;
    // G8/F11: when the air stands still the prediction must equal it (the energy in flight is then just what the demand eats); a persistent gap is a wrong level of the model
    if (F11_TRUST) {
      // steady = the air is flat, close to SP, it has been at SP once, and the power that arrives has not changed over the ring window (then the energy in flight is only what the demand eats)
      float paLo = 1.0e9f, paHi = -1.0e9f;
      for (uint8_t i = 0; i < RING_N; ++i) { if (paRing_[i] < paLo) paLo = paRing_[i]; if (paRing_[i] > paHi) paHi = paRing_[i]; }
      const bool still = trusted_ && idDone_ && atSpOnce_ && slopeOk_ && ringCount_ >= RING_N && !brakeActive_ && !shadowNow_ && idPhase_ != 1 && idPhase_ != 2 &&
                         std::fabs(slopeMks_) <= flatLimit() && std::fabs(pv_ - spNow_) < 0.5f && (paHi - paLo) <= STEADY_PA_PCT && fanChgS_ >= STEADY_FAN_S && fanF_ < 0.05f + (fanNow_ ? 1.0f : 0.0f);
      if (still) gapEma_ += (pvPred_ - pv_ - gapEma_) * (10.0f / GAP_TAU_S);
      const float g0 = GAP_G0_C + 0.5f * dQ_, ag = std::fabs(gapEma_);
      float w = 1.0f - (ag - g0) / GAP_SPAN_C;
      trust_ = w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
    }
    // G8: a flat or slowly drifting deficit that the present slope would not close within STUCK_REC_S, with the air already at SP once: the law is satisfied by a prediction the air does not follow
    {
      const float def = spNow_ - pv_;
      const bool inBand = trusted_ && idDone_ && atSpOnce_ && idPhase_ != 1 && idPhase_ != 2 && slopeOk_ && def > STUCK_DEF_C && def < 1.0f && std::fabs(slopeMks_) <= flatLim;
      if (F10A_STUCK && inBand && !(STUCK_NOARR && arrives) && slopeMks_ < def / STUCK_REC_S * 1000.0f) stuckS_ += 10.0f;
      else if (!F10A_STUCK || !inBand || def < 0.5f * STUCK_DEF_C) stuckS_ = 0.0f;
    }
    // a steady moderate decline is a loss (a door held open); a sudden steep one is a step of the air, which the energy balance already handles
    if (trusted_ && slopeOk_ && slopeMks_ < -kinMinSlope() && slopeMks_ > -FALL_STEEP_MKS && pv_ < spNow_ - 0.15f) fallS_ += 10.0f; else fallS_ = 0.0f;
  }
  void stallTrack() {
    if (!F9_PATIENCE) return;
    const float thr = dQ_ > STALL_MIN_C ? dQ_ : STALL_MIN_C;
    const bool climbing = trusted_ && pv_ < spNow_ - 1.0f && paRing_[RING_N - 1] >= 15.0f;
    if (!climbing) { stallRef_ = NAN; stallRunS_ = 0.0f; return; }
    if (!std::isfinite(stallRef_) || std::fabs(pv_ - stallRef_) >= thr) { stallRef_ = pv_; stallRunS_ = 0.0f; return; }
    stallRunS_ += 10.0f;
    if (stallRunS_ > patienceObs_) patienceObs_ = stallRunS_;
  }
  // F3: stop the first approach where the predicted crest reaches SP - BRAKE_BELOW_C (a fast oven sooner, a slow one later), watch how far the air drifts on
  bool brakeStep(const Input &in) {
    if (!F3_BRAKE || !trusted_ || !idDone_ || idPhase_ == 1 || idPhase_ == 2) return false;
    if (brakeActive_) {
      if (pv_ > brakePeak_) brakePeak_ = pv_;
      const float el = absS_ - brakeT0_;
      const bool early = V2_BRAKE_RESUME > 0.0f && el >= 30.0f && pvPred_ < in.sp - V2_BRAKE_RESUME;       // the crest is now far below SP: the law may take over, the coast is not worth waiting for
      const bool done = early || pv_ >= in.sp + 0.1f || pv_ < brakePeak_ - 0.15f || el >= coastWindowS() || (el >= 40.0f && slopeOk_ && slopeMks_ <= 0.5f);
      if (!done) return true;
      if (!early) learnCoast(brakePeak_ - brakePv0_, brakeRise_, 1.0f);
      if (V2_LEVELCLEAN) { float w = 1.3f * (tdPrime() + tau_); if (w > 300.0f) w = 300.0f; if (w < 60.0f) w = 60.0f;
        if (binAvg(1, static_cast<uint16_t>(w)) < CUT_AVG_LO) levelClean(); }   // nothing delivered for longer than a transit: nothing still arrives, what the air does is the loss alone
      brakeActive_ = false; brakeDone_ = true;
      return false;
    }
    if (brakeDone_) return false;
    if (pv_ >= in.sp - 0.05f) { brakeDone_ = true; return false; }   // reached without help: nothing to measure
    if (!slopeOk_ || slopeMks_ < CUT_MIN_SLOPE_MKS) return false;
    const float rise = pvPred_ - pv_;                                // what the law itself expects the air still to gain
    if (pvPred_ < in.sp - BRAKE_BELOW_C) return false;
    if (rise < BRAKE_MIN_COAST_C) { brakeDone_ = true; return false; }
    brakeActive_ = true; brakeT0_ = absS_; brakePv0_ = pv_; brakeRise_ = rise; brakePeak_ = pv_;
    return true;
  }
  // a clean window (no power arriving): the slope IS the loss, take the level to it without the 25 % gate
  void levelClean() {
    if (ringCount_ < SLOPE_N || !slopeOk_) return;
    float pvm = 0.0f; for (uint8_t i = RING_N - SLOPE_N; i < RING_N; ++i) pvm += pvRing_[i]; pvm /= SLOPE_N;
    const float meas = -100.0f * slopeMks_ / th_[0];
    float nb = dBias_ + 0.5f * (meas - demandAt(pvm)); if (nb > 40.0f) nb = 40.0f; else if (nb < -40.0f) nb = -40.0f;
    float step = nb - dBias_;
    float ni = I_ - step; if (ni > I_MAX) ni = I_MAX; else if (ni < -I_MAX) ni = -I_MAX;
    step = I_ - ni;
    dBias_ += step; I_ = ni;
  }
  static float clampPos(float v) { return v > 0.0f ? v : 0.0f; }
  float kRaw() const { const float k = th_[0] * 1e-5f; return k < K_MIN ? K_MIN : (k > K_MAX_EST ? K_MAX_EST : k); }
  // cautious k: estimate plus one standard deviation (a larger k means a smaller gain and an earlier stop)
  float kUsed() const { const float k = kRaw() + kStd(); return k > K_MAX_EST ? K_MAX_EST : k; }
  float aSafe() const { return th_[0] > 1.0f ? th_[0] : 1.0f; }
  float wEgg() const { return wDead_ ? 0.0f : std::exp(-absS_ / TAU_E_C[tauEsel_]); }       // after 25000 s (> 11 time constants) the start offset is gone for good, whatever the clock rebase does
  float haOf() const { return 100.0f * th_[3] / aSafe(); }                                   // egg exchange, % of bank per degC
  float demandAt(float pv) const {                                                            // demand if the air were at pv
    const float d = 100.0f * (th_[1] + th_[2] * (pv - P0) - th_[3] * (Te_ - P0) - th_[4] * wEgg()) / aSafe() + dBias_ + fanW_ * dFan_;
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
    const float dl = (in.permit && std::isfinite(in.deliveredPct)) ? (in.deliveredPct < 0.0f ? 0.0f : (in.deliveredPct > 100.0f ? 100.0f : in.deliveredPct)) : 0.0f;
    secAcc_ += dl * dt; secT_ += dt;
    while (secT_ >= 1.0f) {
      const float bin = secAcc_ / secT_; secAcc_ = 0; secT_ -= 1.0f; absS_ += 1.0f;
      if (absS_ > 25000.0f) wDead_ = true;
      if (absS_ >= TIME_REBASE_S) {                                  // keep float time exact: only differences of absolute seconds are ever used
        absS_ -= TIME_REBASE_S; lastPushAbs_ -= TIME_REBASE_S; idT0_ -= TIME_REBASE_S; idTTheta_ -= TIME_REBASE_S;
        if (firstPowerAbs_ >= 0.0f) { firstPowerAbs_ -= TIME_REBASE_S; if (firstPowerAbs_ < 0.0f) firstPowerAbs_ = 0.0f; }
        batchActive_ = false;
      }
      { const float bq = bin * 2.0f + 0.5f; binU_[binHead_] = static_cast<uint8_t>(bq > 255.0f ? 255.0f : (bq < 0.0f ? 0.0f : bq)); }
      if (firstPowerAbs_ < 0.0f && bin > 1.0f) firstPowerAbs_ = absS_;
      const uint16_t td = static_cast<uint16_t>(tdPrime() + 0.5f);
      const uint16_t idx = static_cast<uint16_t>((binHead_ + BIN_N - td) % BIN_N);   // the bin that leaves the transport delay now
      if (RESP_WATCHDOG && in.permit && pvBadS_ <= 0.0f) {
        const bool known = trusted_;                                              // after an accepted fit the account uses what was learned, with margins
        float mature = RESP_MATURE_S, loss = RESP_LOSS_PCT;
        if (known) {
          mature = tdPrime() + 2.0f * tau_ + 10.0f; if (mature < 60.0f) mature = 60.0f; if (mature > RESP_MATURE_S) mature = RESP_MATURE_S;
          loss = 1.5f * demandAt(std::isfinite(pv_) ? pv_ : 35.0f) + 10.0f; if (loss < 25.0f) loss = 25.0f; if (loss > RESP_LOSS_PCT) loss = RESP_LOSS_PCT;
        }
        const float m = binU_[(binHead_ + BIN_N - static_cast<uint16_t>(mature)) % BIN_N] * 0.5f;       // the bin delivered `mature` seconds ago
        respT_ += 1.0f; respE_ += m - loss;
        if (respT_ < mature || respE_ < 0.0f) respE_ = 0.0f;                             // nothing has matured yet; a period below the loss is no credit
        respNeed_ = RESP_MOVE_C / (known ? std::fmax(K_MIN, 0.5f * kRaw()) : K_MIN); if (respNeed_ < 600.0f) respNeed_ = 600.0f;
      }
      binHead_ = static_cast<uint16_t>((binHead_ + 1) % BIN_N);
      lag_ += (binU_[idx] * 0.5f - lag_) * (1.0f - std::exp(-1.0f / tau_));
      if (ring10_ >= 9) {
        ring10_ = 0;
        const bool gap = !(in.permit || shadowNow_) || pvBadS_ > 0.0f;
        if (!haveSample_ || !std::isfinite(pv_) || (gap && (lostS_ > GAP_KEEP_S || pvBadS_ > GAP_KEEP_S))) {      // nothing valid for too long: the learning window restarts
          ringCount_ = 0; rlsHold_ = SLOPE_N + 1; histCount_ = 0; batchDue_ = false;
        } else {
          if (gap) rlsHold_ = SLOPE_N + 1;                                // short gap: held values keep the 10 s grid, nothing is learned from them
          for (uint8_t i = 0; i + 1 < RING_N; ++i) { pvRing_[i] = pvRing_[i + 1]; paRing_[i] = paRing_[i + 1]; }
          pvRing_[RING_N - 1] = pv_; paRing_[RING_N - 1] = lag_;
          pvH_[histHead_] = pv_;
          for (uint8_t j = 0; j < 3; ++j) teH_[j][histHead_] = std::isfinite(teJ_[j]) ? teJ_[j] : pv_;
          histOk_[histHead_] = !gap;
          histHead_ = static_cast<uint8_t>((histHead_ + 1) % HIST_N); if (histCount_ < HIST_N) ++histCount_; lastPushAbs_ = absS_;
          if (ringCount_ < RING_N) ++ringCount_;
          if (RESP_WATCHDOG) {
            if (!std::isfinite(respPv_) || std::fabs(pv_ - respPv_) >= RESP_MOVE_C) { respPv_ = pv_; respT_ = 0.0f; respE_ = 0.0f; respSuspect_ = false; }
            else if (respE_ >= respNeed_ && pv_ < spNow_ - RESP_DEFICIT_C) respSuspect_ = true;
          }
          if (!batchActive_ && firstPowerAbs_ >= 0.0f && batchStage_ < 2 && absS_ - firstPowerAbs_ >= (batchStage_ == 0 ? BATCH1_S : BATCH2_S)) batchDue_ = true;
          slopeUpdate(gap); flatTrack(); coastTrack(); stallTrack();
          if (rlsHold_ > 0 && !gap) --rlsHold_;
          else if (!gap) { if (shadowNow_ || (F12_VENT && fanF_ > 0.05f)) levelUpdate(); else { eggObserver(); rlsUpdate(); levelUpdate(); } }       // shadow: the vent changes the loss, only the level may follow
        }
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
    if (F12_VENT && fanChgS_ < FAN_SETTLE_S) return;                  // the fan switched (or is about to land) a moment ago: the air and the arriving power are in a transient, nothing is learned from it
    const float save = fanW_; fanW_ = fanF_; levelUpdateCore(); fanW_ = save;
  }
  void levelUpdateCore() {
    if (!idDone_ || ringCount_ < RING_N || batchActive_) return;
    const bool fanLevel = F12_VENT && fanF_ > 0.5f;                  // while the fan runs the mismatch belongs to the fan, not to the oven
    float &lv = fanLevel ? dFan_ : dBias_;
    const float lvLo = fanLevel ? 0.0f : -40.0f, lvHi = fanLevel ? FAN_MAX_PCT : 40.0f;
    const float rise = pvRing_[RING_N - 1] - pvRing_[0];
    const bool flat = std::fabs(rise) <= LEVEL_FLAT_C;
    if (!flat && !LEVEL_SLOPE_MODE) return;
    float pa = 0, pvm = 0; for (uint8_t i = 0; i < RING_N; ++i) { pa += paRing_[i]; pvm += pvRing_[i]; }
    pa /= RING_N; pvm /= RING_N;
    float meas = pa;                                                  // flat: the power that arrives IS the demand
    if (F10D_FLATSLOPE && flat && std::fabs(rise) >= 2.0f * dQ_) {    // ... minus what the air still gains or loses: a "flat" 0.1 degC drift over 110 s is 5 % of the demand
      const float slopeW = 1000.0f * rise / (10.0f * (RING_N - 1));    // mK/s over the whole ring window
      meas = pa - 100.0f * slopeW / th_[0];
    }
    if (!flat) {                                                      // moving: demand = arriving power - slope / k (mK/s -> %)
      const uint8_t n = SLOPE_N; float sx = 0, sy = 0, sxx = 0, sxy = 0, pam = 0;
      for (uint8_t i = 0; i < n; ++i) { const float t = i * 10.0f, y = pvRing_[RING_N - n + i]; sx += t; sy += y; sxx += t * t; sxy += t * y; pam += paRing_[RING_N - n + i]; }
      const float slopeMks = 1000.0f * (n * sxy - sx * sy) / (n * sxx - sx * sx);
      pam /= n;
      meas = pam - 100.0f * slopeMks / th_[0];
      pvm = 0; for (uint8_t i = RING_N - n; i < RING_N; ++i) pvm += pvRing_[i]; pvm /= n;
    }
    const float err = meas - demandAt(pvm);
    bool slowUp = false;
    if (F4C_ASYM && err > 0.0f && (!F10E_SLOWGATE || err > SLOWUP_ERR_PCT) && pv_ < spNow_ - 0.15f && (flatDeficitS_ >= 30.0f || (slopeOk_ && slopeMks_ < -kinMinSlope()) || (atSpOnce_ && pv_ < spNow_ - 0.3f))) {
      if (V2_SLOWUP <= 0.0f) return;
      slowUp = true;                                                // a disturbance signature: a door must not become the level in minutes, a cold tray still does in tens of minutes
    }
    if (F4C_ASYM && err < -LEVEL_SLOPE_GATE) {                         // a level far too high: take it down at once, in bounded steps
      float step = LEVEL_SLOPE_GAIN * err; if (step < -LEVEL_DOWN_STEP_MAX) step = -LEVEL_DOWN_STEP_MAX;
      float nb = lv + step; if (nb < lvLo) nb = lvLo;
      step = nb - lv;
      float ni = I_ - step; if (ni > I_MAX) ni = I_MAX; else if (ni < -I_MAX) ni = -I_MAX;
      step = I_ - ni;
      lv += step; I_ = ni; gateCnt_ = 0; return;
    }
    if (std::fabs(err) > LEVEL_SLOPE_GATE) {                          // a mismatch this large is a door / vent / dead heater, not the demand ... once; a repeating one is the new level
      const int8_t sg = err > 0.0f ? 1 : -1;
      if (sg == gateSign_) { if (gateCnt_ < 255) ++gateCnt_; } else { gateSign_ = sg; gateCnt_ = 1; }
      if (!(F4_PERSIST && gateCnt_ >= LEVEL_PERSIST_N)) return;
    } else gateCnt_ = 0;
    float step = (flat ? LEVEL_GAIN : LEVEL_SLOPE_GAIN) * err;
    if (slowUp && !(F10A_STUCK && stuckS_ >= STUCK_AFTER_S)) step *= V2_SLOWUP;           // G8: not a door any more once it has lasted this long
    float nb = lv + step; if (nb > lvHi) nb = lvHi; else if (nb < lvLo) nb = lvLo;
    step = nb - lv;
    float ni = I_ - step; if (ni > I_MAX) ni = I_MAX; else if (ni < -I_MAX) ni = -I_MAX;
    step = I_ - ni;                                                  // what I_ could take is all that moves: the pair (model level, I) stays conserved
    lv += step; I_ = ni;
  }

  // Egg observer: Te is a STATE (a tray swap, warm eggs after a power cut move it in one step), the coefficients are parameters.  The innovation of the
  // 60 s slope against the model is turned into a correction of Te: slope has +ce*Te, so a slope lower than predicted means colder eggs than modelled.
  // It runs only where the innovation does not depend on an uncertain k: after the first batch fit, or while (almost) no heater power arrives.
  void eggObserver() {
    if (!EGG_OBSERVER_FINE || !fineSensor_ || batchActive_ || ringCount_ < SLOPE_N || !haveSample_) return;
    const uint8_t n = SLOPE_N; float sx = 0, sy = 0, sxx = 0, sxy = 0, pa = 0, pvm = 0;
    for (uint8_t i = 0; i < n; ++i) {
      const float t = i * 10.0f, y = pvRing_[RING_N - n + i];
      sx += t; sy += y; sxx += t * t; sxy += t * y; pa += paRing_[RING_N - n + i]; pvm += y;
    }
    pa /= n; pvm /= n;
    if (!(trusted_ || pa < OBS_MAX_LA_PRIOR)) return;
    const float slopeMks = 1000.0f * (n * sxy - sx * sy) / (n * sxx - sx * sx);
    const float pred = th_[0] * pa * 0.01f - (th_[1] + th_[2] * (pvm - P0) - th_[3] * (Te_ - P0) - th_[4] * wEgg());
    float nu = slopeMks - pred;                                       // mK/s
    const float ce = th_[3] > 0.5f ? th_[3] : 0.5f;
    if (std::fabs(nu) < OBS_DEADBAND_MKS) { bigNu_ = 0; return; }
    if (std::fabs(nu) > OBS_GATE_MKS) {                                  // a step of the AIR (door, vent) recovers within minutes; a cold tray does not
      if (bigNu_ < 255) ++bigNu_;
      if (bigNu_ < OBS_PERSIST_N) return;
      nu = nu > 0.0f ? OBS_GATE_MKS : -OBS_GATE_MKS;
    } else bigNu_ = 0;
    float dTe = OBS_GAIN * nu / ce;
    if (dTe > OBS_STEP_MAX_C) dTe = OBS_STEP_MAX_C; else if (dTe < -OBS_STEP_MAX_C) dTe = -OBS_STEP_MAX_C;
    for (auto &t : teJ_) t += dTe;
    Te_ = teJ_[tauEsel_];
  }

  // ---- the running learner: slope(mK/s) = a*La/100 - c0 - cp*(PV-P0) + ce*(Te-P0) + c3*w   (theta = [a, c0, cp, ce, c3]) ----
  void rlsUpdate() {
    if (RLS_NEEDS_BATCH && batchStage_ == 0) return;                                              // before the first batch fit the data cannot separate the unknowns: priors only
    if (batchActive_ || !haveSample_ || ringCount_ < SLOPE_N || (!idDone_ && idPhase_ == 1)) return;              // the delay must be known before P_air means anything
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
    if (!std::isfinite(err) || !std::isfinite(s2) || !(err * err <= GATE_SIGMAS * GATE_SIGMAS * s2)) return;           // outlier (door, tray, vent) or garbage: not a slope of this plant
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
    bns_ = histCount_ > HIST_N - BATCH_GUARD_POINTS ? static_cast<uint8_t>(HIST_N - BATCH_GUARD_POINTS) : histCount_;
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
        bool ok = true;
        for (int q = 0; q < 6; ++q) { const uint8_t h = static_cast<uint8_t>((bfirst_ + j - 5 + q) % HIST_N); if (!histOk_[h]) ok = false; pa += bla_[j - 5 + q]; pvm += pvH_[h]; num += (q - 2.5f) * pvH_[h]; }
        if (!ok) continue;                                       // a window that touches a gap (permit or sensor lost) is not a slope of the plant
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
  // P_air at the times of the CURRENT ring slots and at now, from the bins as they are now (the history moved while the batch ran)
  void rebuildRing(float td, float tau) {
    absRef_ = absS_; binHeadRef_ = binHead_;
    const float alpha = 1.0f - std::exp(-1.0f / tau), t0 = lastPushAbs_ - 10.0f * (RING_N - 1) - 150.0f;       // 150 s run-in (> 3 lags)
    float y = binAt(t0 - td);
    const int n = static_cast<int>(absS_ - t0 + 0.5f);
    for (int sidx = 1; sidx <= n; ++sidx) {
      const float tt = t0 + sidx; y += (binAt(tt - td) - y) * alpha;
      const int back = static_cast<int>(lastPushAbs_ - tt + 0.5f);
      if (back >= 0 && back % 10 == 0 && back / 10 < RING_N) paRing_[RING_N - 1 - back / 10] = y;
    }
    lag_ = y;
  }
  void batchApply() {
    batchActive_ = false;
    // a fit is accepted only if it is inside the searched family and explains the data: not on the delay grid edge, k not on its clamp, residual small
    const bool edge = bestTd_ <= TD_MIN_S + 0.1f || bestTd_ >= TD_MAX_S - 0.1f;
    const bool kEdge = bestTh_[0] <= K_MIN * 1e5f * 1.02 || bestTh_[0] >= K_MAX_EST * 1e5f * 0.98;
    const bool bad = !bestFound_ || respSuspect_ || edge || kEdge || bestRms_ > FIT_RMS_MAX_MKS || (batchStage_ > 0 && prevFitRms_ > 0.0f && bestRms_ > FIT_RMS_WORSE * prevFitRms_);
    if (bad) {
      ++fitRejects_;
      if (batchStage_ == 0 && fitRejects_ <= FIT_RETRIES) firstPowerAbs_ = absS_ - BATCH1_S + FIT_RETRY_S;   // try again later with more data
      else ++batchStage_;
      return;
    }
    ++batchStage_; prevFitRms_ = static_cast<float>(bestRms_); lastFitOk_ = true; trusted_ = true; ++fitSerial_;
    const double s2 = static_cast<double>(SLOPE_STD_MKS) * SLOPE_STD_MKS;
    tau_ = bestTau_; setDelay(bestTd_);
    tauEsel_ = bestTe_; Te_ = teJ_[tauEsel_];
    for (int p = 0; p < NP; ++p) th_[p] = static_cast<float>(bestTh_[p]);
    for (int p = 0; p < NP; ++p) for (int q = 0; q < NP; ++q) P_[p][q] = static_cast<float>(s2 * bestInv_[p][q]);
    clampTheta();
    // the arriving-power state and the last ring entries now follow the identified path (history replayed up to the moment the batch began, then to now)
    rebuildRing(bestTd_, bestTau_);
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
      if (in.sp - in.pv < ID_ROOM_C * 0.5f) { idDone_ = true; idPhase_ = 3; return; }       // the room has gone (SP lowered, or the air rose by itself)
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
  uint8_t bigNu_ = 0; float respPv_ = NAN, respT_ = 0.0f, respE_ = 0.0f, respNeed_ = 3750.0f; bool respSuspect_ = false; bool lastFitOk_ = false, trusted_ = false, wDead_ = false; uint8_t fitSerial_ = 0; bool fineSensor_ = false; bool idDone_ = false; uint8_t idPhase_ = 0, rlsHold_ = 0;
  uint8_t binU_[BIN_N];
  float pvH_[HIST_N], teH_[3][HIST_N], bla_[HIST_N], bx_[HIST_N][NP + 1], teJ_[3];
  bool histOk_[HIST_N];
  uint8_t histCount_ = 0, histHead_ = 0, batchStage_ = 0, tauEsel_ = TAU_E_PRIOR; bool batchDue_ = false, batchActive_ = false;
  uint8_t bns_ = 0, bfirst_ = 0, bsub_ = 0, brows_ = 0; float bA_[NP][NP], bb_[NP]; uint16_t bidx_ = 0; uint16_t binHeadRef_ = 0; float absRef_ = 0, lastPushRef_ = 0;
  double bth0_[NP], bsd_[NP], bestRms_ = 1e30, bestTh_[NP], bestInv_[NP][NP]; float bestTd_ = 0, bestTau_ = TAU_LAG_S; uint8_t bestTe_ = TAU_E_PRIOR; bool bestFound_ = false;
  uint8_t fitRejects_ = 0; float prevFitRms_ = 0.0f;
  bool kFrozen_ = false; float kStdB_ = 0, dBias_ = 0, sigPred_ = 1.0f, pvBadS_ = 0.0f;
  float lastPushAbs_ = 0, firstPowerAbs_ = -1.0f, tau_ = TAU_LAG_S;
  float pvRing_[RING_N], paRing_[RING_N];
  uint16_t binHead_; uint8_t ringCount_, ring10_;
  float secT_, secAcc_, lag_, absS_;
  float th_[NP], P_[NP][NP];
  bool far_ = false;
  float tdEst_ = 150.0f, thetaMeas_ = 0, pvAtTheta_ = 0; bool thetaSeen_ = false;
  float theta_, I_, Te_, pv_, spNow_, pvPred_ = 0, lastU_, lostS_;
  bool haveSample_;
  // G7 state
  float dQ_ = 0.1f, slopeMks_ = 0.0f, pvPredModel_ = 0.0f, predMeas_ = 0.0f, eSm_ = NAN, farAgeS_ = 0.0f, coastGain_ = 1.0f;
  bool slopeOk_ = false, shadowNow_ = false, cutActive_ = false, brakeActive_ = false, brakeDone_ = false;
  uint8_t tcN_ = 0, gateCnt_ = 0; int8_t gateSign_ = 0;
  float cutT0_ = 0, cutPv0_ = 0, cutRise_ = 0, cutPeak_ = 0, brakeT0_ = 0, brakePv0_ = 0, brakeRise_ = 0, brakePeak_ = 0;
  bool atSpOnce_ = false;
  float patienceObs_ = 0.0f, stallRef_ = NAN, stallRunS_ = 0.0f, flatDeficitS_ = 0.0f, fallS_ = 0.0f, stuckS_ = 0.0f, slopeSeMks_ = 0.0f, gapEma_ = 0.0f, trust_ = 1.0f, dFan_ = 0.0f, fanF_ = 0.0f, fanFut_ = 0.0f, fanW_ = 0.0f, fanWin_ = 0.0f, fanFutPrev_ = 0.0f, fanChgS_ = 1.0e6f; bool fanNow_ = false, fanPrev_ = false;
};

}  // namespace thermalv2
