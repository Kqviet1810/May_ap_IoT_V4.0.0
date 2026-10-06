const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { test } = require('node:test');
const { TransactionLedger, PacketPolicy } = require('../protocol_v2.js');

const ack = (id, operation, phase, ok, code) =>
  ({ v: 2, requestId: id, operation, phase, ok, code });

test('received and broker publish are never success; applied is terminal', () => {
  let clock = 0;
  const tx = new TransactionLedger(() => ++clock);
  tx.create('a', 'batch.start');
  assert.equal(tx.published('a').phase, 'PUBLISHED');
  assert.equal(tx.ack('a', ack('a', 'batch.start', 'received', false, 'RECEIVED')), 'RECEIVED');
  assert.equal(tx.ack('a', ack('a', 'batch.start', 'completed', true, 'APPLIED')), 'APPLIED');
  assert.equal(tx.ack('a', ack('a', 'batch.start', 'received', false, 'RECEIVED')), 'IGNORED');
});

test('rejection retains stable code and never becomes success', () => {
  const tx = new TransactionLedger(() => 1);
  tx.create('a', 'batch.start');
  assert.equal(tx.ack('a', ack('a', 'batch.start', 'completed', false,
    'BATCH_HEATER_SWITCH_OFF')), 'REJECTED');
  assert.equal(tx.entries.get('a').code, 'BATCH_HEATER_SWITCH_OFF');
});

test('lost received, lost terminal, late terminal, reconnect and reboot', () => {
  const tx = new TransactionLedger(() => 1);
  tx.create('a', 'light.toggle');
  tx.published('a');
  assert.equal(tx.ack('a', ack('a', 'light.toggle', 'completed', true, 'APPLIED')), 'APPLIED');
  tx.create('b', 'config.save');
  tx.published('b');
  assert.equal(tx.timeout('b'), 'UNCERTAIN');
  assert.equal(tx.ack('b', ack('b', 'config.save', 'completed', true, 'APPLIED')), 'APPLIED');
  tx.create('c', 'batch.stop');
  tx.published('c');
  // A reconnect or ESP reboot produces no terminal evidence by itself.
  assert.equal(tx.timeout('c'), 'UNCERTAIN');
});

test('out-of-order, mismatched operation, invalid phase, duplicate results', () => {
  const tx = new TransactionLedger(() => 1);
  tx.create('a', 'alarm.ack');
  assert.equal(tx.ack('a', ack('a', 'batch.start', 'completed', true, 'APPLIED')), 'PROTOCOL_ERROR');
  assert.equal(tx.ack('a', ack('a', 'alarm.ack', 'queued', true, 'APPLIED')), 'PROTOCOL_ERROR');
  assert.equal(tx.ack('a', ack('a', 'alarm.ack', 'completed', false, 'REJECTED')), 'REJECTED');
  assert.equal(tx.ack('a', ack('a', 'alarm.ack', 'completed', true, 'APPLIED')), 'IGNORED');
});

test('two browsers have distinct transactions; invalid auth cannot be applied', () => {
  const first = new TransactionLedger(() => 1);
  const second = new TransactionLedger(() => 1);
  first.create('web1-a', 'batch.start');
  second.create('web2-a', 'batch.start');
  assert.equal(first.ack('web2-a', ack('web2-a', 'batch.start', 'completed', true, 'APPLIED')), 'IGNORED');
  assert.equal(first.ack('web1-a', ack('web1-a', 'batch.start', 'completed', false, 'AUTH_ERROR')), 'REJECTED');
  assert.equal(second.timeout('web2-a'), 'UNCERTAIN');
});

