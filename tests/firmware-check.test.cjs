'use strict';
// Daily firmware check policy on the host (tests/firmware-check.cpp) + the configured cadence.
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { test } = require('node:test');

const root = path.resolve(__dirname, '..');
test('firmware check: first check after power-up delay, then daily with jitter, check-now immediate', (t) => {
  const work = fs.mkdtempSync(path.join(os.tmpdir(), 'mayap-fwcheck-'));
  const exe = path.join(work, 'firmware-check');
  const compile = spawnSync('g++', ['-std=c++11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
    path.join(root, 'tests/firmware-check.cpp'), '-o', exe], { encoding: 'utf8' });
  if (compile.error && compile.error.code === 'ENOENT') return t.skip('g++ not available');
  assert.equal(compile.status, 0, compile.stderr);
  const run = spawnSync(exe, [], { encoding: 'utf8' });
  assert.equal(run.status, 0, run.stderr + run.stdout);
  assert.match(run.stdout, /firmware check cadence host tests PASS/);
  fs.rmSync(work, { recursive: true, force: true });
});
test('firmware check cadence is 24 h, 0-30 min jitter, first check >= 5 min after power-up', () => {
  const config = fs.readFileSync(path.join(root, 'MAYAP_INDUSTRIAL_v1_0_0/config.h'), 'utf8');
  assert.match(config, /FIRMWARE_CHECK_INTERVAL_MS = 24UL \* 60UL \* 60UL \* 1000UL/);
  assert.match(config, /FIRMWARE_CHECK_JITTER_MS = 30UL \* 60UL \* 1000UL/);
  const first = /FIRMWARE_FIRST_CHECK_DELAY_MS = (\d+)UL \* 60UL \* 1000UL/.exec(config);
  assert.ok(first && Number(first[1]) >= 5, 'first check delay');
  const web = fs.readFileSync(path.join(root, 'MAYAP_INDUSTRIAL_v1_0_0/ota_web_update.h'), 'utf8');
  assert.match(web, /MayapFirmwareCheck::due\(now, lastCheckAt, checkNow, FIRMWARE_FIRST_CHECK_DELAY_MS/);
});
