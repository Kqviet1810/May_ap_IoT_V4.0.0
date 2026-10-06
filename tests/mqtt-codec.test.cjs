'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const { test } = require('node:test');
const wire = require('./fixtures/mqtt-wire.cjs');

let codec;
test('load codec module', async () => {
  codec = await import(path.resolve(__dirname, '../cloudflare/src/broker/mqtt-codec.js'));
  assert.ok(codec.StreamingDecoder);
});

function decodeAll(bytes) {
  const d = new codec.StreamingDecoder();
  d.push(bytes);
  return d.drain();
}

test('CONNECT valid clean session', () => {
  const bytes = wire.connect({
    clientId: 'esp-MAP-001122334455',
    username: 'MAP-001122334455',
    password: 'pw',
    will: { topic: 'mayap/v1/MAP-001122334455/presence', qos: 1, retain: true, payload: '{"online":false}' },
  });
  const [pkt] = decodeAll(bytes);
  assert.equal(pkt.type, 'CONNECT');
  assert.equal(pkt.cleanSession, true);
  assert.equal(pkt.keepalive, 60);
  assert.equal(pkt.clientId, 'esp-MAP-001122334455');
  assert.equal(pkt.username, 'MAP-001122334455');
  assert.equal(pkt.will.topic, 'mayap/v1/MAP-001122334455/presence');
  assert.equal(pkt.will.qos, 1);
  assert.equal(pkt.will.retain, true);
  assert.deepEqual(Buffer.from(pkt.will.payload).toString('utf-8'), '{"online":false}');
});

test('CONNECT rejects CleanSession=0', () => {
  const bytes = wire.connect({ cleanSession: false });
  assert.throws(() => decodeAll(bytes), { code: 'CLEAN_SESSION_REQUIRED' });
});

test('CONNECT protocol level != 4 → CONNECT_UNSUPPORTED (MQTT-3.1.2-2)', () => {
  const bytes = wire.connect({ protoLevel: 5 });
  const [pkt] = decodeAll(bytes);
  assert.equal(pkt.type, 'CONNECT_UNSUPPORTED');
});

test('CONNECT protocol name != MQTT → CONNECT_UNSUPPORTED', () => {
  const bytes = wire.connect({ protoName: 'MQIsdp' });
  const [pkt] = decodeAll(bytes);
  assert.equal(pkt.type, 'CONNECT_UNSUPPORTED');
});

test('CONNECT rejects reserved flag set', () => {
  const bytes = wire.connect({ reservedFlag: true });
  assert.throws(() => decodeAll(bytes), { code: 'BAD_FLAGS' });
});

test('CONNECT rejects willQos=2', () => {
  const bytes = wire.connect({
    will: { topic: 'mayap/v1/x/presence', qos: 2, retain: false, payload: '' },
  });
  assert.throws(() => decodeAll(bytes), { code: 'QOS_UNSUPPORTED' });
});

test('CONNECT rejects password without username', () => {
  const bytes = wire.connect({ username: null, password: 'x' });
  assert.throws(() => decodeAll(bytes), { code: 'BAD_FLAGS' });
});

test('PUBLISH QoS1 with packet id', () => {
  const bytes = wire.publish({
    topic: 'mayap/v1/MAP-001122334455/command',
    qos: 1, packetId: 7, payload: '{"op":"start"}',
  });
  const [pkt] = decodeAll(bytes);
  assert.equal(pkt.type, 'PUBLISH');
  assert.equal(pkt.qos, 1);
  assert.equal(pkt.packetId, 7);
  assert.equal(Buffer.from(pkt.payload).toString('utf-8'), '{"op":"start"}');
});

test('PUBLISH rejects QoS=2', () => {
  const bytes = wire.publish({ topic: 'a/b', qos: 2, packetId: 1 });
  assert.throws(() => decodeAll(bytes), { code: 'QOS_UNSUPPORTED' });
});

test('PUBLISH rejects wildcard topic', () => {
  const bytes = wire.publish({ topic: 'a/+/c', payload: 'x' });
  assert.throws(() => decodeAll(bytes), { code: 'BAD_TOPIC' });
});

