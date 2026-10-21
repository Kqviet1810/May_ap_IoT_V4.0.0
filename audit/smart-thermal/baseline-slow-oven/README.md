# Reference A/B rows after the 2026-10-15 slow-oven change

`../baseline/` is the immutable Phase-0 reference ("later phases must add new files, never overwrite these") and is left untouched. This directory adds the
reference that `tools/ab_smart_thermal.py` now reproduces byte for byte for **A (legacy PID)** and **B (Adaptive V1)**, because the slow-oven change
(`doc/SLOW_OVEN_STUDY.md`) deliberately changes both on long-delay ovens: the startup controller now learns the hold when PV is flat just under the set point
(`ThermalStartupController::decide()`), and Adaptive V1 no longer treats a stale fast accumulator as evidence of a plant change in every mode
(`ThermalLearner::detectMismatch()`). With `MAYAP_SMART_THERMAL = 0` that is the default behaviour of the machine.

Regenerate (a reviewed, deliberate step): `python3 tools/ab_smart_thermal.py --write-reference --report-dir <dir>`; check: `python3 tools/ab_smart_thermal.py`.
The vent / hardware-change / fault files of the original baseline are still read from `../baseline/` (they are tables, not byte-for-byte checks).

## What changed against `../baseline/` (same case ids, same oracle)

| suite | rows | changed | PASS A before → after | PASS B before → after | PASS→FAIL | FAIL→PASS | rows with High (before → after) | rows with Emergency (before → after) |
|---|---|---|---|---|---|---|---|---|
| legacy788 | 1576 | 112 | 186 → 186 | 509 → 519 | 1 | 11 | 1 → 0 | 0 → 0 |
| matrix | 4320 | 331 | 319 → 325 | 1242 → 1281 | 6 | 51 | 66 → 66 | 34 → 34 |
| holdout | 720 | 56 | 45 → 44 | 254 → 258 | 1 | 4 | 0 → 0 | 0 → 0 |

The High / Emergency counts are rows (modes A and B together) in which the TRUE temperature crossed 38.2 / 39.0 °C. None increased.
The 8 rows that went from PASS to FAIL (all marginal; none is a safety crossing):

* `legacy788` `cold#706` (legacy PID, class SAFETY_LIMITED; dead 120 s, lag 8 s, loss 180 W/K, ambient 35 °C, probe 0.1000): ripple 0.044 → 0.209, MAE 0.092 → 0.072, P95 0.111 → 0.148, settling 3531 → -1
* `matrix` `m1279#1279` (Adaptive V1, class SAFETY_LIMITED; dead 120 s, lag 8 s, loss 180 W/K, ambient 35 °C, probe 0.0100): ripple 0.084 → 0.085, MAE 0.098 → 0.129, P95 0.137 → 0.168, settling 2643 → -1
* `matrix` `m1430#1430` (Adaptive V1, class REACHABLE; dead 60 s, lag 30 s, loss 300 W/K, ambient 28 °C, probe 0.0100): ripple 0.026 → 0.148, MAE 0.066 → 0.139, P95 0.077 → 0.194, settling 6829 → -1
* `matrix` `m1639#1639` (legacy PID, class SAFETY_LIMITED; dead 120 s, lag 8 s, loss 180 W/K, ambient 35 °C, probe 0.0100): ripple 0.112 → 0.243, MAE 0.084 → 0.073, P95 0.127 → 0.143, settling 4362 → -1
* `matrix` `m1679#1679` (Adaptive V1, class SAFETY_LIMITED; dead 120 s, lag 30 s, loss 300 W/K, ambient 35 °C, probe 0.0100): ripple 0.156 → 0.253, MAE 0.027 → 0.059, P95 0.122 → 0.175, settling 2219 → -1
* `matrix` `m1999#1999` (Adaptive V1, class SAFETY_LIMITED; dead 120 s, lag 8 s, loss 180 W/K, ambient 35 °C, probe 0.0100): ripple 0.108 → 0.278, MAE 0.096 → 0.078, P95 0.135 → 0.150, settling 4159 → -1
* `matrix` `m1999#1999` (legacy PID, class SAFETY_LIMITED; dead 120 s, lag 8 s, loss 180 W/K, ambient 35 °C, probe 0.0100): ripple 0.108 → 0.278, MAE 0.096 → 0.078, P95 0.135 → 0.150, settling 4159 → -1
* `holdout` `h275#275` (legacy PID, class REACHABLE; dead 90 s, lag 20 s, loss 150 W/K, ambient 31 °C, probe 0.1000): ripple 0.157 → 0.254, MAE 0.080 → 0.082, P95 0.142 → 0.148, settling 8271 → -1

