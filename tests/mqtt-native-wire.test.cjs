'use strict';
// The firmware's own MQTT client (mqtt_wire.h) against the broker's codec, both directions.
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { test } = require('node:test');

const root = path.resolve(__dirname, '..');
const bin = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'mayap-wire-')), 'mqtt-wire');
const compiled = spawnSync('g++', ['-std=c++11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
  path.join(root, 'tests/mqtt-wire.cpp'), '-o', bin], { encoding: 'utf8' });
const haveCompiler = compiled.status === 0;
const skip = haveCompiler ? false : 'g++ not available';

let codec;
test('load codec', async () => { codec = await import(path.join(root, 'cloudflare/src/broker/mqtt-codec.js')); });

function decodeOne(hexText) {
  const decoder = new codec.StreamingDecoder();
  decoder.push(Buffer.from(hexText, 'hex'));
  const packets = decoder.drain();
  assert.equal(packets.length, 1);
  return packets[0];
}

test('device-encoded packets are accepted by the broker codec', { skip }, () => {
  const run = spawnSync(bin, ['encode'], { encoding: 'utf8' });
  assert.equal(run.status, 0, run.stderr);
  const lines = Object.fromEntries(run.stdout.trim().split('\n').map((l) => l.split(' ')));

  const connect = decodeOne(lines.connect);
  assert.equal(connect.type, 'CONNECT');
  assert.equal(connect.cleanSession, true);
  assert.equal(connect.keepalive, 30);
  assert.equal(connect.clientId, 'esp-MAP-AABBCCDDEEFF');
  assert.equal(connect.username, 'MAP-AABBCCDDEEFF');
  assert.match(Buffer.from(connect.password).toString(), /^[0-9a-f]{64}$/);
  assert.equal(connect.will.topic, 'mayap/v1/MAP-AABBCCDDEEFF/presence');
  assert.equal(connect.will.qos, 1);
  assert.equal(connect.will.retain, true);
  assert.equal(Buffer.from(connect.will.payload).toString(), '{"online":false}');

  const subscribe = decodeOne(lines.subscribe);
  assert.equal(subscribe.type, 'SUBSCRIBE');
  assert.equal(subscribe.packetId, 1);
  assert.deepEqual(subscribe.filters.map((f) => [f.topic || f.filter, f.qos]), [
    ['mayap/v1/MAP-AABBCCDDEEFF/command', 1], ['mayap/v1/MAP-AABBCCDDEEFF/config/set', 1],
    ['mayap/v1/MAP-AABBCCDDEEFF/history/request', 1], ['mayap/v1/MAP-AABBCCDDEEFF/session', 0]]);

  const presence = decodeOne(lines['publish-q1-retain']);
  assert.deepEqual([presence.type, presence.qos, presence.retain, presence.packetId], ['PUBLISH', 1, true, 7]);
  assert.equal(presence.topic, 'mayap/v1/MAP-AABBCCDDEEFF/presence');
  const snapshot = decodeOne(lines['publish-q0']);
  assert.deepEqual([snapshot.qos, snapshot.retain], [0, false]);
  const big = decodeOne(lines['publish-big']);
  assert.equal(big.payload.length, 2000);
  assert.equal(big.packetId, 65535);
  assert.equal(decodeOne(lines.puback).packetId, 513);
  assert.equal(decodeOne(lines.pingreq).type, 'PINGREQ');
  assert.equal(decodeOne(lines.disconnect).type, 'DISCONNECT');
});

test('broker-encoded packets parse in the firmware parser however the stream is split', { skip }, () => {
  const topic = 'mayap/v1/MAP-AABBCCDDEEFF/command';
  const frames = [
    codec.encodeConnack(0),
    codec.encodeSuback(1, [1, 1, 1, 0]),
    codec.encodePublish({ topic, qos: 1, packetId: 42, payload: Buffer.from('{"v":2,"x":1}') }),
    codec.encodePublish({ topic, qos: 0, payload: Buffer.alloc(1500, 0x41) }),
    codec.encodePuback(9),
    codec.encodePingresp(),
    codec.encodePublish({ topic, qos: 0, payload: Buffer.alloc(3000, 0x42) }),   // > 2560 B buffer: truncated, not stored
    codec.encodePublish({ topic, qos: 0, payload: Buffer.from('after-big') }),   // stream stays in sync afterwards
  ];
  const input = Buffer.concat(frames.map((f) => Buffer.from(f))).toString('hex') + '\n';
  const run = spawnSync(bin, ['decode'], { input, encoding: 'utf8' });
  assert.equal(run.status, 0, run.stderr);
  const out = run.stdout.trim().split('\n');
  assert.equal(out[0], 'type=2 flags=0 len=2 body=0000');
  assert.equal(out[1], 'type=9 flags=0 len=6 body=000101010100');
  assert.match(out[2], /^publish qos=1 retain=0 id=42 topic=mayap\/v1\/MAP-AABBCCDDEEFF\/command payload=\{"v":2,"x":1\} len=13$/);
  assert.match(out[3], /^publish qos=0 retain=0 id=0 .* len=1500$/);
  assert.equal(out[4], 'type=4 flags=0 len=2 body=0009');
  assert.equal(out[5], 'type=13 flags=0 len=0 body=');
  assert.equal(out[6], 'truncated type=3');
  assert.match(out[7], /payload=after-big len=9$/);
  assert.equal(out[8], 'packets=8');
});

test('malformed Remaining Length is fatal', { skip }, () => {
  const run = spawnSync(bin, ['decode'], { input: '30ffffffff7f\n', encoding: 'utf8' });
  assert.match(run.stdout, /fatal/);
});
