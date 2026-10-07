'use strict';
// mqtt_ws.h on the host: RFC vectors and framing (tests/mqtt-ws.cpp), with Sec-WebSocket-Accept cross-checked by Node's crypto.
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { test } = require('node:test');

const root = path.resolve(__dirname, '..');
test('WebSocket client layer: RFC 6455 vectors, in-place masked framing, bounded server parser', (t) => {
  const work = fs.mkdtempSync(path.join(os.tmpdir(), 'mayap-ws-'));
  const exe = path.join(work, 'mqtt-ws');
  const compile = spawnSync('g++', ['-std=c++11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
    path.join(root, 'tests/mqtt-ws.cpp'), '-o', exe], { encoding: 'utf8' });
  if (compile.error && compile.error.code === 'ENOENT') return t.skip('g++ not available');
  assert.equal(compile.status, 0, compile.stderr);
  const run = spawnSync(exe, [], { encoding: 'utf8' });
  assert.equal(run.status, 0, run.stderr + run.stdout);
  assert.match(run.stdout, /mqtt ws host tests PASS/);
  const lines = run.stdout.split('\n').filter((l) => l.startsWith('ACCEPT '));
  assert.equal(lines.length, 64);
  for (const line of lines) {
    const [, key, accept] = line.split(' ');
    assert.equal(Buffer.from(key, 'base64').length, 16);
    const expected = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    assert.equal(accept, expected, key);
  }
  fs.rmSync(work, { recursive: true, force: true });
});
