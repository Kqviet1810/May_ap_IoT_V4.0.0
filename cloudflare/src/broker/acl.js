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
});

// Suffix lists keyed by role, enforced AFTER verifying the leading
// `mayap/v1/<deviceId>/` prefix matches this DO's device.

// Session (per V2): Web PUBLISHES to signal its active/ttl/sync; ESP32
// SUBSCRIBES. Device never publishes, Web never subscribes.
const DEVICE_PUB = new Set([
  Topics.presence, Topics.snapshot, Topics.ack, Topics.log,
  Topics.configReported, Topics.remindersReported, Topics.historyReported,
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
]);

// Topics for which broker refuses to retain even if the publisher sets
// retain=1. These must NEVER be stored (per §2).
export const RETAIN_FORBIDDEN = new Set([
  Topics.command, Topics.configSet, Topics.remindersSet,
  Topics.historyRequest, Topics.snapshot, Topics.ack, Topics.log,
  Topics.historyReported, Topics.configReported, Topics.session,
]);

// Topics that the contract explicitly allows to be retained.
export const RETAIN_ALLOWED = new Set([Topics.presence, Topics.remindersReported]);

// QoS cap per topic suffix (max QoS the broker will fan out at).
export function topicQosCap(suffix) {
  if (suffix === Topics.snapshot || suffix === Topics.log || suffix === Topics.session) return 0;
  return 1;
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

// Credential resolver interface — Phase 2B uses an in-memory fixture.
// Returns { role: 'device'|'web' } or null if credentials invalid.
export function makeFixtureCredentials({ devicePassword, webPassword } = {}) {
  const dev = devicePassword == null ? null : String(devicePassword);
  const web = webPassword == null ? null : String(webPassword);
  return function resolve(deviceId, username, passwordBytes) {
    const pwd = passwordBytes
      ? new TextDecoder('utf-8', { fatal: false }).decode(passwordBytes)
      : '';
    if (dev && username === deviceId && pwd === dev) return { role: 'device' };
    if (web && username && username.startsWith('web:') && pwd === web) return { role: 'web' };
    return null;
  };
}
