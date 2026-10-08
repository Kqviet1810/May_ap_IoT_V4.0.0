// Bounded first-party wrapper around the vendored MQTT.js (MQTT 3.1.1 over WSS).
// One connection per selected device: the broker runs one Durable Object per
// device, so the device id is part of the WebSocket path. The wrapper only moves
// bytes; request ids, signatures, replay and APPLIED/REJECTED stay in app.js.
(() => {
  'use strict';
  const ROOT = 'mayap/v1';
  const MAX_INBOUND_BYTES = 4096;
  // [channel, subscribed QoS]. QoS follows doc/MQTT_CONTRACT.md section 2.
  const SUBSCRIPTIONS = [
    ['presence', 1], ['snapshot', 0], ['ack', 1], ['log', 0],
    ['config/reported', 1], ['history/reported', 1],
    // Broker-originated live state: how many device alarms the broker still has to hand to the Worker (QoS0, not retained).
    ['uplink', 0],
  ];
  const noop = () => {};

  function fail(code, message) {
    const error = new Error(message);
    error.code = code;
    return error;
  }

  function create(options) {
    const { deviceId, url, username, password, clientId, onMessage = noop, onState = noop } = options;
    if (!window.mqtt?.connect) throw fail('TRANSPORT_ERROR', 'Không tải được thành phần kết nối');
    if (!/^wss?:\/\//i.test(String(url || ''))) throw fail('TRANSPORT_ERROR', 'Địa chỉ broker không hợp lệ');
    const prefix = `${ROOT}/${deviceId}/`;
    const client = window.mqtt.connect(url, {
      protocolVersion: 4, clean: true, clientId, username, password,
      keepalive: options.keepalive || 30,
      // 0 = never reconnect by itself (a standby socket that fails is simply replaced by the caller).
      reconnectPeriod: options.reconnectPeriod === 0 ? 0 : (options.reconnectPeriod || 3000),
      connectTimeout: options.connectTimeout || 10000,
      resubscribe: false,
    });
    let ready = false;
    let ended = false;
    let tokenExp = Number(options.expiresAt || 0) * 1000;   // ms; when the broker stops accepting THIS socket's credentials

    const transport = {
      deviceId, client,
      get connected() { return ready && client.connected === true && !ended; },
      get tokenExpiresAt() { return tokenExp; },
      // Credentials used by this client's NEXT automatic reconnect (the open socket is not touched). The broker checks a token
      // only at CONNECT, so a fresh one must be in place before a reconnect happens - never by closing a healthy socket.
      // A standby socket is created without auto-reconnect; once it has become the live one it needs the normal behaviour.
      enableReconnect(ms) { if (!ended) client.options.reconnectPeriod = ms || 3000; },
      updateCredentials(mqttReply) {
        if (ended || !mqttReply || typeof mqttReply.password !== 'string') return;
        client.options.username = mqttReply.username || client.options.username;
        client.options.password = mqttReply.password;
        if (!client.connected) tokenExp = Number(mqttReply.expiresAt || 0) * 1000;
      },
      // Resolves on the broker PUBACK for QoS 1. That is transport delivery only:
      // the caller must still wait for the device's terminal ACK.
      publish(channel, wire, { qos = 1 } = {}) {
        return new Promise((resolve, reject) => {
          if (!transport.connected) return reject(fail('TRANSPORT_ERROR', 'Chưa kết nối với máy chủ'));
          try {
            client.publish(prefix + channel, wire, { qos, retain: false }, (error) => {
              if (error) reject(fail('UNCERTAIN', String(error.message || error)));
              else resolve({ at: performance.now() });
            });
          } catch (error) { reject(fail('TRANSPORT_ERROR', String(error.message || error))); }
        });
      },
      end() {
        ended = true;
        ready = false;
        try { client.end(true); } catch (_) {}
      },
    };

    client.on('connect', () => {
      if (ended) return;
      ready = false;
      onState('subscribing');
      const filters = Object.fromEntries(SUBSCRIPTIONS.map(([channel, qos]) => [prefix + channel, { qos }]));
      client.subscribe(filters, (error, granted) => {
        if (ended) return;
        if (error || !Array.isArray(granted) || granted.length !== SUBSCRIPTIONS.length ||
            granted.some((item) => item.qos === 128)) {
          onState('error', error || fail('TRANSPORT_ERROR', 'Broker từ chối đăng ký kênh'));
          try { client.reconnect?.(); } catch (_) {}
          return;
        }
        ready = true;
        onState('ready');
      });
    });
    client.on('reconnect', () => { ready = false; if (!ended) onState('reconnecting'); });
    client.on('close', () => { ready = false; if (!ended) onState('closed'); });
    client.on('offline', () => { ready = false; if (!ended) onState('offline'); });
    client.on('error', (error) => { if (!ended) onState('error', error); });
    client.on('message', (topic, data) => {
      if (ended || !topic.startsWith(prefix) || data.length > MAX_INBOUND_BYTES) return;
      let payload;
      try { payload = JSON.parse(new TextDecoder().decode(data)); } catch (_) { return; }
      if (payload === null || typeof payload !== 'object') return;
      onMessage(topic.slice(prefix.length), payload);
    });
    return transport;
  }

  window.MayapMqttTransport = Object.freeze({ create, ROOT, SUBSCRIPTIONS });
})();
