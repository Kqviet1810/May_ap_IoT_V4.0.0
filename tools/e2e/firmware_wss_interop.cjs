'use strict';
// Firmware transport interop (no hardware): the REAL firmware sources mqtt_transport.h / mqtt_wire.h /
// mqtt_ws.h (host-compiled, tests/host-wss-client.cpp, real TCP socket instead of WiFiClientSecure) against
// the REAL broker Durable Object on workerd (wrangler dev) and a real MQTT.js Web client.
//   node tools/e2e/firmware_wss_interop.cjs [outDir]
// Proves the Cloudflare-only path ESP32 -> WSS -> broker DO -> Web without any other server. TLS itself is
// WiFiClientSecure's job on the device and is not part of this test.
const { spawn, spawnSync } = require('node:child_process');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { setTimeout: sleep } = require('node:timers/promises');

const root = path.resolve(__dirname, '../..');
const out = path.resolve(process.argv[2] || path.join(root, 'work', 'e2e-fw-wss'));
fs.mkdirSync(out, { recursive: true });
const PORT = Number(process.env.E2E_BROKER_PORT || 8799);
const DEVICE_ID = 'MAP-1234567890AB';
const DEVICE_SECRET = 'e2e-device-secret';
const WEB_TOKEN_SECRET = 'e2e-web-token-secret';
const hmacHex = (key, text) => crypto.createHmac('sha256', key).update(text).digest('hex');
const DEVICE_PASSWORD = hmacHex(DEVICE_SECRET, `mayap-mqtt-device:v1\n${DEVICE_ID}`);
function webToken(deviceId, username) {
  const exp = Math.floor(Date.now() / 1000) + 3600;
  return `v1.${exp}.${hmacHex(WEB_TOKEN_SECRET, `mayap-mqtt-web:v1\n${deviceId}\n${username}\n${exp}`)}`;
}
const results = [];
const record = (name, ok, detail = '') => { results.push(ok); console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? ' - ' + detail : ''}`); };
async function until(label, probe, timeoutMs = 20000, stepMs = 100) {
  const end = Date.now() + timeoutMs;
  for (;;) { const v = await probe(); if (v) return v; if (Date.now() > end) throw new Error(`timeout: ${label}`); await sleep(stepMs); }
}

async function main() {
  const children = [];
  try {
    // 1. Build the client from the production headers (copied next to the stubs, as the unit test does).
    const work = fs.mkdtempSync(path.join(os.tmpdir(), 'mayap-fw-wss-'));
    for (const f of fs.readdirSync(path.join(root, 'tests/stubs/mqtt'))) fs.copyFileSync(path.join(root, 'tests/stubs/mqtt', f), path.join(work, f));
    for (const f of ['mqtt_transport.h', 'mqtt_wire.h', 'mqtt_ws.h', 'mqtt_uplink.h']) fs.copyFileSync(path.join(root, 'MAYAP_INDUSTRIAL_v1_0_0', f), path.join(work, f));
    const exe = path.join(work, 'client');
    const build = spawnSync('g++', ['-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', work, `-DMAYAP_BROKER_HOST="127.0.0.1"`,
      `-DMAYAP_BROKER_PORT=${PORT}`, path.join(root, 'tests/host-wss-client.cpp'), '-o', exe], { encoding: 'utf8' });
    if (build.status !== 0) throw new Error(build.stderr);

    // 2. The real broker.
    const brokerLog = fs.createWriteStream(path.join(out, 'broker.log'));
    const broker = spawn(path.join(root, 'cloudflare/node_modules/.bin/wrangler'), ['dev', '--config', 'wrangler-broker.toml', '--port', String(PORT),
      '--local', '--var', `BROKER_DEVICE_SECRET:${DEVICE_SECRET}`, '--var', `BROKER_WEB_TOKEN_SECRET:${WEB_TOKEN_SECRET}`],
      { cwd: path.join(root, 'cloudflare'), stdio: ['ignore', 'pipe', 'pipe'], detached: true });
    broker.stdout.pipe(brokerLog); broker.stderr.pipe(brokerLog); children.push(broker);
    await until('broker up', async () => { try { return (await fetch(`http://127.0.0.1:${PORT}/healthz`)).status < 500; } catch (_) { return false; } }, 60000, 500);

    // 3. The Web side (MQTT.js, as the Web uses it).
    const mqtt = require(path.join(root, 'cloudflare/node_modules/mqtt'));
    const web = mqtt.connect(`ws://127.0.0.1:${PORT}/mqtt/${DEVICE_ID}`, { protocolVersion: 4, clean: true, clientId: 'web-e2e', username: 'web:qa',
      password: webToken(DEVICE_ID, 'web:qa'), keepalive: 30, reconnectPeriod: 0, wsOptions: { protocol: 'mqtt' } });
    const seen = [];
    web.on('message', (topic, payload, packet) => seen.push({ topic, payload: payload.toString(), retain: packet.retain }));
    await until('web connected', () => web.connected);
    const prefix = `mayap/v1/${DEVICE_ID}`;
    await new Promise((resolve, reject) => web.subscribe([`${prefix}/presence`, `${prefix}/ack`, `${prefix}/snapshot`], { qos: 1 }, (e, g) => e ? reject(e) : resolve(g)));

    // 4. The firmware transport.
    const startClient = () => {
      const child = spawn(exe, [DEVICE_ID, DEVICE_PASSWORD, '60'], { stdio: ['ignore', 'pipe', 'pipe'] });
      const lines = [];
      child.stdout.on('data', (d) => { for (const l of d.toString().split('\n')) if (l) lines.push(l); });
      child.stderr.on('data', (d) => lines.push('ERR ' + d));
      children.push(child);
      fs.writeFileSync(path.join(out, 'client-lines.txt'), '');
      return { child, lines };
    };
    let client = startClient();
    await until('firmware transport up', () => client.lines.some((l) => l.includes('wss+mqtt up')), 25000);
    record('firmware WSS transport: Upgrade + CONNECT + SUBSCRIBE accepted by the broker Durable Object', true,
      client.lines.find((l) => l.includes('wss+mqtt up')).slice(0, 90));
    await until('presence online', () => seen.some((m) => m.topic === `${prefix}/presence` && m.payload.includes('"online":true')), 15000);
    record('retained presence {online:true} published by the firmware reaches the Web', true);

    // 5. Web -> device command (QoS1), device -> Web ack.
    web.publish(`${prefix}/command`, '{"v":2,"requestId":"R-e2e-1"}', { qos: 1 });
    await until('command delivered', () => client.lines.some((l) => l.startsWith('DELIVERED command') && l.includes('R-e2e-1')), 15000);
    await until('ack to web', () => seen.some((m) => m.topic === `${prefix}/ack` && m.payload.includes('completed')), 15000);
    record('Web command -> firmware bridge dispatch -> firmware ack -> Web', true);

    // 6. Large frame (16-bit WebSocket length) both directions inside the 2048 B contract cap.
    const big = JSON.stringify({ v: 2, requestId: 'R-big', pad: 'x'.repeat(1700) });
    web.publish(`${prefix}/command`, big, { qos: 1 });
    await until('big command delivered', () => client.lines.some((l) => l.startsWith('DELIVERED command') && l.includes('R-big')), 15000);
    const delivered = client.lines.find((l) => l.startsWith('DELIVERED command') && l.includes('R-big'));
    record('1.7 kB command crosses the WebSocket intact (extended frame length, masked client frames)', delivered.length > 1700);

    // 7. Abrupt power loss: the broker must publish the LWT (retained presence offline).
    seen.length = 0;
    client.child.kill('SIGKILL');
    await until('LWT', () => seen.some((m) => m.topic === `${prefix}/presence` && m.payload.includes('"online":false')), 20000);
    record('abrupt loss of the firmware connection fires the LWT (presence {online:false})', true);

    // 8. Reconnect: a new firmware session takes over.
    seen.length = 0;
    client = startClient();
    await until('reconnect', () => client.lines.some((l) => l.includes('wss+mqtt up')), 25000);
    await until('presence online again', () => seen.some((m) => m.topic === `${prefix}/presence` && m.payload.includes('"online":true')), 15000);
    record('firmware reconnects and republishes presence online', true);

    // 8b. Command burst (item E of the field log): 40 Web commands at once, each acknowledged by the firmware with a QoS1 ack. The
    //     firmware's in-flight window must not pin at 8, no "ACK PUB FAIL" refusal may be needed for a normal burst, and every
    //     terminal ack must reach the Web.
    seen.length = 0;
    const burst = 40;
    for (let n = 0; n < burst; n += 1) web.publish(`${prefix}/command`, JSON.stringify({ v: 2, requestId: `R-burst-${n}` }), { qos: 1 });
    web.on('close', () => console.log('  [debug] web socket closed during burst'));
    try { await until('burst acks', () => seen.filter((m) => m.topic === `${prefix}/ack`).length >= burst, 30000, 100); } catch (e) { console.log(client.lines.filter((l) => /ACKED|STAT|inflight|MQTT/.test(l)).slice(-25).join('\n')); console.log(`  [debug] acks seen ${seen.filter((m) => m.topic === `${prefix}/ack`).length}/${burst}, web connected=${web.connected}`); throw e; }
    record(`QoS1 burst: ${burst} Web commands -> ${burst} firmware terminal acks all reach the Web`,
      seen.filter((m) => m.topic === `${prefix}/ack`).length === burst);

    // 8c. Alarms through the REAL uplink mailbox + transport + broker with NO Worker behind the broker (the broker is started
    //     alone here): PUBACK = BROKER_STORED comes back at once, nothing waits for D1, the link does not move.
    {
      const alarmRun = spawn(exe, [DEVICE_ID, DEVICE_PASSWORD, '14'], { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, UPLINK_ALARMS: '5' } });
      const lines2 = []; alarmRun.stdout.on('data', (d) => { for (const l of d.toString().split('\n')) if (l) lines2.push(l); });
      children.push(alarmRun);
      await new Promise((resolve) => alarmRun.on('exit', resolve));
      const acked = lines2.filter((l) => /^ALARM \d+ ACKED/.test(l)).map((l) => Number(l.split(' ')[3]));
      const stat = lines2.find((l) => l.startsWith('STAT')) || '';
      record('firmware uplink mailbox -> broker: 5 alarms PUBACKed (BROKER_STORED) < 1 s each with no Worker behind the broker; no close, no loss',
        acked.length === 5 && Math.max(...acked) < 1000 && /closes=0\/0\/0\/0 lost=0/.test(stat) && lines2.some((l) => l.includes('DONE connected=1')),
        `PUBACK ms: ${acked.join(',')}; ${stat}`);
    }

    // 9. Wrong password is refused and the firmware stays offline.
    const bad = spawn(exe, [DEVICE_ID, 'f'.repeat(64), '8'], { stdio: ['ignore', 'pipe', 'pipe'] });
    const badLines = []; bad.stdout.on('data', (d) => badLines.push(d.toString()));
    children.push(bad);
    await new Promise((resolve) => bad.on('exit', resolve));
    record('a firmware session with a wrong per-device password is refused', !badLines.join('').includes('wss+mqtt up') && badLines.join('').includes('DONE connected=0'));
    web.end(true);
  } finally {
    // detached children (wrangler + its workerd) are signalled as a group; the rest individually
    for (const c of children) { try { process.kill(c.spawnargs[0].endsWith('wrangler') ? -c.pid : c.pid, 'SIGKILL'); } catch (_) { /* gone */ } }
  }
  const failed = results.filter((ok) => !ok).length;
  console.log(`\n${results.length - failed}/${results.length} interop checks passed`);
  process.exit(failed ? 1 : 0);
}
main().catch((error) => { console.error('FAIL ', error.message); process.exit(1); });
