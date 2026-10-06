'use strict';
// Minimal in-process stub of the Workers runtime surface the broker DO uses.
// Exercises hibernation by letting a test "wake" the DO: construct a new
// MqttBrokerDO with the SAME StubState (storage + sockets with attachments
// serialized to plain objects). Nothing of substance may live only in RAM.

const path = require('node:path');

function b64enc(bytes) {
  return Buffer.from(bytes).toString('base64');
}
function b64dec(str) {
  return new Uint8Array(Buffer.from(str, 'base64'));
}

class StubWebSocket {
  constructor(role) {
    this.role = role; // 'client' | 'server'
    this.peer = null;
    this.sent = []; // bytes received by the far end (for inspection)
    this.closed = false;
    this.closeCode = 0;
    this.closeReason = '';
    this._attachment = null;
    this._accepted = false;
  }
  get readyState() { return this.closed ? 3 : 1; }
  send(data) {
    if (this.closed) return;
    const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
    // Deliver as peer's incoming binary message.
    if (this.peer) this.peer._deliver(bytes);
  }
  _deliver(bytes) {
    if (this.closed) return;
    this.incoming.push(new Uint8Array(bytes));
  }
  close(code = 1000, reason = '') {
    if (this.closed) return;
    this.closed = true;
    this.closeCode = code;
    this.closeReason = reason;
    if (this.peer && !this.peer.closed) this.peer.closed = true;
  }
  serializeAttachment(obj) {
    if (obj === undefined) { this._attachment = null; return; }
    // Round-trip through JSON to match Workers hibernation semantics.
    this._attachment = JSON.parse(JSON.stringify(obj));
  }
  deserializeAttachment() {
    return this._attachment ? JSON.parse(JSON.stringify(this._attachment)) : null;
  }
}

class StubWebSocketPair {
  static make() {
    const a = new StubWebSocket('client');
    const b = new StubWebSocket('server');
    a.peer = b; b.peer = a;
    a.incoming = []; b.incoming = [];
    return [a, b];
  }
}

class StubStorage {
  constructor() { this.map = new Map(); this._alarm = null; }
  async get(key) { return this.map.has(key) ? JSON.parse(JSON.stringify(this.map.get(key))) : undefined; }
  async put(key, value) { this.map.set(key, JSON.parse(JSON.stringify(value))); }
  async delete(key) { this.map.delete(key); }
  async list({ prefix = '' } = {}) {
    const out = new Map();
    for (const [k, v] of this.map.entries()) {
      if (k.startsWith(prefix)) out.set(k, v);
    }
    return out;
  }
  async getAlarm() { return this._alarm; }
  async setAlarm(ts) { this._alarm = ts; }
  async deleteAlarm() { this._alarm = null; }
}

class StubState {
  constructor() {
    this.storage = new StubStorage();
    this.sockets = []; // all accepted server-side sockets
  }
  async blockConcurrencyWhile(fn) { return fn(); }
  acceptWebSocket(ws) {
    ws._accepted = true;
    this.sockets.push(ws);
  }
  getWebSockets() { return this.sockets.filter((ws) => !ws.closed); }
}

class StubWebSocketPairGlobal {
  constructor() {
    const [c, s] = StubWebSocketPair.make();
    this[0] = c;
    this[1] = s;
    return [c, s];
  }
}

async function loadBrokerModule() {
  const mod = await import(path.resolve(__dirname, '../../cloudflare/src/broker/broker-do.js'));
  return mod;
}

function installGlobals() {
  globalThis.WebSocketPair = function WebSocketPair() {
    const [c, s] = StubWebSocketPair.make();
    return [c, s];
  };
  if (typeof globalThis.btoa !== 'function') {
    globalThis.btoa = (s) => Buffer.from(s, 'binary').toString('base64');
    globalThis.atob = (b) => Buffer.from(b, 'base64').toString('binary');
  }
  // Node's native Response rejects status 101. Install a lenient stub so the
  // DO can finish a WebSocket upgrade response.
  const NativeResponse = globalThis.Response;
  class StubResponse {
    constructor(body, init = {}) {
      this.body = body;
      this.status = init.status || 200;
      this.headers = new Map(Object.entries(init.headers || {}));
      this.webSocket = init.webSocket || null;
    }
    async text() { return typeof this.body === 'string' ? this.body : ''; }
  }
  globalThis.Response = StubResponse;
  globalThis.__NativeResponse = NativeResponse;
}

async function makeBroker({ env = {} } = {}) {
  installGlobals();
  const { MqttBrokerDO } = await loadBrokerModule();
  const state = new StubState();
  const broker = new MqttBrokerDO(state, env);
  // Let the ctor's blockConcurrencyWhile settle.
  await Promise.resolve();
  return { broker, state };
}

async function openWebSocket(broker, deviceId, { subprotocol = 'mqtt' } = {}) {
  const req = new Request(`https://broker.test/mqtt/${deviceId}`, {
    method: 'GET',
    headers: {
      Upgrade: 'websocket',
      ...(subprotocol ? { 'Sec-WebSocket-Protocol': subprotocol } : {}),
    },
  });
  const res = await broker.fetch(req);
  if (res.status !== 101) {
    const text = await res.text().catch(() => '');
    throw new Error(`upgrade failed: ${res.status} ${text}`);
  }
  const client = res.webSocket;
  // The server-side ws is the last one accepted on state.
  const server = broker.state.sockets[broker.state.sockets.length - 1];
  return { client, server };
}

async function feed(broker, server, bytes) {
  const arr = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  await broker.webSocketMessage(server, arr);
}

function drainClient(client) {
  const out = client.incoming.slice();
  client.incoming.length = 0;
  return out;
}

async function clientReceive(client, timeoutMs = 50) {
  // Our stub is synchronous; return immediately.
  return drainClient(client);
}

async function simulateHibernate(brokerAndState, env = {}) {
  const { state } = brokerAndState;
  installGlobals();
  const { MqttBrokerDO } = await loadBrokerModule();
  const broker = new MqttBrokerDO(state, env);
  await Promise.resolve();
  return { broker, state };
}

module.exports = {
  makeBroker,
  openWebSocket,
  feed,
  clientReceive,
  simulateHibernate,
  StubWebSocket,
  b64enc, b64dec,
};
