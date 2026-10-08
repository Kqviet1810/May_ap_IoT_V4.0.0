'use strict';
// PROTOCOL EMULATOR of the ESP32 side, NOT the firmware. It reproduces what
// MAYAP_INDUSTRIAL_v1_0_0/transaction_bridge.h puts on the wire (V2 grant/body
// verification, replay fence, terminal cache, signed ACKs, session leases, live
// snapshot, config/reported chunks) so the Web transport and the broker can be
// exercised end to end without hardware. It proves nothing about the real
// controller, esp-mqtt or heap use; those need the device.
const crypto = require('node:crypto');
const mqtt = require('../../cloudflare/node_modules/mqtt');

const hmac = (key, text) => crypto.createHmac('sha256', key).update(text).digest();
const hex = (buffer) => buffer.toString('hex');
const safeEqual = (a, b) => a.length === b.length && crypto.timingSafeEqual(a, b);

const CONFIG_DEFAULTS = {
  targetTemp: 37.5, tempHysteresis: 0.3, lowTempAlarm: 36.5, highTempAlarm: 38.5, emergencyTemp: 40,
  kp: 20, ki: 0.1, kd: 5, lowHumidityAlarm: 40, humidifierInstalled: false, humidifierEnabled: false,
  targetHumidity: 55, humidifierHysteresisRh: 3, ventOnTemp: 38.2, ventOffTemp: 37.8,
  ventScheduleEnabled: false, ventScheduleCount: 3, ventScheduleDurationMin: 5, ventScheduleHour1: 6,
  ventScheduleHour2: 10, ventScheduleHour3: 14, ventScheduleHour4: 18, ventScheduleHour5: 22,
  ventScheduleHour6: 2, tempOffset: 0, humidityOffset: 0, pidCycleSec: 2, humidityAlarmDelaySec: 300,
  turnIntervalMin: 120, turnMaxRunSec: 30, powerRestoreDelaySec: 10, sensorTimeoutSec: 15,
  maxHeaterPower: 100, totalIncubationDays: 21, circulationFanEnabled: true, turningEnabled: true,
  autoResumeAfterPower: true, allowHeatWithoutBatch: false, alarmEnabled: true,
  lightAfterBatchAlarmEnabled: false, highTempAlarmWithoutBatch: true, controlMode: 1, nextDirection: 0,
  heaterStuckMinRiseC: 0.5, heaterStuckDurationSec: 600, tempRateLimitC: 2, tempRateWindowSec: 60,
  tempOscillationCrossLimit: 8, tempOscillationWindowSec: 600, autotuneRelayPowerPercent: 50,
  autotuneBandC: 0.3, manualTurnReanchorsSchedule: false, sirenSelfTestEnabled: true,
};

const COMMANDS = new Set(['batch_start', 'batch_stop', 'resume_yes', 'resume_no', 'autotune_start',
  'autotune_cancel', 'firmware_check_now', 'firmware_rollback', 'light_toggle', 'alarm_ack',
  'batch_overdue_continue']);

// transaction_bridge.h ackCode()/ackFriendlyMessage() for the codes this emulator emits.
function ackCode(result, message) {
  if (message.startsWith('CONFIG_')) return message;
  return { applied: 'APPLIED', accepted: 'RECEIVED', unauthorized: 'AUTH_ERROR', stale: 'STALE_REQUEST',
    busy: 'CONTROLLER_BUSY', expired: 'CONTROLLER_TIMEOUT', invalid: 'INVALID_REQUEST',
    unsupported: 'UNSUPPORTED_OPERATION' }[result] || 'CONTROLLER_REJECTED';
}
function friendly(code, raw) {
  if (raw === 'STALE_BOOT') return 'Máy vừa khởi động lại; hãy đồng bộ rồi thử lại';
  if (raw === 'REPLAY SEQUENCE') return 'Yêu cầu cũ đã được gửi trước đó';
  if (raw === 'INVALID_CONFIG_PATCH') return 'Thông số cấu hình không hợp lệ';
  if (code === 'CONFIG_SAFETY_BLOCK') return 'Máy đang có lỗi an toàn; chưa thể lưu';
  if (code === 'UNSUPPORTED_OPERATION') return 'Firmware chưa hỗ trợ thao tác này';
  if (raw) return raw;
  return code === 'APPLIED' ? 'Máy đã thực hiện' : code === 'RECEIVED' ? 'Máy đã nhận yêu cầu' : 'Máy từ chối yêu cầu';
}

