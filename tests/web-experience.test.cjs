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
      validateAdvancedForm, REQUIRED_CONFIG_KEYS, VENT_PROFILE_KEYS, createDevice,
connectionStatus, pollServerAlerts, syncServerAlertPolling, serverConnectionDetail, renderServerAlarmBanner, setRealtimeStatus, SERVER_ALERT,
      buildTechFields, techGateInfo, pmValues, PM_ITEMS, TECH_FIELDS, AUTOTUNE_PHASES, AUTOTUNE_REASONS });
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
    h.elements.set('lightAfterBatchAlarmEnabled', { checked: false }); h.elements.set('sirenSelfTestEnabled', { checked: false });
    assert.equal(h.buildConfig('lightAlarm').pidCycleSec, pidCycleSec);         // echoed untouched
    const values = { advKp:18, advKi:0.8, advKd:45, advMaxHeaterPower:100,
      advTempRateLimitC:1, advTempRateWindowSec:120, advTempOscillationCrossLimit:6,
      advTempOscillationWindowSec:600, advHeaterStuckMinRiseC:0.3,
      advHeaterStuckDurationSec:900, advAutotuneRelayPowerPercent:30, advAutotuneBandC:0.2,
      tempOffset:0, humidityOffset:0 };
    for (const [id,value] of Object.entries(values)) h.elements.set(id,{value});
    assert.equal(h.validateAdvancedForm(), true);
    assert.equal(Object.hasOwn(h.buildTechFields(h.device), 'pidCycleSec'), false);   // never a technical request field
  }
});

