'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const { test } = require('node:test');

const wire = require('./fixtures/mqtt-wire.cjs');
const harness = require('./fixtures/mqtt-broker-harness.cjs');

const DEV = 'MAP-001122334455';
// Per-device password: same derivation as cloudflare/src/broker/acl.js deriveDevicePassword().
const DEV_SECRET = 'device-secret-test';
const devicePasswordFor = (id) => require('node:crypto').createHmac('sha256', DEV_SECRET).update(`mayap-mqtt-device:v1\n${id}`).digest('hex');
const DEV_PWD = devicePasswordFor(DEV);
const crypto = require('node:crypto');
const WEB_TOKEN_SECRET = 'web-token-secret-test';
// Same construction as cloudflare/src/broker/acl.js signWebToken().
function webToken(username = 'web:u1', deviceId = DEV, ttlSec = 600) {
  const exp = Math.floor(Date.now() / 1000) + ttlSec;
  const mac = crypto.createHmac('sha256', WEB_TOKEN_SECRET)
    .update(`mayap-mqtt-web:v1\n${deviceId}\n${username}\n${exp}`).digest('hex');
  return `v1.${exp}.${mac}`;
}

function envFixture() {
  return {
    BROKER_DEVICE_SECRET: DEV_SECRET,
    BROKER_WEB_TOKEN_SECRET: WEB_TOKEN_SECRET,
  };
}

async function connectDevice(broker) {
  const { client, server } = await harness.openWebSocket(broker, DEV);
  await harness.feed(broker, server, wire.connect({
    clientId: `esp-${DEV}`,
    username: DEV,
    password: DEV_PWD,
    will: {
      topic: `mayap/v1/${DEV}/presence`, qos: 1, retain: true,
      payload: '{"online":false}',
    },
  }));
  const [connack] = await harness.clientReceive(client);
  return { client, server, connack };
}

async function connectWeb(broker, { username = 'web:u1' } = {}) {
  const { client, server } = await harness.openWebSocket(broker, DEV);
  await harness.feed(broker, server, wire.connect({
    clientId: `web-u1-${Math.random().toString(36).slice(2, 6)}`,
    username, password: webToken(username),
  }));
  const [connack] = await harness.clientReceive(client);
  return { client, server, connack };
}

// --------------------------------------------------------------------------

test('CONNECT device with valid credentials → CONNACK 0', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const { connack } = await connectDevice(b.broker);
  const parsed = wire.parseConnack(connack);
  assert.equal(parsed.returnCode, 0);
  assert.equal(parsed.sessionPresent, false);
});

test('CONNECT with bad credentials → CONNACK 4', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const { client, server } = await harness.openWebSocket(b.broker, DEV);
  await harness.feed(b.broker, server, wire.connect({
    clientId: `esp-${DEV}`, username: DEV, password: 'wrong',
  }));
  const [frame] = await harness.clientReceive(client);
  const parsed = wire.parseConnack(frame);
  assert.equal(parsed.returnCode, 4);
  assert.equal(server.closed, true);
});

test('CONNECT CleanSession=0 → broker rejects', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const { client, server } = await harness.openWebSocket(b.broker, DEV);
  await harness.feed(b.broker, server, wire.connect({
    clientId: 'c', cleanSession: false, username: DEV, password: DEV_PWD,
  }));
  // Malformed by contract → fatal close with no CONNACK.
  assert.equal(server.closed, true);
});

test('SUBSCRIBE: web gets granted QoS1 on command topic', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const web = await connectWeb(b.broker);
  await harness.feed(b.broker, web.server, wire.subscribe({
    packetId: 10,
    filters: [{ filter: `mayap/v1/${DEV}/snapshot`, qos: 1 }],
  }));
  const [sub] = await harness.clientReceive(web.client);
  const parsed = wire.parseSuback(sub);
  assert.equal(parsed.packetId, 10);
  // snapshot is a QoS0 topic cap per contract — so granted QoS is 0.
  assert.deepEqual(parsed.codes, [0]);
});

test('SUBSCRIBE: device cannot subscribe to presence (not in DEVICE_SUB)', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const dev = await connectDevice(b.broker);
  await harness.feed(b.broker, dev.server, wire.subscribe({
    packetId: 11,
    filters: [{ filter: `mayap/v1/${DEV}/presence`, qos: 1 }],
  }));
  const [sub] = await harness.clientReceive(dev.client);
  assert.deepEqual(wire.parseSuback(sub).codes, [0x80]);
});

