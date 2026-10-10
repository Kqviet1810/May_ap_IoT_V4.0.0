# Smart Thermal A/B/C (simulation only)

* reproducibility legacy788: A and B rows identical to frozen baseline `adaptive-788.csv`: **True**
* reproducibility matrix: A and B rows identical to frozen baseline `adaptive-2160.csv`: **True**

## 788-case matrix

| | PASS | reachable PASS | High | Emergency |
|---|---|---|---|---|
| A legacy PID | 186/788 | 139/489 | 1 | 0 |
| B Adaptive V1 | 509/788 | 387/489 | 0 | 0 |
| C Smart Thermal | 506/788 | 387/489 | 0 | 0 |

B PASS -> C FAIL: 14   B FAIL -> C PASS: 11   A PASS -> C FAIL: 0

| class | cases | A | B | C |
|---|---|---|---|---|
| COOLING_REQUIRED | 108 | 36 | 36 | 36 |
| HEAT_LIMITED | 4 | 0 | 0 | 0 |
| REACHABLE | 489 | 139 | 387 | 387 |
| SAFETY_LIMITED | 187 | 11 | 86 | 83 |

C fail reasons (heuristic): LEARNER_LOW_CONFIDENCE=88, PHYSICAL_COOLING_LIMIT=72, PHYSICAL_POWER_LIMIT=4, RIPPLE_FAILURE=1, SETTLING_TIMEOUT=18, STARTUP_OVERSHOOT=8, STEADY_MAE=91

## 2160-case matrix

| | PASS | reachable PASS | High | Emergency |
|---|---|---|---|---|
| A legacy PID | 319/2160 | 304/1535 | 33 | 17 |
| B Adaptive V1 | 1242/2160 | 1090/1535 | 33 | 17 |
| C Smart Thermal | 1227/2160 | 1074/1535 | 18 | 3 |

B PASS -> C FAIL: 52   B FAIL -> C PASS: 37   A PASS -> C FAIL: 5

| class | cases | A | B | C |
|---|---|---|---|---|
| HEAT_LIMITED | 35 | 0 | 0 | 0 |
| REACHABLE | 1535 | 304 | 1090 | 1074 |
| SAFETY_LIMITED | 590 | 15 | 152 | 153 |

C fail reasons (heuristic): LEARNER_LOW_CONFIDENCE=409, PHYSICAL_POWER_LIMIT=35, RIPPLE_FAILURE=1, SETTLING_TIMEOUT=71, STARTUP_OVERSHOOT=39, STEADY_MAE=360, UNSAFE_TRUE_TEMP=18

## vent

| mode | rows | High | Emergency | vent dev max mean | integral wind-up mean | tail MAE mean |
|---|---|---|---|---|---|---|
| BASELINE_V4 | 63 | 0 | 0 | 0.735 | 48.9 | 0.376 |
| V1_NO_VENT_COORD | 63 | 0 | 0 | 0.594 | 21.1 | 0.153 |
| ADAPTIVE_V1 | 63 | 0 | 0 | 0.620 | 5.6 | 0.102 |
| SMART_THERMAL | 63 | 0 | 0 | 0.620 | 5.6 | 0.102 |

## hwchange

| mode | rows | High | Emergency | tail MAE mean |
|---|---|---|---|---|
| BASELINE_V4 | 16 | 0 | 0 | 0.458 |
| ADAPTIVE_V1 | 16 | 0 | 0 | 0.109 |
| SMART_THERMAL | 16 | 0 | 0 | 0.109 |

## faults

| mode | rows | High | Emergency | tail MAE mean |
|---|---|---|---|---|
| BASELINE_V4 | 30 | 0 | 0 | 0.539 |
| ADAPTIVE_V1 | 30 | 0 | 0 | 0.036 |
| SMART_THERMAL | 30 | 0 | 0 | 0.036 |
