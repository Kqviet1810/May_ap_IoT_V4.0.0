const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const config = fs.readFileSync(path.join(root, 'MAYAP_INDUSTRIAL_v1_0_0', 'config.h'), 'utf8');
const control = fs.readFileSync(path.join(root, 'MAYAP_INDUSTRIAL_v1_0_0', 'machine_control.h'), 'utf8');

test('ventilation defaults cover all incubation stages and preserve GPIO13', () => {
  for (const [field, duty] of [
    ['ventDutyDay1To3', 10], ['ventDutyDay4To7', 15],
    ['ventDutyDay8To11', 25], ['ventDutyDay12To15', 35],
    ['ventDutyDay16To18', 50], ['ventDutyDay19To21', 70],
  ]) assert.match(config, new RegExp(`uint8_t ${field} = ${duty};`));
  assert.match(config, /ventCycleMinutes = 40;/);
  assert.match(config, /RELAY_VENT_MIN_ON_MS = 120000UL/);
  assert.match(config, /RELAY_VENT_MIN_OFF_MS = 120000UL/);
  assert.match(config, /PIN_OUT_VENT_FAN\s*=\s*13/);
});

test('thermal safety overrides profile and RH, while manual output test remains immediate', () => {
  assert.match(control, /return rhLow \? std::min<uint8_t>\(duty, 5U\) : duty/);
  assert.match(control, /req\.ventFanForceOn = emergencyActive_ \|\| highTemperatureActive_ \|\|/);
  assert.match(control, /req\.ventFan = req\.ventFanForceOn \|\| automaticVent/);
  assert.match(control, /request\.ventFanBypassTiming[\s\S]*?setImmediate\(PIN_OUT_VENT_FAN/);
  assert.match(control, /req\.ventFanBypassTiming = true/);
});
