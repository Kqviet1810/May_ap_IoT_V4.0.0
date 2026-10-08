'use strict';
// Alarm uplink resilience on the REAL broker Durable Object (workerd via wrangler dev), no hardware:
//   device model (MQTT.js, firmware-shaped alarm/heartbeat/snapshot traffic) + Web clients (MQTT.js)
//     -> real broker Worker + MqttBrokerDO (durable uplink queue)
//     -> scripted stand-in for the main Worker (cloudflare/tests/e2e-front): slow / down / throttling / lost reply
// The main Worker's own D1 logic is covered by tests/alarm-delivery.test.cjs; the firmware's decision logic by
// tests/runtime-alarm-fallback.cpp. This file proves the broker/queue/transport contract end to end:
//   * PUBACK on an alarm = BROKER_STORED and never waits for the Worker (slow / down / throttled / lost reply);
//   * the device link (PINGRESP, other PUBACKs, snapshots to the Web) is never delayed by a slow Worker;
//   * nothing is lost, duplicated or reordered when the Worker recovers, when the broker restarts, or when the queue fills;
//   * 20 devices + 20 Web clients: no unexpected disconnect, bounded Worker calls.
//
//   node tools/e2e/uplink_resilience.cjs [outDir]
const { spawn } = require('node:child_process');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { setTimeout: sleep } = require('node:timers/promises');

const root = path.resolve(__dirname, '../..');
const cf = path.join(root, 'cloudflare');
const out = path.resolve(process.argv[2] || path.join(root, 'work', 'e2e-uplink'));
fs.mkdirSync(out, { recursive: true });
const PORT = Number(process.env.E2E_UPLINK_PORT || 8812);
const DEVICE_SECRET = 'e2e-device-secret';
const WEB_SECRET = 'e2e-web-token-secret';
const mqtt = require(path.join(cf, 'node_modules/mqtt'));
const hmacHex = (key, text) => crypto.createHmac('sha256', key).update(text).digest('hex');
const devicePassword = (id) => hmacHex(DEVICE_SECRET, `mayap-mqtt-device:v1\n${id}`);
const webToken = (id, user) => { const exp = Math.floor(Date.now() / 1000) + 3000; return `v1.${exp}.${hmacHex(WEB_SECRET, `mayap-mqtt-web:v1\n${id}\n${user}\n${exp}`)}`; };
const idFor = (n) => `MAP-${(0x100000000000 + n).toString(16).toUpperCase().padStart(12, '0')}`;

