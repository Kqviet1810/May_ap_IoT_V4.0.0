const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const fw = (name) => fs.readFileSync(path.resolve(__dirname, '..', 'MAYAP_INDUSTRIAL_v1_0_0', name), 'utf8').replace(/\r\n/g, '\n');
const bridge = fw('transaction_bridge.h');
const machine = fw('machine_control.h');
const access = fw('tech_access.h');
const history = fw('advanced_history.h');
const hmi = fw('hmi.h');

function body(source, signature) {
  const start = source.indexOf(signature);
  assert.notEqual(start, -1, `${signature} is missing`);
  const open = source.indexOf('{', start);
  let depth = 0;
  for (let i = open; i < source.length; i++) {
    if (source[i] === '{') depth++;
    if (source[i] === '}' && --depth === 0) return source.slice(open + 1, i);
  }
  throw new Error(`Unclosed ${signature}`);
}

test('MQTT bypass: config/set can never change a technical field (checked before any save is started)', () => {
  const set = body(bridge, 'inline void handleConfigSetMessage(');
  const refusal = set.indexOf('TECH_VIA_HMI_APPROVAL');
  assert.ok(refusal > 0);
  assert.match(set.slice(0, refusal), /AdvancedHistory::differingMask\(AdvancedHistory::fromConfig\(known\), AdvancedHistory::fromConfig\(candidate\)\) != 0U/);
  assert.ok(refusal < set.indexOf('startConfigSave('), 'refusal must come before the save transaction is registered');
  // and the controller refuses it again on its own (defence in depth: a stale HMI copy or a future caller)
  assert.match(body(machine, 'void processHmiTransactions('), /techLocked = advancedChange && !techGate_\.hmiUnlocked\(now\)/);
  assert.match(body(machine, 'void processHmiTransactions('), /saveAllowed = !safetySaveBlocked && !techLocked/);
});

test('MQTT bypass: a remote autotune_start / PIN set / permission / decision / restore never executes', () => {
  const mapper = body(bridge, 'inline HmiCommandType mapCommandAction(');
  for (const hmiOnly of ['tech_pin_set', 'tech_web_set', 'tech_decision', 'advanced_restore', 'tech_restore'])
    assert.doesNotMatch(mapper, new RegExp(hmiOnly), `${hmiOnly} must not be reachable from the Web`);
  const process = body(machine, 'void processHmiTransactions(');
  assert.match(process, /case HmiCommandType::AutoTuneStart:\s*\/\/[^\n]*\n\s*if \(command\.source == HmiCommandSource::Remote\) \{ message = "TECH_VIA_HMI_APPROVAL"; break; \}/);
  const tech = body(machine, 'bool handleTechCommand(');
  for (const kind of ['TechPinSet', 'TechWebSet', 'TechDecision', 'AdvancedRestore'])
    assert.match(tech.slice(tech.indexOf(`case HmiCommandType::${kind}:`), tech.indexOf(`case HmiCommandType::${kind}:`) + 260), /if \(remote\)/, kind);
  assert.match(tech, /case HmiCommandType::TechRequestSubmit:\s*\n\s*if \(!remote\)/);
});

test('Web request is only parked; every safety condition is re-checked when the HMI says yes', () => {
  const submit = body(machine, 'bool submitTechRequest(');
  assert.doesNotMatch(submit, /saveConfig|applyAdvancedSnapshot|startAutoTune/);
  const execute = body(machine, 'bool executeTechPending(');
  assert.match(execute, /RequestTtlMs|timeReached\(now, request\.deadline\)/);
  assert.match(execute, /webAllowed\(\) \|\| !techGate_\.webSessionActive\(now\)/);
  const apply = body(machine, 'bool applyAdvancedSnapshot(');
  assert.ok(apply.indexOf('techEditBlocked(message)') < apply.indexOf('store_.saveConfig'));
  assert.ok(apply.indexOf('pushHistory(current)') < apply.indexOf('store_.saveConfig'), 'undo record is written before the new values');
  assert.match(apply, /mayapPidHasAuthority\(candidate\)/);
  assert.match(body(machine, 'bool techEditBlocked('), /batchRunning_ \|\| resumePending_/);
  assert.match(access, /WebSessionMs = 600000/);
  assert.match(fw('tech_request.h'), /RequestTtlMs = 60000UL/);
});

