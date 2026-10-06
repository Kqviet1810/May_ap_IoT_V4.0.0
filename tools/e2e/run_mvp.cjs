'use strict';
// End-to-end MVP proof (no hardware):
//   real app.js + real MQTT.js in headless Chromium
//     -> real broker Durable Object running on workerd (wrangler dev)
//     -> device PROTOCOL EMULATOR (tools/e2e/device_emulator.cjs, NOT the firmware)
// /api/mqtt-session is answered by the test with the same grant algorithm as
// cloudflare/src/account-auth.js controlGrant() (that route is covered by
// tests/account-security.test.cjs against the real Worker code).
//
//   node tools/e2e/run_mvp.cjs [outDir]
const { spawn } = require('node:child_process');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const { setTimeout: sleep } = require('node:timers/promises');
const { chromium } = require('../../cloudflare/node_modules/playwright');
const { startDeviceEmulator } = require('./device_emulator.cjs');

const root = path.resolve(__dirname, '../..');
const out = path.resolve(process.argv[2] || path.join(root, 'work', 'e2e-mvp'));
fs.mkdirSync(out, { recursive: true });
const BROKER_PORT = Number(process.env.E2E_BROKER_PORT || 8798);
const WEB_PORT = Number(process.env.E2E_WEB_PORT || 8766);
const DEVICE_ID = 'MAP-1234567890AB';
const DEVICE_PASSWORD = 'e2e-device-fixture';
const WEB_TOKEN_SECRET = 'e2e-web-token-secret';
const PEPPER = 'e2e-device-key-pepper';
const hmacHex = (key, text) => crypto.createHmac('sha256', key).update(text).digest('hex');
const commandKeyHex = hmacHex(PEPPER, `mayap-command-key:v1:${DEVICE_ID}`);
// Same construction as cloudflare/src/broker/acl.js signWebToken().
function webToken(deviceId, username, ttlSec = 3600) {
  const exp = Math.floor(Date.now() / 1000) + ttlSec;
  return { exp, token: `v1.${exp}.${hmacHex(WEB_TOKEN_SECRET, `mayap-mqtt-web:v1\n${deviceId}\n${username}\n${exp}`)}` };
}
async function brokerLogin(url, username, password) {
  const mqtt = require('../../cloudflare/node_modules/mqtt');
  return new Promise((resolve) => {
    const client = mqtt.connect(url, { protocolVersion: 4, clean: true, clientId: `probe-${Math.random().toString(16).slice(2, 8)}`,
      username, password, keepalive: 30, reconnectPeriod: 0, wsOptions: { protocol: 'mqtt' } });
    const done = (value) => { client.end(true); resolve(value); };
    client.once('connect', () => done('connected'));
    client.once('error', (error) => done(`refused:${error.code ?? error.message}`));
    setTimeout(() => done('timeout'), 8000);
  });
}
const brokerUrl = `ws://127.0.0.1:${BROKER_PORT}/mqtt/${DEVICE_ID}`;

