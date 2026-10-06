const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { test } = require('node:test');
const vm = require('node:vm');
const { webcrypto } = require('node:crypto');
const { TransactionLedger, PacketPolicy } = require('../protocol_v2.js');

function browser() {
  const source = readFileSync(require.resolve('../app.js'), 'utf8').replace(/  init\(\);\s*\}\)\(\);\s*$/, `
    Object.assign(window.hooks, { state, transactions, startTransaction, handleAck,
      verifyDeviceAck, sweepUncertain, handleConfigReport,
      moveToUncertain, publish, retrySameRequest, resumeExactRetries, clearPending, telemetryChart, storeControlSession,
      signRealtimeWrite, controlSession, controlSessions, handleSnapshot, CONFIG_KEYS,
      buildConfig, validateHumidifierForm, syncHumidifierFeatureUi,
      refreshFaultPopupContent });
  })();`);
  const timers = new Map();
  let nextTimer = 0;
  let time = 0;
  const elements = new Map();
  const window = { addEventListener() {}, MayapProtocolV2: {
    TransactionLedger: class extends TransactionLedger { constructor() { super(() => time); } },
    PacketPolicy },
    MAYAP_WEB_CONFIG: { cloudApiBase: 'https://test.invalid' }, hooks: {} };
  const context = { window, crypto: webcrypto, TextEncoder, URL, URLSearchParams, AbortController,
    console: { info() {}, warn() {}, error() {} },
    performance: { now: () => time },
    localStorage: { getItem: () => null, setItem() {} },
    fetch: async () => ({ ok: false, status: 403,
      json: async () => ({ success: false, error: 'Phiên hết hạn' }) }),
    document: { getElementById: id => elements.get(id) || null, addEventListener() {},
      createElement: () => ({ children: [], className: '', textContent: '',
        append(...items) { this.children.push(...items); },
        replaceChildren(...items) { this.children = items; } }) },
    setTimeout: fn => { const id = ++nextTimer; timers.set(id, fn); return id; },
    clearTimeout: id => timers.delete(id),
    setInterval: () => 1, clearInterval() {} };
  vm.runInNewContext(source, context, { filename: 'app.js' });
  const device = { id: 'MAP-1234567890AB', bootId: 123, revision: 0, presence: { proto: 2 },
    pairingToken: 'test', config: null };
  window.hooks.state.devices.push(device);
  return { ...window.hooks, device, elements, timers,
    advance(ms) { time += ms; } };
}

async function signedAck(h, id, operation, ok, key, override = {}) {
  const ack = { v: 2, requestId: id, operation, phase: 'completed', ok,
    code: ok ? 'APPLIED' : 'BATCH_AUTO_OFF', bootId: 123,
    revision: 1, message: ok ? 'Đã thực hiện' : 'Chưa bật AUTO', ...override };
  const text = ['mayap-mqtt-ack:v2', h.device.id, ack.requestId, ack.operation,
    ack.phase, ack.ok ? '1' : '0', ack.code, ack.bootId, ack.revision,
    ack.message].join('\n');
  ack.sig = Buffer.from(await webcrypto.subtle.sign('HMAC', key,
    new TextEncoder().encode(text))).toString('hex');
  return ack;
}

async function start(h, id, operation = 'light.toggle') {
  const key = await webcrypto.subtle.importKey('raw', new Uint8Array(32).fill(7),
    { name: 'HMAC', hash: 'SHA-256' }, false, ['sign', 'verify']);
  const pending = h.startTransaction(id, { kind: 'command', operation,
    deviceId: h.device.id, action: operation.replaceAll('.', '_'), ackKey: key }, 100);
  return { key, pending };
}