test('technical fields never travel through config/set: the Web only builds a tech_request of the changed fields', () => {
  const webSource = fs.readFileSync(require.resolve('../app.js'), 'utf8');
  assert.doesNotMatch(webSource, /group === 'advanced'|group === 'sensor'|sendConfig\('advancedForm'|sendConfig\('sensorForm'/);
  assert.match(webSource, /sendCommand\('tech_request', \{ extra: body/);
  assert.match(webSource, /sendCommand\('tech_unlock', \{ extra: \{ pin \} \}\)/);
  assert.match(webSource, /if \(options\.extra\) Object\.assign\(payload, options\.extra\)/);
  const h = browser();
  h.device.config = Object.fromEntries(h.REQUIRED_CONFIG_KEYS.map(key => [key, 0]));
  Object.assign(h.device.config, { kp: 18, ki: 0.8, kd: 45, maxHeaterPower: 100, tempOffset: 0.1, humidityOffset: 0,
    heaterStuckMinRiseC: 0.3, heaterStuckDurationSec: 900, tempRateLimitC: 1, tempRateWindowSec: 120,
    tempOscillationCrossLimit: 6, tempOscillationWindowSec: 600, autotuneRelayPowerPercent: 30, autotuneBandC: 0.2 });
  for (const [key, id] of h.TECH_FIELDS) h.elements.set(id, { value: h.device.config[key] });
  assert.deepEqual(JSON.parse(JSON.stringify(h.buildTechFields(h.device))), {});                         // nothing changed -> nothing to send
  h.elements.get('advKp').value = 20.5;
  h.elements.get('tempOffset').value = 0.1000000001;                          // float noise is not a change
  assert.deepEqual(JSON.parse(JSON.stringify(h.buildTechFields(h.device))), { kp: 20.5 });
});

test('adaptive thermal setting is explicit opt-in and omitted for legacy firmware', () => {
  const html=fs.readFileSync(require.resolve('../index.html'),'utf8');
  assert.match(html,/Tự cân bằng nhiệt/);
  assert.match(html,/id="adaptiveThermalBalanceEnabled" type="checkbox" disabled/);
  const webSource=fs.readFileSync(require.resolve('../app.js'),'utf8');
  assert.match(webSource,/unsupportedAdaptive = input\.id === 'adaptiveThermalBalanceEnabled'/);
  const h=browser();h.device.config=Object.fromEntries(h.REQUIRED_CONFIG_KEYS.map(key=>[key,0]));
  for (const [key, id] of h.TECH_FIELDS) h.elements.set(id, { value: h.device.config[key] });
  h.elements.set('adaptiveThermalBalanceEnabled',{checked:true});
  assert.equal(Object.hasOwn(h.buildTechFields(h.device),'adaptiveThermalBalanceEnabled'),false);   // legacy firmware: omitted
  h.device.config.adaptiveThermalBalanceEnabled=false;
  assert.equal(h.buildTechFields(h.device).adaptiveThermalBalanceEnabled,1);
  h.elements.get('adaptiveThermalBalanceEnabled').checked=false;
  assert.equal(Object.hasOwn(h.buildTechFields(h.device),'adaptiveThermalBalanceEnabled'),false);
});

test('technical section is hidden unless the HMI grants it; unlocked only with a live device session', () => {
  const h = browser();
  assert.deepEqual({ ...h.techGateInfo(undefined) }, { allowed: false, session: false, text: '' });                 // old firmware
  assert.deepEqual({ ...h.techGateInfo({ tech: {} }) }, { allowed: false, session: false, text: '' });
  const hidden = h.techGateInfo({ tech: { allowed: false, session: true } });                                      // hidden wins over any stale session
  assert.equal(hidden.allowed, false); assert.equal(hidden.session, false); assert.match(hidden.text, /Quyền Web PID/);
  const locked = h.techGateInfo({ tech: { allowed: true, session: false } });
  assert.equal(locked.allowed, true); assert.equal(locked.session, false);
  assert.equal(h.techGateInfo({ tech: { allowed: true, session: true } }).session, true);
  const html = fs.readFileSync(require.resolve('../index.html'), 'utf8');
  assert.match(html, /<details class="settingCard" id="techCard" hidden>/);                                        // hidden by default
  assert.match(html, /id="techPin"[^>]*type="password"/);                                                          // PIN never echoed
  assert.match(html, /maxlength="4"/);
  const webSource = fs.readFileSync(require.resolve('../app.js'), 'utf8');
  assert.doesNotMatch(webSource, /localStorage[^;\n]*techPin|(?:setItem|console\.\w+)\([^)]*\bpin\b/);             // PIN is not stored or logged
});

test('PID Monitor shows measured values only and lists every required figure', () => {
  const h = browser();
  const keys = h.PM_ITEMS.map((item) => item[0]);
  for (const key of ['pv', 'sp', 'err', 'corr', 'hold', 'vent', 'limit', 'req', 'act', 'kp', 'ki', 'kd', 'conf', 'gain', 'delay', 'coast', 'holdPct', 'ventGain'])
    assert.ok(keys.includes(key), key);
  const none = h.pmValues({ config: { kp: 18, ki: 0.8, kd: 45 } }, { temperature: 37.5 });          // firmware without pidMon: nothing invented
  for (const key of ['pv', 'sp', 'err', 'corr', 'hold', 'vent', 'req', 'act', 'conf', 'gain']) assert.ok(Number.isNaN(none[key]), key);
  assert.equal(none.limit, ''); assert.equal(none.kp, 18);
  const live = h.pmValues({ config: {} }, { temperature: 37.4, adaptiveThermal: { enabled: true },
    pidMon: { valid: true, sp: 37.5, err: 0.1, corr: 3, hold: 8, vent: 0, lo: 0, hi: 60, req: 11, act: 10.5, conf: 80, gain: 0.02, delay: 30, coast: 0.2, holdPct: 8, ventGain: 0.1 } });
  assert.equal(live.pv, 37.4); assert.equal(live.sp, 37.5); assert.equal(live.limit, '0–60'); assert.equal(live.conf, 80);
  const noLearn = h.pmValues({ config: {} }, { temperature: 37.4, pidMon: { valid: true, sp: 37.5, err: 0.1, corr: 3, hold: 8, vent: 0, lo: 0, hi: 60, req: 11, act: 10.5, conf: 0, gain: 0, delay: 0, coast: 0, holdPct: 0, ventGain: 0 } });
  assert.ok(Number.isNaN(noLearn.conf));                                                           // adaptive profile off -> no fake zeros
  const webSource = fs.readFileSync(require.resolve('../app.js'), 'utf8');
  assert.match(webSource, /\.\.\.\(mon \? \{ mon: true \} : \{\}\)/);                              // lease asks for pidMon only while the card is open
  assert.equal(h.AUTOTUNE_PHASES.length, 15); assert.equal(h.AUTOTUNE_REASONS.length, 23);
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

test('realtime lost: alarm state comes from the Worker over HTTPS, kept apart from "machine lost", and resyncs without stale data', async () => {
  const h = browser(), calls = [];
  const banner = { hidden: true, textContent: '', children: [], replaceChildren() { this.children = []; }, append(...n) { this.children.push(...n); } };
  h.elements.set('serverAlarmBanner', banner);
  h.document.createElement = () => ({ textContent: '', children: [], append() {} });
  h.device.snapshot = { runtime: {} }; h.device.snapshotAt = h.now(); h.device.dataSource = 'live';
  let reply = null;
  h.context.fetch = async (url) => { calls.push(String(url)); if (!reply) throw new Error('offline');
    return { ok: true, status: 200, json: async () => reply() }; };
  const serverReply = (lastSeenAgoMs, alarms) => () => ({ success: true, exists: true, status: 'online', server_time: h.now(),
    last_seen: h.now() - lastSeenAgoMs, alarms, events: [] });
  const poll = async () => { await h.pollServerAlerts(); };

  // Realtime up: no polling at all and no server copy.
  h.state.realtimeConnected = true; h.setRealtimeStatus('ready', 'ok');
  assert.equal(h.state.serverAlertTimer, 0);
  assert.equal(calls.length, 0);

  // Realtime drops: nothing changes for the first moments (debounce), then one poll is scheduled, not before.
  h.state.realtimeConnected = false; h.setRealtimeStatus('connecting', '...');
  assert.ok(h.state.serverAlertTimer);
  assert.equal(h.connectionStatus(h.device), 'cache');                    // unchanged until the server answered
  // The Web itself cannot reach the Worker: NO conclusion about the machine.
  reply = null; h.elapse(h.SERVER_ALERT.firstDelayMs + 1);
  await poll();
  assert.equal(h.device.server.ok, false);
  assert.equal(h.connectionStatus(h.device), 'cache');
  assert.match(calls[0], /\/api\/device\/MAP-1234567890AB\/status\?alarms=1$/);

  // Worker answers: the machine reported 30 s ago and has a fault -> "mất realtime", NOT "máy mất kết nối"; the fault is shown.
  reply = serverReply(30000, [{ alarm_type: 'FAULT_130', message: 'Quá nhiệt', since: 1, updated: 2 }, { alarm_type: 'DEVICE_OFFLINE', message: 'x' }]);
  await poll();
  assert.equal(h.connectionStatus(h.device), 'norealtime');
  assert.doesNotMatch(h.serverConnectionDetail(h.device, 'norealtime'), /Internet|mất mạng|ngoại tuyến/i);
  h.renderServerAlarmBanner(h.device);
  assert.equal(banner.hidden, false);
  assert.equal(banner.children.length >= 3, true);                         // title + FAULT_130 line (DEVICE_OFFLINE excluded) + source note
  assert.ok(h.state.serverAlertTimer);                                     // keeps polling while realtime is down

  // The Worker has heard nothing for > 3 min on any channel -> "máy mất kết nối".
  reply = serverReply(200000, []);
  await poll();
  assert.equal(h.connectionStatus(h.device), 'devicelost');
  h.renderServerAlarmBanner(h.device);
  assert.equal(banner.hidden, true);

  // An answer that arrives AFTER realtime came back is dropped (no stale/duplicate alarm view).
  reply = serverReply(1000, [{ alarm_type: 'FAULT_130', message: 'old' }]);
  const late = poll();
  h.state.realtimeConnected = true; h.setRealtimeStatus('ready', 'ok');
  await late;
  assert.equal(h.device.server, null);
  assert.equal(h.state.serverAlertTimer, 0);
  h.renderServerAlarmBanner(h.device);
  assert.equal(banner.hidden, true);
  assert.notEqual(h.connectionStatus(h.device), 'norealtime');
});

test('server alert view never outlives its freshness window', () => {
  const h = browser();
  h.device.server = { ok: true, at: h.now() - h.SERVER_ALERT.pollMs * 3 - 1, serverTime: h.now(), lastSeen: h.now() - 500000, alarms: [], events: [] };
  h.state.realtimeConnected = false; h.state.realtimeLostAt = h.now() - 60000;
  assert.notEqual(h.connectionStatus(h.device), 'devicelost');            // stale copy is ignored: unknown beats wrong
});