const results = [];
const record = (name, ok, detail = '') => {
  results.push({ name, ok, detail });
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? ' - ' + detail : ''}`);
};
async function until(label, probe, timeoutMs = 15000, stepMs = 100) {
  const end = Date.now() + timeoutMs;
  let last;
  while (Date.now() < end) {
    last = await probe();
    if (last) return last;
    await sleep(stepMs);
  }
  throw new Error(`timeout: ${label}`);
}
function spawnLogged(command, args, options, logName) {
  const log = fs.createWriteStream(path.join(out, logName));
  const child = spawn(command, args, { ...options, stdio: ['ignore', 'pipe', 'pipe'] });
  child.stdout.pipe(log);
  child.stderr.pipe(log);
  return child;
}
async function httpUp(url) {
  try { return (await fetch(url)).status < 500; } catch (_) { return false; }
}

function controlGrant(clientId) {
  const key = Buffer.from(commandKeyHex, 'hex');
  const expiresAt = Math.floor(Date.now() / 1000) + 300;
  const grant = `${clientId}|${expiresAt}|${crypto.randomBytes(12).toString('hex')}`;
  return { grant, expiresAt,
    grantSig: hmacHex(key, `mayap-control-grant:v2\n${DEVICE_ID}\n${grant}`),
    sessionKey: hmacHex(key, `mayap-control-session:v2\n${DEVICE_ID}\n${grant}`) };
}

async function main() {
  const children = [];
  let browser, emulator;
  try {
    const broker = spawnLogged(path.join(root, 'cloudflare/node_modules/.bin/wrangler'), [
      'dev', '--config', 'wrangler-broker.toml', '--port', String(BROKER_PORT), '--local',
      '--var', `BROKER_FIXTURE_DEVICE_PASSWORD:${DEVICE_PASSWORD}`,
      '--var', `BROKER_WEB_TOKEN_SECRET:${WEB_TOKEN_SECRET}`],
    { cwd: path.join(root, 'cloudflare') }, 'broker.log');
    children.push(broker);
    const web = spawnLogged('python3', ['-m', 'http.server', String(WEB_PORT), '--bind', '127.0.0.1'],
      { cwd: root }, 'web.log');
    children.push(web);
    await until('broker up', () => httpUp(`http://127.0.0.1:${BROKER_PORT}/healthz`), 60000, 500);
    await until('web up', () => httpUp(`http://127.0.0.1:${WEB_PORT}/index.html`), 15000, 200);

    emulator = startDeviceEmulator({ url: brokerUrl, deviceId: DEVICE_ID, password: DEVICE_PASSWORD,
      commandKeyHex });
    await until('device connected', () => emulator.connected, 15000);
    record('device emulator connects to workerd broker (CONNECT/SUBACK/retained presence)', true);
    const own = webToken(DEVICE_ID, 'web:qa-sub');
    const results = {
      valid: await brokerLogin(brokerUrl, 'web:qa-sub', own.token),
      otherDevice: await brokerLogin(brokerUrl, 'web:qa-sub', webToken('MAP-AAAAAAAAAAAA', 'web:qa-sub').token),
      otherUser: await brokerLogin(brokerUrl, 'web:attacker', own.token),
      expired: await brokerLogin(brokerUrl, 'web:qa-sub', webToken(DEVICE_ID, 'web:qa-sub', -10).token),
      devicePasswordAsWeb: await brokerLogin(brokerUrl, 'web:qa-sub', DEVICE_PASSWORD),
    };
    record('broker auth: a web token is accepted only for its own device and account, and while unexpired',
      results.valid === 'connected' && results.otherDevice.startsWith('refused') &&
      results.otherUser.startsWith('refused') && results.expired.startsWith('refused') &&
      results.devicePasswordAsWeb.startsWith('refused'), JSON.stringify(results));

    browser = await chromium.launch({ headless: true,
      ...(process.env.E2E_CHROMIUM ? { executablePath: process.env.E2E_CHROMIUM } : {}) });
    const context = await browser.newContext({ viewport: { width: 390, height: 844 }, serviceWorkers: 'block' });
    await context.addInitScript(() => sessionStorage.setItem('mayap.account.session.v1', 'aa'.repeat(32)));
    const sessionRequests = [];
    await context.route('**/*', async (route) => {
      const url = new URL(route.request().url());
      if (!url.pathname.startsWith('/api/')) return route.continue();
      const json = (body) => route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
      if (url.pathname === '/api/account/session') return json({ success: true,
        user: { sub: 'qa-sub', name: 'QA' }, expiresAt: Date.now() + 86400000,
        devices: [{ device_id: DEVICE_ID, device_name: 'May ap thu', role: 'owner' }] });
      if (url.pathname === '/api/mqtt-session') {
        const body = JSON.parse(route.request().postData() || '{}');
        // First token is deliberately short so the Web must renew it before expiry.
        const issued = webToken(DEVICE_ID, 'web:qa-sub', sessionRequests.length === 0 ? 100 : 3600);
        sessionRequests.push(body);
        return json({ success: true, control: controlGrant(body.client_id),
          mqtt: { url: brokerUrl, username: 'web:qa-sub', password: issued.token, expiresAt: issued.exp, mode: 'token' } });
      }
      return json({ success: true, notes: [], reminders: [] });
    });
    const page = await context.newPage();
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(error.message));
    await page.goto(`http://127.0.0.1:${WEB_PORT}/`, { waitUntil: 'domcontentloaded' });
    const connection = () => page.evaluate(() => document.body.dataset.connection);
    const text = (selector) => page.locator(selector).first().textContent();

    // 1. Presence + 2. live snapshot
    await until('web online', async () => (await connection()) === 'online', 20000);
    record('W1 presence: Web shows device online from retained presence on a real broker', true);
    const before = emulator.stats.snapshots;
    await sleep(3500);
    const gained = emulator.stats.snapshots - before;
    const temp = (await text('#liveTemp')).trim();
    record('W2 live snapshot: session lease raises cadence and Web renders temperature',
      gained >= 3 && /^3\d[.,]\d/.test(temp) && (await connection()) === 'online',
      `${gained} snapshots in 3.5 s, liveTemp="${temp}"`);

    // 3. Light toggle with signed grant, terminal ACK
    await until('light button enabled', async () => !(await page.locator('#outputLightBtn').isDisabled()), 10000);
    const lightBefore = (await text('#outputLight')).trim();
    await page.locator('#outputLightBtn').click();
    await until('light on', async () => (await text('#outputLight')).trim() !== lightBefore, 8000);
    const lightAfter = (await text('#outputLight')).trim();
    const phases = emulator.stats.acks.filter((ack) => /light/.test(ack.requestId || '') || true)
      .map((ack) => `${ack.phase}:${ack.code}`);
    record('W3 light toggle Web->broker->device->ACK->Web (signed V2 grant verified by device)',
      lightAfter !== lightBefore && emulator.stats.commandsExecuted === 1 && emulator.stats.verifyFailures === 0,
      `UI ${lightBefore} -> ${lightAfter}; device executed=${emulator.stats.commandsExecuted}; acks=${phases.join(',')}`);

    // 4. Config save
    await page.locator('#quickTarget').waitFor();
    await until('config loaded', async () => (await page.locator('#quickTarget').inputValue()) !== '', 12000);
    await page.locator('#quickTarget').fill('38.0');
    await page.locator('#quickForm button[type="submit"]').click();
    await until('device config updated', async () => emulator.state.config.targetTemp === 38, 10000);
    const configAck = emulator.stats.acks.filter((ack) => ack.code === 'APPLIED').length;
    record('W4 config save: Web config/set patch verified, applied and config/reported by device',
      emulator.state.config.targetTemp === 38 && emulator.stats.verifyFailures === 0,
      `device targetTemp=${emulator.state.config.targetTemp}, APPLIED acks=${configAck}`);

    // 5a. Terminal REJECTED reaches the Web
    emulator.state.safetyBlock = true;
    await page.locator('#quickTarget').fill('38.5');
    await page.locator('#quickForm button[type="submit"]').click();
    await until('rejected ack', async () => emulator.stats.acks.some((ack) => ack.code === 'CONFIG_SAFETY_BLOCK'), 10000);
    const toast = await until('rejection toast', async () => {
      const value = (await text('#toast')) || '';
      return /từ chối/i.test(value) ? value : '';
    }, 8000);
    record('W5a terminal REJECTED (CONFIG_SAFETY_BLOCK) is shown to the user, config unchanged',
      emulator.state.config.targetTemp === 38 && /an toàn/.test(toast), `toast="${toast}"`);
    emulator.state.safetyBlock = false;

    // 5b. PUBACK is not APPLIED: device verifies the command but never acknowledges
    await sleep(500);
    emulator.state.silent = true;
    const lightMid = (await text('#outputLight')).trim();
    await page.locator('#outputLightBtn').click();
    const uncertain = await until('uncertain toast', async () => {
      const value = (await text('#toast')) || '';
      return /chưa chắc chắn/i.test(value) ? value : '';
    }, 16000, 200);
    const lightHeld = (await text('#outputLight')).trim();
    record('W5b PUBACK is not APPLIED: with no device ACK the Web ends UNCERTAIN and the UI does not flip',
      lightHeld === lightMid && emulator.stats.commandsExecuted === 1, `toast="${uncertain}", light=${lightHeld}`);
    // 5c. Late terminal ACK settles the uncertain transaction
    await emulator.releaseSilenced();
    await until('late ack applied', async () => emulator.stats.commandsExecuted === 2, 8000);
    await until('light flips after late ack', async () => (await text('#outputLight')).trim() !== lightMid, 8000);
    record('W5c late terminal ACK after UNCERTAIN settles the transaction (V2 late-ACK)', true,
      `light ${lightMid} -> ${(await text('#outputLight')).trim()}`);

    // Reconnect: abrupt device loss -> retained LWT -> Web offline; new device session -> online
    emulator.dropConnection();
    await until('web offline via LWT', async () => (await connection()) === 'offline', 15000);
    record('W1b LWT: abrupt device loss publishes retained online=false and the Web shows offline', true);
    emulator = startDeviceEmulator({ url: brokerUrl, deviceId: DEVICE_ID, password: DEVICE_PASSWORD, commandKeyHex });
    await until('web online again', async () => (await connection()) === 'online', 20000);
    record('W1c reconnect: a new device session (new bootId) brings the Web back online', true);
    // After a reboot the device restarts its config revision; the Web must drop its
    // pre-reboot config and accept the new device's report instead of rejecting it as old.
    await until('config resync after reboot', async () => Number(await page.locator('#quickTarget').inputValue()) === 37.5, 15000);
    record('W1d reboot: bootId change resets the Web config state and the new device config is accepted', true,
      `quickTarget=${await page.locator('#quickTarget').inputValue()} (was 38 before the reboot)`);

    record('Web console: no uncaught page errors', pageErrors.length === 0, pageErrors.join(' | '));
    record('Web renewed the short-lived broker token before expiry and kept working (control grant via /api/mqtt-session)',
      sessionRequests.length >= 2, `${sessionRequests.length} /api/mqtt-session request(s)`);
    await page.screenshot({ path: path.join(out, 'e2e-mvp.png') });
  } catch (error) {
    record('e2e run completed', false, String(error && error.message || error));
  } finally {
    try { emulator && emulator.stop(); } catch (_) {}
    try { browser && await browser.close(); } catch (_) {}
    for (const child of children) { try { child.kill('SIGTERM'); } catch (_) {} }
    fs.writeFileSync(path.join(out, 'e2e-mvp.json'), JSON.stringify({ at: new Date().toISOString(), results }, null, 2));
    const failed = results.filter((item) => !item.ok);
    console.log(`\n${results.length - failed.length}/${results.length} checks passed`);
    process.exit(failed.length ? 1 : 0);
  }
}
main();
