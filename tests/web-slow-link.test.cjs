'use strict';
// A congested but working uplink must not be shown as slow data, must not make the Web flood the machine with retries/syncs,
// and a light press must not be repeatable while the first one is unconfirmed (it is a relative flip on the machine).
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { test } = require('node:test');
const vm = require('node:vm');
const { webcrypto } = require('node:crypto');
const { TransactionLedger, PacketPolicy } = require('../protocol_v2.js');

function browser() {
  const source = readFileSync(require.resolve('../app.js'), 'utf8').replace(/  init\(\);\s*\}\)\(\);\s*$/, `
    Object.assign(window.hooks, { state, WEB, connectionStatus, isDeviceOnline, deviceDataAgeMs, selectedNeedsSync, retryDelayMs,
      lightCommandPending, sendCommand, slowNote, dispatchRealtimeMessage });
  })();`);
  const elements = new Map();
  const window = { addEventListener() {}, MayapProtocolV2: { TransactionLedger, PacketPolicy },
    MAYAP_WEB_CONFIG: { cloudApiBase: 'https://test.invalid' }, hooks: {} };
  const context = { window, crypto: webcrypto, TextEncoder, URL, URLSearchParams, AbortController,
    console: { info() {}, warn() {}, error() {} }, performance: { now: () => 0 },
    localStorage: { getItem: () => null, setItem() {} },
    fetch: async () => ({ ok: false, status: 403, json: async () => ({}) }),
    document: { getElementById: (id) => elements.get(id) || null, addEventListener() {},
      createElement: () => ({ children: [], className: '', textContent: '', setAttribute() {}, append() {}, replaceChildren() {} }) },
    setTimeout: () => 1, clearTimeout() {}, setInterval: () => 1, clearInterval() {} };
  vm.runInNewContext(source, context, { filename: 'app.js' });
  const h = window.hooks;
  const device = { id: 'MAP-1234567890AB', bootId: 123, revision: 1, presence: { proto: 2, online: true, state: 'online' }, presenceEpoch: 0,
    snapshot: { revision: 1, runtime: {} }, config: null };
  h.state.devices.push(device);
  h.state.selectedId = device.id;
  h.state.realtimeConnected = true;
  device.dataSource = 'live';
  device.liveEpoch = h.state.subscriptionEpoch;
  device.presenceEpoch = h.state.subscriptionEpoch;
  return { ...h, device };
}
const ago = (ms) => Date.now() - ms;

test('a snapshot gap of a congested uplink is still ONLINE; "slow data" needs a long silence', () => {
  const h = browser();
  assert.ok(h.WEB.staleAfterMs >= 20000);
  h.device.snapshotAt = ago(2000); assert.equal(h.connectionStatus(h.device), 'online');
  h.device.snapshotAt = ago(12000); assert.equal(h.connectionStatus(h.device), 'online');     // was 'degraded' after 8 s
  assert.match(h.slowNote(h.device), /cập nhật 1[12] s trước/);                               // the age is shown instead of an alarm
  h.device.snapshotAt = ago(25000); assert.equal(h.connectionStatus(h.device), 'degraded');
});

test('any frame the machine sent keeps it fresh, but only on the current connection and never presence', () => {
  const h = browser();
  h.device.snapshotAt = ago(25000);
  assert.equal(h.connectionStatus(h.device), 'degraded');
  h.device.lastHeardAt = Date.now() - 1500; h.device.lastHeardEpoch = h.state.subscriptionEpoch;   // what dispatchRealtimeMessage records
  assert.equal(h.connectionStatus(h.device), 'online');
  assert.ok(h.deviceDataAgeMs(h.device) < 3000);
  h.state.subscriptionEpoch++; h.device.liveEpoch = h.state.subscriptionEpoch; h.device.presenceEpoch = h.state.subscriptionEpoch;
  assert.ok(h.deviceDataAgeMs(h.device) >= 24000);                                                // a frame heard on the old connection counts for nothing
  const source = readFileSync(require.resolve('../app.js'), 'utf8');
  const dispatch = source.slice(source.indexOf('function dispatchRealtimeMessage('), source.indexOf('function realtimeHandlers('));
  assert.match(dispatch, /channel === 'snapshot' \|\| channel === 'config\/reported' \|\| channel === 'log' \|\| channel === 'history\/reported' \|\| channel === 'ack'\) \{\s*device\.lastHeardAt/);
  const condition = dispatch.match(/if \(([^\n]*)\) \{\s*device\.lastHeardAt/)[1];
  assert.doesNotMatch(condition, /presence|uplink/);                                             // broker-originated channels are not proof of data
});

test('controls stay usable through a slow stretch and switch off only after the offline limit', () => {
  const h = browser();
  h.device.snapshotAt = ago(15000); assert.equal(h.isDeviceOnline(h.device), true);
  h.device.snapshotAt = ago(25000); assert.equal(h.isDeviceOnline(h.device), true);           // degraded label, control not removed
  h.device.snapshotAt = ago(35000); assert.equal(h.isDeviceOnline(h.device), false);
  h.dispatchRealtimeMessage(h.device, 'ack', { requestId: 'x' });
  assert.equal(h.isDeviceOnline(h.device), true);
});

test('the Web does not ask the machine to replay its state while the data is merely a few seconds old', () => {
  const source = readFileSync(require.resolve('../app.js'), 'utf8');
  const needs = source.slice(source.indexOf('function selectedNeedsSync()'), source.indexOf('function clearSyncRetries()'));
  assert.match(needs, /deviceDataAgeMs\(device\) > WEB\.staleAfterMs/);                            // the replay request uses the same 20 s limit and any-frame freshness
  assert.match(source, /state\.syncRetryTimers = \[2500, 6000\]\.map\(/);                         // first-connection syncs are spread, not 0 / 0.7 / 1.6 s
  const h = browser();
  assert.equal(h.selectedNeedsSync(), true);                                                       // an incomplete config still asks (no config yet)
});

test('retries back off (5, 10, 20, 30 s) instead of repeating every 3.5 s, and timeouts exceed the longest measured wire wait', () => {
  const h = browser();
  assert.deepEqual([0, 1, 2, 3, 4, 5].map(h.retryDelayMs), [5000, 10000, 20000, 30000, 30000, 30000]);
  assert.ok(h.WEB.commandTimeoutMs >= 20000 && h.WEB.configTimeoutMs >= 30000);
  const config = readFileSync(require.resolve('../config.js'), 'utf8');
  assert.match(config, /staleAfterMs: 20000/); assert.match(config, /commandTimeoutMs: 20000, configTimeoutMs: 30000/);
});

test('a second light press is refused while the first is unconfirmed, and allowed again once it settles', async () => {
  const h = browser();
  h.device.snapshotAt = Date.now();
  h.device.lightPendingId = 'cmd-first';
  h.state.pending.set('cmd-first', { kind: 'command' });
  assert.equal(h.lightCommandPending(h.device), true);
  assert.equal(await h.sendCommand('light_toggle', { device: h.device }), false);
  assert.equal(h.state.pending.size, 1);                                                      // nothing new was started
  h.state.pending.delete('cmd-first');
  assert.equal(h.lightCommandPending(h.device), false);
});