test('SUBSCRIBE: foreign device scope → 0x80', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const web = await connectWeb(b.broker);
  await harness.feed(b.broker, web.server, wire.subscribe({
    packetId: 1,
    filters: [{ filter: `mayap/v1/MAP-DEADBEEF0000/snapshot`, qos: 1 }],
  }));
  const [sub] = await harness.clientReceive(web.client);
  assert.deepEqual(wire.parseSuback(sub).codes, [0x80]);
});

test('PUBLISH command QoS1: web→device fanout + PUBACK to publisher', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const dev = await connectDevice(b.broker);
  await harness.feed(b.broker, dev.server, wire.subscribe({
    packetId: 5, filters: [{ filter: `mayap/v1/${DEV}/command`, qos: 1 }],
  }));
  await harness.clientReceive(dev.client); // suback
  const web = await connectWeb(b.broker);
  await harness.feed(b.broker, web.server, wire.publish({
    topic: `mayap/v1/${DEV}/command`, qos: 1, packetId: 77,
    payload: '{"op":"start","requestId":"R1"}',
  }));
  const webFrames = await harness.clientReceive(web.client);
  const puback = wire.parsePuback(webFrames[0]);
  assert.equal(puback.packetId, 77);

  const devFrames = await harness.clientReceive(dev.client);
  const fanned = wire.parsePublish(devFrames[0]);
  assert.equal(fanned.topic, `mayap/v1/${DEV}/command`);
  assert.equal(fanned.qos, 1);
  assert.equal(fanned.retain, false);
  assert.equal(fanned.payload.toString('utf-8'), '{"op":"start","requestId":"R1"}');

  // Device PUBACKs → broker clears inflight.
  await harness.feed(b.broker, dev.server, wire.puback({ packetId: fanned.packetId }));
  const att = dev.server.deserializeAttachment();
  assert.deepEqual(att.inflight, []);
});

test('ACL: device cannot publish command (silent drop + close after 2nd)', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const dev = await connectDevice(b.broker);
  await harness.feed(b.broker, dev.server, wire.publish({
    topic: `mayap/v1/${DEV}/command`, qos: 1, packetId: 1, payload: 'x',
  }));
  // PUBACK still sent to hide role structure, no fanout.
  const after1 = await harness.clientReceive(dev.client);
  assert.equal(wire.parsePuback(after1[0]).packetId, 1);
  assert.equal(dev.server.closed, false);
  await harness.feed(b.broker, dev.server, wire.publish({
    topic: `mayap/v1/${DEV}/command`, qos: 1, packetId: 2, payload: 'y',
  }));
  assert.equal(dev.server.closed, true);
});

test('PUBLISH never upgrades QoS on fanout (snapshot QoS0 stays QoS0)', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const dev = await connectDevice(b.broker);
  const web = await connectWeb(b.broker);
  // Web subscribes at QoS1.
  await harness.feed(b.broker, web.server, wire.subscribe({
    packetId: 1, filters: [{ filter: `mayap/v1/${DEV}/snapshot`, qos: 1 }],
  }));
  await harness.clientReceive(web.client);
  // Device publishes at QoS0.
  await harness.feed(b.broker, dev.server, wire.publish({
    topic: `mayap/v1/${DEV}/snapshot`, qos: 0, payload: '{"t":36}',
  }));
  const frames = await harness.clientReceive(web.client);
  const pub = wire.parsePublish(frames[0]);
  assert.equal(pub.qos, 0);
});

test('retain: presence retained, new subscriber receives', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const dev = await connectDevice(b.broker);
  await harness.feed(b.broker, dev.server, wire.publish({
    topic: `mayap/v1/${DEV}/presence`, qos: 1, retain: true, packetId: 1,
    payload: '{"online":true}',
  }));
  await harness.clientReceive(dev.client); // puback

  const web = await connectWeb(b.broker);
  await harness.feed(b.broker, web.server, wire.subscribe({
    packetId: 1, filters: [{ filter: `mayap/v1/${DEV}/presence`, qos: 1 }],
  }));
  const frames = await harness.clientReceive(web.client);
  // frame[0] = suback, frame[1] = retained PUBLISH.
  assert.equal(wire.parseSuback(frames[0]).codes[0], 1);
  const pub = wire.parsePublish(frames[1]);
  assert.equal(pub.topic, `mayap/v1/${DEV}/presence`);
  assert.equal(pub.retain, true);
  assert.equal(pub.payload.toString('utf-8'), '{"online":true}');
});

