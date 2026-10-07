'use strict';
// Real mqtt_transport.h / mqtt_wire.h against a scripted TLS socket (see tests/mqtt-transport.cpp).
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { test } = require('node:test');

const root = path.resolve(__dirname, '..');

test('native MQTT/TLS transport: handshake, QoS1, inbound dispatch, keepalive, backoff, gates', (t) => {
  const work = fs.mkdtempSync(path.join(os.tmpdir(), 'mayap-transport-'));
  for (const f of fs.readdirSync(path.join(root, 'tests/stubs/mqtt'))) fs.copyFileSync(path.join(root, 'tests/stubs/mqtt', f), path.join(work, f));
  // Production files, unmodified, placed beside the stubs so their quoted includes resolve to the stubs.
  for (const f of ['mqtt_transport.h', 'mqtt_wire.h']) fs.copyFileSync(path.join(root, 'MAYAP_INDUSTRIAL_v1_0_0', f), path.join(work, f));
  fs.copyFileSync(path.join(root, 'tests/mqtt-transport.cpp'), path.join(work, 'mqtt-transport.cpp'));
  const exe = path.join(work, 'mqtt-transport');
  const compile = spawnSync('g++', ['-std=c++11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-I', work,
    path.join(work, 'mqtt-transport.cpp'), '-o', exe], { encoding: 'utf8' });
  if (compile.error && compile.error.code === 'ENOENT') return t.skip('g++ not available');
  assert.equal(compile.status, 0, compile.stderr);
  const run = spawnSync(exe, [], { encoding: 'utf8' });
  assert.equal(run.status, 0, run.stderr + run.stdout);
  assert.match(run.stdout, /mqtt transport host tests PASS/);
  fs.rmSync(work, { recursive: true, force: true });
});
