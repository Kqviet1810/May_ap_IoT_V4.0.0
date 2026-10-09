# Smart AutoTune V1 — active identification of the installed oven (SIMULATION QUALIFIED, physical commissioning REQUIRED)

AutoTune is now the first step of the Adaptive Thermal system, not a stand-alone Ku/Pu finder:

```
BASELINE -> EXCITE -> COAST -> [EXCITE2] -> APPROACH -> NEAR_SP (relay, reused) -> SETTLE -> GENERATE -> VALIDATING -> SUCCESS | FAILED
```

Nothing is saved before VALIDATING passes. "Not sure -> do not accept": every doubt ends in FAILED and the old PID and profile stay.

## State machine (`thermal_smart_autotune.h`, engine `SmartAutoTune`; `RelayAutoTune` kept as the NEAR_SP stage and as the legacy engine)

| Phase | What happens | Exit / timeout | Abort |
|---|---|---|---|
| BASELINE | 90 s of observation, heater OFF: jump ≤ 0.6 °C, drift ≤ 0.0015 °C/s, noise ≤ 0.12 °C | 90 s (+30 s) → EXCITE or `BASELINE_UNSTABLE` | every control cycle |
| EXCITE | 25 % duty (capped by `maxHeaterPower`), running regression of PV vs delayed ACTUAL ON-seconds over 13 delay candidates | quality met (gain SE ≤ 10 %, delay separation ≥ 1.15, rise ≥ 0.5 °C, ≥ 4 delays long) or rise cap `min(2 °C, 35 % of headroom)` or 12 min | `NO_HEADROOM` if PV is within 1 °C of SP |
| COAST | heater OFF, track PV peak (coast rise / time) | no new maximum for 60 s after 1.2 θ + 30 s, or 8 min | `NO_HEADROOM` |
| EXCITE2 | at most ONE stronger excitation (35 %, hard cap) if gain/delay/coast are not trustworthy | same as EXCITE | else `NO_RESPONSE` / `MODEL_INVALID` |
| APPROACH | prediction brake `u = 40·(SP−0.6−PV)/coast100`, never above what would still coast under High | equilibrium (PV moved < 0.15 °C in 200 s) → hold at equilibrium is MEASURED; 90 min | `APPROACH_TIMEOUT`, `POWER_LIMITED` |
| NEAR_SP | the existing `RelayAutoTune` with a power derived from the plant (hold·1.15 + the part that coasts under High; ≥ 5 %, ≤ 60 %), 3 rolling cycles | relay limits 80 min total / 30 min per phase | `RELAY_FAILED` (+ relay reason), `POWER_LIMITED` |
| SETTLE | heater OFF until PV ≤ SP + 0.05 (and ≤ SP − 0.8 for up to 10 min) so the check starts from a real approach error | 15 min | – |
| GENERATE | SIMC candidate + model → ThermalProfile seed | instant | `CANDIDATE_INVALID` |
| VALIDATING | the normal controller runs the CANDIDATE with the assist Adaptive V1 will give a measured profile | 45 min (settle 30 min + scored tail = max(15 min, 2.5 relay periods)) | `VALIDATION_FAILED` |
| SUCCESS | one atomic config record, then profile seed + forced profile record | – | `SAVE_FAILED` |

Total hard limit 270 min. The machine aborts **every control cycle** (not only per sample) on: sensor lost/invalid/frozen, High, Emergency, raw ≥ High, storage/safety-journal fault, abnormal reset, AUTO off, HEATER off, batch start, test mode, trip, master drop / SSR inhibit, firmware maintenance, boot not complete. High/Emergency always win.

## Preconditions

`startAutoTune()` keeps every previous precondition (boot complete, no maintenance/OTA/trip/batch/resume, AUTO ON, heater ON, sensor valid and finite, RTC valid, no High/Emergency, SP+band < High) and adds `SP − PV ≥ 2.5 °C`: the first excitation needs room, so the first tune is run on a **cold, empty** oven. Scheduled ventilation is not active outside a batch; safety ventilation is never deferred. The circulation fan is held by the existing tune permit (`fanStable`). The SHT30 offset stays the user's manual calibration; AutoTune never reads or changes it.

