// MAYAP V4 broker-lite - Durable Object MQTT 3.1.1 broker per
// doc/MQTT_CONTRACT.md. Single DO instance per deviceId.
//
// Hibernation: all connection metadata lives in ws.serializeAttachment(). All
// retained payloads live in DO Storage. Nothing critical is kept only in RAM.
//
// The MQTT codec is pure (mqtt-codec.js); the ACL is pure (acl.js). This file
// orchestrates: WebSocket upgrade, per-connection parser state, authentication,
// retained+LWT, QoS1 fanout, bounded inflight ring.

import {
  StreamingDecoder, MqttDecodeError, PacketType, ConnackCode,
  encodeConnack, encodeSuback, encodePuback, encodePingresp, encodePublish,
  topicMatches, MQTT_MAX_PACKET,
} from './mqtt-codec.js';
import {
  DEVICE_ID_RE, TOPIC_ROOT, Topics,
  RETAIN_FORBIDDEN, RETAIN_ALLOWED,
  parseTopic, canPublish, evaluateSubscribe, topicQosCap,
  makeFixtureCredentials,
} from './acl.js';

const KEEPALIVE_MIN_SEC = 10;
const KEEPALIVE_MAX_SEC = 300;
const KEEPALIVE_GRACE = 1.5;
const ALARM_INTERVAL_MS = 15 * 1000;
const INFLIGHT_LIMIT = 16;        // bounded QoS1 server→client ring
const RETAINED_LIMIT = 32;        // bounded retained messages per DO

const b64enc = (bytes) => {
  // atob/btoa in Workers handle binary strings.
  let s = '';
  for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
  return btoa(s);
};
const b64dec = (s) => {
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
};

