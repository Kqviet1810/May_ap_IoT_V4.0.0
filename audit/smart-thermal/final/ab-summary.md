# Smart Thermal A/B/C (simulation only)

* reproducibility legacy788: A and B rows identical to frozen baseline `adaptive-788.csv`: **True**
* reproducibility matrix: A and B rows identical to frozen baseline `adaptive-2160.csv`: **True**
* reproducibility holdout: A and B rows identical to frozen baseline `adaptive-holdout360.csv`: **True**

## 788-case matrix

| | PASS | reachable PASS | High | Emergency |
|---|---|---|---|---|
| A legacy PID | 186/788 | 139/489 | 1 | 0 |
| B Adaptive V1 | 509/788 | 387/489 | 0 | 0 |
| C Smart Thermal | 548/788 | 420/489 | 0 | 0 |

B PASS -> C FAIL: 8   B FAIL -> C PASS: 47   A PASS -> C FAIL: 0

| class | cases | A | B | C |
|---|---|---|---|---|
| COOLING_REQUIRED | 108 | 36 | 36 | 36 |
| HEAT_LIMITED | 4 | 0 | 0 | 0 |
| REACHABLE | 489 | 139 | 387 | 420 |
| SAFETY_LIMITED | 187 | 11 | 86 | 92 |

C fail reasons (heuristic): LEARNER_LOW_CONFIDENCE=56, PHYSICAL_COOLING_LIMIT=72, PHYSICAL_POWER_LIMIT=4, RIPPLE_FAILURE=1, SETTLING_TIMEOUT=14, STARTUP_OVERSHOOT=8, STEADY_MAE=85

## 2160-case matrix

| | PASS | reachable PASS | High | Emergency |
|---|---|---|---|---|
| A legacy PID | 319/2160 | 304/1535 | 33 | 17 |
| B Adaptive V1 | 1242/2160 | 1090/1535 | 33 | 17 |
| C Smart Thermal | 1442/2160 | 1277/1535 | 18 | 3 |

B PASS -> C FAIL: 41   B FAIL -> C PASS: 241   A PASS -> C FAIL: 5

| class | cases | A | B | C |
|---|---|---|---|---|
| HEAT_LIMITED | 35 | 0 | 0 | 0 |
| REACHABLE | 1535 | 304 | 1090 | 1277 |
| SAFETY_LIMITED | 590 | 15 | 152 | 165 |

C fail reasons (heuristic): LEARNER_LOW_CONFIDENCE=212, PHYSICAL_POWER_LIMIT=35, RIPPLE_FAILURE=1, SETTLING_TIMEOUT=71, STARTUP_OVERSHOOT=39, STEADY_MAE=342, UNSAFE_TRUE_TEMP=18

## holdout-360-case matrix (HOLDOUT: parameter values in neither of the other two matrices)

| | PASS | reachable PASS | High | Emergency |
|---|---|---|---|---|
| A legacy PID | 45/360 | 45/290 | 0 | 0 |
| B Adaptive V1 | 254/360 | 224/290 | 0 | 0 |
| C Smart Thermal | 288/360 | 256/290 | 0 | 0 |

B PASS -> C FAIL: 5   B FAIL -> C PASS: 39   A PASS -> C FAIL: 0

| class | cases | A | B | C |
|---|---|---|---|---|
| REACHABLE | 290 | 45 | 224 | 256 |
| SAFETY_LIMITED | 70 | 0 | 30 | 32 |

C fail reasons (heuristic): LEARNER_LOW_CONFIDENCE=38, SETTLING_TIMEOUT=11, STARTUP_OVERSHOOT=5, STEADY_MAE=18

## vent

| mode | rows | High | Emergency | vent dev max mean | integral wind-up mean | tail MAE mean |
|---|---|---|---|---|---|---|
| BASELINE_V4 | 63 | 0 | 0 | 0.735 | 48.9 | 0.376 |
| V1_NO_VENT_COORD | 63 | 0 | 0 | 0.594 | 21.1 | 0.153 |
| ADAPTIVE_V1 | 63 | 0 | 0 | 0.620 | 5.6 | 0.102 |
| SMART_THERMAL | 63 | 0 | 0 | 0.598 | 0.0 | 0.085 |

## ventx

| mode | rows | High | Emergency | tail MAE mean |
|---|---|---|---|---|
| BASELINE_V4 | 25 | 4 | 4 | 1.261 |
| V1_NO_VENT_COORD | 25 | 4 | 4 | 0.778 |
| ADAPTIVE_V1 | 25 | 4 | 4 | 0.669 |
| SMART_THERMAL | 25 | 4 | 4 | 0.667 |

## hwchange

| mode | rows | High | Emergency | tail MAE mean |
|---|---|---|---|---|
| BASELINE_V4 | 16 | 0 | 0 | 0.458 |
| ADAPTIVE_V1 | 16 | 0 | 0 | 0.109 |
| SMART_THERMAL | 16 | 0 | 0 | 0.082 |

## faults

| mode | rows | High | Emergency | tail MAE mean |
|---|---|---|---|---|
| BASELINE_V4 | 30 | 0 | 0 | 0.539 |
| ADAPTIVE_V1 | 30 | 0 | 0 | 0.036 |
| SMART_THERMAL | 30 | 0 | 0 | 0.033 |

## Long runs (SIMULATED hours: 12 h steady and 72 h drift; vent profile 10 % / 40 min, sensor noise)

| mode | cases | High | Emergency | mean |PV-SP| after the first 6 h (°C) | worst 6 h block after the first 6 h | profile flash writes (72 h run, max) |
|---|---|---|---|---|---|---|
| BASELINE_V4 | 8 | 0 | 0 | 0.380 | 0.490 | 0 |
| ADAPTIVE_V1 | 8 | 0 | 0 | 0.090 | 0.498 | 5 |
| SMART_THERMAL | 8 | 0 | 0 | 0.090 | 0.498 | 5 |