test('late signed applied and rejected ACK settle an UNCERTAIN command', async () => {
  for (const ok of [true, false]) {
    const h = browser();
    const { key, pending } = await start(h, 'cmd-late');
    pending.onTimeout();
    assert.equal(h.state.uncertain.size, 1);
    const ack = await signedAck(h, 'cmd-late', 'light.toggle', ok, key);
    assert.equal(await h.verifyDeviceAck(h.device, ack), true);
    h.handleAck(h.device, ack);
    assert.equal(h.state.uncertain.size, 0);
    assert.equal(h.transactions.entries.size, 0);
    assert.equal(pending.phase, ok ? 'APPLIED' : 'REJECTED');
    assert.equal(h.elements.get('toast')?.textContent, undefined);
  }
});

test('invalid HMAC, operation mismatch and replay cannot complete late transaction', async () => {
  const h = browser();
  const { key, pending } = await start(h, 'cmd-secure');
  pending.onTimeout();
  const valid = await signedAck(h, 'cmd-secure', 'light.toggle', true, key);
  assert.equal(await h.verifyDeviceAck(h.device, { ...valid, ok: false }), false);
  assert.equal(await h.verifyDeviceAck(h.device, { ...valid, sig: '0'.repeat(64) }), false);
  const wrongOp = await signedAck(h, 'cmd-secure', 'batch.start', true, key);
  assert.equal(await h.verifyDeviceAck(h.device, wrongOp), true);
  h.handleAck(h.device, wrongOp);
  assert.equal(h.state.uncertain.size, 1);
  h.handleAck(h.device, valid);
  h.handleAck(h.device, valid); // duplicate terminal is ignored
  assert.equal(h.state.uncertain.size, 0);
  const { key: expiredKey, pending: expiredPending } = await start(h, 'cmd-expired');
  expiredPending.onTimeout();
  const expired = await signedAck(h, 'cmd-expired', 'light.toggle', false, expiredKey,
    { code: 'SESSION_EXPIRED', message: 'Phiên đã hết hạn' });
  assert.equal(await h.verifyDeviceAck(h.device, expired), true);
  h.handleAck(h.device, expired);
  assert.equal(expiredPending.phase, 'REJECTED');
});

test('bounded uncertain cleanup includes every operation and late ACK after observation', async () => {
  const h = browser();
  for (const [id, kind, operation] of [
    ['c', 'config', 'config.save'],
    ['h', 'history', 'history.read'], ['b', 'command', 'batch.start']]) {
    const pending = h.startTransaction(id, { kind, operation, deviceId: h.device.id,
      action: kind === 'command' ? 'batch_start' : undefined,
      formId: 'quickForm', revision: 1, patch: { targetTemp: 37 } }, 100);
    pending.onTimeout();
  }
  assert.equal(h.state.uncertain.size, 3);
  h.advance(PacketPolicy.UNCERTAIN_TTL_MS + 1);
  h.sweepUncertain();
  assert.equal(h.state.uncertain.size, 0);
  assert.equal(h.transactions.entries.size, 0);

  const other = browser();
  const { key, pending } = await start(other, 'late-observed');
  pending.onTimeout();
  pending.observed = true;
  const rejected = await signedAck(other, 'late-observed', 'light.toggle', false, key);
  assert.equal(await other.verifyDeviceAck(other.device, rejected), true);
  other.handleAck(other.device, rejected);
  assert.equal(other.state.uncertain.size, 0);
});

test('background history timeout stays inside chart and does not show a global toast', () => {
  const h = browser();
  const toast = { textContent: '', classList: { add() {}, remove() {} } };
  h.elements.set('toast', toast);
  const pending = h.startTransaction('hist-background', {
    kind: 'history', operation: 'history.read', deviceId: h.device.id
  }, 100);
  pending.onTimeout();
  assert.equal(pending.phase, 'UNCERTAIN');
  assert.equal(toast.textContent, '');
});

test('fault popup renders every active fault instead of only the newest one', () => {
  const h = browser();
  const list = { children: [], replaceChildren(...items) { this.children = items; } };
  h.elements.set('faultPopup', { hidden: true });
  h.elements.set('faultPopupList', list);
  h.device.activeFaults = [
    { code: 101, severity: 1 },
    { code: 502, severity: 1 },
    { code: 301, severity: 2 }
  ];
  h.refreshFaultPopupContent(h.device);
  assert.equal(list.children.length, 3);
  assert.equal(list.children[0].children[0].children[0].textContent, 'E101');
  assert.equal(list.children[2].children[0].children[0].textContent, 'E301');
});