test('FRAME packet worst cases stay within budgets', () => {
  const topic = (suffix) => `mayap/v1/MAP-1234567890AB/${suffix}`;
  const grant = `w-${'a'.repeat(16)}|1780000000|${'b'.repeat(24)}`;
  const base = { v: 2, requestId: `cmd-${'a'.repeat(20)}`,
    clientId: `w-${'a'.repeat(16)}`, seq: 1780000000000, nonce: 'c'.repeat(16) };
  const wireBytes = (suffix, body) => Buffer.byteLength(JSON.stringify({ v: 2,
    grant, grantSig: 'a'.repeat(64), body: JSON.stringify(body), sig: 'b'.repeat(64) })) +
    Buffer.byteLength(JSON.stringify({v:1,channel:suffix,payload:null})) - 4;
  const command = { ...base, bootId: 4294967295, expiresAt: 1780000000,
    action: 'batch_overdue_continue' };
  assert.ok(wireBytes('command', command) < PacketPolicy.SMALL_TARGET);
  const advanced = {
    kp: 100, ki: 20, kd: 200, pidCycleSec: 60, maxHeaterPower: 100, adaptiveThermalBalanceEnabled: true,
    tempRateLimitC: 10, tempRateWindowSec: 1800, tempOscillationCrossLimit: 30,
    tempOscillationWindowSec: 3600, heaterStuckMinRiseC: 5,
    heaterStuckDurationSec: 3600,
    autotuneRelayPowerPercent: 80, autotuneBandC: 1,
  };
  const webSource = readFileSync(require.resolve('../app.js'), 'utf8');
  const advancedSource = webSource.split("} else if (group === 'advanced') {")[1]
    ?.split('    return config;')[0];
  assert.ok(advancedSource, 'advanced form source not found');
  const advancedFields = [...advancedSource.matchAll(/config\.([A-Za-z0-9]+)\s*=/g)]
    .map((match) => match[1]);
  // The reserved field still fits the legacy packet but is no longer editable.
  assert.deepEqual([...new Set(advancedFields)].sort(), Object.keys(advanced).filter(key => key !== 'pidCycleSec').sort());
  assert.ok(webSource.includes("'pidCycleSec'"), 'legacy full-config readback remains supported');
  const config = { ...base, requestId: `cfg-${'a'.repeat(20)}`,
    revision: 4294967295, bootId: 4294967295, config: advanced };
  assert.ok(wireBytes('config/set', config) < PacketPolicy.CHUNK_TARGET);
  const vent = { ventOnTemp: 39, ventOffTemp: 38, ventAutoEnabled: true,
    ventProfileLevel: 2, ventCycleMinutes: 120, ventScheduleEnabled: false,
    ventDutyDay1To3: 10, ventDutyDay4To7: 15, ventDutyDay8To11: 25,
    ventDutyDay12To15: 35, ventDutyDay16To18: 50, ventDutyDay19To21: 70 };
  assert.ok(wireBytes('config/set', { ...config, config: vent }) < PacketPolicy.CHUNK_TARGET);
  const report = { v: 2, bootId: 4294967295, revision: 4294967295,
    part: 99, done: true, config: { field: 'x'.repeat(700) } };
  assert.ok(Buffer.byteLength(JSON.stringify(report)) + Buffer.byteLength(JSON.stringify({v:1,channel:'config/reported',payload:null})) - 4 < PacketPolicy.CHUNK_TARGET);
  const history = { v: 2, bootId: 4294967295, requestId: `hist-${'a'.repeat(20)}`,
    windowMin: 1440, intervalSec: 300, cursor: 288, done: true,
    samples: Array.from({ length: 12 }, (_, i) => [4294967295 - i * 300, -20.0]) };
  assert.ok(Buffer.byteLength(JSON.stringify(history)) + Buffer.byteLength(JSON.stringify({v:1,channel:'history/reported',payload:null})) - 4 < PacketPolicy.CHUNK_TARGET);
  const snapshot = { bootId: 4294967295, revision: 4294967295, runtime: {
    temperature: 100, humidity: 100, machineState: 4294967295,
    batchRunning: true, currentDay: 200, heaterOn: true, heaterPower: 100,
    circulationFanOn: true, ventFanOn: true, humidifierOn: true,
    lightOn: true, sirenOn: true, turnState: 255, nextTurnMinutes: 720,
    autoTuneState: 255, autoTuneProgress: 100, resumeConfirmationRequired: true,
    batchOverdueConfirmationPending: true,
    activeFaults: Array.from({ length: 12 }, () => ({ code: 65535, severity: 255 })) } };
  assert.ok(Buffer.byteLength(JSON.stringify(snapshot)) + Buffer.byteLength(JSON.stringify({v:1,channel:'snapshot',payload:null})) - 4 < PacketPolicy.CHUNK_TARGET);
  const response = { v: 2, requestId: `cmd-${'a'.repeat(20)}`,
    operation: 'batch.overdue.continue', phase: 'completed', ok: false,
    code: 'BATCH_HEATER_SWITCH_OFF', bootId: 4294967295,
    result: 'rejected', message: 'Hãy bật công tắc thanh nhiệt trước',
    revision: 4294967295, tDeviceReceived: 4294967295,
    tDeviceCompleted: 4294967295, sig: 'a'.repeat(64) };
  assert.ok(Buffer.byteLength(JSON.stringify(response)) + Buffer.byteLength(JSON.stringify({v:1,channel:'ack',payload:null})) - 4 < PacketPolicy.SMALL_TARGET);
});