test('retain: empty payload with retain=1 clears retained', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const dev = await connectDevice(b.broker);
  await harness.feed(b.broker, dev.server, wire.publish({
    topic: `mayap/v1/${DEV}/presence`, qos: 1, retain: true, packetId: 1,
    payload: '{"online":true}',
  }));
  await harness.clientReceive(dev.client);
  await harness.feed(b.broker, dev.server, wire.publish({
    topic: `mayap/v1/${DEV}/presence`, qos: 1, retain: true, packetId: 2,
    payload: '',
  }));
  await harness.clientReceive(dev.client);
  const web = await connectWeb(b.broker);
  await harness.feed(b.broker, web.server, wire.subscribe({
    packetId: 1, filters: [{ filter: `mayap/v1/${DEV}/presence`, qos: 1 }],
  }));
  const frames = await harness.clientReceive(web.client);
  // Only suback — nothing retained.
  assert.equal(frames.length, 1);
  assert.equal(wire.parseSuback(frames[0]).codes[0], 1);
});

test('retain forbidden on command topic even if client sets retain=1', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const web = await connectWeb(b.broker);
  await harness.feed(b.broker, web.server, wire.publish({
    topic: `mayap/v1/${DEV}/command`, qos: 1, retain: true, packetId: 1,
    payload: 'x',
  }));
  await harness.clientReceive(web.client); // puback
  // Verify storage holds no retained entry.
  const list = await b.state.storage.list({ prefix: 'retained:' });
  assert.equal(list.size, 0);
});

test('LWT: unclean close publishes will to subscribers', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const web = await connectWeb(b.broker);
  await harness.feed(b.broker, web.server, wire.subscribe({
    packetId: 1, filters: [{ filter: `mayap/v1/${DEV}/presence`, qos: 1 }],
  }));
  await harness.clientReceive(web.client);
  const dev = await connectDevice(b.broker);
  // Unclean close: broker calls webSocketClose with wasClean=false.
  await b.broker.webSocketClose(dev.server, 1006, 'abnormal', false);
  const frames = await harness.clientReceive(web.client);
  // Expect the will PUBLISH.
  const pub = wire.parsePublish(frames[0]);
  assert.equal(pub.topic, `mayap/v1/${DEV}/presence`);
  assert.equal(pub.payload.toString('utf-8'), '{"online":false}');
});

test('clean DISCONNECT suppresses LWT', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const web = await connectWeb(b.broker);
  await harness.feed(b.broker, web.server, wire.subscribe({
    packetId: 1, filters: [{ filter: `mayap/v1/${DEV}/presence`, qos: 1 }],
  }));
  await harness.clientReceive(web.client);
  const dev = await connectDevice(b.broker);
  await harness.feed(b.broker, dev.server, wire.disconnect());
  await b.broker.webSocketClose(dev.server, 1000, 'done', true);
  const frames = await harness.clientReceive(web.client);
  assert.equal(frames.length, 0);
});

test('PINGREQ → PINGRESP', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const dev = await connectDevice(b.broker);
  await harness.feed(b.broker, dev.server, wire.pingreq());
  const frames = await harness.clientReceive(dev.client);
  assert.equal(wire.parsePingresp(frames[0]), true);
});

test('malformed packet → broker closes without crashing', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const dev = await connectDevice(b.broker);
  // Send a PUBLISH with QoS=3 (reserved) — codec rejects.
  const bad = new Uint8Array([0x3c, 0x02, 0x00, 0x01]); // 0x3c = PUBLISH, QoS=11
  await harness.feed(b.broker, dev.server, bad);
  assert.equal(dev.server.closed, true);
});

test('oversized packet (> 4 KiB) causes close with no CONNACK', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const { client, server } = await harness.openWebSocket(b.broker, DEV);
  // Fixed header announcing 5000-byte body.
  const big = new Uint8Array([0x10, 0x88, 0x27]); // 0x10 = CONNECT, VarLen 5000
  await harness.feed(b.broker, server, big);
  assert.equal(server.closed, true);
  const frames = await harness.clientReceive(client);
  assert.equal(frames.length, 0);
});

test('non-CONNECT before CONNECT → close', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const { client, server } = await harness.openWebSocket(b.broker, DEV);
  await harness.feed(b.broker, server, wire.pingreq());
  assert.equal(server.closed, true);
});

test('text frame rejected', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const { server } = await harness.openWebSocket(b.broker, DEV);
  await b.broker.webSocketMessage(server, 'hello');
  assert.equal(server.closed, true);
});

