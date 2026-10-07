'use strict';
// The standalone broker process: ESP32 over MQTT/TLS and Web over MQTT/WS(S) on ONE session/topic logic.
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const tls = require('node:tls');
const { spawnSync } = require('node:child_process');
const { test, before, after } = require('node:test');
const wire = require('./fixtures/mqtt-wire.cjs');

const root = path.resolve(__dirname, '..');
const DEV = 'MAP-001122334455';
const OTHER = 'MAP-AABBCCDDEEFF';
const DEVICE_SECRET = 'device-secret-node-test';
const WEB_SECRET = 'web-secret-node-test';
const devicePassword = (id) => crypto.createHmac('sha256', DEVICE_SECRET).update(`mayap-mqtt-device:v1\n${id}`).digest('hex');
const webToken = (deviceId, username = 'web:u1', ttl = 600) => {
  const exp = Math.floor(Date.now() / 1000) + ttl;
  return `v1.${exp}.${crypto.createHmac('sha256', WEB_SECRET).update(`mayap-mqtt-web:v1\n${deviceId}\n${username}\n${exp}`).digest('hex')}`;
};
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const freePort = () => new Promise((resolve) => { const s = net.createServer().listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => resolve(p)); }); });

let tmp, certFile, keyFile, cert, mqtt, WebSocket, startBroker;
let skip = false;
before(async () => {
  tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mayap-broker-'));
  certFile = path.join(tmp, 'cert.pem'); keyFile = path.join(tmp, 'key.pem');
  const made = spawnSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', keyFile, '-out', certFile,
    '-days', '2', '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'], { encoding: 'utf8' });
  if (made.status !== 0) { skip = 'openssl not available'; return; }
  cert = fs.readFileSync(certFile);
  // Missing dependencies must FAIL (CI installs broker/ and cloudflare/), only openssl may skip.
  ({ startBroker } = await import(path.join(root, 'broker/server.js')));
  mqtt = require(path.join(root, 'cloudflare/node_modules/mqtt'));
});
after(() => { fs.rmSync(tmp, { recursive: true, force: true }); });

async function launch(extra = {}) {
  const ports = { tls: await freePort(), ws: await freePort() };
  const broker = await startBroker({
    bindHost: '127.0.0.1', certFile, keyFile, tlsPort: ports.tls, tcpPort: 0, wsPort: ports.ws, dataFile: '',
    brokerEnv: { BROKER_DEVICE_SECRET: DEVICE_SECRET, BROKER_WEB_TOKEN_SECRET: WEB_SECRET }, ...extra,
  });
  return { broker, ports };
}

// ESP32 stand-in: raw MQTT 3.1.1 bytes over TLS, exactly what the firmware sends.
class Device {
  constructor(socket) { this.socket = socket; this.buffer = Buffer.alloc(0); this.packets = []; this.closed = false;
    socket.on('data', (d) => { this.buffer = Buffer.concat([this.buffer, d]); this.parse(); });
    socket.on('close', () => { this.closed = true; }); socket.on('error', () => {}); }
  parse() {
    for (;;) {
      if (this.buffer.length < 2) return;
      let remaining = 0, multiplier = 1, i = 1, digit;
      do { if (i >= this.buffer.length) return; digit = this.buffer[i++]; remaining += (digit & 127) * multiplier; multiplier *= 128; } while (digit & 128);
      if (this.buffer.length < i + remaining) return;
      const frame = this.buffer.subarray(0, i + remaining);
      this.buffer = this.buffer.subarray(i + remaining);
      const type = frame[0] >> 4;
      this.packets.push(type === 3 ? { type, ...wire.parsePublish(frame) } : { type, frame });
    }
  }
  send(bytes) { this.socket.write(Buffer.from(bytes)); }
  async wait(predicate, ms = 3000) {
    const end = Date.now() + ms;
    while (Date.now() < end) { const hit = this.packets.find(predicate); if (hit) return hit; await sleep(10); }
    throw new Error(`timeout; got ${JSON.stringify(this.packets.map((p) => p.type))}`);
  }
}
async function connectDevice(port, { id = DEV, password = devicePassword(DEV), username = id, keepalive = 30, will = true } = {}) {
  const socket = tls.connect({ host: '127.0.0.1', port, ca: cert, servername: 'localhost' });
  await new Promise((resolve, reject) => { socket.once('secureConnect', resolve); socket.once('error', reject); });
  const device = new Device(socket);
  device.send(wire.connect({ clientId: `esp-${id}`, username, password, keepalive,
    will: will ? { topic: `mayap/v1/${id}/presence`, qos: 1, retain: true, payload: '{"online":false}' } : null }));
  const connack = await device.wait((p) => p.type === 2).catch(() => null);
  return { device, returnCode: connack ? connack.frame[3] : null };
}
function webClient(port, deviceId = DEV, { token = webToken(deviceId), clientId = `web-${Math.random().toString(36).slice(2, 7)}` } = {}) {
  const client = mqtt.connect(`wss://127.0.0.1:${port}/mqtt/${deviceId}`, { protocolVersion: 4, clean: true, clientId, username: 'web:u1',
    password: token, keepalive: 30, reconnectPeriod: 0, wsOptions: { protocol: 'mqtt', ca: cert } });
  client.messages = [];
  client.on('message', (topic, payload, packet) => client.messages.push({ topic, payload: payload.toString(), qos: packet.qos, retain: packet.retain }));
  return new Promise((resolve, reject) => { client.once('connect', () => resolve(client)); client.once('error', reject); client.once('close', () => reject(new Error('closed'))); });
}
const subscribe = (client, topic, qos = 1) => new Promise((resolve, reject) => client.subscribe(topic, { qos }, (e, g) => (e ? reject(e) : resolve(g))));
const publish = (client, topic, payload, opts) => new Promise((resolve, reject) => client.publish(topic, payload, opts, (e) => (e ? reject(e) : resolve())));
async function until(predicate, ms = 3000) { const end = Date.now() + ms; while (Date.now() < end) { if (predicate()) return; await sleep(10); } throw new Error('timeout'); }

