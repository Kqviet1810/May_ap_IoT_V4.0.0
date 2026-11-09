// The day of a batch is the machine's own counter (batch_day.h): the wiring of the start-from-day / set-day commands through firmware, HMI and Web,
// and the date <-> day arithmetic of the Web form.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const read = (name) => fs.readFileSync(path.resolve(__dirname, '..', name), 'utf8').replace(/\r\n/g, '\n');
const fw = (name) => read('MAYAP_INDUSTRIAL_v1_0_0/' + name);

test('firmware: BatchSetDay is an appended command, mapped from batch_set_day, carried as alarmMask and checked against the total days', () => {
  const config = fw('config.h');
  assert.match(config, /TurnMechanicalCheck,[^\n]*\n\s*BatchSetDay/);                       // appended: no older command changes its number
  const bridge = fw('transaction_bridge.h');
  assert.match(bridge, /"batch_set_day"\)\) return HmiCommandType::BatchSetDay/);
  assert.match(bridge, /type == HmiCommandType::BatchStart \|\| type == HmiCommandType::BatchSetDay/);
  assert.match(bridge, /alarmMaskParam = day;/);
  const machine = fw('machine_control.h');
  assert.match(machine, /#include "batch_day.h"/);
  assert.match(machine, /case HmiCommandType::BatchSetDay:\s*ok = setBatchDay\(now, command\.alarmMask, message\)/);
  assert.match(machine, /startBatch\(now, message, static_cast<uint8_t>\(std::min<uint32_t>\(command\.alarmMask, 255U\)\)\)/);
  const start = machine.slice(machine.indexOf('  bool startBatch('), machine.indexOf('  // Moves the day counter of the RUNNING batch'));
  assert.match(start, /batchday::valid\(startDay, config_\.totalIncubationDays\)/);
  assert.match(start, /elapsedBeforeStartSec_ = batchday::startElapsed\(startDay\)/);
  const set = machine.slice(machine.indexOf('  bool setBatchDay('), machine.indexOf('  bool stopBatch('));
  assert.match(set, /if \(!batchRunning_\)/);
  assert.match(set, /resumePending_/);
  assert.match(set, /batchday::valid\(day, config_\.totalIncubationDays\)/);
  assert.match(set, /elapsedBeforeStartSec_ = after;\s*batchStartedAt_ = now;/);
  assert.match(set, /saveBatchRecord\(\)/);                                                   // written to the EEPROM at once, not at the next checkpoint
  assert.match(set, /setBatchOverdueAcknowledged\(false\)/);
});

test('HMI: the batch screen has a fifth parameter (day); starting sends the chosen day once, a running batch gets BatchSetDay', () => {
  const hmi = fw('hmi.h');
  assert.match(hmi, /a == HmiCommandType::BatchSetDay/);                                      // the three batch commands never queue together
  assert.match(hmi, /BATCH_PARAM_COUNT = 5/);
  assert.match(hmi, /batchParamIndex == 4\) saveBatchDay\(\)/);
  assert.match(hmi, /queueCommand\(HmiCommandType::BatchSetDay, COMMAND_DEFAULT_VALID_MS, 0, day\)/);
  assert.match(hmi, /starting \? static_cast<uint32_t>\(batchStartDay\) : AlarmNone/);
  assert.match(hmi, /if \(starting\) batchStartDay = 1;/);
  assert.match(hmi, /TU NGAY %u\/%u/);
});

test('Web: the day field, its two-way link with the start date, and the commands', () => {
  const html = read('index.html');
  assert.ok(!/id="batchDay"/.test(html), "the day is derived from the start date: no separate input");
  const app = read('app.js');
  assert.match(app, /sendCommand\('batch_set_day', \{ arg0: day \}\)/);
  assert.match(app, /sendCommand\('batch_start', batchDayInput\(\) > 1 \? \{ arg0: batchDayInput\(\) \} : \{\}\)/);
  assert.match(app, /pending\.action === 'batch_set_day'/);
  assert.match(app, /'NGAY AP > SO NGAY'/);
  assert.match(app, /'DA DOI NGAY AP'/);
  assert.match(app, /syncBatchDayFields\(currentDevice\(\)\)/);
  assert.match(read('tools/e2e/device_emulator.cjs'), /'batch_set_day'/);
});

test('Web: the date <-> day arithmetic (Vietnam date, whole days)', () => {
  const app = read('app.js');
  const grab = (name) => { const m = new RegExp(`  function ${name}\\([^)]*\\) \\{[\\s\\S]*?\\n  \\}\\n`).exec(app); assert.ok(m, name); return m[0]; };
  const src = ['isoToDayNumber', 'dayNumberToIso', 'startIsoFromBatchDay', 'batchDayFromStartIso'].map(grab).join('\n');
  const make = (today) => new Function('vnDateIso', `${src}\nreturn { startIsoFromBatchDay, batchDayFromStartIso };`)(() => today);
  const a = make('2026-11-05');
  assert.equal(a.startIsoFromBatchDay(1), '2026-11-05');
  assert.equal(a.startIsoFromBatchDay(4), '2026-11-02');                                    // the screenshot of the report: 02/11 is day 4 on 05/11
  assert.equal(a.batchDayFromStartIso('2026-11-02'), 4);
  assert.equal(a.batchDayFromStartIso('2026-11-05'), 1);
  assert.ok(a.batchDayFromStartIso('2026-11-06') < 1);                                      // a start date in the future is not a day
  const b = make('2026-03-02');                                                             // across a month end, February of a normal year
  assert.equal(b.startIsoFromBatchDay(5), '2026-02-26');
  assert.equal(b.batchDayFromStartIso('2026-02-26'), 5);
  for (let d = 1; d <= 45; ++d) assert.equal(a.batchDayFromStartIso(a.startIsoFromBatchDay(d)), d);
});