test('inflight ring is bounded (16)', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const dev = await connectDevice(b.broker);
  await harness.feed(b.broker, dev.server, wire.subscribe({
    packetId: 1, filters: [{ filter: `mayap/v1/${DEV}/command`, qos: 1 }],
  }));
  await harness.clientReceive(dev.client);
  const web = await connectWeb(b.broker);
  for (let i = 0; i < 20; i++) {
    await harness.feed(b.broker, web.server, wire.publish({
      topic: `mayap/v1/${DEV}/command`, qos: 1, packetId: 100 + i,
      payload: `cmd-${i}`,
    }));
  }
  // Device never PUBACKs — 16 stay in flight, the rest wait in the bounded queue (not in the in-flight ring).
  const att = dev.server.deserializeAttachment();
  assert.ok(att.inflight.length <= 16, `inflight=${att.inflight.length}`);
  assert.equal(att.inflight.length + att.pend, 20);
});

test('hibernation: retained + attachment survive broker re-instantiation', async () => {
  let b = await harness.makeBroker({ env: envFixture() });
  const dev = await connectDevice(b.broker);
  await harness.feed(b.broker, dev.server, wire.publish({
    topic: `mayap/v1/${DEV}/reminders/reported`, qos: 1, retain: true, packetId: 1,
    payload: '{"revision":42}',
  }));
  await harness.clientReceive(dev.client);

  // "Hibernate": construct a fresh DO against the same state.
  b = await harness.simulateHibernate(b, envFixture());

  // New web subscriber attaches via the still-live server socket? For the
  // stub this means we open a brand-new ws against the woken DO.
  const web = await connectWeb(b.broker);
  await harness.feed(b.broker, web.server, wire.subscribe({
    packetId: 1, filters: [{ filter: `mayap/v1/${DEV}/reminders/reported`, qos: 1 }],
  }));
  const frames = await harness.clientReceive(web.client);
  const pub = wire.parsePublish(frames[1]);
  assert.equal(pub.topic, `mayap/v1/${DEV}/reminders/reported`);
  assert.equal(pub.retain, true);
  assert.equal(pub.payload.toString('utf-8'), '{"revision":42}');
});

test('hibernation: decoder buffer survives mid-packet (partial feed then wake)', async () => {
  let b = await harness.makeBroker({ env: envFixture() });
  const { client, server } = await harness.openWebSocket(b.broker, DEV);
  const full = wire.connect({
    clientId: `esp-${DEV}`, username: DEV, password: DEV_PWD,
  });
  const half = full.slice(0, Math.floor(full.length / 2));
  const rest = full.slice(Math.floor(full.length / 2));
  await harness.feed(b.broker, server, half);
  // Nothing sent yet.
  assert.deepEqual(await harness.clientReceive(client), []);

  // The server socket persists across hibernation in the stub; the new DO
  // must rebuild the decoder from the attachment's bufferB64.
  const woken = await harness.simulateHibernate(b, envFixture());
  await harness.feed(woken.broker, server, rest);
  const frames = await harness.clientReceive(client);
  const parsed = wire.parseConnack(frames[0]);
  assert.equal(parsed.returnCode, 0);
});

test('keepalive out of range rejected with CONNACK 5', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const { client, server } = await harness.openWebSocket(b.broker, DEV);
  await harness.feed(b.broker, server, wire.connect({
    clientId: 'c', username: DEV, password: DEV_PWD, keepalive: 9000,
  }));
  const [frame] = await harness.clientReceive(client);
  assert.equal(wire.parseConnack(frame).returnCode, 5);
  assert.equal(server.closed, true);
});

test('path mismatch (bad device id) → 400', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const res = await b.broker.fetch(new Request('https://x.test/mqtt/not-a-device', {
    headers: { Upgrade: 'websocket' },
  }));
  assert.equal(res.status, 400);
});

test('device-mismatch (second deviceId on same DO) → 409', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  await connectDevice(b.broker);
  const res = await b.broker.fetch(new Request(`https://x.test/mqtt/MAP-AABBCCDDEEFF`, {
    headers: { Upgrade: 'websocket' },
  }));
  assert.equal(res.status, 409);
});

test('LWT on forbidden-retain topic refused', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const { client, server } = await harness.openWebSocket(b.broker, DEV);
  await harness.feed(b.broker, server, wire.connect({
    clientId: `esp-${DEV}`, username: DEV, password: DEV_PWD,
    will: {
      topic: `mayap/v1/${DEV}/snapshot`, qos: 1, retain: true, payload: 'x',
    },
  }));
  const [frame] = await harness.clientReceive(client);
  // snapshot is retain-forbidden → CONNACK NOT_AUTHORIZED (5).
  assert.equal(wire.parseConnack(frame).returnCode, 5);
  assert.equal(server.closed, true);
});

