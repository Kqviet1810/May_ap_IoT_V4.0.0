'use strict';
// Hand-rolled MQTT 3.1.1 encoder/decoder for TESTS ONLY. Mirrors the client
// side of what the broker expects. Kept intentionally dumb and bounded.

function writeVarLen(len, out) {
  do {
    let digit = len % 128;
    len = Math.floor(len / 128);
    if (len > 0) digit |= 0x80;
    out.push(digit);
  } while (len > 0);
}

function utf8(s) {
  const buf = Buffer.from(s, 'utf-8');
  return [buf.length >> 8, buf.length & 0xff, ...buf];
}

function frame(firstByte, varPay) {
  const lenBytes = [];
  writeVarLen(varPay.length, lenBytes);
  return new Uint8Array([firstByte, ...lenBytes, ...varPay]);
}

exports.connect = ({
  clientId = 'test-client',
  cleanSession = true,
  keepalive = 60,
  username = null,
  password = null,
  will = null,
  protoName = 'MQTT',
  protoLevel = 4,
  reservedFlag = false,
} = {}) => {
  let flags = 0;
  if (cleanSession) flags |= 0x02;
  if (will) {
    flags |= 0x04;
    flags |= (will.qos & 0x03) << 3;
    if (will.retain) flags |= 0x20;
  }
  if (username != null) flags |= 0x80;
  if (password != null) flags |= 0x40;
  if (reservedFlag) flags |= 0x01;
  const vp = [];
  vp.push(...utf8(protoName));
  vp.push(protoLevel);
  vp.push(flags);
  vp.push((keepalive >> 8) & 0xff, keepalive & 0xff);
  vp.push(...utf8(clientId));
  if (will) {
    vp.push(...utf8(will.topic));
    const payloadBuf = Buffer.isBuffer(will.payload) ? will.payload : Buffer.from(will.payload || '', 'utf-8');
    vp.push((payloadBuf.length >> 8) & 0xff, payloadBuf.length & 0xff, ...payloadBuf);
  }
  if (username != null) vp.push(...utf8(username));
  if (password != null) {
    const buf = Buffer.isBuffer(password) ? password : Buffer.from(password, 'utf-8');
    vp.push((buf.length >> 8) & 0xff, buf.length & 0xff, ...buf);
  }
  return frame(0x10, vp);
};

exports.subscribe = ({ packetId = 1, filters } = {}) => {
  const vp = [];
  vp.push((packetId >> 8) & 0xff, packetId & 0xff);
  for (const { filter, qos } of filters) {
    vp.push(...utf8(filter));
    vp.push(qos & 0xff);
  }
  return frame(0x82, vp); // 1000 0010
};

exports.publish = ({ topic, qos = 0, retain = false, dup = false, packetId = 0, payload = Buffer.alloc(0) }) => {
  const vp = [];
  vp.push(...utf8(topic));
  if (qos > 0) vp.push((packetId >> 8) & 0xff, packetId & 0xff);
  const payloadBuf = Buffer.isBuffer(payload) ? payload : Buffer.from(payload, 'utf-8');
  vp.push(...payloadBuf);
  const first = 0x30 | (dup ? 0x08 : 0) | ((qos & 3) << 1) | (retain ? 1 : 0);
  return frame(first, vp);
};

exports.puback = ({ packetId }) => frame(0x40, [(packetId >> 8) & 0xff, packetId & 0xff]);
exports.pingreq = () => frame(0xc0, []);
exports.disconnect = () => frame(0xe0, []);

// --- parse helpers for inspecting server→client traffic ---
exports.parseFixed = (bytes) => {
  const type = (bytes[0] >> 4) & 0x0f;
  const flags = bytes[0] & 0x0f;
  let remaining = 0, multiplier = 1, i = 1;
  while (true) {
    const d = bytes[i];
    remaining += (d & 0x7f) * multiplier;
    i++;
    if ((d & 0x80) === 0) break;
    multiplier *= 128;
    if (i > 5) throw new Error('var len too long');
  }
  return { type, flags, headerLen: i, remaining, body: bytes.slice(i, i + remaining) };
};

exports.parseConnack = (bytes) => {
  const f = exports.parseFixed(bytes);
  if (f.type !== 2) throw new Error(`not CONNACK: type=${f.type}`);
  return { sessionPresent: !!(f.body[0] & 1), returnCode: f.body[1] };
};

exports.parseSuback = (bytes) => {
  const f = exports.parseFixed(bytes);
  if (f.type !== 9) throw new Error(`not SUBACK: type=${f.type}`);
  const packetId = (f.body[0] << 8) | f.body[1];
  return { packetId, codes: Array.from(f.body.slice(2)) };
};

exports.parsePublish = (bytes) => {
  const f = exports.parseFixed(bytes);
  if (f.type !== 3) throw new Error(`not PUBLISH: type=${f.type}`);
  const retain = !!(f.flags & 1);
  const qos = (f.flags >> 1) & 3;
  const dup = !!(f.flags & 8);
  const topicLen = (f.body[0] << 8) | f.body[1];
  const topic = Buffer.from(f.body.slice(2, 2 + topicLen)).toString('utf-8');
  let p = 2 + topicLen;
  let packetId = 0;
  if (qos > 0) {
    packetId = (f.body[p] << 8) | f.body[p + 1];
    p += 2;
  }
  const payload = Buffer.from(f.body.slice(p));
  return { topic, qos, retain, dup, packetId, payload };
};

exports.parsePuback = (bytes) => {
  const f = exports.parseFixed(bytes);
  if (f.type !== 4) throw new Error(`not PUBACK`);
  return { packetId: (f.body[0] << 8) | f.body[1] };
};

exports.parsePingresp = (bytes) => {
  const f = exports.parseFixed(bytes);
  if (f.type !== 13) throw new Error('not PINGRESP');
  return true;
};
