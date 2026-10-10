# Smart Thermal — Phase 4: ventilation and hardware adaptation (measurement + audit)

SIMULATION ONLY. **No production code changed in this phase** (a change was not justified by evidence). Added: the `ventx` suite
(`tests/thermal-adaptive-v1.cpp`, wired into `tools/ab_smart_thermal.py`), artifacts in `phase4/`.

## What was measured
`ventx` = the 21 combinations of 3 plants × 7 schedules of the existing vent matrix with a **600 W/K** exhaust (the existing 63 cover 40/120/300),
plus 4 cases with the chamber in a room **hotter than the set point** (ambient 40 °C, exhaust 120/300 W/K, medium and heavy plant).

| ventx (25 cases) | legacy | Adaptive V1 | Smart |
|---|---|---|---|
| 600 W/K: High + Emergency | 0 | 0 | 0 |
| 600 W/K: mean / worst vent dip (°C) | 2.09 / 6.32 | 2.24 / 5.72 | 2.23 / 5.72 |
| 600 W/K: integral wind-up mean | 61.0 | 8.2 | 4.6 |
| 600 W/K: tail MAE mean (°C) | 1.025 | 0.320 | 0.317 |
| hot room (ambient 40 °C): High / Emergency cases | 4 / 4 | 4 / 4 | 4 / 4 |

* 600 W/K: a 5.7 °C dip on the lightest plant is heater-limited (16 kW cannot refill a 180 kJ/°C chamber against 600 W/K); Adaptive V1 and Smart
  remove the legacy wind-up and cut the tail error by 3×. Smart does not change it (the vent path is untouched).
* Hot room: the chamber converges to the room temperature (40 °C > Emergency 39 °C) with the heater OFF; all modes are identical. This is the
  COOLING_REQUIRED class (no cooling capacity exists in the machine) — not a controller defect, and not something this program claims to solve.
  **Note for commissioning:** with a room hotter than the set point the forced exhaust at High brings heat in; the machine has no measure of the room
  temperature, so whether forcing the fan at High helps or hurts depends on the installation (needs the ambient reference of the commissioning plan).

## Audit findings (code reading + the evidence above)
1. **Vent feed-forward is not added twice.** Ventilation compensation reaches the heater through exactly one additive path, `Assist.addForward`
   (planner `ventFF`, integral frozen with a +12 pp ceiling, faded in RECOVERY). `StartupHint.ventPct` only raises the *ceiling* of the startup
   brake (`holdEff`), it never adds to the request. Measured: mean integral wind-up per vent 5.6 vs 48.9 (legacy).
2. **Forced / safety vents are never compensated** (`ventInfo_.forced`), and AutoTune and test mode are outside the plan.
3. **Hardware change** — the persisted profile signature (`thermalSignature`) covers: bank watts constant, SSR burst quantum, heater GPIO, sensor
   profile, model version. It deliberately does not cover PID gains or set point (the profile describes the plant, not the controller) nor the
   sensor offset. A physical change of the heating elements is a compile-time constant in firmware, so the signature cannot see it; it is
   detected online only: heater 100→60 % is declared on all four plants (536, 536, 1460, 1392 s Adaptive V1; 536, 536, 1460, 5224 s Smart),
   100→150 % is not declared by Adaptive V1 and by Smart only on the heavy plant (3942 s). Gain stays stale after such a change; control stays accurate
   (tail MAE 0.015–0.14 °C in both). Open: faster gain tracking after a heater change (needs excitation the closed loop does not provide).
4. **Persistence**: this program changed no persisted format (`ThermalProfile` 60 B unchanged, `MachineConfig` unchanged, new learner/startup members are
   RAM-only), so no EEPROM migration is needed and rollback to the previous firmware reads the same records.

## Not done / NOT TESTED
Vent 40/120/300/600 W/K × the *Smart* AutoTune flow; vent events during the first minutes after a power-loss restart; relay wear of the exhaust
contactor under repeated forced runs.