test('web role cannot subscribe to a non-device-scope topic', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const web = await connectWeb(b.broker);
  await harness.feed(b.broker, web.server, wire.subscribe({
    packetId: 1, filters: [{ filter: `other/root/thing`, qos: 1 }],
  }));
  const [sub] = await harness.clientReceive(web.client);
  assert.deepEqual(wire.parseSuback(sub).codes, [0x80]);
});

// ---- Device uplink (alarm / heartbeat): PUBACK only after the main Worker confirms a durable write ----
function ingestFixture(handler) {
  const calls = [];
  const env = { ...envFixture(), CLOUD_INGEST: { async fetch(url, init) {
    calls.push({ url, init, body: JSON.parse(init.body) });
    return handler(calls.length, init);
  } } };
  return { env, calls };
}
// The harness swaps the global Response for a 101-tolerant stub; the Worker's reply is a real one.
const NativeResponse = globalThis.Response;   // captured at load, before the harness replaces it
const reply = (body, status = 200) => new NativeResponse(body, { status });
const durable = (value = true) => reply(JSON.stringify({ success: true, durable: value }));
const alarmEvent = (id, extra = {}) => JSON.stringify({ event_id: id, alarm_type: 'FAULT_130', state: 'active', message: 'm', severity: 'critical', ...extra });

// Contract (intentionally changed from "PUBACK after D1"): PUBACK on `alarm`/`heartbeat` = BROKER_STORED. The Worker is
// reached afterwards from the DO alarm; a slow, failing or throttling Worker never touches any packet of the device link.
const publishAlarm = (b, dev, packetId, payload, topic = 'alarm') =>
  harness.feed(b.broker, dev.server, wire.publish({ topic: `mayap/v1/${DEV}/${topic}`, qos: 1, packetId, payload }));
const pubacks = async (dev) => (await harness.clientReceive(dev.client)).map((f) => wire.parsePuback(f).packetId);
const results = (events, outcome = 'stored', extra = {}) => reply(JSON.stringify({ success: true,
  results: events.map((e) => ({ event_id: e.event_id, outcome, ...extra })) }));

test('uplink alarm: PUBACK = BROKER_STORED before any Worker call; signed batched hand-off afterwards; never fanned out to Web', async () => {
  const { env, calls } = ingestFixture((n, init) => results(JSON.parse(init.body).events));
  const b = await harness.makeBroker({ env });
  const dev = await connectDevice(b.broker);
  const web = await connectWeb(b.broker);
  await harness.feed(b.broker, web.server, wire.subscribe({ packetId: 3, filters: [{ filter: `mayap/v1/${DEV}/#`, qos: 0 }] }));
  await harness.clientReceive(web.client);
  await publishAlarm(b, dev, 41, alarmEvent('e-1'));
  assert.deepEqual(await pubacks(dev), [41]);
  assert.equal(calls.length, 0, 'the PUBACK did not wait for the Worker');
  const row = [...b.state.storage.map.keys()].filter((k) => k.startsWith('uq:e:'));
  assert.equal(row.length, 1, 'the event is a durable row before the PUBACK');
  await b.broker.alarm();
  assert.equal(calls.length, 1);
  const { body, init } = calls[0];
  assert.deepEqual(body, { device_id: DEV, kind: 'alarms', events: [JSON.parse(alarmEvent('e-1'))] });
  const ts = init.headers['x-mayap-ts'];
  const expected = crypto.createHmac('sha256', DEV_SECRET).update(`mayap-uplink-ingest:v1\n${ts}\n${init.body}`).digest('hex');
  assert.equal(init.headers['x-mayap-sig'], expected);
  assert.equal((await b.broker.uplink.status()).pending, 0);
  assert.deepEqual((await harness.clientReceive(web.client)).filter((f) => f[0] >> 4 === 3 && !String(Buffer.from(f)).includes('/uplink')), []);   // nothing but status reaches subscribers
});

test('uplink: a failing, slow, throttling or missing Worker never withholds the PUBACK and never closes the link', async () => {
  for (const handler of [() => results([{ event_id: 'e-2' }], 'error'), () => reply('boom', 500), () => { throw new Error('binding down'); }]) {
    const { env } = ingestFixture(handler);
    const b = await harness.makeBroker({ env });
    const dev = await connectDevice(b.broker);
    await publishAlarm(b, dev, 5, alarmEvent('e-2'));
    assert.deepEqual(await pubacks(dev), [5]);
    await b.broker.alarm();
    const status = await b.broker.uplink.status();
    assert.equal(status.pending, 1);                                      // kept, visible, retried with backoff
    assert.equal(status.retrying, 1);
    assert.equal(dev.server.closed, false);
  }
  const b = await harness.makeBroker({ env: envFixture() });               // no CLOUD_INGEST binding: still stored, drain reports NO_INGEST
  const dev = await connectDevice(b.broker);
  await publishAlarm(b, dev, 6, '{"batch_running":false}', 'heartbeat');
  assert.deepEqual(await pubacks(dev), [6]);
  await publishAlarm(b, dev, 7, alarmEvent('e-3'));
  assert.deepEqual(await pubacks(dev), [7]);
  await b.broker.alarm();
  assert.equal((await b.broker.uplink.status()).last_error, 'NO_INGEST');
});

