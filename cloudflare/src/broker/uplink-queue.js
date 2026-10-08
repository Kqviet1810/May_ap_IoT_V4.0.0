// MAYAP V4 broker - durable uplink queue (device alarm / heartbeat -> main Worker).
//
// Why it exists: the device must not wait for D1. A PUBACK on `alarm` means exactly BROKER_STORED: the event is in
// Durable Object storage (ordered, bounded, deduplicated by eventId). Handing it to the Worker (D1 + Web Push) is a
// separate, retried, observable step. Nothing here says anything about the MQTT transport: that is judged by the
// device from socket state, PINGRESP and rx timing.
//
//   BROKER_STORED   -> `uq:e:*` row written before the PUBACK is sent (DO output gate holds the PUBACK until durable)
//   D1_STORED       -> the Worker answered outcome=stored|duplicate for this event
//   PUSH_*          -> owned by the Worker (alarm_deliveries); the queue never claims it
//
// Pure with respect to the runtime: storage and clock are injected so every branch is unit-testable.

export const UQ = Object.freeze({
  MAX_UNFINISHED: 64,          // events waiting for the Worker (bounded: a full queue withholds the PUBACK = backpressure)
  DONE_KEEP: 48,               // finished rows kept for dedupe of device retransmits
  DONE_MAX_AGE_MS: 30 * 60 * 1000,
  MAX_AGE_MS: 24 * 60 * 60 * 1000,   // an event the Worker never accepted is dropped as `expired` (visible) after this
  BATCH_MAX: 8,
  FORWARD_TIMEOUT_MS: 8000,
  BACKOFF_MS: Object.freeze([2000, 5000, 15000, 60000, 300000]),
  THROTTLE_MIN_MS: 1000,
  THROTTLE_MAX_MS: 16000,
  HEARTBEAT_FORWARD_MS: 30000,
  REJECT_RING: 8,
  EVENT_ID_RE: /^[a-zA-Z0-9_-]{1,64}$/,
  TYPE_MAX: 64,
  MESSAGE_MAX: 1024,
});

const KEY_ENTRY = 'uq:e:';
const KEY_SEQ = 'uq:seq';
const KEY_HB = 'uq:hb';
const KEY_STAT = 'uq:stat';
const pad = (n) => String(n).padStart(10, '0');
const UNFINISHED = new Set(['pending', 'retrying', 'throttled']);

export function validateAlarm(data) {
  if (!data || typeof data !== 'object' || Array.isArray(data)) return false;
  if (typeof data.event_id !== 'string' || !UQ.EVENT_ID_RE.test(data.event_id)) return false;
  if (typeof data.alarm_type !== 'string' || !data.alarm_type.trim() || data.alarm_type.length > UQ.TYPE_MAX) return false;
  if (typeof data.message !== 'string' || data.message.length === 0 || data.message.length > UQ.MESSAGE_MAX) return false;
  return true;
}

export class UplinkQueue {
  constructor(storage, now = () => Date.now()) {
    this.storage = storage;
    this.now = now;
    this._chain = Promise.resolve();   // serialises enqueue so the seq counter never races (also in a non-gated stub)
  }

  _serial(fn) {
    const run = this._chain.then(fn, fn);
    this._chain = run.catch(() => {});
    return run;
  }

  async _rows() {
    const listed = await this.storage.list({ prefix: KEY_ENTRY });
    const rows = [];
    for (const [key, value] of listed.entries()) if (key.startsWith(KEY_ENTRY) && value) rows.push(value);
    rows.sort((a, b) => a.seq - b.seq);
    return rows;
  }

  async _stat() {
    return (await this.storage.get(KEY_STAT)) || { okAt: 0, errAt: 0, err: '', forwarded: 0, rejected: 0, expired: 0, overflow: 0, ring: [] };
  }

  async _saveStat(stat) { await this.storage.put(KEY_STAT, stat); }