const results = [];
const record = (name, ok, detail = '') => { results.push({ name, ok, detail }); console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? ' - ' + detail : ''}`); };
async function until(label, probe, timeoutMs = 20000, stepMs = 50) {
  const end = Date.now() + timeoutMs;
  for (;;) { const v = await probe(); if (v) return v; if (Date.now() > end) throw new Error(`timeout: ${label}`); await sleep(stepMs); }
}
// The broker's current backlog as a NEW Web subscriber sees it (the `uplink` topic is sent once on subscribe).
async function brokerBacklog(id) {
  const probe = new Web(id, 'web:probe');
  try { await probe.up(); await until('uplink status', () => probe.uplink.length > 0, 8000, 50); return probe.uplink[0]; } finally { probe.end(); }
}
const backlogCleared = async (id, extra = () => true) => {
  let last = null;
  try { await until('backlog cleared', async () => { last = await brokerBacklog(id); return last.pending === 0 && extra(last); }, 120000, 500); }
  catch (error) { throw new Error(`${error.message}; last broker backlog ${JSON.stringify(last)}`); }
};
const pct = (values, p) => { const s = [...values].sort((a, b) => a - b); return s.length ? s[Math.min(s.length - 1, Math.floor(p * s.length))] : 0; };

// ------------------------------------------------------------------ process control
let wr = null;
const dirs = { persist: fs.mkdtempSync(path.join(os.tmpdir(), 'mayap-uplink-')) };
function writeVars() {
  fs.writeFileSync(path.join(cf, '.dev.vars'), `BROKER_DEVICE_SECRET=${DEVICE_SECRET}\nBROKER_WEB_TOKEN_SECRET=${WEB_SECRET}\n`);
  fs.writeFileSync(path.join(cf, 'tests/e2e-front/.dev.vars'), `MQTT_DEVICE_SECRET=${DEVICE_SECRET}\n`);
}
function removeVars() { for (const f of [path.join(cf, '.dev.vars'), path.join(cf, 'tests/e2e-front/.dev.vars')]) { try { fs.unlinkSync(f); } catch (_) { /* gone */ } } }
async function startStack(tag) {
  writeVars();
  const log = fs.createWriteStream(path.join(out, `wrangler-${tag}.log`));
  wr = spawn(path.join(cf, 'node_modules/.bin/wrangler'), ['dev', '-c', 'tests/e2e-front/wrangler.toml', '-c', 'wrangler-broker.toml',
    '--port', String(PORT), '--local', '--persist-to', dirs.persist], { cwd: cf, stdio: ['ignore', 'pipe', 'pipe'], detached: true });
  wr.stdout.pipe(log); wr.stderr.pipe(log);
  await until('stack up', async () => { try { return (await fetch(`http://127.0.0.1:${PORT}/healthz`)).status === 200; } catch (_) { return false; } }, 90000, 500);
}
// wrangler starts workerd as a child: signal the whole process group so no orphan keeps the port (or the DO storage) busy.
function killStack() {
  return new Promise((resolve) => {
    if (!wr) return resolve();
    const child = wr; wr = null;
    const signal = (name) => { try { process.kill(-child.pid, name); } catch (_) { /* gone */ } };
    signal('SIGTERM');
    setTimeout(() => signal('SIGKILL'), 4000);
    setTimeout(resolve, 5500);
  });
}
const base = `http://127.0.0.1:${PORT}`;
const ctl = async (mode = {}, reset = false) => (await fetch(`${base}/__ctl?mode=${encodeURIComponent(JSON.stringify(mode))}${reset ? '&reset=1' : ''}`)).json();
const stats = async () => (await fetch(`${base}/__stats`)).json();
const storedFor = (s, id) => s.events.filter((e) => e.key.startsWith(`ev:${id}:`));

// ------------------------------------------------------------------ device and Web models
class Device {
  constructor(id, { keepalive = 30 } = {}) {
    this.id = id; this.prefix = `mayap/v1/${id}`; this.closes = 0; this.latencies = []; this.pongs = []; this.acked = new Set();
    this.client = mqtt.connect(`ws://127.0.0.1:${PORT}/mqtt/${id}`, { protocolVersion: 4, clean: true, clientId: `esp-${id}`, username: id,
      password: devicePassword(id), keepalive, reconnectPeriod: 0, wsOptions: { protocol: 'mqtt' },
      will: { topic: `${this.prefix}/presence`, payload: Buffer.from('{"online":false}'), qos: 1, retain: true } });
    this.client.on('close', () => { this.closes += 1; });
    this.client.on('error', () => {});
    this.client.on('packetreceive', (packet) => { if (packet.cmd === 'pingresp' && this.pingAt) { this.pongs.push(Date.now() - this.pingAt); this.pingAt = 0; } });
  }
  async up() { await until(`${this.id} connected`, () => this.client.connected, 20000); return this; }
  alarm(eventId, type, state, extra = {}) {
    const started = Date.now();
    const payload = JSON.stringify({ event_id: eventId, alarm_type: type, severity: 'critical', state, message: `${type} ${state}`, detected_uptime_ms: 1000, age_ms: 5, ...extra });
    return new Promise((resolve) => this.client.publish(`${this.prefix}/alarm`, payload, { qos: 1 }, (error) => {
      const latency = Date.now() - started;
      if (!error) { this.latencies.push(latency); this.acked.add(eventId); }
      resolve({ eventId, latency, error });
    }));
  }
  heartbeat() { return new Promise((resolve) => { const t = Date.now(); this.client.publish(`${this.prefix}/heartbeat`, '{"batch_running":true}', { qos: 1 }, () => resolve(Date.now() - t)); }); }
  snapshot(n = 0) { this.client.publish(`${this.prefix}/snapshot`, JSON.stringify({ v: 2, n, t: 37.5 }), { qos: 0 }); }
  ping() { return new Promise((resolve) => { this.pingAt = Date.now(); const before = this.pongs.length; this.client._sendPacket({ cmd: 'pingreq' }, () => {});
    const timer = setInterval(() => { if (this.pongs.length > before) { clearInterval(timer); resolve(this.pongs[this.pongs.length - 1]); } }, 5); setTimeout(() => { clearInterval(timer); resolve(-1); }, 15000); }); }
  drop() { this.client.stream.destroy(); }
  end() { try { this.client.end(true); } catch (_) { /* gone */ } }
}
class Web {
  constructor(id, user = 'web:qa') {
    this.id = id; this.prefix = `mayap/v1/${id}`; this.snapshots = 0; this.uplink = []; this.presence = []; this.closes = 0;
    this.client = mqtt.connect(`ws://127.0.0.1:${PORT}/mqtt/${id}`, { protocolVersion: 4, clean: true, clientId: `web-${id}-${Math.random().toString(16).slice(2, 8)}`,
      username: user, password: webToken(id, user), keepalive: 30, reconnectPeriod: 0, wsOptions: { protocol: 'mqtt' } });
    this.client.on('close', () => { this.closes += 1; });
    this.client.on('error', () => {});
    this.client.on('message', (topic, payload) => {
      const channel = topic.slice(this.prefix.length + 1);
      if (channel === 'snapshot') this.snapshots += 1;
      else if (channel === 'uplink') this.uplink.push(JSON.parse(payload.toString()));
      else if (channel === 'presence') this.presence.push(payload.toString());
    });
  }
  async up() {
    await until(`${this.id} web connected`, () => this.client.connected, 20000);
    await new Promise((resolve, reject) => this.client.subscribe({ [`${this.prefix}/presence`]: { qos: 1 }, [`${this.prefix}/snapshot`]: { qos: 0 },
      [`${this.prefix}/uplink`]: { qos: 0 } }, (e) => e ? reject(e) : resolve()));
    return this;
  }
  end() { try { this.client.end(true); } catch (_) { /* gone */ } }
}