test('uplink: malformed payloads are dropped without PUBACK; web cannot publish or subscribe; per-type order and cooldown are kept', async () => {
  const seen = [];
  const verdict = { 'a-1': ['throttled', { retry_after_ms: 5000 }] };
  const { env } = ingestFixture(async (n, init) => {
    const events = JSON.parse(init.body).events;
    seen.push(events.map((e) => e.event_id));
    const failed = new Set();      // the Worker skips later events of an alarm type after one was not accepted
    return reply(JSON.stringify({ success: true, results: events.map((e) => {
      if (failed.has(e.alarm_type)) return { event_id: e.event_id, outcome: 'skipped' };
      const v = verdict[e.event_id];
      if (v && v[0] !== 'stored') failed.add(e.alarm_type);
      return v ? { event_id: e.event_id, outcome: v[0], ...v[1] } : { event_id: e.event_id, outcome: 'stored' }; }) }));
  });
  const b = await harness.makeBroker({ env });
  let clock = 1_000_000; b.broker._now = () => clock; b.broker.uplink.rand = () => 0.5;
  const dev = await connectDevice(b.broker);
  await publishAlarm(b, dev, 7, 'not json');
  await publishAlarm(b, dev, 8, JSON.stringify({ event_id: 'bad id!', alarm_type: 'X', message: 'm' }));
  assert.deepEqual(await pubacks(dev), []);
  // ACTIVE (throttled by the Worker cooldown) then RESOLVED of the same type, plus another type.
  await publishAlarm(b, dev, 9, alarmEvent('a-1'));
  await publishAlarm(b, dev, 10, alarmEvent('a-2', { state: 'resolved' }));
  await publishAlarm(b, dev, 11, alarmEvent('b-1', { alarm_type: 'FAULT_131' }));
  assert.deepEqual(await pubacks(dev), [9, 10, 11]);
  await b.broker.alarm();
  assert.deepEqual(seen, [['a-1', 'a-2', 'b-1']]);
  let status = await b.broker.uplink.status();
  assert.equal(status.throttled, 1);                                      // a-1 waits for the cooldown; a-2 (same type) is held behind it
  assert.equal(status.forwarded, 1);                                      // b-1 (other type) is not blocked
  clock += 1500; await b.broker.alarm();
  assert.equal(seen.length, 1, 'no retry before the cooldown elapsed');
  verdict['a-1'] = ['stored', {}];
  clock += 5000; await b.broker.alarm();
  assert.deepEqual(seen[1], ['a-1', 'a-2']);                              // order preserved: ACTIVE before RESOLVED
  status = await b.broker.uplink.status();
  assert.equal(status.pending, 0);
  const web = await connectWeb(b.broker);
  await harness.feed(b.broker, web.server, wire.subscribe({ packetId: 2, filters: [{ filter: `mayap/v1/${DEV}/alarm`, qos: 0 }] }));
  assert.deepEqual(wire.parseSuback((await harness.clientReceive(web.client))[0]).codes, [0x80]);
  await harness.feed(b.broker, web.server, wire.publish({ topic: `mayap/v1/${DEV}/alarm`, qos: 1, packetId: 3, payload: alarmEvent('forged') }));
  assert.equal((await b.broker.uplink.status()).pending, 0);               // the ACL stopped it before the queue
});

test('uplink: the queue is bounded; a full queue withholds the PUBACK (backpressure) instead of dropping silently', async () => {
  const { env } = ingestFixture(() => reply('boom', 503));
  const b = await harness.makeBroker({ env });
  const dev = await connectDevice(b.broker);
  for (let i = 0; i < 64; i++) await publishAlarm(b, dev, 100 + i, alarmEvent(`q-${i}`, { alarm_type: `T${i % 5}` }));
  assert.equal((await pubacks(dev)).length, 64);
  await publishAlarm(b, dev, 200, alarmEvent('q-over'));
  assert.deepEqual(await pubacks(dev), []);
  const status = await b.broker.uplink.status();
  assert.equal(status.pending, 64);
  assert.equal(status.overflow, 1);
  assert.equal(dev.server.closed, false);
  await publishAlarm(b, dev, 100, alarmEvent('q-0', { alarm_type: 'T0' }));    // a resend of a stored event is always acknowledged
  assert.deepEqual(await pubacks(dev), [100]);
});

