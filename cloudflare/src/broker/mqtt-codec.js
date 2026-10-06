// MAYAP V4 broker-lite - MQTT 3.1.1 bounded streaming codec.
// Pure ES module. No I/O. Safe to require from Node tests via dynamic import.
// Enforces §7 cua doc/MQTT_CONTRACT.md.

export const MQTT_MAX_PACKET = 4096;

export const PacketType = Object.freeze({
  CONNECT: 1,
  CONNACK: 2,
  PUBLISH: 3,
  PUBACK: 4,
  SUBSCRIBE: 8,
  SUBACK: 9,
  PINGREQ: 12,
  PINGRESP: 13,
  DISCONNECT: 14,
});

export const ConnackCode = Object.freeze({
  ACCEPTED: 0x00,
  UNACCEPTABLE_PROTOCOL: 0x01,
  IDENTIFIER_REJECTED: 0x02,
  SERVER_UNAVAILABLE: 0x03,
  BAD_CREDENTIALS: 0x04,
  NOT_AUTHORIZED: 0x05,
});

const utf8Decoder = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true });
const utf8Encoder = new TextEncoder();

export class MqttDecodeError extends Error {
  constructor(message, code = 'MALFORMED') {
    super(message);
    this.code = code;
  }
}

// Streaming decoder: feed bytes via push(), pull packets via drain().
//
// Per Phase 2B.1 contract revision §7 — the 4 KB limit is PER MQTT PACKET,
// not per WebSocket message. Multiple small packets may legitimately arrive
// coalesced into one frame totalling more than one packet's maximum.
//
// We therefore accept a larger accumulator (`bufferLimit`), but reject as
// OVERFLOW as soon as a single packet's declared Remaining Length would push
// its totalLen past MQTT_MAX_PACKET. The accumulator cap is a safety net
// against a peer that pushes gigabytes of unparsed junk without any complete
// packet; set it to MQTT_MAX_PACKET * 4 (16 KiB) which comfortably holds any
// stall case while remaining bounded.
export class StreamingDecoder {
  constructor({ max = MQTT_MAX_PACKET, bufferLimit } = {}) {
    this.max = max;
    this.bufferLimit = bufferLimit != null ? bufferLimit : max * 4;
    this.buffer = new Uint8Array(0);
  }

  push(bytes) {
    if (!(bytes instanceof Uint8Array)) {
      if (bytes && bytes.buffer instanceof ArrayBuffer) {
        bytes = new Uint8Array(bytes.buffer, bytes.byteOffset || 0, bytes.byteLength);
      } else if (bytes instanceof ArrayBuffer) {
        bytes = new Uint8Array(bytes);
      } else {
        throw new MqttDecodeError('push expects bytes', 'BAD_INPUT');
      }
    }
    // Reject obviously-oversized frames BEFORE allocating a merged buffer so
    // a hostile peer cannot cause us to grow the buffer past bufferLimit.
    if (this.buffer.length + bytes.length > this.bufferLimit) {
      throw new MqttDecodeError('decoder buffer overflow', 'OVERFLOW');
    }
    const next = new Uint8Array(this.buffer.length + bytes.length);
    next.set(this.buffer, 0);
    next.set(bytes, this.buffer.length);
    this.buffer = next;
  }

  // Return array of parsed packets consumed from the buffer.
  drain() {
    const out = [];
    while (true) {
      if (this.buffer.length < 2) break;
      const first = this.buffer[0];
      const type = (first >> 4) & 0x0f;
      const flags = first & 0x0f;
      if (type === 0 || type === 15) {
        throw new MqttDecodeError('reserved packet type', 'BAD_TYPE');
      }
      let remaining = 0;
      let multiplier = 1;
      let i = 1;
      let encodedBytes = 0;
      while (true) {
        // Guard BEFORE reading a 5th byte — Remaining Length is at most 4
        // bytes per MQTT 3.1.1 §2.2.3, regardless of whether the 5th byte
        // happens to have MSB=0. This closes the "byte-5 with cleared high
        // bit" bypass that a lenient parser would accept.
        if (encodedBytes >= 4) {
          throw new MqttDecodeError('remaining length > 4 bytes', 'BAD_LEN');
        }
        if (i >= this.buffer.length) {
          // Incomplete length — wait for more bytes.
          return out;
        }
        const digit = this.buffer[i];
        remaining += (digit & 0x7f) * multiplier;
        encodedBytes++;
        i++;
        if ((digit & 0x80) === 0) break;
        multiplier *= 128;
      }
      const totalLen = 1 + encodedBytes + remaining;
      if (totalLen > this.max) {
        throw new MqttDecodeError('packet exceeds MQTT_MAX_PACKET', 'OVERFLOW');
      }
      if (this.buffer.length < totalLen) break; // wait for payload
      const payload = this.buffer.subarray(1 + encodedBytes, totalLen);
      const packet = decodePacket(type, flags, payload);
      out.push(packet);
      this.buffer = this.buffer.subarray(totalLen);
    }
    return out;
  }
}

