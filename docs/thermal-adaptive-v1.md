# Adaptive Thermal Control V1

Status: **simulation qualified. Physical validation required.** Nothing here claims ±0.1 °C on a
real oven; the numbers below come from a production-derived host simulation (`tests/thermal-adaptive-*.{h,cpp}`).

## 1. What it is

V1 keeps the V4 PID as the control core and adds a per-oven *effective* thermal profile that is learned
from what the single probe can observe. It reuses ThermalObserver, ThermalStartupController,
AdaptiveThermalSupervisor, HeaterBurstScheduler, OutputArbiter, RelayAutoTune and the whole fault/safety
layer. Nothing in the safety path was edited (no removed or modified lines around `candidateRaw`,
`safetyTemp`, High/Emergency, E101–E104/E115, inhibit/master drop, sensor-loss handling). The SHT30 offset
is **not** learned, and a negative offset still cannot lower the safety temperature.

```
Sensor -> validation/filter -> ThermalObserver -> ThermalLearner (profile, confidence)
       -> ThermalStartupController (learned hint) -> PID + feed-forward (Assist)
       -> adaptive authority limiter (existing supervisor) -> VentCoordinator
       -> HeaterBurstScheduler -> OutputArbiter -> SSR / master
Safety (High, Emergency, E101-E104, E115, inhibit, master drop, sensor loss) is independent and
always overrides: it resets the PID and with it the plan.
```

### Files

| File | Role |
|---|---|
| `thermal_assist.h` | `Assist` / `StartupHint`: the only interface between V1 and the legacy algorithms (defaults == legacy, verified by test) |
| `thermal_profile.h` | `ThermalProfile` (60 B, versioned, CRC32), range checks, sanitising, seed-trust and flash-wear policy |
| `thermal_learner.h` | `ThermalLearner`: clean-window gate, differential regression over a delay-candidate bank, hold, coast, vent estimators, confidence, mismatch, learning states |
| `thermal_adaptive_v1.h` | `VentCoordinator` (IDLE/PREPARE/ACTIVE/RECOVERY), `AdaptiveV1` planner (authority policy, overshoot guard) |
| `thermal_profile_storage.h` | A/B NVS slots, write+read-back verify, written only from `loop()` |
| `thermal_control.h` | PID accepts optional `Assist`; startup controller accepts a learned hint |
| `machine_control.h`, `config.h`, `.ino` | wiring, runtime telemetry, `[THERMAL-LEARN]` log, events, `profileStorage.service()` |

## 2. Persistent profile (`ThermalProfile`, 60 bytes, version 1)

`magic 'TPV1'`, `version`, `size`, `heaterGain` (°C/s at 100 % *actual* duty), `heaterDelaySec`,
`coastRiseC`, `coastTimeSec`, `holdPowerPct`, `ventCoolingGain`, `confidence`, `state`, `ventConfidence`,
`validLearningSec`, `modelVersion`, `signature` (hardware/config), `epoch`, `sequence`, `crc` (CRC32, poly 0xEDB88320).
A bad magic/version/size/CRC, any NaN/Inf/out-of-range field, a different signature, an abnormal reset or a
different `modelVersion` discards the record: the oven runs the plain PID. A stored profile is only a *seed*:
confidence is capped (40 fresh / 15 stale / 20 unknown age) and authority is recomputed from fresh clean
windows. Flash wear: only a qualified profile (confidence >= 60), at most once per hour, and only on a
material change; the control task never writes flash.

## 3. Learning rules

* Input is the **actual** SSR on-time (post-arbiter), never the requested PID percentage.
* A window is learned only when clean: sensor valid, no safety/recovery/test/maintenance/AutoTune, heater
  not inhibited, circulation fan stable, exhaust fan off (vent has its own estimator), no jump/gap/door.
* Confidence 0..100 (gain fit, delay evidence, hold windows, aging); rises slowly, falls fast.
  Authority is monotone in it: hold FF ramps in at 30–60, startup hint 45–75, correction limiter and Ki
  ceiling 45–80, vent FF 30–60. Confidence 0 or a corrupt profile means legacy PID.
* States: UNLEARNED / LEARNING / QUALIFIED / ADAPTING / DEGRADED. Plant change (hold deviation outside
  max(5 pp, 25 %) plus one sensor LSB of window drift, or fast/slow gain ratio) raises
  `THERMAL_MODEL_MISMATCH`, halves confidence and re-learns. It never starts AutoTune.
* Overshoot guard: PV more than 0.35 °C above SP while V1 is in control withdraws the profile for 30 min
  (doubling on repeats). The Ki *stability* ceiling is kept after a mismatch so a long-delay plant does not
  fall back to an unlimited legacy Ki.
* The circulation fan is only an operating condition. AutoTune is untouched; learning is gated off while it
  runs. A future active-identification step can hook in through the same gate (`LearnInput::tune`).

