# 05 — REGRESSION AGAINST THE PRIOR AUDIT

The prior V3 report (removed from the V4 clean baseline, formerly a verbatim copy of the
`audit/FINAL_AUDIT_REPORT.md` that existed before this pass) was **read only after**
the independent re-audit of the current source was complete, and is used strictly as
a regression checklist. All line numbers in it were re-verified against commit
`f3ec7cb`; none of its line references are still accurate, so every item below was
re-located by symbol, not by line.

| Old ID | Previous issue | Current code | Status | Evidence | Regression introduced? |
|---|---|---|---|---|---|
| F-01 | `protectedBatchChange` still rejected turningEnabled / turnIntervalMin / turnMaxRunSec although the UI had been unlocked for them | `processHmiTransactions()` now computes `protectedBatchChange` from `totalIncubationDays` and an Offline→Online connectivity switch **only**; the three turning fields save normally mid-batch | **FIXED** | STATIC — machine_control.h:4053-4061 vs hmi.h:1035-1049 (`settingLockedDuringBatch` also narrowed to `totalIncubationDays`) | No. A *different* layer mismatch remains: the **web** batch form still submits `totalIncubationDays` unconditionally and does not grey it out during a batch → new finding F-10 |
| F-02 | Turn-fault ACK had no escalation ceiling; identical hardware failure could loop for ever | `turnFaultStreak_` counts consecutive TurnTimeout/TurnLimitStuck with no successful move between; at `TURN_FAULT_STREAK_LIMIT`=3 it latches `TurnMechanicalCheckRequired` (E205, inhibitsTurning) which only Test Mode can clear; `completeTurn()` resets the streak | **FIXED** | STATIC — latchTurnFault()/completeTurn()/updateTestMode() | **Yes, two.** (a) E205 does not block `startBatch()`, so a fresh batch can be started while turning is permanently inhibited → F-04. (b) E205 lives only in RAM, so a power cycle silently lifts the lockout → F-07 |
| F-03 | No escalating alarm if nobody answers the post-power-loss CO/HUY prompt | `FaultCode::ResumeConfirmationPending` (E135) raised after `RESUME_CONFIRM_ALERT_MS` = 15 min, plus `ResumeRtcWaitTooLong` (E137) for the RTC-blocked case | **FIXED** | STATIC — processResume():4511-4520, faultDescriptor E135/E137 | No |
| F-04 | `stopTurn()` left `trayPosition_` at the stale move origin after an abrupt mid-move stop | `stopTurn()` now sets `trayPosition_ = Unknown` and `needHome_ = true` whenever it is called while phase is MovingLeft/MovingRight | **CHANGED (partially fixed)** | STATIC — stopTurn():5242-5256 | The remaining hole: `latchTurnFault()` assigns `turnPhase_ = Fault` **directly, without calling `stopTurn()`**, so a fault raised mid-move leaves `trayPosition_` stale until `clearTurnFault()`/`startBatch()` recomputes it from the limit switches → F-11 (LOW, self-healing) |
| F-05 | A one-tick sensor blip reset the whole heater-stuck (E115) tracking window | The `!sensorUsable_` branch is now an explicit *pause*: it neither resets nor advances the tracking window (comment documents the intent) | **FIXED** | STATIC — updateAlarms():4770-4782 | No |
| F-06 | Emergency siren could be muted for 5 minutes by ACK | `SIREN_TEMPORARY_MUTE_MS` is now 60 000 ms (1 minute); the heat cut-off remains independent of acknowledgement | **CHANGED (risk reduced)** | STATIC — config.h:565, machine_control.h:4124 | Cosmetic only: the operator message still reads "COI TAM DUNG 5 PHUT" → F-12 (LOW) |
| F-07 | Batch-overdue existed only as a Cloud Push, with no on-machine alarm | `FaultCode::BatchOverdue` (E136, Warning) now exists locally and is raised from `updateAlarms()` using the un-clamped day count | **FIXED** | STATIC — updateAlarms():4839-4847 | No |
| F-08 | UNKNOWN — `PersistentStore::saveBatch/loadBatch` torn-write behaviour never read | Read in full this pass: A/B ping-pong slots, magic+schema+size+CRC32, wrapping sequence selection, mandatory read-back-and-compare after every write; a torn write can only damage the non-current slot | **NO LONGER APPLICABLE** (resolved to STATIC) | STATIC — PersistentStore:1906-1945, 2208-2255; see `audit/04_POWER_LOSS_MATRIX.md` §4.1 | No |
| F-09 | `processResume()` lacked the `turningEnabled` pre-check that `startBatch()` enforced | `processResume()` now has `ResumeBlockReason::TurningDisabled` with the same check, deliberately placed after the storage/sensor/RTC gates | **FIXED** | STATIC — processResume():4585-4590 | No |

## Summary of the regression pass

* 6 of 9 prior findings **FIXED**, 1 **CHANGED/partially fixed** (F-04), 1
  **CHANGED with the risk reduced** (F-06), 1 **NO LONGER APPLICABLE** (F-08,
  the open UNKNOWN, now resolved in the firmware's favour).
* 0 prior findings **STILL PRESENT** unchanged.
* 0 prior findings **NOT REVERIFIED**.
* **3 new findings were introduced by the fixes themselves**: F-04 and F-07 both
  come from the F-02 remediation, and F-10 from the F-01 remediation (the HMI lock
  was narrowed correctly but the web form was not updated to match).
* The prior report's largest concerns (persistence atomicity, resume confirmation,
  turn-retry ceiling) are genuinely closed. The most severe findings in the current
  pass (F-01 transport security, F-02 fault-table overflow, F-03 inert
  `dropHeatMaster`) are **new areas that the prior audit did not examine**, not
  regressions of prior findings.
