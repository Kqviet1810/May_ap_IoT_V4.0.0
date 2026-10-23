# Thermal V2 digital twin (simulation only)

`run_grid.py selftest` builds `v2grid.cpp` against the **production** text of `machine_control.h` / `config.h` (OutputArbiter, sensor filter,
sensor acceptance, High/Emergency alarm, E115 accounting, the whole `updateHeatingAndOutputs()` route and the real `heater_burst_scheduler.h`)
and checks the host plant against closed forms. `run_grid.py grid --ctl NAME --sets A[,B,C]` grades a controller against
`tools/thermal-research/acceptance.json`. The controller plugs into the single production line `float commandedPower = 0.0f;`, which
`make_includes.py` replaces by `rigSlot(...)`; nothing else is substituted.

A pass is **not** proof for the real oven. Sets B and C are held out: `run_grid.py` refuses to run them on a dirty tree and logs every run to `bc_runs.log`.
