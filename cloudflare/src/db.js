// Tat ca truy van D1 tap trung o day - de bao tri, khong rai SQL khap noi.

export async function getDeviceByDeviceId(db, deviceId) {
  return db
    .prepare('SELECT * FROM devices WHERE device_id = ?1')
    .bind(deviceId)
    .first();
}

export async function insertDevice(db, { deviceId, deviceName, deviceKeyHash, pairingToken, now }) {
  await db
    .prepare(
      `INSERT INTO devices (device_id, device_name, device_key_hash, pairing_token, created_at, last_seen, status)
       VALUES (?1, ?2, ?3, ?4, ?5, ?5, 'online')`
    )
    .bind(deviceId, deviceName || '', deviceKeyHash, pairingToken, now)
    .run();
}

export async function touchDevice(db, deviceId, status, now, deviceName) {
  if (deviceName) {
    await db
      .prepare('UPDATE devices SET last_seen = ?2, status = ?3, device_name = ?4 WHERE device_id = ?1')
      .bind(deviceId, now, status, deviceName)
      .run();
  } else {
    await db
      .prepare('UPDATE devices SET last_seen = ?2, status = ?3 WHERE device_id = ?1')
      .bind(deviceId, now, status)
      .run();
  }
}

// Rieng cho heartbeat: ghi them batch_running (0/1) cung luc voi last_seen -
// day la nguon "dang co me ap chay khong" moi nhat de checkDeviceConnectivity
// quyet dinh co bao mat ket noi hay khong (chi bao khi dang co me ap chay).
export async function touchDeviceHeartbeat(db, deviceId, now, batchRunning) {
  await db
    .prepare('UPDATE devices SET last_seen = ?2, status = ?3, batch_running = ?4 WHERE device_id = ?1')
    .bind(deviceId, now, 'online', batchRunning ? 1 : 0)
    .run();
}

// Doi ten hien thi (hoan toan do web quan ly - xem ghi chu trong handleRegister
// o index.js ve ly do KHONG con de firmware ghi de truong nay).
export async function renameDevice(db, deviceId, name) {
  await db.prepare('UPDATE devices SET device_name = ?2 WHERE device_id = ?1').bind(deviceId, name).run();
}

function randomPairingTokenHex() {
  const bytes = new Uint8Array(12);  // 96 bit ngau nhien, khop token ghep noi hien tai.
  crypto.getRandomValues(bytes);
  return Array.from(bytes, (value) => value.toString(16).padStart(2, '0')).join('');
}

// Luu hash PIN web moi. Neu thiet bi DA co PIN truoc do thi dong thoi xoay
// pairing_token de moi trinh duyet/ung dung giu token cu mat quyen xin phien
// MQTT moi sau khi chu may doi/reset PIN. Lan cap PIN dau tien (web_pin_hash
// dang NULL) giu nguyen token vua tao boi handleRegister(), tranh tra ve mot
// token da bi thay doi ngay trong cung giao dich dang ky.
export async function setDevicePinHash(db, deviceId, pinHash) {
  const nextPairingToken = randomPairingTokenHex();
  await db.prepare(
    `UPDATE devices
     SET web_pin_hash = ?2,
         pairing_token = CASE
           WHEN web_pin_hash IS NULL THEN pairing_token
           ELSE ?3
         END
     WHERE device_id = ?1`
  ).bind(deviceId, pinHash, nextPairingToken).run();
}

export async function setDeviceKeyHash(db, deviceId, deviceKeyHash) {
  await db.prepare('UPDATE devices SET device_key_hash = ?2 WHERE device_id = ?1')
    .bind(deviceId, deviceKeyHash)
    .run();
}

// Doi status ma KHONG dung toi last_seen (touchDevice() dung khi that su co
// tin moi tu thiet bi; ham nay dung khi Worker tu suy ra trang thai, vi du
// tu danh dau 'offline' luc phat hien im lang - khong duoc lam moi last_seen).
export async function setDeviceStatus(db, deviceId, status) {
  await db.prepare('UPDATE devices SET status = ?2 WHERE device_id = ?1').bind(deviceId, status).run();
}

export async function getSubscriptionsForDevice(db, deviceId) {
  const { results } = await db
    .prepare(`SELECT ps.* FROM push_subscriptions ps
      JOIN users u ON u.google_sub=ps.user_sub AND u.disabled=0
      JOIN user_devices ud ON ud.user_sub=ps.user_sub AND ud.device_id=ps.device_id
      WHERE ps.device_id = ?1`)
    .bind(deviceId)
    .all();
  return results || [];
}

export async function getSubscriptionByEndpoint(db, endpoint) {
  return db
    .prepare('SELECT * FROM push_subscriptions WHERE endpoint = ?1')
    .bind(endpoint)
    .first();
}

