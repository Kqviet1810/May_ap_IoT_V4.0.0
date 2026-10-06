# 01 — CONTROL SYSTEM MAP

Firmware: MAYAP INDUSTRIAL v3.7.0 · Branch `main` · Commit `f3ec7cb`
Audit date: 2026-09-16 · Evidence for this file: **STATIC** (read from source only)

```text
CONTROL SYSTEM MAP

Entry:
  MAYAP_INDUSTRIAL_v3_4_0.ino :: setup()
    mayapSafeOutputsEarly() -> Serial -> I2C mutex -> Wire.begin ->
    network/OTA/rollback/weblink/cloudalert begin -> esp_task_wdt_reconfigure ->
    Machine.begin() -> hmiBegin() -> 5 static FreeRTOS tasks.

Main control loop:
  controlTask (core 1, prio 5, CONTROL_TASK_PERIOD_MS, WDT-subscribed)
    -> Machine.update(now)  [MachineController::update, machine_control.h:3520]
  Other tasks: hmiTask (core 0, prio 2), networkTask (core 0, prio 1, MQTT+cloud),
  otaTask (core 0, prio 1, OTA/web-update/rollback), supervisorTask (core 1, prio 6,
  WDT-subscribed, heartbeat + cycle-deadline TRIP + health restart executor).
  loop() does nothing but vTaskDelay.

Machine controller:
  Mayap::MachineController (machine_control.h:3382-6560), single static instance
  `Machine`. Update order per cycle:
    power_.service -> inputs_.update -> processInputEvents -> serviceSerial ->
    processNetworkState -> sensor_.update -> processSensor -> rtc_.update ->
    serviceI2cDeviceRecovery -> serviceBatchClear -> eventLog_.syncClock ->
    RtcFailure fault -> adjustResumeElapsedFromRtc -> processInputModeTransition ->
    processHmiTransactions -> updateTestMode -> processResume -> updateAlarms ->
    updateAutoTune -> updateTurning -> updateHeatingAndOutputs ->
    processOutputEvents -> syncOutputFaults -> updateBatchTime -> serviceBatchLog ->
    serviceHealthMonitor -> updateLed -> copyRuntimeToHmi (gated) ->
    checkpointBatch (gated) -> printStatus (gated).

State machine:
  Two orthogonal machines, no single enum:
   * Batch lifecycle: BatchPhase { Stopped, Prestart, Homing, Running }
     (machine_control.h:3570) + booleans batchRunning_, resumePending_,
     resumeConfirmationRequired_, batchClearPending_, testModeActive_.
   * Turning: TurnPhase { Idle, DeadtimeLeft, DeadtimeRight, MovingLeft,
     MovingRight, Fault } + TrayPosition { Unknown, Left, Right }.
  Auxiliary: ResumeBlockReason (14 values) explains why resume is held.

Sensor:
  SHT485Industrial (RS485/Modbus-like, machine_control.h:2494) ->
  MachineController::processSensor() (3936). Two tiers: rawTemperature_/
  safetySampleValid_ for the protection path, filtered temperature_/sensorUsable_
  (plausibility + SENSOR_RECOVERY_GOOD_SAMPLES streak) for the PID path.
  RtcDs3231 (I2C) supplies epoch; InputManager debounces 8 digital inputs.

Actuator:
  OutputArbiter (2965) is the sole writer of the 9 physical pins. Applies
  min-on / min-switch / master pickup + drop delay / turn direction interlock /
  forceSafe on system trip.

Heating:
  updateHeatingAndOutputs (5303) -> ThermalController (PID, anti-windup) ->
  ssrWindowOn() time-proportioning -> req.heaterSsr + req.heatMaster ->
  OutputArbiter. RelayAutoTune (3261) is the alternative power source.

Turning:
  updateTurning (5024) / updateManualTurning (5133) / requestTurn /
  beginPhysicalMove / completeTurn / finishHoming / stopTurn / latchTurnFault /
  clearTurnFault (5169-5301). Lockdown last TURN_LOCKDOWN_DAYS=3 days.

Fault:
  FaultCode enum (369) + faultDescriptor() table (440) + FaultManager (535).
  Per-fault flags: latching, inhibitSsr, dropHeatMaster, inhibitsTurning,
  forceVentFan, forceCirculationFan, alarmBit, severity, displayPriority.

Alarm:
  updateAlarms (4626) computes conditions; FaultManager holds state and exposes
  ssrInhibited() / masterDropRequired() / turningInhibited() / ventForced() /
  circulationForced() / alarmMask(). Siren = physical relay, driven only by
  emergencyActive_ && !sirenMutedUntil_.

Persistence:
  ExternalEeprom24xx (I2C 24xx) + PersistentStore (1815). Three A/B double-buffered
  record families: Config, Batch, Reminders. Each record = magic + schema + size +
  payload + CRC32; write goes to the *other* slot, then is read back and verified.
  Loader picks the valid record with the newer wrapping sequence; legacy config
  schemas V3..V7 migrate forward.
  SafetyJournal = internal NVS (Preferences): stop-intent tombstone + reset counter.

Power-loss recovery:
  PowerManager::begin (esp_reset_reason + reset-storm counter) ->
  MachineController::begin() decision tree (stop-intent vs wasRunning) ->
  resumePending_ / batchClearPending_ -> processResume() (4505) gate list ->
  adjustResumeElapsedFromRtc() (4443) recomputes elapsed from checkpointEpoch.

HMI:
  hmi.h (4665 lines) — local screen + buttons. Commands via queueCommand() into a
  shared ring drained by processHmiTransactions(); config via startConfigSave()/
  hmiTakeSavedConfig()/hmiConfirmConfigSave(). All shared state under
  portENTER_CRITICAL(&hmiApiMux).

Web:
  index.html / app.js (GitHub Pages PWA) -> MQTT topics
  `<root>/<deviceId>/{command,config/set,reminders/set,session}`.

Realtime:
  transaction_bridge.h — PubSubClient on networkTask. handleCommandMessage() maps 8
  string actions to HmiCommandType and reuses the *same* HMI command queue.
  handleConfigSetMessage() merges a partial JSON config onto the last known config
  and reuses startConfigSave().

Configuration:
  MachineConfig (config.h:927) + sanitizeMachineConfig() (machine_control.h:1216),
  applied on every load and every save, from every source.

Safety-related code:
  mayapSafeOutputsEarly(), mayapLatchSystemTrip()/mayapSystemTripLatched(),
  supervisorTask TRIP path, OutputArbiter forceSafe/interlocks, FaultManager
  inhibit flags, SafetyJournal stop-intent, startBatch()/processResume() gate lists.
```

## Out of scope (per spec §2)
CSS/layout, icons, PWA service worker, push.js presentation, Cloudflare D1 schema,
QR label generator, manual/PDF artifacts. `cloudflare/src/index.js` was checked only
for command-capable endpoints (it serves push registration + firmware metadata; it
does not publish MQTT control commands).