test('ESP32 over MQTT/TLS and Web over WSS: command goes down, terminal ACK comes back, one topic space', {}, async (t) => {
  if (skip) return t.skip(skip);
  const { broker, ports } = await launch();
  t.after(() => broker.stop());
  const { device, returnCode } = await connectDevice(ports.tls);
  assert.equal(returnCode, 0);
  device.send(wire.subscribe({ packetId: 1, filters: [`command`, `config/set`, `history/request`].map((s) => ({ filter: `mayap/v1/${DEV}/${s}`, qos: 1 })).concat([{ filter: `mayap/v1/${DEV}/session`, qos: 0 }]) }));
  const suback = await device.wait((p) => p.type === 9);
  assert.deepEqual([...suback.frame.subarray(4)], [1, 1, 1, 0]);

  const web = await webClient(ports.ws);
  await subscribe(web, `mayap/v1/${DEV}/ack`);
  await subscribe(web, `mayap/v1/${DEV}/presence`);
  await publish(web, `mayap/v1/${DEV}/command`, '{"v":2,"requestId":"R1"}', { qos: 1 });
  const command = await device.wait((p) => p.type === 3 && p.topic.endsWith('/command'));
  assert.equal(command.payload.toString(), '{"v":2,"requestId":"R1"}');
  assert.equal(command.qos, 1);
  device.send(wire.puback({ packetId: command.packetId }));
  // Device -> web: terminal ACK, QoS1; the broker PUBACKs the device.
  device.send(wire.publish({ topic: `mayap/v1/${DEV}/ack`, qos: 1, packetId: 5, payload: '{"phase":"completed","code":"APPLIED"}' }));
  await device.wait((p) => p.type === 4);
  await until(() => web.messages.some((m) => m.topic.endsWith('/ack')));
  assert.match(web.messages.find((m) => m.topic.endsWith('/ack')).payload, /APPLIED/);
  web.end(true);
  device.socket.destroy();
});

test('retained presence + LWT cross transports: late Web joiner sees online, then offline after an abrupt TLS drop', {}, async (t) => {
  if (skip) return t.skip(skip);
  const { broker, ports } = await launch();
  t.after(() => broker.stop());
  const { device } = await connectDevice(ports.tls);
  device.send(wire.publish({ topic: `mayap/v1/${DEV}/presence`, qos: 1, retain: true, packetId: 1, payload: '{"online":true}' }));
  await device.wait((p) => p.type === 4);
  const first = await webClient(ports.ws);
  await subscribe(first, `mayap/v1/${DEV}/presence`);
  await until(() => first.messages.length === 1);
  assert.deepEqual([first.messages[0].retain, JSON.parse(first.messages[0].payload).online], [true, true]);
  device.socket.destroy();                                  // no DISCONNECT: the broker fires the LWT
  await until(() => first.messages.some((m) => JSON.parse(m.payload).online === false));
  const late = await webClient(ports.ws);
  await subscribe(late, `mayap/v1/${DEV}/presence`);
  await until(() => late.messages.length === 1);
  assert.equal(JSON.parse(late.messages[0].payload).online, false);
  first.end(true); late.end(true);
});

