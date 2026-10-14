'use strict';
// Serial HEALTH command: read-only diagnostics, only reachable in diagnostic builds, never touches control.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const dir = path.join(__dirname, '..', 'MAYAP_INDUSTRIAL_v1_0_0');
const read = (f) => fs.readFileSync(path.join(dir, f), 'utf8');

test('HEALTH reports heap, static RAM regions, stacks, cycle times, CPU per core and chip temperature', () => {
  const h = read('system_health.h');
  for (const text of ['heap internal', 'static RAM by region', '.data', '.bss', 'IRAM', 'stack %-5s peak', 'cycle us: control', 'cpu core0=', 'temperatureRead()']) assert.ok(h.includes(text), text);
  assert.match(h, /__attribute__\(\(weak\)\)/);                                   // a missing linker symbol prints n/a instead of breaking the link
  assert.match(h, /esp_register_freertos_idle_hook_for_cpu\(idleHook, 0\)[\s\S]*esp_register_freertos_idle_hook_for_cpu\(idleHook, 1\)/);
  assert.match(h, /esp_deregister_freertos_idle_hook_for_cpu\(idleHook, 0\)[\s\S]*esp_deregister_freertos_idle_hook_for_cpu\(idleHook, 1\)/);
  assert.match(h, /#else  \/\/ host build/);                                          // empty stubs off target
});

test('the CPU sampler exists only for 2 s windows and is started only by HEALTH (diagnostic) or LOG CPU', () => {
  const m = read('machine_control.h');
  assert.match(m, /strcmp\(cmd, "HEALTH"\)/);
  assert.match(m, /MayapHealth::resetPeaks\(\)/);
  const service = m.slice(m.indexOf('void serviceSerial('), m.indexOf('void handleSerialLine('));
  assert.match(service, /#if MAYAP_SERIAL_INPUT_SIM \|\| MAYAP_DIAGNOSTIC_SERIAL[\s\S]*MayapHealth::service\(MayapSerialLog::mode\(\) == MayapSerialLog::Mode::Cpu\);[\s\S]*#elif MAYAP_SERIAL_LOG_PORT/);
  const ino = read('MAYAP_INDUSTRIAL_v1_0_0.ino');
  assert.match(ino, /#if MAYAP_SERIAL_WRITER\n\s*\/\/ Serial HEALTH command and LOG CPU[\s\S]*MayapHealth::setCycleCounters[\s\S]*#endif/);
  for (const task of ['ctrl', 'hmi', 'sup', 'net', 'mqtt', 'cloud', 'ota']) assert.match(ino, new RegExp('MayapHealth::addTask\\("' + task + '"'));
});

test('HEALTH never writes outputs, config or the journal', () => {
  const h = read('system_health.h');
  assert.doesNotMatch(h, /digitalWrite|ledcWrite|Preferences|EEPROM|Wire\.|saveConfig|outputs_|config_/);
});
