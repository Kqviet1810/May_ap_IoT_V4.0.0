// MAYAP MQTT broker process: ESP32 over MQTT/TLS and Web over MQTT/WSS, one topic space.
// Configuration is environment only; see broker/README.md.
import fs from 'node:fs';
import http from 'node:http';
import https from 'node:https';
import net from 'node:net';
import tls from 'node:tls';
import { WebSocketServer } from 'ws';

import { StreamingDecoder } from '../cloudflare/src/broker/mqtt-codec.js';
import { DEVICE_ID_RE } from '../cloudflare/src/broker/acl.js';
import { BrokerRuntime, NodeConnection } from './runtime-node.js';

const log = (record) => console.log(JSON.stringify({ t: new Date().toISOString(), ...record }));

export function configFromEnv(env = process.env) {
  const num = (name, fallback) => (env[name] === undefined || env[name] === '' ? fallback : Number(env[name]));
  return {
    bindHost: env.BIND_HOST || '0.0.0.0',
    certFile: env.TLS_CERT_FILE || '',
    keyFile: env.TLS_KEY_FILE || '',
    tlsPort: num('MQTT_TLS_PORT', 8883),     // ESP32: MQTT over TLS
    tcpPort: num('MQTT_TCP_PORT', 0),        // plain MQTT, local/dev only (0 = off)
    wsPort: num('WS_PORT', 443),             // Web: MQTT over WSS (plain WS when no certificate)
    dataFile: env.BROKER_DATA_FILE || '',
    brokerEnv: {
      BROKER_DEVICE_SECRET: env.BROKER_DEVICE_SECRET || '',
      BROKER_WEB_TOKEN_SECRET: env.BROKER_WEB_TOKEN_SECRET || '',
    },
  };
}

const PRE_CONNECT_MAX_BYTES = 4096;
const PRE_CONNECT_TIMEOUT_MS = 10_000;