  // BROKER_STORED. Returns 'stored' | 'duplicate' | 'full' | 'invalid'. Only 'stored'/'duplicate' may be PUBACKed.
  enqueueAlarm(data) {
    return this._serial(async () => {
      if (!validateAlarm(data)) return { status: 'invalid' };
      const now = this.now();
      const rows = await this._rows();
      const same = rows.find((row) => row.id === data.event_id);
      if (same) {
        const a = same.data || {};
        if (a.alarm_type !== data.alarm_type || a.state !== data.state || a.message !== data.message) {
          // Same id, different content: a device bug, never a retransmit. It is acknowledged (so the device does not loop on it),
          // counted and remembered where the Web can see it - not forwarded and not silently merged.
          const stat = await this._stat();
          stat.rejected += 1;
          stat.ring = [...(stat.ring || []), { id: data.event_id, type: String(data.alarm_type), err: 'EVENT_ID_CONFLICT', at: now }].slice(-UQ.REJECT_RING);
          await this._saveStat(stat);
          return { status: 'conflict', seq: same.seq };
        }
        return { status: 'duplicate', seq: same.seq, state: same.state };
      }
      const unfinished = rows.filter((row) => UNFINISHED.has(row.state)).length;
      if (unfinished >= UQ.MAX_UNFINISHED) {
        const stat = await this._stat();
        stat.overflow += 1;
        await this._saveStat(stat);
        return { status: 'full' };
      }
      const seq = ((await this.storage.get(KEY_SEQ)) || 0) + 1;
      const row = { seq, id: data.event_id, type: data.alarm_type, state: 'pending', data, recvAt: now, attempts: 0, nextAt: now, err: '' };
      await this.storage.put(KEY_SEQ, seq);
      await this.storage.put(KEY_ENTRY + pad(seq), row);
      return { status: 'stored', seq };
    });
  }

  async recordHeartbeat(data) {
    const now = this.now();
    const prior = (await this.storage.get(KEY_HB)) || { at: 0, fwdAt: 0, dirty: false, batch: false, fails: 0 };
    prior.at = now;
    prior.dirty = true;
    prior.batch = Boolean(data && data.batch_running);
    await this.storage.put(KEY_HB, prior);
  }

  // Rows the Worker should get now: oldest first, same-type order preserved by the Worker's per-type skip rule, and a row
  // behind an unfinished older row of the SAME type only goes along in the same batch (never alone, never reordered).
  async pickBatch() {
    const now = this.now();
    const rows = await this._rows();
    const blocked = new Set();      // alarm types whose earlier row is not eligible yet
    const batch = [];
    for (const row of rows) {
      if (!UNFINISHED.has(row.state)) continue;
      if (blocked.has(row.type)) continue;
      if (row.nextAt > now) { blocked.add(row.type); continue; }
      if (batch.length < UQ.BATCH_MAX) batch.push(row); else blocked.add(row.type);
    }
    return batch;
  }

  // `results` is the Worker's per-event verdict list: { event_id, outcome, retry_after_ms?, error? }.
  // outcome: stored | duplicate (-> forwarded), throttled (-> wait, order kept), rejected (-> terminal), anything else -> retry.
  async applyResults(batch, results, failure) {
    const now = this.now();
    const stat = await this._stat();
    const byId = new Map();
    for (const item of Array.isArray(results) ? results : []) if (item && typeof item.event_id === 'string') byId.set(item.event_id, item);
    let hardFail = false;
    for (const row of batch) {
      const verdict = failure ? null : byId.get(row.id);
      const outcome = verdict && typeof verdict.outcome === 'string' ? verdict.outcome : '';
      if (outcome === 'skipped') {      // an earlier event of the same alarm type was not accepted: wait, order is kept
        row.nextAt = now + UQ.THROTTLE_MIN_MS;
        await this.storage.put(KEY_ENTRY + pad(row.seq), row);
        continue;
      }
      row.attempts += 1;
      if (outcome === 'stored' || outcome === 'duplicate') {
        row.state = 'forwarded'; row.doneAt = now; row.err = '';
        stat.forwarded += 1; stat.okAt = now;
      } else if (outcome === 'throttled') {
        const wait = Math.min(UQ.THROTTLE_MAX_MS, Math.max(UQ.THROTTLE_MIN_MS, Number(verdict.retry_after_ms) || UQ.THROTTLE_MIN_MS));
        row.state = 'throttled'; row.nextAt = now + wait; row.err = 'THROTTLED';
      } else if (outcome === 'rejected') {
        row.state = 'rejected'; row.doneAt = now; row.err = String(verdict.error || 'REJECTED').slice(0, 48);
        stat.rejected += 1;
        stat.ring = [...(stat.ring || []), { id: row.id, type: row.type, err: row.err, at: now }].slice(-UQ.REJECT_RING);
      } else {
        // Worker/D1 unavailable, timeout, malformed answer, or a later event skipped behind a failed one: retry with backoff.
        const step = Math.min(UQ.BACKOFF_MS.length - 1, row.attempts - 1);
        row.state = 'retrying';
        row.nextAt = now + UQ.BACKOFF_MS[step];
        row.err = failure ? String(failure).slice(0, 48) : String((verdict && verdict.error) || outcome || 'NO_RESULT').slice(0, 48);
        hardFail = true;
        stat.err = row.err; stat.errAt = now;
      }
      await this.storage.put(KEY_ENTRY + pad(row.seq), row);
    }
    if (!hardFail) { stat.err = ''; }
    await this._saveStat(stat);
    return { hardFail };
  }

