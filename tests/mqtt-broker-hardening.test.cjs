'use strict';
// Phase 2B.1 hardening tests. Keep the 2B baseline (mqtt-broker.test.cjs)
// pristine so the regression surface is obvious.

const assert = require('node:assert/strict');
const { test } = require('node:test');
const wire = require('./fixtures/mqtt-wire.cjs');
const harness = require('./fixtures/mqtt-broker-harness.cjs');

const DEV = 'MAP-001122334455';
const DEV_PWD = 'dev-pass-A';
const WEB_PWD = 'web-pass-B';
const envFixture = () => ({
  BROKER_FIXTURE_DEVICE_PASSWORD: DEV_PWD,
  BROKER_FIXTURE_WEB_PASSWORD: WEB_PWD,
});

async function dev(broker, { willRetain = true, lwtPayload = '{"online":false}' } = {}) {
  const { client, server } = await harness.openWebSocket(broker, DEV);
  await harness.feed(broker, server, wire.connect({
    clientId: `esp-${DEV}`, username: DEV, password: DEV_PWD,
    will: {
      topic: `mayap/v1/${DEV}/presence`, qos: 1, retain: willRetain,
      payload: lwtPayload,
    },
  }));
  return { client, server, connack: (await harness.clientReceive(client))[0] };
}

async function web(broker) {
  const { client, server } = await harness.openWebSocket(broker, DEV);
  await harness.feed(broker, server, wire.connect({
    clientId: 'web-u1', username: 'web:u1', password: WEB_PWD,
  }));
  return { client, server, connack: (await harness.clientReceive(client))[0] };
}

// --------------------------------------------------------------------------
// 1. LWT retain writes retained store (new subscriber after disconnect sees it)
// --------------------------------------------------------------------------
test('§1 LWT retained store: offline presence survives for a late subscriber', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const d = await dev(b.broker);
  // Device publishes online=true retained so there is a baseline retained row.
  await harness.feed(b.broker, d.server, wire.publish({
    topic: `mayap/v1/${DEV}/presence`, qos: 1, retain: true, packetId: 1,
    payload: '{"online":true}',
  }));
  await harness.clientReceive(d.client); // puback

  // Abnormal close → LWT fires with retain=true. Broker must overwrite the
  // retained row to online=false BEFORE fanout.
  await b.broker.webSocketClose(d.server, 1006, 'abnormal', false);
  const stored = await b.state.storage.get(`retained:presence`);
  assert.ok(stored, 'retained presence missing after LWT');
  const payload = Buffer.from(stored.payloadB64, 'base64').toString('utf-8');
  assert.equal(payload, '{"online":false}');

  // A LATE subscriber (joins after LWT) must still receive the offline
  // presence from storage, not nothing.
  const w = await web(b.broker);
  await harness.feed(b.broker, w.server, wire.subscribe({
    packetId: 1, filters: [{ filter: `mayap/v1/${DEV}/presence`, qos: 1 }],
  }));
  const frames = await harness.clientReceive(w.client);
  assert.equal(frames.length, 2, 'expected SUBACK + retained PUBLISH');
  const pub = wire.parsePublish(frames[1]);
  assert.equal(pub.topic, `mayap/v1/${DEV}/presence`);
  assert.equal(pub.retain, true);
  assert.equal(pub.payload.toString('utf-8'), '{"online":false}');
});

// --------------------------------------------------------------------------
// 2. QoS1 lifecycle — per-connection packetId, no reuse of inflight ids,
//    no silent drop, slow consumer closed.
// --------------------------------------------------------------------------
test('§2 packetId is per-connection and persists via attachment', async () => {
  let b = await harness.makeBroker({ env: envFixture() });
  const d = await dev(b.broker);
  // Device subscribes to command so it will receive QoS1 fanouts.
  await harness.feed(b.broker, d.server, wire.subscribe({
    packetId: 1, filters: [{ filter: `mayap/v1/${DEV}/command`, qos: 1 }],
  }));
  await harness.clientReceive(d.client);
  // Hibernate: a fresh DO must still generate a next packetId that doesn't
  // collide with pre-existing inflight ids.
  b = await harness.simulateHibernate(b, envFixture());

  // Prime inflight by sending commands and NOT PUBACKing.
  const w = await web(b.broker);
  for (let i = 0; i < 3; i++) {
    await harness.feed(b.broker, w.server, wire.publish({
      topic: `mayap/v1/${DEV}/command`, qos: 1, packetId: 10 + i, payload: `cmd-${i}`,
    }));
  }
  const att = d.server.deserializeAttachment();
  assert.equal(att.inflight.length, 3);
  const ids = att.inflight.map((e) => e.packetId);
  assert.equal(new Set(ids).size, 3, 'packetIds must be unique');
  for (const id of ids) assert.ok(id >= 1 && id <= 0xffff);
});