export async function startBroker(config = configFromEnv()) {
  const runtime = new BrokerRuntime({ env: config.brokerEnv, dataFile: config.dataFile, log });
  const servers = [];
  let tlsServer = null, tcpServer = null;
  const useTls = Boolean(config.certFile && config.keyFile);
  const readTls = () => ({ cert: fs.readFileSync(config.certFile), key: fs.readFileSync(config.keyFile), minVersion: 'TLSv1.2' });
  if (!config.brokerEnv.BROKER_DEVICE_SECRET || !config.brokerEnv.BROKER_WEB_TOKEN_SECRET) {
    log({ event: 'warning', message: 'missing BROKER_DEVICE_SECRET and/or BROKER_WEB_TOKEN_SECRET: that role fails closed' });
  }

  // --- raw MQTT (ESP32): the device id is the CONNECT username, so the first packet is
  // parsed (not consumed) to pick the per-device core, then replayed into it.
  const onDeviceSocket = (socket) => {
    socket.setNoDelay(true);
    let routed = null;
    let pre = Buffer.alloc(0);
    const timer = setTimeout(() => socket.destroy(), PRE_CONNECT_TIMEOUT_MS);
    socket.on('error', () => {});
    socket.on('data', (chunk) => {
      if (routed) { runtime.message(routed.entry, routed.connection, new Uint8Array(chunk)); return; }
      pre = Buffer.concat([pre, chunk]);
      if (pre.length > PRE_CONNECT_MAX_BYTES) { socket.destroy(); return; }
      let packet;
      try {
        const decoder = new StreamingDecoder();
        decoder.push(new Uint8Array(pre));
        [packet] = decoder.drain();
      } catch { socket.destroy(); return; }
      if (!packet) return;                                     // CONNECT not complete yet
      const deviceId = packet.type === 'CONNECT' ? packet.username : '';
      if (!deviceId || !DEVICE_ID_RE.test(deviceId)) { socket.destroy(); return; }
      clearTimeout(timer);
      const connection = new NodeConnection('tcp', socket);
      routed = { entry: null, connection };
      const first = new Uint8Array(pre);
      runtime.attach(deviceId, connection).then((entry) => {
        routed.entry = entry;
        socket.on('close', () => runtime.closed(entry, connection, connection.closeCode || 1006, connection.closeReason || '', Boolean(connection.closeCode)));
        runtime.message(entry, connection, first);
      });
    });
    socket.on('close', () => { clearTimeout(timer); });
  };
  if (useTls && config.tlsPort) {
    const server = tls.createServer(readTls(), onDeviceSocket);
    await new Promise((resolve, reject) => server.once('error', reject).listen(config.tlsPort, config.bindHost, resolve));
    servers.push(server);
    tlsServer = server;
    log({ event: 'listening', transport: 'mqtt+tls', port: server.address().port });
  }
  if (config.tcpPort) {
    const server = net.createServer(onDeviceSocket);
    await new Promise((resolve, reject) => server.once('error', reject).listen(config.tcpPort, config.bindHost, resolve));
    servers.push(server);
    tcpServer = server;
    log({ event: 'listening', transport: 'mqtt+tcp (plain)', port: server.address().port });
  }

  // --- MQTT over WebSocket (Web): /mqtt/<deviceId>, subprotocol "mqtt" mandatory.
  const wss = new WebSocketServer({ noServer: true, maxPayload: 8192, perMessageDeflate: false });
  const onRequest = (request, response) => {
    if (request.url === '/healthz') { response.writeHead(200, { 'content-type': 'text/plain' }); response.end('ok'); return; }
    response.writeHead(404); response.end('not found');
  };
  const wsServer = useTls ? https.createServer(readTls(), onRequest) : http.createServer(onRequest);
  wsServer.on('upgrade', (request, socket, head) => {
    const match = /^\/mqtt\/([A-Za-z0-9_-]{3,40})$/.exec((request.url || '').split('?')[0]);
    const offered = String(request.headers['sec-websocket-protocol'] || '').split(',').map((s) => s.trim());
    if (!match || !DEVICE_ID_RE.test(match[1]) || !offered.includes('mqtt')) {
      socket.end('HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n');
      return;
    }
    const deviceId = match[1];
    wss.handleUpgrade(request, socket, head, (ws) => {
      const connection = new NodeConnection('ws', ws);
      runtime.attach(deviceId, connection).then((entry) => {
        ws.on('message', (data, isBinary) => runtime.message(entry, connection,
          isBinary ? new Uint8Array(Array.isArray(data) ? Buffer.concat(data) : data) : 'text'));
        ws.on('close', (code, reason) => runtime.closed(entry, connection, code, String(reason || ''), code === 1000));
        ws.on('error', (error) => runtime.failed(entry, connection, error));
      });
    });
  });
  wss.options.handleProtocols = (protocols) => (protocols.has('mqtt') ? 'mqtt' : false);
  await new Promise((resolve, reject) => wsServer.once('error', reject).listen(config.wsPort, config.bindHost, resolve));
  servers.push(wsServer);
  log({ event: 'listening', transport: useTls ? 'mqtt+wss' : 'mqtt+ws (plain)', port: wsServer.address().port });

  // Certificates are renewed outside the process (certbot, ~60 days): reload on SIGHUP and
  // whenever the files change, without dropping connections.
  const reload = () => {
    if (!useTls) return;
    try {
      const context = readTls();
      for (const server of servers) if (typeof server.setSecureContext === 'function') server.setSecureContext(context);
      log({ event: 'tls-reloaded' });
    } catch (error) { log({ event: 'tls-reload-failed', error: String(error) }); }
  };
  if (useTls) {
    process.on('SIGHUP', reload);
    const stamp = () => `${fs.statSync(config.certFile).mtimeMs}:${fs.statSync(config.keyFile).mtimeMs}`;
    let last = stamp();
    setInterval(() => { try { const now = stamp(); if (now !== last) { last = now; reload(); } } catch {} }, 6 * 3600 * 1000).unref();
  }

  return {
    runtime,
    ports: { tls: tlsServer ? tlsServer.address().port : 0, tcp: tcpServer ? tcpServer.address().port : 0, ws: wsServer.address().port },
    async stop() {
      runtime.saveNow();
      for (const server of servers) server.close();
      wss.close();
    },
  };
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const broker = await startBroker();
  const shutdown = () => { broker.stop().finally(() => process.exit(0)); };
  process.on('SIGTERM', shutdown);
  process.on('SIGINT', shutdown);
}