test('config and batch state reconcile without hiding a later signed rejection', async () => {
  const h = browser();
  const full = Object.fromEntries(h.CONFIG_KEYS.map((key) => [key, 1]));
  full.targetTemp = 37;
  const key = await webcrypto.subtle.importKey('raw', new Uint8Array(32).fill(9),
    { name: 'HMAC', hash: 'SHA-256' }, false, ['sign', 'verify']);
  const config = h.startTransaction('cfg-a', { kind: 'config', operation: 'config.save',
    deviceId: h.device.id, formId: 'quickForm', revision: 2, bootId: 123,
    patch: { targetTemp: 37 }, config: full, ackKey: key }, 100);
  config.onTimeout();
  // A matching value without the verified request ID is not confirmation.
  h.handleConfigReport(h.device, { revision: 2, bootId: 123, config: full });
  assert.equal(h.state.uncertain.has('cfg-a'), true);
  const reject = await signedAck(h, 'cfg-a', 'config.save', false, key,
    { revision: 2, code: 'CONFIG_EEPROM_ERROR', message: 'Lưu EEPROM lỗi' });
  assert.equal(await h.verifyDeviceAck(h.device, reject), true);
  h.handleAck(h.device, reject);
  assert.equal(config.phase, 'REJECTED');

  const verified = h.startTransaction('cfg-b', { kind: 'config', operation: 'config.save',
    deviceId: h.device.id, formId: 'quickForm', revision: 3, bootId: 123,
    patch: { targetTemp: 37 }, config: full, ackKey: key }, 100);
  verified.onTimeout();
  h.handleConfigReport(h.device, { revision: 3, bootId: 123, requestId: 'cfg-b', config: full });
  assert.equal(verified.phase, 'UNCERTAIN'); // Settled by the verified report.
  assert.equal(h.state.uncertain.has('cfg-b'), false);

  const batch = h.startTransaction('cmd-batch', { kind: 'command', operation: 'batch.start',
    action: 'batch_start', deviceId: h.device.id, ackKey: key }, 100);
  batch.onTimeout();
  h.handleSnapshot(h.device, { bootId: 123, revision: 2, runtime: { batchRunning: true } });
  assert.equal(batch.observed, true);
  assert.equal(h.state.uncertain.has('cmd-batch'), true);
});

test('disabled publisher leaves Transaction V2 unsettled', async () => {
  const h = browser();
  const { pending } = await start(h, 'disabled-publisher');
  await assert.rejects(h.publish({ deviceId:h.device.id, channel:'command' }, {requestId:'disabled-publisher'}, {awaitAck:true}), {code:'TRANSPORT_ERROR'});
  assert.equal(pending.phase, 'CREATED');
});

test('session HMAC binds channel/body; expired session requires refresh; firmware replay gates remain', async () => {
  const h = browser();
  const raw = '07'.repeat(32);
  await h.storeControlSession(h.device, { sessionKey: raw, grant: 'w-1234567890abcdef|1780000000|' + 'a'.repeat(24),
    grantSig: 'b'.repeat(64), expiresAt: Math.floor(Date.now() / 1000) + 120 });
  const pending = h.startTransaction('cmd-auth', { operation: 'light.toggle', deviceId: h.device.id }, 100);
  const wire = await h.signRealtimeWrite(h.device, 'command', { v: 2, requestId: 'cmd-auth' });
  const session = h.controlSessions.get(h.device.id);
  const body = JSON.parse(wire.body);
  const input = `mayap-mqtt-write:v2\n${h.device.id}\ncommand\n${wire.grant}\n${wire.body}`;
  assert.equal(await webcrypto.subtle.verify('HMAC', session.key,
    Buffer.from(wire.sig, 'hex'), new TextEncoder().encode(input)), true);
  assert.equal(await webcrypto.subtle.verify('HMAC', session.key,
    Buffer.from(wire.sig, 'hex'), new TextEncoder().encode(input.replace('\ncommand\n', '\nconfig/set\n'))), false);
  assert.equal(pending.ackKey, session.key);
  session.expiresAt = Math.floor(Date.now() / 1000) - 1;
  await assert.rejects(h.signRealtimeWrite(h.device, 'command',
    { v: 2, requestId: 'cmd-expired-local' }), { code: 'AUTH_ERROR' });
  const firmware = readFileSync(require.resolve('../MAYAP_INDUSTRIAL_v1_0_0/transaction_bridge.h'), 'utf8');
  assert.match(firmware, /expiry < static_cast<unsigned long>\(now\)/);
  assert.match(firmware, /if \(expired\)[\s\S]*?SESSION_EXPIRED/);
  assert.match(firmware, /if \(replayTerminal\(id\)\)/);
  assert.match(firmware, /if \(v2 && !checkReplaySequence\(bodyDoc\)\)/);
});

