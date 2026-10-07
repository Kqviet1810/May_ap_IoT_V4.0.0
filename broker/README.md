# MAYAP MQTT broker

One process, one topic space, two ways in:

```
ESP32  --MQTT 3.1.1 over TLS (tcp 8883)------\
                                              >-- broker/server.js --  per-device core (cloudflare/src/broker)
Web    --MQTT.js over WSS  /mqtt/<deviceId> --/                         topics, QoS0/1, retain, LWT, ACL, takeover

Cloudflare Worker: login, D1, push, OTA, device registration (returns the broker host and the device's
own password). It is NOT in the realtime path of either client.
```

Both transports are adapters (`runtime-node.js`) around the same broker core
(`cloudflare/src/broker/broker-do.js`, `acl.js`, `mqtt-codec.js`), so a command published by the Web over WSS
reaches the ESP32 on its TLS connection, the ESP32's retained presence and LWT are what the Web sees, and
a new ESP32 connection takes over the old one regardless of how each arrived. Contract:
[doc/MQTT_CONTRACT.md](../doc/MQTT_CONTRACT.md).

## Run

```bash
cd broker && npm ci
BROKER_DEVICE_SECRET=... BROKER_WEB_TOKEN_SECRET=... \
TLS_CERT_FILE=/etc/letsencrypt/live/<host>/fullchain.pem TLS_KEY_FILE=/etc/letsencrypt/live/<host>/privkey.pem \
MQTT_TLS_PORT=8883 WS_PORT=443 BROKER_DATA_FILE=/var/lib/mayap/state.json \
node server.js
```

or `docker build -f broker/Dockerfile -t mayap-mqtt-broker .` from the repository root (mount the certificate
directory at `/certs` and a volume at `/data`; map `443:8443`).

| Variable | Meaning |
|---|---|
| `BROKER_DEVICE_SECRET` | Derives every ESP32's password: `HMAC-SHA256(secret, "mayap-mqtt-device:v1\n<deviceId>")`. Same value as the Worker's `MQTT_DEVICE_SECRET`. Unset: devices cannot connect. |
| `BROKER_WEB_TOKEN_SECRET` | Verifies the Web's short-lived tokens. Same value as the Worker's `MQTT_WEB_TOKEN_SECRET`. Unset: the Web cannot connect. |
| `TLS_CERT_FILE`, `TLS_KEY_FILE` | PEM chain/key (for example Let's Encrypt). Without them only plain WS/TCP listeners exist (development). Reloaded on `SIGHUP` and when the files change, without dropping connections. |
| `MQTT_TLS_PORT` | ESP32 listener, default `8883`. |
| `WS_PORT` | Web listener (`https` + WebSocket with a certificate), default `443`. `/healthz` answers `ok`. |
| `MQTT_TCP_PORT` | Plain MQTT for local development only (default off). |
| `BROKER_DATA_FILE` | Optional JSON file that keeps retained messages across restarts. |
| `BIND_HOST` | Default `0.0.0.0`. |

The certificate must chain to a root the firmware trusts (`MAYAP_TLS_ROOT_CA` in `build_public.h` already holds
GTS Root R4 and ISRG Root X1, so Let's Encrypt works). Cloudflare's proxy cannot carry raw TCP: the broker host
must be reachable directly (a VPS or any container host that exposes TCP).

## Worker side

Set the host once (GitHub repository variable `MQTT_BROKER_HOST`, or `MQTT_BROKER_HOST` in `wrangler.toml`) and the
secrets `MQTT_DEVICE_SECRET` / `MQTT_WEB_TOKEN_SECRET`. `/api/device/register` then returns
`mqtt_host`, `mqtt_port` and `mqtt_password` to the device (stored in NVS); `/api/mqtt-session` hands the Web
`wss://<host>/mqtt/<deviceId>` plus its token. Nothing about the broker is compiled into the firmware.

## Limits (by design)

One process holds all sessions (clean-session MQTT, nothing to replay after a restart); retained presence is the
only state and it is persisted to `BROKER_DATA_FILE`. Horizontal scaling would need device-affine routing.
