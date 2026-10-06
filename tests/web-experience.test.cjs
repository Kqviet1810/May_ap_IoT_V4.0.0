const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const { webcrypto } = require('node:crypto');
const protocol = require('../protocol_v2.js');

function browser(overrides = {}, initialStorage = {}) {
  const source = fs.readFileSync(require.resolve('../app.js'), 'utf8').replace(/  init\(\);\s*\}\)\(\);\s*$/, `
    renderDevice = () => { window.renders.push({ source: currentDevice()?.dataSource, status: connectionStatus(currentDevice()), temperature: currentDevice()?.snapshot?.runtime?.temperature }); };
    feedTelemetrySnapshot = () => {}; renderReminderList = () => {}; renderPushStatus = () => {};
    applyConfigToUi = () => {}; clearInvalid = () => {};
    invalidate = (form, id) => { window.invalidField = id; return false; };
    Object.assign(window.hooks, { state, supportsVentProfile, swipeDestination,
      buildConfig, validateTemperatureForm, validateSensorForm, validateVentForm,
      validateAdvancedForm, REQUIRED_CONFIG_KEYS, VENT_PROFILE_KEYS, createDevice });
  })();`);
  let now = 0, wall = Date.now(), timerId = 0;
  class BrowserDate extends Date { static now() { return wall; } }
  const timers = new Map(), elements = new Map(), clients = [], events = new Map();
  const storage = new Map(Object.entries(initialStorage));
  const window = { hooks: {}, renders: [], addEventListener(name, fn) { events.set(name, fn); }, MayapProtocolV2: protocol,
    MAYAP_WEB_CONFIG: { cloudApiBase:'https://test.invalid', ...overrides } };

  const document = { hidden: false, body: { dataset: { page: 'device' } },
    addEventListener(name, fn) { events.set(name, fn); }, getElementById: id => elements.get(id) };
  const context = { window, document, Date: BrowserDate, crypto: webcrypto, URL, URLSearchParams, TextEncoder, AbortController,
    localStorage: { getItem: key => storage.get(key) || null, setItem: (key, value) => storage.set(key, value) }, console,
    performance: { now: () => now },
    setTimeout(fn, delay) { const id = ++timerId; timers.set(id, { fn, delay, at: wall + delay }); return id; },
    clearTimeout: id => timers.delete(id),
    setInterval(fn, delay) { const id = ++timerId; timers.set(id, { fn, delay, at: wall + delay, interval: true }); return id; },
    clearInterval: id => timers.delete(id) };
  vm.runInNewContext(source, context);
  const h = window.hooks, device = h.createDevice('MAP-1234567890AB', 'Máy thử', 'token');
  h.state.devices = [device]; h.state.selectedId = device.id;
  return { ...h, device, document, elements, window, clients, timers, context, storage, events,
    now: () => wall,
    elapse(ms, mono = ms) { wall += ms; now += mono; },
    correctClock(ms) { wall += ms; },
    tick() { for (const [id, t] of [...timers]) if (timers.has(id) && t.at <= wall) {
      if (t.interval) t.at = wall + t.delay; else timers.delete(id); t.fn();
    } },
    run(delay) { for (const [id, t] of [...timers]) if (!t.interval && t.delay === delay) {
      timers.delete(id); now += delay; wall += delay; t.fn();
    } } };
}

test('advanced UI hides SSR cycle while preserving legacy protocol readback', () => {
  const html = fs.readFileSync(require.resolve('../index.html'), 'utf8');
  assert.doesNotMatch(html, /advPidCycleSec/);
  for (const pidCycleSec of [1, 10, 60]) {
    const h = browser();
    h.device.config = Object.fromEntries(h.REQUIRED_CONFIG_KEYS.map(key => [key, 0]));
    h.device.config.pidCycleSec = pidCycleSec;
    const values = { advKp:18, advKi:0.8, advKd:45, advMaxHeaterPower:100,
      advTempRateLimitC:1, advTempRateWindowSec:120, advTempOscillationCrossLimit:6,
      advTempOscillationWindowSec:600, advHeaterStuckMinRiseC:0.3,
      advHeaterStuckDurationSec:900, advAutotuneRelayPowerPercent:30, advAutotuneBandC:0.2 };
    for (const [id,value] of Object.entries(values)) h.elements.set(id,{value});
    assert.equal(h.validateAdvancedForm(), true);
    assert.equal(h.buildConfig('advanced').pidCycleSec, pidCycleSec);
  }
});

