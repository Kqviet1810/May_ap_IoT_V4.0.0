# D5 — switching budget for AutoTune (measurement + helper; numbers PENDING component data)

Status: **measurement IMPLEMENTED/TESTED; limits and wiring PENDING the rated data of the fitted parts.** No number was invented.
## What already exists
`OutputArbiter::recordTransition` counts mechanical-relay transitions per hour for contactor / fans / light / turn / spare and raises `RelayRateExceeded` (Warning,
non-stopping) above `MAX_RELAY_TRANSITIONS_PER_HOUR = 1800`; the heater SSR is deliberately excluded ("pulse-rated"). Minimum times: `SSR_MIN_ON/OFF_MS` 300 ms,
`HEAT_MASTER_PICKUP_MS` 500, `HEAT_MASTER_MIN_OFF_MS` 3000, `HEAT_MASTER_DROP_DELAY_MS` 120. There is no component rating in the repository (SSR model, switching rating,
contactor mechanical/electrical endurance), so a wear budget cannot be derived from it.
## What a Smart AutoTune actually does to the outputs (540-plant matrix, `phase8/autotune-output-edges.csv`)
| per tune | median | p95 | max |
|---|---|---|---|
| SSR edges (ON↔OFF) | 4 078 (accepted 4 934, rejected 2 870) | 11 222 | 21 867 |
| max SSR edges in any 10 min | 858 | 1 551 | 2 001 |
| master-contactor edges | 0 | 0 | 0 |
| exhaust / circulation edges | 0 / 0 | 0 / 0 | 0 / 0 |
| duration | median 5 836 s | – | 15 130 s |
So an AutoTune never cycles the contactor or the fans; it works the SSR, hard (up to 21 867 edges; ≈ 3 per second peak in 10 min windows). That is harmless for a
real solid-state relay within its rating, and heavy for an electromechanical relay: **which part is fitted decides**.
## Helper (implemented, default disabled): `output_budget.h`, `tests/output-budget.cpp` (11 checks)
`EdgeBudget`: sliding-window edge counter (30 buckets, 60 B), minimum ON/OFF violation counters, `abortRequested()` latched per run; every limit 0 = disabled, so an
instance without approved numbers can never abort. AutoTune would read it on each control cycle and cancel through its existing abort path; it never touches the
Emergency/High/sensor paths. One instance per class: solid-state SSR, electromechanical relay, contactor.
## Needed to finish (PENDING)
1. SSR part number and datasheet (rated load current, minimum on/off, zero-cross, thermal derating, maximum switching rate) and whether the two SSRs share GPIO1 timing.
2. Contactor datasheet (mechanical and electrical endurance at the heater current, coil timing).  3. Any electromechanical relay in the heater path (should be none).
4. Decision which limits are abort limits and which are warnings.  5. A reviewed change to `updateAutoTune` (protected by a preservation fingerprint) to read the helper;
it is deliberately not made here.
