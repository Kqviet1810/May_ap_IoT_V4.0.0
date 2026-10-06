const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const read = (name) => fs.readFileSync(path.resolve(__dirname, '..', name), 'utf8');
const dir = 'MAYAP_INDUSTRIAL_v1_0_0/';
const ino = read(dir + 'MAYAP_INDUSTRIAL_v1_0_0.ino');
const diagnostic = read(dir + 'boot_diagnostic.h');
const hmi = read(dir + 'hmi.h');

function body(source, signature) {
  const start = source.indexOf(signature);
  assert.notEqual(start, -1, signature);
  const open = source.indexOf('{', start);
  let depth = 0;
  for (let i = open; i < source.length; ++i) {
    if (source[i] === '{') ++depth;
    if (source[i] === '}' && --depth === 0) return source.slice(open + 1, i);
  }
  throw new Error(`Unclosed ${signature}`);
}

test('setup keeps outputs safe and yields to a staged coordinator without network startup', () => {
  const setup = body(ino, 'void setup()');
  assert.ok(setup.indexOf('mayapSafeOutputsEarly()') < setup.indexOf('mayapBootDiagnosticBegin()'));
  assert.doesNotMatch(setup, /mayap(?:Network|WebLink|CloudAlert|Ota)Begin\(|xTaskCreate/);
  assert.doesNotMatch(body(ino, 'static void stagedStartupUpdate('), /\bdelay\(/);
  assert.match(body(ino, 'void loop()'), /stagedStartupUpdate/);
  assert.match(setup, /wdtConfig\.timeout_ms = CONTROL_WDT_TIMEOUT_MS/);
  assert.match(setup, /wdtConfig\.trigger_panic = true/);
});

test('admission follows local, Wi-Fi, MQTT, Cloud, OTA order with owner-only initialization', () => {
  const coordinator = body(ino, 'static void stagedStartupUpdate(');
  const stages = ['Storage', 'SensorMachine', 'Hmi', 'ControlSafety', 'LocalSettle', 'Wifi', 'Mqtt', 'Cloud', 'Ota', 'Running'];
  let last = -1;
  for (const stage of stages) {
    const pos = coordinator.indexOf(`case Stage::${stage}:`);
    assert.ok(pos > last, stage);
    last = pos;
  }
  for (const [task, begin] of [['networkTask', 'mayapNetworkBegin'], ['mqttTask', 'mayapRealtimeBegin'],
    ['cloudTask', 'mayapCloudAlertBegin'], ['otaTask', 'mayapOtaBegin']])
    assert.match(body(ino, `void ${task}(`), new RegExp(`${begin}\\(\\)`));
  // Deferred initialization must not overwrite an Online configuration read from EEPROM.
  assert.doesNotMatch(body(read(dir + 'network_service.h'), 'inline void mayapNetworkBegin()'), /__atomic_store_n\(&requestedMode/);
});

test('Home and boot success depend on local stability, never server connectivity', () => {
  const stability = body(ino, 'static void updateBootStability(');
  assert.doesNotMatch(stability, /WiFi|mqttConnected|networkReady|mayapGetNetworkStatus|mqtt\.connected/);
  assert.match(stability, /localSuccessStability\.held\(now, MayapBoot::SUCCESS_STABLE_MS\)/);
  assert.match(stability, /sensorHealthy/);
  assert.match(stability, /displayHealthy/);
  assert.match(body(ino, 'static bool localTasksHealthy('), /supervisorHeartbeatMs/);
  assert.match(hmi, /mayapBootHomeReleased\(\) \|\| elapsed >= SPLASH_MAX_MS/);
});

test('Supervisor admission gates retain fatal thresholds and persist reason before TWDT fallback', () => {
  const supervisor = body(ino, 'void supervisorTask(');
  for (const needle of ['controlExpected', 'hmiExpected', 'CONTROL_HEARTBEAT_TIMEOUT_MS', 'HMI_FATAL_HEARTBEAT_TIMEOUT_MS',
    'CONTROL_CYCLE_TRIP_COUNT', 'HMI_CYCLE_TRIP_COUNT', 'vTaskSuspend(controlTaskHandle)', 'mayapSafeOutputsEarly()'])
    assert.ok(supervisor.includes(needle), needle);
  assert.ok(supervisor.indexOf('mayapBootPlanRestart(') < supervisor.indexOf('while (elapsedMs'));
  assert.match(body(ino, 'void networkTask('), /mayapBootStage\(\) < MayapBoot::Stage::Ota[\s\S]*mayapSetWifiPortalOtaQuiesced\(true\)/);
});

test('all explicit restarts are reasoned and ArduinoOTA automatic restart has a marker', () => {
  for (const file of fs.readdirSync(path.resolve(__dirname, '..', dir)).filter((f) => /\.(h|ino)$/.test(f))) {
    if (file === 'boot_diagnostic.h') continue;
    const source = read(dir + file).replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');
    assert.doesNotMatch(source, /\b(?:esp_restart|ESP\.restart)\s*\(/, file);
  }
  assert.match(diagnostic, /mayapBootPlanRestart\(reason, detail\);\s*esp_restart\(\)/);
  assert.match(read(dir + 'ota_update.h'), /mayapBootPlanRestart\(MayapBoot::RestartReason::ArduinoOta/);
  assert.match(diagnostic, /RTC_DATA_ATTR/);
  assert.match(diagnostic, /RTC_NOINIT_ATTR/);
  assert.doesNotMatch(diagnostic, /EEPROM|Preferences/);
});

test('LCD splash renders centered logo and only three dots as requested', () => {
  const splash = body(hmi, 'void drawSplash()');
  assert.equal((splash.match(/drawXBMP\(/g) || []).length, 1);
  assert.equal((splash.match(/drawDisc\(/g) || []).length, 3);
  assert.doesNotMatch(splash, /bootStatusBits|mayapBootStatus/);
  assert.doesNotMatch(splash, /drawStr|drawCenteredText|drawHeader|drawToast/);
  const assets = read(dir + 'boot_assets.h');
  assert.doesNotMatch(assets, /bootStatusBits|bootStatus[0-7]/);
});
