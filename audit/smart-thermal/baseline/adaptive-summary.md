# Adaptive Thermal V1: simulation summary (simulation only)

## 788-case matrix

| | PASS | High | Emergency | reachable PASS | worst overshoot | mean MAE | worst MAE | worst P95 | worst ripple |
|---|---|---|---|---|---|---|---|---|---|
| V4 baseline | 186/788 | 1 | 0 | 139/489 | 0.269 | 0.385 | 7.925 | 8.409 | 2.333 |
| Adaptive V1 | 509/788 | 0 | 0 | 387/489 | 0.484 | 0.077 | 1.215 | 1.769 | 1.323 |

Cases where the baseline passes and V1 fails: 1

## Ventilation (63 scenarios x 3 modes)

| mode | High | Emergency | dev max mean | dev max worst | post-vent overshoot worst | recovery worst s | heater-on mean s | integral wind-up mean | tail MAE mean |
|---|---|---|---|---|---|---|---|---|---|
| BASELINE_V4 | 0 | 0 | 0.735 | 4.554 | 0.098 | 900 | 426 | 48.9 | 0.376 |
| V1_NO_VENT_COORD | 0 | 0 | 0.594 | 2.854 | 0.147 | 900 | 496 | 21.1 | 0.153 |
| ADAPTIVE_V1 | 0 | 0 | 0.620 | 2.811 | 0.091 | 900 | 500 | 5.6 | 0.102 |

## Hardware change (heater 100->150 / 100->60 %, vent 1->2)

| plant | change | V1 tail MAE | V1 overshoot | detected at s | mismatch events | High | Emergency |
|---|---|---|---|---|---|---|---|
| light_d15 | heater_100_to_150 | 0.034 | 0.248 | -1.0000 | 0 | 0 | 0 |
| light_d15 | heater_100_to_60 | 0.571 | 0.070 | 536.0000 | 3 | 0 | 0 |
| light_d15 | vent_1_to_2 | 0.044 | 0.070 | -1.0000 | 0 | 0 | 0 |
| light_d15 | no_change_control | 0.027 | 0.070 | -1.0000 | 0 | 0 | 0 |
| medium_d15 | heater_100_to_150 | 0.034 | 0.171 | -1.0000 | 0 | 0 | 0 |
| medium_d15 | heater_100_to_60 | 0.038 | 0.070 | 536.0000 | 1 | 0 | 0 |
| medium_d15 | vent_1_to_2 | 0.032 | 0.070 | -1.0000 | 0 | 0 | 0 |
| medium_d15 | no_change_control | 0.013 | 0.070 | -1.0000 | 0 | 0 | 0 |
| medium_d60 | heater_100_to_150 | 0.102 | 0.371 | -1.0000 | 0 | 0 | 0 |
| medium_d60 | heater_100_to_60 | 0.635 | 0.137 | 1460.0000 | 2 | 0 | 0 |
| medium_d60 | vent_1_to_2 | 0.060 | 0.137 | -1.0000 | 0 | 0 | 0 |
| medium_d60 | no_change_control | 0.033 | 0.137 | -1.0000 | 0 | 0 | 0 |
| heavy_d30 | heater_100_to_150 | 0.015 | 0.183 | -1.0000 | 0 | 0 | 0 |
| heavy_d30 | heater_100_to_60 | 0.063 | -0.009 | 1392.0000 | 1 | 0 | 0 |
| heavy_d30 | vent_1_to_2 | 0.037 | 0.077 | 1302.0000 | 1 | 0 | 0 |
| heavy_d30 | no_change_control | 0.011 | 0.064 | 1286.0000 | 1 | 0 | 0 |

## Sensor / disturbance faults

30 scenarios, V1 High/Emergency cases: 0

## Factorial matrix (2160 cases)

baseline PASS 319/2160 (High 33, Emergency 17); V1 PASS 1242/2160 (High 33, Emergency 17); reachable PASS 1090/1535
