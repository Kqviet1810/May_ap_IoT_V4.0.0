# D2 + D3 — design package for approval (NOTHING here is in the production firmware)

Status: **PENDING APPROVAL.** Delivered now: the harness, traces, false-positive scan, candidate detectors in the simulator only, an executable
state table for the latch (`MAYAP_INDUSTRIAL_v1_0_0/heat_latch.h`, `tests/heat-latch.cpp`, 302 177 checks, ASan+UBSan), and the evidence below.
Not touched: E115, E104, High 38.2 / Emergency 39.0, the fault table, the OutputArbiter, `updateHeatingAndOutputs`, the SafetyJournal.
Everything is SIMULATION on an uncalibrated plant; an energy or latch heuristic is never a substitute for the independent thermostat / thermal fuse.

## 1. Current logic (read from `machine_control.h`)
| Item | Today |
|---|---|
| Emergency | `max(raw, filtered) ≥ 39.0` on the first valid sample → `emergencyActive_`; Stop-severity row drops SSR **and** master contactor; clears only after −0.3 °C for 30 s. Not latching. |
| High | `≥ 38.2` for 1 s → inhibit SSR, drop master, force both fans; clears after −0.2 °C for 10 s. **Not latching**, so after it clears the master closes again (pick-up 500 ms, min OFF 3 s). |
| Drop sequence | SSR OFF first, master released `HEAT_MASTER_DROP_DELAY_MS` = 120 ms later; `immediateMasterDrop` for Emergency / sensor loss. |
| E115 `HeaterNotHeating` (latching) | counts ACTUAL SSR ON-time (full bank) **only while** batch ∧ heater enabled ∧ sensor usable ∧ `PV < SP − tempHysteresis`; resets when PV rises ≥ `heaterStuckMinRiseC` from the window start; trips at 900 s of ON-time. |
| E104 `SensorFrozen` | reading within 0.01 °C for 20 min **and** the E115 ON-time accumulator ≥ max(60 s, 450 s). |
| Existing persisted safety flags | `SafetyJournal` (NVS) keys with read-back-verified writes, e.g. `turnMechanicalCheckRequired`; a journal that cannot be read already holds the heat off (`SafetyJournalUnavailable`). |
Consequence (Phase 1): evidence only accumulates while the reported PV is below `SP − hysteresis`. A stuck/frozen/drifting probe that reads inside `[SP − 0.2, SP)`
produces no evidence at all; a stuck-low probe is stopped only after ~15–20 min (plant 40.9–77.6 °C).

## 2. D2 — frozen / drifting sensor: candidates, false positives, false negatives, latency
Candidates evaluated in the harness only (`tests/thermal-safety-proposals.h`), run in OBSERVATION mode over **6 774 fault-free runs**
(788 + 2160 + holdout 360 + vent 63 + hwchange 16, Adaptive V1 and Smart) and ACTING on the 60 sensor-path fault cases:
| Candidate | False positives (fault-free runs fired) | Stuck-low (8): detected / latency | Frozen value (4) | Drift low (8) | Verdict |
|---|---|---|---|---|---|
| existing E115/E104 | 4 nuisance trips under probe lag ≥ 30 s (Phase 1) | 8/8, 15–20 min, plant up to 77.6 °C | 1/4 | 0/8 | baseline |
| FrozenEnergy 600 s (reading unchanged, ON-time > 1.3 × hold) | **211** (3.1 %) | – | – | – | reject |
| FrozenEnergy 1200 s | **81** (1.2 %) | 6/8, 1222 s (no better than E104) | 1/4 | 0/8 | reject |
| NetEnergyNoRise 600 s (learned gain × net ON-time ≥ 3 °C, PV rose < ¼ of it) | **551** (8.1 %) | 6/8, median 432 s, plant peak 43.5 °C (vs 77.6) | 1/4 | 1/8 (1604 s) | reject: too many false stops |
| NetEnergyNoRise 1200 s | **660** (9.7 %) | 6/8, median 422 s | 1/4 | 1/8 | reject |
* **Latency/benefit:** the energy check would cut the stuck-low excursion from 77.6 °C to 43.5 °C, i.e. it is *useful*, but a false stop costs an entire batch
  (the heater and master are cut for the rest of the run), and 8–10 % of ordinary runs would be stopped. In sensor-path no-fault cases 1/20 also fired.
* **False negatives that no software candidate fixes:** drifting probe inside the band (8/8 undetected by every candidate except one 1604 s case) — the probe
  follows a plant heated at hold duty, so energy and reading stay mutually consistent. Only a second independent sensor or the thermostat catches this.
* **Effect on heater/contactor if a candidate were adopted:** same as E115 today: SSR inhibit + master drop (latching), batch heat lost until the operator acts.
* **Conclusion/recommendation:** keep E115/E104 unchanged. Do not adopt energy plausibility. What would actually close D2: (a) a second independent temperature
  sensor with cross-check (hardware), (b) the mechanical thermostat (already required), (c) at most, trace/telemetry of the E115 accumulator and the "reading
  unchanged" timer (read-only) to collect field data. Any change to E115/E104 needs a new design reviewed against these numbers.