  // Heartbeat to forward now, or null. `batch_running` is the latest value; the Worker only needs "alive + batch flag".
  async heartbeatDue() {
    const now = this.now();
    const hb = await this.storage.get(KEY_HB);
    if (!hb || !hb.dirty) return null;
    const waitBackoff = hb.fails > 0 ? UQ.BACKOFF_MS[Math.min(UQ.BACKOFF_MS.length - 1, hb.fails - 1)] : UQ.HEARTBEAT_FORWARD_MS;
    if (hb.fwdAt && now - hb.fwdAt < Math.min(waitBackoff, hb.fails > 0 ? waitBackoff : UQ.HEARTBEAT_FORWARD_MS)) return null;
    return hb;
  }

  async heartbeatDone(sentAt, ok) {
    const now = this.now();
    const hb = await this.storage.get(KEY_HB);
    if (!hb) return;
    if (ok) { hb.fwdAt = now; hb.fails = 0; if (hb.at <= sentAt) hb.dirty = false; }
    else { hb.fwdAt = now; hb.fails = Math.min(255, (hb.fails || 0) + 1); }
    await this.storage.put(KEY_HB, hb);
  }

  // Drops finished rows past the retention window and expires events the Worker never accepted.
  async maintain() {
    const now = this.now();
    const rows = await this._rows();
    const stat = await this._stat();
    let changed = false;
    for (const row of rows) {
      if (UNFINISHED.has(row.state) && now - row.recvAt > UQ.MAX_AGE_MS) {
        row.state = 'expired'; row.doneAt = now; row.err = 'EXPIRED';
        stat.expired += 1; changed = true;
        stat.ring = [...(stat.ring || []), { id: row.id, type: row.type, err: 'EXPIRED', at: now }].slice(-UQ.REJECT_RING);
        await this.storage.put(KEY_ENTRY + pad(row.seq), row);
      }
    }
    const done = rows.filter((row) => !UNFINISHED.has(row.state));
    done.sort((a, b) => a.seq - b.seq);
    const drop = done.filter((row, index) => index < done.length - UQ.DONE_KEEP || now - (row.doneAt || row.recvAt) > UQ.DONE_MAX_AGE_MS);
    for (const row of drop) await this.storage.delete(KEY_ENTRY + pad(row.seq));
    if (changed) await this._saveStat(stat);
  }

  // Earliest time the queue needs the DO awake again (null = nothing to do).
  async nextWake() {
    const now = this.now();
    let at = null;
    const take = (value) => { at = at === null ? value : Math.min(at, value); };
    for (const row of await this._rows()) if (UNFINISHED.has(row.state)) take(Math.max(row.nextAt, now));
    const hb = await this.storage.get(KEY_HB);
    if (hb && hb.dirty) {
      const wait = hb.fails > 0 ? UQ.BACKOFF_MS[Math.min(UQ.BACKOFF_MS.length - 1, hb.fails - 1)] : UQ.HEARTBEAT_FORWARD_MS;
      take(Math.max(now, (hb.fwdAt || 0) + wait));
    }
    return at;
  }

  // Observable state for the Web (`uplink` topic) and for tests.
  async status() {
    const now = this.now();
    const rows = await this._rows();
    const open = rows.filter((row) => UNFINISHED.has(row.state));
    const stat = await this._stat();
    const hb = await this.storage.get(KEY_HB);
    const oldest = open.length ? Math.min(...open.map((row) => row.recvAt)) : 0;
    return {
      pending: open.length,
      retrying: open.filter((row) => row.state === 'retrying').length,
      throttled: open.filter((row) => row.state === 'throttled').length,
      oldest_age_ms: oldest ? Math.max(0, now - oldest) : 0,
      forwarded: stat.forwarded, rejected: stat.rejected, expired: stat.expired, overflow: stat.overflow,
      last_ok_at: stat.okAt, last_error: stat.err, last_error_at: stat.errAt,
      recent_rejected: (stat.ring || []).slice(-3),
      heartbeat_pending: Boolean(hb && hb.dirty),
      at: now,
    };
  }
}