## Identification (what is measured, from what)

* Energy: `tick(now, actualOn)` integrates the arbiter's ACTUAL SSR state, never the requested %.
* Gain `Kh` [°C/s at 100 % actual duty]: slope of PV against delayed ON-seconds (Welford regression, 13 delay candidates 4–190 s, parabolic refinement). This is the EFFECTIVE plant gain: no kW, volume or CFM is assumed.
* Delay `θ` [s]: the best candidate (apparent delay = dead time + lag).
* Coast: `coastRiseC`, `coastTimeSec` after the cut; `coast100 = coastRise / duty_in_flight`; consistency check `coastRise / (Kh·duty·θ)` must be 0.4–1.7.
* Hold: mean ACTUAL duty at the approach equilibrium (scaled to SP with the cold baseline as ambient) and, finally, the relay's mean duty (`High·heat/(heat+cool)`). Hold > 30 % → `POWER_LIMITED` (authority margin < 3.3×; classified HEAT_LIMITED, not tuned).
* Model confidence 60–70 (never 100); Adaptive V1 keeps refining it online and withdraws it on a long mismatch.

## PID candidate (formulas and units)

Plant `PV' = k'·u(t−θ)`, `k' = Kh/100` [°C/s per %], θ = apparent delay + 4 s (sampling/burst quantum, ≥ 6 s). SIMC for an integrating process with delay (Skogestad), `τc = max(1.5 θ, 20 s)`:

```
Kc   = 1 / (k'·(τc + θ))      [% per °C]        Kp = min(Kc, Ku_relay / 2.2, 60)   (>= 1 or the candidate is refused)
τI   = 4·(τc + θ)             [s]
Ki   = Kp / τI                [% per (°C·s)]    clamped to [0.002, 3]
Kd   = 0                      (no derivative on a thermal plant with a 0.01–0.1 °C probe)
```

Any NaN/Inf, `Kp < 1`, relay period < 2 θ or a value changed by `sanitizeMachineConfig` refuses the candidate. With `τc ≈ 1.5 θ` the resulting Ki equals the ceiling Adaptive V1 itself derives (`100/(16·Kh·θ²)`), so V1 never has to cut it.

## Closed-loop validation (mandatory) and accept / reject

The candidate never touches `config_`; it lives in the engine and is applied to a copy for the PID call. Validation runs the production heating route (startup braking, scheduler, arbiter) with the SAME assist Adaptive V1 gives a measured profile (hold feed-forward, correction limiter, Ki ceiling) and the startup hint built from the model, PID reset to zero integral, from a real approach error. Scored over the tail window:

overshoot ≤ 0.30 °C (checked over the whole check; immediate reject), MAE ≤ 0.12, P95 ≤ 0.18, ripple ≤ 0.30 °C, heater saturated ≤ 30 % of the tail, integral < 95 % of its limit; High = 0, Emergency = 0. **These targets are unchanged.** What changed in the final hardening (2026-10-09) is how much evidence the firmware demands before it believes a measured number:

1. **Horizon from the identified loop, never from the simulator** (`SmartTune::verificationPlan`, pure and unit-tested). warm-up ≥ max(30 min, 3 × tauI) and scored tail ≥ max(15 min, 2.5 × relay period, 2 × tauI), where tauI = 4·(τc+θ) is the SIMC integral time computed from the *measured* delay (production has no τ of the oven and none is invented). A coarse probe (≥ 0.05 °C steps) stretches the tail × 1.5. The floors are the original 45 min, so a quick oven is verified exactly as before (reference plant: 2700 s, tune time unchanged at 4290 s). Hard maximum 150 min and the 270 min total cap: a candidate that would need more is **rejected** (`VALIDATION_FAILED`, `why=horizon`), never verified over a shorter window.
2. **Probe resolution is read, not configured.** `noteSensor()` receives the RAW probe reading each fresh sample; the resolution is the smallest real step (> 3 mK, so float dust is ignored) between two consecutive readings, trusted after ≥ 4 steps, otherwise 0.5 °C (nothing proven). The probe only says the truth is within half a step, so each scored quantity is judged at its worst case: overshoot, MAE, P95 + res/2, ripple + res. Targets are not relaxed; a 0.1 °C probe therefore needs a measured MAE ≤ 0.07, P95 ≤ 0.13, ripple ≤ 0.20.
3. **Steady-regime evidence.** The integral only works in the startup controller's HOLD phase. At least 90 % of the scored tail must be in HOLD (`why=not-steady` otherwise): a window where the integral was frozen proves nothing about the steady state the candidate will run in.