test('adaptive thermal setting is explicit opt-in and omitted for legacy firmware', () => {
  const html=fs.readFileSync(require.resolve('../index.html'),'utf8');
  assert.match(html,/Tự cân bằng nhiệt/);
  assert.match(html,/id="adaptiveThermalBalanceEnabled" type="checkbox" disabled/);
  const webSource=fs.readFileSync(require.resolve('../app.js'),'utf8');
  assert.match(webSource,/unsupportedAdaptive = input\.id === 'adaptiveThermalBalanceEnabled'/);
  const h=browser();h.device.config=Object.fromEntries(h.REQUIRED_CONFIG_KEYS.map(key=>[key,0]));
  const values={advKp:18,advKi:.8,advKd:45,advMaxHeaterPower:100,advTempRateLimitC:1,
    advTempRateWindowSec:120,advTempOscillationCrossLimit:6,advTempOscillationWindowSec:600,
    advHeaterStuckMinRiseC:.3,advHeaterStuckDurationSec:900,advAutotuneRelayPowerPercent:30,advAutotuneBandC:.2};
  for(const [id,value] of Object.entries(values))h.elements.set(id,{value});
  h.elements.set('adaptiveThermalBalanceEnabled',{checked:true});
  assert.equal(Object.hasOwn(h.buildConfig('advanced'),'adaptiveThermalBalanceEnabled'),false);
  h.device.config.adaptiveThermalBalanceEnabled=false;
  assert.equal(h.buildConfig('advanced').adaptiveThermalBalanceEnabled,true);
  h.elements.get('adaptiveThermalBalanceEnabled').checked=false;
  const off=h.buildConfig('advanced');assert.equal(off.adaptiveThermalBalanceEnabled,false);
  assert.equal(off.kp,18);assert.equal(off.ki,.8);assert.equal(off.kd,45);
});

test('fan config requires complete capabilities and preserves legacy schedules', () => {
  const h = browser();
  h.device.config = Object.fromEntries(h.REQUIRED_CONFIG_KEYS.map(key => [key, 0]));
  h.device.config.ventScheduleEnabled = true; h.device.config.highTempAlarm = 39;
  for (const [id, value] of Object.entries({ ventOn: 38, ventOff: 37.8 })) h.elements.set(id, { value });
  assert.equal(h.supportsVentProfile(h.device.config), false);
  const legacy = h.buildConfig('vent');
  assert.equal(legacy.ventScheduleEnabled, true);
  assert.equal(Object.hasOwn(legacy, 'ventAutoEnabled'), false);
  for (const key of h.VENT_PROFILE_KEYS) { h.device.config[key] = 10; h.elements.set(key, { value: 10, checked: true }); }
  h.elements.get('ventProfileLevel').value = 1;
  h.elements.get('ventCycleMinutes').value = 40;
  assert.equal(h.validateVentForm(), true);
  assert.equal(h.buildConfig('vent').ventScheduleEnabled, false);
  h.elements.get('ventCycleMinutes').value = 45;
  assert.equal(h.validateVentForm(), false);
  assert.equal(h.window.invalidField, 'ventCycleMinutes');
});

test('swipe changes adjacent tabs only and rejects vertical, short, slow or edge swipes', () => {
  const h = browser();
  assert.equal(h.swipeDestination('device', -100, 5, 250), 'batch');
  assert.equal(h.swipeDestination('batch', -100, 5, 250), 'settings');
  assert.equal(h.swipeDestination('settings', 100, 5, 250), 'batch');
  for (const args of [['device', 100, 5, 250], ['settings', -100, 5, 250], ['device', -50, 1, 200],
    ['device', -100, 90, 200], ['device', -100, 1, 900]]) assert.equal(h.swipeDestination(...args), null);
});

test('landing has no duplicated top nav and Auto Tune exposes cancel while running', () => {
  const html = fs.readFileSync(require.resolve('../index.html'), 'utf8');
  const css = fs.readFileSync(require.resolve('../landing.css'), 'utf8');
  const app = fs.readFileSync(require.resolve('../app.js'), 'utf8');
  assert.doesNotMatch(html, /<nav class="landingNav"/);
  assert.match(css, /grid-template-rows:minmax\(0,1fr\) auto 30px/);
  assert.match(app, /textContent = 'Hủy tự dò PID'/);
  assert.match(app, /sendCommand\('autotune_cancel'\)/);
});

