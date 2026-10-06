# 00 — AUDIT STATUS LOG

```text
Current phase:  COMPLETE (Phase 1 .. Phase 23 all executed)
Target:         MAYAP INDUSTRIAL v3.7.0, branch main, commit f3ec7cb
Runtime:        RUNTIME VERIFICATION NOT AVAILABLE
Production code modified: NO   Commits: NO   Pushes: NO
```

## Phase completion

| Phase | Subject | Status | Output |
|---|---|---|---|
| 1 | Repository discovery | done | 01_SYSTEM_MAP.md |
| 2 | Reconstruct machine behaviour | done | 02_MACHINE_FLOW.md |
| 3 | Start conditions (20 cases) | done | report §D |
| 4 | Happy path | done | report §C |
| 5 | Sensor fault injection | done | report §G, 03_FAULT_MATRIX.md |
| 6 | Actuator fault injection | done | report §E, §F |
| 7 | Turning deep audit | done | report §F, findings F-02/F-04/F-07/F-11/F-13 |
| 8 | Heating deep audit | done | report §E, finding F-03 |
| 9 | Timer / timeout audit | done | sim/timing_model.cpp (14/14 pass) |
| 10 | Stop / emergency | done | report §D |
| 11 | Alarm system | done | 03_FAULT_MATRIX.md, finding F-02 |
| 12 | Recovery | done | report §I |
| 13 | Multi-fault batch | done | report §J |
| 14 | Configuration during operation | done | report §N, finding F-10 |
| 15 | Persistent storage deep audit | done | 04_POWER_LOSS_MATRIX.md §4.1 |
| 16 | Power-loss matrix (16 points) | done | 04_POWER_LOSS_MATRIX.md §4.2 |
| 17 | Batch boundary | done | report §M, finding F-06 |
| 18 | Command collision | done | report §N |
| 19 | Invalid state / impossible combinations | done | report §38 |
| 20 | Old-findings regression | done | 05_REGRESSION.md |
| 21 | Missing functionality | done | report §R |
| 22 | Adversarial second review | done | see below |
| 23 | Finding consistency check | done | see below |

## Coverage

```text
Files reviewed (control-relevant):
  MAYAP_INDUSTRIAL_v3_4_0.ino      (331 lines, full)
  machine_control.h                (6562 lines, full for all control paths)
  config.h                         (1211 lines, all constants/schema referenced)
  hmi.h                            (command queue, config transactions, batch locks)
  transaction_bridge.h                  (command/config/session message handling, MQTT
                                    connection and credentials)
  cloud_alert_link.h               (fault/runtime consumption only)
  network_service.h, ota_web_update.h, ota_rollback.h (command entry points only)
  app.js                           (command + config submission paths only)
  cloudflare/src/index.js          (route list only - no control commands exist)
  .github/workflows/build-firmware.yml, config.js, README.md (release pipeline)

Functions reviewed in depth: ~70, including
  setup/controlTask/supervisorTask, MachineController::begin/update,
  startBatch/stopBatch/startAutoTune, processResume/adjustResumeElapsedFromRtc,
  processHmiTransactions, processSensor, updateAlarms, updateAutoTune,
  updateTurning/updateManualTurning/requestTurn/beginPhysicalMove/completeTurn/
  finishHoming/stopTurn/latchTurnFault/clearTurnFault, updateHeatingAndOutputs,
  ssrWindowOn, updateBatchTime, checkpointBatch/saveBatchRecord/clearBatchRecord/
  tryCommitBatchClear/serviceBatchClear, latchStorageFault, serviceHealth*,
  serviceI2cDeviceRecovery, enterTestMode/updateTestMode/updateTestModeOutputs,
  FaultManager::set/acknowledge/clearRecovered/slot/clearState,
  OutputArbiter::update/updateTurnOutputs/setImmediate/setMinOn/setMinSwitch,
  ThermalController, RelayAutoTune, PersistentStore::save*/load*/refresh*Cache,
  ExternalEeprom24xx::read/writeBytes, SafetyJournal, PowerManager,
  handleCommandMessage/handleConfigSetMessage/subscribeAll/connectMqtt.

Execution paths verified: 20 start-condition cases, 16 power-loss injection points,
  13 sensor-fault cases, 9 actuator/output cases, 18 turning cases, 13 heating
  injection cases, 9 stop/emergency injection points, 9 command-collision pairs,
  8 impossible-combination checks, 9 regression items.

Fault scenarios exercised: all 36 FaultCode values mapped to trigger, output action
  and clear path (03_FAULT_MATRIX.md).

Executed models (SIMULATED, in audit/sim/, not production code):
  timing_model.cpp  — millis() wrap-around, timeReached boundaries, SSR window
                      min-on/min-off and rollover, turn-schedule epoch arithmetic,
                      elapsed-time overflow clamp, temperature sanitiser ordering.
                      Result: 14/14 PASS.
  fault_slots.cpp   — FaultManager slot allocation with the real per-cycle call
                      order. Result: 13/13 PASS, confirming finding F-02.
```

