import { findAlarmEvent, queueAlarmEvent, scheduleAlarmDelivery, maintainAlarmDeliveries } from './alarm-delivery.js';
import { deriveDevicePassword, verifyUplink } from './broker/acl.js';
import { hashDeviceKey, verifyDeviceKey, randomToken, isValidDeviceId } from './auth.js';
import {
  getDeviceByDeviceId,
  insertDevice,
  touchDevice,
  touchDeviceHeartbeat,
  setDeviceStatus,
  renameDevice,
  setDevicePinHash,
  setDeviceKeyHash,
  getStaleOnlineDevices,
  getRecoveredOfflineDevices,
  getSubscriptionsForDevice,
  getSubscriptionByEndpoint,
  upsertSubscription,
  deleteSubscriptionByEndpoint,
  getAlarmState,
  upsertAlarmState,
  insertAlarmLog,
  getCachedFirmware,
  setFirmwareCache,
  touchFirmwareCache,
} from './db.js';
import { sendWebPush, buildNotificationPayload } from './push.js';

// Ran an toan phia server, DOC LAP voi anti-spam cua ESP32 (xem cloud_alert_link.h):
// du firmware co loi va goi lien tuc, worker cung khong ban push nhanh hon
// muc nay cho CUNG mot (device_id, alarm_type) khi trang thai khong doi.
const MIN_ALARM_COOLDOWN_MS = 15_000;
const PIN_RATE_WINDOW_MS = 15 * 60 * 1000;
const PIN_RATE_BLOCK_MS = 15 * 60 * 1000;
const PIN_RATE_MAX_FAILURES = 5;

