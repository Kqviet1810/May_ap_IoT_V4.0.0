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
    ['cloudTask', 'mayapCloudAlertBegin']])
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
  assert.match(hmi, /mayapBootHomeReleased\(\) \|\| \(!mayapBootDiagnosticActive\(\) && elapsed >= SPLASH_MAX_MS\)/);
});

test('Supervisor admission gates retain fatal thresholds and persist reason before TWDT fallback', () => {
  const supervisor = body(ino, 'void supervisorTask(');
  for (const needle of ['controlExpected', 'hmiExpected', 'CONTROL_HEARTBEAT_TIMEOUT_MS', 'HMI_FATAL_HEARTBEAT_TIMEOUT_MS',
    'CONTROL_CYCLE_TRIP_COUNT', 'HMI_CYCLE_TRIP_COUNT', 'vTaskSuspend(controlTaskHandle)', 'mayapSafeOutputsEarly()'])
    assert.ok(supervisor.includes(needle), needle);
  assert.ok(supervisor.indexOf('mayapBootPlanRestart(') < supervisor.indexOf('while (elapsedMs'));
  assert.match(body(ino, 'void networkTask('), /mayapBootStage\(\) < MayapBoot::Stage::Ota[\s\S]*mayapSetWifiPortalOtaQuiesced\(true\)/);
});

test('all explicit restarts are reasoned and the web OTA restart has a marker', () => {
  for (const file of fs.readdirSync(path.resolve(__dirname, '..', dir)).filter((f) => /\.(h|ino)$/.test(f))) {
    if (file === 'boot_diagnostic.h') continue;
    const source = read(dir + file).replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');
    assert.doesNotMatch(source, /\b(?:esp_restart|ESP\.restart)\s*\(/, file);
  }
  assert.match(diagnostic, /mayapBootPlanRestart\(reason, detail\);\s*esp_restart\(\)/);
  assert.match(read(dir + 'ota_web_update.h'), /mayapRestart\(MayapBoot::RestartReason::InternetOta/);
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

test('Home is released only by the local gate: valid stable sensor + every local safety condition, else a 30 s diagnostic', () => {
  const stability = body(ino, 'static void updateBootStability(');
  assert.match(stability, /homeGate\.update\(now, in\)/);
  for (const field of ['tasksHealthy', 'displayHealthy', 'storageBlocked', 'tripLatched', 'sensorUsable', 'sensorBootReason'])
    assert.match(stability, new RegExp(`in\\.${field}`), field);
  // the old "tasks stable for N ms" Home rule is gone: Home needs the gate
  assert.doesNotMatch(stability, /localTaskStability\.held\(now, homeDelay\)/);
  assert.equal((ino.match(/mayapBootReleaseHome\(\)/g) || []).length, 1);
  assert.match(stability, /Phase::Diagnostic[\s\S]*mayapBootPublishDiagnostic\(MayapBoot::blockCode\(why\), why, homeGate\.age\(now\)\)/);
  // a diagnostic is never reported as ready: the operator override does not call mayapBootShowReady()
  const override = stability.slice(stability.indexOf('mayapBootHomeRequested()'), stability.indexOf('} else {', stability.indexOf('mayapBootHomeRequested()')));
  assert.doesNotMatch(override, /mayapBootShowReady/);
  const policy = read(dir + 'boot_policy.h');
  assert.match(policy, /BOOT_DEADLINE_MS = 30000U/);
  assert.match(policy, /READY_HOLD_MS = 1500U/);
  // the control task publishes the verdicts the gate reads (no struct shared across tasks)
  const control = body(ino, 'void controlTask(');
  for (const v of ['bootSensorUsable', 'bootSensorReason', 'bootStorageBlocked']) assert.ok(control.includes(v), v);
  // HMI: input on the diagnostic screen is only "continue"; nothing else is accepted during the splash
  assert.match(body(hmi, 'void handleInput()'), /mayapBootDiagnosticActive\(\) && rotary\.button == ButtonEvent::ShortPress\) mayapBootRequestHome\(\)/);
  assert.match(read(dir + 'config.h'), /SENSOR_RECOVERY_GOOD_SAMPLES = 3UL/);   // the simulation's "usable" rule mirrors this constant
});

test('Wi-Fi has one decision-maker; MQTT, Cloud and OTA can never touch the radio; power mode stays PERFORMANCE', () => {
  const net = read(dir + 'network_service.h');
  assert.match(net, /#include "wifi_fsm\.h"/);
  assert.match(body(net, 'inline void mayapNetworkUpdate('), /wifiFsm\.update\(now, associated, staDisconnectAt, wifiJitterMs\(\)\)/);
  assert.doesNotMatch(net, /staBackoff|deepPolicy|mayapRequestWifiDeepRecovery|DeepPhase::Isolated/);
  assert.doesNotMatch(ino, /mayapRequestWifiDeepRecovery/);
  // the heavy owner-drain transaction starts for exactly two reasons
  const deep = body(net, 'inline bool mayapNetworkDeepRecoveryUpdate(');
  assert.match(deep, /wifiFsm\.state\(\) != MayapNetwork::WifiState::Recovery\) return false/);
  assert.match(deep, /explicitStop/);
  // no MQTT / Cloud / OTA source can call the Wi-Fi driver or the recovery machinery
  for (const file of ['mqtt_transport.h', 'mqtt_uplink.h', 'mqtt_ws.h', 'mqtt_wire.h', 'transaction_bridge.h', 'cloud_alert_link.h', 'ota_web_update.h']) {
    const source = read(dir + file).replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');
    assert.doesNotMatch(source, /\bWiFi\.(begin|reconnect|disconnect|mode|setSleep|setAutoReconnect)\b|esp_wifi_(stop|start|connect|disconnect|restore|deinit|set_ps)|wifiFsm|mayapRadioQuiesceBegin\(/, file);
  }
  // the station is (re)started in exactly these places
  const netCode = net.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');
  const connectSites = [...netCode.matchAll(/WiFi\.(?:begin|reconnect)\(/g)].length;
  assert.equal(connectSites, 6, 'station start (1) + portal test (1) + RECOVERY (2) + light reconnect (2)');
  // PERFORMANCE: modem sleep stays behind MAYAP_WIFI_ECO (default 0); nothing else sets a power-save mode
  assert.match(read(dir + 'wifi_power_policy.h'), /#define MAYAP_WIFI_ECO 0/);
  assert.equal([...net.matchAll(/esp_wifi_set_ps\(/g)].length, 1);
  for (const file of fs.readdirSync(path.resolve(__dirname, '..', dir)).filter((f) => /\.(h|ino)$/.test(f)))
    if (file !== 'network_service.h') assert.doesNotMatch(read(dir + file), /esp_wifi_set_ps|WiFi\.setSleep/, file);
});
