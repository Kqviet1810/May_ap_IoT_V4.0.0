# FINAL PRE-RELEASE FIRMWARE AUDIT — MAYAP INDUSTRIAL

Independent machine-control verification. This audit was performed from scratch on
the current source; the prior report was read only afterwards, as a regression
checklist (see `05_REGRESSION.md`).

Companion files: `00_AUDIT_STATUS.md`, `01_SYSTEM_MAP.md`, `02_MACHINE_FLOW.md`,
`03_FAULT_MATRIX.md`, `04_POWER_LOSS_MATRIX.md`, `05_REGRESSION.md`,
and `sim/` (executable models). The superseded V3 archive was removed from the V4 clean baseline.

---

## A. EXECUTIVE SUMMARY

```text
Firmware audited:   MAYAP INDUSTRIAL v3.7.0 (MAYAP_INDUSTRIAL_v3_4_0/)
Branch/commit:      main / f3ec7cb2b5260341176b1b045ea557e95bee7c81
Runtime available:  NO — RUNTIME VERIFICATION NOT AVAILABLE
                    (no ESP32-S3 hardware, no ESP32 toolchain/core installed, no
                    HIL rig. PlatformIO and g++ exist on the audit host; g++ was
                    used only for stand-alone logic models in audit/sim/, which
                    are SIMULATED evidence, never RUNTIME.)
Scope:              boot/init, control loop, MachineController, batch lifecycle,
                    start/stop/emergency, sensor + actuator layers, OutputArbiter,
                    heating/PID/SSR/master contactor, turning system, fault and
                    alarm managers, recovery, persistence, power-loss, batch
                    boundary, and every command/config path that can change
                    machine behaviour (HMI, MQTT realtime, web dashboard).
                    Cosmetic UI, CSS, PWA assets, D1 schema: out of scope.

Release blockers:   4
Critical:           1
High:               3
Medium:             5
Low:                6
Unknown:            3
```

**Release decision: NEEDS FIX BEFORE RELEASE.**

---

## B. MACHINE CONTROL FLOW

Reconstructed from source in `02_MACHINE_FLOW.md`. Condensed:

```text
POWER ON -> mayapSafeOutputsEarly() -> Machine.begin()
  -> PowerManager (reset reason + reset-storm counter in NVS)
  -> PersistentStore.begin() -> loadConfig() (A/B + CRC + legacy migration)
  -> loadBatch() x SafetyJournal.stopIntentPending()
        stop tombstone      -> batchClearPending_   (never resumes)
        wasRunning==1       -> resumePending_ [+ confirmation if real power loss]
        otherwise           -> clean boot
IDLE/READY --startBatch()--> PRESTART(5 s) --[needHome_]--> HOMING --> RUNNING
RUNNING = PID heat + scheduled turning + 5-min checkpoints + batch log
          (last 3 days: turning lockdown)
RUNNING --stopBatch()--> tombstone -> RAM reset -> EEPROM clear (retry 3 s) -> READY
No automatic BATCH END state exists (finding F-06).
```

Two orthogonal state machines (`BatchPhase` and `TurnPhase`) plus a set of latched
booleans. No unreachable states, no conflicting transitions, no outputs carried
over between states (the whole `OutputRequest` is rebuilt every cycle and
`OutputArbiter` is the single writer of all nine relay pins).

---

## C. HAPPY PATH

```text
HAPPY PATH
Evidence:   STATIC (full execution trace) + SIMULATED (timing arithmetic,
            audit/sim/timing_model.cpp, 14/14 checks pass)
Result:     The complete sequence BOOT -> READY -> START -> PRESTART -> HOMING ->
            RUNNING -> HEATING -> TURNING -> checkpointing -> LOCKDOWN -> STOP ->
            READY FOR NEXT BATCH is coherent and reachable, with one structural
            gap: the batch never ends by itself (F-06). Fan/contactor stagger,
            master pickup delay before SSR, SSR minimum on/off, turn dead-time
            and limit interlocks all behave correctly in the traced path.
Confidence: MEDIUM
            (HIGH for the logic; capped at MEDIUM because no code has been
            executed on the target and no I/O timing has been observed.)
```

---

## D. START / STOP / EMERGENCY

### Start conditions (§12) — `startBatch()` gate list, all verified STATIC

| Condition at the moment of Start | Start allowed? | Expected? | Evidence |
|---|---|---|---|
| sensor invalid / not yet usable | NO | YES | `if (!sensorUsable_)` |
| sensor missing | NO | YES | same gate |
| fault active — turn fault | NO | YES | `if (turnFaultLatched_)` |
| fault active — **E205 mechanical-check lockout** | **YES** | **NO** | no gate exists → **F-04** |
| fault active — high/emergency temperature | NO | YES | `highTemperatureActive_ \|\| emergencyActive_` |
| fault active — abnormal reset | NO | YES | `abnormalResetLatched_` |
| storage unavailable / degraded | NO | YES | `storageFaultLatched_ \|\| storageDegraded_` |
| safety journal unavailable | NO | YES | `safetyJournalFaultLatched_` |
| batch clear pending | NO | YES | `batchClearPending_` |
| resume pending | NO | YES | `resumePending_` |
| AUTO switch off | NO | YES | `!in.autoMode` |
| heater switch off | NO | YES | `!in.heaterEnable` |
| turning disabled in config | NO | YES | `!config_.turningEnabled` |
| both limit switches closed | NO | YES | `in.limitLeft && in.limitRight` |
| RTC invalid | NO | YES | `!rtc_.valid()` |
| auto-tune running | NO | YES | `autotune_.running()` |
| test mode active | NO | YES | `testModeActive_` |
| batch already running | NO | YES | `batchRunning_` |
| previous batch not fully cleared | NO | YES | `batchClearPending_` |
| EEPROM write of the new batch record fails | NO — and the whole start is rolled back | YES | end of `startBatch()` |

One gap only: **F-04**.

### Stop (§19) — injected at every state

Stop was traced from Prestart, Homing, heating, turning dead-time, turning
mid-move, with an alarm active, during resume-wait, during a config transaction
and at a checkpoint. In every case:
`setStopIntent()` (tombstone, before anything else) → `batchClearPending_=true` →
RAM state zeroed → `stopTurn(false)` (which, mid-move, also invalidates
`trayPosition_` and sets `needHome_`) → `postCoolUntil_` → EEPROM clear with
3-second retry for ever. Traced through to the **final physical-output request**:
`batchClearPending_` forces `req.immediateMasterDrop`, and `batchAllowsHeat` becomes
false, so the SSR is de-energised the same cycle and the contactor immediately
after. Stop intent survives a power loss at any point in that sequence
(`04_POWER_LOSS_MATRIX.md`). **PASS.**

### Emergency

There is no dedicated E-stop input; the physical heater switch is the manual kill
(by design — it has absolute authority over the master contactor).
`EmergencyTemperature` is the automatic emergency: it sets `emergencyActive_`,
which appears directly inside `normalMasterPermit`, so the contactor opens
immediately, the SSR is inhibited, both fans are forced on, turning is inhibited
and the siren relay is energised. Recovery requires −0.3 °C sustained for 30 s and
then imposes `HEAT_RESTART_LOCKOUT_MS`. **PASS**, with two caveats: the siren can
be re-muted indefinitely by repeated ACK (F-01 makes that remotely abusable; F-09),
and the whole ladder can be switched off outside a batch (F-05).

---

## E. HEATING SAFETY

Traced end to end: temperature → validation (two tiers: raw/`safetySampleValid_`
for protection, filtered/`sensorUsable_` for PID) → PID (clamped, conditional
anti-windup) → `ssrWindowOn()` time-proportioning (min-on/min-off enforced;
verified in simulation across a `millis()` rollover) → `req.heaterSsr` /
`req.heatMaster` → `OutputArbiter` (contactor pickup before SSR, SSR off before
contactor drop) → pins.

Injection results:

| Injected | SSR | Master contactor | Verdict |
|---|---|---|---|
| sensor lost / invalid / suspect | inhibited | **stays energised during a batch** | **FAIL — F-03** |
| high temperature (E111) | inhibited, PID reset, both fans forced | stays energised (by design table) | PASS |
| emergency temperature (E112) | inhibited | dropped immediately | PASS |
| heater not heating (E115) | unchanged (warning only) | unchanged | PASS (diagnostic by design) |
| output conflict (E304) | inhibited | **stays energised during a batch** | **FAIL — F-03** |
| storage unavailable / journal fail / clear pending / abnormal reset | inhibited | **stays energised during a batch** | **FAIL — F-03** |
| heater switch off mid-batch (E130) | inhibited | dropped (the switch itself gates the permit) | PASS |
| AUTO off mid-batch | unchanged — PID continues by design | unchanged | PASS (documented) |
| fan not stable / fan off | SSR blocked by `fanAllowsHeat` | unchanged | PASS |
| sensor recovers while still hot | `heatRestartNotBefore_` lockout + vent/post-cool | — | PASS |