Serial, once per verification (never per control cycle):
`[TUNE-VALIDATE] res=0.100 delay=60.8 coast=76.0 period=728 horizon=4675 tail=2730 samples=1366 hold=1.00@384s over=-0.100 MAE=0.100 P95=0.110 ripple=0.000 why=mae RESULT=FAIL` (`hold=<fraction of tail>@<s until HOLD was first seen>`; `why` = first failed check, `ok` on PASS, `horizon` = refused before the check started).

PASS → `saveConfig` (existing A/B slots + CRC + read-back) → on success the measured profile (`heaterGain`, `heaterDelaySec`, `coastRiseC`, `coastTimeSec`, `holdPowerPct`, `confidence`, `modelVersion`, signature, epoch) seeds `ThermalLearner` (hold marked *measured* so the biased-low forming windows are skipped) and is offered to `ProfileStorage::offerForced` (single flash writer = loop(), slot A/B, CRC, read-back). The PID record is the commit point; a power cut between the two records leaves a new PID with the previous/no profile, and a profile is only a capped seed (age cap 40, requalified online), so the pair is always safe.
FAIL / abort / save error → nothing was applied: old PID and profile are untouched; heater scheduler, PID and startup controller are reset; POST_COOL + restart lockout apply; the failure reason is logged (`[TUNE] FAIL reason=… over=… mae=… p95=… ripple=…`). A truncated config save is `SAVE_FAILED` (+ storage fault latch as before).

## Re-used code

`RelayAutoTune` (NEAR_SP, now with settable preheat power and limits; legacy constants stay the defaults), `ThermalController`, `ThermalStartupController`, `AdaptiveV1` + `ThermalLearner` (seed), `ThermalProfile`/`ProfileStorage`, `HeaterBurstScheduler`, `OutputArbiter`, existing config store/CRC, `EventLog`, the existing fault/permit chain. No dynamic allocation, no `String`, no new task; the engine is one static object (see sizes below).

## Qualification (SOFTWARE / SIMULATION; plant: first-order + transport delay + heater lag, uncalibrated)

`python3 tools/test_smart_autotune.py` (gate in `reliability-checks.yml`; regression ceilings in `tests/thermal-smart-autotune-baseline.json`):

| | cases | accepted | rejected (safe) | ACCEPTED_BAD | High / Emergency during tune | post-tune High / Emergency |
|---|---|---|---|---|---|---|
| Mini matrix (every level of efficiency 50–200 %, mass light/medium/heavy, dead time 0–120 s, loss low/med/high, resolution 0.01/0.1, ambient 20/28) | 36 | 16 | 20 | **0** | 0 / 0 | 0 / 0 |
| Full matrix (final hardening) | 540 | 231 | 309 | **0** | 0 / 0 | 0 / 0 |
| Full matrix before the hardening (7af68b0) | 540 | 251 | 289 | 2 | 0 / 0 | 0 / 0 |
| Legacy relay-only, cold start (same 36 plants) | 36 | 1 | 0 (29 timeouts, 6 safety aborts) | 0 | **6** / 0 | 0 / 0 |
| Legacy relay-only, warm start 37 °C | 36 | 10 | 0 (19 timeouts, 7 safety aborts) | **5** | **7** / 0 | 0 / 0 |

ACCEPTED_BAD = accepted by AutoTune but, in a separate 3 h cold-start run with the accepted gains and profile under Adaptive V1 (the oracle; the firmware never sees it), a REACHABLE plant misses overshoot ≤ 0.30 / MAE ≤ 0.12 / P95 ≤ 0.18 / ripple ≤ 0.30 / settled, or any plant crosses High/Emergency. The oracle is the production path (accepted gains + the persisted profile seed).

