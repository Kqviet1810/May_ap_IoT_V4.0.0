'use strict';
// Host test of the firmware's parser for the /api/firmware/check answer (needs the ArduinoJson headers: MAYAP_ARDUINOJSON, as in the CI "host" job).
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const root = path.join(__dirname, '..');

test('device parser accepts every shape of the check answer and rejects broken ones', (t) => {
  const aj = process.env.MAYAP_ARDUINOJSON;
  if (!aj || !fs.existsSync(path.join(aj, 'ArduinoJson.h'))) { t.skip('MAYAP_ARDUINOJSON not set'); return; }
  const work = fs.mkdtempSync(path.join(os.tmpdir(), 'mayap-fwparse-'));
  const exe = path.join(work, 'fwparse');
  const build = spawnSync('g++', ['-std=c++11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-I', aj, path.join(root, 'tests/firmware-check-parse.cpp'), '-o', exe], { encoding: 'utf8' });
  assert.equal(build.status, 0, build.stderr);
  const run = spawnSync(exe, [], { encoding: 'utf8' });
  assert.equal(run.status, 0, run.stderr);
  assert.match(run.stdout, /firmware check answer parser host tests PASS/);
  fs.rmSync(work, { recursive: true, force: true });
});

test('ota_web_update.h uses the parser, a heap buffer and reports every outcome', () => {
  const src = fs.readFileSync(path.join(root, 'MAYAP_INDUSTRIAL_v1_0_0/ota_web_update.h'), 'utf8');
  assert.match(src, /#include "firmware_check_parse\.h"/);
  assert.doesNotMatch(src, /char resp\[1024\]/);
  assert.match(src, /malloc\(P::MaxBodyBytes\)/);
  for (const text of ['TRA LOI QUA DAI', 'TRA LOI KHONG DOC DUOC', 'SAI KHOA THIET BI 401', 'BAN MOI VUOT PHAN VUNG', 'CHUA ONLINE/MAT MANG'])
    assert.ok(src.includes(text), text);
  assert.match(src, /\[FWWEB\] check OK: dang chay/);
});