test('all eight config forms use patches within the shared packet policy', () => {
  const source = readFileSync(require.resolve('../app.js'), 'utf8');
  const build = source.split('  function buildConfig(group) {')[1].split('  function nextRevision')[0];
  const groups = ['quick', 'batch', 'temperature', 'turning', 'sensor', 'lightAlarm', 'humidifier', 'advanced'];
  for (const group of groups) {
    const section = build.split(new RegExp(`(?:if|else if) \\(group === '${group}'\\) \\{`))[1]
      ?.split(/\n    \} else if|\n    \}\n    return config/)[0];
    assert.ok(section, `missing config form: ${group}`);
    const names = [...new Set([...section.matchAll(/config\.([A-Za-z0-9]+)\s*=/g)]
      .map((match) => match[1]))];
    if (group === 'quick' || group === 'batch') names.push(
      'lowTempAlarm', 'highTempAlarm', 'emergencyTemp', 'ventOnTemp', 'ventOffTemp');
    assert.ok(names.length, `${group}: no fields`);
    const patch = Object.fromEntries(names.map((name) => [name, name === 'sirenSelfTestEnabled'
      ? true : 4294967295]));
    const body = { v: 2, requestId: 'cfg-' + 'a'.repeat(20), clientId: 'w-' + 'b'.repeat(16),
      seq: 1780000000000, nonce: 'c'.repeat(16), bootId: 4294967295,
      revision: 4294967295, config: patch };
    const envelope = { v: 2, grant: `w-${'b'.repeat(16)}|1780000000|${'d'.repeat(24)}`,
      grantSig: 'e'.repeat(64), body: JSON.stringify(body), sig: 'f'.repeat(64) };
    const bytes = Buffer.byteLength(JSON.stringify(envelope)) +
      PacketPolicy.FRAME_OVERHEAD;
    assert.ok(bytes < PacketPolicy.NORMAL_CAP, `${group}: ${bytes} B`);
  }
  const header = readFileSync(require.resolve('../MAYAP_INDUSTRIAL_v1_0_0/protocol_limits.h'), 'utf8');
  for (const [key, name] of [['HARD_CAP', 'FRAME_HARD_CAP'],
    ['NORMAL_CAP', 'FRAME_NORMAL_CAP'], ['SMALL_TARGET', 'FRAME_SMALL_TARGET'],
    ['CHUNK_TARGET', 'FRAME_CHUNK_TARGET'], ['FRAME_OVERHEAD', 'FRAME_OVERHEAD']]) {
    assert.match(header, new RegExp(`${name} = ${PacketPolicy[key]}U;`));
  }
});

test('humidifier thresholds preserve the config record layout and legacy default', () => {
  const machine = readFileSync(require.resolve('../MAYAP_INDUSTRIAL_v1_0_0/machine_control.h'), 'utf8');
  const firmware = readFileSync(require.resolve('../MAYAP_INDUSTRIAL_v1_0_0/transaction_bridge.h'), 'utf8');
  assert.match(machine, /humidityAlarmDelaySec & 0x03FFU/);
  assert.match(machine, /humidifierHysteresisRh & 0x0FU/);
  assert.match(machine, /humidityGap \? humidityGap : MachineConfig\{\}\.humidifierHysteresisRh/);
  assert.match(machine, /config_\.targetHumidity - config_\.humidifierHysteresisRh/);
  assert.match(firmware, /c\["humidifierHysteresisRh"\] = cfg\.humidifierHysteresisRh/);
  assert.match(firmware, /candidate\.humidifierHysteresisRh = configObj\["humidifierHysteresisRh"\]/);
  const packed = (60 & 0x03ff) | (6 << 10) | 0x4000 | 0x8000;
  assert.equal(packed & 0x03ff, 60);
  assert.equal((packed >> 10) & 0x0f, 6);
  assert.equal(packed & 0xc000, 0xc000);
});

