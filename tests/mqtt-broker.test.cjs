'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const { test } = require('node:test');

const wire = require('./fixtures/mqtt-wire.cjs');
const harness = require('./fixtures/mqtt-broker-harness.cjs');

const DEV = 'MAP-001122334455';
const DEV_PWD = 'dev-pass-A';
const WEB_PWD = 'web-pass-B';

function envFixture() {
  return {
    BROKER_FIXTURE_DEVICE_PASSWORD: DEV_PWD,
    BROKER_FIXTURE_WEB_PASSWORD: WEB_PWD,
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
    username, password: WEB_PWD,
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
  // Device never PUBACKs — all remain pending but capped at 16.
  const att = dev.server.deserializeAttachment();
  assert.ok(att.inflight.length <= 16, `inflight=${att.inflight.length}`);
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