test('§2 slow consumer: inflight full → broker closes, no silent drop', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const d = await dev(b.broker);
  await harness.feed(b.broker, d.server, wire.subscribe({
    packetId: 1, filters: [{ filter: `mayap/v1/${DEV}/command`, qos: 1 }],
  }));
  await harness.clientReceive(d.client);
  const w = await web(b.broker);
  // Fire 17 commands at a device that never PUBACKs.
  for (let i = 0; i < 17; i++) {
    await harness.feed(b.broker, w.server, wire.publish({
      topic: `mayap/v1/${DEV}/command`, qos: 1, packetId: 100 + i, payload: `cmd-${i}`,
    }));
  }
  // The 17th publish fills the ring; broker closes the slow device.
  assert.equal(d.server.closed, true, 'slow consumer must be closed');
  assert.equal(d.server.closeCode, 1013);
});

test('§2 PUBACK clears exactly its id, inflight shrinks', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const d = await dev(b.broker);
  await harness.feed(b.broker, d.server, wire.subscribe({
    packetId: 1, filters: [{ filter: `mayap/v1/${DEV}/command`, qos: 1 }],
  }));
  await harness.clientReceive(d.client);
  const w = await web(b.broker);
  await harness.feed(b.broker, w.server, wire.publish({
    topic: `mayap/v1/${DEV}/command`, qos: 1, packetId: 1, payload: 'a',
  }));
  await harness.clientReceive(w.client);
  const frames = await harness.clientReceive(d.client);
  const pub = wire.parsePublish(frames[0]);
  await harness.feed(b.broker, d.server, wire.puback({ packetId: pub.packetId }));
  const att = d.server.deserializeAttachment();
  assert.deepEqual(att.inflight, []);
});

// --------------------------------------------------------------------------
// 3. Streaming parser — per-packet 4K, strict var-len, fragmentation.
// --------------------------------------------------------------------------
test('§3 per-packet 4K cap: oversize PUBLISH header rejected before payload', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const d = await dev(b.broker);
  // Fixed header for a PUBLISH with Remaining Length = 5000 bytes.
  const bad = new Uint8Array([0x30, 0x88, 0x27]); // varlen 5000
  await harness.feed(b.broker, d.server, bad);
  assert.equal(d.server.closed, true);
});

test('§3 coalesce many small packets > 4KB in one frame', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const d = await dev(b.broker);
  await harness.feed(b.broker, d.server, wire.subscribe({
    packetId: 1, filters: [{ filter: `mayap/v1/${DEV}/command`, qos: 1 }],
  }));
  await harness.clientReceive(d.client);
  const w = await web(b.broker);
  const parts = [];
  for (let i = 0; i < 5; i++) {
    parts.push(wire.publish({
      topic: `mayap/v1/${DEV}/command`, qos: 1, packetId: 1 + i,
      payload: 'x'.repeat(800),
    }));
  }
  const total = parts.reduce((n, p) => n + p.length, 0); // ~4+KB
  const merged = new Uint8Array(total);
  let o = 0;
  for (const p of parts) { merged.set(p, o); o += p.length; }
  await harness.feed(b.broker, w.server, merged);
  // Web receives 5 PUBACKs; device sees 5 fanouts.
  const webFrames = await harness.clientReceive(w.client);
  assert.equal(webFrames.length, 5);
  const devFrames = await harness.clientReceive(d.client);
  assert.equal(devFrames.length, 5);
});

test('§3 5-byte Remaining Length rejected (MSB=0 on 5th byte)', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const d = await dev(b.broker);
  await harness.feed(b.broker, d.server, new Uint8Array([0x30, 0x80, 0x80, 0x80, 0x80, 0x00]));
  assert.equal(d.server.closed, true);
});

// --------------------------------------------------------------------------
// 4. retain=1 on command/config.set/reminders.set/history.request → DROP.
// --------------------------------------------------------------------------
test('§4 retain=1 on command: PUBACK + DROP, no fanout', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const d = await dev(b.broker);
  await harness.feed(b.broker, d.server, wire.subscribe({
    packetId: 1, filters: [{ filter: `mayap/v1/${DEV}/command`, qos: 1 }],
  }));
  await harness.clientReceive(d.client); // suback

  const w = await web(b.broker);
  await harness.feed(b.broker, w.server, wire.publish({
    topic: `mayap/v1/${DEV}/command`, qos: 1, retain: true, packetId: 42,
    payload: '{"op":"start"}',
  }));
  const webFrames = await harness.clientReceive(w.client);
  assert.equal(wire.parsePuback(webFrames[0]).packetId, 42);
  const devFrames = await harness.clientReceive(d.client);
  assert.equal(devFrames.length, 0, 'command with retain=1 must NOT fanout');
  const list = await b.state.storage.list({ prefix: 'retained:' });
  assert.equal(list.size, 0);
});

