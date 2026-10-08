import { getSubscriptionsForDevice } from './db.js';
import { sendWebPush } from './push.js';

const TTL_MS = 3600000;
const LEASE_MS = 15000;
const RETRY_MS = [2000, 10000, 30000, 60000, 300000];

export async function findAlarmEvent(env, deviceId, eventId) {
  return env.DB.prepare('SELECT * FROM alarm_events WHERE device_id=?1 AND event_id=?2')
    .bind(deviceId, eventId).first();
}

// D1 batch is atomic. A successful receipt means the event AND all recipient
// jobs are durable, not that an OS has displayed a notification.
export async function queueAlarmEvent(env, { deviceId, eventId, alarmType, notification, now = Date.now() }) {
  const existing = await findAlarmEvent(env, deviceId, eventId);
  if (existing) return existing;
  const subscriptions = await getSubscriptionsForDevice(env.DB, deviceId);
  const payload = JSON.stringify(notification);
  const statements = [env.DB.prepare(`INSERT INTO alarm_events
    (device_id,event_id,alarm_type,payload,created_at,expires_at) VALUES (?1,?2,?3,?4,?5,?6)
    ON CONFLICT(device_id,event_id) DO NOTHING`).bind(deviceId,eventId,alarmType,payload,now,now+TTL_MS)];
  for (const sub of subscriptions) statements.push(env.DB.prepare(`INSERT OR IGNORE INTO alarm_deliveries
    (event_seq,endpoint,next_attempt_at) SELECT order_seq,?3,?4 FROM alarm_events WHERE device_id=?1 AND event_id=?2`)
    .bind(deviceId,eventId,sub.endpoint,now));
  await env.DB.batch(statements);
  const event = await findAlarmEvent(env,deviceId,eventId);
  if (!event) throw new Error('ALARM_PERSIST_FAILED');
  console.log(`[alarm] stored device=${deviceId} event=${eventId} recipients=${subscriptions.length}`);
  return event;
}

export async function drainAlarmDeliveries(env, { now = Date.now(), limit = 16 } = {}) {
  // Preserve transitions for the same alarm on each phone. A retry of ACTIVE
  // must not arrive after RESOLVED. Claims also serialize overlapping Workers.
  const { results = [] } = await env.DB.prepare(`SELECT d.*,e.device_id,e.event_id,e.alarm_type,e.payload,e.expires_at
    FROM alarm_deliveries d JOIN alarm_events e ON e.order_seq=d.event_seq
    WHERE d.status='pending' AND d.next_attempt_at<=?1 AND d.lease_until<=?1 AND e.expires_at>?1
    AND NOT EXISTS (SELECT 1 FROM alarm_deliveries older JOIN alarm_events oe ON oe.order_seq=older.event_seq
      WHERE older.endpoint=d.endpoint AND older.status='pending' AND oe.device_id=e.device_id
      AND oe.alarm_type=e.alarm_type AND oe.order_seq<e.order_seq AND oe.expires_at>?1)
    ORDER BY e.order_seq LIMIT ?2`).bind(now, Math.max(1,Math.min(16,limit))).all();
  for (let start=0;start<results.length;start+=4) {
    await Promise.all(results.slice(start,start+4).map(async row => {
      const token=crypto.randomUUID();
      const claim=await env.DB.prepare(`UPDATE alarm_deliveries SET lease_token=?3,lease_until=?4
        WHERE event_seq=?1 AND endpoint=?2 AND status='pending' AND lease_until<=?5 RETURNING event_seq`)
        .bind(row.event_seq,row.endpoint,token,Date.now()+LEASE_MS,now).first();
      if (!claim) return;
      // Do not send durable work to a revoked owner or removed subscription.
      const sub=await env.DB.prepare(`SELECT ps.* FROM push_subscriptions ps JOIN users u
        ON u.google_sub=ps.user_sub AND u.disabled=0 JOIN user_devices ud
        ON ud.user_sub=ps.user_sub AND ud.device_id=ps.device_id
        WHERE ps.endpoint=?1 AND ps.device_id=?2`).bind(row.endpoint,row.device_id).first();
      const result=sub ? await sendWebPush(env,sub,JSON.parse(row.payload)) : {gone:true,status:410};
      const attempts=row.attempts+1;
      const finished=Date.now();
      const delay=RETRY_MS[Math.min(attempts-1,RETRY_MS.length-1)];
      await env.DB.prepare(`UPDATE alarm_deliveries SET status=?4,attempts=?5,next_attempt_at=?6,
        lease_token=NULL,lease_until=0,last_status=?7 WHERE event_seq=?1 AND endpoint=?2 AND lease_token=?3`)
        .bind(row.event_seq,row.endpoint,token,result.ok?'sent':result.gone?'gone':'pending',attempts,
          finished+Math.max(delay,Math.min(300000,result.retryAfterMs || 0)),result.status || 0).run();
      if (result.gone && sub) await env.DB.prepare('DELETE FROM push_subscriptions WHERE endpoint=?1').bind(row.endpoint).run();
      // Timing split: how long the device held the event (device_age_ms), how long the Worker held it before this push attempt
      // (since_received_ms), and the push service call itself (ms). "accepted" = the push service took it, not "displayed".
      let timing='';
      try {
        const data=JSON.parse(row.payload).data || {};
        if (Number.isFinite(data.receivedAt)) timing+=` since_received_ms=${Math.max(0,finished-data.receivedAt)}`;
        if (Number.isInteger(data.deviceAgeMs)) timing+=` device_age_ms=${data.deviceAgeMs} via=${data.via || 'https'}`;
      } catch {}
      console.log(`[alarm] delivery device=${row.device_id} event=${row.event_id} result=${result.ok?'accepted':result.gone?'gone':'retry'} ms=${finished-now}${timing}`);
    }));
  }
}

