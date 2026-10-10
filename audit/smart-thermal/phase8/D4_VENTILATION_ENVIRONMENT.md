# D4 — forced exhaust at High: does it cool or heat? (study, no policy change)

Status: **evidence delivered; fail-safe logic UNCHANGED; any policy change PENDING APPROVAL.**
Setup (`tests/thermal-sensor-path.cpp ventenv`, `phase8/vent-environment.csv`): the same overheating event (SSR stuck ON, contactor working) on 4 plants,
room 20 / 30 / 37 / 40 / 44 °C, exhaust 0 (no thermal effect) / 120 / 300 W/K. The exhaust exchange is modelled as `G·(T_chamber − T_room)`.
| Room vs chamber | Effect of the forced exhaust (light plant / heavy plant, time at or above High) |
|---|---|
| room colder (20 °C) | helps: 3 594 s → 2 119 s (300 W/K) / 2 551 s → 1 829 s |
| room colder (30 °C) | helps less: 4 205 → 3 460 s / 3 063 → 2 609 s |
| room ≈ chamber (37 °C) | neutral to slightly helpful: 4 529 → 4 161 s / 3 124 → 3 192 s (within noise of the cycling) |
| room hotter than High (40, 44 °C) | the chamber converges to the room whatever the fan does; the machine is outside its operating domain (no cooling capacity exists) |
Reading: the exchange can bring the chamber towards the room temperature but not beyond it, so for a room between the set point and High the worst case is a
rise bounded by (room − chamber), and for a room above High nothing the controller does matters. The current forced-vent-at-High behaviour is therefore not harmful in
the modelled domain. **Not modelled:** humidity, CO₂/gas-exchange need, local hot spots at the intake, fan motor heat, room temperature that changes during the event.
Because the machine has no room-temperature sensor, the firmware cannot know which case it is in; the validated protection logic stays as is. Commissioning plan item:
log room temperature at the intake during the High test (Phase 1, test 1.5) to confirm the sign of the effect on the real installation before any policy is proposed.