function decodePacket(type, flags, payload) {
  switch (type) {
    case PacketType.CONNECT: return decodeConnect(flags, payload);
    case PacketType.PUBLISH: return decodePublish(flags, payload);
    case PacketType.PUBACK: return decodePuback(flags, payload);
    case PacketType.SUBSCRIBE: return decodeSubscribe(flags, payload);
    case PacketType.PINGREQ:
      if (flags !== 0) throw new MqttDecodeError('PINGREQ flags != 0', 'BAD_FLAGS');
      if (payload.length !== 0) throw new MqttDecodeError('PINGREQ len', 'BAD_LEN');
      return { type: 'PINGREQ' };
    case PacketType.DISCONNECT:
      if (flags !== 0) throw new MqttDecodeError('DISCONNECT flags != 0', 'BAD_FLAGS');
      if (payload.length !== 0) throw new MqttDecodeError('DISCONNECT len', 'BAD_LEN');
      return { type: 'DISCONNECT' };
    default:
      throw new MqttDecodeError(`unsupported packet type ${type}`, 'UNSUPPORTED');
  }
}

// --- helpers --------------------------------------------------------------

class Cursor {
  constructor(bytes) { this.bytes = bytes; this.pos = 0; }
  remaining() { return this.bytes.length - this.pos; }
  readU8() {
    if (this.remaining() < 1) throw new MqttDecodeError('eof u8', 'BAD_LEN');
    return this.bytes[this.pos++];
  }
  readU16() {
    if (this.remaining() < 2) throw new MqttDecodeError('eof u16', 'BAD_LEN');
    const v = (this.bytes[this.pos] << 8) | this.bytes[this.pos + 1];
    this.pos += 2;
    return v;
  }
  readBytes(len) {
    if (this.remaining() < len) throw new MqttDecodeError('eof bytes', 'BAD_LEN');
    const out = this.bytes.subarray(this.pos, this.pos + len);
    this.pos += len;
    return out;
  }
  readString() {
    const len = this.readU16();
    const raw = this.readBytes(len);
    // MQTT 3.1.1 strings: strict UTF-8, no U+0000.
    for (let i = 0; i < raw.length; i++) {
      if (raw[i] === 0) throw new MqttDecodeError('null byte in string', 'BAD_UTF8');
    }
    try { return utf8Decoder.decode(raw); }
    catch { throw new MqttDecodeError('invalid utf-8', 'BAD_UTF8'); }
  }
  readBinary() {
    const len = this.readU16();
    return this.readBytes(len).slice(); // copy, caller may retain
  }
}

function isValidPublishTopic(topic) {
  if (!topic || topic.length === 0) return false;
  if (topic.includes('+') || topic.includes('#')) return false;
  if (topic.includes('\u0000')) return false;
  return true;
}

function isValidSubscribeFilter(filter) {
  if (!filter || filter.length === 0) return false;
  if (filter.includes('\u0000')) return false;
  // Each wildcard char must be its own level.
  const parts = filter.split('/');
  for (let i = 0; i < parts.length; i++) {
    const p = parts[i];
    if (p === '#') {
      if (i !== parts.length - 1) return false;
    } else if (p.includes('#')) {
      return false;
    } else if (p === '+') {
      // OK, single-level.
    } else if (p.includes('+')) {
      return false;
    }
  }
  return true;
}

// --- CONNECT --------------------------------------------------------------