test('a clean DISCONNECT keeps the last presence, a new TLS connect takes over without firing the old LWT', {}, async (t) => {
  if (skip) return t.skip(skip);
  const { broker, ports } = await launch();
  t.after(() => broker.stop());
  const a = await connectDevice(ports.tls);
  a.device.send(wire.publish({ topic: `mayap/v1/${DEV}/presence`, qos: 1, retain: true, packetId: 1, payload: '{"online":true,"boot":1}' }));
  await a.device.wait((p) => p.type === 4);
  const b = await connectDevice(ports.tls);                 // same device id: takeover
  assert.equal(b.returnCode, 0);
  await until(() => a.device.closed);
  const web = await webClient(ports.ws);
  await subscribe(web, `mayap/v1/${DEV}/presence`);
  await until(() => web.messages.length === 1);
  assert.equal(JSON.parse(web.messages[0].payload).online, true, 'taken-over session must not publish its will');
  b.device.send(wire.publish({ topic: `mayap/v1/${DEV}/presence`, qos: 1, retain: true, packetId: 2, payload: '{"online":false,"clean":true}' }));
  await b.device.wait((p) => p.type === 4);
  b.device.send(wire.disconnect());
  await sleep(150);
  const late = await webClient(ports.ws);
  await subscribe(late, `mayap/v1/${DEV}/presence`);
  await until(() => late.messages.length === 1);
  assert.equal(JSON.parse(late.messages[0].payload).clean, true);
  web.end(true); late.end(true);
});

test('authentication and routing: per-device password, device ids only on TLS, token bound to the device, mqtt subprotocol', {}, async (t) => {
  if (skip) return t.skip(skip);
  const { broker, ports } = await launch();
  t.after(() => broker.stop());
  assert.equal((await connectDevice(ports.tls, { password: devicePassword(OTHER) })).returnCode, 4);   // another device's password
  assert.equal((await connectDevice(ports.tls, { password: 'shared-fixture' })).returnCode, 4);
  const web = webToken(DEV);
  const asDevice = await connectDevice(ports.tls, { username: 'web:u1', password: web }).catch(() => ({ returnCode: null }));
  assert.equal(asDevice.returnCode, null, 'web usernames are not routable on the device listener');
  await assert.rejects(webClient(ports.ws, OTHER, { token: webToken(DEV) }));                            // token for DEV used on OTHER
  const wrongProto = await new Promise((resolve) => {
    const s = tls.connect({ host: '127.0.0.1', port: ports.ws, ca: cert, servername: 'localhost' }, () => s.write(`GET /mqtt/${DEV} HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
    let data = ''; s.on('data', (d) => { data += d; }); s.on('close', () => resolve(data));
  });
  assert.match(wrongProto, /^HTTP\/1\.1 400/);
  // Web may not publish a presence record, nor reach another device's topics.
  const ok = await webClient(ports.ws);
  const denied = await subscribe(ok, `mayap/v1/${OTHER}/command`).catch(() => [{ qos: 128 }]);
  assert.equal(denied[0].qos, 128);
  ok.end(true);
});

test('two devices stay isolated; retained state survives a broker restart (data file)', {}, async (t) => {
  if (skip) return t.skip(skip);
  const dataFile = path.join(tmp, 'state.json');
  let { broker, ports } = await launch({ dataFile });
  const a = await connectDevice(ports.tls);
  a.device.send(wire.publish({ topic: `mayap/v1/${DEV}/presence`, qos: 1, retain: true, packetId: 1, payload: '{"online":true,"fw":"x"}' }));
  await a.device.wait((p) => p.type === 4);
  const b = await connectDevice(ports.tls, { id: OTHER, password: devicePassword(OTHER) });
  assert.equal(b.returnCode, 0);
  b.device.send(wire.subscribe({ packetId: 1, filters: [{ filter: `mayap/v1/${OTHER}/command`, qos: 1 }] }));
  await b.device.wait((p) => p.type === 9);
  const web = await webClient(ports.ws);
  await publish(web, `mayap/v1/${DEV}/command`, '{"x":1}', { qos: 1 });
  await sleep(150);
  assert.equal(b.device.packets.filter((p) => p.type === 3).length, 0, 'a command for one device must never reach another');
  web.end(true);
  a.device.send(wire.disconnect());                         // clean: the retained record stays as published
  b.device.send(wire.disconnect());
  await sleep(200);
  await broker.stop();
  await sleep(100);
  ({ broker, ports } = await launch({ dataFile }));
  const again = await webClient(ports.ws);
  await subscribe(again, `mayap/v1/${DEV}/presence`);
  await until(() => again.messages.length >= 1);
  assert.equal(JSON.parse(again.messages[0].payload).fw, 'x');
  again.end(true);
  await broker.stop();
});