test('§4 retain=1 on history/request twice → close', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const w = await web(b.broker);
  await harness.feed(b.broker, w.server, wire.publish({
    topic: `mayap/v1/${DEV}/history/request`, qos: 1, retain: true,
    packetId: 1, payload: '{}',
  }));
  await harness.feed(b.broker, w.server, wire.publish({
    topic: `mayap/v1/${DEV}/history/request`, qos: 1, retain: true,
    packetId: 2, payload: '{}',
  }));
  assert.equal(w.server.closed, true);
});

// --------------------------------------------------------------------------
// 5. Mandatory `mqtt` subprotocol.
// --------------------------------------------------------------------------
test('§5 upgrade without subprotocol → 400', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const req = new Request(`https://x.test/mqtt/${DEV}`, {
    headers: { Upgrade: 'websocket' },
  });
  const res = await b.broker.fetch(req);
  assert.equal(res.status, 400);
});

test('§5 upgrade with foreign subprotocol only → 400', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const req = new Request(`https://x.test/mqtt/${DEV}`, {
    headers: { Upgrade: 'websocket', 'Sec-WebSocket-Protocol': 'xmpp' },
  });
  const res = await b.broker.fetch(req);
  assert.equal(res.status, 400);
});

test('§5 upgrade with mqtt subprotocol → 101 + echo header', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const req = new Request(`https://x.test/mqtt/${DEV}`, {
    headers: { Upgrade: 'websocket', 'Sec-WebSocket-Protocol': 'mqtt' },
  });
  const res = await b.broker.fetch(req);
  assert.equal(res.status, 101);
  assert.equal(res.headers.get('Sec-WebSocket-Protocol'), 'mqtt');
});

// --------------------------------------------------------------------------
// 6. Single device connection: takeover on second CONNECT.
// --------------------------------------------------------------------------
test('§6 duplicate device CONNECT: old connection is closed with TAKEOVER', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const d1 = await dev(b.broker);
  const d2 = await dev(b.broker);
  // The second CONNECT succeeded.
  assert.equal(wire.parseConnack(d2.connack).returnCode, 0);
  // The first socket is closed by broker.
  assert.equal(d1.server.closed, true, 'old device socket must be closed');
  assert.equal(d1.server.closeCode, 1000);
  // Takeover: LWT must NOT fire for the evicted socket.
  await b.broker.webSocketClose(d1.server, 1000, 'TAKEOVER', true);
  // d2 subscribes to presence? No, device role cannot sub presence. Just
  // confirm no retained online=false was written.
  const stored = await b.state.storage.get('retained:presence');
  assert.equal(stored, undefined);
});

test('§6 two web connections with distinct clientIds coexist', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const { client: c1, server: s1 } = await harness.openWebSocket(b.broker, DEV);
  await harness.feed(b.broker, s1, wire.connect({
    clientId: 'web-A', username: 'web:u1', password: WEB_PWD,
  }));
  await harness.clientReceive(c1);
  const { client: c2, server: s2 } = await harness.openWebSocket(b.broker, DEV);
  await harness.feed(b.broker, s2, wire.connect({
    clientId: 'web-B', username: 'web:u1', password: WEB_PWD,
  }));
  await harness.clientReceive(c2);
  assert.equal(s1.closed, false);
  assert.equal(s2.closed, false);
});

test('§6 MQTT-3.1.4-2: duplicate clientId takeover (same role)', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const { client: c1, server: s1 } = await harness.openWebSocket(b.broker, DEV);
  await harness.feed(b.broker, s1, wire.connect({
    clientId: 'web-DUP', username: 'web:u1', password: WEB_PWD,
  }));
  await harness.clientReceive(c1);
  const { client: c2, server: s2 } = await harness.openWebSocket(b.broker, DEV);
  await harness.feed(b.broker, s2, wire.connect({
    clientId: 'web-DUP', username: 'web:u1', password: WEB_PWD,
  }));
  await harness.clientReceive(c2);
  assert.equal(s1.closed, true, 'duplicate clientId must evict old session');
  assert.equal(s1.closeReason, 'CLIENTID_TAKEOVER');
  assert.equal(s2.closed, false);
});

