const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const read = file => fs.readFileSync(path.resolve(__dirname, '..', file), 'utf8');
const dir = 'MAYAP_INDUSTRIAL_v1_0_0/';

test('E503 uses bounded per-state synchronization, not stale cached flags', () => {
  const machine = read(dir + 'machine_control.h');
  const link = machine.slice(machine.indexOf('  void updateAttinyLink('), machine.indexOf('  void updateBatchTime('));
  assert.match(link, /attinyBatchSync_\.expect\(expectedBatch, now\)/);
  assert.match(link, /attinyActivitySync_\.expect\(expectedActivity, now\)/);
  assert.match(link, /completedCode == batchCommand &&\s*!mayapAttinyBusCommandPending\(oppositeBatchCommand\)/);
  assert.match(link, /completedCode == activityCommand &&\s*!mayapAttinyBusCommandPending\(oppositeActivityCommand\)/);
  assert.match(link, /attinyStatusKnown_ && \(attinyBatchSync_\.fault\(\) \|\| attinyActivitySync_\.fault\(\)\)/);
  assert.doesNotMatch(link, /attinyStatusKnown_ && \(!attinyBatchSynced_ \|\| !attinyActivitySynced_\)/);
  assert.match(link, /attinyLinkChecked_ && !attinyLinkHealthy_/); // E501 is not hidden.
  assert.match(link, /attinyStatusKnown_ && attiny9vLow_/); // E502 unchanged.
  assert.match(link, /attinyBatchSync_\.commandFailed\(\)/);
  assert.match(link, /attinyActivitySync_\.commandFailed\(\)/);
  assert.ok(link.indexOf('if (desiredSiren != attinySirenMirrorOn_') < link.indexOf('if (attinyBatchSync_.pending())'));
});

test('start/stop mark transitions and retain arm-before-disarm handoff', () => {
  const source = read(dir + 'machine_control.h');
  const start = source.slice(source.indexOf('  bool startBatch('), source.indexOf('  bool stopBatch('));
  const stop = source.slice(source.indexOf('  bool stopBatch('), source.indexOf('  void adjustResumeElapsedFromRtc('));
  assert.match(start, /attinyBatchSync_\.expect\(true, millis\(\)\)/);
  assert.match(stop, /attinyBatchSync_\.expect\(false, millis\(\)\)/);
  assert.ok(stop.indexOf('mayapAttinyBusRequest(ATTINY_MSG_ACTIVITY_ON)') < stop.indexOf('mayapAttinyBusRequest(ATTINY_MSG_BATCH_END)'));
  const runner = read('tools/test_runtime_buses.py');
  assert.match(runner, /actual-attiny-controller\.inc/);
  assert.match(runner, /'runtime-attiny-state'/);
});