**The `FAULT ACTIVE + HEAT OUTPUT STILL ACTIVE` path explicitly hunted for by §17
exists and is finding F-03.**

---

## F. TURNING SYSTEM

Idle → request → dead-time (500 ms, enforced again independently inside
`OutputArbiter`) → move → limit → complete → next schedule. Verified: left→right,
right→left, homing, manual turning, AUTO off, turning disabled, lockdown, limit
conflict, limit stuck, limit never reached, timeout, stop mid-move, fault mid-move,
power loss mid-move, config change mid-move, ACK with the hardware fault still
present, repeated faults, retry behaviour.

Strengths: direction relays can never be commanded together (state machine *and*
arbiter), `trayPosition_` is never trusted from EEPROM and is always re-derived from
the limit switches, `moveOrigin_` gives a real "limit did not release" detection,
and a failure streak escalates to a mechanical-check lockout.

Failures found: **F-02** (the turn-fault alarm codes are erased from the fault
manager within 30 s), **F-04** (a new batch may start under the mechanical lockout),
**F-07** (the lockout is lost on reboot), **F-11** (stale `trayPosition_` while a
fault is latched), **F-13** (a manual turn does not re-anchor the auto schedule).

---

## G. SENSOR FAULTS

| Injected | Detected | Response |
|---|---|---|
| never ON / missing at startup | YES (after the startup grace) | E101, SSR inhibited, fans forced |
| signal lost mid-batch | YES (`dataAgeMs > sensorTimeoutSec`) | E101 + PID reset + restart lockout + post-cool |
| signal returns | YES, only after `SENSOR_RECOVERY_GOOD_SAMPLES` consecutive good samples | heat re-enabled after the lockout |
| invalid value / out of range | YES (`dataValid()` + `isfinite`) | E102 |
| implausible downward step | YES (`SENSOR_MAX_DOWN_STEP_C` + confirmation streak) | E103, filtered value held |
| implausible upward step | NOT plausibility-checked — accepted | fails safe (drives the over-temperature ladder) |
| intermittent / flapping | YES | the recovery streak prevents rapid re-enable |
| **stuck at a constant plausible value** | **NO** | **F-08 — MISSING ALARM** |
| CRC / framing / timeout errors | counted and exposed on the diagnostic page | — |
| both limit switches closed | YES | E201 immediately |
| limit switch stuck closed at the origin | YES | E203 after 2.5 s |
| limit switch never reached | YES | E202 after `turnMaxRunSec` |
| contradictory sensors | covered by E201 for the limits; there is only one T/RH sensor, so no cross-check is possible (U-02) |

---

## H. ALARM SYSTEM

Full table in `03_FAULT_MATRIX.md`. Structure is sound: a single descriptor table
drives severity, latching, SSR inhibit, contactor drop, turning inhibit and fan
forcing; acknowledgement never clears a condition that is still true.

Two structural defects:
* **F-02** — the fault table has 32 slots but 36 codes; 32 are allocated
  unconditionally within ~90 s of every boot, so the four turn-fault codes
  permanently share one overflow slot with `StorageRetryTrend` and are wiped
  (with a wrong "cleared" event logged) at the next 30-second health tick.
* **F-03** — the `dropHeatMaster` column of the table is inert during a running
  batch for every fault except `EmergencyTemperature`.

---

## I. RECOVERY

For every significant fault the chain NORMAL → FAULT → OUTPUT RESPONSE → ALARM →
ACK → CLEAR → RECOVERY → VALID STATE → CONTINUE was traced.

* Timers reset: yes (`pid_.reset()`, `heatRestartNotBefore_`, `postCoolUntil_`,
  the relevant `ConditionTimer`s).
* Flags reset: yes for the turn path (`clearTurnFault()` rebuilds
  `trayPosition_`/`needHome_` from the live inputs).
* Sensor revalidated: yes (recovery streak).
* Position revalidated: yes.
* Batch time preserved: yes (elapsed is anchored to the checkpoint + RTC).
* Next schedule valid: yes (`setTurnScheduleAnchor` / `scheduleNextTurnFromAnchor`).

Defects: **UNSAFE RECOVERY** — none found. **PARTIAL RECOVERY** — F-04 (a batch
may be started into a state where turning can never run). **RECOVERY LOOP** —
FAULT → ACK → SAME FAULT is possible for turn faults, but it is bounded by the
streak counter and E205; the bound is lost across a reboot (F-07). **FAULT → ACK →
INVALID STATE** — not found.

---

## J. MULTI-FAULT

Sequence A — `START → TURN FAULT → ACK → RECOVERY → SENSOR LOST → RECOVERY →
HEATING FAULT → RECOVERY → STOP → START`: traced with no stale-flag contamination
except the two already-recorded items (`turnFaultStreak_` and
`turnMechanicalCheckRequired_` survive `stopBatch()`/`startBatch()` — deliberate for
the first, harmful for the second, F-04).

Sequence B — `NORMAL → FAULT A → RECOVERY → FAULT B → RECOVERY → FAULT A AGAIN`:
correct, with one caveat: because of **F-02**, once the fault table is full, the
"FAULT A AGAIN" occurrence counter and first-seen timestamp for a turn fault are
those of whatever code last owned the shared slot, so repeat-fault statistics shown
to the operator are unreliable.

Checked for and not found: stale timer, stale alarm, stale command (the HMI command
queue expires entries by `validForMs` and re-arms the alarm mask when it does),
stale persisted state, stale position.

---

## K. PERSISTENCE

Read in full, not inferred. A/B ping-pong slots, magic + schema + size + CRC32,
wrapping sequence selection, mandatory read-back-and-compare after every write,
no-op when the payload is unchanged (saves EEPROM wear), page-aware writes with
ACK polling and bounded retries, forward migration of five legacy config schemas.
The stop-intent tombstone lives in NVS and every NVS write is read back.

**The prior audit's open UNKNOWN on this subsystem is resolved in the firmware's
favour.** Full matrix in `04_POWER_LOSS_MATRIX.md` §4.1. **PASS.**

---

## L. POWER LOSS

Sixteen injection points traced (`04_POWER_LOSS_MATRIX.md` §4.2). Every one ends in
a defined, safe state. Specifically **not** found: accidental auto-resume, lost stop
intent, stale batch resurrection, wrong tray position, heating before validation.
Elapsed time is bounded by one 5-minute checkpoint and is corrected exactly from the
RTC when available (over/under-flow clamped — simulated, PASS). **PASS**, with one
side effect recorded separately: a reboot also erases the RAM-only mechanical-check
lockout (**F-07**).

---

## M. BATCH BOUNDARY

`stopBatch()` clears elapsed, checkpoint, schedule, log state and turn state, and
`startBatch()` re-initialises elapsed, counters, schedule, tray position and the
low-temperature/humidity timers. Batch-2 was then traced end to end.

Leakage from batch 1 into batch 2: `turnFaultStreak_` and
`turnMechanicalCheckRequired_` (deliberate for the streak, harmful for the lockout
— **F-04**), `heaterStuckTracking_`, `tempOscillationCrossCount_`,
`tempRateRefValue_`, `lastTurnCounterDay_`, `lightWebOverrideActive_`,
`sirenMutedUntil_`, `ssrWindowStartedAt_` — all of which self-correct on their next
evaluation window and none of which changes an actuator decision. No leakage of
fault state, alarm state, batch elapsed time, resume flag, checkpoint or stop
intent. The missing piece is the boundary itself: **F-06**, there is no automatic
end of batch.

---

## N. CONFIG / HMI / WEB / API

Command-capable paths: HMI buttons, MQTT `<root>/<deviceId>/command` (8 actions:
`batch_start`, `batch_stop`, `resume_yes`, `resume_no`, `alarm_ack`,
`autotune_start`, `firmware_check_now`, `firmware_rollback`), MQTT
`<root>/<deviceId>/config/set`, MQTT `<root>/<deviceId>/reminders/set`. The
Cloudflare Worker exposes only push-registration and firmware-metadata endpoints and
cannot issue control commands.

Good: web and HMI share one queue and one transaction mechanism, so there is exactly
one implementation of every command; all shared state is behind
`portENTER_CRITICAL`; every config write is re-sanitised in firmware regardless of
what the client validated; `nextDirection` is forcibly overwritten with the internal
scheduler value; commands are de-duplicated by `requestId`/`sequence` and expire.

Permission consistency (UI == backend == runtime):

| Field | HMI lock | Firmware lock | Web lock | Verdict |
|---|---|---|---|---|
| totalIncubationDays | locked during batch | locked during batch | **not locked** | **mismatch — F-10** |
| Offline→Online connectivity | not locked | locked during batch | not locked | mismatch (fails safe; the whole save is rejected with feedback) |
| turningEnabled / turnIntervalMin / turnMaxRunSec | unlocked (with CO/HUY confirm) | unlocked | unlocked | consistent (prior F-01 fixed) |
| everything else | unlocked | unlocked | unlocked | consistent |