async function main() {
  const A = idFor(1);
  try {
    await startStack('a');
    await ctl({}, true);
    const web = await new Web(A).up();
    const dev = await new Device(A).up();
    await until('presence', () => web.presence.length >= 0, 1000);

    // T1 ---------------------------------------------------------------------------------------------------------------------
    await ctl({ delayMs: 0, status: 0, loseReply: false, throttle: true }, true);
    const t1 = await dev.alarm('t1-0001', 'FAULT_130', 'active');
    await until('t1 stored', async () => storedFor(await stats(), A).length === 1, 15000, 200);
    record('T1 healthy: one Critical is PUBACKed by the broker (BROKER_STORED) in well under a second and reaches the Worker', t1.latency < 1000 && !t1.error,
      `PUBACK ${t1.latency} ms`);

    // T2: 100 alarms ---------------------------------------------------------------------------------------------------------
    await ctl({}, true); await fetch(`${base}/__clear`);
    const closesBefore = dev.closes + web.closes;
    const t2 = [];
    for (let n = 0; n < 100; n += 1) {
      const type = `FAULT_${200 + (n % 10)}`, round = Math.floor(n / 10);       // each type alternates active/resolved: no cooldown throttling
      t2.push(dev.alarm(`t2-${String(n).padStart(4, '0')}`, type, round % 2 === 0 ? 'active' : 'resolved'));
      await sleep(30);
    }
    const t2r = await Promise.all(t2);
    await until('t2 stored', async () => storedFor(await stats(), A).length === 100, 90000, 500);
    const s2 = await stats();
    const bySeq = storedFor(s2, A).sort((a, b) => a.seq - b.seq);
    let ordered = true;
    for (let t = 0; t < 10; t += 1) {
      const ids = bySeq.filter((e) => e.type === `FAULT_${200 + t}`).map((e) => e.key.split(':')[2]);
      if (ids.join() !== [...ids].sort().join()) ordered = false;
    }
    const lat2 = t2r.map((r) => r.latency);
    record('T2 100 controlled alarms on a healthy link: every PUBACK < 1 s, all stored exactly once, per-type order kept, link never dropped',
      t2r.every((r) => !r.error) && pct(lat2, 0.99) < 1000 && s2.counters.stored === 100 && s2.counters.duplicate === 0 && ordered &&
      dev.closes + web.closes === closesBefore, `PUBACK p50=${pct(lat2, 0.5)} p95=${pct(lat2, 0.95)} max=${Math.max(...lat2)} ms; Worker calls=${s2.counters.alarmsCalls} for 100 events`);
    record('T2b Worker calls are batched (<= 1 call per 2 events on a burst)', s2.counters.alarmsCalls <= 60, `${s2.counters.alarmsCalls} calls`);

    // T3: slow Worker (6 s per call, beyond the former 4 s and 5 s deadlines) ----------------------------------------------
    await ctl({ delayMs: 6000 }, true); await fetch(`${base}/__clear`);
    const t3 = await Promise.all([1, 2, 3, 4, 5].map((n) => dev.alarm(`t3-000${n}`, `FAULT_30${n}`, 'active')));
    const pongs3 = [];
    for (let n = 0; n < 4; n += 1) { pongs3.push(await dev.ping()); dev.snapshot(n); await sleep(300); }
    const hb3 = await dev.heartbeat();
    record('T3 slow Worker (6 s/call): every alarm is PUBACKed < 1 s; PINGRESP and a heartbeat PUBACK are not delayed while the Worker call is in flight',
      t3.every((r) => !r.error && r.latency < 1000) && pongs3.every((p) => p >= 0 && p < 500) && hb3 < 1000 && dev.client.connected,
      `alarm PUBACK max ${Math.max(...t3.map((r) => r.latency))} ms; PINGRESP max ${Math.max(...pongs3)} ms; heartbeat PUBACK ${hb3} ms`);
    await until('t3 stored', async () => storedFor(await stats(), A).length === 5, 60000, 500);
    record('T3b the slow Worker eventually received all 5 alarms, once each', (await stats()).counters.stored === 5 && (await stats()).counters.duplicate === 0);
    record('T3c the device link and the Web link were never closed while the Worker was slow', dev.client.connected && web.client.connected && web.presence.every((p) => !p.includes('false')));

    // T4: cooldown / throttle ------------------------------------------------------------------------------------------------
    await ctl({ delayMs: 0, throttle: true }, true); await fetch(`${base}/__clear`);
    const a1 = await dev.alarm('t4-0001', 'FAULT_400', 'active');
    const a2 = await dev.alarm('t4-0002', 'FAULT_400', 'active');            // same type + state inside the 15 s cooldown
    await until('t4 first stored', async () => storedFor(await stats(), A).length >= 1, 15000, 100);
    await sleep(1500);
    const midStats = await stats();
    record('T4 Worker cooldown (429-equivalent): both alarms PUBACKed at once, the second waits in the broker (throttled), link untouched',
      a1.latency < 1000 && a2.latency < 1000 && midStats.counters.throttled >= 1 && storedFor(midStats, A).length === 1 && dev.client.connected,
      `stored ${storedFor(midStats, A).length}, throttled verdicts ${midStats.counters.throttled}`);
    await until('t4 second stored after cooldown', async () => storedFor(await stats(), A).length === 2, 40000, 500);
    const s4 = (await stats());
    record('T4b after the cooldown the throttled alarm is delivered, in order, exactly once',
      storedFor(s4, A).sort((a, b) => a.seq - b.seq).map((e) => e.key.split(':')[2]).join() === 't4-0001,t4-0002');

    // T5: Worker down, observable backlog, recovery -----------------------------------------------------------------------------
    await ctl({ status: 503, delayMs: 0 }, true); await fetch(`${base}/__clear`);
    web.uplink.length = 0;
    const t5 = await Promise.all([1, 2, 3].map((n) => dev.alarm(`t5-000${n}`, `FAULT_50${n}`, 'active')));
    await until('uplink status shows backlog', () => web.uplink.some((u) => u.pending === 3), 15000, 100);
    await sleep(3500);
    const lastStatus = web.uplink[web.uplink.length - 1];
    record('T5 Worker/D1 down: alarms are still PUBACKed (BROKER_STORED) and the backlog is observable on the Web (`uplink` topic), no disconnect',
      t5.every((r) => !r.error && r.latency < 1000) && lastStatus.pending === 3 && /HTTP_503|DOWN/.test(lastStatus.last_error) && dev.client.connected && web.client.connected,
      `pending=${lastStatus.pending} retrying=${lastStatus.retrying} error=${lastStatus.last_error}`);
    await ctl({ status: 0 });
    await until('t5 drained after recovery', async () => storedFor(await stats(), A).length === 3, 90000, 500);
    await backlogCleared(A);
    record('T5b when the Worker recovers the pending alarms are delivered by themselves, once each, and the backlog clears',
      (await stats()).counters.stored === 3 && (await brokerBacklog(A)).pending === 0);

    // T6: Worker stored but its reply was lost -----------------------------------------------------------------------------------
    await ctl({ status: 0, loseReply: true }, true); await fetch(`${base}/__clear`);
    const t6 = await dev.alarm('t6-0001', 'FAULT_600', 'active');
    await until('t6 first attempt', async () => (await stats()).counters.alarmsCalls >= 1, 40000, 100);
    await ctl({ loseReply: false });
    await backlogCleared(A);
    const s6 = await stats(); const ev6 = storedFor(s6, A);
    record('T6 Worker stored the event but the reply was lost: the broker retries, the Worker answers duplicate, exactly one stored event (no repeat push)',
      !t6.error && ev6.length === 1 && s6.counters.stored === 1 && s6.counters.duplicate >= 1, `stored=${s6.counters.stored} duplicate verdicts=${s6.counters.duplicate}`);

    // T7: queue overflow -------------------------------------------------------------------------------------------------------
    await ctl({ status: 503 }, true); await fetch(`${base}/__clear`);
    const replies = [];
    for (let n = 0; n < 70; n += 1) replies.push(Promise.race([dev.alarm(`t7-${String(n).padStart(3, '0')}`, `FAULT_7${n % 20}`.slice(0, 9), n % 2 ? 'resolved' : 'active'), sleep(4000).then(() => ({ timeout: true }))]));
    const t7 = await Promise.all(replies);
    const acked7 = t7.filter((r) => !r.timeout).length;
    const pong7 = await dev.ping();
    await sleep(500);
    const overflow = web.uplink[web.uplink.length - 1];
    record('T7 queue bounded at 64: the 64 oldest are PUBACKed, the rest get NO PUBACK (backpressure, counted as overflow); PINGRESP still answers, link up',
      acked7 === 64 && overflow.overflow >= 6 && pong7 >= 0 && pong7 < 500 && dev.client.connected, `acked=${acked7}/70 overflow=${overflow.overflow} PINGRESP=${pong7} ms`);
    await ctl({ status: 0 });
    await until('t7 drained', async () => (await stats()).counters.stored >= 60, 120000, 500);
    const unacked = [];
    for (let n = 0; n < 70; n += 1) if (!dev.acked.has(`t7-${String(n).padStart(3, '0')}`)) unacked.push(n);
    const resend = await Promise.all(unacked.map((n) => dev.alarm(`t7-${String(n).padStart(3, '0')}`, `FAULT_7${n % 20}`.slice(0, 9), n % 2 ? 'resolved' : 'active')));
    await until('t7 all stored', async () => storedFor(await stats(), A).length === 70, 120000, 500);
    record('T7b after the Worker recovers the device retry of the 6 refused events is accepted; all 70 are delivered once each, nothing lost',
      resend.every((r) => !r.error) && storedFor(await stats(), A).length === 70 && (await stats()).counters.stored === 70);

    // T8: device disappears while its alarms are still queued -------------------------------------------------------------------
    await ctl({ status: 503 }, true); await fetch(`${base}/__clear`);
    await dev.alarm('t8-0001', 'FAULT_800', 'active');
    dev.drop();
    await until('LWT seen', () => web.presence.some((p) => p.includes('false')), 40000, 200);
    await ctl({ status: 0 });
    await until('t8 delivered without the device', async () => storedFor(await stats(), A).length === 1, 90000, 500);
    record('T8 device power/TCP loss with an alarm still queued at the broker: the event survives and is delivered; the Web sees the LWT (real loss)', true);
    web.end();

    // T9: broker restart with pending events and the Worker down ----------------------------------------------------------------
    const B = idFor(2);
    let webB = await new Web(B).up(); let devB = await new Device(B).up();
    await ctl({ status: 503 }, true); await fetch(`${base}/__clear`);
    const t9 = await Promise.all([1, 2, 3].map((n) => devB.alarm(`t9-000${n}`, `FAULT_90${n}`, 'active')));
    await sleep(1200);
    devB.end(); webB.end();
    await killStack();
    await startStack('b');                                   // same persisted Durable Object storage; the stand-in Worker is back and healthy
    await until('t9 delivered after restart', async () => storedFor(await stats(), B).length === 3, 120000, 500);
    record('T9 broker process killed with 3 alarms pending (Worker was down): after restart the persisted queue drains by itself, once each',
      t9.every((r) => !r.error) && (await stats()).counters.stored === 3 && (await stats()).counters.duplicate === 0);

    // T10: 20 devices ----------------------------------------------------------------------------------------------------------------
    await ctl({ delayMs: 150, status: 0, loseReply: false }, true); await fetch(`${base}/__clear`);
    const N = 20; const devs = [], webs = [];
    for (let n = 0; n < N; n += 1) { const id = idFor(100 + n); webs.push(await new Web(id).up()); devs.push(await new Device(id).up()); }
    const runMs = 20000; const startedAt = Date.now(); let tick = 0;
    const alarmPromises = [];
    let hbs = 0;
    while (Date.now() - startedAt < runMs) {
      tick += 1;
      if (tick % 4 === 0) for (const d of devs) d.snapshot(tick);                   // 1 Hz per device while a Web is watching (the real cadence)
      if (tick % 16 === 0) for (const d of devs) { hbs += 1; d.heartbeat(); }      // accelerated heartbeats: every 4 s (real: 60 s)
      if (tick % 32 === 0) { const round = tick / 32; devs.forEach((d, i) => alarmPromises.push(d.alarm(`t10-${i}-${round}`, `FAULT_10${round % 3}`, round % 2 ? 'active' : 'resolved'))); }
      if (tick === 24) { devs[7].drop(); await sleep(100); devs[7] = await new Device(devs[7].id).up(); }   // one planned device reconnect
      await sleep(250);
    }
    const t10 = await Promise.all(alarmPromises);
    await until('t10 stored', async () => (await stats()).counters.stored + (await stats()).counters.throttled >= 1, 20000, 500);
    await sleep(40000);                                      // let throttled events (15 s cooldown) and backoffs settle
    const s10 = await stats();
    const unexpectedDevCloses = devs.reduce((a, d, i) => a + (i === 7 ? 0 : d.closes), 0);
    const unexpectedWebCloses = webs.reduce((a, w) => a + w.closes, 0);
    const snapshotsOk = webs.every((w) => w.snapshots > 12);
    record('T10 20 devices + 20 Web clients (snapshots 1 Hz, accelerated heartbeats, alarm rounds, one device reconnect): no unexpected close, all PUBACKs fast, Web snapshots flow for every device',
      t10.every((r) => !r.error && r.latency < 2000) && unexpectedDevCloses === 0 && unexpectedWebCloses === 0 && snapshotsOk,
      `alarm PUBACK p95=${pct(t10.map((r) => r.latency), 0.95)} max=${Math.max(...t10.map((r) => r.latency))} ms, ${t10.length} alarms, min snapshots/web=${Math.min(...webs.map((w) => w.snapshots))}`);
    const total10 = s10.events.length;
    record('T10b every alarm of every device reached the Worker exactly once (throttled ones after their cooldown)',
      total10 === t10.length && s10.counters.stored === t10.length, `stored ${s10.counters.stored}/${t10.length}, duplicate verdicts ${s10.counters.duplicate}, throttled verdicts ${s10.counters.throttled}`);
    const hbForwardMax = N * (Math.ceil((runMs + 40000) / 30000) + 1);
    record('T10c Worker calls are bounded: heartbeats are forwarded at most every 30 s per device, alarms are batched',
      s10.counters.hbCalls <= hbForwardMax && s10.counters.alarmsCalls <= t10.length,
      `heartbeats sent ${hbs} -> forwarded ${s10.counters.hbCalls} (cap ${hbForwardMax}); alarm batches ${s10.counters.alarmsCalls} for ${t10.length} events; total Worker calls ${s10.counters.calls}`);
    fs.writeFileSync(path.join(out, 'quota-measure.json'), JSON.stringify({ devices: N, runMs, heartbeatsSent: hbs, counters: s10.counters, alarms: t10.length }, null, 2));
    for (const d of devs) d.end();
    for (const w of webs) w.end();
  } catch (error) {
    record('uplink resilience run completed', false, String(error && error.stack || error));
  } finally {
    await killStack();
    removeVars();
    fs.writeFileSync(path.join(out, 'uplink-resilience.json'), JSON.stringify({ at: new Date().toISOString(), results }, null, 2));
    const failed = results.filter((r) => !r.ok);
    console.log(`\n${results.length - failed.length}/${results.length} checks passed`);
    process.exit(failed.length ? 1 : 0);
  }
}
main();
