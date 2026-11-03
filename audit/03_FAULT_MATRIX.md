# 03 — FAULT / ALARM MATRIX

Source of truth: `faultDescriptor()` table (machine_control.h:440-519) cross-checked
against every `faults_.set()` call site and against the actual consumers
(`updateHeatingAndOutputs`, `updateTurning`). Evidence: **STATIC**.

Legend: SSR = inhibitSsr · MST = dropHeatMaster · TRN = inhibitsTurning ·
VNT = forceVentFan · CIR = forceCirculationFan · L = latching.

| Code | Name | Sev | L | SSR | MST | TRN | VNT | CIR | Trigger (verified) | Clear |
|---|---|---|---|---|---|---|---|---|---|---|
| E101 | SensorLost | Stop | – | ✓ | ✓* | – | – | ✓ | `!sensorUsable_` after startup grace | condition falls |
| E102 | SensorInvalid | Stop | – | ✓ | ✓* | – | – | ✓ | online but `!safetySampleValid_` | condition |
| E103 | SensorSuspect | Stop | – | ✓ | ✓* | – | – | ✓ | down-step > SENSOR_MAX_DOWN_STEP_C not yet confirmed | condition |
| E110 | LowTemperature | Warn | – | – | – | – | – | – | batch + usable + `T <= lowTempAlarm`, after grace, LOW_TEMP_CONFIRM_MS | +0.2 °C |
| E111 | HighTemperature | Stop | – | ✓ | – | ✓ | ✓ | ✓ | `safetyTemp >= highTempAlarm` 1 s, eligible | hysteresis + confirm |
| E112 | EmergencyTemperature | Emerg | – | ✓ | ✓ | ✓ | ✓ | ✓ | `safetyTemp >= emergencyTemp` immediately | −0.3 °C for 30 s |
| E113 | TemperatureRateExceeded | Warn | – | – | – | – | – | – | Δ over tempRateWindowSec ≥ tempRateLimitC | next window |
| E114 | TemperatureUnstable | Warn | – | – | – | – | – | – | crossings ≥ limit in oscillation window | next window |
| E115 | HeaterNotHeating | Warn | – | – | – | – | – | – | SSR commanded ON continuously ≥ heaterStuckDurationSec with rise < heaterStuckMinRiseC and T below SV−hyst | SSR off / batch stop |
| E120 | HumidityLow | Warn | – | – | – | – | – | – | batch + `RH <= lowHumidityAlarm` after delay | +2 %RH |
| E121 | HumidityHigh | Warn | – | – | – | – | – | – | batch + `RH >= HUMIDITY_HIGH_ALARM_C` after delay | hysteresis |
| E130 | HeaterSwitchOffDuringBatch | Stop | – | ✓ | ✓* | – | – | – | heat function required and switch OFF | switch ON + 1 s |
| E132 | ResumeRequiresAuto | Warn | – | – | – | – | – | – | resume executing and AUTO off | AUTO on |
| E133 | AutoModeOffDuringBatch | Stop | – | – | – | – | – | – | batch + AUTO off for AUTO_LOST_ALARM_DELAY_MS | AUTO on |
| E134 | AutoTurningDisabledDuringBatch | Stop | – | – | – | ✓ | – | – | batch + `!turningEnabled` + not lockdown | config on |
| E135 | ResumeConfirmationPending | Warn | – | – | – | – | – | – | confirmation prompt open ≥ RESUME_CONFIRM_ALERT_MS (15 min) | Yes/No |
| E136 | BatchOverdue | Warn | – | – | – | – | – | – | elapsed days ≥ totalIncubationDays | stop batch |
| E137 | ResumeRtcWaitTooLong | Warn | – | – | – | – | – | – | blocked on RTC ≥ RESUME_RTC_WAIT_ALERT_MS | RTC valid |
| E201 | TurnLimitConflict | Stop | ✓ | – | – | ✓ | – | – | both limits ON | ACK + condition |
| E202 | TurnTimeout | Stop | ✓ | – | – | ✓ | – | – | move > turnMaxRunSec | ACK |
| E203 | TurnLimitStuck | Stop | ✓ | – | – | ✓ | – | – | origin limit still ON after 2.5 s | ACK |
| E204 | TurnCommandConflict | Stop | ✓ | – | – | ✓ | – | – | both manual turn inputs ON ≥ TURN_INPUT_CONFLICT_MS | ACK |
| E205 | TurnMechanicalCheckRequired | Stop | – | – | – | ✓ | – | – | 3 consecutive E202/E203 with no successful move between | **Test Mode only** (both limits verified) |
| E301 | StorageUnavailable | Stop | ✓ | ✓ | ✓ | – | – | – | EEPROM failure streak ≥ 3 while no batch running | reconnect service |
| E302 | StorageDegraded | Warn | – | – | – | – | – | – | 1–2 EEPROM failures, or any failure during a batch | next success |
| E303 | AbnormalReset | Stop | ✓ | ✓ | ✓ | – | – | – | reset-storm counter ≥ 3 | ACK (system mask) |
| E304 | OutputConflict | Emerg | ✓ | ✓ | ✓ | ✓ | – | – | arbiter asked for both turn directions | ACK + condition |
| E305 | RelayRateExceeded | Warn | ✓ | ✓ | – | – | – | – | > MAX_RELAY_TRANSITIONS_PER_HOUR | hourly window reset + ACK |
| E306 | RtcFailure | Warn | – | – | – | – | – | – | `!rtc_.valid()` | RTC valid |
| E313 | BatchStateClearPending | Stop | – | ✓ | ✓ | – | – | – | `batchClearPending_` | EEPROM clear verified |
| E314 | SafetyJournalUnavailable | Stop | ✓ | ✓ | ✓ | – | – | – | NVS open/write failure | ACK + condition |
| E315 | BatchLogUnavailable | Warn | – | – | – | – | – | – | logger unhealthy during batch | logger healthy |
| E401 | HeapLow | Warn | – | – | – | – | – | – | free heap ≤ warn % for 3 samples | hysteresis |
| E402 | HeapCritical | Warn | – | – | – | – | – | – | free heap ≤ critical % | restart |
| E404 | StorageRetryTrend | Warn | – | – | – | – | – | – | soft I2C retries ≥ threshold per window | next window |