test('uplink: stored events survive hibernation and drain after the Worker recovers; a lost Worker reply is retried safely', async () => {
  let workerUp = false;
  const stored = new Set();
  const handler = (n, init) => {
    const events = JSON.parse(init.body).events;
    if (!workerUp) return reply('down', 503);
    return reply(JSON.stringify({ success: true, results: events.map((e) => {
      const dup = stored.has(e.event_id); stored.add(e.event_id); return { event_id: e.event_id, outcome: dup ? 'duplicate' : 'stored' }; }) }));
  };
  const first = ingestFixture(handler);
  let b = await harness.makeBroker({ env: first.env });
  const dev = await connectDevice(b.broker);
  await publishAlarm(b, dev, 1, alarmEvent('h-1'));
  assert.deepEqual(await pubacks(dev), [1]);
  b = await harness.simulateHibernate(b, first.env);                        // DO evicted: only storage survives
  workerUp = true;
  await b.broker.alarm();
  assert.deepEqual([...stored], ['h-1']);
  assert.equal((await b.broker.uplink.status()).pending, 0);
  // Worker stored it but the reply was lost: the row stays pending and the retry gets `duplicate`, no second store.
  const lost = ingestFixture((n, init) => { handler(n, init); return reply('lost', 502); });
  const b2 = await harness.makeBroker({ env: lost.env });
  let clock = 2_000_000; b2.broker._now = () => clock; b2.broker.uplink.rand = () => 0.5;
  const dev2 = await connectDevice(b2.broker);
  await publishAlarm(b2, dev2, 1, alarmEvent('h-2'));
  await b2.broker.alarm();
  assert.equal((await b2.broker.uplink.status()).pending, 1);
  const ok = ingestFixture(handler);
  b2.broker.env.CLOUD_INGEST = ok.env.CLOUD_INGEST;
  clock += 2500; await b2.broker.alarm();
  assert.equal((await b2.broker.uplink.status()).pending, 0);
  assert.deepEqual([...stored].sort(), ['h-1', 'h-2']);
});

test('uplink: a slow Worker does not delay PINGRESP, other PUBACKs or the keep-alive supervisor (no await on the network in the packet path)', async () => {
  let release;
  const gate = new Promise((resolve) => { release = resolve; });
  const { env } = ingestFixture(async (n, init) => { await gate; return results(JSON.parse(init.body).events); });
  const b = await harness.makeBroker({ env });
  const dev = await connectDevice(b.broker);
  const t0 = Date.now();
  const alarmDone = (async () => { await publishAlarm(b, dev, 1, alarmEvent('s-1')); return b.broker.alarm(); })();
  await new Promise((resolve) => setTimeout(resolve, 20));
  await harness.feed(b.broker, dev.server, wire.pingreq());
  await publishAlarm(b, dev, 2, alarmEvent('s-2', { alarm_type: 'FAULT_131' }));
  await publishAlarm(b, dev, 3, '{"batch_running":true}', 'heartbeat');
  const frames = await harness.clientReceive(dev.client);
  const types = frames.map((f) => f[0] >> 4);
  assert.ok(types.includes(13), 'PINGRESP answered while the Worker call is still pending');
  assert.deepEqual(frames.filter((f) => f[0] >> 4 === 4).map((f) => wire.parsePuback(f).packetId).sort(), [1, 2, 3]);
  assert.ok(Date.now() - t0 < 1000);
  release(); await alarmDone;
  assert.equal((await b.broker.uplink.status()).pending, 0);
});

test('keep-alive supervision is deadline-driven, not a 15 s poll: the DO wakes at the earliest real deadline', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const base = Date.now();
  b.broker._now = () => base;
  const dev = await connectDevice(b.broker);
  await b.broker._rescheduleAlarm();
  const at = await b.state.storage.getAlarm();
  const keepaliveMs = (await b.broker.state.getWebSockets()[0].deserializeAttachment()).keepaliveMs;
  assert.ok(Math.abs(at - base - (keepaliveMs * 1.5 + 1000)) <= 1500, `alarm in ${at - base} ms (keep-alive ${keepaliveMs} ms x 1.5 + 1 s), not a fixed 15 s poll`);
  assert.ok(at - base > 15000);
  await harness.feed(b.broker, dev.server, wire.pingreq());
  // Idle with no sockets and no queue: no alarm at all.
  const empty = await harness.makeBroker({ env: envFixture() });
  await empty.broker._rescheduleAlarm();
  assert.equal(await empty.state.storage.getAlarm(), null);
});