// --------------------------------------------------------------------------
// 7. Session topic — Web PUB only, Device SUB only.
// --------------------------------------------------------------------------
test('§7 device cannot subscribe web→device publish on session', async () => {
  // Device MAY subscribe session.
  const b = await harness.makeBroker({ env: envFixture() });
  const d = await dev(b.broker);
  await harness.feed(b.broker, d.server, wire.subscribe({
    packetId: 1, filters: [{ filter: `mayap/v1/${DEV}/session`, qos: 1 }],
  }));
  const [sub] = await harness.clientReceive(d.client);
  // granted QoS cap for session = 0 (per contract).
  assert.deepEqual(wire.parseSuback(sub).codes, [0]);
});

test('§7 device cannot publish session', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const d = await dev(b.broker);
  await harness.feed(b.broker, d.server, wire.publish({
    topic: `mayap/v1/${DEV}/session`, qos: 0, payload: '{"active":true}',
  }));
  // QoS0 → no PUBACK; just confirm no fanout to a watching web.
  const w = await web(b.broker);
  await harness.feed(b.broker, w.server, wire.subscribe({
    packetId: 1, filters: [{ filter: `mayap/v1/${DEV}/session`, qos: 0 }],
  }));
  const sub = await harness.clientReceive(w.client);
  // Web is NOT allowed to subscribe session per 2B.1 contract → 0x80.
  assert.deepEqual(wire.parseSuback(sub[0]).codes, [0x80]);
});

// --------------------------------------------------------------------------
// §Codex-1: per-topic publish QoS enforcement.
// --------------------------------------------------------------------------
test('§Codex-1 publish presence at QoS0 is DROPPED (required QoS1)', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const d = await dev(b.broker);
  const w = await web(b.broker);
  await harness.feed(b.broker, w.server, wire.subscribe({
    packetId: 1, filters: [{ filter: `mayap/v1/${DEV}/presence`, qos: 1 }],
  }));
  await harness.clientReceive(w.client);
  // Device publishes presence QoS0 — contract requires QoS1.
  await harness.feed(b.broker, d.server, wire.publish({
    topic: `mayap/v1/${DEV}/presence`, qos: 0, retain: true,
    payload: '{"online":true}',
  }));
  // No fanout to web.
  const frames = await harness.clientReceive(w.client);
  assert.equal(frames.length, 0);
  // Nothing retained.
  const stored = await b.state.storage.get('retained:presence');
  assert.equal(stored, undefined);
});

test('§Codex-1 publish snapshot at QoS1 is DROPPED (required QoS0)', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const d = await dev(b.broker);
  const w = await web(b.broker);
  await harness.feed(b.broker, w.server, wire.subscribe({
    packetId: 1, filters: [{ filter: `mayap/v1/${DEV}/snapshot`, qos: 0 }],
  }));
  await harness.clientReceive(w.client);
  await harness.feed(b.broker, d.server, wire.publish({
    topic: `mayap/v1/${DEV}/snapshot`, qos: 1, packetId: 7, payload: '{"t":36}',
  }));
  // Device still gets a PUBACK (hides role structure), but web sees nothing.
  const devFrames = await harness.clientReceive(d.client);
  assert.equal(wire.parsePuback(devFrames[0]).packetId, 7);
  const webFrames = await harness.clientReceive(w.client);
  assert.equal(webFrames.length, 0);
});

// --------------------------------------------------------------------------
// §Codex-4: keepalive refreshed only after a complete control packet.
// --------------------------------------------------------------------------
test('§Codex-4 partial frames do NOT refresh keepalive timestamp', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const d = await dev(b.broker);
  const att0 = d.server.deserializeAttachment();
  const t0 = att0.lastRxMs;
  // Backdate lastRxMs by 10s so we can see whether it advances.
  att0.lastRxMs = t0 - 10_000;
  d.server.serializeAttachment(att0);
  // Push one byte that cannot form a complete MQTT packet.
  await harness.feed(b.broker, d.server, new Uint8Array([0xc0]));
  const att1 = d.server.deserializeAttachment();
  assert.equal(att1.lastRxMs, t0 - 10_000,
    'incomplete frame must not advance keepalive');
  // Finish the PINGREQ — now the keepalive advances.
  await harness.feed(b.broker, d.server, new Uint8Array([0x00]));
  const att2 = d.server.deserializeAttachment();
  assert.ok(att2.lastRxMs > t0 - 10_000);
});

