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

test('3.1.2: the profile / scheduled vent goes through ONE hold rule; forced vents and a Web command do not', () => {
  const bridge = fs.readFileSync(path.join(root, 'MAYAP_INDUSTRIAL_v1_0_0', 'transaction_bridge.h'), 'utf8');
  const web = fs.readFileSync(path.join(root, 'app.js'), 'utf8');
  // one rule, one place: the old warm-up variables are gone
  assert.doesNotMatch(control, /warmupVent/);
  assert.doesNotMatch(config, /WARMUP_VENT/);
  assert.match(config, /VENT_HOLD_ENTER_ERR_C = 0\.3f/);
  assert.match(config, /VENT_HOLD_RELEASE_ERR_C = 0\.2f/);
  assert.match(control, /ventHold_\.update\(now, batchRunning_, sensorUsable_, config_\.targetTemp - temperature_, thermalV3_\.controller\(\)\.doorDetected\(\), hp\)/);
  assert.match(control, /if \(ventHold_\.holding\(\)\) \{ scheduledVentActive = false; profileVentActive = false; \}/);
  // the hold only silences the profile and the schedule: forced vents (and a Web command) are decided after it, untouched
  assert.match(control, /req\.ventFanForceOn = emergencyActive_ \|\| highTemperatureActive_ \|\|\s*ventTemperatureActive_ \|\| faults_\.ventForced\(\) \|\| sensorFaultNeedsFan;/);
  assert.match(control, /webOverrideLive\(webVentUntil_, now\) \? webVentValue_ : \(profileVentActive \|\| scheduledVentActive\)/);
  // a Web button sends the VALUE it wants (arg0: 1 off, 2 on, 3 automatic); the machine reads it and still understands the old flip (0)
  for (const action of ['heater_toggle', 'vent_toggle', 'circ_toggle', 'siren_toggle']) assert.match(web, new RegExp(`sendCommand\\('${action}', \\{ device, arg0:`), action);
  assert.match(web, /const WANT = \{ off: 1, on: 2, auto: 3 \};/);
  assert.match(bridge, /alarmMaskParam = static_cast<uint32_t>\(doc\["arg0"\] \| 0U\) & 3U;/);
  assert.match(control, /const uint8_t want = static_cast<uint8_t>\(command\.alarmMask & 3U\);/);
  assert.match(control, /webVentValue_ = want == 0U \? !o\.ventFan : \(want == 2U\);/);
  assert.match(control, /const bool circWanted = want == 0U \? !o\.circulationFan : \(want == 2U\);/);
  assert.match(control, /const bool heaterOff = want == 0U \? !webHeaterOff_ : \(want == 1U\);/);
  // the dangerous refusal stays: a circulation fan beside a hot heater is never switched off from the Web
  assert.match(control, /!circWanted && o\.circulationFan && \(o\.heaterSsr \|\| o\.heatMaster \|\| temperature_ > 38\.0f\)/);
});