test('ACK HMAC binds result, reason and request identity', async () => {
  const { webcrypto } = require('node:crypto');
  const key = await webcrypto.subtle.importKey('raw', new Uint8Array(32).fill(7),
    { name: 'HMAC', hash: 'SHA-256' }, false, ['sign', 'verify']);
  const input = ['mayap-mqtt-ack:v2', 'MAP-1234567890AB', 'cmd-001',
    'batch.start', 'completed', '0', 'BATCH_HEATER_SWITCH_OFF',
    '123', '4', 'Hãy bật công tắc thanh nhiệt trước'].join('\n');
  const signature = await webcrypto.subtle.sign('HMAC', key, Buffer.from(input));
  assert.ok(await webcrypto.subtle.verify('HMAC', key, signature, Buffer.from(input)));
  assert.equal(await webcrypto.subtle.verify('HMAC', key, signature,
    Buffer.from(input.replace('completed\n0', 'completed\n1'))), false);
});

test('firmware guards the replay, EEPROM, safety and packet boundaries', () => {
  const realtime = readFileSync(require.resolve('../MAYAP_INDUSTRIAL_v1_0_0/transaction_bridge.h'), 'utf8');
  const hmi = readFileSync(require.resolve('../MAYAP_INDUSTRIAL_v1_0_0/hmi.h'), 'utf8');
  const machine = readFileSync(require.resolve('../MAYAP_INDUSTRIAL_v1_0_0/machine_control.h'), 'utf8');
  const web = readFileSync(require.resolve('../app.js'), 'utf8');
  const worker = readFileSync(require.resolve('../cloudflare/src/index.js'), 'utf8');
  assert.equal(web.includes('/api/device/sign-mqtt'), false);
  assert.equal(worker.includes('handleLegacySignMqtt'), false);
  assert.ok(worker.includes('ACCOUNT_UPGRADE_REQUIRED'));
  assert.match(realtime, /terminalCache\[16\]/);
  assert.match(realtime, /replayTerminal\(id\)/);
  assert.match(realtime, /checkReplaySequence\(bodyDoc\)/);
  assert.match(realtime, /bodyDoc\["bootId"\]\.as<uint32_t>\(\) != bootId/);
  assert.match(realtime, /expiry < static_cast<unsigned long>\(now\)/);
  assert.match(realtime, /forceSnapshotPublish = true/);
  assert.match(realtime, /mayap-mqtt-ack:v2/);
  assert.match(realtime, /expirePendingCommands\(now\)/);
  assert.match(realtime, /timeReached\(now, slot\.queuedAt\)/);
  assert.match(realtime, /timeReached\(now, pendingConfigSave\.queuedAt\)/);
  assert.doesNotMatch(realtime, /pendingReminderSave/);
  assert.match(hmi, /timeReached\(now, configSave\.startedAt\)/);
  assert.equal((hmi.match(/timeReached\(now, command\.createdAt\)/g) || []).length, 2);
  assert.match(machine, /store_\.saveConfig\(requested, readback\)/);
  for (const code of ['BATCH_AUTO_OFF', 'BATCH_HEATER_SWITCH_OFF', 'BATCH_SENSOR_ERROR',
    'BATCH_RTC_INVALID', 'BATCH_TURNING_FAULT', 'BATCH_OVERHEAT',
    'BATCH_EEPROM_ERROR', 'CONFIG_EEPROM_ERROR', 'HISTORY_EEPROM_ERROR']) {
    assert.ok(realtime.includes(code), code);
  }
});

test('a callback timestamp newer than the loop sample is not an elapsed timeout', () => {
  const nowCapturedBeforeCallback = 1000;
  const queuedInsideCallback = 1001;
  const rawUnsignedElapsed = (nowCapturedBeforeCallback - queuedInsideCallback) >>> 0;
  assert.equal(rawUnsignedElapsed, 0xffffffff);
  const timeReached = (now, target) => ((now - target) | 0) >= 0;
  assert.equal(timeReached(nowCapturedBeforeCallback, queuedInsideCallback), false);
  assert.equal(timeReached(queuedInsideCallback + 8000, queuedInsideCallback), true);
});
