# Smart Thermal A/B/C (simulation only)

## 788-case matrix

| | PASS | reachable PASS | High | Emergency |
|---|---|---|---|---|
| A legacy PID | 186/788 | 139/489 | 1 | 0 |
| B Adaptive V1 | 509/788 | 387/489 | 0 | 0 |
| C Smart Thermal | 546/788 | 418/489 | 0 | 0 |

B PASS -> C FAIL: 9   B FAIL -> C PASS: 46   A PASS -> C FAIL: 0

| class | cases | A | B | C |
|---|---|---|---|---|
| COOLING_REQUIRED | 108 | 36 | 36 | 36 |
| HEAT_LIMITED | 4 | 0 | 0 | 0 |
| REACHABLE | 489 | 139 | 387 | 418 |
| SAFETY_LIMITED | 187 | 11 | 86 | 92 |

C fail reasons (heuristic): LEARNER_LOW_CONFIDENCE=57, PHYSICAL_COOLING_LIMIT=72, PHYSICAL_POWER_LIMIT=4, RIPPLE_FAILURE=1, SETTLING_TIMEOUT=14, STARTUP_OVERSHOOT=8, STEADY_MAE=86

## 2160-case matrix

| | PASS | reachable PASS | High | Emergency |
|---|---|---|---|---|
| A legacy PID | 319/2160 | 304/1535 | 33 | 17 |
| B Adaptive V1 | 1242/2160 | 1090/1535 | 33 | 17 |
| C Smart Thermal | 1397/2160 | 1232/1535 | 18 | 3 |

B PASS -> C FAIL: 48   B FAIL -> C PASS: 203   A PASS -> C FAIL: 5

| class | cases | A | B | C |
|---|---|---|---|---|
| HEAT_LIMITED | 35 | 0 | 0 | 0 |
| REACHABLE | 1535 | 304 | 1090 | 1232 |
| SAFETY_LIMITED | 590 | 15 | 152 | 165 |

C fail reasons (heuristic): LEARNER_LOW_CONFIDENCE=221, PHYSICAL_POWER_LIMIT=35, RIPPLE_FAILURE=1, SETTLING_TIMEOUT=85, STARTUP_OVERSHOOT=39, STEADY_MAE=364, UNSAFE_TRUE_TEMP=18

## holdout-360-case matrix (HOLDOUT: parameter values in neither of the other two matrices)

| | PASS | reachable PASS | High | Emergency |
|---|---|---|---|---|
| A legacy PID | 45/360 | 45/290 | 0 | 0 |
| B Adaptive V1 | 254/360 | 224/290 | 0 | 0 |
| C Smart Thermal | 282/360 | 250/290 | 0 | 0 |

B PASS -> C FAIL: 8   B FAIL -> C PASS: 36   A PASS -> C FAIL: 0

| class | cases | A | B | C |
|---|---|---|---|---|
| REACHABLE | 290 | 45 | 224 | 250 |
| SAFETY_LIMITED | 70 | 0 | 30 | 32 |

C fail reasons (heuristic): LEARNER_LOW_CONFIDENCE=42, SETTLING_TIMEOUT=11, STARTUP_OVERSHOOT=5, STEADY_MAE=20

## vent

| mode | rows | High | Emergency | vent dev max mean | integral wind-up mean | tail MAE mean |
|---|---|---|---|---|---|---|
| BASELINE_V4 | 63 | 0 | 0 | 0.735 | 48.9 | 0.376 |
| V1_NO_VENT_COORD | 63 | 0 | 0 | 0.594 | 21.1 | 0.153 |
| ADAPTIVE_V1 | 63 | 0 | 0 | 0.620 | 5.6 | 0.102 |
| SMART_THERMAL | 63 | 0 | 0 | 0.637 | 5.6 | 0.093 |

## ventx

| mode | rows | High | Emergency | tail MAE mean |
|---|---|---|---|---|
| BASELINE_V4 | 25 | 4 | 4 | 1.261 |
| V1_NO_VENT_COORD | 25 | 4 | 4 | 0.778 |
| ADAPTIVE_V1 | 25 | 4 | 4 | 0.669 |
| SMART_THERMAL | 25 | 4 | 4 | 0.666 |

## hwchange

| mode | rows | High | Emergency | tail MAE mean |
|---|---|---|---|---|
| BASELINE_V4 | 16 | 0 | 0 | 0.458 |
| ADAPTIVE_V1 | 16 | 0 | 0 | 0.109 |
| SMART_THERMAL | 16 | 0 | 0 | 0.083 |

## faults

| mode | rows | High | Emergency | tail MAE mean |
|---|---|---|---|---|
| BASELINE_V4 | 30 | 0 | 0 | 0.539 |
| ADAPTIVE_V1 | 30 | 0 | 0 | 0.036 |
| SMART_THERMAL | 30 | 0 | 0 | 0.033 |

