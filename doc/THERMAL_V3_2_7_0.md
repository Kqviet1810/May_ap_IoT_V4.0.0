# Thermal controller 2.7.0

What changed against 2.5.8, and what was measured. Every number is from tools/thermal-research/fleet-bench.cpp (53 ovens, every fourth of the 210; 11 situations;
`TRUTH=1`: the measures read the real temperature of the oven, not the reading; the machine's real fan line; the same fan inputs for both sides). The bench is a
simulated oven, not YOUR oven: it finds structural faults, the real logs judge.

## What the oven showed (log of 01/11) and what is done about it

| Finding | 2.7.0 |
|---|---|
| The day-profile exhaust fan dips the air by 0.2 degC and more, nobody pays for it | The fan's share is MEASURED (energy balance, `fanLoss()`) and the heater pays it ONE transit time ahead (`fanBump()`). What the controller learns while the profile fan runs goes to that level, not to the hold level, so the hold level is right the moment the fan stops. |
| Anti-Stuck needed an hour to lift a reading that stood at 36 degC | Far under SV (more than 0.8 degC) it waits 90 s and raises the hold level by 0.1 %/s per degC beyond. Near SV nothing changes. It counts only after the transit time of the oven has passed since the start (the flat stretch of the delay is not a stall: without that rule the empty oven overshot by 0.75 degC). |
| The learned state was never saved when the heater ran over 40 % (`responseSuspect`) | The flag no longer holds the save back (the E115 heater watch and the High/Emergency lines are the guards and are unchanged). |
| The slope of a fan dip (0.002 degC/s) is smaller than one 0.1 degC step over the controller's own 40 s window | `sensor_fine.h`: a least-squares line over 40..150 s of readings gives the slope; the machine hands it to the controller (`Input::slopeFine`). |

## Measured (2.5.8 against 2.7.0, same fan inputs, readings every 2 s as on the machine)

| | fan 100 W | fan 2.5 kW |
|---|---|---|
| mean share of situations back within +-0.1 degC | 73.8 % -> 83.5 % | 67.6 % -> 69.3 % |
| day profile (28 of 40 min fans), summed error [degC min] | 11.8 -> 4.5 | 45.5 -> 15.2 |
| day profile, lowest air | 37.05 -> 37.31 | 36.44 -> 36.85 |
| scheduled fan, lowest air | 37.29 -> 37.29 | 36.85 -> 37.17 |
| ovens whose summed error is not worse | 53 / 53 | 53 / 53 |

Three situations are a little lower than 2.5.8 (door 3 min 83 -> 83 and 85 -> 79; reboot with stored state 89 -> 83 at 2.5 kW; one step over 70 -> 68): two or three
ovens of 53. The first climb is not faster (30.6 -> 30.7 min) and its first peak is the same: the stall that stood at 36 degC in the log is not reproduced by the bench,
and the version that lifted the hold level faster made the closed-loop tests fail (empty oven overshoot 0.75 degC against 0.50, reboot under load 35.3 degC against
the limit of 35.5). No limit was loosened.

## What was tried and is NOT in 2.7.0

Feeding the fine value (0.01 effective) to the controller as its reading, or the fine slope to its brake, door detection or damping above SV: all worse than the unchanged
reading in the bench (mean 84 % -> 26..77 %). 2.5.8 is tuned on the staircase of a 0.1 degC reading (its heater-gain estimate reads the stairs high, so it is cautious): a
better reading needs a controller written around it, not a substitution. A window of 150 s makes the fine value lag; windows of 10..30 s follow (equal to the old reading at
a 1 s sample, slightly lower at the machine's 2 s). Freezing the hold learner far from SV: the cold-tray first climb took 59 minutes. The first-climb overshoot falls from
+0.24 to +0.10 degC when the brake and the dead zone read a finer step, but the oven then sags and climbs slower.

## Left for the oven

Log columns of the fine value / slope next to the reading (a raw log of the module: how often it really refreshes, its noise), then a controller built on them.
