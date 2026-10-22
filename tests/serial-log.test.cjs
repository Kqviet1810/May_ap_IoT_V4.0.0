'use strict';
// LOG CPU / LOG AI: structural guarantees (the behaviour itself is tested by tests/serial-log.cpp against the real queue).
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const root = path.join(__dirname, '..');
const dir = path.join(root, 'MAYAP_INDUSTRIAL_v1_0_0');
const read = (f) => fs.readFileSync(path.join(dir, f), 'utf8');
function body(source, signature) {
  const start = source.indexOf(signature);
  assert.notEqual(start, -1, signature);
  const open = source.indexOf('{', start);
  let depth = 0;
  for (let i = open; i < source.length; ++i) {
    if (source[i] === '{') ++depth;
    if (source[i] === '}' && --depth === 0) return source.slice(open + 1, i);
  }
  throw new Error('unclosed ' + signature);
}

test('exactly one log mode exists and a mode change ends the previous one', () => {
  const h = read('serial_log.h');
  assert.match(h, /enum class Mode : uint8_t \{ Off = 0, Machine = 1, Cpu = 2, Ai = 3 \}/);
  assert.equal((h.match(/__atomic_store_n\(&gState\.mode/g) || []).length, 1);          // the only writer of the mode variable, besides the initialiser
  const set = body(h, 'inline void setMode(Mode m)');
  assert.match(set, /mayapSetSerialDebugEnabled\(m == Mode::Machine\)/);
  assert.match(set, /mayapSerialLogDrop\(\)/);
  const m = read('machine_control.h');
  const handler = m.slice(m.indexOf('void handleSerialLine('), m.indexOf('void printSerialHelp('));
  assert.doesNotMatch(handler, /mayapSetSerialDebugEnabled\(/);                          // every switch goes through MayapSerialLog::setMode / apply
  for (const word of ['Command::Cpu', 'Command::Ai', 'Command::Off', 'Mode::Machine']) assert.ok(handler.includes(word), word);
});

test('a production build understands only the whitelist and keeps the generic text log compiled out', () => {
  const h = read('serial_log.h');
  const parse = body(h, 'inline Command parseCommand(const char *line)');
  for (const word of ['LOG CPU', 'LOG AI', 'LOG OFF', 'EXIT']) assert.ok(parse.includes('"' + word + '"'), word);
  assert.equal((parse.match(/is\("/g) || []).length, 4);                                    // nothing else is accepted
  const m = read('machine_control.h');
  const service = body(m, 'void serviceSerial(uint32_t now)');
  assert.match(service, /#if MAYAP_SERIAL_INPUT_SIM \|\| MAYAP_DIAGNOSTIC_SERIAL[\s\S]*handleSerialLine[\s\S]*#elif MAYAP_SERIAL_LOG_PORT\s*\n\s*\(void\)now;\s*\n\s*MayapSerialLog::pollInput\(\);/);
  const d = read('serial_diagnostics.h');
  assert.match(d, /inline void mayapSerialPrintf\(bool force, const char \*format, \.\.\.\) \{\n#if MAYAP_DIAGNOSTIC_SERIAL/);
  assert.match(d, /#define MAYAP_SERIAL_WRITER \(MAYAP_DIAGNOSTIC_SERIAL \|\| MAYAP_SERIAL_LOG_PORT\)/);
  const wf = fs.readFileSync(path.join(root, '.github/workflows/build-firmware.yml'), 'utf8');
  assert.match(wf, /MAYAP_CPP_FLAGS=-DMAYAP_DIAGNOSTIC_SERIAL=0 -DMAYAP_SERIAL_INPUT_SIM=0"/);   // the production profile keeps the diagnostic text log and command set out
  assert.doesNotMatch(wf, /MAYAP_SERIAL_LOG_PORT=0/);
});

test('the control task only copies a snapshot; formatting and writing stay in the supervisor task', () => {
  const m = read('machine_control.h');
  const publish = body(m, 'void publishSerialLog(uint32_t now)');
  assert.match(publish, /^\s*if \(!MayapSerialLog::aiActive\(\)\) return;/);
  assert.doesNotMatch(publish, /snprintf|mayapSerialPrintf|Serial\.|Preferences|digitalWrite|outputs_\.(?!state\(\))/);
  assert.match(m, /publishSerialLog\(now\);\n\s*if \(lastHmiEventSequence_/);
  const ino = read('MAYAP_INDUSTRIAL_v1_0_0.ino');
  assert.match(ino, /mayapSerialDrain\(\);[^\n]*\n\s*MayapSerialLog::service\(now\);/);
  const control = body(ino, 'void controlTask(void *parameter)');
  assert.doesNotMatch(control, /MayapSerialLog::service|formatAiLine/);
  const h = read('serial_log.h');
  assert.doesNotMatch(h, /digitalWrite|ledcWrite|Preferences|EEPROM|Wire\.|outputs_|config_/);   // reads nothing but the snapshot, writes nothing but the queue
});

test('self-protection and the LOG CPU sampler cannot be left running', () => {
  const h = read('serial_log.h');
  assert.match(h, /GUARD_CYCLE_US = 100000U/);
  assert.match(h, /AUTO-OFF: slow control cycle/);
  assert.match(h, /AUTO-OFF: low supervisor stack/);
  const svc = body(h, 'inline void service(uint32_t now)');
  assert.ok(svc.indexOf('MayapHealth::service(m == Mode::Cpu)') < svc.indexOf('if (m != Mode::Cpu && m != Mode::Ai)'));   // a running CPU sample is always finished, in any mode
  const sh = read('system_health.h');
  assert.match(sh, /esp_deregister_freertos_idle_hook_for_cpu\(idleHook, 0\)/);
  assert.match(sh, /if \(!logAllowed\) return;/);
});

test('the AI log carries the columns promised in the documentation', () => {
  const h = read('serial_log.h');
  const cols = h.match(/AI_COLUMNS\[\] =\s*\n?\s*"([^"]+)\\n"/)[1].split(',');
  assert.equal(cols.length, 33);
  for (const c of ['seq', 't', 'age', 'sp', 'pv', 'raw', 'req', 'ssrms', 'avg60', 'fl', 'integ', 'phase', 'fits', 'kx1e3', 'al', 'fc', 'drop', 'crc']) assert.ok(cols.includes(c), c);
  const doc = fs.readFileSync(path.join(root, 'doc/SERIAL_LOG.md'), 'utf8');
  for (const c of cols) assert.ok(doc.includes('`' + c + '`'), 'doc: ' + c);
});