test('thermal final bounds match firmware and zero-authority PID is rejected', () => {
  const html = fs.readFileSync(require.resolve('../index.html'), 'utf8');
  assert.doesNotMatch(html, /id="sensorTimeout"/);
  assert.match(html, /id="tempOffset" max="5" min="-5"/);
  assert.match(html, /id="highAlarm" max="42"/);
  assert.match(html, /id="emergencyTemp" max="45"/);
  assert.match(html, /Mức tăng nhiệt tối thiểu/);

  const h = browser();
  for (const [id, value] of Object.entries({
    targetTemp:37.5, lowAlarm:36.5, highAlarm:38.2, emergencyTemp:39,
    tempOffset:0, humidityOffset:0,
    advKp:0, advKi:0, advKd:0, advMaxHeaterPower:100,
    advTempRateLimitC:1, advTempRateWindowSec:120,
    advTempOscillationCrossLimit:6, advTempOscillationWindowSec:600,
    advHeaterStuckMinRiseC:0.3, advHeaterStuckDurationSec:900,
    advAutotuneRelayPowerPercent:30, advAutotuneBandC:0.2
  })) h.elements.set(id, { value });
  assert.equal(h.validateTemperatureForm(), true);
  assert.equal(h.validateSensorForm(), true);
  assert.equal(h.validateAdvancedForm(), false);
  assert.equal(h.window.invalidField, 'advKp');

  h.elements.get('advKp').value = 18;
  assert.equal(h.validateAdvancedForm(), true);
  h.elements.get('tempOffset').value = -5.1;
  assert.equal(h.validateSensorForm(), false);
  assert.equal(h.window.invalidField, 'tempOffset');
});

test('running-batch target edit re-anchors safety and ventilation envelope instead of editing locked thresholds', () => {
  const h = browser();
  h.device.config = Object.fromEntries(h.REQUIRED_CONFIG_KEYS.map(key => [key, 0]));
  Object.assign(h.device.config, {
    targetTemp:37.5, lowTempAlarm:36.5, highTempAlarm:38.2, emergencyTemp:39.0,
    ventOnTemp:38.0, ventOffTemp:37.6, highTempAlarmWithoutBatch:true
  });
  h.device.snapshot = { runtime:{ batchRunning:true, resumeConfirmationRequired:false } };
  h.elements.set('targetTemp', { value:30 });
  const cfg = h.buildConfig('temperature');
  assert.equal(cfg.targetTemp, 30);
  assert.ok(Math.abs(cfg.lowTempAlarm - 29.0) < 1e-9);
  assert.ok(Math.abs(cfg.highTempAlarm - 30.7) < 1e-9);
  assert.ok(Math.abs(cfg.emergencyTemp - 31.5) < 1e-9);
  assert.ok(Math.abs(cfg.ventOnTemp - 30.5) < 1e-9);
  assert.ok(Math.abs(cfg.ventOffTemp - 30.1) < 1e-9);
  assert.equal(cfg.highTempAlarmWithoutBatch, true);
});

test('Notes and custom Reminders persist through account-scoped D1 APIs, not device realtime', () => {
  const app=fs.readFileSync(require.resolve('../app.js'),'utf8');
  const notes=fs.readFileSync(require.resolve('../notes.js'),'utf8');
  const worker=fs.readFileSync(require.resolve('../cloudflare/src/account-worker.js'),'utf8');
  assert.doesNotMatch(app,/Chưa có giao thức lưu Nhắc nhở/);
  assert.doesNotMatch(notes,/Chưa có giao thức lưu Ghi chú/);
  assert.match(app,/\/api\/device\/\$\{encodeURIComponent\(device\.id\)\}\/reminders/);
  assert.match(app,/listNotes:listCloudNotes/);
  assert.match(worker,/cloud_notes/);
  assert.match(worker,/cloud_reminders/);
  assert.doesNotMatch(worker,/notes\/request/);
});

test('clean Web baseline has no old realtime script and disables remote commands', () => {
  const html=fs.readFileSync(require.resolve('../index.html'),'utf8');
  const app=fs.readFileSync(require.resolve('../app.js'),'utf8');
  assert.doesNotMatch(html,/realtime_transport\.js/);
  assert.doesNotMatch(app,/connectRealtime|requestRealtimeSession|MayapRealtime\.Client/);
  const h=browser();
  assert.equal(h.state.realtimeConnected,false);
});