test('uplink status topic: web gets the backlog live and once on subscribe; the device cannot read or write it', async () => {
  const { env } = ingestFixture(() => reply('down', 503));
  const b = await harness.makeBroker({ env });
  const dev = await connectDevice(b.broker);
  const web = await connectWeb(b.broker);
  await publishAlarm(b, dev, 1, alarmEvent('w-1'));
  await pubacks(dev);
  await harness.feed(b.broker, web.server, wire.subscribe({ packetId: 4, filters: [{ filter: `mayap/v1/${DEV}/uplink`, qos: 1 }] }));
  const frames = await harness.clientReceive(web.client);
  assert.deepEqual(wire.parseSuback(frames[0]).codes, [0]);                 // QoS0 only
  const first = JSON.parse(wire.parsePublish(frames[1]).payload.toString());
  assert.equal(first.pending, 1);
  await harness.feed(b.broker, dev.server, wire.subscribe({ packetId: 5, filters: [{ filter: `mayap/v1/${DEV}/uplink`, qos: 0 }] }));
  assert.deepEqual(wire.parseSuback((await harness.clientReceive(dev.client))[0]).codes, [0x80]);
});

test('uplink: QoS0 or retained alarm publishes are protocol misuse and never reach the Worker', async () => {
  const { env, calls } = ingestFixture(() => durable());
  const b = await harness.makeBroker({ env });
  const dev = await connectDevice(b.broker);
  await harness.feed(b.broker, dev.server, wire.publish({ topic: `mayap/v1/${DEV}/alarm`, qos: 0, payload: alarmEvent('q0') }));
  await harness.feed(b.broker, dev.server, wire.publish({ topic: `mayap/v1/${DEV}/heartbeat`, qos: 1, retain: true, packetId: 4, payload: '{}' }));
  assert.equal(calls.length, 0);
});

test('web socket lifetime: a socket that outlives its token (not replaced by the Web) is closed after a bounded grace; the device never is', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const base = Date.now();
  let clock = base; b.broker._now = () => clock;
  const dev = await connectDevice(b.broker);
  const { client, server } = await harness.openWebSocket(b.broker, DEV);
  await harness.feed(b.broker, server, wire.connect({ clientId: 'web-life', username: 'web:u1', password: webToken('web:u1', DEV, 600) }));
  assert.equal(wire.parseConnack((await harness.clientReceive(client))[0]).returnCode, 0);
  assert.ok((server.deserializeAttachment().tokenExp - base) > 590000);
  clock = base + 600000 + 60000;                       // token expired a minute ago: still inside the grace
  server.serializeAttachment({ ...server.deserializeAttachment(), lastRxMs: clock });
  dev.server.serializeAttachment({ ...dev.server.deserializeAttachment(), lastRxMs: clock });
  await b.broker.alarm();
  assert.equal(server.closed, false);
  clock = base + 600000 + 125000 + 1000;               // past exp + grace
  server.serializeAttachment({ ...server.deserializeAttachment(), lastRxMs: clock });
  dev.server.serializeAttachment({ ...dev.server.deserializeAttachment(), lastRxMs: clock });
  await b.broker.alarm();
  assert.equal(server.closed, true);
  assert.equal(server.closeReason, 'TOKEN_EXPIRED');
  assert.equal(dev.server.closed, false);              // a device socket has no token lifetime
});

test('re-scheduling from socket events never postpones a retry that is already due soon (probe/reconnect storms cannot starve the uplink queue)', async () => {
  let calls = 0;
  const { env } = ingestFixture(() => { calls += 1; return reply('down', 503); });
  const b = await harness.makeBroker({ env });
  let clock = 5_000_000; b.broker._now = () => clock; b.broker.uplink.rand = () => 0.5;
  const dev = await connectDevice(b.broker);
  await publishAlarm(b, dev, 1, alarmEvent('s-1'));
  await b.broker.alarm();                                   // first attempt fails: next retry at +2000
  const due = await b.state.storage.getAlarm();
  assert.equal(due, clock + 2000);
  for (let n = 0; n < 10; n++) { clock += 150; await b.broker._rescheduleAlarm(); }   // socket events keep re-scheduling
  assert.equal(await b.state.storage.getAlarm(), due, 'the retry deadline did not move');
  clock = due + 1; await b.broker.alarm();
  assert.equal(calls, 2);
});
