// MAYAP V4 broker-lite - auth + ACL per doc/MQTT_CONTRACT.md §5.
// Pure. No I/O. The DO owns credential resolution; this module just enforces
// topic scope once the role is known.

export const DEVICE_ID_RE = /^MAP-[0-9A-F]{12}$/;

export const TOPIC_ROOT = 'mayap/v1';

export const Topics = Object.freeze({
  presence: 'presence',
  snapshot: 'snapshot',
  ack: 'ack',
  log: 'log',
  configReported: 'config/reported',
  remindersReported: 'reminders/reported',
  historyReported: 'history/reported',
  command: 'command',
  configSet: 'config/set',
  remindersSet: 'reminders/set',
  historyRequest: 'history/request',
  session: 'session',
  // Device -> cloud ingest (alarms, heartbeat). Not fanned out: the broker hands them to the main Worker
  // and PUBACKs only once the Worker confirms a durable write.
  alarm: 'alarm',
  heartbeat: 'heartbeat',
  // Broker -> Web only (live, not retained): what the uplink queue still has to hand to the Worker.
  uplink: 'uplink',
});

// Suffix lists keyed by role, enforced AFTER verifying the leading
// `mayap/v1/<deviceId>/` prefix matches this DO's device.

// Session (per V2): Web PUBLISHES to signal its active/ttl/sync; ESP32
// SUBSCRIBES. Device never publishes, Web never subscribes.
const DEVICE_PUB = new Set([
  Topics.presence, Topics.snapshot, Topics.ack, Topics.log,
  Topics.configReported, Topics.remindersReported, Topics.historyReported,
  Topics.alarm, Topics.heartbeat,
]);

const DEVICE_SUB = new Set([
  Topics.command, Topics.configSet, Topics.remindersSet,
  Topics.historyRequest, Topics.session,
]);

const WEB_PUB = new Set([
  Topics.command, Topics.configSet, Topics.remindersSet,
  Topics.historyRequest, Topics.session,
]);

const WEB_SUB = new Set([
  Topics.presence, Topics.snapshot, Topics.ack, Topics.log,
  Topics.configReported, Topics.remindersReported, Topics.historyReported,
  Topics.uplink,
]);

// Topics for which broker refuses to retain even if the publisher sets
// retain=1. These must NEVER be stored (per §2).
export const RETAIN_FORBIDDEN = new Set([
  Topics.command, Topics.configSet, Topics.remindersSet,
  Topics.historyRequest, Topics.snapshot, Topics.ack, Topics.log,
  Topics.historyReported, Topics.configReported, Topics.session,
  Topics.alarm, Topics.heartbeat, Topics.uplink,
]);

// Suffixes handled by the ingest path instead of subscriber fanout.
export const UPLINK_SUFFIXES = new Set([Topics.alarm, Topics.heartbeat]);

// Topics that the contract explicitly allows to be retained.
export const RETAIN_ALLOWED = new Set([Topics.presence, Topics.remindersReported]);

// QoS cap per topic suffix (max QoS the broker will fan out at).
export function topicQosCap(suffix) {
  if (suffix === Topics.snapshot || suffix === Topics.log || suffix === Topics.session || suffix === Topics.uplink) return 0;
  return 1;
}

// Required publish QoS per topic suffix (contract §2). PUBLISH packets whose
// decoded QoS != this must be rejected: 0-QoS topics never get PUBACK, and
// 1-QoS topics must come with a packetId so the broker-level reliability
// stays intact. Mismatch → drop + close (second violation counts).
const PUB_QOS_REQUIREMENT = Object.freeze({
  [Topics.presence]: 1,
  [Topics.snapshot]: 0,
  [Topics.ack]: 1,
  [Topics.log]: 0,
  [Topics.configReported]: 1,
  [Topics.remindersReported]: 1,
  [Topics.historyReported]: 1,
  [Topics.command]: 1,
  [Topics.configSet]: 1,
  [Topics.remindersSet]: 1,
  [Topics.historyRequest]: 1,
  [Topics.session]: 0,
  [Topics.alarm]: 1,
  [Topics.heartbeat]: 1,
});

export function requiredPublishQos(suffix) {
  const q = PUB_QOS_REQUIREMENT[suffix];
  return typeof q === 'number' ? q : -1;
}

export function parseTopic(deviceId, topic) {
  const prefix = `${TOPIC_ROOT}/${deviceId}/`;
  if (!topic.startsWith(prefix)) return { ok: false, reason: 'wrong device' };
  const suffix = topic.slice(prefix.length);
  if (suffix.length === 0) return { ok: false, reason: 'empty suffix' };
  return { ok: true, suffix };
}

export function canPublish(role, deviceId, topic) {
  const parsed = parseTopic(deviceId, topic);
  if (!parsed.ok) return false;
  const set = role === 'device' ? DEVICE_PUB : role === 'web' ? WEB_PUB : null;
  if (!set) return false;
  return set.has(parsed.suffix);
}

// Returns the granted QoS (0 or 1), or 0x80 (not authorized).
export function evaluateSubscribe(role, deviceId, filter, requestedQos) {
  const parsed = parseTopic(deviceId, filter);
  if (!parsed.ok) return 0x80;
  const set = role === 'device' ? DEVICE_SUB : role === 'web' ? WEB_SUB : null;
  if (!set) return 0x80;
  if (!set.has(parsed.suffix)) return 0x80;
  const cap = topicQosCap(parsed.suffix);
  return Math.min(requestedQos, cap);
}