Config changes were injected while IDLE, RUNNING, TURNING, with a fault active and
while resume-pending: the save is a single all-or-nothing transaction with rollback
and explicit operator feedback on both HMI and web; PID re-tuning is bumpless; new
thresholds are re-evaluated by `updateAlarms()` on the very next cycle.

**Transport security is the failure here, not the logic: F-01.**

---

## O. REGRESSION AGAINST OLD AUDIT

| Old Finding | Current Status | Evidence | Regression |
|---|---|---|---|
| F-01 protectedBatchChange vs UI lock | FIXED | STATIC | New, different mismatch on the web side → F-10 |
| F-02 no turn-fault retry ceiling | FIXED (streak + E205) | STATIC | **Yes** → F-04 and F-07 |
| F-03 no resume-confirmation escalation | FIXED (E135, E137) | STATIC | No |
| F-04 stale trayPosition after abrupt stop | CHANGED (fixed in `stopTurn`, still open in `latchTurnFault`) | STATIC | Residual → F-11 |
| F-05 E115 tracking reset by a sensor blip | FIXED (now pauses) | STATIC | No |
| F-06 5-minute siren mute | CHANGED (now 60 s) | STATIC | Message text not updated → F-12 |
| F-07 batch-overdue only via Cloud Push | FIXED (E136 local) | STATIC | No |
| F-08 PersistentStore torn write UNKNOWN | NO LONGER APPLICABLE (verified sound) | STATIC | No |
| F-09 processResume missing turningEnabled check | FIXED (ResumeBlockReason::TurningDisabled) | STATIC | No |

Detail in `05_REGRESSION.md`.

---

## P. FINDINGS

| ID | Type | Severity | Evidence | Confidence | Status | Release Blocker |
|---|---|---|---|---|---|---|
| F-01 | CONFIRMED BUG | CRITICAL | STATIC | HIGH | CONFIRMED | YES |
| F-02 | CONFIRMED BUG | HIGH | SIMULATED | HIGH | CONFIRMED | YES |
| F-03 | DESIGN RISK | HIGH | STATIC | HIGH | CONFIRMED | YES |
| F-04 | CONFIRMED BUG | HIGH | STATIC | HIGH | CONFIRMED | YES |
| F-05 | DESIGN RISK | MEDIUM | STATIC | HIGH | CONFIRMED | NO |
| F-06 | MISSING FEATURE | MEDIUM | STATIC | HIGH | CONFIRMED | NO |
| F-07 | DESIGN RISK | MEDIUM | STATIC | HIGH | CONFIRMED | NO |
| F-08 | MISSING ALARM | MEDIUM | STATIC | MEDIUM | CONFIRMED | NO |
| F-09 | DESIGN RISK | MEDIUM | STATIC | HIGH | CONFIRMED | NO |
| F-10 | CONFIRMED BUG | LOW | STATIC | HIGH | CONFIRMED | NO |
| F-11 | DESIGN RISK | LOW | STATIC | HIGH | CONFIRMED | NO |
| F-12 | CONFIRMED BUG | LOW | STATIC | HIGH | CONFIRMED | NO |
| F-13 | DESIGN RISK | LOW | STATIC | MEDIUM | CONFIRMED | NO |
| F-14 | DESIGN RISK | LOW | STATIC | MEDIUM | UNCONFIRMED | NO |
| F-15 | POTENTIAL BUG | LOW | STATIC | MEDIUM | CONFIRMED | NO |

---

```text
ID: F-01
Title: The documented release build ships an unauthenticated, unencrypted public
       MQTT command channel that can start/stop batches and acknowledge alarms

Type: CONFIRMED BUG
Severity: CRITICAL
Status: CONFIRMED
Evidence Level: STATIC
Confidence: HIGH

Location: config.h:79-107; transaction_bridge.h:63-114, 355-441, 673-702;
          .github/workflows/build-firmware.yml:69-100; config.js:7-10; README.md:118,169
Function: MayapRealtimeInternal::handleCommandMessage(), subscribeAll(), connectMqtt()
Relevant variables: MAYAP_MQTT_HOST, MAYAP_MQTT_PORT, MAYAP_MQTT_USE_TLS,
          MAYAP_MQTT_USERNAME, MAYAP_MQTT_PASSWORD, MAYAP_MQTT_TOPIC_ROOT, deviceId

Trigger: Any machine whose operator selects ConnectivityMode::Online while running a
         binary produced by the project's own release pipeline.

Preconditions:
  - config_.connectivityMode == Online (operator opt-in; default is Offline)
  - the machine has internet access

Execution Path:
  1. config.h defines the MQTT macros with `#ifndef` fallbacks:
     host = "broker.emqx.io", port = 1883, TLS = 0, username = "", password = "",
     topic root = "mayap/v1".
  2. .github/workflows/build-firmware.yml invokes `arduino-cli compile` with only
     --fqbn and --output-dir. No `--build-property` and no build_flags are passed,
     so none of those macros is ever overridden. This workflow is the documented
     release mechanism (README.md:109) and its .bin is what the Cloudflare Worker
     serves for remote OTA (README.md:107-112).
  3. transaction_bridge.h:69 builds deviceId = "MAP-<MAC>"; topicOf() = "mayap/v1/MAP-.../<x>".
  4. connectMqtt() passes nullptr for user and password when the macros are empty,
     i.e. it connects anonymously, in cleartext, to a public broker.
  5. subscribeAll() subscribes to command, config/set, reminders/set and session.
  6. handleCommandMessage() accepts any JSON with an "action" string and enqueues it
     into the same command queue the physical HMI uses. There is no PIN, token,
     signature, nonce or pairing check on this path — the only filters are a
     duplicate requestId and a monotonically increasing sequence number, both
     supplied by the caller.
  7. Reachable actions: batch_start, batch_stop, resume_yes, resume_no, alarm_ack,
     autotune_start, firmware_check_now, firmware_rollback.
  8. Device discovery is trivial: every device publishes its own snapshot/presence on
     the same public broker, so `mayap/v1/+/snapshot` enumerates all online machines,
     and the deviceId is also printed on the machine's QR label (qr/).

Current Behavior: A third party anywhere on the internet can stop a running
  incubation batch (batch_stop → stop tombstone → EEPROM batch record erased → the
  batch cannot be resumed, only restarted from day 0), reject a pending power-loss
  resume (resume_no, same destruction), acknowledge and clear latched turn faults,
  re-mute the emergency-temperature siren every 60 s indefinitely, start an
  unattended batch, start an auto-tune, or force a firmware rollback and reboot.
  Config/set additionally lets them rewrite setpoints and alarm thresholds within the
  sanitiser's bounds, and switch autoResumeOnPowerLoss and highTempAlarmWithoutBatch.

Expected Behavior: Control commands must be authenticated end to end. The release
  pipeline must not be able to emit a binary that points at the public test broker,
  and README's "do not use the public broker for commercial machines" instruction must
  have an enforcement path on the firmware side, not only on the web side.

Impact: Loss of control authority over a deployed machine; destruction of a 21-day
  batch by a single unauthenticated message; suppression of the audible emergency
  alarm.

Physical Safety Impact: YES
  (repeated remote alarm_ack keeps the siren relay de-energised while
  EmergencyTemperature is active; the heat cut-off itself is not defeated)

Operational Impact: CRITICAL — total batch loss, remotely, with no operator action.

Recovery: Operator must set the machine back to Offline mode, or the fleet must be
  re-flashed with a private broker and credentials.

Regression Risk: Changing the defaults is low-risk for the firmware logic but will
  break the existing web dashboard configuration (config.js) unless both are changed
  together.

Evidence: The four files quoted above; no `--build-property` / build_flags anywhere in
  the repository (verified by search), and no authentication check anywhere in
  handleCommandMessage()'s call chain.

Recommended Direction: Make the release build fail (or refuse to enable Online mode)
  unless a private broker host, TLS and per-device credentials have been supplied at
  build time; add a per-device shared secret or signed-command check on the
  command/config topics. No code was changed by this audit.
```

```text
ID: F-02
Title: Fault table overflows — all four turn-fault codes are silently erased from
       the alarm system within 30 seconds of being raised

Type: CONFIRMED BUG
Severity: HIGH
Status: CONFIRMED
Evidence Level: SIMULATED
Confidence: HIGH

Location: machine_control.h:369-421 (FaultCode enum), 706-745 (FaultManager::slot,
          clearState, states_), 4626-4888 (updateAlarms), 3592-3712 (serviceHealth*),
          5257-5282 (latchTurnFault)
Function: FaultManager::slot() overflow branch
Relevant variables: MAX_FAULTS (32), count_, states_[], FaultCode::TurnLimitConflict /
          TurnTimeout / TurnLimitStuck / TurnCommandConflict, FaultCode::StorageRetryTrend

