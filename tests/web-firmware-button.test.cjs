'use strict';
// The Update button: in developer mode it sends apply_firmware_now, otherwise only firmware_check_now (the HMI confirms). The flag is read from
// the runtime the machine publishes (device.snapshot.runtime.tech.devMode) - the Web has no device.tech and no state.telemetry.
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { test } = require('node:test');
const vm = require('node:vm');
const { webcrypto } = require('node:crypto');
const { TransactionLedger, PacketPolicy } = require('../protocol_v2.js');

function browser() {
  const source = readFileSync(require.resolve('../app.js'), 'utf8')
    .replace('async function sendCommand(action, options = {}) {', 'async function sendCommand(action, options = {}) { window.hooks.sent.push(action); return false;')
    .replace(/  init\(\);\s*\}\)\(\);\s*$/, `
    Object.assign(window.hooks, { state, renderFirmwareCard, setLatest(v) { firmwareLatestCache = v; } });
  })();`);
  const button = { handler: null, addEventListener(type, fn) { if (type === 'click') this.handler = fn; } };
  const elements = new Map([
    ['firmwareSummary', { textContent: '' }], ['firmwareBody', { innerHTML: '' }], ['firmwareUpdateDot', { hidden: true }],
    ['firmwareUpdateBtn', button]]);
  const window = { addEventListener() {}, MayapProtocolV2: { TransactionLedger, PacketPolicy },
    MAYAP_WEB_CONFIG: { cloudApiBase: 'https://test.invalid' }, hooks: { sent: [] } };
  const context = { window, crypto: webcrypto, TextEncoder, URL, URLSearchParams, AbortController,
    console: { info() {}, warn() {}, error() {} }, performance: { now: () => 0 },
    localStorage: { getItem: () => null, setItem() {} },
    fetch: async () => ({ ok: false, status: 403, json: async () => ({}) }),
    document: { body: { dataset: { page: 'dashboard' } }, getElementById: (id) => elements.get(id) || null, addEventListener() {},
      createElement: () => ({ children: [], className: '', textContent: '', setAttribute() {}, append() {}, replaceChildren() {} }) },
    setTimeout: () => 1, clearTimeout() {}, setInterval: () => 1, clearInterval() {} };
  vm.runInNewContext(source, context, { filename: 'app.js' });
  const h = window.hooks;
  h.setLatest({ version: '9.9.9', notes: '' });
  return { ...h, button };
}

function press(devMode) {
  const h = browser();
  const tech = devMode === undefined ? undefined : { allowed: true, devMode };
  const device = { id: 'MAP-1234567890AB', presence: { fw: '2.3.3', online: true }, snapshot: { revision: 1, runtime: { tech } } };
  h.renderFirmwareCard(device);
  assert.equal(typeof h.button.handler, 'function', 'the Update button must be wired');
  h.button.handler();
  return h.sent;
}

test('developer mode: the Update button applies the firmware at once', () => {
  assert.deepEqual(press(true), ['apply_firmware_now']);
});

test('not in developer mode (or unknown): the Update button only asks the machine to check', () => {
  assert.deepEqual(press(false), ['firmware_check_now']);
  assert.deepEqual(press(undefined), ['firmware_check_now']);
});
