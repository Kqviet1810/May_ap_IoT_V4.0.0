// MAYAP V4 broker-lite - standalone Worker entry.
//
// This Worker is INDEPENDENT of the main Push worker. It mounts
// `/mqtt/<deviceId>` and forwards WebSocket upgrades to the MqttBrokerDO
// instance for that device.
//
// Deployed with its OWN wrangler config (cloudflare/wrangler-broker.toml) so
// Phase 2B can stand up a broker without touching production Push/OTA/alarm
// infrastructure.

export { MqttBrokerDO } from './broker-do.js';
import { DEVICE_ID_RE } from './acl.js';

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const match = url.pathname.match(/^\/mqtt\/([A-Za-z0-9_-]{3,40})$/);
    if (match) {
      const deviceId = match[1];
      if (!DEVICE_ID_RE.test(deviceId)) {
        return new Response('bad device id', { status: 400 });
      }
      const id = env.MQTT_BROKER.idFromName(deviceId);
      const stub = env.MQTT_BROKER.get(id);
      return stub.fetch(request);
    }
    if (url.pathname === '/healthz') {
      return new Response('ok', { status: 200 });
    }
    return new Response('not found', { status: 404 });
  },
};