test('PUBLISH rejects empty topic', () => {
  const bytes = wire.publish({ topic: '', payload: 'x' });
  assert.throws(() => decodeAll(bytes), { code: 'BAD_TOPIC' });
});

test('PUBLISH rejects null byte in topic', () => {
  const bytes = wire.publish({ topic: 'a\u0000b', payload: 'x' });
  assert.throws(() => decodeAll(bytes), { code: 'BAD_UTF8' });
});

test('PUBLISH rejects DUP with QoS0', () => {
  const bytes = wire.publish({ topic: 'a/b', dup: true, qos: 0, payload: 'x' });
  assert.throws(() => decodeAll(bytes), { code: 'BAD_FLAGS' });
});

test('SUBSCRIBE rejects empty filter list', () => {
  // Encode an empty subscribe packet.
  const vp = [0, 1]; // packetId
  const bytes = new Uint8Array([0x82, vp.length, ...vp]);
  assert.throws(() => decodeAll(bytes), { code: 'BAD_LEN' });
});

test('SUBSCRIBE rejects reserved QoS bits', () => {
  const vp = [0, 1, 0, 1, 'a'.charCodeAt(0), 0xff];
  const bytes = new Uint8Array([0x82, vp.length, ...vp]);
  assert.throws(() => decodeAll(bytes), { code: 'BAD_FLAGS' });
});

test('SUBSCRIBE rejects QoS=2', () => {
  const bytes = wire.subscribe({ filters: [{ filter: 'a/b', qos: 2 }] });
  assert.throws(() => decodeAll(bytes), { code: 'QOS_UNSUPPORTED' });
});

test('SUBSCRIBE rejects # not at end', () => {
  const bytes = wire.subscribe({ filters: [{ filter: 'a/#/c', qos: 1 }] });
  assert.throws(() => decodeAll(bytes), { code: 'BAD_TOPIC' });
});

test('PINGREQ parses and DISCONNECT parses', () => {
  const bytes = new Uint8Array([
    0xc0, 0, // pingreq
    0xe0, 0, // disconnect
  ]);
  const packets = decodeAll(bytes);
  assert.deepEqual(packets.map((p) => p.type), ['PINGREQ', 'DISCONNECT']);
});

test('reserved packet type 0 rejected', () => {
  const bytes = new Uint8Array([0x00, 0]);
  assert.throws(() => decodeAll(bytes), { code: 'BAD_TYPE' });
});

test('reserved packet type 15 rejected', () => {
  const bytes = new Uint8Array([0xf0, 0]);
  assert.throws(() => decodeAll(bytes), { code: 'BAD_TYPE' });
});

test('remaining length encoding > 4 bytes rejected', () => {
  const bytes = new Uint8Array([0x30, 0x80, 0x80, 0x80, 0x80, 0x80]);
  assert.throws(() => decodeAll(bytes), { code: 'BAD_LEN' });
});

test('streaming: byte-by-byte feed yields CONNECT', () => {
  const full = wire.connect({ clientId: 'c' });
  const d = new codec.StreamingDecoder();
  let got = [];
  for (let i = 0; i < full.length; i++) {
    d.push(Uint8Array.of(full[i]));
    got.push(...d.drain());
  }
  assert.equal(got.length, 1);
  assert.equal(got[0].type, 'CONNECT');
});

test('streaming: multiple packets in one chunk', () => {
  const chunks = [
    wire.pingreq(),
    wire.pingreq(),
    wire.disconnect(),
  ];
  const total = new Uint8Array(chunks.reduce((n, c) => n + c.length, 0));
  let o = 0; for (const c of chunks) { total.set(c, o); o += c.length; }
  const packets = decodeAll(total);
  assert.deepEqual(packets.map((p) => p.type), ['PINGREQ', 'PINGREQ', 'DISCONNECT']);
});

test('packet exceeding MQTT_MAX_PACKET is rejected', () => {
  // Build a fake PUBLISH header with Remaining Length = 5000.
  const first = 0x30;
  const lenBytes = [];
  let n = 5000;
  do {
    let d = n % 128;
    n = Math.floor(n / 128);
    if (n > 0) d |= 0x80;
    lenBytes.push(d);
  } while (n > 0);
  const bytes = new Uint8Array([first, ...lenBytes]);
  assert.throws(() => decodeAll(bytes), { code: 'OVERFLOW' });
});