`✓*` = **declared in the table but NOT effective while a batch is running** — see
finding F-04. `dropHeatMaster` is consumed only through
`req.immediateMasterDrop`, and `OutputArbiter::update()` reads that field only
inside the `if (!request.heatMaster …)` branch. During a batch with the physical
heater switch ON, `request.heatMaster` is `true`, so the drop request is never
evaluated. Rows marked `✓*` therefore inhibit the SSR only. E112 still drops the
contactor because `normalMasterPermit` itself contains `!emergencyActive_`.

## Alarm-system checks demanded by §20

* **MISSING ALARM** — frozen/constant sensor value is not detected (F-07).
  No alarm for "automatic turning has produced no completed turn for N intervals"
  independent of E202/E205 (covered operationally by F-02).
* **FALSE ALARM** — none found. Every condition has a confirm timer or streak.
* **WRONG CAUSE** — none found; `detail` fields carry the measured value.
* **DUPLICATE ALARM** — E120/E121 deliberately share `AlarmHumidityLow`; distinct
  codes, distinct text. Not a defect.
* **ALARM MASKING** — `FaultManager::slot()` reuses the last slot when 32 slots are
  exhausted; 34 codes now exist, so in the pathological case where all 32 slots are
  occupied a 33rd code overwrites slot 31. Reaching that state requires 32 distinct
  faults to have been *seen* (slots are allocated on first `set()`, including
  `set(code,false)`, and never freed). Several codes are only ever set on their
  active edge, so exhaustion is not demonstrable from static reading → recorded as
  UNKNOWN U-02.
* **ALARM CLEARS TOO EARLY** — no. `set(code,false)` on a latching fault returns
  early unless already acknowledged.
* **ALARM NEVER CLEARS** — E205 requires Test Mode, unreachable during a batch (F-02).
* **ACK CLEARS ACTIVE ROOT CAUSE** — `FaultManager::acknowledge()` only calls
  `clearState()` when `state.condition` is already false, so acknowledging does not
  clear a live condition. The exception is `clearTurnFault()`, which *forces*
  `condition=false` for the latched turn code after checking only the limit and
  manual-turn inputs — it does not prove the mechanism works (F-08 covers the
  remote-ACK aspect; the streak counter + E205 is the intended compensation).