function startDeviceEmulator({ url, deviceId, password, commandKeyHex, activeIntervalMs = 1000,
  idleIntervalMs = 120000 }) {
  const commandKey = Buffer.from(commandKeyHex, 'hex');
  const bootId = (crypto.randomBytes(4).readUInt32BE() % 0xfffffff0) + 1;
  const root = `mayap/v1/${deviceId}`;
  const state = {
    config: { ...CONFIG_DEFAULTS }, revision: 1, safetyBlock: false,
    runtime: { temperature: 37.5, humidity: 55, machineState: 'Không có tác vụ đang chạy', batchRunning: false,
      currentDay: 0, heaterOn: false, heaterPower: 0, circulationFanOn: true, ventFanOn: false,
      humidifierOn: false, lightOn: false, sirenOn: false, turnState: 3, nextTurnMinutes: 90,
      autoTuneState: 0, autoTuneProgress: 0, resumeConfirmationRequired: false,
      batchOverdueConfirmationPending: false, activeFaults: [] },
  };
  const leases = new Map();
  const replay = new Map();      // clientId -> last seq
  const terminal = new Map();    // requestId -> {result, message, op, key}
  const stats = { commandsExecuted: 0, snapshots: 0, acks: [], verifyFailures: 0, rejectedByPolicy: 0 };
  let lastSnapshotAt = 0, forceSnapshot = true, connected = false;
  const events = [];
  const inflight = new Set();   // requestIds accepted but not yet terminal (pendingCommands)
  const silenced = [];   // commands received while state.silent: verified, never acknowledged

  const client = mqtt.connect(url, {
    protocolVersion: 4, clean: true, clientId: `esp-${deviceId}`, username: deviceId, password,
    keepalive: 30, reconnectPeriod: 0, wsOptions: { protocol: 'mqtt' },
    will: { topic: `${root}/presence`, payload: Buffer.from('{"online":false}'), qos: 1, retain: true },
  });

  const publish = (channel, payload, qos, retain = false) => new Promise((resolve) => {
    client.publish(`${root}/${channel}`, JSON.stringify(payload), { qos, retain }, () => resolve());
  });
  const presence = (online) => publish('presence', { online, bootId, ip: '192.168.1.50', rssi: -55,
    fw: '1.1.2', firmware: '1.1.2', proto: 2, maxPacket: 2048,
    caps: ['transactions', 'config.patch', 'control.session', 'history.chunk'], hw: 'emulator' }, 1, true);

  function publishAck(requestId, result, message, op, key) {
    const received = result === 'accepted', uncertain = result === 'expired', ok = result === 'applied';
    const code = ackCode(result, message || '');
    const text = friendly(code, message || '');
    const phase = received ? 'received' : uncertain ? 'uncertain' : 'completed';
    const doc = { v: 2, requestId, operation: op, phase, ok, code, bootId, result, message: text,
      revision: state.revision, tDeviceReceived: Date.now() % 0xffffffff, tDeviceCompleted: Date.now() % 0xffffffff };
    if (key) doc.sig = hex(hmac(key, ['mayap-mqtt-ack:v2', deviceId, requestId, op, phase, ok ? 1 : 0, code,
      bootId, state.revision, text].join('\n')));
    if (!received && !uncertain) {
      terminal.set(requestId, { result, message, op, key });
      forceSnapshot = true;
    }
    stats.acks.push({ requestId, phase, code, ok });
    return publish('ack', doc, 1);
  }

  // transaction_bridge.h realtimeVerifyV2()
  function verifyV2(channel, wire) {
    const { grant = '', grantSig = '', body = '', sig = '' } = wire;
    const m = /^([A-Za-z0-9_-]{8,39})\|(\d+)\|([0-9a-f]{24})$/.exec(grant);
    if (!m || !body || !/^[0-9a-f]{64}$/.test(grantSig) || !/^[0-9a-f]{64}$/.test(sig)) return null;
    const expectedGrant = hmac(commandKey, `mayap-control-grant:v2\n${deviceId}\n${grant}`);
    if (!safeEqual(expectedGrant, Buffer.from(grantSig, 'hex'))) return null;
    const sessionKey = hmac(commandKey, `mayap-control-session:v2\n${deviceId}\n${grant}`);
    const expectedBody = hmac(sessionKey, `mayap-mqtt-write:v2\n${deviceId}\n${channel}\n${grant}\n${body}`);
    if (!safeEqual(expectedBody, Buffer.from(sig, 'hex'))) return null;
    let doc;
    try { doc = JSON.parse(body); } catch (_) { return null; }
    if (!(doc.v === 2 && doc.clientId === m[1] && String(doc.requestId || '').length > 0 &&
        String(doc.nonce || '').length >= 16 && Number(doc.seq) > 0)) return null;
    const now = Math.floor(Date.now() / 1000), expiry = Number(m[2]);
    return { doc, sessionKey, expired: expiry < now || expiry > now + 300 };
  }

  function applyPatch(patch) {
    const keys = Object.keys(patch || {});
    if (!keys.length || keys.some((key) => !(key in CONFIG_DEFAULTS) ||
        !['number', 'boolean'].includes(typeof patch[key]))) return false;
    Object.assign(state.config, patch);
    return true;
  }

  async function dispatch(channel, payload) {
    if (channel === 'session') return handleSession(payload);
    if (!['command', 'config/set'].includes(channel)) return;
    const verified = verifyV2(channel, payload);
    const op0 = channel === 'command' ? '' : 'config.save';
    if (!verified) {
      stats.verifyFailures++;
      if (payload.requestId) await publishAck(payload.requestId, 'unauthorized', 'CHU KY LENH KHONG HOP LE', op0);
      return;
    }
    const { doc, sessionKey, expired } = verified;
    const op = channel === 'command' ? String(doc.action || '').replaceAll('_', '.') : 'config.save';
    if (expired) return publishAck(doc.requestId, 'unauthorized', 'SESSION_EXPIRED', op, undefined);
    if (Number(doc.bootId) !== bootId) return publishAck(doc.requestId, 'stale', 'STALE_BOOT', op, sessionKey);
    if (terminal.has(doc.requestId)) {                       // replayTerminal()
      const t = terminal.get(doc.requestId);
      return publishAck(doc.requestId, t.result, t.message, t.op, t.key);
    }
    // dispatchApplicationMessage(): a duplicate of an in-flight request is answered
    // `accepted` BEFORE the replay-sequence fence is consulted.
    if (inflight.has(doc.requestId)) return publishAck(doc.requestId, 'accepted', '', op, sessionKey);
    const last = replay.get(doc.clientId) || 0;
    if (Number(doc.seq) <= last) return publishAck(doc.requestId, 'stale', 'REPLAY SEQUENCE', op, sessionKey);
    replay.set(doc.clientId, Number(doc.seq));
    if (channel === 'command') return handleCommand(doc, op, sessionKey);
    return handleConfigSet(doc, sessionKey);
  }

  async function handleCommand(doc, op, key) {
    const now = Math.floor(Date.now() / 1000);
    if (!(Number(doc.expiresAt) >= now && Number(doc.expiresAt) <= now + 30))
      return publishAck(doc.requestId, 'expired', 'EXPIRED_REQUEST', op, key);
    if (!COMMANDS.has(doc.action)) return publishAck(doc.requestId, 'unsupported', '', op, key);
    if (doc.action === 'firmware_rollback')
      return publishAck(doc.requestId, 'rejected', 'QUAY LAI CAN XAC NHAN TAI MAY', op, key);
    inflight.add(doc.requestId);
    if (state.silent) { silenced.push({ doc, op, key }); return; }
    await runCommand(doc, op, key);
  }

  async function runCommand(doc, op, key) {
    await publishAck(doc.requestId, 'accepted', '', op, key);
    // The real controller consumes the HMI queue on its own task; emulate the delay.
    setTimeout(async () => {
      if (doc.action === 'light_toggle') state.runtime.lightOn = !state.runtime.lightOn;
      stats.commandsExecuted++;
      inflight.delete(doc.requestId);
      await publishAck(doc.requestId, 'applied', '', op, key);
    }, 120);
  }

  async function handleConfigSet(doc, key) {
    await publishAck(doc.requestId, 'accepted', '', 'config.save', key);
    if (state.safetyBlock) return publishAck(doc.requestId, 'rejected', 'CONFIG_SAFETY_BLOCK', 'config.save', key);
    if (!applyPatch(doc.config)) return publishAck(doc.requestId, 'invalid', 'INVALID_CONFIG_PATCH', 'config.save', key);
    state.revision = Number(doc.revision) > state.revision ? Number(doc.revision) : state.revision + 1;
    await publishConfig(doc.requestId);
    return publishAck(doc.requestId, 'applied', '', 'config.save', key);
  }

  async function publishConfig(requestId) {
    const entries = Object.entries(state.config);
    let part = 0, chunk = [];
    const flush = async (done) => {
      const body = { v: 2, bootId, revision: state.revision, part, done, config: Object.fromEntries(chunk) };
      if (requestId) body.requestId = requestId;
      await publish('config/reported', body, 1);
      part++; chunk = [];
    };
    for (const entry of entries) {
      chunk.push(entry);
      if (JSON.stringify(Object.fromEntries(chunk)).length > 700) await flush(false);
    }
    await flush(true);
  }

  function handleSession(doc) {
    const clientId = String(doc.clientId || '');
    if (clientId.length < 8 || clientId.length >= 40) return;
    const now = Date.now();
    if (doc.active) leases.set(clientId, now + Math.min(Number(doc.ttlMs) || 60000, 60000));
    else leases.delete(clientId);
    if (doc.sync) { forceSnapshot = true; lastSnapshotAt = 0; publishConfig(undefined); }
  }

  const tick = setInterval(async () => {
    if (!connected) return;
    const now = Date.now();
    for (const [id, expiry] of leases) if (expiry <= now) leases.delete(id);
    const interval = leases.size ? activeIntervalMs : idleIntervalMs;
    if (!forceSnapshot && now - lastSnapshotAt < interval) return;
    forceSnapshot = false; lastSnapshotAt = now;
    state.runtime.temperature = Math.round((37.5 + 0.2 * Math.sin(now / 5000)) * 10) / 10;
    stats.snapshots++;
    await publish('snapshot', { bootId, revision: state.revision, runtime: state.runtime }, 0);
  }, 100);

  client.on('connect', () => {
    client.subscribe({ [`${root}/command`]: { qos: 1 }, [`${root}/config/set`]: { qos: 1 },
      [`${root}/history/request`]: { qos: 1 }, [`${root}/session`]: { qos: 0 } }, async (error, granted) => {
      if (error || granted.some((g) => g.qos === 128)) { events.push('subscribe-rejected'); return; }
      connected = true;
      await presence(true);
      events.push('online');
    });
  });
  client.on('message', (topic, data) => {
    if (!topic.startsWith(`${root}/`)) return;
    let payload;
    try { payload = JSON.parse(data.toString()); } catch (_) { return; }
    dispatch(topic.slice(root.length + 1), payload).catch((error) => events.push(`error:${error.message}`));
  });
  client.on('error', (error) => events.push(`mqtt-error:${error.message}`));

  return {
    deviceId, bootId, state, stats, events,
    // Late terminal ACK for commands that were silently held (V2 late-ACK path).
    async releaseSilenced() { state.silent = false; for (const item of silenced.splice(0)) await runCommand(item.doc, item.op, item.key); },
    get connected() { return connected; },
    // Device -> cloud alarm over MQTT (QoS1, the PUBACK means BROKER_STORED). Resolves with the PUBACK latency in ms.
    publishAlarm(eventId, alarmType = 'FAULT_130', state = 'active') {
      const started = Date.now();
      return new Promise((resolve) => client.publish(`${root}/alarm`, JSON.stringify({ event_id: eventId, alarm_type: alarmType,
        severity: 'critical', state, message: `${alarmType} ${state}` }), { qos: 1 }, (error) => resolve(error ? -1 : Date.now() - started)));
    },
    // Abrupt socket loss: the broker must publish the retained LWT.
    dropConnection() { clearInterval(tick); connected = false; client.stream.destroy(); },
    stop() { clearInterval(tick); connected = false; client.end(true); },
  };
}

module.exports = { startDeviceEmulator, CONFIG_DEFAULTS };
