'use strict';
// Light-left-on alarm policy on the host (tests/light-alarm.cpp).
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { test } = require('node:test');

const root = path.resolve(__dirname, '..');
test('light alarm: toggling is silent, only 30 min continuous on alarms, resolved only after a raise', (t) => {
  const work = fs.mkdtempSync(path.join(os.tmpdir(), 'mayap-light-'));
  const exe = path.join(work, 'light-alarm');
  const compile = spawnSync('g++', ['-std=c++11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
    path.join(root, 'tests/light-alarm.cpp'), '-o', exe], { encoding: 'utf8' });
  if (compile.error && compile.error.code === 'ENOENT') return t.skip('g++ not available');
  assert.equal(compile.status, 0, compile.stderr);
  const run = spawnSync(exe, [], { encoding: 'utf8' });
  assert.equal(run.status, 0, run.stderr + run.stdout);
  assert.match(run.stdout, /light alarm host tests PASS/);
  fs.rmSync(work, { recursive: true, force: true });
});