Trigger: Any turn fault (E201/E202/E203/E204) raised more than ~90 seconds after boot.

Preconditions: none beyond normal operation. EXTERNAL_EEPROM_ENABLED is true, so the
  health monitor's fourth code is always allocated.

Execution Path:
  1. There are 36 real FaultCode values; MAX_FAULTS is 32. The in-code comment still
     says "Hien co 28 ma FaultCode thuc", which is stale.
  2. FaultManager::slot() allocates a slot on the FIRST call for a code — including
     a call with condition == false — and never frees it.
  3. Every control cycle unconditionally calls faults_.set() for 28 distinct codes:
     RtcFailure (update), ResumeConfirmationPending + ResumeRtcWaitTooLong
     (processResume), 23 codes in updateAlarms(), OutputConflict + RelayRateExceeded
     (syncOutputFaults).
  4. From HEALTH_BASELINE_CAPTURE_DELAY_MS (60 s) and then every
     HEALTH_CHECK_INTERVAL_MS (30 s), serviceHealthMonitor() adds four more
     unconditionally: HeapCritical, HeapLow, TemperatureTrendWarning,
     StorageRetryTrend. count_ reaches exactly 32 — the table is full.
  5. The only four codes that are never set literally are the four turn-fault codes;
     they reach FaultManager only through latchTurnFault()/clearTurnFault().
  6. latchTurnFault(TurnTimeout) -> set() -> slot() finds nothing, count_ == MAX_FAULTS,
     so it takes the overflow branch: states_[31].code = TurnTimeout, reusing the
     FaultState that currently belongs to StorageRetryTrend. The fault is raised
     correctly at this instant.
  7. At the next health tick (<= 30 s later) serviceHealthStorageRetry() calls
     set(StorageRetryTrend, false). slot() again finds nothing and again rewrites
     states_[31].code — now back to StorageRetryTrend — while state.active is still
     true from the turn fault. set() then evaluates the StorageRetryTrend descriptor,
     whose latching flag is false, and calls clearState().
  8. Result: the active turn fault is cleared, and the event log records
     "FaultCleared: StorageRetryTrend" instead.

Current Behavior: E201/E202/E203/E204 appear on the HMI, in runtime_.alarmMask, in the
  event log and in the Cloud Push feed for at most one health interval, then vanish and
  are replaced by a false "storage retry trend cleared" entry. The occurrence counter
  and first-seen timestamp of any turn fault are also corrupted, because the slot is
  shared. The machine does NOT become unsafe: turnFaultLatched_ is a separate
  MachineController member, so turning stays blocked, the motor outputs stay off and
  the HMI state text still reads "LOI DAO"; acknowledgement still works. What is lost
  is the alarm itself.

Expected Behavior: A raised Stop-severity fault must remain active in the fault manager
  until its condition clears and it has been acknowledged. Fault codes must not share
  storage.

Impact: An operator (or the remote alerting path) can miss that turning has stopped.
  The escalation to E205 after three consecutive failures still works, because
  TurnMechanicalCheckRequired does have its own slot.

Physical Safety Impact: NO (no dangerous output is enabled by this)

Operational Impact: HIGH — the primary alarm for "the eggs are no longer being turned"
  disappears within 30 seconds, and repeat-fault statistics are unreliable.

Recovery: None available to the operator; the alarm simply does not persist.

Regression Risk: Enlarging MAX_FAULTS is a RAM-only change (FaultState is small) and
  touches no control logic; a static_assert tying MAX_FAULTS to the number of enum
  entries would prevent recurrence.

Evidence: audit/sim/fault_slots.cpp reproduces FaultManager::slot()/set()/clearState()
  verbatim together with the real per-cycle call order; all 13 assertions pass,
  including "SAU 1 NHIP HEALTH (<=30s): E202 BI XOA khoi FaultManager" and
  "Su kien ghi vao nhat ky la 'CLEARED:StorageRetryTrend' (SAI ma)".

Recommended Direction: Size the slot array from the enum (or raise MAX_FAULTS well
  above the code count) and add a compile-time assertion; alternatively make the
  overflow branch refuse to reuse a slot whose state is active. No code was changed.
```

```text
ID: F-03
Title: dropHeatMaster is inert during a running batch — the master contactor stays
       energised for every safety fault except emergency temperature

Type: DESIGN RISK
Severity: HIGH
Status: CONFIRMED
Evidence Level: STATIC
Confidence: HIGH

Location: machine_control.h:440-519 (descriptor table), 5386-5445
          (updateHeatingAndOutputs), 2975-3000 and 3028-3060 (OutputArbiter::update)
Function: MachineController::updateHeatingAndOutputs() / OutputArbiter::update()
Relevant variables: normalMasterPermit, heatDemandContext, req.heatMaster,
          req.immediateMasterDrop, FaultManager::masterDropRequired()

Trigger: Any fault whose descriptor sets dropHeatMaster == true, raised while a batch
  is running and the physical heater switch is ON. That is: SensorLost (E101),
  SensorInvalid (E102), SensorSuspect (E103), StorageUnavailable (E301), AbnormalReset
  (E303), OutputConflict (E304), BatchStateClearPending (E313), SafetyJournalUnavailable
  (E314).

Preconditions: batchRunning_ == true, in.heaterEnable == true, emergencyActive_ == false.

Execution Path:
  1. normalMasterPermit = in.heaterEnable && !emergencyActive_ && heatDemandContext.
     It deliberately does not consult faults_.masterDropRequired().
  2. req.heatMaster = masterPermit = normalMasterPermit || tunePermit  ->  TRUE.
  3. req.immediateMasterDrop is set TRUE by the same faults (it includes
     faults_.masterDropRequired(), !sensorUsable_, storageFaultLatched_,
     batchClearPending_, safetyJournalFaultLatched_, abnormalResetLatched_).
  4. OutputArbiter::update() reads request.immediateMasterDrop ONLY inside the branch
     `if (!request.heatMaster || request.forceAllSafe) { ... }`. With
     request.heatMaster == true the code takes the else branch, which energises (or
     keeps energised) PIN_OUT_HEAT_MASTER.
  5. The SSR is correctly withheld: heaterPidConditions includes
     !faults_.masterDropRequired() and sensorUsable_, so req.heaterSsr is false.

