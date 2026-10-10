# Smart Thermal program — index (simulation evidence; nothing here is physical qualification)

Start with `FINAL_REPORT.md`, then `COMMISSIONING_TEST_PLAN.md`.

| File | What |
|---|---|
| `PHASE0_BASELINE_AUDIT.md`, `baseline/` | reproduced baselines (frozen, SHA-256), verified data flow, root cause of the 33 High / 17 Emergency |
| `PHASE1_SENSOR_PATH.md`, `phase1-sensor-path-adaptive.csv` | simulator integrity: alarms/E115/E104 from the REPORTED temperature, sensor and actuator faults |
| `PHASE2_PREDICTIVE_STARTUP.md`, `phase2/` | self-calibrated heat-in-flight brake (flag) |
| `PHASE3_LEARNER_HOLD.md`, `phase3/` | learner hold convergence, holdout matrix |
| `PHASE4_VENTILATION_HARDWARE.md`, `phase4/` | 600 W/K vent, hot room, hardware-change audit |
| `PHASE5_AUTOTUNE_REJECTIONS.md`, `phase5/` | what happens after each of the 309 AutoTune refusals |
| `PHASE6_INCUBATION_PROGRAM.md` | design-only set-point programme and egg-cooling modules (not in the firmware) |
| `final/` | final A/B/C per case id, sensor-path results, qualification manifest |

Switch: `MAYAP_SMART_THERMAL` (default 0 = byte-identical Adaptive V1 behaviour). Tools: `tools/ab_smart_thermal.py`, `tools/test_thermal_sensor_path.py`,
`tools/autotune_reject_study.py`; unit/property tests `tests/thermal-smart-startup.cpp`, `tests/thermal-adaptive-unit.cpp`, `tests/thermal-program.cpp`.