Five of the eight are hot-room (35 °C) plants with a strong heater and a 120 s dead time (hold of only 1–2 %), `m1430` is a 1.6 MJ chamber with a 60 s dead time and a
0.01 °C probe, `h275` a 0.1 °C probe plant with a 90 s dead time. In them the weaker brake that fixes the stuck-below-the-set-point oven turns an offset that happened
to be inside the MAE limit into a small limit cycle (ripple 0.21–0.28 °C against the 0.25 limit) or a slower settle.

Why the change is still wanted: the field oven (weak heater, ~200 s delay, 1.9 % hold) sat 0.1–0.3 °C under the set point in every mode and its Smart AutoTune never
identified it, while the same code turns 66 rows (A and B together) from FAIL to PASS, with no extra High / Emergency row. Isolating the pieces (`doc/SLOW_OVEN_STUDY.md`, §4):
the Adaptive false-mismatch guard alone gives +4 / +1 / +35 rows and no loss; every loss comes from the hold learner of the startup controller, and removing any single part of it
(measured-duty step, slow integrator, proportional decrement) only trades some losses for others.

## 2026-10-20 update (firmware 1.1.11): hold measured over a window

The hold measurement of the startup controller (slow-oven branch) now judges "PV is flat" over a six minute window (the slow PV average moved less than
0.02 C) instead of six uninterrupted minutes of per-sample flatness, which a 0.1 C probe never allowed (a 4 % hold oven with a 60 s dead time sat 0.5 C under
the set point, see `doc/SLOW_OVEN_STUDY.md`). The three CSV files were regenerated with `--write-reference`; `../baseline/` is untouched.

Against the reference of the section above (same case ids, same oracle; rows are A and B together):

| suite | rows | changed | PASS before → after | rows with High (before → after) | rows with Emergency (before → after) |
|---|---|---|---|---|---|
| legacy788 | 1576 | 80 | 705 → 700 | 0 → 0 | 0 → 0 |
| matrix | 4320 | 208 | 1606 → 1605 | 66 → 66 | 34 → 34 |
| holdout | 720 | 37 | 302 → 301 | 0 → 0 | 0 → 0 |

Net seven marginal rows went from PASS to FAIL (ripple or settling next to the limit, no safety crossing, no extra High / Emergency row) and the sticking oven above is
fixed. Not validated on a real oven.

## 2026-10-20 update (firmware 1.1.12): first rise by PV climb, measured heater gain, quantization-aware HOLD

The three CSV files were regenerated again with `--write-reference` for the startup controller changes of 1.1.12 (`doc/SLOW_OVEN_STUDY.md`, second
addendum): a first rise is also recognised by a 0.25 C PV climb; the heater gain is measured from ON-time older than the apparent delay and scales
the legacy energy coast (4 x reserve); the slow-oven hold reads the duty over the flat window shifted back by the delay; the controller enters HOLD
when PV is within 0.12 C of its slow average despite the IIR slope spike of a 0.1 C probe step. `../baseline/` is untouched.

Against the reference of the section above (same case ids, same oracle; rows are A and B together):

| suite | rows | PASS before -> after | rows with High (before -> after) | rows with Emergency (before -> after) |
|---|---|---|---|---|
| legacy788 | 1576 | 700 -> 722 | 0 -> 0 | 0 -> 0 |
| matrix | 4320 | 1605 -> 1674 | 66 -> 66 | 34 -> 34 |
| holdout | 720 | 301 -> 323 | 0 -> 0 | 0 -> 0 |

A first attempt with a 2 x reserve on the measured gain gave 3 holdout rows with High (dead time 90 s, room 31 C, overshoot 0.77-0.88 C) and was
rejected; the 4 x reserve has no extra High / Emergency row in any suite. Not validated on a real oven.

## 2026-10-21 update (firmware 1.1.13): measured launch model

The three CSV files were regenerated again with `--write-reference` for the startup controller of 1.1.13 (`doc/SLOW_OVEN_STUDY.md`, third addendum): the heat of every
ON-second is followed through the measured delay and lag, the gain, delay, drift and hold are measured on the launch itself and kept as a plan for the next batch, and
the proportional gain is limited by the measured gain and delay. `../baseline/` is untouched.

Against the reference of the section above (same case ids, same oracle; rows are A and B together):

| suite | rows | PASS before -> after | rows with High (before -> after) | rows with Emergency (before -> after) |
|---|---|---|---|---|
| legacy788 | 1576 | 722 -> 1338 | 0 -> 0 | 0 -> 0 |
| matrix | 4320 | 1674 -> 3782 | 66 -> 33 | 34 -> 4 |
| holdout | 720 | 323 -> 698 | 0 -> 0 | 0 -> 0 |

No suite has more High / Emergency rows than before. Not validated on a real oven.
