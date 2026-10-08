// Scripted stand-in for the main Worker's /api/internal/uplink (same signature check, same per-event verdicts as
// cloudflare/src/index.js ingestQueuedAlarm) with fault injection controlled over /__ctl.
import { verifyUplink } from '../../../src/broker/acl.js';

const COOLDOWN_MS = 15000;
let mode = { delayMs: 0, status: 0, loseReply: false, throttle: true, hbDelayMs: 0 };
const counters = { calls: 0, alarmsCalls: 0, hbCalls: 0, events: 0, stored: 0, duplicate: 0, throttled: 0, rejected: 0, badSig: 0 };
const last = new Map();                 // deviceId:type -> { state, at } (cooldown, like alarm_state)
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const json = (body, status = 200) => new Response(JSON.stringify(body), { status, headers: { 'content-type': 'application/json' } });

async function ingest(request, env) {
  const body = await request.text();
  counters.calls += 1;
  if (!await verifyUplink(env.MQTT_DEVICE_SECRET, request.headers.get('x-mayap-ts'), body, request.headers.get('x-mayap-sig'))) {
    counters.badSig += 1;
    return json({ success: false, error: 'UPLINK_AUTH' }, 401);
  }
  const message = JSON.parse(body);
  if (mode.delayMs) await sleep(message.kind === 'heartbeat' && mode.hbDelayMs ? mode.hbDelayMs : mode.delayMs);
  if (mode.status) return json({ success: false, error: 'DOWN' }, mode.status);
  if (message.kind === 'heartbeat') {
    counters.hbCalls += 1;
    await env.STORE.put(`hb:${message.device_id}`, JSON.stringify({ at: Date.now(), batch: Boolean(message.data?.batch_running) }));
    return json({ success: true, durable: true });
  }
  counters.alarmsCalls += 1;
  const results = [];
  const failedTypes = new Set();
  for (const event of message.events) {
    counters.events += 1;
    const key = `ev:${message.device_id}:${event.event_id}`;
    const typeKey = `${message.device_id}:${event.alarm_type}`;
    if (failedTypes.has(event.alarm_type)) { results.push({ event_id: event.event_id, outcome: 'skipped' }); continue; }
    const existing = await env.STORE.get(key, 'json');
    if (existing) {
      existing.writes += 1; counters.duplicate += 1;
      await env.STORE.put(key, JSON.stringify(existing));
      results.push({ event_id: event.event_id, outcome: 'duplicate' });
      continue;
    }
    const prior = last.get(typeKey);
    if (mode.throttle && prior && prior.state === event.state && Date.now() - prior.at < COOLDOWN_MS) {
      counters.throttled += 1; failedTypes.add(event.alarm_type);
      results.push({ event_id: event.event_id, outcome: 'throttled', retry_after_ms: Math.max(1000, COOLDOWN_MS - (Date.now() - prior.at)) });
      continue;
    }
    last.set(typeKey, { state: event.state, at: Date.now() });
    await env.STORE.put(key, JSON.stringify({ writes: 1, seq: ++counters.stored, type: event.alarm_type, state: event.state, at: Date.now() }));
    results.push({ event_id: event.event_id, outcome: 'stored' });
  }
  if (mode.loseReply) return json({ success: false, error: 'REPLY_LOST' }, 502);
  return json({ success: true, results });
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/api/internal/uplink' && request.method === 'POST') return ingest(request, env);
    if (url.pathname === '/__ctl') {
      mode = { ...mode, ...JSON.parse(url.searchParams.get('mode') || '{}') };
      if (url.searchParams.get('reset') === '1') { for (const k of Object.keys(counters)) counters[k] = 0; last.clear(); }
      return json({ mode, counters });
    }
    if (url.pathname === '/__stats') {
      const listed = await env.STORE.list({ prefix: 'ev:' });
      const events = [];
      for (const k of listed.keys) events.push({ key: k.name, ...(await env.STORE.get(k.name, 'json')) });
      return json({ counters, mode, events });
    }
    if (url.pathname === '/__clear') {
      const listed = await env.STORE.list();
      for (const k of listed.keys) await env.STORE.delete(k.name);
      for (const key of Object.keys(counters)) counters[key] = 0;
      last.clear();
      return json({ ok: true });
    }
    if (url.pathname.startsWith('/mqtt/') || url.pathname === '/healthz') return env.BROKER.fetch(request);
    return new Response('not found', { status: 404 });
  },
};
