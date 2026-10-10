# Smart Thermal — Phase 5: Smart AutoTune rejection study

SIMULATION ONLY. **No AutoTune code, guard or threshold was changed** (`ACCEPTED_BAD` stays 0, accepted stays 231/540). This phase is the
study the task asks for: are the 309 refusals correct, over-conservative, or harmful? New: `tools/autotune_reject_study.py` (read-only),
evidence `phase5/autotune-rejections-passive-control.csv`.

## Method
For each of the 309 rejected plants of the frozen 540 matrix, the same plant (SP 37.5, 3 h cold start, same lag / dead time / loss / mass /
efficiency / probe resolution) is run through the production control path with the **default gains and the learner only** — what the oven does
after a safe rejection — for Adaptive V1 and for Smart Thermal. The unchanged oracle and the plant-true High/Emergency decide. A refusal is a
*false-rejection candidate* only if the plant is REACHABLE, the refusal happened, and the passive controller still FAILS the oracle.

## Result (309 rejections)
| reason | n | plant class | passive Adaptive PASS | passive Smart PASS | High/Emergency Adaptive | High/Emergency Smart | false-rejection candidates |
|---|---|---|---|---|---|---|---|
| POWER_LIMITED | 142 | REACHABLE 56, SAFETY_LIMITED 86 | 35 | 63 | 13 / 9 | 5 / 2 | 11 |
| RELAY_FAILED | 99 | REACHABLE 68, SAFETY_LIMITED 31 | 67 | 74 | 0 / 0 | 0 / 0 | 13 |
| VALIDATION_FAILED | 57 | REACHABLE 50, SAFETY_LIMITED 7 | 40 | 47 | 0 / 0 | 0 / 0 | 9 (mae 6, not-steady 3) |
| MODEL_INVALID | 11 | REACHABLE 11 | 7 | 7 | 0 / 0 | 0 / 0 | 4 |
| **all** | **309** | | **149** | **191** | | | **37** |

## Answers to the seven questions
1. *Is the plant really untunable?* 86 POWER_LIMITED and 31 RELAY_FAILED plants are SAFETY_LIMITED (strong heater on a small mass: the coast under
   the 0.7 °C High margin leaves < 5 % relay authority). Refusing is correct there; the High/Emergency counts of the passive controller on those
   plants (Adaptive 13/9, Smart 5/2) are the startup-physics cases of Phases 0/2, not a consequence of refusing.
2. *Is the excitation too conservative?* The 56 REACHABLE POWER_LIMITED plants all need > 30 % hold duty; 45 of them pass passively with Smart,
   i.e. nothing is lost. 11 do not.
3. *Is passive learning enough?* For 191 of 309 refusals (62 %) the Smart controller meets the unchanged oracle without any tune (Adaptive V1: 149).
4. *Can an earlier profile be reused?* Not evaluated (needs a profile history the simulation does not model): NOT TESTED.
5. *Safe partial identification?* Not implemented; the passive result above is the bar any such feature would have to beat on the 37 candidates.
6. *Timeouts from a thermal time constant?* RELAY_FAILED (99) are TOTAL_TIMEOUT cases of slow loops (dead time ≥ 60 s with heavy mass): the 80 min
   relay budget is a fixed number, not derived from the identified loop. 13 REACHABLE ones fail passively. Candidate for a later, separately
   reviewed change (derive the relay budget from the measured delay + coast), not done here: loosening a guard needs its own regression proof.
7. *Does the controller stay safe after a refusal?* Yes by construction and by test: the unit suite aborts at 84 points (7 phases × 11 faults +
   cancel), 7 phase-boundary power cuts and 5 truncated saves, every time with the old PID/profile intact, heater OFF within one control cycle;
   in this study no REACHABLE refused plant crosses High or Emergency under the passive controller.

## Conclusion
No false-rejection pattern justifies relaxing a guard: the 37 candidates (12 % of refusals) split over four unrelated reasons and, being REACHABLE,
are the only place a better tune could pay. The list is in the CSV (`plant_class == REACHABLE and smart_pass == FAIL`). Raising the accepted
rate was explicitly not a goal; `ACCEPTED_BAD = 0` is untouched.
