# Smart AutoTune V1: simulation summary (SOFTWARE / SIMULATION ONLY)


## Mini matrix (36 cases: every level of efficiency / mass / dead time / loss / resolution / ambient)

cases=36 started=36 model_valid=35 candidate=20 validation_started=20 accepted=16 rejected=20 timeout=0 safety_abort=0 sensor_abort=0 **ACCEPTED_BAD=0** High/Emergency during tune=0/0 post-tune High/Emergency=0/0

rejection reasons: MODEL_INVALID=1, POWER_LIMITED=11, RELAY_FAILED=4, VALIDATION_FAILED=4

accepted: gain err % mean|worst = 2.5|5.3, delay err s mean|worst = 6.2|7.6, hold err pp worst = 0.2, post-tune MAE worst 0.077, P95 worst 0.120, ripple worst 0.100, overshoot worst 0.068


## Full matrix (540 cases: 6 efficiencies x 3 masses x 3 losses x 5 dead times x 2 ambients, lag 3/8/30, resolution 0.01/0.1)

cases=540 started=540 model_valid=529 candidate=288 validation_started=288 accepted=231 rejected=309 timeout=0 safety_abort=0 sensor_abort=0 **ACCEPTED_BAD=0** High/Emergency during tune=0/0 post-tune High/Emergency=0/0

rejection reasons: MODEL_INVALID=11, POWER_LIMITED=142, RELAY_FAILED=99, VALIDATION_FAILED=57

accepted: gain err % mean|worst = 4.6|28.0, delay err s mean|worst = 5.5|29.2, hold err pp worst = 0.6, post-tune MAE worst 0.081, P95 worst 0.120, ripple worst 0.131, overshoot worst 0.080

ACCEPTED_BAD cases: none


## Legacy relay-only AutoTune vs Smart AutoTune (same 36 mini plants)

| engine / start | accepted | rejected | timeout | safety abort | ACCEPTED_BAD | High during tune | Emergency during tune |
|---|---|---|---|---|---|---|---|
| Smart, cold start | 16 | 20 | 0 | 0 | 0 | 0 | 0 |
| Legacy, cold start | 1 | 0 | 29 | 6 | 0 | 3 | 0 |
| Legacy, warm start 37.0 C | 9 | 0 | 19 | 8 | 4 | 2 | 0 |

(Smart refuses a warm chamber at start: it needs >= 2.5 C of headroom for the first excitation.)