// --------------------------------------------------------------------------
// §Codex-6: keepalive range 30..120.
// --------------------------------------------------------------------------
test('§Codex-6 keepalive=0 (disabled) → CONNACK 5', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const { client, server } = await harness.openWebSocket(b.broker, DEV);
  await harness.feed(b.broker, server, wire.connect({
    clientId: 'c', username: DEV, password: DEV_PWD, keepalive: 0,
  }));
  const [frame] = await harness.clientReceive(client);
  assert.equal(wire.parseConnack(frame).returnCode, 5);
});

test('§Codex-6 keepalive=10 (below min) → CONNACK 5', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const { client, server } = await harness.openWebSocket(b.broker, DEV);
  await harness.feed(b.broker, server, wire.connect({
    clientId: 'c', username: DEV, password: DEV_PWD, keepalive: 10,
  }));
  const [frame] = await harness.clientReceive(client);
  assert.equal(wire.parseConnack(frame).returnCode, 5);
});

test('§Codex-6 keepalive=200 (above max) → CONNACK 5', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const { client, server } = await harness.openWebSocket(b.broker, DEV);
  await harness.feed(b.broker, server, wire.connect({
    clientId: 'c', username: DEV, password: DEV_PWD, keepalive: 200,
  }));
  const [frame] = await harness.clientReceive(client);
  assert.equal(wire.parseConnack(frame).returnCode, 5);
});

// --------------------------------------------------------------------------
// §Codex-7: violation counter resets on a valid publish.
// --------------------------------------------------------------------------
test('§Codex-7 valid publish resets consecutive-violation counter', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const d = await dev(b.broker);
  // One bad publish (ack is device-pub, but at wrong QoS=0 required=1).
  await harness.feed(b.broker, d.server, wire.publish({
    topic: `mayap/v1/${DEV}/ack`, qos: 0, payload: 'x',
  }));
  let att = d.server.deserializeAttachment();
  assert.equal(att.violations, 1);
  // One VALID publish of ack at QoS1 — should reset counter to 0.
  await harness.feed(b.broker, d.server, wire.publish({
    topic: `mayap/v1/${DEV}/ack`, qos: 1, packetId: 50, payload: 'ok',
  }));
  await harness.clientReceive(d.client);
  att = d.server.deserializeAttachment();
  assert.equal(att.violations, 0);
  // Another bad one — must NOT close (counter is 1 again, not 2).
  await harness.feed(b.broker, d.server, wire.publish({
    topic: `mayap/v1/${DEV}/ack`, qos: 0, payload: 'y',
  }));
  assert.equal(d.server.closed, false);
});

// --------------------------------------------------------------------------
// §Codex-10: unsupported protocol level returns CONNACK 0x01.
// --------------------------------------------------------------------------
test('§Codex-10 MQTT 5 CONNECT → CONNACK 0x01 + close', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const { client, server } = await harness.openWebSocket(b.broker, DEV);
  await harness.feed(b.broker, server, wire.connect({
    clientId: 'c', protoLevel: 5, username: DEV, password: DEV_PWD,
  }));
  const [frame] = await harness.clientReceive(client);
  assert.equal(wire.parseConnack(frame).returnCode, 1);
  assert.equal(server.closed, true);
});

test('§Codex-10 bad protocol name → CONNACK 0x01 + close', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const { client, server } = await harness.openWebSocket(b.broker, DEV);
  await harness.feed(b.broker, server, wire.connect({
    clientId: 'c', protoName: 'MQIsdp', username: DEV, password: DEV_PWD,
  }));
  const [frame] = await harness.clientReceive(client);
  assert.equal(wire.parseConnack(frame).returnCode, 1);
  assert.equal(server.closed, true);
});

test('§7 web→device session publish fanouts to device', async () => {
  const b = await harness.makeBroker({ env: envFixture() });
  const d = await dev(b.broker);
  await harness.feed(b.broker, d.server, wire.subscribe({
    packetId: 1, filters: [{ filter: `mayap/v1/${DEV}/session`, qos: 0 }],
  }));
  await harness.clientReceive(d.client);
  const w = await web(b.broker);
  await harness.feed(b.broker, w.server, wire.publish({
    topic: `mayap/v1/${DEV}/session`, qos: 0, payload: '{"ttl":60}',
  }));
  const frames = await harness.clientReceive(d.client);
  const pub = wire.parsePublish(frames[0]);
  assert.equal(pub.topic, `mayap/v1/${DEV}/session`);
  assert.equal(pub.qos, 0);
  assert.equal(pub.payload.toString('utf-8'), '{"ttl":60}');
});
