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
  parseTopic, canPublish, evaluateSubscribe, topicQosCap, requiredPublishQos,
  makeFixtureCredentials,
} from './acl.js';

// Contract §1 — client keepalive 30..120 s. Zero is explicitly forbidden
// because retained presence must not stale indefinitely on half-open links.
const KEEPALIVE_MIN_SEC = 30;
const KEEPALIVE_MAX_SEC = 120;
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
    // Phase 2B.1 §5 — MQTT subprotocol is MANDATORY. Reject upgrades that
    // omit it or offer only unrelated subprotocols.
    const subprotoHeader = request.headers.get('Sec-WebSocket-Protocol') || '';
    const offered = subprotoHeader.split(',').map((s) => s.trim()).filter(Boolean);
    if (!offered.includes('mqtt')) {
      return new Response('mqtt subprotocol required', { status: 400 });
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
    return new Response(null, {
      status: 101,
      webSocket: client,
      headers: { 'Sec-WebSocket-Protocol': 'mqtt' },
    });
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
    // MQTT 3.1.1 §3.1.2.10 — refresh keepalive only when at least one
    // complete Control Packet has been decoded. Byte-dribbling partials
    // must not keep the connection alive indefinitely.
    if (packets.length > 0) att.lastRxMs = Date.now();
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
      const topic = att.lwt.topic;
      const parsed = parseTopic(this.deviceId, topic);
      const lwtPayload = att.lwt.payloadB64 ? b64dec(att.lwt.payloadB64) : new Uint8Array(0);
      // Phase 2B.1 §1 — a retained LWT must survive the author's death.
      // Persist it before fanout so a brand-new subscriber that connects
      // AFTER the LWT fires still receives the offline presence record.
      if (att.lwt.retain && parsed.ok && RETAIN_ALLOWED.has(parsed.suffix)) {
        if (lwtPayload.length === 0) {
          try { await this.storage.delete(`retained:${parsed.suffix}`); } catch {}
        } else {
          try {
            await this.storage.put(`retained:${parsed.suffix}`, {
              qos: att.lwt.qos, payloadB64: att.lwt.payloadB64, ts: Date.now(),
            });
          } catch {}
        }
      }
      await this._fanoutPublish({
        topic, qos: att.lwt.qos, retain: false,
        payload: lwtPayload, originWs: ws,
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
    if (att.state === 'await-connect'
        && pkt.type !== 'CONNECT' && pkt.type !== 'CONNECT_UNSUPPORTED') {
      return this._closeFatal(ws, 1002, 'EXPECT_CONNECT');
    }
    switch (pkt.type) {
      case 'CONNECT': return this._handleConnect(ws, att, pkt);
      case 'CONNECT_UNSUPPORTED':
        // MQTT-3.1.2-2 — tell the client we don't speak its protocol.
        ws.send(encodeConnack(ConnackCode.UNACCEPTABLE_PROTOCOL));
        return this._closeFatal(ws, 1008, 'BAD_PROTOCOL');
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
    // Contract §1: 30..120 inclusive. Zero (keepalive-disabled) is refused
    // because retained presence must not stale on a half-open link.
    if (keepaliveSec < KEEPALIVE_MIN_SEC || keepaliveSec > KEEPALIVE_MAX_SEC) {
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
    att.nextPacketId = 1; // per-connection, persists via attachment
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
    // Phase 2B.1 §6 — at most one authenticated device-role connection per
    // DO. A fresh ESP32 CONNECT evicts any prior device socket so two
    // devices never both subscribe to `command`.
    //
    // Phase 2B.2 — MQTT-3.1.4-2 also mandates that a CONNECT with a client
    // id already in use must disconnect the existing session. Evict any
    // other open socket sharing this clientId, regardless of role (covers
    // the "two web tabs with the same clientId" case too).
    {
      const sockets = typeof this.state.getWebSockets === 'function'
        ? this.state.getWebSockets() : [];
      for (const peer of sockets) {
        if (peer === ws) continue;
        let patt;
        try { patt = peer.deserializeAttachment() || {}; } catch { continue; }
        if (patt.state !== 'open') continue;
        const sameDeviceRole = resolved.role === 'device' && patt.role === 'device';
        const sameClientId = patt.clientId && patt.clientId === att.clientId;
        if (sameDeviceRole || sameClientId) {
          patt.disconnected = true;
          try { peer.serializeAttachment(patt); } catch {}
          try { peer.close(1000, sameClientId ? 'CLIENTID_TAKEOVER' : 'TAKEOVER'); } catch {}
        }
      }
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

    // Contract §2 — a PUBLISH's decoded QoS must match the topic's required
    // publish QoS exactly. Mismatch is a protocol misuse: PUBACK to hide
    // role structure (if QoS1), then DROP (no retain, no fanout). Counts
    // against the per-connection violation budget just like an ACL miss.
    const needQos = requiredPublishQos(parsed.suffix);
    if (needQos < 0 || pkt.qos !== needQos) {
      if (pkt.qos > 0) ws.send(encodePuback(pkt.packetId));
      att.violations = (att.violations || 0) + 1;
      ws.serializeAttachment(att);
      if (att.violations >= 2) return this._closeFatal(ws, 1008, 'BAD_PUB_QOS');
      return;
    }

    // Phase 2B.1 §4 — a PUBLISH with retain=1 to a retain-forbidden control
    // topic (command/config.set/reminders.set/history.request) is a protocol
    // misuse. Transport-ACK it to avoid leaking role structure, then DROP:
    // no retain store, no fanout, no execution. Legitimate publishers never
    // set retain=1 on these topics.
    if (pkt.retain && RETAIN_FORBIDDEN.has(parsed.suffix)) {
      if (pkt.qos > 0) ws.send(encodePuback(pkt.packetId));
      att.violations = (att.violations || 0) + 1;
      ws.serializeAttachment(att);
      if (att.violations >= 2) return this._closeFatal(ws, 1008, 'RETAIN_FORBIDDEN');
      return;
    }

    // Retain only honoured for the two allow-listed topics (presence,
    // reminders/reported) and never persisted for others.
    let effectiveRetain = pkt.retain && RETAIN_ALLOWED.has(parsed.suffix);

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
    // Authorized publish arriving at the right QoS clears the consecutive
    // violation counter — only CONSECUTIVE misuses trigger a close.
    if (att.violations) {
      att.violations = 0;
      try { ws.serializeAttachment(att); } catch {}
    }
    // QoS1 PUBACK to publisher before fanout — this is a transport ACK,
    // never an application ACK (see TRANSACTION_V2_SPEC.md).
    if (pkt.qos > 0) ws.send(encodePuback(pkt.packetId));
    const closedSlow = await this._fanoutPublish({
      topic: pkt.topic, qos: pkt.qos, retain: false,
      payload: pkt.payload, originWs: ws,
    });
    if (closedSlow && closedSlow.length > 0) {
      // Slow consumers were closed; no further work on this publish.
    }
  }

  async _fanoutPublish({ topic, qos, retain, payload, originWs }) {
    const sockets = typeof this.state.getWebSockets === 'function'
      ? this.state.getWebSockets() : [];
    const closed = [];
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
        att.inflight = att.inflight || [];
        // Phase 2B.1 §2 — bounded, non-dropping. When a slow consumer fills
        // its inflight ring, we DO NOT silently discard a QoS1 message. The
        // broker closes that connection (slow-consumer backpressure); the
        // client reconnects (clean session) and any state-changing control
        // layer above us retries per Transaction V2.
        if (att.inflight.length >= INFLIGHT_LIMIT) {
          try { peer.serializeAttachment(att); } catch {}
          this._closeFatal(peer, 1013, 'SLOW_CONSUMER');
          closed.push(peer);
          continue;
        }
        packetId = this._nextOutboundPacketId(att);
        att.inflight.push({ packetId, deliveredAt: Date.now() });
        peer.serializeAttachment(att);
      }
      try {
        peer.send(encodePublish({ topic, qos: outQos, retain, packetId, payload }));
      } catch { /* peer closed; ignore */ }
    }
    return closed;
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
        att.inflight = att.inflight || [];
        if (att.inflight.length >= INFLIGHT_LIMIT) {
          try { ws.serializeAttachment(att); } catch {}
          this._closeFatal(ws, 1013, 'SLOW_CONSUMER_RETAINED');
          return;
        }
        packetId = this._nextOutboundPacketId(att);
        att.inflight.push({ packetId, deliveredAt: Date.now() });
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

  // Per-connection packet id generator. The id and the inflight set BOTH
  // live in `att`, so hibernation preserves them and the generator never
  // hands out a packet id that is currently inflight to this peer.
  _nextOutboundPacketId(att) {
    att.inflight = att.inflight || [];
    const inflightIds = new Set(att.inflight.map((e) => e.packetId));
    let start = typeof att.nextPacketId === 'number' && att.nextPacketId >= 1 ? att.nextPacketId : 1;
    for (let attempt = 0; attempt < 0xffff; attempt++) {
      let candidate = start + attempt;
      // Wrap 1..0xffff (packet id 0 is reserved).
      candidate = ((candidate - 1) % 0xffff) + 1;
      if (!inflightIds.has(candidate)) {
        att.nextPacketId = ((candidate) % 0xffff) + 1;
        return candidate;
      }
    }
    // Should be unreachable: inflight is bounded at INFLIGHT_LIMIT.
    throw new Error('packet id space exhausted');
  }

  async _scheduleAlarm() {
    const current = typeof this.storage.getAlarm === 'function'
      ? await this.storage.getAlarm() : null;
    if (current == null) {
      try { await this.storage.setAlarm(Date.now() + ALARM_INTERVAL_MS); } catch {}
    }
  }
}