// Credential resolution. Returns { role: 'device'|'web' } or null.
//
// device: per-device password derived statelessly from a server secret:
//           password = hex HMAC-SHA256(secret, 'mayap-mqtt-device:v1\n<deviceId>')
//         The account Worker hands it to the device at /api/device/register (the
//         device keeps it in NVS); the broker recomputes it, so no D1 lookup and
//         no shared password. Username must equal this DO's device id.
// web:    a signed token bound to BOTH the device and the user, so a credential
//         issued for one device is useless on every other device DO:
//           password = "v1.<exp>.<hex HMAC-SHA256(secret,
//                       'mayap-mqtt-web:v1\n<deviceId>\n<username>\n<exp>')>"
//         Verified statelessly (no D1 in the connect path); lifetime is capped.
export const WEB_TOKEN_MAX_TTL_SEC = 7200;

const textEncoder = new TextEncoder();
const hexToBytes = (hex) => new Uint8Array(hex.match(/../g).map((byte) => parseInt(byte, 16)));
const bytesToHex = (bytes) => Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('');

async function hmacKey(secret, usages) {
  return crypto.subtle.importKey('raw', textEncoder.encode(String(secret)),
    { name: 'HMAC', hash: 'SHA-256' }, false, usages);
}

export async function signWebToken(secret, deviceId, username, expiresAtSec) {
  const mac = await crypto.subtle.sign('HMAC', await hmacKey(secret, ['sign']),
    textEncoder.encode(`mayap-mqtt-web:v1\n${deviceId}\n${username}\n${expiresAtSec}`));
  return `v1.${expiresAtSec}.${bytesToHex(new Uint8Array(mac))}`;
}

export async function verifyWebToken(secret, deviceId, username, token, nowMs = Date.now()) {
  const match = /^v1\.(\d{1,12})\.([0-9a-f]{64})$/.exec(String(token || ''));
  if (!secret || !match || !String(username || '').startsWith('web:')) return false;
  const expiresAt = Number(match[1]), nowSec = Math.floor(nowMs / 1000);
  if (expiresAt < nowSec || expiresAt > nowSec + WEB_TOKEN_MAX_TTL_SEC) return false;
  return crypto.subtle.verify('HMAC', await hmacKey(secret, ['verify']), hexToBytes(match[2]),
    textEncoder.encode(`mayap-mqtt-web:v1\n${deviceId}\n${username}\n${expiresAt}`));
}

// Broker -> main Worker ingest authentication (service binding, but the route is also reachable from the
// internet, so every request is signed). Both Workers already hold MQTT_DEVICE_SECRET /
// BROKER_DEVICE_SECRET (the same value), so no new secret is needed:
//   sig = hex HMAC-SHA256(secret, 'mayap-uplink-ingest:v1\n<tsMs>\n<body>')
// Replays are harmless (events are idempotent by event_id) but the timestamp bounds them anyway.
export const UPLINK_MAX_SKEW_MS = 5 * 60 * 1000;
export async function signUplink(secret, tsMs, body) {
  const mac = await crypto.subtle.sign('HMAC', await hmacKey(secret, ['sign']),
    textEncoder.encode(`mayap-uplink-ingest:v1\n${tsMs}\n${body}`));
  return bytesToHex(new Uint8Array(mac));
}
export async function verifyUplink(secret, tsHeader, body, sigHex, nowMs = Date.now()) {
  const ts = Number(tsHeader);
  if (!secret || !Number.isFinite(ts) || Math.abs(nowMs - ts) > UPLINK_MAX_SKEW_MS) return false;
  if (!/^[0-9a-f]{64}$/.test(String(sigHex || ''))) return false;
  return crypto.subtle.verify('HMAC', await hmacKey(secret, ['verify']), hexToBytes(sigHex),
    textEncoder.encode(`mayap-uplink-ingest:v1\n${ts}\n${body}`));
}

export async function deriveDevicePassword(secret, deviceId) {
  const mac = await crypto.subtle.sign('HMAC', await hmacKey(secret, ['sign']),
    textEncoder.encode(`mayap-mqtt-device:v1\n${deviceId}`));
  return bytesToHex(new Uint8Array(mac));
}

function constantTimeEqual(a, b) {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

export function makeCredentialResolver({ deviceSecret, webTokenSecret } = {}) {
  const devSecret = deviceSecret == null || deviceSecret === '' ? null : String(deviceSecret);
  const secret = webTokenSecret == null || webTokenSecret === '' ? null : String(webTokenSecret);
  return async function resolve(deviceId, username, passwordBytes) {
    const pwd = passwordBytes ? new TextDecoder('utf-8', { fatal: false }).decode(passwordBytes) : '';
    if (devSecret && username === deviceId && /^[0-9a-f]{64}$/.test(pwd) &&
        constantTimeEqual(pwd, await deriveDevicePassword(devSecret, deviceId))) return { role: 'device' };
    if (secret && await verifyWebToken(secret, deviceId, username, pwd)) {
      return { role: 'web', expiresAt: Number(/^v1\.(\d{1,12})\./.exec(pwd)[1]) };   // seconds; the broker bounds the socket's life by it
    }
    return null;
  };
}