function decodeConnect(flags, payload) {
  if (flags !== 0) throw new MqttDecodeError('CONNECT flags != 0', 'BAD_FLAGS');
  const c = new Cursor(payload);
  const protoName = c.readString();
  if (protoName !== 'MQTT') {
    throw new MqttDecodeError(`bad protocol name "${protoName}"`, 'BAD_PROTOCOL');
  }
  const protoLevel = c.readU8();
  if (protoLevel !== 4) {
    throw new MqttDecodeError('protocol level != 4', 'BAD_PROTOCOL_LEVEL');
  }
  const connectFlags = c.readU8();
  if ((connectFlags & 0x01) !== 0) {
    throw new MqttDecodeError('reserved connect flag set', 'BAD_FLAGS');
  }
  const cleanSession = (connectFlags & 0x02) !== 0;
  const willFlag = (connectFlags & 0x04) !== 0;
  const willQos = (connectFlags >> 3) & 0x03;
  const willRetain = (connectFlags & 0x20) !== 0;
  const passwordFlag = (connectFlags & 0x40) !== 0;
  const usernameFlag = (connectFlags & 0x80) !== 0;
  if (!cleanSession) {
    throw new MqttDecodeError('CleanSession must be 1', 'CLEAN_SESSION_REQUIRED');
  }
  if (!willFlag && (willQos !== 0 || willRetain)) {
    throw new MqttDecodeError('will flags set without will', 'BAD_FLAGS');
  }
  if (willQos > 1) {
    throw new MqttDecodeError('willQos > 1', 'QOS_UNSUPPORTED');
  }
  if (!usernameFlag && passwordFlag) {
    throw new MqttDecodeError('password without username', 'BAD_FLAGS');
  }
  const keepalive = c.readU16();
  const clientId = c.readString();
  let will = null;
  if (willFlag) {
    const willTopic = c.readString();
    const willPayload = c.readBinary();
    if (!isValidPublishTopic(willTopic)) {
      throw new MqttDecodeError('bad will topic', 'BAD_TOPIC');
    }
    will = { topic: willTopic, qos: willQos, retain: willRetain, payload: willPayload };
  }
  let username = null;
  let password = null;
  if (usernameFlag) username = c.readString();
  if (passwordFlag) {
    const bin = c.readBinary();
    // Treat password as binary; many brokers accept non-UTF-8.
    password = bin;
  }
  if (c.remaining() !== 0) {
    throw new MqttDecodeError('CONNECT trailing bytes', 'BAD_LEN');
  }
  return {
    type: 'CONNECT',
    cleanSession,
    keepalive,
    clientId,
    will,
    username,
    password,
  };
}

// --- PUBLISH --------------------------------------------------------------

function decodePublish(flags, payload) {
  const dup = (flags & 0x08) !== 0;
  const qos = (flags >> 1) & 0x03;
  const retain = (flags & 0x01) !== 0;
  if (qos > 1) throw new MqttDecodeError('PUBLISH QoS > 1', 'QOS_UNSUPPORTED');
  if (dup && qos === 0) throw new MqttDecodeError('DUP with QoS0', 'BAD_FLAGS');
  const c = new Cursor(payload);
  const topic = c.readString();
  if (!isValidPublishTopic(topic)) {
    throw new MqttDecodeError('bad publish topic', 'BAD_TOPIC');
  }
  let packetId = 0;
  if (qos > 0) {
    packetId = c.readU16();
    if (packetId === 0) throw new MqttDecodeError('packetId=0', 'BAD_PACKET_ID');
  }
  const body = c.bytes.subarray(c.pos);
  return { type: 'PUBLISH', dup, qos, retain, topic, packetId, payload: body };
}

// --- PUBACK ---------------------------------------------------------------

function decodePuback(flags, payload) {
  if (flags !== 0) throw new MqttDecodeError('PUBACK flags != 0', 'BAD_FLAGS');
  if (payload.length !== 2) throw new MqttDecodeError('PUBACK len != 2', 'BAD_LEN');
  const packetId = (payload[0] << 8) | payload[1];
  if (packetId === 0) throw new MqttDecodeError('PUBACK packetId=0', 'BAD_PACKET_ID');
  return { type: 'PUBACK', packetId };
}

// --- SUBSCRIBE ------------------------------------------------------------

function decodeSubscribe(flags, payload) {
  // MQTT 3.1.1 fixed-header flags for SUBSCRIBE must be 0b0010.
  if (flags !== 2) throw new MqttDecodeError('SUBSCRIBE flags != 2', 'BAD_FLAGS');
  const c = new Cursor(payload);
  const packetId = c.readU16();
  if (packetId === 0) throw new MqttDecodeError('SUBSCRIBE packetId=0', 'BAD_PACKET_ID');
  const filters = [];
  while (c.remaining() > 0) {
    const filter = c.readString();
    const reqQos = c.readU8();
    if ((reqQos & 0xfc) !== 0) throw new MqttDecodeError('subscribe reserved bits', 'BAD_FLAGS');
    if (reqQos > 1) throw new MqttDecodeError('subscribe QoS > 1', 'QOS_UNSUPPORTED');
    if (!isValidSubscribeFilter(filter)) {
      throw new MqttDecodeError('bad subscribe filter', 'BAD_TOPIC');
    }
    filters.push({ filter, qos: reqQos });
  }
  if (filters.length === 0) {
    throw new MqttDecodeError('SUBSCRIBE empty', 'BAD_LEN');
  }
  return { type: 'SUBSCRIBE', packetId, filters };
}