## Phase 22 — adversarial second review

Each CRITICAL/HIGH finding was attacked with the aim of rejecting it:

* **F-01** — attempted rejection: "the default is Offline, so nobody is exposed", and
  "the owner probably builds with private build_flags". Rejected as a rejection: the
  README documents Online mode as a product feature, the release workflow contains no
  build properties, and the Cloudflare OTA path distributes exactly that artefact.
  Finding stands.
* **F-02** — attempted rejection: "turnFaultLatched_ still blocks turning, so nothing
  unsafe happens". Accepted in part — the severity was reduced from the initially
  considered CRITICAL to HIGH and Physical Safety Impact set to NO. The alarm loss
  itself could not be rejected; it was instead reproduced in an executed model.
* **F-03** — attempted rejection: "the comment says this is a deliberate installer
  request, so it is by design". Rejected as a rejection: a deliberate change does not
  make the contradicting descriptor table correct, and the two-failure path (SSR
  shorted + sensor lost) has no compensating control anywhere in the repository.
  Severity was held at HIGH rather than CRITICAL because it requires two failures.
* **F-04** — attempted rejection: "E205 is displayed, so the operator will notice".
  Rejected: the start is not blocked, and because turning never runs, no further turn
  fault is generated to re-alert. Finding stands.
* **U-01** — re-examined whether any runtime verification was achievable within the
  environment. It is not (no ESP32 core, no board, no simulator); the two host models
  are explicitly labelled SIMULATED and are not claimed as runtime evidence.

## Phase 23 — consistency check

```text
Duplicate IDs:                 none
Duplicate findings:            none (F-09 overlaps F-01 in exposure but is a distinct
                               defect that survives F-01's fix; stated in the finding)
One Severity per finding:      yes
One Type per finding:          yes
One Status per finding:        yes
Hybrid severities/statuses:    none ("MEDIUM-HIGH", "CONFIRMED/UNKNOWN" not used)
Evidence levels used:          STATIC, SIMULATED only (RUNTIME never claimed)
Confidence values:             HIGH / MEDIUM only, independent of severity
Counts match:  15 findings = 1 CRITICAL + 3 HIGH + 5 MEDIUM + 6 LOW
               4 release blockers = F-01, F-02, F-03, F-04
               3 UNKNOWN entries = U-01, U-02, U-03
Top risks match findings:      yes
Executive summary matches:     yes
Final verdict matches:         yes (NEEDS FIX BEFORE RELEASE, driven by the 4 blockers)
```

## Remaining high-risk areas (not closed by this audit)

1. Everything covered by U-01 — no code was executed on the target.
2. Peripheral behaviour under fault (U-02), which bounds the likelihood of F-08.
3. PID tuning quality and thermal dynamics, which cannot be assessed without hardware.
4. The MQTT broker/credential model as actually deployed by the owner, which decides
   the real-world exposure of F-01.