**The two escapes of 7af68b0 and their root causes** (both `resolution 0.1`, neither crossed a safety threshold):

* full344 (eff 1.2, 1.6 MJ/K, dead 30 + lag 30 s): validation parked PV on one probe step under SP (MAE exactly 0.100 ≤ 0.12) for the whole 15 min tail. With a 0.1 °C probe that number only bounds the true error to 0.15; the startup controller's hold cap sat a little under the real hold duty, so the offset never closed, and in the 3 h run Adaptive V1's learner later raised a gain-mismatch (prediction error 1.27 at ~7000 s), withdrew the feed-forward and the slow integral produced a 0.38 °C wander. A longer window alone would not have caught it; the resolution margin does (`why=mae`, 0.100 + 0.05 > 0.12). The late mismatch is Adaptive V1 behaviour and was left untouched.
* full400 (eff 1.5, 0.6 MJ/K, dead 0): in the validation the startup controller sat in SoftLanding (integral frozen, I = 0.00) for 40 of 45 minutes and reached HOLD only 290 s before the end, so the 15 min tail looked perfect (MAE 0.009) without ever exercising the integral. In the 3 h run the same candidate never reached HOLD: with a 0.1 °C probe the 10 s IIR slope spikes above `|slope| ≤ 0.002` on every probe step, which keeps resetting the 60 s HOLD timer, `freezePositiveIntegral` stays true and PV is parked 0.145 °C below SP. It is now refused for lack of steady-regime evidence (`why=not-steady`, HOLD in 55 % of the tail).

Effect of the hardening on the full matrix, same 540 plants: 20 of the 251 old accepts are now safe rejects (the 2 bad + 18 that happened to be good in the 3 h run; all 0.1 °C probes: 12 × `mae` bound, 7 × `not-steady`, 1 × `ripple`), no plant newly accepted, the 231 that remain have an identical outcome and the same tune time (median +10 s, worst +1586 s from the longer window of slow loops). Accepted-case worst post-tune numbers: MAE 0.081 (target 0.12), P95 0.120 (0.18), ripple 0.115 (0.30), overshoot 0.080 (0.30).

**Sensitivity probes (not the oracle, reported so the number above is not over-read).** Re-running the 3 h oracle with a perturbed Adaptive seed (`SEED_CONF=30/60/100`) leaves 1–2 ACCEPTED_BAD (full501 in two of three, full165, full321, full480): all `resolution 0.1`, fast plants parked 0.14–0.15 °C under SP by the same quantisation / startup-freeze interplay as full400, and with no seed at all (`NO_SEED=1`, not a production configuration: the accepted profile is always persisted and seeded) 14. The verification cannot see this because in its own window the controller does reach SP; the cure lives in the startup controller (see limits), not in the verification.

Unit/abort/power-loss (`tests/thermal-smart-autotune-unit.cpp`, 471 checks; new in the hardening: horizon planner, probe-resolution estimator, and the targeted regressions full344 / full400 / fast 0.01 °C plant (not slower) / medium plants (still accepted) / power-limited (own reason)): estimator recovery on synthetic ramps, SIMC numbers and bounds, baseline gates, bumpless hand-over, accept + one atomic save + profile seed + persisted profile, **84 immediate-abort cuts** (7 phases × 11 faults + operator cancel: heater OFF within one control cycle, no save, old PID/profile intact), power loss at each of the 7 phase boundaries and 5 truncated config saves (A/B slot keeps the old record), validation reject and HEAT_LIMITED rollback, warm chamber refused.

## Limits that remain