function bytesToHex(bytes) {
  return [...new Uint8Array(bytes)].map((b) => b.toString(16).padStart(2, '0')).join('');
}
async function hmacSha256(keyBytes, message) {
  const key = await crypto.subtle.importKey(
    'raw', keyBytes, { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']
  );
  return crypto.subtle.sign('HMAC', key, new TextEncoder().encode(message));
}
async function deriveCommandKeyHex(env, deviceId) {
  const master = String(env.DEVICE_KEY_PEPPER || '');
  if (!master || !isValidDeviceId(deviceId)) return '';
  const digest = await hmacSha256(
    new TextEncoder().encode(master),
    `mayap-command-key:v1:${deviceId}`
  );
  return bytesToHex(digest);
}
// Per-device MQTT broker password (stateless; the broker derives the same value).
async function deriveMqttPasswordHex(env, deviceId) {
  const secret = String(env.MQTT_DEVICE_SECRET || '');
  if (!secret || !isValidDeviceId(deviceId)) return '';
  return deriveDevicePassword(secret, deviceId);
}
function pinRateKey(request, deviceId) {
  const ip = request.headers.get('CF-Connecting-IP') || 'unknown';
  return `pin:${deviceId}:${ip}`;
}
async function readPinRate(env, key) {
  return env.DB.prepare('SELECT attempts, window_started_at, blocked_until FROM auth_rate_limits WHERE rate_key = ?1').bind(key).first();
}
async function pinRateAllowed(env, key, now) {
  const row = await readPinRate(env, key);
  if (!row) return { allowed: true };
  if (Number(row.blocked_until || 0) > now) return { allowed: false, retryAfterMs: Number(row.blocked_until) - now };
  if (now - Number(row.window_started_at || 0) >= PIN_RATE_WINDOW_MS) {
    await env.DB.prepare('DELETE FROM auth_rate_limits WHERE rate_key = ?1').bind(key).run();
  }
  return { allowed: true };
}
async function recordPinFailure(env, key, now) {
  const row = await readPinRate(env, key);
  const fresh = !row || now - Number(row.window_started_at || 0) >= PIN_RATE_WINDOW_MS;
  const attempts = fresh ? 1 : Number(row.attempts || 0) + 1;
  const started = fresh ? now : Number(row.window_started_at);
  const blocked = attempts >= PIN_RATE_MAX_FAILURES ? now + PIN_RATE_BLOCK_MS : 0;
  await env.DB.prepare(`INSERT INTO auth_rate_limits
    (rate_key, attempts, window_started_at, blocked_until, updated_at)
    VALUES (?1, ?2, ?3, ?4, ?5)
    ON CONFLICT(rate_key) DO UPDATE SET attempts=excluded.attempts,
    window_started_at=excluded.window_started_at,
    blocked_until=excluded.blocked_until, updated_at=excluded.updated_at`)
    .bind(key, attempts, started, blocked, now).run();
}
async function clearPinFailures(env, key) {
  await env.DB.prepare('DELETE FROM auth_rate_limits WHERE rate_key = ?1').bind(key).run();
}
async function verifyPinGuarded(request, env, device, pin) {
  const now = Date.now();
  const key = pinRateKey(request, device.device_id);
  const rate = await pinRateAllowed(env, key, now);
  if (!rate.allowed) return { valid: false, limited: true };
  const valid = await verifyDevicePin(env, device, pin);
  if (valid) await clearPinFailures(env, key);
  else await recordPinFailure(env, key, now);
  return { valid, limited: false };
}

// ESP32 heartbeat moi 15s (CLOUD_HEARTBEAT_INTERVAL_MS trong config.h). Nguong
// nay TRUOC DAY la 75s (~5 lan bo lo heartbeat) - du de tha luc mang giat
// nhe, nhung KHONG du cho 1 chu ky khoi dong lai CO CHU DICH: nap firmware
// qua web (tai file ~3MB tu GitHub qua Worker + ghi flash) hoac quay lai
// ban cu (esp_ota_set_boot_partition + restart) deu ket thuc bang mot lan
// restart that su - ESP32 mat toi thieu vai chuc giay de tai/ghi xong, roi
// them thoi gian ket noi lai Wi-Fi/MQTT/heartbeat, cong don co the vuot 75s
// trong dieu kien mang binh thuong (chua tinh mang yeu). Ket qua: nguoi dung
// nap OTA/quay lai firmware xong lai nhan canh bao "mat dien/mat Wi-Fi" gia,
// dung luc thiet bi van dang tu khoi dong lai binh thuong. Nang len 180s (3
// phut) de bao trum thoai mai ca 2 truong hop nay - van la do tre chap nhan
// duoc cho 1 canh bao "mat dien that su" tren mot he thong ap trung (nhiet
// do khong the doi trong vai phut do khoi luong nhiet cua tu ap), va van
// nam trong gioi han Cron Trigger 1 phut/lan cua Cloudflare (khong the
// nhanh hon du muon).
const DEVICE_OFFLINE_THRESHOLD_MS = 180 * 1000;

function corsHeaders(env) {
  return {
    'Access-Control-Allow-Origin': env.ALLOWED_ORIGIN || '*',
    'Access-Control-Allow-Methods': 'GET, POST, DELETE, OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type',
    'Access-Control-Max-Age': '86400',
  };
}

// Repo GitHub luu ma nguon firmware - noi phat hanh cac ban ".bin" (xem
// .github/workflows/build-firmware.yml: tu dong bien dich + tao GitHub
// Release moi khi day tag "vX.Y.Z"). Day la NGUON DUY NHAT cho tinh nang
// cap nhat firmware tu xa - khong con trang admin/upload thu cong.
const GITHUB_OWNER = 'Kqviet1810';
const GITHUB_REPO = 'May_ap_trung_V2.1.1';
// Bao lau thi coi cache la "cu", can hoi lai GitHub xem tag co doi khong
// (hoi nhe, khong tai file - chi tai+bam lai file khi THAT SU co tag moi).
const FIRMWARE_CACHE_MAX_AGE_MS = 10 * 60 * 1000;

function json(env, data, status = 200) {
  return new Response(JSON.stringify(data), {
    status,
    headers: { 'Content-Type': 'application/json; charset=utf-8', ...corsHeaders(env) },
  });
}

async function readJson(request) {
  try {
    return await request.json();
  } catch (_) {
    return null;
  }
}

// -------------------------- Endpoint: dang ky thiet bi --------------------------
// Trust-on-first-use: lan dau goi voi 1 device_id chua ton tai se TAO thiet bi
// va luu hash cua device_key gui len; cac lan sau PHAI gui dung device_key cu
// (khong cho ai "chiem" mot device_id da co bang cach dang ky de len lai).
async function handleRegister(request, env) {
  const body = await readJson(request);
  const deviceId = String(body?.device_id || '').trim();
  const deviceKey = String(body?.device_key || '');
  const deviceName = body?.device_name ? String(body.device_name).slice(0, 64) : '';

  if (!isValidDeviceId(deviceId) || deviceKey.length < 8) {
    return json(env, { success: false, error: 'device_id/device_key khong hop le' }, 400);
  }

  const now = Date.now();
  const existing = await getDeviceByDeviceId(env.DB, deviceId);
  if (!existing) {
    const commandKey = await deriveCommandKeyHex(env, deviceId);
    if (!commandKey) return json(env, { success: false, error: 'may chu thieu khoa bao ve lenh' }, 503);
    const deviceKeyHash = await hashDeviceKey(deviceKey, env.DEVICE_KEY_PEPPER);
    const pairingToken = randomToken(12);
    await insertDevice(env.DB, { deviceId, deviceName, deviceKeyHash, pairingToken, now });
    const webPin = await issueWebPin(env, deviceId);
    const mqttPassword = await deriveMqttPasswordHex(env, deviceId);
    return json(env, { success: true, device_id: deviceId, pairing_token: pairingToken, web_pin: webPin, command_key: commandKey,
      ...(mqttPassword ? { mqtt_password: mqttPassword } : {}), created: true });
  }

  const valid = await verifyDeviceKey(deviceKey, env.DEVICE_KEY_PEPPER, existing.device_key_hash);
  if (!valid) {
    return json(env, { success: false, error: 'device_key khong khop voi thiet bi da dang ky' }, 401);
  }
  // KHONG truyen deviceName o day: ESP32 luon gui device_name = chinh
  // device_id cua no (khong co gia tri gi hon), truyen vao se GHI DE mat ten
  // than thien nguoi dung da tu doi qua web moi lan ESP32 dang ky lai (moi
  // lan khoi dong lai). Ten hien thi gio HOAN TOAN do web quan ly (xem
  // handleRenameDevice) - firmware khong con vai tro gi voi truong nay.
  await touchDevice(env.DB, deviceId, 'online', now);
  // Di tru ban cu con web_pin_hash=NULL: cap PIN ngau nhien mot lan va tra
  // ve firmware. May da co PIN thi endpoint dang ky khong phat lai PIN.
  const webPin = existing.web_pin_hash ? '' : await issueWebPin(env, deviceId);
  const commandKey = await deriveCommandKeyHex(env, deviceId);
  if (!commandKey) return json(env, { success: false, error: 'may chu thieu khoa bao ve lenh' }, 503);
  const mqttPassword = await deriveMqttPasswordHex(env, deviceId);
  return json(env, {
    success: true,
    device_id: deviceId,
    pairing_token: existing.pairing_token,
    command_key: commandKey,
    ...(mqttPassword ? { mqtt_password: mqttPassword } : {}),
    ...(webPin ? { web_pin: webPin } : {}),
    created: false,
  });
}

// -------------------------- Endpoint: heartbeat --------------------------
async function handleHeartbeat(request, env) {
  const body = await readJson(request);
  const deviceId = String(body?.device_id || '').trim();
  const deviceKey = String(body?.device_key || '');

  const device = await getDeviceByDeviceId(env.DB, deviceId);
  if (!device) return json(env, { success: false, error: 'device chua dang ky' }, 404);
  const valid = await verifyDeviceKey(deviceKey, env.DEVICE_KEY_PEPPER, device.device_key_hash);
  if (!valid) return json(env, { success: false, error: 'device_key sai' }, 401);

  await touchDeviceHeartbeat(env.DB, deviceId, Date.now(), Boolean(body?.batch_running));
  return json(env, { success: true });
}

// -------------------------- Endpoint: dat lai PIN web ve mac dinh --------------------------
// Xac thuc bang device_key (bi mat cua FIRMWARE, khac hoan toan web_pin cua
// nguoi dung) - giong het handleHeartbeat/handleAlarm, KHONG dung
// verifyDevicePin(). Day la chu dich: chi thiet bi that (ESP32 goi tu HMI,
// xem cloud_alert_link.h::sendResetPin) moi kich hoat duoc, dam bao "co mat
// vat ly tai may" la dieu kien duy nhat de khoi phuc PIN da quen - khong ai
// tu web goi duoc lenh nay du co biet ca device_id lan web_pin cu.
async function handleResetPin(request, env) {
  const body = await readJson(request);
  const deviceId = String(body?.device_id || '').trim();
  const deviceKey = String(body?.device_key || '');

  const device = await getDeviceByDeviceId(env.DB, deviceId);
  if (!device) return json(env, { success: false, error: 'device chua dang ky' }, 404);
  const valid = await verifyDeviceKey(deviceKey, env.DEVICE_KEY_PEPPER, device.device_key_hash);
  if (!valid) return json(env, { success: false, error: 'device_key sai' }, 401);

  const webPin = await issueWebPin(env, deviceId);
  return json(env, { success: true, web_pin: webPin });
}

async function handleRotateDeviceKey(request, env) {
  const body = await readJson(request);
  const deviceId = String(body?.device_id || '').trim();
  const oldKey = String(body?.device_key || '');
  const newKey = String(body?.new_device_key || '');
  if (!isValidDeviceId(deviceId) || newKey.length < 32 || newKey.length > 128) {
    return json(env, { success: false, error: 'du lieu xoay khoa khong hop le' }, 400);
  }
  const device = await getDeviceByDeviceId(env.DB, deviceId);
  if (!device) return json(env, { success: false, error: 'device chua dang ky' }, 404);
  if (!await verifyDeviceKey(oldKey, env.DEVICE_KEY_PEPPER, device.device_key_hash)) {
    return json(env, { success: false, error: 'device_key sai' }, 401);
  }
  const newHash = await hashDeviceKey(newKey, env.DEVICE_KEY_PEPPER);
  await setDeviceKeyHash(env.DB, deviceId, newHash);
  return json(env, { success: true });
}

// -------------------------- Endpoint: bao dong / canh bao --------------------------
function alarmFields(body) {
  return {
    alarmType: String(body?.alarm_type || '').trim(),
    message: String(body?.message || '').slice(0, 300),
    severity: ['info', 'warning', 'critical', 'system'].includes(body?.severity) ? body.severity : 'warning',
    state: body?.state === 'resolved' ? 'resolved' : 'active',
    temperature: Number.isFinite(Number(body?.temperature)) ? Number(body.temperature) : null,
    humidity: Number.isFinite(Number(body?.humidity)) ? Number(body.humidity) : null,
  };
}

// One alarm event for an already authenticated device. Shared by the single endpoint and the batch
// endpoint so both keep identical idempotency (event_id), conflict and cooldown semantics.
// Returns { status, payload, schedule } - the caller sends the HTTP response and starts delivery once.
async function recordAlarmEvent(env, device, deviceId, body, fields, via = 'https') {
  const { alarmType, message, severity, state, temperature, humidity } = fields;
  const now = Date.now();
  const suppliedId=body?.event_id;
  if (suppliedId !== undefined && !/^[a-zA-Z0-9_-]{1,64}$/.test(String(suppliedId)))
    return { status: 400, payload: {success:false,error:'INVALID_EVENT_ID'} };
  const eventId=suppliedId || crypto.randomUUID();
  const existing=await findAlarmEvent(env,deviceId,eventId);
  if (existing) {
    const saved=JSON.parse(existing.payload).data;
    if (saved.alarmType!==alarmType || saved.state!==state || saved.message!==message)
      return { status: 409, payload: {success:false,error:'EVENT_ID_CONFLICT'} };
    return { status: 200, payload: {success:true,durable:true,stored:true,event_id:eventId,duplicate:true}, schedule: true };
  }
  await touchDevice(env.DB,deviceId,'online',now);
  const priorState=await getAlarmState(env.DB,deviceId,alarmType);
  const unchanged=priorState && Boolean(priorState.active)===(state==='active');
  if (unchanged && now-Number(priorState.last_sent_at || 0)<MIN_ALARM_COOLDOWN_MS) {
    // New firmware retains the event and retries; older firmware preserves its
    // previous throttle semantics. Already-durable duplicates bypass this gate.
    return { status: suppliedId?429:200, payload: {success:!suppliedId,durable:false,throttled:true} };
  }
  const notification=buildNotificationPayload({deviceId,deviceName:device.device_name,alarmType,severity,state,message,temperature,humidity});
  notification.data.eventId=eventId;notification.data.message=message;
  notification.data.receivedAt=now;
  notification.data.detectedUptimeMs=Number.isInteger(body?.detected_uptime_ms) ? body.detected_uptime_ms : null;
  // How long the device held the event before sending it (detection -> send) and the channel it came in on: together with
  // receivedAt this separates detection, device queueing/fallback, Worker receipt and Push in the logs and the status endpoint.
  notification.data.deviceAgeMs=Number.isInteger(body?.age_ms) && body.age_ms>=0 && body.age_ms<=7*86400000 ? body.age_ms : null;
  notification.data.via=via;
  const event=await queueAlarmEvent(env,{deviceId,eventId,alarmType,notification,now});
  const persisted=JSON.parse(event.payload).data;
  if(persisted.alarmType!==alarmType || persisted.state!==state || persisted.message!==message)
    return { status: 409, payload: {success:false,error:'EVENT_ID_CONFLICT'} };
  // The receipt above remains valid even if ancillary history updates fail.
  try {
    await upsertAlarmState(env.DB,{deviceId,alarmType,active:state==='active',firstSentAt:now,lastSentAt:now,lastMessage:message});
    await insertAlarmLog(env.DB,{deviceId,alarmType,severity,state,message,temperature,humidity,notificationSent:false,now});
  } catch(error) {console.error('[alarm] history failed',String(error?.message || error));}
  // `durable`/`stored`: the event and its push jobs are in D1. It says nothing about the push itself (see /status `push`).
  return { status: 200, payload: {success:true,durable:true,stored:true,event_id:event.event_id}, schedule: true };
}

async function handleAlarm(request, env, ctx) {
  const body = await readJson(request);
  const deviceId = String(body?.device_id || '').trim();
  const deviceKey = String(body?.device_key || '');
  const fields = alarmFields(body);

  if (!isValidDeviceId(deviceId) || !fields.alarmType || !fields.message) {
    return json(env, { success: false, error: 'thieu device_id/alarm_type/message' }, 400);
  }

  const device = await getDeviceByDeviceId(env.DB, deviceId);
  if (!device) return json(env, { success: false, error: 'device chua dang ky' }, 404);
  const valid = await verifyDeviceKey(deviceKey, env.DEVICE_KEY_PEPPER, device.device_key_hash);
  if (!valid) return json(env, { success: false, error: 'device_key sai' }, 401);

  const result = await recordAlarmEvent(env, device, deviceId, body, fields, 'https');
  if (result.schedule) scheduleAlarmDelivery(env,ctx);
  return json(env, result.payload, result.status);
}

// -------------------------- Endpoint: nhieu bao dong trong 1 request --------------------------
// The ESP32 pays one TLS handshake per request (RAM it has to take from the realtime link), so it
// sends its queued events together. Auth once, then each event goes through recordAlarmEvent in
// order; results are per event so the device removes exactly the ones that became durable.
// After one event of an alarm type fails, later events of that type are skipped (kept in order).
const MAX_ALARM_BATCH = 8;
async function handleAlarmBatch(request, env, ctx) {
  const body = await readJson(request);
  const deviceId = String(body?.device_id || '').trim();
  const deviceKey = String(body?.device_key || '');
  const events = Array.isArray(body?.events) ? body.events : null;
  if (!isValidDeviceId(deviceId) || !events || events.length === 0 || events.length > MAX_ALARM_BATCH) {
    return json(env, { success: false, error: 'thieu device_id/events (1..' + MAX_ALARM_BATCH + ')' }, 400);
  }
  const device = await getDeviceByDeviceId(env.DB, deviceId);
  if (!device) return json(env, { success: false, error: 'device chua dang ky' }, 404);
  const valid = await verifyDeviceKey(deviceKey, env.DEVICE_KEY_PEPPER, device.device_key_hash);
  if (!valid) return json(env, { success: false, error: 'device_key sai' }, 401);

  const results = [];
  const failedTypes = new Set();
  let schedule = false;
  for (const event of events) {
    const fields = alarmFields(event);
    const eventId = event?.event_id === undefined ? undefined : String(event.event_id);
    if (!fields.alarmType || !fields.message || eventId === undefined) {
      results.push({ event_id: eventId, status: 400, success: false, durable: false, error: 'thieu event_id/alarm_type/message' });
      continue;
    }
    if (failedTypes.has(fields.alarmType)) {
      results.push({ event_id: eventId, status: 409, success: false, durable: false, error: 'SKIPPED_AFTER_FAILURE' });
      continue;
    }
    let outcome;
    try {
      outcome = await recordAlarmEvent(env, device, deviceId, event, fields, 'https');
    } catch (error) {
      console.error('[alarm] batch event failed', String(error?.message || error));
      outcome = { status: 500, payload: { success: false, durable: false, error: 'STORAGE_ERROR' } };
    }
    if (outcome.schedule) schedule = true;
    if (outcome.payload.durable !== true) failedTypes.add(fields.alarmType);
    results.push({ ...outcome.payload, durable: outcome.payload.durable === true, event_id: eventId, status: outcome.status });
  }
  if (schedule) scheduleAlarmDelivery(env, ctx);
  return json(env, { success: true, results });
}

// -------------------------- Endpoint: alarm/heartbeat tu ESP32 qua MQTT (broker -> Worker) --------------------------
// Only the MQTT broker calls this (service binding), signed with the shared device secret; the device itself was
// authenticated by the broker with its per-device MQTT password. Reuses the exact alarm/heartbeat writes of the HTTPS
// endpoints. `durable:true` is the only thing that makes the broker PUBACK the device.
async function handleUplink(request, env, ctx) {
  const bodyText = await request.text();
  const secret = String(env.MQTT_DEVICE_SECRET || '');
  if (bodyText.length > 4096 ||
      !await verifyUplink(secret, request.headers.get('x-mayap-ts'), bodyText, request.headers.get('x-mayap-sig'))) {
    return json(env, { success: false, error: 'UPLINK_AUTH' }, 401);
  }
  let message;
  try { message = JSON.parse(bodyText); } catch { return json(env, { success: false, error: 'BAD_JSON' }, 400); }
  const deviceId = String(message?.device_id || '').trim();
  const data = message?.data;
  if (!isValidDeviceId(deviceId) || !data || typeof data !== 'object') {
    return json(env, { success: false, error: 'BAD_UPLINK' }, 400);
  }
  const device = await getDeviceByDeviceId(env.DB, deviceId);
  if (!device) return json(env, { success: true, durable: false, error: 'device chua dang ky' });
  if (message.kind === 'heartbeat') {
    await touchDeviceHeartbeat(env.DB, deviceId, Date.now(), Boolean(data.batch_running));
    return json(env, { success: true, durable: true });
  }
  if (message.kind === 'alarm') {
    const fields = alarmFields(data);
    if (!fields.alarmType || !fields.message || data.event_id === undefined) {
      return json(env, { success: true, durable: false, status: 400, error: 'thieu event_id/alarm_type/message' });
    }
    const outcome = await recordAlarmEvent(env, device, deviceId, data, fields, 'mqtt');
    if (outcome.schedule) scheduleAlarmDelivery(env, ctx);
    return json(env, { success: true, durable: outcome.payload.durable === true, status: outcome.status });
  }
  return json(env, { success: false, error: 'BAD_KIND' }, 400);
}

// -------------------------- Endpoint: dang ky / huy Push subscription --------------------------
async function handleSubscribe(request, env) {
  const body = await readJson(request);
  const deviceId = String(body?.device_id || '').trim();
  const pairingToken = body?.pairing_token ? String(body.pairing_token) : '';
  const sub = body?.subscription;

  if (!isValidDeviceId(deviceId) || !sub?.endpoint || !sub?.keys?.p256dh || !sub?.keys?.auth) {
    return json(env, { success: false, error: 'thieu device_id hoac subscription khong hop le' }, 400);
  }

  const device = await getDeviceByDeviceId(env.DB, deviceId);
  if (!device) {
    return json(env, { success: false, error: 'device chua dang ky - hay bat may va cho ket noi mang truoc' }, 404);
  }
  // Chi trinh duyet da xac thuc PIN moi duoc lien ket Push.
  if (!pairingToken || !device.pairing_token || pairingToken !== device.pairing_token) {
    return json(env, { success: false, error: 'Cần xác thực lại PIN thiết bị' }, 401);
  }

  await upsertSubscription(env.DB, {
    deviceId,
    endpoint: sub.endpoint,
    p256dh: sub.keys.p256dh,
    auth: sub.keys.auth,
    userAgent: request.headers.get('User-Agent') || '',
    now: Date.now(),
  });

  return json(env, { success: true });
}

async function handleUnsubscribe(request, env) {
  const body = await readJson(request);
  const endpoint = String(body?.endpoint || '');
  if (!endpoint) return json(env, { success: false, error: 'thieu endpoint' }, 400);
  await deleteSubscriptionByEndpoint(env.DB, endpoint);
  return json(env, { success: true });
}

// -------------------------- Endpoint: gui thu 1 thong bao test --------------------------
async function handleTestPush(request, env) {
  const body = await readJson(request);
  const endpoint = String(body?.endpoint || '');
  if (!endpoint) return json(env, { success: false, error: 'thieu endpoint' }, 400);

  const sub = await getSubscriptionByEndpoint(env.DB, endpoint);
  if (!sub) return json(env, { success: false, error: 'chua dang ky thong bao tren trinh duyet nay' }, 404);
  const device = await getDeviceByDeviceId(env.DB, sub.device_id);
  const deviceLabel = device?.device_name || sub.device_id;

  const notification = {
    title: `🔔 Test thành công - ${deviceLabel}`,
    body: 'Thiết bị này đã kết nối thông báo thành công.',
    icon: './icons/icon-192.png',
    badge: './icons/badge-72.png',
    data: {
      deviceId: sub.device_id, alarmType: 'TEST', severity: 'info', state: 'active',
      url: `./?device=${encodeURIComponent(sub.device_id || '')}`, ts: Date.now(),
    },
  };
  const result = await sendWebPush(env, sub, notification);
  if (!result.ok && result.gone) await deleteSubscriptionByEndpoint(env.DB, endpoint);
  // Tra ve them status/error khi that bai - de xem duoc ly do that qua tab
  // Network cua trinh duyet ma khong bat buoc phai chay wrangler tail.
  return json(env, {
    success: result.ok,
    notification_sent: result.ok ? 1 : 0,
    ...(result.ok ? {} : { push_status: result.status, push_error: result.error || '' }),
  });
}

// PIN rieng cua nguoi dung (KHAC device_key cua firmware) - gate cho "them
// thiet bi" va "doi ten may" tren web, tranh nguoi la biet device_id la them/
// sua duoc thiet bi cua nguoi khac. NULL = chua tung doi, coi nhu dang la
// PIN ngau nhien do Worker cap va HMI hien thi.
async function verifyDevicePin(env, device, pin) {
  const value = String(pin || '');
  if (!device.web_pin_hash) return false;
  return verifyDeviceKey(value, env.DEVICE_KEY_PEPPER, device.web_pin_hash);
}

function randomWebPin() {
  const bytes = new Uint32Array(1);
  crypto.getRandomValues(bytes);
  return String(100000 + (bytes[0] % 900000));
}

async function issueWebPin(env, deviceId) {
  const pin = randomWebPin();
  const pinHash = await hashDeviceKey(pin, env.DEVICE_KEY_PEPPER);
  await setDevicePinHash(env.DB, deviceId, pinHash);
  return pin;
}

function isValidPin(pin) {
  return typeof pin === 'string' && /^[0-9]{4,8}$/.test(pin);
}

// -------------------------- Endpoint: doi ten may --------------------------
async function handleRenameDevice(request, env) {
  const body = await readJson(request);
  const deviceId = String(body?.device_id || '').trim();
  const pin = String(body?.pin || '');
  const name = String(body?.name || '').trim().slice(0, 64);
  if (!isValidDeviceId(deviceId) || !name) {
    return json(env, { success: false, error: 'thieu device_id/pin/name hop le' }, 400);
  }

  const device = await getDeviceByDeviceId(env.DB, deviceId);
  if (!device) return json(env, { success: false, error: 'device chua dang ky' }, 404);
  const check = await verifyPinGuarded(request, env, device, pin);
  if (check.limited) return json(env, { success: false, error: 'Thử sai quá nhiều lần - vui lòng chờ 15 phút' }, 429);
  if (!check.valid) return json(env, { success: false, error: 'Sai mã PIN của thiết bị' }, 401);

  await renameDevice(env.DB, deviceId, name);
  return json(env, { success: true, device_name: name });
}

// -------------------------- Endpoint: doi PIN --------------------------
async function handleChangePin(request, env) {
  const body = await readJson(request);
  const deviceId = String(body?.device_id || '').trim();
  const oldPin = String(body?.old_pin || '');
  const newPin = String(body?.new_pin || '');
  if (!isValidDeviceId(deviceId)) return json(env, { success: false, error: 'device_id khong hop le' }, 400);
  if (!isValidPin(newPin)) {
    return json(env, { success: false, error: 'PIN mới phải là số, từ 4 đến 8 chữ số' }, 400);
  }

  const device = await getDeviceByDeviceId(env.DB, deviceId);
  if (!device) return json(env, { success: false, error: 'device chua dang ky' }, 404);
  const check = await verifyPinGuarded(request, env, device, oldPin);
  if (check.limited) return json(env, { success: false, error: 'Thử sai quá nhiều lần - vui lòng chờ 15 phút' }, 429);
  if (!check.valid) return json(env, { success: false, error: 'Sai mã PIN hiện tại' }, 401);

  const newHash = await hashDeviceKey(newPin, env.DEVICE_KEY_PEPPER);
  await setDevicePinHash(env.DB, deviceId, newHash);
  const refreshed = await getDeviceByDeviceId(env.DB, deviceId);
  return json(env, { success: true, pairing_token: refreshed?.pairing_token || '' });
}

// ==================== Cap nhat firmware tu xa (xem ota_web_update.h) ====================
// Nguon duy nhat: GitHub Releases cua chinh repo nay (xem .github/workflows/
// build-firmware.yml - tu dong bien dich + tao Release moi khi day tag
// "vX.Y.Z"). Khong con trang admin/token upload thu cong - Worker tu hoi
// GitHub API, TU BAM LAI SHA-256 that su cua file .bin (khong tin bat ky
// checksum co san nao tu GitHub) va cache ket qua trong D1 de khong phai
// tai lai file .bin moi lan co thiet bi/trinh duyet hoi.

function isValidFirmwareVersion(version) {
  return typeof version === 'string' && /^\d{1,4}\.\d{1,4}\.\d{1,4}$/.test(version);
}

function toHexDigest(buffer) {
  return [...new Uint8Array(buffer)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

// So sanh 2 phien ban dang "X.Y.Z" - duong neu 'a' MOI HON 'b' (giong ham
// cung ten phia firmware, xem mayapFirmwareVersionNewer trong ota_web_update.h).
function isFirmwareVersionNewer(a, b) {
  const pa = String(a).split('.').map((n) => parseInt(n, 10) || 0);
  const pb = String(b).split('.').map((n) => parseInt(n, 10) || 0);
  for (let i = 0; i < 3; i += 1) {
    const diff = (pa[i] || 0) - (pb[i] || 0);
    if (diff !== 0) return diff > 0;
  }
  return false;
}

async function fetchGithubJson(env, path) {
  const headers = {
    'User-Agent': 'mayap-push-worker',
    Accept: 'application/vnd.github+json',
  };
  if (env.GITHUB_TOKEN) headers.Authorization = `Bearer ${env.GITHUB_TOKEN}`;
  const res = await fetch(`https://api.github.com${path}`, { headers });
  if (!res.ok) return null;
  return res.json();
}

// Tai THAT SU file .bin tu GitHub roi tu bam SHA-256 - buoc TON KEM nhat nen
// chi goi khi biet chac tag GitHub da doi so voi cache (xem getFirmwareCache).
async function refreshFirmwareCache(env, release) {
  const version = String(release.tag_name || '').replace(/^v/, '');
  if (!isValidFirmwareVersion(version)) return null;
  const asset = (release.assets || []).find((a) => a.name && a.name.endsWith('.bin'));
  const signatureAsset = (release.assets || []).find((a) => a.name && a.name.endsWith('.sig'));
  if (!asset || !asset.browser_download_url || !signatureAsset?.browser_download_url) return null;

  const assetRes = await fetch(asset.browser_download_url, {
    headers: { 'User-Agent': 'mayap-push-worker' },
  });
  if (!assetRes.ok) return null;
  const buffer = await assetRes.arrayBuffer();
  const digest = await crypto.subtle.digest('SHA-256', buffer);
  const signatureRes = await fetch(signatureAsset.browser_download_url, {
    headers: { 'User-Agent': 'mayap-push-worker' },
  });
  if (!signatureRes.ok) return null;
  const signature = (await signatureRes.text()).trim();
  if (!/^[A-Za-z0-9+/]{40,}={0,2}$/.test(signature)) return null;

  const cache = {
    version,
    assetUrl: asset.browser_download_url,
    sha256: toHexDigest(digest),
    signature,
    size: buffer.byteLength,
    notes: String(release.body || '').slice(0, 2000),
    fetchedAt: Date.now(),
  };
  await setFirmwareCache(env.DB, cache);
  return cache;
}

// Cache 1 dong duy nhat trong D1 (firmware_cache). Chi hoi GitHub API nhe
// (khong tai file) de biet tag co doi khong; CHI tai+bam lai file .bin khi
// tag THAT SU khac cache hien co - tranh ton bang thong/CPU cho moi lan
// thiet bi/trinh duyet hoi ma khong co gi moi.
async function getFirmwareCache(env) {
  const cached = await getCachedFirmware(env.DB);
  const stale = !cached || (Date.now() - cached.fetched_at) > FIRMWARE_CACHE_MAX_AGE_MS;
  if (!stale) return cached;

  const release = await fetchGithubJson(env, `/repos/${GITHUB_OWNER}/${GITHUB_REPO}/releases/latest`);
  if (!release || !release.tag_name) return cached;  // GitHub loi tam thoi - giu cache cu neu co

  const latestVersion = String(release.tag_name).replace(/^v/, '');
  if (cached && cached.version === latestVersion) {
    await touchFirmwareCache(env.DB, Date.now());
    return cached;
  }
  const fresh = await refreshFirmwareCache(env, release);
  return fresh || cached;
}

// -------------------------- Endpoint (web, cong khai): phien ban moi nhat --------------------------
// Khong can xac thuc device_key - chi la thong tin CONG KHAI "ban moi nhat
// hien co la gi", dung de web quyet dinh hien nut "Cap nhat" hay thong tin
// thiet bi (so sanh voi phien ban dang chay lay tu MQTT, xem app.js).
async function handleFirmwareLatestPublic(env) {
  const cache = await getFirmwareCache(env);
  if (!cache) return json(env, { success: true, version: null });
  return json(env, { success: true, version: cache.version, notes: cache.notes });
}

// -------------------------- Endpoint (thiet bi): kiem tra ban moi --------------------------
async function handleFirmwareCheck(request, env) {
  const body = await readJson(request);
  const deviceId = String(body?.device_id || '').trim();
  const deviceKey = String(body?.device_key || '');
  const currentVersion = String(body?.current_version || '');
  if (!isValidDeviceId(deviceId)) return json(env, { success: false, error: 'device_id khong hop le' }, 400);

  const device = await getDeviceByDeviceId(env.DB, deviceId);
  if (!device) return json(env, { success: false, error: 'device chua dang ky' }, 404);
  const valid = await verifyDeviceKey(deviceKey, env.DEVICE_KEY_PEPPER, device.device_key_hash);
  if (!valid) return json(env, { success: false, error: 'device_key sai' }, 401);

  const latest = await getFirmwareCache(env);
  if (!latest || !isFirmwareVersionNewer(latest.version, currentVersion)) {
    return json(env, { success: true, update_available: false });
  }
  return json(env, {
    success: true,
    update_available: true,
    version: latest.version,
    sha256: latest.sha256,
    signature: latest.signature,
    size: latest.size,
    notes: latest.notes,
  });
}

// -------------------------- Endpoint (thiet bi): tai file firmware --------------------------
// Xac thuc qua HEADER (khong phai query string) de device_key khong lo qua
// access log/URL history - X-Device-Id/X-Device-Key, xem ota_web_update.h.
// Worker dong vai tro PROXY: thiet bi khong bao gio tu ket noi thang toi
// GitHub, tat ca van di qua kenh HTTPS quen thuoc toi CLOUD_API_HOST.
async function handleFirmwareDownload(env, version, request) {
  const deviceId = String(request.headers.get('X-Device-Id') || '').trim();
  const deviceKey = String(request.headers.get('X-Device-Key') || '');
  if (!isValidDeviceId(deviceId)) return json(env, { success: false, error: 'device_id khong hop le' }, 400);

  const device = await getDeviceByDeviceId(env.DB, deviceId);
  if (!device) return json(env, { success: false, error: 'device chua dang ky' }, 404);
  const valid = await verifyDeviceKey(deviceKey, env.DEVICE_KEY_PEPPER, device.device_key_hash);
  if (!valid) return json(env, { success: false, error: 'device_key sai' }, 401);

  const cache = await getFirmwareCache(env);
  if (!cache || cache.version !== version) {
    return json(env, { success: false, error: 'phien ban khong ton tai hoac khong con la ban moi nhat' }, 404);
  }
  const assetRes = await fetch(cache.asset_url, { headers: { 'User-Agent': 'mayap-push-worker' } });
  if (!assetRes.ok || !assetRes.body) {
    return json(env, { success: false, error: 'khong tai duoc file tu GitHub' }, 502);
  }
  return new Response(assetRes.body, {
    status: 200,
    headers: {
      'Content-Type': 'application/octet-stream',
      'Content-Length': String(cache.size),
      'X-Firmware-Sha256': cache.sha256,
    },
  });
}

// -------------------------- Endpoint: trang thai lien ket cua 1 thiet bi --------------------------
async function handleDeviceStatus(env, deviceId) {
  const device = await getDeviceByDeviceId(env.DB, deviceId);
  if (!device) return json(env, { success: true, exists: false });
  const subs = await getSubscriptionsForDevice(env.DB, deviceId);
  return json(env, {
    success: true,
    exists: true,
    device_id: device.device_id,
    device_name: device.device_name,
    status: device.status,
    last_seen: device.last_seen,
    linked_browsers: subs.length,
  });
}

// -------------------------- Cron: phat hien thiet bi mat ket noi --------------------------
// Chay DOC LAP voi ESP32 (xem wrangler.toml [triggers]) - vi khi mat dien
// chinh may ap cung chet theo nen KHONG THE tu goi API bao "toi vua mat
// dien". Worker phai tu phat hien qua khoang lang cua last_seen.
async function sendDeviceLifecycleAlarm(env, device, { state, message }) {
  const now = Date.now();
  const subscriptions = await getSubscriptionsForDevice(env.DB, device.device_id);
  const notification = buildNotificationPayload({
    deviceId: device.device_id,
    deviceName: device.device_name,
    alarmType: 'DEVICE_OFFLINE',
    severity: 'critical',
    state,
    message,
  });

  const prior=await getAlarmState(env.DB,device.device_id,'DEVICE_OFFLINE');
  if (prior && Boolean(prior.active)===(state==='active')) return;
  const eventId=`lifecycle-${state}-${device.last_seen || 0}`;
  notification.data.eventId=eventId;notification.data.message=message;
  await queueAlarmEvent(env,{deviceId:device.device_id,eventId,alarmType:'DEVICE_OFFLINE',notification,now});
  await upsertAlarmState(env.DB,{deviceId:device.device_id,alarmType:'DEVICE_OFFLINE',active:state==='active',firstSentAt:now,lastSentAt:now,lastMessage:message});
}

async function checkDeviceConnectivity(env) {
  const staleBefore = Date.now() - DEVICE_OFFLINE_THRESHOLD_MS;

  const staleDevices = await getStaleOnlineDevices(env.DB, staleBefore);
  for (const device of staleDevices) {
    // Mark offline only after its alarm was persisted successfully.
    // Chi gui push khi device dang co me ap chay tai lan heartbeat GAN NHAT
    // (batch_running ghi kem moi heartbeat - xem touchDeviceHeartbeat trong
    // db.js) - khong co me nao dang chay thi mat mang/mat dien khong can bao,
    // theo yeu cau: chi quan tam khi dang ap that su.
    if (!device.batch_running) {await setDeviceStatus(env.DB,device.device_id,'offline');continue;}
    const thresholdSeconds = Math.round(DEVICE_OFFLINE_THRESHOLD_MS / 1000);
    await sendDeviceLifecycleAlarm(env, device, {
      state: 'active',
      message: `Mất kết nối trên ${thresholdSeconds} giây - kiểm tra nguồn điện hoặc Wi-Fi ngay.`,
    });
    await setDeviceStatus(env.DB,device.device_id,'offline');
  }

  const recoveredDevices = await getRecoveredOfflineDevices(env.DB, staleBefore);
  for (const device of recoveredDevices) {
    // Noi ro may da song lai, khong bao chung chung. Chi tiet "me ap co tu
    // chay tiep hay dang cho xac nhan" do chinh ESP32 gui rieng ngay khi no
    // khoi dong lai (POWER_RESTORED trong cloud_alert_link.h) - o day Worker
    // chi biet den muc "da thay heartbeat tro lai".
    await sendDeviceLifecycleAlarm(env, device, {
      state: 'resolved',
      message: 'Đã hết: máy đã có điện/mạng trở lại.',
    });
  }
}

export default {
  async scheduled(event, env, ctx) {
    ctx.waitUntil((async()=>{await checkDeviceConnectivity(env);await maintainAlarmDeliveries(env);})());
  },
  async fetch(request, env, ctx) {
    const url = new URL(request.url);

    if (request.method === 'OPTIONS') {
      return new Response(null, { status: 204, headers: corsHeaders(env) });
    }

    try {
      if (url.pathname === '/api/push/vapid-public-key' && request.method === 'GET') {
        return json(env, { publicKey: env.VAPID_PUBLIC_KEY });
      }
      if (url.pathname === '/api/device/register' && request.method === 'POST') {
        return await handleRegister(request, env);
      }
      if (url.pathname === '/api/device/heartbeat' && request.method === 'POST') {
        return await handleHeartbeat(request, env);
      }
      if (url.pathname === '/api/device/rotate-key' && request.method === 'POST') {
        return await handleRotateDeviceKey(request, env);
      }
      if (url.pathname === '/api/device/reset-pin' && request.method === 'POST') {
        return await handleResetPin(request, env);
      }
      if (url.pathname === '/api/device/alarm' && request.method === 'POST') {
        return await handleAlarm(request, env, ctx);
      }
      if (url.pathname === '/api/device/alarms' && request.method === 'POST') {
        return await handleAlarmBatch(request, env, ctx);
      }
      if (url.pathname === '/api/internal/uplink' && request.method === 'POST') {
        return await handleUplink(request, env, ctx);
      }
      if (['/api/device/verify-pin','/api/device/mqtt-session','/api/device/sign-mqtt','/api/device/session-check'].includes(url.pathname)) {
        return json(env, { success:false, error:'ACCOUNT_UPGRADE_REQUIRED' }, 410);
      }
      if (url.pathname === '/api/device/rename' && request.method === 'POST') {
        return await handleRenameDevice(request, env);
      }
      if (url.pathname === '/api/device/change-pin' && request.method === 'POST') {
        return await handleChangePin(request, env);
      }
      if (url.pathname === '/api/push/subscribe' && request.method === 'POST') {
        return await handleSubscribe(request, env);
      }
      if (url.pathname === '/api/push/subscribe' && request.method === 'DELETE') {
        return await handleUnsubscribe(request, env);
      }
      if (url.pathname === '/api/push/test' && request.method === 'POST') {
        return await handleTestPush(request, env);
      }
      const statusMatch = url.pathname.match(/^\/api\/device\/([A-Za-z0-9_-]{3,40})\/status$/);
      if (statusMatch && request.method === 'GET') {
        return await handleDeviceStatus(env, statusMatch[1]);
      }

      // ---- Cap nhat firmware tu xa (nguon: GitHub Releases, xem ota_web_update.h) ----
      if (url.pathname === '/api/firmware/latest' && request.method === 'GET') {
        return await handleFirmwareLatestPublic(env);
      }
      if (url.pathname === '/api/firmware/check' && request.method === 'POST') {
        return await handleFirmwareCheck(request, env);
      }
      const firmwareDownloadMatch = url.pathname.match(/^\/api\/firmware\/download\/(\d{1,4}\.\d{1,4}\.\d{1,4})$/);
      if (firmwareDownloadMatch && request.method === 'GET') {
        return await handleFirmwareDownload(env, firmwareDownloadMatch[1], request);
      }

      return json(env, { success: false, error: 'not found' }, 404);
    } catch (error) {
      return json(env, { success: false, error: 'internal error', detail: String(error && error.message ? error.message : error) }, 500);
    }
  },
};
