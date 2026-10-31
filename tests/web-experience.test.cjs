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
      buildTechFields, techGateInfo, pmValues, PM_ITEMS, TECH_FIELDS, pmIngestLog, pmCsv, pidMon });
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
    const values = { advMaxHeaterPower:100,
      advTempRateLimitC:1, advTempRateWindowSec:120, advTempOscillationCrossLimit:6,
      advTempOscillationWindowSec:600, advHeaterStuckMinRiseC:0.3,
      advHeaterStuckDurationSec:900,
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
  Object.assign(h.device.config, { maxHeaterPower: 100, tempOffset: 0.1, humidityOffset: 0,
    heaterStuckMinRiseC: 0.3, heaterStuckDurationSec: 900, tempRateLimitC: 1, tempRateWindowSec: 120,
    tempOscillationCrossLimit: 6, tempOscillationWindowSec: 600 });
  for (const [key, id] of h.TECH_FIELDS) h.elements.set(id, { value: h.device.config[key] });
  assert.deepEqual(JSON.parse(JSON.stringify(h.buildTechFields(h.device))), {});                         // nothing changed -> nothing to send
  h.elements.get('advMaxHeaterPower').value = 70;
  h.elements.get('tempOffset').value = 0.1000000001;                          // float noise is not a change
  assert.deepEqual(JSON.parse(JSON.stringify(h.buildTechFields(h.device))), { maxHeaterPower: 70 });
});

test('Thermal Control V2.0.0: the Web no longer offers Auto Tune, PID gains, balance switch or relay tuning', () => {
  const html=fs.readFileSync(require.resolve('../index.html'),'utf8');
  const webSource=fs.readFileSync(require.resolve('../app.js'),'utf8');
  for (const gone of ['Tự cân bằng nhiệt', 'adaptiveThermalBalanceEnabled', 'id="startTune"', 'advKp', 'advKi', 'advKd', 'advAutotune', 'tuneBar']) {
    assert.ok(!html.includes(gone), `${gone} must be gone from index.html`);
  }
  for (const gone of ['autotune_start', 'autotune_cancel', 'AUTOTUNE_PHASES', 'startTune', 'unsupportedAdaptive', "kind: 'autotune'"]) {
    assert.ok(!webSource.includes(gone), `${gone} must be gone from app.js`);
  }
  const h=browser();h.device.config=Object.fromEntries(h.REQUIRED_CONFIG_KEYS.map(key=>[key,0]));
  for (const [key, id] of h.TECH_FIELDS) h.elements.set(id, { value: h.device.config[key] });
  assert.ok(!Object.hasOwn(h.buildTechFields(h.device),'adaptiveThermalBalanceEnabled'));
  assert.ok(!h.TECH_FIELDS.some(([key]) => /^(kp|ki|kd|autotune)/.test(key)));
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
  assert.match(html, /<details class="settingCard" id="techCard">/);                                              // ONE "Nâng cao" card (the old machine overview and PID section are merged)
  assert.match(html, /<div id="techLocked" hidden>/);                                                              // the PIN form shows only when the machine allows it and the session is locked
  assert.match(html, /id="techPin"[^>]*type="password"/);                                                          // PIN never echoed
  assert.match(html, /maxlength="4"/);
  const webSource = fs.readFileSync(require.resolve('../app.js'), 'utf8');
  assert.doesNotMatch(webSource, /localStorage[^;\n]*techPin|(?:setItem|console\.\w+)\([^)]*\bpin\b/);             // PIN is not stored or logged
});

test('Heater monitor shows measured values only and lists every figure the firmware still sends', () => {
  const h = browser();
  const keys = h.PM_ITEMS.map((item) => item[0]);
  assert.deepEqual(JSON.parse(JSON.stringify(keys)), ['pv', 'sp', 'err', 'req', 'act', 'pred', 'phase', 'fmt', 'slot']);
  const none = h.pmValues({ config: {} }, { temperature: 37.5 });          // firmware without pidMon: nothing invented
  for (const key of keys) assert.ok(key === 'phase' || key === 'fmt' ? none[key] === '' : Number.isNaN(none[key]), key);
  const live = h.pmValues({ config: {} }, { temperature: 37.4, pidMon: { valid: true, sp: 37.5, err: 0.1, req: 11, act: 10.5 } });
  assert.equal(live.pv, 37.4); assert.equal(live.sp, 37.5); assert.equal(live.err, 0.1); assert.equal(live.req, 11); assert.equal(live.act, 10.5);
  assert.equal(h.pmValues({ config: {} }, { temperature: 37.4, pidMon: { valid: true, fmt: 2 } }).fmt, '0,01 °C');
  assert.equal(h.pmValues({ config: {} }, { temperature: 37.4, pidMon: { valid: true, fmt: 1 } }).fmt, '0,1 °C');
  assert.match(h.pmValues({ config: {} }, { temperature: 37.4, pidMon: { valid: true, fmt: 1, fb: true } }).fmt, /đã tự hạ/);
  assert.equal(h.pmValues({ config: {} }, { temperature: 37.4, pidMon: { valid: true, fmt: 0 } }).fmt, 'đang dò định dạng');
  const webSource = fs.readFileSync(require.resolve('../app.js'), 'utf8');
  assert.match(webSource, /\.\.\.\(mon \? \{ mon: true \} : \{\}\)/);                              // lease asks for pidMon only while the card is open
});

