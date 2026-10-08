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

overshoot ≤ 0.30 °C (checked over the whole check; immediate reject), MAE ≤ 0.12, P95 ≤ 0.18, ripple ≤ 0.30 °C, heater saturated ≤ 30 % of the tail, integral < 95 % of its limit; High = 0, Emergency = 0.

PASS → `saveConfig` (existing A/B slots + CRC + read-back) → on success the measured profile (`heaterGain`, `heaterDelaySec`, `coastRiseC`, `coastTimeSec`, `holdPowerPct`, `confidence`, `modelVersion`, signature, epoch) seeds `ThermalLearner` (hold marked *measured* so the biased-low forming windows are skipped) and is offered to `ProfileStorage::offerForced` (single flash writer = loop(), slot A/B, CRC, read-back). The PID record is the commit point; a power cut between the two records leaves a new PID with the previous/no profile, and a profile is only a capped seed (age cap 40, requalified online), so the pair is always safe.
FAIL / abort / save error → nothing was applied: old PID and profile are untouched; heater scheduler, PID and startup controller are reset; POST_COOL + restart lockout apply; the failure reason is logged (`[TUNE] FAIL reason=… over=… mae=… p95=… ripple=…`). A truncated config save is `SAVE_FAILED` (+ storage fault latch as before).

## Re-used code

`RelayAutoTune` (NEAR_SP, now with settable preheat power and limits; legacy constants stay the defaults), `ThermalController`, `ThermalStartupController`, `AdaptiveV1` + `ThermalLearner` (seed), `ThermalProfile`/`ProfileStorage`, `HeaterBurstScheduler`, `OutputArbiter`, existing config store/CRC, `EventLog`, the existing fault/permit chain. No dynamic allocation, no `String`, no new task; the engine is one static object (see sizes below).

## Qualification (SOFTWARE / SIMULATION; plant: first-order + transport delay + heater lag, uncalibrated)

`python3 tools/test_smart_autotune.py` (gate in `reliability-checks.yml`; regression ceilings in `tests/thermal-smart-autotune-baseline.json`):

| | cases | accepted | rejected (safe) | ACCEPTED_BAD | High / Emergency during tune | post-tune High / Emergency |
|---|---|---|---|---|---|---|
| Mini matrix (every level of efficiency 50–200 %, mass light/medium/heavy, dead time 0–120 s, loss low/med/high, resolution 0.01/0.1, ambient 20/28) | 36 | 17 | 19 | **0** | 0 / 0 | 0 / 0 |
| Full matrix | 540 | 251 | 289 | **4** | 0 / 0 | 0 / 0 |
| Legacy relay-only, cold start (same 36 plants) | 36 | 1 | 0 (29 timeouts, 6 safety aborts) | 0 | **6** / 0 | 0 / 0 |
| Legacy relay-only, warm start 37 °C | 36 | 10 | 0 (19 timeouts, 7 safety aborts) | **5** | **7** / 0 | 0 / 0 |

ACCEPTED_BAD = accepted by AutoTune but, in a separate 3 h cold-start run with the accepted gains and profile under Adaptive V1, a REACHABLE plant misses overshoot ≤ 0.30 / MAE ≤ 0.12 / P95 ≤ 0.18 / ripple ≤ 0.30 / settled, or any plant crosses High/Emergency. The four residual cases (full344, full381, full400, full480) are `resolution 0.1` plants: three with a fast plant (θ ≤ 11 s) stall one probe quantum below SP (post-tune MAE 0.145–0.153, i.e. 0.03 °C over the 0.12 target; Adaptive V1's startup integral freeze `|error| < 0.15` combined with the 0.1 °C quantisation, also present with the default gains at other operating points), one with a long lag (full344, lag 30 s + dead 30 s) shows a slow ripple (P95 0.39) in the 3 h run although its validation passed. None crossed a safety threshold. These are reported, not hidden; see limits.

Unit/abort/power-loss (`tests/thermal-smart-autotune-unit.cpp`, 438 checks): estimator recovery on synthetic ramps, SIMC numbers and bounds, baseline gates, bumpless hand-over, accept + one atomic save + profile seed + persisted profile, **84 immediate-abort cuts** (7 phases × 11 faults + operator cancel: heater OFF within one control cycle, no save, old PID/profile intact), power loss at each of the 7 phase boundaries and 5 truncated config saves (A/B slot keeps the old record), validation reject and HEAT_LIMITED rollback, warm chamber refused.

## Limits that remain

* Simulation only: no heater kW, volume or CFM is known to the firmware, but the plant is an idealised first-order model. **Physical commissioning is required** before trusting any accepted gain.
* The first tune needs a cold, empty oven (≥ 2.5 °C headroom); a hot oven is refused, not tuned.
* Plants that need > 30 % duty to hold, or whose coast under the High alarm (0.7 °C of room by default) leaves the relay < 5 % of authority (strong heater, small mass), are refused as POWER_LIMITED (safe rejection; 132 of 540). Slow plants (dead time ≥ 60 s with heavy mass) can end in `RELAY_FAILED / TOTAL_TIMEOUT` (115 of 540).
* Residual ACCEPTED_BAD (4 / 251) above; root cause lives in Adaptive V1 / the startup freeze, which this task was told not to redesign. Suggested next step: let the startup controller's integral freeze use a quantisation-aware threshold (`max(0.15, 1.5·LSB)`).
* Persisted-profile refresh after the 30-day age and the online mismatch logic are unchanged.
* HMI shows the existing "DANG TU CHINH + progress %"; the phase names (`BASELINE/EXCITE/COAST/…`) are in the serial diagnostics (`[TUNE] …`) and progress is mapped 1–100 across the phases.

## First AutoTune on a real oven (supervised, whole time)

1. Empty oven, door closed, circulation fan normal, no eggs, ambient steady; SHT30 offset calibrated against an independent reference thermometer.
2. Check High/Emergency thresholds (default High 38.2 °C, SP 37.5 °C): the tune needs the oven below SP − 2.5 °C.
3. AUTO ON, HEATER switch ON, press AutoTune. Watch the SSR/heater and the serial `[TUNE]` lines: `BASELINE → EXCITE → COAST → APPROACH → NEAR_SP → SETTLE → VALIDATING → RESULT`.
4. Keep the reference thermometer next to the probe. Expect: gain/delay plausible for the oven, `hold` ≈ the duty that keeps SP, MAE/P95 in `[TUNE] RESULT`. Any `FAIL` leaves the old PID untouched (read the reason).
5. After SUCCESS let the oven hold SP for 1–2 h before the first batch and compare the reference reading with the displayed PV; only then load eggs.
6. Any High/Emergency, unexpected heater behaviour, or a reference reading differing from PV by more than the calibration tolerance → stop, switch HEATER OFF, report with the serial log.