export class MqttBrokerDO {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.storage = state.storage;
    // deviceId is set on first upgrade; also persisted in storage.
    this.deviceId = null;
    this.decoders = new WeakMap(); // ws -> StreamingDecoder (RAM; rebuilt on wake)
    // Credential resolver: Phase 2B fixture via env; Phase 2E will call Worker.
    this.resolveCredentials = makeFixtureCredentials({
      devicePassword: env && env.BROKER_FIXTURE_DEVICE_PASSWORD,
      webPassword: env && env.BROKER_FIXTURE_WEB_PASSWORD,
    });
    // Next packet id for server→client QoS1. 1..65535.
    this._packetSeq = 1;
    // Alarm pump — ensure the alarm is scheduled if we have any sockets.
    state.blockConcurrencyWhile(async () => {
      this.deviceId = await this.storage.get('deviceId');
      const existing = typeof state.getWebSockets === 'function'
        ? state.getWebSockets() : [];
      if (existing && existing.length > 0) {
        await this._scheduleAlarm();
      }
    });
  }

  async fetch(request) {
    const url = new URL(request.url);
    const match = url.pathname.match(/^\/mqtt\/([A-Za-z0-9_-]{3,40})$/);
    if (!match) return new Response('bad path', { status: 404 });
    const deviceId = match[1];
    if (!DEVICE_ID_RE.test(deviceId)) {
      return new Response('bad device id', { status: 400 });
    }
    if (this.deviceId && this.deviceId !== deviceId) {
      return new Response('device mismatch', { status: 409 });
    }
    if (!this.deviceId) {
      this.deviceId = deviceId;
      await this.storage.put('deviceId', deviceId);
    }
    if (request.headers.get('Upgrade') !== 'websocket') {
      return new Response('expected websocket', { status: 426 });
    }
    const subprotoHeader = request.headers.get('Sec-WebSocket-Protocol') || '';
    const offered = subprotoHeader.split(',').map((s) => s.trim()).filter(Boolean);
    if (offered.length > 0 && !offered.includes('mqtt')) {
      return new Response('subprotocol required', { status: 400 });
    }

    const pair = new WebSocketPair();
    const client = pair[0];
    const server = pair[1];
    this.state.acceptWebSocket(server);
    // Initial attachment — anonymous, awaiting CONNECT.
    server.serializeAttachment({
      state: 'await-connect',
      deviceId,
      createdAt: Date.now(),
      bufferB64: '',
    });
    await this._scheduleAlarm();
    const responseInit = { status: 101, webSocket: client };
    if (offered.includes('mqtt')) {
      responseInit.headers = { 'Sec-WebSocket-Protocol': 'mqtt' };
    }
    return new Response(null, responseInit);
  }

  // ------------- Hibernation callbacks ------------------------------------

  async webSocketMessage(ws, message) {
    let decoder = this.decoders.get(ws);
    const att = ws.deserializeAttachment() || {};
    if (!decoder) {
      decoder = new StreamingDecoder({ max: MQTT_MAX_PACKET });
      if (att.bufferB64) {
        try { decoder.push(b64dec(att.bufferB64)); } catch { /* overflow handled below */ }
      }
      this.decoders.set(ws, decoder);
    }
    let bytes;
    if (typeof message === 'string') {
      // Spec: text frames are protocol error for binary MQTT.
      return this._closeFatal(ws, 1003, 'TEXT_FRAME');
    }
    if (message instanceof ArrayBuffer) bytes = new Uint8Array(message);
    else if (message instanceof Uint8Array) bytes = message;
    else if (message && message.buffer) bytes = new Uint8Array(message.buffer);
    else return this._closeFatal(ws, 1003, 'BAD_FRAME');

    try {
      decoder.push(bytes);
    } catch (err) {
      return this._closeFatal(ws, 1009, err.code || 'OVERFLOW');
    }
    let packets;
    try {
      packets = decoder.drain();
    } catch (err) {
      return this._closeFatal(ws, 1002, err.code || 'MALFORMED');
    }
    att.lastRxMs = Date.now();
    for (const pkt of packets) {
      try {
        await this._handlePacket(ws, att, pkt);
      } catch (err) {
        return this._closeFatal(ws, 1011, err && err.code || 'HANDLER');
      }
    }
    // Persist decoder buffer across hibernation.
    att.bufferB64 = b64enc(decoder.buffer);
    try { ws.serializeAttachment(att); } catch { /* ws may already be gone */ }
  }

  async webSocketClose(ws, code, reason, wasClean) {
    const att = ws.deserializeAttachment() || {};
    this.decoders.delete(ws);
    if (att.state === 'open' && att.lwt && !att.disconnected) {
      await this._fanoutPublish({
        topic: att.lwt.topic,
        qos: att.lwt.qos,
        retain: att.lwt.retain,
        payload: att.lwt.payloadB64 ? b64dec(att.lwt.payloadB64) : new Uint8Array(0),
        originWs: ws,
      });
    }
    // No-op; attachment is dropped with the ws.
  }

  async webSocketError(ws, err) {
    try { await this.webSocketClose(ws, 1011, String(err && err.message || err), false); } catch {}
  }

  async alarm() {
    const now = Date.now();
    const sockets = typeof this.state.getWebSockets === 'function'
      ? this.state.getWebSockets() : [];
    for (const ws of sockets) {
      const att = ws.deserializeAttachment() || {};
      if (att.state === 'open' && att.keepaliveMs > 0) {
        const limit = Math.ceil(att.keepaliveMs * KEEPALIVE_GRACE);
        if (att.lastRxMs && now - att.lastRxMs > limit) {
          try { ws.close(1001, 'KEEPALIVE'); } catch {}
          // webSocketClose will publish LWT.
        }
      } else if (att.state === 'await-connect') {
        if (now - att.createdAt > 10_000) {
          try { ws.close(1008, 'CONNECT_TIMEOUT'); } catch {}
        }
      }
    }
    const remaining = typeof this.state.getWebSockets === 'function'
      ? this.state.getWebSockets() : [];
    if (remaining.length > 0) await this._scheduleAlarm();
  }

  // ------------- Packet handling ------------------------------------------

  async _handlePacket(ws, att, pkt) {
    if (att.state === 'await-connect' && pkt.type !== 'CONNECT') {
      return this._closeFatal(ws, 1002, 'EXPECT_CONNECT');
    }
    switch (pkt.type) {
      case 'CONNECT': return this._handleConnect(ws, att, pkt);
      case 'PUBLISH': return this._handlePublish(ws, att, pkt);
      case 'PUBACK': return this._handlePuback(ws, att, pkt);
      case 'SUBSCRIBE': return this._handleSubscribe(ws, att, pkt);
      case 'PINGREQ': return this._handlePingreq(ws, att);
      case 'DISCONNECT': return this._handleDisconnect(ws, att);
      default:
        return this._closeFatal(ws, 1002, 'UNSUPPORTED');
    }
  }

  async _handleConnect(ws, att, pkt) {
    if (att.state !== 'await-connect') {
      return this._closeFatal(ws, 1002, 'DOUBLE_CONNECT');
    }
    if (!pkt.cleanSession) {
      ws.send(encodeConnack(ConnackCode.NOT_AUTHORIZED));
      return this._closeFatal(ws, 1008, 'CLEAN_SESSION_REQUIRED');
    }
    const keepaliveSec = pkt.keepalive;
    if (keepaliveSec !== 0 && (keepaliveSec < KEEPALIVE_MIN_SEC || keepaliveSec > KEEPALIVE_MAX_SEC)) {
      ws.send(encodeConnack(ConnackCode.NOT_AUTHORIZED));
      return this._closeFatal(ws, 1008, 'BAD_KEEPALIVE');
    }
    const resolved = this.resolveCredentials(this.deviceId, pkt.username, pkt.password);
    if (!resolved) {
      ws.send(encodeConnack(ConnackCode.BAD_CREDENTIALS));
      return this._closeFatal(ws, 1008, 'BAD_CREDENTIALS');
    }
    // Validate LWT scope — must be writable by this role, retain rule applies.
    if (pkt.will) {
      if (!canPublish(resolved.role, this.deviceId, pkt.will.topic)) {
        ws.send(encodeConnack(ConnackCode.NOT_AUTHORIZED));
        return this._closeFatal(ws, 1008, 'LWT_ACL');
      }
      const parsed = parseTopic(this.deviceId, pkt.will.topic);
      if (pkt.will.retain && RETAIN_FORBIDDEN.has(parsed.suffix)) {
        ws.send(encodeConnack(ConnackCode.NOT_AUTHORIZED));
        return this._closeFatal(ws, 1008, 'LWT_RETAIN_FORBIDDEN');
      }
    }
    att.state = 'open';
    att.role = resolved.role;
    att.clientId = pkt.clientId || `anon-${Math.random().toString(36).slice(2, 8)}`;
    att.keepaliveMs = keepaliveSec * 1000;
    att.lastRxMs = Date.now();
    att.subs = {}; // suffix -> grantedQos
    att.inflight = []; // [{packetId, deliveredAt}]
    att.violations = 0;
    att.disconnected = false;
    if (pkt.will) {
      att.lwt = {
        topic: pkt.will.topic,
        qos: pkt.will.qos,
        retain: pkt.will.retain,
        payloadB64: b64enc(pkt.will.payload || new Uint8Array(0)),
      };
    }
    ws.serializeAttachment(att);
    ws.send(encodeConnack(ConnackCode.ACCEPTED, false));
  }

  async _handlePublish(ws, att, pkt) {
    if (!canPublish(att.role, this.deviceId, pkt.topic)) {
      att.violations = (att.violations || 0) + 1;
      ws.serializeAttachment(att);
      // Per §5: silently drop + PUBACK (if QoS1), close after second violation.
      if (pkt.qos > 0) ws.send(encodePuback(pkt.packetId));
      if (att.violations >= 2) return this._closeFatal(ws, 1008, 'ACL_PUBLISH');
      return;
    }
    const parsed = parseTopic(this.deviceId, pkt.topic);
    // Retain policy: forbidden list wins over client bit.
    let effectiveRetain = pkt.retain;
    if (RETAIN_FORBIDDEN.has(parsed.suffix)) effectiveRetain = false;
    if (!RETAIN_ALLOWED.has(parsed.suffix)) effectiveRetain = false;

    if (effectiveRetain) {
      if (pkt.payload.length === 0) {
        await this.storage.delete(`retained:${parsed.suffix}`);
      } else {
        // Bounded retained store — refuse new keys past RETAINED_LIMIT.
        const existing = await this.storage.get(`retained:${parsed.suffix}`);
        if (!existing) {
          const list = await this.storage.list({ prefix: 'retained:' });
          if (list.size >= RETAINED_LIMIT) {
            // Over quota — drop retain bit, still fan out live.
            effectiveRetain = false;
          }
        }
        if (effectiveRetain) {
          await this.storage.put(`retained:${parsed.suffix}`, {
            qos: pkt.qos, payloadB64: b64enc(pkt.payload), ts: Date.now(),
          });
        }
      }
    }
    // QoS1 PUBACK to publisher before fanout — this is a transport ACK,
    // never an application ACK (see TRANSACTION_V2_SPEC.md).
    if (pkt.qos > 0) ws.send(encodePuback(pkt.packetId));
    await this._fanoutPublish({
      topic: pkt.topic, qos: pkt.qos, retain: false,
      payload: pkt.payload, originWs: ws,
    });
  }

  async _fanoutPublish({ topic, qos, retain, payload, originWs }) {
    const sockets = typeof this.state.getWebSockets === 'function'
      ? this.state.getWebSockets() : [];
    for (const peer of sockets) {
      if (peer === originWs) continue;
      let att;
      try { att = peer.deserializeAttachment() || {}; } catch { continue; }
      if (att.state !== 'open') continue;
      let matchedQos = -1;
      for (const [suffix, grantedQos] of Object.entries(att.subs || {})) {
        const filter = `${TOPIC_ROOT}/${this.deviceId}/${suffix}`;
        if (topicMatches(filter, topic)) {
          if (grantedQos > matchedQos) matchedQos = grantedQos;
        }
      }
      if (matchedQos < 0) continue;
      // Spec §2: fan out at min(publishQos, grantedQos). Never upgrade.
      const outQos = Math.min(qos, matchedQos);
      let packetId = 0;
      if (outQos > 0) {
        packetId = this._nextPacketId();
        att.inflight = att.inflight || [];
        att.inflight.push({ packetId, deliveredAt: Date.now() });
        if (att.inflight.length > INFLIGHT_LIMIT) {
          // Drop oldest — bounded ring; client will retry higher-layer if needed.
          att.inflight.shift();
        }
        peer.serializeAttachment(att);
      }
      try {
        peer.send(encodePublish({ topic, qos: outQos, retain, packetId, payload }));
      } catch { /* peer closed; ignore */ }
    }
  }

  async _handlePuback(ws, att, pkt) {
    if (!att.inflight) return;
    att.inflight = att.inflight.filter((e) => e.packetId !== pkt.packetId);
    ws.serializeAttachment(att);
  }

  async _handleSubscribe(ws, att, pkt) {
    const codes = [];
    for (const { filter, qos: reqQos } of pkt.filters) {
      const granted = evaluateSubscribe(att.role, this.deviceId, filter, reqQos);
      codes.push(granted);
      if (granted !== 0x80) {
        const parsed = parseTopic(this.deviceId, filter);
        att.subs[parsed.suffix] = granted;
      }
    }
    ws.serializeAttachment(att);
    ws.send(encodeSuback(pkt.packetId, codes));
    // Deliver retained for newly accepted subs.
    for (let i = 0; i < pkt.filters.length; i++) {
      if (codes[i] === 0x80) continue;
      const parsed = parseTopic(this.deviceId, pkt.filters[i].filter);
      if (!RETAIN_ALLOWED.has(parsed.suffix)) continue;
      const key = `retained:${parsed.suffix}`;
      const stored = await this.storage.get(key);
      if (!stored) continue;
      const payload = b64dec(stored.payloadB64);
      const topic = `${TOPIC_ROOT}/${this.deviceId}/${parsed.suffix}`;
      const outQos = Math.min(stored.qos, codes[i]);
      let packetId = 0;
      if (outQos > 0) {
        packetId = this._nextPacketId();
        att.inflight = att.inflight || [];
        att.inflight.push({ packetId, deliveredAt: Date.now() });
        if (att.inflight.length > INFLIGHT_LIMIT) att.inflight.shift();
        ws.serializeAttachment(att);
      }
      try {
        ws.send(encodePublish({ topic, qos: outQos, retain: true, packetId, payload }));
      } catch {}
    }
  }

  _handlePingreq(ws, att) {
    ws.send(encodePingresp());
  }

  async _handleDisconnect(ws, att) {
    att.disconnected = true;
    try { ws.serializeAttachment(att); } catch {}
    try { ws.close(1000, 'DISCONNECT'); } catch {}
  }

  _closeFatal(ws, code, reason) {
    try {
      const att = ws.deserializeAttachment() || {};
      att.disconnected = (reason === 'DISCONNECT');
      ws.serializeAttachment(att);
    } catch {}
    try { ws.close(code, String(reason).slice(0, 120)); } catch {}
  }

  _nextPacketId() {
    const id = this._packetSeq;
    this._packetSeq = (this._packetSeq % 0xffff) + 1;
    return id;
  }

  async _scheduleAlarm() {
    const current = typeof this.storage.getAlarm === 'function'
      ? await this.storage.getAlarm() : null;
    if (current == null) {
      try { await this.storage.setAlarm(Date.now() + ALARM_INTERVAL_MS); } catch {}
    }
  }
}