## 4. Ventilation coordination

Ventilation still decides *when and for how long*. Thermal only coordinates: PREPARE (lead = delay − 8 s,
only when the schedule is known and the compensation is trusted), ACTIVE (bounded additive feed-forward,
capped at 35 % duty, integral frozen with a +12 % ceiling), RECOVERY (smooth fade, brake against heat in
flight). `ventCoolingGain` is learned per vent event with the heater contribution subtracted. Forced/safety
vents are never compensated.

## 5. Results (simulation, identical plants, same default configuration)

788-case matrix (`tools/test_thermal_adaptive.py`):

| | PASS | reachable PASS | High | Emergency | worst overshoot (reachable) | mean / worst MAE | worst ripple |
|---|---|---|---|---|---|---|---|
| V4 baseline | 186/788 | 139/489 | 1 | 0 | 0.27 | 0.385 / 7.9 | 2.33 |
| Adaptive V1 | **504/788** | **382/489** | **0** | **0** | 0.46 | 0.073 / 1.0 | 1.43 |

Classes (PASS/total, baseline → V1): REACHABLE 139→382 /489, SAFETY_LIMITED 11→85 /187,
COOLING_REQUIRED 36→36 /108, HEAT_LIMITED 0→0 /4. 2 cases pass on the baseline and fail on V1.
The 107 reachable failures are concentrated in heavy plants (63 with 1.6 MJ/°C) and long dead times
and are mostly slow settling (87), MAE (89) and P95 (85); only 6 exceed the 0.30 °C overshoot gate.

2160-case factorial matrix: baseline 319 PASS, V1 1245 PASS (reachable 304 → 1093 of 1535); High 33 and
Emergency 17 for **both** (the identical SAFETY_LIMITED plants: V1 adds none). 5 cases pass on the baseline and fail on V1. Worst-case reachable
overshoot 0.50, so the 0.30 target is not met everywhere.

Ventilation (63 scenarios): V1 without coordination vs full V1 vs baseline — mean wind-up 21.5 / 4.8 / 48.9,
mean tail MAE 0.117 / 0.091 / 0.376, mean max deviation 0.65 / 0.60 / 0.74 °C, worst deviation 2.85 / 2.81 / 4.55 °C,
High = Emergency = 0. The ±0.2–0.3 °C vent transient is reached for weak exhaust and for medium exhaust on
heavy/medium plants; a strong exhaust on a light plant still drops 2–3 °C (the compensation is capped and the
heater is the limit).

Learning quality on QUALIFIED runs (502/788, median 1009 s to qualify): gain error median 11 % (p90 41 %,
max 86 %), delay error median 4 s (p90 10 s), hold error median 0.07 pp (p90 0.7 pp), coast error median
0.22 °C (p90 3.0 °C). Flash writes: at most 2 in a 3 h run.

Hardware change after QUALIFIED (4 plants): High = Emergency = 0 in all. Heater 100→60 % is detected on all 4
(9–28 min after the change), heater 100→150 % on only 1 of 4 (after 2.4 h),
and the heavy-plant runs with no heater change still report one spurious mismatch. Gain stays stale after a
change (−37 % … +42 % error); control stays accurate (tail MAE 0.03–0.55 °C), because the loop does not
depend on the learned gain alone.

Sensor/disturbance faults (30 scenarios, loss, invalid, jump, frozen, noisy, jitter, missing, reboot,
manual, heater off, safety cut, power loss, door, rollover, vent): High = Emergency = 0 on both. Three heavy
cases have V1 tail MAE 0.33–0.49 °C (invalid samples, heater off, safety cut), still no worse than baseline.
A reboot restores at most the capped seed (confidence 28 in the reboot cases).

Cost: `AdaptiveV1` is 3040 B of RAM (learner 2904 B, profile 60 B, planner ~80 B), no dynamic allocation.
Host worst-case learner sample is ~0.14 ms (x86 -O2); the on-target figure has not been measured.

## 6. Limitations (read before trusting it)

* Simulation only. No physical oven, SSR, sensor lag or fan was measured.
* Plant-change detection is the weakest part: late for some plants, absent for heater ×1.5 on 3 of 4
  plants, one spurious event on a heavy plant. Authority degrades through the overshoot guard and the
  confidence drop, but the gain estimate can stay wrong for hours.
* The closed-loop gain estimate is biased on heavy plants (median 11 %, tails to 86 %).
* HEAT_LIMITED, COOLING_REQUIRED and many SAFETY_LIMITED plants cannot meet the targets by any controller.
* Strong exhaust on a light plant is limited by the heater, not the algorithm.
* On-target CPU time, stack headroom and flash size must be measured with the CI build (not done here).
* Active plant identification is left as an interface only.