test('PIN is never logged, never put in a snapshot, and no PIN is hard-coded', () => {
  for (const [name, source] of [['tech_access.h', access], ['advanced_history.h', history]]) {
    assert.doesNotMatch(source, /Serial|mayapSerialPrintf|printf\s*\(\s*"[^"]*(pin|PIN)/, `${name} must not print`);
  }
  const techRegion = machine.slice(machine.indexOf('Technical access (PIN, Web permission'), machine.indexOf('// Read-only PID monitor.'));
  assert.ok(techRegion.length > 1000);
  assert.doesNotMatch(techRegion, /mayapSerialPrintf|Serial\./);
  // the HMI keeps digits only in a 4-byte array that is wiped after queueing
  assert.doesNotMatch(hmi, /mayapSerialPrintf\([^)]*pin(Digits|First|Cur)/i);
  const snap = body(bridge, 'inline bool publishSnapshot(');
  const techBlock = snap.slice(snap.indexOf('JsonObject tech'), snap.indexOf('autoTunePhase'));
  assert.doesNotMatch(techBlock, /hash|salt|pinDigits|\["pin"\]|history/i);
  assert.doesNotMatch(machine + hmi + access, /(?:verify|compare|equals?)[^;\n]*"[0-9]{4}"/i);
});

test('old-record snapshot excludes PIN, Web permission, batch data and independent safety thresholds', () => {
  const snapshot = history.slice(history.indexOf('struct Snapshot {'), history.indexOf('};', history.indexOf('struct Snapshot {')));
  const members = [...snapshot.matchAll(/\b(?:float|uint8_t|uint16_t)\s+([^;]+);/g)].flatMap((m) => m[1].split(',').map((n) => n.split('=')[0].trim()));
  assert.deepEqual(members.sort(), ['adaptiveThermalBalanceEnabled', 'autotuneBandC', 'autotuneRelayPowerPercent', 'heaterStuckDurationSec', 'heaterStuckMinRiseC',
    'humidityOffset', 'kd', 'ki', 'kp', 'maxHeaterPower', 'pidCycleSec', 'tempOffset', 'tempOscillationCrossLimit', 'tempOscillationWindowSec',
    'tempRateLimitC', 'tempRateWindowSec'].sort());
  for (const forbidden of ['highTempAlarm', 'emergencyTemp', 'lowTempAlarm', 'sensorTimeout', 'pin', 'webAllowed', 'batch'])
    assert.doesNotMatch(snapshot, new RegExp(forbidden, 'i'));
  // the Web can neither read nor trigger old records
  assert.doesNotMatch(bridge, /history\[|historyCache|AdvancedRestore|readAdvHistory/);
  assert.match(fw('config.h'), /AdvancedRestore\s+\/\/ Local only/);
});

test('technical EEPROM records sit in spare space and never overlap config A/B, batch or the temperature history', () => {
  const addr = (name) => Number.parseInt(new RegExp(`${name} = (0x[0-9A-Fa-f]+)U`).exec(machine)[1], 16);
  assert.equal(addr('EEPROM_ADDR_TECH_AUTH_A'), 0x3000);
  assert.ok(addr('EEPROM_ADDR_TECH_AUTH_B') >= 0x3000 + 0x80 - 0x80);
  assert.ok(addr('EEPROM_ADDR_ADV_HISTORY') > addr('EEPROM_ADDR_TECH_AUTH_B'));
  assert.match(machine, /EEPROM_ADDR_TECH_AUTH_A >= EEPROM_ADDR_TEMP_HISTORY \+ TEMP_HISTORY_STORAGE_BYTES/);
  assert.match(machine, /EEPROM_ADDR_ADV_HISTORY \+ AdvancedHistory::Slots \* EEPROM_ADV_HISTORY_SLOT_BYTES <= EEPROM_CAPACITY_BYTES/);
});

test('HMI/Web/EEPROM work stays off the thermal loop', () => {
  // The technical service is O(1) per cycle: no EEPROM read in serviceTech outside the 3 s error-retry path.
  const service = body(machine, 'void serviceTech(');
  const reads = [...service.matchAll(/history_\.|refreshHistoryCache|retryLoad/g)].length;
  assert.ok(reads <= 4);
  assert.match(service, /techGate_\.storageError\(\) && timeReached\(now, techRetryAt_\)/);
  assert.doesNotMatch(body(hmi, 'void serviceAdvanced('), /saveConfig|delay\(|Wire\./);
  assert.doesNotMatch(body(hmi, 'void infoItem('), /new |malloc|String/);
});