// Immediate delivery is outside the ESP32 request; two bounded quick retries
// cover transient failures. Cron resumes persisted work after Worker termination.
export function scheduleAlarmDelivery(env,ctx) {
  const work=(async()=>{
    await drainAlarmDeliveries(env,{limit:4});
    for(const delay of [2100,10100]) {
      await new Promise(resolve=>setTimeout(resolve,delay));
      await drainAlarmDeliveries(env,{limit:4});
    }
  })().catch(error=>console.error('[alarm] drain failed',String(error?.message || error)));
  if(ctx?.waitUntil) ctx.waitUntil(work);
  return work;
}
export async function maintainAlarmDeliveries(env) {
  await drainAlarmDeliveries(env);
  // Retain receipts for seven days, even when jobs expire after one hour.
  await env.DB.batch([
    env.DB.prepare("UPDATE alarm_deliveries SET status='expired',lease_token=NULL,lease_until=0 WHERE status='pending' AND event_seq IN (SELECT order_seq FROM alarm_events WHERE expires_at<=?1)").bind(Date.now()),
    env.DB.prepare('DELETE FROM alarm_deliveries WHERE event_seq IN (SELECT order_seq FROM alarm_events WHERE created_at<?1)').bind(Date.now()-7*86400000),
    env.DB.prepare('DELETE FROM alarm_events WHERE created_at<?1').bind(Date.now()-7*86400000),
  ]);
}

// What the Web needs when it has no realtime link: the alarms that are active right now and the latest events with what became
// of their Push (stored != pushed: `push.sent` is "accepted by the push service", `pending` still has retries ahead).
export async function alarmStatusSnapshot(env, deviceId, { now = Date.now(), eventLimit = 10 } = {}) {
  const active = await env.DB.prepare(`SELECT alarm_type,first_sent_at,last_sent_at,last_message FROM alarm_state
    WHERE device_id=?1 AND active=1 ORDER BY last_sent_at DESC LIMIT 20`).bind(deviceId).all();
  const recent = await env.DB.prepare(`SELECT e.order_seq,e.event_id,e.alarm_type,e.payload,e.created_at,
      COUNT(d.endpoint) AS recipients,
      COALESCE(SUM(d.status='sent'),0) AS sent, COALESCE(SUM(d.status='pending'),0) AS pending,
      COALESCE(SUM(d.status='gone'),0) AS gone, COALESCE(SUM(d.status='expired'),0) AS expired
    FROM alarm_events e LEFT JOIN alarm_deliveries d ON d.event_seq=e.order_seq
    WHERE e.device_id=?1 AND e.created_at>?2 GROUP BY e.order_seq ORDER BY e.order_seq DESC LIMIT ?3`)
    .bind(deviceId, now - TTL_MS, Math.max(1, Math.min(20, eventLimit))).all();
  return {
    server_time: now,
    alarms: (active.results || []).map(row => ({ alarm_type: row.alarm_type, since: row.first_sent_at, updated: row.last_sent_at, message: row.last_message })),
    events: (recent.results || []).map(row => {
      let data = {};
      try { data = JSON.parse(row.payload).data || {}; } catch {}
      return {
        event_id: row.event_id, seq: row.order_seq, alarm_type: row.alarm_type, state: data.state || null, severity: data.severity || null,
        message: data.message || '', received_at: row.created_at, device_age_ms: Number.isInteger(data.deviceAgeMs) ? data.deviceAgeMs : null,
        via: data.via || null,
        push: { recipients: row.recipients, sent: row.sent, pending: row.pending, gone: row.gone, expired: row.expired },
      };
    }),
  };
}
