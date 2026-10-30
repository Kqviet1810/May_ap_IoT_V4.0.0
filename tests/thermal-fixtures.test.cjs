const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const read = (name) => fs.readFileSync(path.resolve(__dirname, '..', name));
const sha = (name) => crypto.createHash('sha256').update(read(name)).digest('hex');

// The thermal tests compare the controller of this release with the one of 2.3.4 and replay the machine's own log of 29/10. Both are
// evidence, not code under development: an edit must be a decision, not an accident.
test('frozen reference controller of firmware 2.3.4 and the failed-batch log are unchanged', () => {
  assert.equal(sha('tests/fixtures/thermal-v3-2.3.4/thermal_v3.h'), '4d610fa9b6a35d41b340c0606740746cfc3c3669d7a7d38f2b5b3fc57c656110');
  assert.equal(sha('tests/fixtures/real-logs/mayap-20261029-batch-failure.csv'), '9e34946647ae65fb8685e1f9e4aa28094a99261b5c590efb754ab1b6be03bc4c');
});

test('the log fixture carries no device identity', () => {
  const text = read('tests/fixtures/real-logs/mayap-20261029-batch-failure.csv').toString('utf8');
  assert.doesNotMatch(text, /MAP-[0-9A-F]{12}/);
  assert.match(text.split('\n')[0], /^# mayap pid log device=MAP-X+ /);
});

test('every thermal host test is run by the reliability workflow', () => {
  const workflow = read('.github/workflows/reliability-checks.yml').toString('utf8');
  for (const name of ['thermal-v3', 'heater-response-watch', 'thermal-closed-loop', 'thermal-replay'])
    assert.ok(workflow.includes(`tests/${name}.cpp`), `${name}.cpp is not compiled by CI`);
  assert.ok(workflow.includes('tests/thermal-e115.cpp') || workflow.includes('tools/test_heater_path.py'), 'E115 production slice test');
});

test('the firmware includes the E115 evidence class and no longer carries its own copy of the rule', () => {
  const machine = read('MAYAP_INDUSTRIAL_v1_0_0/machine_control.h').toString('utf8');
  assert.match(machine, /#include "heater_response_watch\.h"/);
  assert.match(machine, /HeaterResponseWatch heaterWatch_;/);
  assert.doesNotMatch(machine, /heaterStuckStartTemp_|heaterStuckAccumOnMs_|heaterStuckTracking_|heaterStuckSinceAt_/);
});