## 3. D3 — contactor latch after repeated High: evidence
Event classification (what the firmware can know): **COAST** (heater was commanded: mean commanded duty over the previous 300 s ≥ 15 %), **SENSOR_ERROR**
(a sensor fault active around the event; the sensor logic owns it), **UNCOMMANDED** (High/Emergency although the heater was commanded ~OFF = suspected SSR
leak/stuck), **UNEXPLAINED** (heater commanded OFF ≥ 300 s, PV ≥ SP + 0.2 and rose ≥ 0.3 °C in 120 s, sustained 60 s).
Detectors in the simulator, same 6 774 fault-free runs and the 8 SSR-stuck-ON cases (4 plants × {contactor works, contactor also stuck}):
| Detector (parameters are choices, not constants) | Fault-free false latches | SSR stuck ON, contactor OK: latched / latency median–max | plant time ≥ High (no latch → latch) | SSR + contactor stuck |
|---|---|---|---|---|
| none (today) | 0 | – | 3 594 s | 5 369 s above High, 108.6 °C |
| UNCOMMANDED High ×1 | **8** (startup coast after a short pulse is mis-read as "uncommanded") | 4/4, 255–335 s | 365 s | 2/4 detected, cannot act |
| UNCOMMANDED High ×2 within 60 min | **0** | 4/4, 603–735 s | 723 s | 0/4 (a welded contactor never gives the second High edge) |
| High ×2 / ×3 / ×4 within 60 min, regardless of command | 0 / 0 / 0 | 4/4 (579–735 s) / 4/4 (1023–1265 s) / – | 723 s / 1 080 s | 0/4 |
| UNEXPLAINED heat, offMin 300 s | **0** | 4/4, 658–802 s | 723 s | 4/4 detected at 433–465 s, **but the contactor is welded: nothing opens** |
| UNEXPLAINED heat, offMin 180 s | **27** | – | – | – (rejected) |
| UNEXPLAINED 300 s + UNCOMMANDED ×2 | 0 | 4/4, 579–735 s | 723 s | 4/4 detected |
* The latch does not reduce the **first** excursion (plant peak 40.2 °C on the light plant in all rows): that peak is detection time (1 s confirm + probe lag) plus heater
  lag, i.e. physics, not policy. It removes the cycling (3 594 s → 365–723 s above High) and the contactor chatter (10–25 re-trips).
* **Contactor capability:** the contactor feeds the SSR bank in series, so opening it removes heater power even if the SSR conducts; it does nothing if the
  contactor itself is welded (the simulator shows 108.6 °C and no software action helps) — that case belongs to the thermal fuse / thermostat.
* N is therefore **not fixed**. Proposal for the approver: UNCOMMANDED High ×2 within 60 min **or** UNEXPLAINED heat (300 s) → latch; Emergency preceded by an
  UNCOMMANDED history → latch at once (no counting); Emergency after a commanded run keeps today's behaviour. ×1 is faster but mislatches 8 coast cases until the
  coast classifier is improved (e.g. a shorter, energy-based history window); that refinement is OPEN.

## 4. Latch state table (executable in `heat_latch.h`; wired nowhere)
| State | Event | Guard | Next | Output / side effect |
|---|---|---|---|---|
| ARMED | High/Emergency edge | COAST (duty ≥ 15 %) | ARMED | none (today's High/Emergency behaviour only) |
| ARMED | High/Emergency edge | SENSOR_ERROR | ARMED | none (sensor logic owns it) |
| ARMED | High edge | UNCOMMANDED, 2nd in window | LATCHED | `holdMasterOpen`, persist TRUE |
| ARMED | Emergency edge | UNCOMMANDED | LATCHED | immediately, persist TRUE |
| ARMED | unexplained-heat detector | no sensor fault | LATCHED | persist TRUE |
| LATCHED | any ack from Web / MQTT / Cloud / Watchdog / Boot / none | – | LATCHED | refused, always |
| LATCHED | ack from local HMI | PV < SP + 0.5, heater OFF ≥ 5 min, no High/Emergency, journal writable | ARMED | persist FALSE (read-back verified) |
| LATCHED | ack from local HMI | journal not writable | LATCHED | cannot record the clear |
| any | power cycle / reboot | stored TRUE | LATCHED | restored **before** the first heat permit |
Output is OR-only into the heat-master permit: a latch can only remove heat. Emergency / High / sensor / E115 paths are unchanged and act first.
Tests: classification (coast and sensor events never count), window edge, Emergency rule, every non-HMI source refused, recovery conditions, journal-unwritable,
restore-before-first-update, and a 300 000-step fuzz (only an accepted local ack ever clears; output ≡ state).

## 5. What the approver must decide
1. Rule and parameters (×2/60 min + unexplained 300 s as proposed? or another option above).  2. Whether Emergency alone should also latch.
3. Local acknowledge: plain hold, or PIN (the technical PIN exists).  4. New SafetyJournal NVS key (name/layout) and event/fault code numbers.
5. HMI text and Web/MQTT read-only display of the latch.  6. Hardware acceptance test for the latch (stuck-SSR rig; welded-contactor case stays a thermostat test).
Not changed without that approval: `faultDescriptor` table, `updateAlarms`, `updateHeatingAndOutputs`, the preservation fingerprints.

## 6. Limitations
Fault model: SSR conducting always from the fault instant; no relay-wear model; no door/vent/hot-room interaction in the latch runs; coast classifier is a
duty threshold, not a physical model; 4 plants only for the SSR cases (the false-positive scan uses 6 774 fault-free runs).
