# Thermal controller 2.7.1

## Why

The night log of 02/11 (firmware 2.5.8, `tests/fixtures/real-logs/mayap-20261102-night-2.5.8.csv`): for 28 minutes the air stood 0.10..0.24 degC under SV (mean 37.353; 100 % of the samples
at least 0.1 under, 25 % inside +-0.1) at a flat 10 % request. Replayed into the controller (open loop, from the logged hold level) 2.5.8 reproduces the logged request to 0.66 %, and 2.7.0 gives
the SAME numbers: 2.7.0 did not touch this. Anti-Stuck needed 120 s of an unbroken "the reading is flat" test; a 0.1 degC reading steps every 30..100 s and wobbles by +-0.05, the longest flat
stretch in the log was 97 s, so it never fired and the hold level crept 0.07 %/min instead of 1.5 %/min.

## What changed

Near SV (under 0.8 degC) "not moving" is the least-squares slope of the reading over the last 240 s (under 0.012 degC/min), not the instantaneous slope; it counts only after 240 s without a door,
a fan or a vent-line hold. Far from SV the test of 2.7.0 stays. `tests/thermal-stall-log.cpp` replays the night log: the hold level rises 9.5 -> 14.3 % (2.5.8 and 2.7.0: 10.0 and 11.5 %); it
fails on 2.7.0.

## What it costs (fleet bench, 53 ovens, real temperature, 2 s sample)

| | 2.7.0 | 2.7.1 |
|---|---|---|
| mean share back within +-0.1 degC, fan 100 W | 83.5 % | 82.7 % |
| mean share, fan 2.5 kW | 69.3 % | 69.6 % |
| door open 3 min, fan 100 W / 2.5 kW | 83 % / 79 % | 72 % / 72 % |

The door recovery is lower: after a door the reading stands flat for a while under SV and the new test answers it as a stall. A slower raise (half the rate) and a longer quiet time (360 s) bring
the door back but fail the reboot-under-load limit of tests/thermal-closed-loop.cpp (35.32 degC against 35.5), which no change may loosen. Closed-loop limits: all pass.

## Not done

The fix is measured on the log only open loop (the logged PV does not answer the new commands) and on the simulated fleet. The learned state is still never saved when the batch starts next to SV
(`identified` stays 0). Neither is solved by this version.