test('decoder buffer limit (16 KiB) overflow is fatal', () => {
  const d = new codec.StreamingDecoder();
  // 4 KiB is accepted so multiple packets can coalesce in one frame.
  d.push(new Uint8Array(codec.MQTT_MAX_PACKET));
  // Push beyond the accumulator limit (4× max) → OVERFLOW.
  assert.throws(() => d.push(new Uint8Array(codec.MQTT_MAX_PACKET * 4)),
    { code: 'OVERFLOW' });
});

test('5-byte Remaining Length rejected even when 5th byte MSB=0', () => {
  // 0x80 0x80 0x80 0x80 0x00 — four continuation bytes then terminator.
  const d = new codec.StreamingDecoder();
  d.push(new Uint8Array([0x30, 0x80, 0x80, 0x80, 0x80, 0x00]));
  assert.throws(() => d.drain(), { code: 'BAD_LEN' });
});

test('packets coalesced beyond MQTT_MAX_PACKET parse one by one', () => {
  const d = new codec.StreamingDecoder();
  // 3 × PINGREQ (2 bytes each) + 1 × 4000-byte PUBLISH would exceed 4096
  // in total, but each packet is under the per-packet cap.
  const makePublish = (payloadSize) => {
    const topicBytes = Buffer.from('a/b', 'utf-8');
    const vp = [topicBytes.length >> 8, topicBytes.length & 0xff, ...topicBytes, ...new Uint8Array(payloadSize)];
    const total = vp.length;
    const lenBytes = [];
    let n = total;
    do { let dg = n % 128; n = Math.floor(n / 128); if (n > 0) dg |= 0x80; lenBytes.push(dg); } while (n > 0);
    return new Uint8Array([0x30, ...lenBytes, ...vp]);
  };
  const p1 = makePublish(3800);
  const p2 = makePublish(3800);
  // Buffer limit is 16 KiB — two 3800-byte publishes + overhead fits.
  d.push(p1);
  d.push(p2);
  const got = d.drain();
  assert.equal(got.length, 2);
  assert.equal(got[0].type, 'PUBLISH');
  assert.equal(got[1].type, 'PUBLISH');
});

test('encodeConnack / encodeSuback / encodePuback / encodePingresp byte-exact', () => {
  assert.deepEqual(Array.from(codec.encodeConnack(0)), [0x20, 0x02, 0x00, 0x00]);
  assert.deepEqual(Array.from(codec.encodeConnack(5)), [0x20, 0x02, 0x00, 0x05]);
  assert.deepEqual(Array.from(codec.encodeSuback(7, [1, 0x80])),
    [0x90, 0x04, 0x00, 0x07, 0x01, 0x80]);
  assert.deepEqual(Array.from(codec.encodePuback(9)), [0x40, 0x02, 0x00, 0x09]);
  assert.deepEqual(Array.from(codec.encodePingresp()), [0xd0, 0x00]);
});

test('encodePublish QoS1 shape', () => {
  const bytes = codec.encodePublish({
    topic: 'mayap/v1/x/ack', qos: 1, packetId: 42, payload: new Uint8Array([0x7b, 0x7d]),
  });
  const parsed = wire.parsePublish(bytes);
  assert.equal(parsed.topic, 'mayap/v1/x/ack');
  assert.equal(parsed.qos, 1);
  assert.equal(parsed.packetId, 42);
  assert.equal(parsed.retain, false);
  assert.deepEqual(parsed.payload, Buffer.from([0x7b, 0x7d]));
});

test('topicMatches wildcards', () => {
  assert.ok(codec.topicMatches('mayap/v1/+/presence', 'mayap/v1/x/presence'));
  assert.ok(codec.topicMatches('mayap/v1/x/#', 'mayap/v1/x/snapshot'));
  assert.ok(codec.topicMatches('mayap/v1/x/#', 'mayap/v1/x/config/reported'));
  assert.ok(!codec.topicMatches('mayap/v1/+/presence', 'mayap/v1/x/y/presence'));
  assert.ok(!codec.topicMatches('a/b', 'a/b/c'));
  assert.ok(codec.topicMatches('a/b', 'a/b'));
});