test('PID log: chunks of the machine ring are assembled by index and saved as CSV', () => {
  const h = browser();
  h.pidMon.log = new Map(); h.pidMon.series = []; h.pidMon.logHead = 0;
  h.pmIngestLog({ h: 30, s: 20, d: [3750, 3750, 3751, 40, 22, 0x41, 0, 3752, 3750, 3752, 42, 23, 0x45, 4 | (8 << 4)] });    // samples 20 and 21 (the 7th value: low 4 bits = V3 state, 4 = frozen (heater held); high 4 bits = trust in the prediction, 8/15)
  h.pmIngestLog({ h: 30, s: 20, d: [3750, 3750, 3751, 40, 22, 0x41, 0] });                                          // a repeat changes nothing
  h.pmIngestLog({ h: 30, s: 28, d: [3760, 3750, 3760, 0, 0, 0x03, 5] });
  assert.equal(h.pidMon.log.size, 3);
  assert.equal(h.pidMon.log.get(21).pv, 37.52);
  assert.equal(h.pidMon.log.get(21).req, 21);                                                                    // percent * 2 on the wire
  h.pmIngestLog(null); h.pmIngestLog({ h: 'x', s: 1, d: [] });                                                   // garbage is ignored
  const csv = h.pmCsv({ id: 'MAP-TEST', bootId: 7 });
  const lines = csv.trim().split('\n');
  assert.match(lines[0], /^# mayap pid log device=MAP-TEST boot=7/);
  assert.equal(lines[1], 'time_utc,index,pv_c,sp_c,pred_c,request_pct,ssr_avg60_pct,phase,suspect,vent_fan,ssr_on,heat_master,batch,sensor_ok,v2_state,trust_pct,source,' +
    'safety_c,hold_pct,kair_e5,tau_s,integral_pct,vent_why,vent_line,door,identified,fan_tokens,vent_reason,learn_src,learn_saved,stable_min');
  assert.equal(lines.length, 5);
  const second = lines[3].split(',');
  assert.equal(second[1], '21'); assert.equal(second[7], '1'); assert.equal(second[8], '1'); assert.equal(second[14], 'frozen'); assert.equal(second[15], '53'); assert.equal(second[16], 'machine');   // phase 1, suspect flag
  assert.equal(second.length, 31);                                                                               // an older machine sends no diagnostics: the new columns stay empty
  assert.equal(second.slice(17).join(''), '');
});

test('PID log: the diagnostics chunk (firmware 2.6.0) merges into the samples it belongs to, in either order', () => {
  const h = browser();
  h.pidMon.log = new Map(); h.pidMon.series = []; h.pidMon.logHead = 0;
  // the diagnostics of samples 40 and 41 arrive first: no row yet (no temperature), nothing invented
  // per sample: safety reading x100, hold x2, kAir x1e5, tau, integral x4, vent byte (why | state << 3), ctl byte, learn byte
  const vent40 = 3 | ((1 | 2) << 3);                  // floor, watching + on
  const ctl40 = 1 | 2 | (2 << 2) | (3 << 4);          // door guard, transit measured, 2 tokens, reason 3
  const learn40 = 2 | (1 << 2) | (7 << 3);            // oven + hold restored, snapshot stored, 28 stable minutes
  h.pmIngestLog({ h: 50, xs: 40, x: [3760, 4, 40, 150, -6, vent40, ctl40, learn40, 3750, 3, 40, 150, 0, 0, 0, 0] });
  assert.equal(h.pmCsv({ id: 'MAP-TEST', bootId: 7 }).trim().split('\n').length, 2);                           // header lines only
  h.pmIngestLog({ h: 50, s: 40, d: [3760, 3750, 3761, 4, 0, 0x41, 0, 3750, 3750, 3750, 6, 0, 0x41, 0] });
  const lines = h.pmCsv({ id: 'MAP-TEST', bootId: 7 }).trim().split('\n');
  assert.equal(lines.length, 4);
  const a = lines[2].split(',');
  assert.equal(a.length, 31);
  assert.equal(a[17], '37.6'); assert.equal(a[18], '2'); assert.equal(a[19], '40'); assert.equal(a[20], '150'); assert.equal(a[21], '-1.5');
  assert.equal(a[22], 'floor'); assert.equal(a[23], 'watch+on'); assert.equal(a[24], '1'); assert.equal(a[25], '1'); assert.equal(a[26], '2'); assert.equal(a[27], '3');
  assert.equal(a[28], '2'); assert.equal(a[29], '1'); assert.equal(a[30], '28');
  const b = lines[3].split(',');
  assert.equal(b[17], '37.5'); assert.equal(b[22], ''); assert.equal(b[23], '');                                // no why, no state bits
  h.pmIngestLog({ h: 50, xs: 40, x: [3760, 4, 40, 150, -6, 0, 0] });                                                                   // a short or broken chunk changes nothing
  h.pmIngestLog({ h: 50, xs: 'q', x: [1, 2, 3, 4, 5, 6, 7] });
  assert.equal(h.pmCsv({ id: 'MAP-TEST', bootId: 7 }).trim().split('\n').length, 4);
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

test('landing has no duplicated top nav', () => {
  const html = fs.readFileSync(require.resolve('../index.html'), 'utf8');
  const css = fs.readFileSync(require.resolve('../landing.css'), 'utf8');
  const app = fs.readFileSync(require.resolve('../app.js'), 'utf8');
  assert.doesNotMatch(html, /<nav class="landingNav"/);
  assert.match(css, /grid-template-rows:minmax\(0,1fr\) auto 30px/);
});

test('thermal final bounds match firmware', () => {
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
    advMaxHeaterPower:100,
    advTempRateLimitC:1, advTempRateWindowSec:120,
    advTempOscillationCrossLimit:6, advTempOscillationWindowSec:600,
    advHeaterStuckMinRiseC:0.3, advHeaterStuckDurationSec:900
  })) h.elements.set(id, { value });
  assert.equal(h.validateTemperatureForm(), true);
  assert.equal(h.validateSensorForm(), true);
  assert.equal(h.validateAdvancedForm(), true);
  h.elements.get('advMaxHeaterPower').value = 9;
  assert.equal(h.validateAdvancedForm(), false);
  assert.equal(h.window.invalidField, 'advMaxHeaterPower');
  h.elements.get('advMaxHeaterPower').value = 100;
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

test('Web output control: buttons exist, are gated by the HMI permissions, and offline temperature shows a dash', () => {
  const html = fs.readFileSync(require.resolve('../index.html'), 'utf8');
  const src = fs.readFileSync(require.resolve('../app.js'), 'utf8');
  for (const id of ['outputHeaterBtn', 'outputCircBtn', 'outputVentBtn', 'outputTurnTile', 'turnSplit', 'turnLeftBtn', 'turnRightBtn', 'pmDownload'])
    assert.match(html, new RegExp(`id="${id}"`));
  assert.match(src, /WEBCTL = Object\.freeze\(\{ heater: 1, vent: 2, circ: 4, light: 8, siren: 16, turn: 32, autoUpdate: 64 \}\)/);
  for (const act of ['vent_toggle', 'circ_toggle', 'heater_toggle', 'siren_toggle', 'turn_hold'])
    assert.ok(src.includes(act), act);
  // the Web never writes the permission mask: no command that sets it exists on the Web side
  assert.ok(!/webctl_set|WebCtlSet/.test(src));
  assert.match(src, /\['online',\s*'reconnecting',\s*'degraded'\]/);
  // the egg-turn tile is ONE tile unless the HMI allows turning AND AUTO is off; the old pad row and hint line are gone
  assert.ok(!/turnPad|webCtlHint/.test(html) && !/turnPad|webCtlHint/.test(src));
  assert.match(src, /granted\s*&&\s*manual/);
  // every item of the machine's Advanced menu is listed with its access
  assert.match(html, /id="advOverview"/);
  for (const label of ['PID Monitor', 'Bản ghi cũ', 'Quyền Web', 'Chế độ 2 tài khoản', 'Đổi mã kỹ thuật'])
    assert.ok(src.includes(`'${label}'`) || src.includes(`['${label}'`) || src.includes(label), label);
  for (const label of ['Gia nhiệt', 'Bảo vệ nhiệt', 'Hiệu chuẩn cảm biến', 'Trần công suất nhiệt'])
    assert.ok(html.includes(label), label);                                                                          // the editable tiles are in the same card
  // no legend: editable tiles are the light ones, display-only tiles are a darker shade of the same grid
  assert.match(src, /compactTile ro/);
  assert.match(fs.readFileSync(require.resolve('../styles.css'), 'utf8'), /\.advGrid>\.compactTile\.ro\{background:var\(--tileRo\)/);
  // the exhaust-fan tile says why the fan runs, the egg-turn tile says which way
  assert.match(src, /VENT_WHY/); assert.match(src, /ĐANG ĐẢO TRÁI/); assert.match(html, /<span>Quạt hút<\/span><strong id="outputVent">/);
});