test('humidifier form is hardware gated and maps two thresholds to one bounded hysteresis', () => {
  const h = browser();
  h.device.config = Object.fromEntries(h.CONFIG_KEYS.map(key => [key, 1]));
  h.device.config.targetTemp = 37.5;
  h.device.config.humidifierInstalled = true;
  h.device.config.targetHumidity = 58;
  h.device.config.humidifierHysteresisRh = 2;
  h.state.selectedId = h.device.id;
  for (const [id, value] of [['humidifierOnHumidity', '54'],
    ['humidifierOffHumidity', '60']]) h.elements.set(id, { value });
  h.elements.set('humidifierEnabled', { checked: true });
  h.elements.set('humidifierSetting', { hidden: true, open: true });
  h.elements.set('batchHumidityTile', { hidden: false });
  assert.equal(h.validateHumidifierForm(), true);
  h.syncHumidifierFeatureUi(h.device.config);
  assert.equal(h.elements.get('humidifierSetting').hidden, false);
  assert.equal(h.elements.get('batchHumidityTile').hidden, true);
  const config = h.buildConfig('humidifier');
  assert.equal(config.targetHumidity, 60);
  assert.equal(config.humidifierHysteresisRh, 6);
  assert.equal(config.humidifierEnabled, true);
  h.device.config.humidifierInstalled = false;
  assert.equal(h.buildConfig('humidifier'), null);
  h.syncHumidifierFeatureUi(h.device.config);
  assert.equal(h.elements.get('humidifierSetting').hidden, true);
});

test('terminal ACK cancels reconnect retry for both pending and uncertain transactions',async()=>{
 for(const uncertain of [false,true]){
  const h=browser(),{pending,key}=await start(h,'settle-retry');h.state.selectedId=h.device.id;
  h.retrySameRequest('settle-retry',{deviceId:h.device.id,channel:'command'},{body:'{"requestId":"settle-retry"}',sig:'original'});
  if(uncertain)pending.onTimeout();h.handleAck(h.device,await signedAck(h,'settle-retry','light.toggle',true,key));
  assert.equal(h.timers.size,0);h.resumeExactRetries();assert.equal(h.state.pending.size+h.state.uncertain.size,0);
 }
});

test('signed terminal history ACK cannot complete a transaction with missing chunks',async()=>{
 const h=browser();const {pending,key}=await start(h,'history-missing','history.read');pending.kind='history';h.telemetryChart.activeRequestId='history-missing';
 h.telemetryChart.historyLoaded=false;h.telemetryChart.historyGap=true;
 const ack=await signedAck(h,'history-missing','history.read',true,key);
 h.handleAck(h.device,ack);assert.equal(pending.phase,'UNCERTAIN');assert.equal(h.state.uncertain.has('history-missing'),true);
 h.telemetryChart.historyGap=false;h.telemetryChart.historyLoaded=true;h.handleAck(h.device,ack);
 assert.equal(pending.phase,'APPLIED');assert.equal(h.state.uncertain.has('history-missing'),false);
});