* Simulation only: no heater kW, volume or CFM is known to the firmware, but the plant is an idealised first-order model. **Physical commissioning is required** before trusting any accepted gain.
* The first tune needs a cold, empty oven (≥ 2.5 °C headroom); a hot oven is refused, not tuned.
* Plants that need > 30 % duty to hold, or whose coast under the High alarm (0.7 °C of room by default) leaves the relay < 5 % of authority (strong heater, small mass), are refused as POWER_LIMITED (safe rejection; 132 of 540). Slow plants (dead time ≥ 60 s with heavy mass) can end in `RELAY_FAILED / TOTAL_TIMEOUT` (99 of 540).
* The verification refuses what it cannot prove, so with a 0.1 °C probe the acceptance rate is lower (97 of 270 accepted at 0.1 °C vs 134 of 270 at 0.01 °C). That is intended ("not sure → do not accept"). Open item for a later, separate task (not done here: startup algorithm / integral freeze were out of scope): on a 0.1 °C probe the startup controller can park PV one quantum under SP for hours (`slope` spikes keep it out of HOLD, hold cap just under the real hold duty). A quantisation-aware slope test (`max(0.002, k·LSB/60 s)`) and a hold-cap floor would remove the cause; it needs its own regression proof.
* Horizons are heuristics derived from the identified loop (multiples of the SIMC integral time), not a proof of stability; they bound the risk, they do not remove the need for the physical run below.
* Persisted-profile refresh after the 30-day age and the online mismatch logic are unchanged.
* HMI shows the existing "DANG TU CHINH + progress %"; the phase names (`BASELINE/EXCITE/COAST/…`) are in the serial diagnostics (`[TUNE] …`) and progress is mapped 1–100 across the phases.

## First AutoTune on a real oven (supervised, whole time) — PHYSICAL COMMISSIONING PROCEDURE

Nothing below has been run on a real oven. No production accuracy is claimed from simulation.

1. **Oven**: empty, no eggs, door closed, circulation fan normal, ambient steady.
2. **Sensor**: SHT30 manually calibrated; an independent reference thermometer placed beside the probe for the whole procedure.
3. **Start condition**: PV ≤ SP − 2.5 °C (default SP 37.5 °C, High 38.2 °C): the tune refuses a warmer oven.
4. **Switches**: AUTO ON, HEATER ON, then press AutoTune. **A person watches the oven for the whole tune** (SSR/heater, PV, reference) and keeps a hand on the HEATER switch.
5. **Serial**: expect `[TUNE] BASELINE → EXCITE → COAST → APPROACH → NEAR_SP → SETTLE → VALIDATING`, then `[TUNE-VALIDATE] …RESULT=PASS|FAIL`. Record `res`, `delay`, `coast`, `period`, `horizon`, `hold=` and `why=`. A FAIL leaves the old PID and profile untouched; a `why=mae|not-steady|horizon` refusal is a legitimate outcome, not a fault.
6. **After SUCCESS**: hold SP 37.5 °C for **at least 1–2 h** and log PV, reference thermometer, heater duty and the Adaptive profile (gain, delay, hold, confidence, state). Compare the reference with the displayed PV before trusting anything.
7. **Only then** test ventilation (one vent event at a time, log the dip and the recovery), and **only after that** the door-opening disturbance.
8. Any High/Emergency, unexpected heater behaviour, or a reference differing from PV by more than the calibration tolerance → HEATER OFF, stop, keep the serial log.

### PHYSICAL MEASUREMENT REQUIRED (not provable on a host)

* Real heap / minimum heap / largest free block during a tune and after it; stack high-water of the control task and loop(); CPU time of `SmartAutoTune::update` and of `updateAutoTune` on the ESP32-S3.
* The real oven: coast, delay, hold, gain, how quickly it reaches HOLD with the real probe, and whether the horizon (45–150 min) is long enough for its slow behaviour.
* Real probe resolution and noise (the host only knows a quantised ideal probe).
* Reference-thermometer error of the displayed PV at 37.5 °C over hours.

## Firmware size (CI, PILOT profile, ESP32-S3 `default_8MB`)

| | Flash | static RAM |
|---|---|---|
| Adaptive V1 base (ba0272c) | 1,458,309 | 163,192 |
| Smart AutoTune V1 (fbfbe8d) | 1,449,817 | 162,080 |
| delta | -8,492 | -1,112 |

Hard app-partition limit 3,342,336 B (43 %); dynamic memory limit 327,680 B. Soft CI budgets 1,460,000 / 164,000 are met: headroom 10,183 B Flash / 1,920 B RAM. `sizeof(SmartAutoTune)` = 852 B (RelayAutoTune 208 B); the smaller totals come from the rewritten `updateAutoTune` replacing the older relay-only supervisor path. No budget was raised.