export async function upsertSubscription(db, { deviceId, endpoint, p256dh, auth, userAgent, now }) {
  await db
    .prepare(
      `INSERT INTO push_subscriptions (device_id, endpoint, p256dh, auth, user_agent, created_at, updated_at)
       VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?6)
       ON CONFLICT(device_id, endpoint) DO UPDATE SET
         p256dh = excluded.p256dh,
         auth = excluded.auth,
         user_agent = excluded.user_agent,
         updated_at = excluded.updated_at`
    )
    .bind(deviceId, endpoint, p256dh, auth, userAgent || '', now)
    .run();
}

export async function deleteSubscriptionByEndpoint(db, endpoint) {
  await db.prepare('DELETE FROM push_subscriptions WHERE endpoint = ?1').bind(endpoint).run();
}

// Thiet bi dang danh dau 'online' nhung im lang qua lau (het heartbeat) -
// ung vien "vua mat ket noi" (mat dien/mat mang), chua duoc bao.
export async function getStaleOnlineDevices(db, staleBefore) {
  const { results } = await db
    .prepare("SELECT * FROM devices WHERE status = 'online' AND last_seen IS NOT NULL AND last_seen < ?1")
    .bind(staleBefore)
    .all();
  return results || [];
}

// Thiet bi dang co canh bao DEVICE_OFFLINE con active nhung da online tro
// lai (heartbeat gan day) - can gui tin "da ket noi lai".
export async function getRecoveredOfflineDevices(db, staleBefore) {
  const { results } = await db
    .prepare(
      `SELECT d.* FROM devices d
       JOIN alarm_state a ON a.device_id = d.device_id AND a.alarm_type = 'DEVICE_OFFLINE'
       WHERE a.active = 1 AND d.last_seen IS NOT NULL AND d.last_seen >= ?1`
    )
    .bind(staleBefore)
    .all();
  return results || [];
}

export async function getAlarmState(db, deviceId, alarmType) {
  return db
    .prepare('SELECT * FROM alarm_state WHERE device_id = ?1 AND alarm_type = ?2')
    .bind(deviceId, alarmType)
    .first();
}

export async function upsertAlarmState(db, { deviceId, alarmType, active, firstSentAt, lastSentAt, lastMessage, resolveDue = 0, heldSeq = 0 }) {
  await db
    .prepare(
      `INSERT INTO alarm_state (device_id, alarm_type, active, first_sent_at, last_sent_at, last_message, resolve_due, held_seq)
       VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
       ON CONFLICT(device_id, alarm_type) DO UPDATE SET
         active = excluded.active,
         first_sent_at = COALESCE(alarm_state.first_sent_at, excluded.first_sent_at),
         last_sent_at = excluded.last_sent_at,
         last_message = excluded.last_message,
         resolve_due = excluded.resolve_due,
         held_seq = excluded.held_seq`
    )
    .bind(deviceId, alarmType, active ? 1 : 0, firstSentAt, lastSentAt, lastMessage || '', resolveDue, heldSeq)
    .run();
}

export async function insertAlarmLog(db, entry) {
  await db
    .prepare(
      `INSERT INTO alarm_log
        (device_id, alarm_type, severity, state, message, temperature, humidity, notification_sent, created_at)
       VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)`
    )
    .bind(
      entry.deviceId,
      entry.alarmType,
      entry.severity,
      entry.state,
      entry.message,
      entry.temperature ?? null,
      entry.humidity ?? null,
      entry.notificationSent ? 1 : 0,
      entry.now
    )
    .run();
}

// -------------------------- Cap nhat firmware tu xa (xem index.js) --------------------------
// Cache 1 DONG DUY NHAT (id=1) cua ban GitHub Release moi nhat da xac minh
// (tai that su + tu bam SHA-256) - xem getFirmwareCache() trong index.js.

export async function getCachedFirmware(db) {
  return db.prepare('SELECT * FROM firmware_cache WHERE id = 1').first();
}

export async function setFirmwareCache(db, { version, assetUrl, sha256, signature, size, notes, fetchedAt }) {
  await db
    .prepare(
      `INSERT INTO firmware_cache (id, version, asset_url, sha256, signature, size, notes, fetched_at)
       VALUES (1, ?1, ?2, ?3, ?4, ?5, ?6, ?7)
       ON CONFLICT(id) DO UPDATE SET
         version = excluded.version,
         asset_url = excluded.asset_url,
         sha256 = excluded.sha256,
         signature = excluded.signature,
         size = excluded.size,
         notes = excluded.notes,
         fetched_at = excluded.fetched_at`
    )
    .bind(version, assetUrl, sha256, signature, size, notes || '', fetchedAt)
    .run();
}

export async function touchFirmwareCache(db, fetchedAt) {
  await db.prepare('UPDATE firmware_cache SET fetched_at = ?1 WHERE id = 1').bind(fetchedAt).run();
}