Current Behavior: The second, independent protection layer declared by the fault table
  never operates during a batch. The line-voltage contactor feeding the heater bank
  remains closed while the firmware believes it has commanded it open. The declared
  behaviour of eight fault codes and the actual behaviour disagree. The in-code comment
  at normalMasterPermit documents this as an installer request ("the physical switch is
  the highest authority while incubating; only emergency over-temperature may override
  it"), so it is an intentional change — but the descriptor table was not updated to
  match, and the consequence below does not appear to have been assessed.

  The hazardous combination is: SSR fails shorted (a common real failure mode,
  explicitly named in the code comments) AND the temperature sensor is lost or invalid.
  With the sensor gone, safetySampleValid_ is false, so emergencyActive_ can never
  trip, so nothing opens the contactor, so the heater bank runs at full power with no
  measurement and no cut-off. Under the descriptor table's stated behaviour, E101 alone
  would have opened it.

Expected Behavior: Either (a) faults with dropHeatMaster == true must be able to open
  the contactor even when the heater switch is on, or (b) the dropHeatMaster column
  must be removed for those codes and an alternative independent over-temperature
  cut-off (e.g. a hardware thermostat in series with the contactor coil) must be
  documented as the compensating control.

Impact: Loss of the declared second safety layer; a credible two-failure path to
  uncontrolled heating with no automatic cut-off.

Physical Safety Impact: YES

Operational Impact: HIGH

Recovery: Manual — the operator must switch off the physical heater switch or the
  machine's supply.

Regression Risk: Re-adding masterDropRequired() to the permit would reverse an explicit
  installer request and would cause the contactor to drop on every transient sensor
  loss, so it needs an owner decision rather than a straight revert.

Evidence: the three code locations quoted; the descriptor table's own comment
  ("dropHeatMaster: bat buoc nha contactor tong nhiet va dong thoi cam D1") contradicted
  by the arbiter's branch structure.

Recommended Direction: Owner decision required. Whichever branch is chosen, make the
  descriptor table and the arbiter agree, and record the compensating control for
  "SSR shorted + sensor lost" explicitly. No code was changed.
```

```text
ID: F-04
Title: A new batch can be started while the turning mechanical-check lockout (E205)
       is active, so the whole batch runs with turning permanently inhibited

Type: CONFIRMED BUG
Severity: HIGH
Status: CONFIRMED
Evidence Level: STATIC
Confidence: HIGH

Location: machine_control.h:4286-4310 (startBatch gate list), 5257-5282
          (latchTurnFault), 5479-5489 (enterTestMode), 5557-5585 (updateTestMode),
          4864-4866 (E205 fault set)
Function: MachineController::startBatch()
Relevant variables: turnMechanicalCheckRequired_, turnFaultStreak_, turnFaultLatched_,
          testLimitVerifiedLeft_, testLimitVerifiedRight_

Trigger: Three consecutive TurnTimeout/TurnLimitStuck faults with no successful move in
  between, followed by the operator acknowledging, stopping the batch and starting a
  new one.

Preconditions: turnMechanicalCheckRequired_ == true (set by latchTurnFault once
  turnFaultStreak_ reaches TURN_FAULT_STREAK_LIMIT = 3).

Execution Path:
  1. latchTurnFault() sets turnMechanicalCheckRequired_ = true and clears both
     testLimitVerified* flags. E205 (Stop severity, inhibitsTurning) is raised by
     updateAlarms() every cycle from that variable.
  2. The only place turnMechanicalCheckRequired_ is cleared is updateTestMode(), and
     only after BOTH limit switches have been individually confirmed in Test Mode.
  3. enterTestMode() refuses while batchRunning_ || resumePending_, so the lockout can
     only be cleared with no batch running.
  4. The operator sends AlarmAck. clearTurnFault() clears turnFaultLatched_ and the
     individual turn code, but not turnMechanicalCheckRequired_.
  5. The operator stops the batch and starts a new one. startBatch()'s gate list checks
     turnFaultLatched_ (now false) and the limit switches, but never checks
     turnMechanicalCheckRequired_ or faults_.turningInhibited().
  6. The new batch starts. updateTurning() returns early on
     faults_.turningInhibited() every cycle for the entire batch.

Current Behavior: A fresh 21-day batch begins and never turns a single time. E205 is
  displayed in the fault list, but nothing blocks the start and — because turning never
  runs — no further turn fault is ever raised to re-alert the operator.

Expected Behavior: startBatch() should refuse while turning is inhibited by a latched
  mechanical-check requirement, with a message directing the operator to Test Mode,
  exactly as it already refuses for turnFaultLatched_ and !config_.turningEnabled.

Impact: Complete loss of egg turning for a whole batch — the failure the E205
  escalation was added to prevent.

Physical Safety Impact: NO

Operational Impact: HIGH — total batch loss (unturned eggs do not hatch).

Recovery: Stop the batch, enter Test Mode, verify both limit switches, restart.

Regression Risk: Very low; adding one gate to startBatch() mirrors existing gates.

Evidence: the five code locations quoted; startBatch()'s complete gate list contains no
  reference to turnMechanicalCheckRequired_ or faults_.turningInhibited().

Recommended Direction: Add the missing start gate (and consider the same gate in
  processResume()). No code was changed.
```

```text
ID: F-05
Title: Over-temperature protection (E111/E112) can be disabled entirely during Auto
       Tune when "temperature alarm outside a batch" is switched off

Type: DESIGN RISK
Severity: MEDIUM
Status: CONFIRMED
Evidence Level: STATIC
Confidence: HIGH

Location: machine_control.h:4638 (tempAlarmEligible), 4641-4673 (trip logic),
          4410-4435 (startAutoTune), 5433-5441 (tunePermit); config.h:989
Function: MachineController::updateAlarms()
Relevant variables: config_.highTempAlarmWithoutBatch, batchRunning_,
          tempAlarmEligible, highTemperatureActive_, emergencyActive_

Trigger: Operator disables highTempAlarmWithoutBatch (HMI or web) and then starts an
  Auto Tune.

Preconditions: batchRunning_ == false (Auto Tune refuses to run during a batch).

Execution Path:
  1. tempAlarmEligible = config_.highTempAlarmWithoutBatch || batchRunning_  ->  false.
  2. Both the E111 and the E112 trip conditions are gated on tempAlarmEligible, so
     neither can ever become active while auto-tuning.
  3. tunePermit contains !highTemperatureActive_ && !emergencyActive_ — both are
     permanently false, so that guard is vacuous.
  4. updateAutoTune()'s own abort list also tests highTemperatureActive_ /
     emergencyActive_ — likewise vacuous.
  5. startAutoTune() refuses if targetTemp + autotuneBandC >= highTempAlarm, which
     presumes the high alarm is live.

Current Behavior: Auto Tune drives the heater in relay mode with no over-temperature
  alarm and no emergency contactor drop. Bounding is provided only by the relay
  algorithm itself (power capped at autotuneRelayPowerPercent, heat phase ends at
  target + band) plus AUTOTUNE_MAX_MS / AUTOTUNE_PHASE_MAX_MS, all of which depend on
  the sensor reading being correct.

Expected Behavior: The emergency ladder should remain armed whenever the firmware is
  actively driving the heater, regardless of the "outside a batch" preference, which
  was introduced to stop nuisance alarms in a hot room rather than to disable
  protection during heating.

Impact: A configuration intended to silence nuisance alarms silently removes the
  over-temperature cut-off during the one non-batch activity that heats the machine.

Physical Safety Impact: YES

Operational Impact: MEDIUM (Auto Tune is an occasional, attended commissioning task).

Recovery: Re-enable highTempAlarmWithoutBatch.

Regression Risk: Low — extending tempAlarmEligible with `|| autotune_.running()` does
  not affect the nuisance-alarm case the option was created for.

Evidence: the code locations quoted.

Recommended Direction: Make heat-demand context, not batch state, the eligibility
  condition. No code was changed.
```

```text
ID: F-06
Title: No automatic end of batch — heating and control continue indefinitely past the
       configured incubation period

Type: MISSING FEATURE
Severity: MEDIUM
Status: CONFIRMED
Evidence Level: STATIC
Confidence: HIGH

Location: machine_control.h:3570 (BatchPhase enum), 5631-5658 (updateBatchTime),
          4839-4847 (BatchOverdue), 4358-4408 (stopBatch)
Function: MachineController::updateBatchTime()
Relevant variables: batchPhase_, config_.totalIncubationDays, runtime_.currentDay,
          FaultCode::BatchOverdue

Trigger: A batch reaches and passes config_.totalIncubationDays with no operator action.

Preconditions: none.

Execution Path:
  1. BatchPhase has no Complete/Finished value; the enum is {Stopped, Prestart,
     Homing, Running}.
  2. updateBatchTime() computes the day index and updates runtime_.currentDay, but has
     no branch that ends the batch.
  3. updateAlarms() raises FaultCode::BatchOverdue — Warning severity, no inhibit flags
     at all — once elapsed days >= totalIncubationDays.
  4. Nothing else changes. The PID keeps holding the setpoint, the fans keep running,
     the turning lockdown stays engaged, checkpoints keep being written, and the batch
     record keeps wasRunning = 1, for ever.

Current Behavior: The machine heats indefinitely after the hatch date until a human
  presses Stop. If the site is unattended, a finished batch silently becomes a heater
  running at 37.5 °C with an empty or hatched chamber, and the only signal is a
  Warning-level fault plus a Cloud Push (which requires network and a subscription).

Expected Behavior: Either an explicit completion state that stops heat and turning and
  raises a distinct end-of-batch event, or — if continuing is deliberate, because
  chicks must be kept warm until collection — an escalating, latched alarm rather than
  a plain Warning, and a documented statement that the operator must stop the batch.

Impact: Indefinite unattended heating; the batch lifecycle has no terminal state.

Physical Safety Impact: NO (all normal temperature protection remains active)

Operational Impact: MEDIUM — wasted energy, and the next batch cannot be started until
  someone notices.

Recovery: Operator presses Stop.

Regression Risk: Adding an automatic stop would be a behavioural change that could
  surprise operators who rely on continued warmth after hatching; needs an owner
  decision.

Evidence: exhaustive search for writes to batchPhase_ and for calls to stopBatch();
  the only stopBatch() callers are the HMI/MQTT BatchStop command.

Recommended Direction: Owner decision on the intended end-of-batch behaviour, then make
  the code express it. No code was changed.
```

```text
ID: F-07
Title: The turning mechanical-check lockout and the fault streak are RAM-only, so a
       power cycle silently lifts them

Type: DESIGN RISK
Severity: MEDIUM
Status: CONFIRMED
Evidence Level: STATIC
Confidence: HIGH

Location: machine_control.h:6437-6443 (member declarations), 5257-5282 (latchTurnFault),
          1464-1473 (PackedBatchV1), 5703-5725 (saveBatchRecord)
Function: MachineController (member lifetime) / PersistentStore batch record contents
Relevant variables: turnMechanicalCheckRequired_, turnFaultStreak_,
          testLimitVerifiedLeft_, testLimitVerifiedRight_, PackedBatchV1

Trigger: Power loss, brownout, watchdog reset, health-monitor restart or OTA restart
  while the mechanical-check lockout is latched.

Preconditions: turnMechanicalCheckRequired_ == true.

Execution Path:
  1. turnMechanicalCheckRequired_ and turnFaultStreak_ are plain members of the single
     static MachineController instance; they are re-initialised to false/0 on every boot.
  2. PackedBatchV1 persists wasRunning, nextDirection, turnCountToday, turnCountBatch,
     elapsedSec, checkpointEpoch, batchStartEpoch and lastTurnEpoch. It contains no
     turning-health field.
  3. After a reboot the batch resumes (or is restarted) with the lockout gone, the
     streak back at zero and E205 absent.

Current Behavior: A machine locked out for suspected mechanical damage returns to
  normal automatic turning after any restart, and must accumulate three fresh
  consecutive failures before the lockout reappears — driving the damaged mechanism
  three more times in the process.

Expected Behavior: A maintenance-required latch should outlive a reboot, like the stop
  intent does (the SafetyJournal NVS namespace is already available for exactly this
  kind of flag).

Impact: The escalation ceiling that was added to stop repeated blind retries can be
  cleared by an event the operator does not control.

Physical Safety Impact: NO

Operational Impact: MEDIUM — repeated mechanical stress, delayed diagnosis.

Recovery: The lockout re-latches after three more failures.

Regression Risk: Low; one more NVS key, cleared in the same place Test Mode clears the
  RAM flag.

Evidence: the code locations quoted; PackedBatchV1's full field list.

Recommended Direction: Persist the lockout (and ideally the streak) in SafetyJournal.
  No code was changed.
```

```text
ID: F-08
Title: A sensor frozen at a plausible constant value is never detected

Type: MISSING ALARM
Severity: MEDIUM
Status: CONFIRMED
Evidence Level: STATIC
Confidence: MEDIUM

Location: machine_control.h:3936-4030 (processSensor), 2494-2792 (SHT485Industrial),
          4770-4788 (HeaterNotHeating), 3650-3684 (serviceHealthTemperatureTrend)
Function: MachineController::processSensor()
Relevant variables: sensorUsable_, safetySampleValid_, latestFrameValid_,
          lastAcceptedTemperature_, heaterStuckTracking_

Trigger: The RS485 sensor keeps answering with well-formed, CRC-correct frames carrying
  a constant value (firmware hang inside the sensor, a latched ADC, a frozen bridge or
  converter).

Preconditions: frames continue to arrive within sensorTimeoutSec.

Execution Path:
  1. Validity is judged per frame: dataValid(), isfinite(), transport age, plus a
     plausibility test that only rejects large DOWNWARD steps. A constant value passes
     every one of them.
  2. sensorUsable_ therefore stays true and the PID keeps regulating to a value that no
     longer moves.
  3. If the frozen value sits at or above the setpoint, the PID commands ~0 % power and
     the chamber cools. LowTemperature (E110) is computed from the same frozen
     temperature_, so it cannot fire.
  4. HeaterNotHeating (E115) requires the SSR to be continuously commanded ON; with
     ~0 % demand it never starts tracking.
  5. serviceHealthTemperatureTrend() measures the rate of change of the same frozen
     value — zero — so it raises nothing.

Current Behavior: The chamber drifts to ambient with no alarm of any kind. There is no
  staleness check on the VALUE (only on the transport), no cross-check against a second
  sensor, and no "heat commanded but temperature has not moved at all" detector that is
  independent of the SSR duty.

Expected Behavior: A "sensor value unchanged for N minutes while the machine is
  running" warning, at minimum during a batch.

Impact: The single most consequential silent failure mode for an incubator that has one
  temperature sensor.

Physical Safety Impact: NO in the cooling direction. If the value froze BELOW the true
  temperature, the PID would keep heating and the over-temperature ladder — which reads
  the same frozen value — could not intervene; that direction would be YES, but it
  requires the freeze to happen at a low reading.

Operational Impact: MEDIUM to HIGH depending on which value the sensor froze at.

Recovery: None automatic.

Regression Risk: Low — a pure detection addition with no actuator authority.

Evidence: the four code locations quoted; exhaustive search for any comparison of
  consecutive accepted samples for equality or for a zero-variance window (none found).
  Confidence MEDIUM rather than HIGH because the sensor's own internal watchdog
  behaviour is unknown (see U-02) and might make this failure mode rare.

Recommended Direction: Add a value-staleness detector. No code was changed.
```

```text
ID: F-09
Title: Remote acknowledgement can clear a latched turn fault and re-mute the emergency
       siren with nobody at the machine

Type: DESIGN RISK
Severity: MEDIUM
Status: CONFIRMED
Evidence Level: STATIC
Confidence: HIGH

Location: machine_control.h:4118-4143 (AlarmAck case), 5284-5301 (clearTurnFault);
          transaction_bridge.h:376, 405-421
Function: MachineController::processHmiTransactions() / clearTurnFault()
Relevant variables: sirenMutedUntil_, turnFaultLatched_, turnFaultCode_,
          SIREN_TEMPORARY_MUTE_MS

Trigger: An `alarm_ack` command arriving over MQTT.

Preconditions: connectivityMode == Online.

Execution Path:
  1. handleCommandMessage() maps "alarm_ack" to HmiCommandType::AlarmAck and fills the
     alarm mask from the last published runtime snapshot.
  2. The AlarmAck case mutes the siren for SIREN_TEMPORARY_MUTE_MS whenever
     emergencyActive_, and calls clearTurnFault() whenever turnFaultLatched_.
  3. clearTurnFault() refuses only if both limits are closed or a manual turn input is
     held. It cannot tell whether a human has inspected the mechanism.
  4. The command carries no indication of whether it came from the physical panel or
     from the internet — both sources share one queue by design.

Current Behavior: A remote user can repeatedly clear turn faults (driving a failing
  mechanism again and again, bounded only by the three-strike E205 rule) and can keep
  an active emergency siren silent indefinitely by re-sending alarm_ack every 60 s.

Expected Behavior: Acknowledging a Stop/Emergency-severity fault that requires physical
  inspection should either be restricted to the local HMI or require an explicit,
  audited remote override.

Impact: Weakens the assumption that an acknowledgement means "a human has looked at
  the machine".

Physical Safety Impact: YES (audible alarm suppression only; the heat cut-off is not
  affected by acknowledgement)

Operational Impact: MEDIUM

Recovery: n/a

Regression Risk: Low if the split is limited to the emergency siren and the turn-fault
  clear.

Evidence: the code locations quoted. Note this finding is largely subsumed by F-01
  while the transport is unauthenticated; it remains valid on its own after F-01 is
  fixed, for a legitimate but remote operator.

Recommended Direction: Distinguish the command source and restrict physical-inspection
  acknowledgements to the local panel. No code was changed.
```

```text
ID: F-10
Title: The web dashboard does not lock totalIncubationDays during a batch, although
       the HMI and the firmware both do

Type: CONFIRMED BUG
Severity: LOW
Status: CONFIRMED
Evidence Level: STATIC
Confidence: HIGH

Location: app.js:1088-1090, 1178-1185; hmi.h:1035-1049; machine_control.h:4053-4061
Function: app.js batch-form submit / MachineController::processHmiTransactions()
Relevant variables: config.totalIncubationDays, protectedBatchChange, saveAllowed

Trigger: Operator edits any field of the web "batch" form while a batch is running.

Preconditions: batchRunning_ || resumePending_.

Execution Path:
  1. hmi.h::settingLockedDuringBatch() returns true for totalIncubationDays and shows
     "KHOA" on the local screen.
  2. app.js renders the same value in an ordinary enabled input and always includes it
     in the submitted config.
  3. processHmiTransactions() computes protectedBatchChange from a comparison of the
     submitted value with the current one, so the save succeeds if the field happens to
     match and is rejected wholesale if it does not.
  4. On rejection the whole transaction fails, including any other field the operator
     changed in the same form (for example targetTemp), and the returned message does
     not state that an in-batch policy lock is the cause.

Current Behavior: A web operator can change the incubation length in the form, submit,
  and have every change in that form silently discarded with a generic error.

Expected Behavior: The web form should disable and label the field during a batch, the
  way the HMI does.

Impact: Operator confusion; an intended temperature change may be lost.

Physical Safety Impact: NO

Operational Impact: LOW

Recovery: Re-submit from a different form group.

Regression Risk: None (web-only change).

Evidence: the three code locations quoted.

Recommended Direction: Mirror the HMI lock in app.js and return a specific reason
  string from the firmware. No code was changed.
```

```text
ID: F-11
Title: latchTurnFault() bypasses stopTurn(), leaving trayPosition_ at the stale move
       origin while the fault is latched

Type: DESIGN RISK
Severity: LOW
Status: CONFIRMED
Evidence Level: STATIC
Confidence: HIGH

Location: machine_control.h:5242-5256 (stopTurn), 5257-5282 (latchTurnFault),
          5024-5035 (updateTurning entry)
Function: MachineController::latchTurnFault()
Relevant variables: trayPosition_, needHome_, turnPhase_, moveOrigin_

Trigger: TurnTimeout or TurnLimitStuck raised while the tray is physically mid-travel.

Preconditions: turnPhase_ == MovingLeft or MovingRight.

Execution Path:
  1. stopTurn() contains the invalidation logic: if the phase is Moving it sets
     trayPosition_ = Unknown and needHome_ = true (this was the prior audit's F-04 fix).
  2. latchTurnFault() does not call stopTurn(); it assigns turnPhase_ = Fault directly.
  3. On the next cycle updateTurning() does call stopTurn(), but the phase is already
     Fault, so the Moving branch is not taken and the invalidation never happens.
  4. trayPosition_ therefore still reports the origin of the interrupted move, which is
     not where the tray physically is.

Current Behavior: The stale position is exposed to the HMI and to any reader for as
  long as the fault is latched. It is not acted upon, because every consumer of
  trayPosition_ inside updateTurning() is unreachable while turnFaultLatched_ is true,
  and clearTurnFault() (and startBatch(), and the AUTO transition) all recompute the
  position from the live limit switches before turning can resume.

Expected Behavior: latchTurnFault() should invalidate the position for the same reason
  stopTurn() does.

Impact: Display only, in the traced paths.

Physical Safety Impact: NO

Operational Impact: LOW

Recovery: Automatic at the next acknowledgement or batch start.

Regression Risk: None.

Evidence: the three code locations quoted; every read of trayPosition_ was
  cross-referenced against the turnFaultLatched_ early-return.

Recommended Direction: Call stopTurn() before setting the Fault phase. No code changed.
```

```text
ID: F-12
Title: The siren-mute confirmation message says 5 minutes; the actual mute is 1 minute

Type: CONFIRMED BUG
Severity: LOW
Status: CONFIRMED
Evidence Level: STATIC
Confidence: HIGH

Location: machine_control.h:4137 ("COI TAM DUNG 5 PHUT"); config.h:565
          (SIREN_TEMPORARY_MUTE_MS = 60000UL)
Function: MachineController::processHmiTransactions(), AlarmAck case
Relevant variables: sirenMutedUntil_, SIREN_TEMPORARY_MUTE_MS

Trigger: Acknowledging an active emergency-temperature alarm.

Preconditions: emergencyActive_ == true.

Execution Path: sirenMutedUntil_ = now + 60 000 ms, while the message returned to the
  HMI and to the web client states five minutes.

Current Behavior: The siren restarts after one minute; the operator was told five.
  (The shortened mute is itself an improvement over the previously audited value.)

Expected Behavior: The message must match the constant.

Impact: Operator confusion only.

Physical Safety Impact: NO
Operational Impact: LOW
Recovery: n/a
Regression Risk: None (string only).
Evidence: the two locations quoted.
Recommended Direction: Derive the message text from the constant. No code was changed.
```

```text
ID: F-13
Title: A manual turn during a batch does not re-anchor the automatic turn schedule

Type: DESIGN RISK
Severity: LOW
Status: CONFIRMED
Evidence Level: STATIC
Confidence: MEDIUM

Location: machine_control.h:5133-5167 (updateManualTurning), 5169-5187 (requestTurn),
          5208-5231 (completeTurn), 4976-4982 (setTurnScheduleAnchor)
Function: MachineController::completeTurn()
Relevant variables: moveCounts_, lastTurnAt_, lastTurnEpoch_, nextTurnAt_

Trigger: During a running batch the operator switches AUTO off, turns the tray by hand
  with the panel switches, then switches AUTO back on.

Preconditions: batchRunning_ == true.

Execution Path:
  1. Manual moves are requested with countMove == false, so completeTurn() updates
     trayPosition_ but does not call setTurnScheduleAnchor() and does not persist.
  2. processInputModeTransition() calls scheduleNextTurnFromAnchor() when AUTO returns,
     which recomputes the due time from the OLD lastTurnEpoch_.
  3. If the interval had already elapsed, nextTurnAt_ = now and the machine performs an
     automatic turn immediately after the manual one.

Current Behavior: Possible immediate duplicate turn, and the recorded turn counters do
  not include manual moves.

Expected Behavior: Arguable. A manual move is a real tray movement and plausibly should
  reset the interval; the code's choice not to count it toward the daily total is
  defensible, but the two decisions are bundled together.

Impact: One extra tray movement; turn statistics understate actual movement.

Physical Safety Impact: NO
Operational Impact: LOW
Recovery: n/a
Regression Risk: Low.
Evidence: the four code locations quoted.
Recommended Direction: Decide explicitly whether a manual move re-anchors the schedule,
  and separate that decision from the counter decision. No code was changed.
```

```text
ID: F-14
Title: The health-monitor restart path drops outputs without first suspending the
       control task or latching the system trip

Type: DESIGN RISK
Severity: LOW
Status: UNCONFIRMED
Evidence Level: STATIC
Confidence: MEDIUM

Location: MAYAP_INDUSTRIAL_v3_4_0.ino:238-244 (health restart branch) vs 205-230
          (TRIP branch); machine_control.h:3592-3648 (serviceHealthHeap)
Function: supervisorTask()
Relevant variables: healthRestartRequested_, gMayapSystemTripLatched

Trigger: serviceHealthHeap() requests a controlled restart.

Preconditions: free heap at or below the critical threshold, or below the serious
  threshold for three consecutive samples at a moment deemed safe.

Execution Path:
  1. The supervisor's TRIP branch calls mayapLatchSystemTrip() and vTaskSuspend() on
     the control task BEFORE mayapSafeOutputsEarly(), so the control task can never
     re-drive the pins.
  2. The health-restart branch calls mayapSafeOutputsEarly() and esp_restart() without
     either of those two steps.
  3. Both tasks are pinned to core 1 and the supervisor's priority (6) is higher than
     the control task's (5), so the control task cannot normally preempt. The only
     window is the blocking Serial write between the two calls.

Current Behavior: A very short window in which the control task could re-assert relay
  outputs between the safe-output write and the reset. Not demonstrable from static
  reading alone, hence UNCONFIRMED.

Expected Behavior: The two restart paths should use the same ordering.

Impact: At most a few milliseconds of relay re-assertion immediately before a reset.

Physical Safety Impact: NO
Operational Impact: LOW
Recovery: The reset follows immediately.
Regression Risk: None.
Evidence: the two branches quoted, plus the task priorities and core pinning in setup().
Recommended Direction: Latch the trip and suspend the control task in the health path
  as well. No code was changed.
```

```text
ID: F-15
Title: turnCountToday_ is not reset for a day boundary crossed during a power outage

Type: POTENTIAL BUG
Severity: LOW
Status: CONFIRMED
Evidence Level: STATIC
Confidence: MEDIUM

Location: machine_control.h:5650-5657 (updateBatchTime), 6417 (lastTurnCounterDay_),
          3462-3466 (begin() restores turnCountToday_)
Function: MachineController::updateBatchTime()
Relevant variables: lastTurnCounterDay_, turnCountToday_

Trigger: Power loss that spans a batch-day boundary.

Preconditions: a batch is resumed after the outage.

Execution Path:
  1. turnCountToday_ is restored from the batch record.
  2. lastTurnCounterDay_ is a RAM member and starts at 0 after a reboot.
  3. On the first updateBatchTime() after resume, `if (lastTurnCounterDay_ == 0U)
     lastTurnCounterDay_ = dayNumber;` adopts the NEW day without resetting
     turnCountToday_.

Current Behavior: The "turns today" counter shown to the operator carries over the
  previous day's total until the next day boundary.

Expected Behavior: Persist the day index alongside the counter, or reset the counter
  when the restored day differs from the stored one.

Impact: Display/statistics only. No control decision reads turnCountToday_.

Physical Safety Impact: NO
Operational Impact: LOW
Recovery: Self-corrects at the next day boundary.
Regression Risk: Low (one more field in PackedBatchV1 would change the batch schema).
Evidence: the three code locations quoted; all readers of turnCountToday_ were checked
  and none affects an actuator.
Recommended Direction: Persist the counter's day index. No code was changed.
```

---

## Q. UNKNOWNS

```text
U-01  No runtime verification of any kind
Why unknown:  No firmware was executed. Everything above is source-derived.
What is missing: ESP32-S3 hardware or a faithful simulator, the ESP32 Arduino core,
  the real SHT-485 sensor, DS3231, 24xx EEPROM and the relay panel. The audit host has
  g++ and PlatformIO but no ESP32 platform package and no target board.
Possible impact: Timing under real task contention, I2C bus behaviour with five tasks,
  relay switching behaviour and PID tuning quality are all unverified. Any of the PASS
  verdicts above could be contradicted by a real run.
Blocks release? It does not by itself create a blocker, but it caps every confidence
  rating in this report and forbids any claim that the happy path has been "tested".

U-02  Real behaviour of the three I2C/RS485 peripherals under fault
Why unknown:  The sensor's and EEPROM's own firmware behaviour (does the SHT-485 ever
  latch a constant value? does the 24xx ever ACK a write it did not complete?) is
  outside this repository.
What is missing: Device datasheets and bench fault-injection.
Possible impact: Determines how likely F-08 is in practice, and whether the EEPROM
  read-back verification can be fooled.
Blocks release? No, but it is the reason F-08's confidence is MEDIUM rather than HIGH.

U-03  Whether FaultManager's 32 slots can be exhausted by a path other than the one
      modelled
Why unknown:  The model in audit/sim/fault_slots.cpp reproduces the call order that was
  read from the source, but the audit did not prove that no additional faults_.set()
  call site exists on a rarely taken path.
What is missing: Nothing that cannot be settled by code reading; it was bounded by
  effort, not by access.
Possible impact: Only widens F-02; it cannot narrow it, because the 28 + 4
  unconditional codes already fill the table on their own.
Blocks release? No — F-02 is already CONFIRMED on the modelled path.
```

---

## R. MISSING FUNCTIONALITY

Only gaps with evidence of an existing behaviour plus a realistic scenario plus no
current handling:

* **MISSING FEATURE** — no terminal batch state (F-06). Existing behaviour: a batch has
  a configured length. Scenario: the length elapses at an unattended site. Handling
  today: a Warning fault only.
* **MISSING ALARM** — no frozen-sensor detection (F-08). Existing behaviour: the
  firmware validates transport and step plausibility. Scenario: a sensor that keeps
  answering with a constant value. Handling today: none.
* **MISSING RECOVERY** — none. Every fault examined has a defined path back to a valid
  state; F-04 is a missing *precondition*, not a missing recovery, and F-07 is a latch
  that is lost rather than one that cannot be cleared.
* **DESIGN IMPROVEMENT** (recorded, deliberately not raised as findings): there is no
  second temperature sensor to cross-check against; there is no dedicated E-stop input
  (the heater switch serves that role by design); the batch record has no field for
  turning-health state.

---

## S. RELEASE BLOCKERS

```text
RELEASE BLOCKER 1 — F-01 (CRITICAL)
  Unauthenticated remote control of a deployed machine over a public broker, in the
  binary produced by the project's own release workflow. Matches §34 "safety interlock
  bypass" (remote siren suppression) and "loss of Stop intent" (remote batch_stop
  destroys the batch record).

RELEASE BLOCKER 2 — F-02 (HIGH)
  A raised Stop-severity turn fault is erased from the alarm system within 30 seconds.
  Matches §34 "critical fault khong duoc phat hien".

RELEASE BLOCKER 3 — F-03 (HIGH)
  The declared dropHeatMaster interlock never operates during a batch; a credible
  two-failure path (SSR shorted + sensor lost) leaves the heater contactor closed with
  no automatic cut-off. Matches §34 "uncontrolled heating" and "safety interlock
  bypass".

RELEASE BLOCKER 4 — F-04 (HIGH)
  A new batch can be started into a state where turning is permanently inhibited.
  Matches §34 "recovery lam may chay sai sequence" and is a HIGH bug with direct
  operational-reliability impact.

UNRESOLVED RELEASE RISK — U-01
  No runtime verification was possible. Per §5 and §37.S, this report must not be read
  as evidence that no further blocker exists.
```

Because runtime verification was not performed, this report states four *confirmed*
release blockers and does not assert that no others exist.

---

## FINAL RELEASE GATE (§38)

```text
CAN RUN HAPPY PATH?
YES
Evidence: STATIC full trace + SIMULATED timing model (audit/sim/timing_model.cpp,
  14/14 pass). Not runtime-verified.

CAN START ONLY FROM VALID STATE?
NO
Evidence: startBatch()'s 19-gate list is otherwise complete, but it omits
  turnMechanicalCheckRequired_ / faults_.turningInhibited() (F-04).

CAN STOP FROM ALL IMPORTANT STATES?
YES
Evidence: Stop injected at Prestart, Homing, heating, turning dead-time, turning
  mid-move, alarm active, resume-wait, config transaction and checkpoint; traced to the
  final physical-output request in every case (§D).

DO SAFETY FAULTS REMOVE DANGEROUS OUTPUTS?
PARTIAL
Evidence: The SSR is inhibited correctly for every fault that declares it. The master
  contactor is only dropped for EmergencyTemperature; the dropHeatMaster flag is inert
  for the other eight codes that declare it (F-03).

CAN DETECT MAJOR SENSOR FAULTS?
PARTIAL
Evidence: loss, invalid, out-of-range, implausible downward step, flapping and both
  limit-switch failure modes are all detected (§G). A sensor frozen at a plausible
  value is not (F-08).

CAN DETECT MAJOR ACTUATOR/OUTPUT FAULTS?
PARTIAL
Evidence: turn timeout, limit-not-released, limit conflict, output conflict, relay-rate
  and heater-not-heating all exist. There is no feedback contact on the heater
  contactor, so "contactor commanded open but still closed" is undetectable by design.

CAN RECOVER FROM MAJOR FAULTS?
PARTIAL
Evidence: All traced fault/recovery pairs return to a valid state with timers, flags,
  position and schedule re-derived (§I). Degraded by F-04 (recovery into a batch that
  can never turn) and F-07 (a maintenance latch lost on reboot).

CAN GET STUCK?
NO
Evidence: No blocking wait exists in the control loop; every gated state
  (batchClearPending_, resume-wait, turn fault, E205) has an operator-reachable exit,
  and the watchdog plus supervisor heartbeat/deadline trip cover a hung control task.

CAN ENTER INVALID STATE?
YES
Evidence: Not through the state machine itself — the impossible combinations listed in
  §28 (batchRunning_ && resumePending_, turnLeft && turnRight, SSR ON with master OFF,
  moving while disabled, start while clear-pending) are each prevented structurally,
  and the arbiter re-checks the direction interlock independently of the state machine.
  However, FaultManager's shared overflow slot produces a genuinely invalid internal
  state: one FaultState whose `code` and whose `active`/`occurrences` belong to two
  different faults (F-02).

CAN SURVIVE POWER LOSS CONSISTENTLY?
YES
Evidence: 16 injection points, all ending in a defined safe state
  (04_POWER_LOSS_MATRIX.md §4.2).

CAN RESTORE BATCH STATE RELIABLY?
YES
Evidence: A/B double buffering with magic, schema, size, CRC32, wrapping sequence
  selection and mandatory read-back verification; a torn write can only damage the
  non-current slot (04_POWER_LOSS_MATRIX.md §4.1). This resolves the prior audit's
  open UNKNOWN.

CAN START A CLEAN NEXT BATCH?
PARTIAL
Evidence: stopBatch()/startBatch() clear all control-relevant carry-over and the start
  is blocked until the EEPROM erase is verified; but turnMechanicalCheckRequired_
  survives into the next batch without blocking it (F-04).

ARE HMI/WEB/RUNTIME PERMISSIONS CONSISTENT?
NO
Evidence: totalIncubationDays is locked on the HMI and in firmware but editable on the
  web form (F-10); the Offline→Online connectivity lock exists in firmware only.

ARE OLD HIGH/CRITICAL FINDINGS RESOLVED?
YES
Evidence: prior F-01 and F-02 (the only HIGH items) are both fixed, and the prior
  UNKNOWN F-08 is resolved in the firmware's favour (05_REGRESSION.md). Two new
  findings (F-04, F-07) were introduced by the F-02 remediation.

MISSING CRITICAL FUNCTIONALITY?
YES
Evidence: no terminal batch state (F-06) and no frozen-sensor detection (F-08); both
  have evidence of an existing behaviour, a realistic scenario and no current handling.

RUNTIME VERIFIED?
NO
```

---

## RELEASE DECISION (§39)

```text
NEEDS FIX BEFORE RELEASE

RELEASE BLOCKERS: 4
CRITICAL: 1
HIGH: 3
MEDIUM: 5
LOW: 6
UNKNOWN: 3
```

1. The binary produced by the project's own release workflow accepts unauthenticated
   control commands from the public internet (F-01) — this alone bars release.
2. The fault table holds 32 slots for 36 codes, and the four turn-fault codes are the
   ones that lose; a raised turn fault disappears from the alarm system within 30 s,
   proven by an executed model of the real allocation order (F-02).
3. The `dropHeatMaster` interlock declared for eight fault codes cannot fire during a
   batch, leaving "SSR shorted + sensor lost" with no automatic cut-off (F-03).
4. A new batch can be started while the mechanical-check lockout inhibits turning,
   producing a full batch with zero turns and no further alarm (F-04).
5. Everything the previous audit raised has been genuinely fixed, including the
   persistence UNKNOWN, which this pass verified to be sound.
6. Power-loss handling, stop-intent integrity, the output arbiter and the sensor
   validation ladder are of good quality and are not the reason for this verdict.
7. Nothing was executed on the target, so this verdict is based on source evidence plus
   two executed logic models only; further blockers cannot be ruled out.
8. The overall engineering standard of this firmware is high; a good score does not
   override a confirmed blocker, and four are confirmed.
