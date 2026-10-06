'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const app = fs.readFileSync(path.join(root, 'app.js'), 'utf8');
const proto = fs.readFileSync(path.join(root, 'protocol_v2.js'), 'utf8');

test('every PACKET_POLICY member used by app.js is defined (undefined -> NaN disables the size guard)', () => {
  const block = proto.match(/PacketPolicy\s*=\s*Object\.freeze\(\{([\s\S]*?)\}\)/)
    || proto.match(/PacketPolicy[^{]*\{([\s\S]*?)\}/);
  assert.ok(block, 'PacketPolicy definition not found');
  const defined = new Set([...block[1].matchAll(/([A-Z_]+)\s*:/g)].map((m) => m[1]));
  const used = new Set([...app.matchAll(/PACKET_POLICY\.([A-Z_]+)/g)].map((m) => m[1]));
  for (const name of used) assert.ok(defined.has(name), `PACKET_POLICY.${name} is not defined`);
});

test('MQTT transport creation failure resets the connecting guard and retries', () => {
  const m = app.match(/transport = window\.MayapMqttTransport\.create[\s\S]*?state\.realtime = transport;/);
  assert.ok(m && /catch \(error\)[\s\S]*connectingDeviceId = '';[\s\S]*retryLater\(\)/.test(m[0]));
});