// =========================================================================
// Encoders
// =========================================================================

function writeVarLength(len) {
  if (len < 0 || len > 268435455) throw new MqttDecodeError('var length out of range', 'BAD_LEN');
  const out = [];
  do {
    let digit = len % 128;
    len = Math.floor(len / 128);
    if (len > 0) digit |= 0x80;
    out.push(digit);
  } while (len > 0);
  return out;
}

function framePacket(firstByte, variableAndPayload) {
  const lenBytes = writeVarLength(variableAndPayload.length);
  const out = new Uint8Array(1 + lenBytes.length + variableAndPayload.length);
  out[0] = firstByte;
  for (let i = 0; i < lenBytes.length; i++) out[1 + i] = lenBytes[i];
  out.set(variableAndPayload, 1 + lenBytes.length);
  if (out.length > MQTT_MAX_PACKET) {
    throw new MqttDecodeError('encoded packet exceeds max', 'OVERFLOW');
  }
  return out;
}

export function encodeConnack(returnCode, sessionPresent = false) {
  const vp = new Uint8Array(2);
  vp[0] = sessionPresent ? 1 : 0;
  vp[1] = returnCode & 0xff;
  return framePacket((PacketType.CONNACK << 4), vp);
}

export function encodeSuback(packetId, returnCodes) {
  const vp = new Uint8Array(2 + returnCodes.length);
  vp[0] = (packetId >> 8) & 0xff;
  vp[1] = packetId & 0xff;
  for (let i = 0; i < returnCodes.length; i++) vp[2 + i] = returnCodes[i] & 0xff;
  return framePacket((PacketType.SUBACK << 4), vp);
}

export function encodePuback(packetId) {
  const vp = new Uint8Array(2);
  vp[0] = (packetId >> 8) & 0xff;
  vp[1] = packetId & 0xff;
  return framePacket((PacketType.PUBACK << 4), vp);
}

export function encodePingresp() {
  return framePacket((PacketType.PINGRESP << 4), new Uint8Array(0));
}

export function encodePublish({ topic, qos = 0, retain = false, dup = false, packetId = 0, payload = new Uint8Array(0) }) {
  if (qos > 1) throw new MqttDecodeError('encode QoS > 1', 'QOS_UNSUPPORTED');
  if (!isValidPublishTopic(topic)) throw new MqttDecodeError('encode bad topic', 'BAD_TOPIC');
  const topicBytes = utf8Encoder.encode(topic);
  if (topicBytes.length > 65535) throw new MqttDecodeError('topic too long', 'BAD_LEN');
  const payloadBytes = payload instanceof Uint8Array
    ? payload
    : (payload == null ? new Uint8Array(0) : utf8Encoder.encode(String(payload)));
  const varLen = 2 + topicBytes.length + (qos > 0 ? 2 : 0) + payloadBytes.length;
  const vp = new Uint8Array(varLen);
  let p = 0;
  vp[p++] = (topicBytes.length >> 8) & 0xff;
  vp[p++] = topicBytes.length & 0xff;
  vp.set(topicBytes, p); p += topicBytes.length;
  if (qos > 0) {
    if (packetId <= 0 || packetId > 0xffff) throw new MqttDecodeError('encode bad packetId', 'BAD_PACKET_ID');
    vp[p++] = (packetId >> 8) & 0xff;
    vp[p++] = packetId & 0xff;
  }
  vp.set(payloadBytes, p);
  const firstByte = (PacketType.PUBLISH << 4)
    | (dup ? 0x08 : 0)
    | ((qos & 0x03) << 1)
    | (retain ? 0x01 : 0);
  return framePacket(firstByte, vp);
}

// =========================================================================
// Topic matching for subscribe filters.
// =========================================================================

export function topicMatches(filter, topic) {
  if (filter === topic) return true;
  const f = filter.split('/');
  const t = topic.split('/');
  let i = 0;
  for (; i < f.length; i++) {
    if (f[i] === '#') return i === f.length - 1;
    if (t[i] === undefined) return false;
    if (f[i] === '+') continue;
    if (f[i] !== t[i]) return false;
  }
  return i === t.length;
}
