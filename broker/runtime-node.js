// Node runtime for the MAYAP broker core (cloudflare/src/broker/broker-do.js).
//
// The broker core is written against a tiny runtime surface (state.storage, accepted
// WebSocket-like objects with attachments, alarms). Cloudflare Durable Objects provide it
// for the Web-only Worker deployment; this file provides it for a long-running process
// that accepts BOTH transports:
//   - ESP32: MQTT over TLS (raw TCP, port 8883)   -> NodeConnection('tcp')
//   - Web:   MQTT over WSS  (/mqtt/<deviceId>)    -> NodeConnection('ws')
// Every connection of a device lands in the SAME core instance, so topics, retained
// presence, LWT, QoS1, ACL and takeover behave identically whichever way a client came in.
import fs from 'node:fs';
import path from 'node:path';

import { MqttBrokerDO } from '../cloudflare/src/broker/broker-do.js';

const MAX_WRITE_BACKLOG = 1024 * 1024;
const clone = (value) => JSON.parse(JSON.stringify(value));

class NodeStorage {
  constructor(initial, onChange) {
    this.map = new Map(Object.entries(initial || {}));
    this.onChange = onChange;
    this.alarmAt = null;
    this.alarmTimer = null;
    this.onAlarm = () => {};
  }
  async get(key) { return this.map.has(key) ? clone(this.map.get(key)) : undefined; }
  async put(key, value) { this.map.set(key, clone(value)); this.onChange(); }
  async delete(key) { const had = this.map.delete(key); if (had) this.onChange(); return had; }
  async list({ prefix = '' } = {}) {
    const out = new Map();
    for (const [key, value] of this.map) if (key.startsWith(prefix)) out.set(key, clone(value));
    return out;
  }
  async getAlarm() { return this.alarmAt; }
  async setAlarm(timestampMs) {
    clearTimeout(this.alarmTimer);
    this.alarmAt = timestampMs;
    this.alarmTimer = setTimeout(() => { this.alarmAt = null; this.onAlarm(); }, Math.max(0, timestampMs - Date.now()));
    this.alarmTimer.unref?.();
  }
  async deleteAlarm() { clearTimeout(this.alarmTimer); this.alarmAt = null; }
  snapshot() { return Object.fromEntries(this.map); }
}

class NodeState {
  constructor(storage) { this.storage = storage; this.connections = new Set(); this.ready = Promise.resolve(); }
  blockConcurrencyWhile(fn) { this.ready = Promise.resolve(fn()); return this.ready; }
  acceptWebSocket(connection) { this.connections.add(connection); }
  getWebSockets() { return [...this.connections].filter((c) => c.readyState === 1); }
}

// WebSocket-like wrapper (the surface broker-do.js uses) around a ws WebSocket or a net/tls socket.
export class NodeConnection {
  constructor(kind, impl) {
    this.kind = kind;            // 'ws' | 'tcp'
    this.impl = impl;
    this.closed = false;
    this.attachment = null;
    this.notified = false;
  }
  get readyState() { return this.closed ? 3 : 1; }
  send(data) {
    if (this.closed) return;
    const bytes = Buffer.from(data.buffer ? data.buffer : data, data.byteOffset || 0, data.byteLength);
    if (this.writeBacklog() > MAX_WRITE_BACKLOG) { this.terminate(); return; }
    if (this.kind === 'ws') this.impl.send(bytes, { binary: true });
    else this.impl.write(bytes);
  }
  writeBacklog() { return this.kind === 'ws' ? this.impl.bufferedAmount : this.impl.writableLength; }
  close(code = 1000, reason = '') {
    if (this.closed) return;
    this.closed = true;
    this.closeCode = code;
    this.closeReason = reason;
    if (this.kind === 'ws') this.impl.close(code, reason.slice(0, 100));
    else { this.impl.end(); setTimeout(() => this.impl.destroy(), 2000).unref?.(); }
  }
  terminate() { this.closed = true; if (this.kind === 'ws') this.impl.terminate(); else this.impl.destroy(); }
  serializeAttachment(value) { this.attachment = value === undefined ? null : clone(value); }
  deserializeAttachment() { return this.attachment ? clone(this.attachment) : null; }
}

export class BrokerRuntime {
  constructor({ env, dataFile = '', log = () => {} }) {
    this.env = env;
    this.dataFile = dataFile;
    this.log = log;
    this.cores = new Map();        // deviceId -> { core, state, queue }
    this.saveTimer = null;
    this.persisted = {};
    if (dataFile) {
      try { this.persisted = JSON.parse(fs.readFileSync(dataFile, 'utf8')); }
      catch (error) { if (error.code !== 'ENOENT') log({ event: 'data-load-failed', error: String(error) }); }
    }
  }

  entry(deviceId) {
    let entry = this.cores.get(deviceId);
    if (entry) return entry;
    const storage = new NodeStorage(this.persisted[deviceId], () => this.scheduleSave());
    const state = new NodeState(storage);
    entry = { deviceId, storage, state, core: null, tail: Promise.resolve() };
    entry.core = new MqttBrokerDO(state, this.env);
    entry.tail = state.ready;
    storage.onAlarm = () => this.run(entry, () => entry.core.alarm());
    this.cores.set(deviceId, entry);
    return entry;
  }

  // Events of one device are processed strictly one after another (what a Durable
  // Object's input gate guarantees): packet handlers await storage between steps.
  run(entry, fn) {
    entry.tail = entry.tail.then(fn).catch((error) => this.log({ event: 'core-error', deviceId: entry.deviceId, error: String(error && error.stack || error) }));
    return entry.tail;
  }

  async attach(deviceId, connection) {
    const entry = this.entry(deviceId);
    await this.run(entry, () => entry.core.acceptConnection(connection, deviceId));
    return entry;
  }
  message(entry, connection, data) { return this.run(entry, () => entry.core.webSocketMessage(connection, data)); }
  closed(entry, connection, code, reason, clean) {
    if (connection.notified) return Promise.resolve();
    connection.notified = true;
    connection.closed = true;
    entry.state.connections.delete(connection);
    return this.run(entry, () => entry.core.webSocketClose(connection, code, reason, clean));
  }
  failed(entry, connection, error) { return this.run(entry, () => entry.core.webSocketError(connection, error)); }

  scheduleSave() {
    if (!this.dataFile || this.saveTimer) return;
    this.saveTimer = setTimeout(() => { this.saveTimer = null; this.saveNow(); }, 250);
    this.saveTimer.unref?.();
  }
  saveNow() {
    if (!this.dataFile) return;
    const out = {};
    for (const [deviceId, entry] of this.cores) out[deviceId] = entry.storage.snapshot();
    const merged = { ...this.persisted, ...out };
    try {
      fs.mkdirSync(path.dirname(this.dataFile), { recursive: true });
      const temp = `${this.dataFile}.tmp`;
      fs.writeFileSync(temp, JSON.stringify(merged), { mode: 0o600 });
      fs.renameSync(temp, this.dataFile);
    } catch (error) { this.log({ event: 'data-save-failed', error: String(error) }); }
  }
}
