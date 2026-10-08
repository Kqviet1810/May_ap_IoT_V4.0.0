#pragma once

#include "config.h"
#include "device_identity.h"
#include "network_io_guard.h"
#include "service_recovery.h"
#include "network_service.h"
#include "mqtt_wire.h"
#include "mqtt_ws.h"
#include "transaction_bridge.h"
#include "mqtt_uplink.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <esp_random.h>
#include <time.h>

// MQTT 3.1.1 over WebSocket over TLS (WSS, port 443) to the Cloudflare broker Durable Object -
// the same broker, topics and protocol the Web speaks with MQTT.js. Contract: doc/MQTT_CONTRACT.md.
// Same shape as the V2 owner: ONE static WiFiClientSecure, touched only by mqttTask (the static
// task pinned to core 0 by the sketch, the same task as every Transaction V2 bridge call), bounded
// static buffers, no extra task, no heap churn per message. There is no esp-mqtt and no WebSocket
// library: TLS comes from WiFiClientSecure, framing from mqtt_ws.h, the protocol from
// mqtt_wire.h; both are host-tested. Everything is polled from mayapMqttTransportUpdate(), so
// the controller (core 1) never waits on any of it and no hidden task exists.
//
// Credentials: the broker host is public and tracked below (override with -DMAYAP_BROKER_HOST for
// a private broker). The per-device password is NOT in the source or the binary: the account
// Worker returns it at /api/device/register and the firmware keeps it in NVS (mayapMqttKey).
// Until the device has registered, the transport stays idle.
#ifndef MAYAP_BROKER_HOST
#define MAYAP_BROKER_HOST "mayap-mqtt-broker.vietk-mayaptrung.workers.dev"
#endif
#ifndef MAYAP_BROKER_PORT
#define MAYAP_BROKER_PORT 443
#endif

namespace MayapMqttInternal {

constexpr char TOPIC_ROOT[] = "mayap/v1";
constexpr uint16_t KEEPALIVE_SEC = 30U;             // contract: 30..120
constexpr size_t PACKET_BUFFER = 2120U;             // MQTT packet: 2047 B payload + 63 B topic + 10 B headers
constexpr size_t TX_HEADROOM = MayapMqttWs::MAX_HEADER;   // WebSocket header is written in front of the packet
static_assert(PACKET_BUFFER >= MayapProtocol::FRAME_NORMAL_CAP + 72U, "MQTT packet buffer smaller than a contract frame");
constexpr uint8_t QOS1_INFLIGHT_MAX = 8U;           // PUBACK round trip via Cloudflare is ~100-400 ms; 4 filled during command bursts
constexpr uint32_t QOS1_STUCK_MS = 15000UL;
constexpr uint32_t STEP_TIMEOUT_MS = 5000UL;        // WebSocket upgrade / CONNACK / SUBACK wait
constexpr size_t UPGRADE_RESPONSE_MAX = 768U;       // bounded HTTP response header (held in rxBuffer)
constexpr uint32_t CLOCK_VALID_AFTER = 1700000000UL; // TLS certificate dates need real time
constexpr uint8_t PUMP_PACKET_BUDGET = 8U;

static WiFiClientSecure net;                        // owner: mqttTask only
static uint8_t txBuffer[TX_HEADROOM + PACKET_BUFFER];
static uint8_t rxBuffer[PACKET_BUFFER];
static MayapMqttWire::StreamParser parser(rxBuffer, sizeof(rxBuffer));
static MayapMqttWs::FrameParser ws;
inline uint8_t *txMqtt() { return txBuffer + TX_HEADROOM; }   // the MQTT packet is encoded here

static char clientId[32] = "";
static char prefix[40] = "";
static size_t prefixLength = 0U;
static char willTopic[64] = "";
static constexpr char WILL_MESSAGE[] = "{\"online\":false}";

static bool connected = false, ioEntered = false, netConfigured = false, linkFailed = false;
static uint16_t nextPacketId = 1U;
static uint16_t inflightId[QOS1_INFLIGHT_MAX];
static uint32_t inflightAt[QOS1_INFLIGHT_MAX];
static uint8_t inflightCount = 0U;
static uint32_t lastRxAt = 0U, lastTxAt = 0U;
#if MAYAP_DIAGNOSTIC_SERIAL
static uint32_t lastDiagAt = 0U;
#endif
static uint32_t droppedOversize = 0U, droppedForeign = 0U;
static uint8_t carry[64];                          // raw bytes that followed a handshake packet
static size_t carryLength = 0U;
// Realtime is best-effort and must never starve Wi-Fi or Cloud. The shared BackoffTimer starts at
// 1 s, which for a full TLS handshake is a reconnect storm: every attempt holds the TLS admission
// lease that Cloud HTTPS needs and churns ~40 KiB of heap. Realtime therefore retries slowly,
// and only once Wi-Fi has been up for STA_STABLE_MS.
constexpr uint32_t RETRY_STEPS_MS[] = {15000UL, 30000UL, 60000UL, 120000UL, 300000UL};
constexpr uint8_t RETRY_STEP_COUNT = sizeof(RETRY_STEPS_MS) / sizeof(RETRY_STEPS_MS[0]);
constexpr uint32_t RETRY_JITTER_MAX_MS = 2000UL;
constexpr uint32_t STA_STABLE_MS = 10000UL;
constexpr uint32_t YIELD_RESUME_MS = 3000UL;        // after Cloud released the lease: reconnect quickly (the Web shows offline meanwhile)
constexpr uint32_t BUSY_RETRY_MS = 3000UL;          // TLS lease/heap busy is contention, not a broker failure: no backoff escalation
struct Retry {
  uint8_t step = 0U;
  uint32_t nextAttemptAt = 0U;
  unsigned failures = 0U;
  void reset(uint32_t now) { step = 0U; nextAttemptAt = now; }
  bool ready(uint32_t now) const { return static_cast<int32_t>(now - nextAttemptAt) >= 0; }
  void onFailure(uint32_t now) {
    nextAttemptAt = now + RETRY_STEPS_MS[step] + esp_random() % (RETRY_JITTER_MAX_MS + 1U);
    if (step + 1U < RETRY_STEP_COUNT) ++step;
    ++failures;
  }
  void onSuccess() { step = 0U; }
  // A deliberate stop (Cloud needs the heap/lease) is not a failure, but the next handshake waits too.
  void holdOff(uint32_t now, uint32_t ms = RETRY_STEPS_MS[0]) {
    const uint32_t until = now + ms;
    if (static_cast<int32_t>(until - nextAttemptAt) > 0) nextAttemptAt = until;
  }
};
static Retry backoff{};
static uint32_t staUpSince = 0U;

struct ChannelPolicy { const char *channel; uint8_t qos; bool retain; };
// Publish QoS/retain per doc/MQTT_CONTRACT.md section 2. The broker enforces the
// same table; an unknown channel is never sent.
static constexpr ChannelPolicy POLICY[] = {
  {"presence", 1, true}, {"snapshot", 0, false}, {"ack", 1, false}, {"log", 0, false},
  {"config/reported", 1, false}, {"history/reported", 1, false},
  {"alarm", 1, false}, {"heartbeat", 1, false},   // device -> cloud ingest, see mqtt_uplink.h
};

inline const char *brokerHost() { return MAYAP_BROKER_HOST; }
inline uint16_t brokerPort() { return static_cast<uint16_t>(MAYAP_BROKER_PORT); }
inline bool configured() { return brokerHost()[0] != '\0' && mayapMqttKey()[0] != '\0'; }
inline bool clockValid() { return static_cast<uint32_t>(time(nullptr)) > CLOCK_VALID_AFTER; }
inline bool gateClosing() { return mayapWifiPortalExclusiveRequested() || mayapRadioRecoveryRequested(); }

// Sends one MQTT packet that was encoded at txMqtt(): the WebSocket header is built in front of it
// and the payload is masked in place (RFC 6455: client frames are always masked).
inline bool sendFrame(size_t length, uint8_t opcode) {
  size_t total = 0U;
  uint8_t *frame = MayapMqttWs::wrapClientFrame(txBuffer, TX_HEADROOM, length, opcode, esp_random(), &total);
  if (!frame) { linkFailed = true; return false; }
  if (net.write(frame, total) != total) { linkFailed = true; return false; }
  lastTxAt = millis();
  return true;
}
inline bool sendPacket(size_t length) {
  if (length == 0U) return false;
  return sendFrame(length, MayapMqttWs::BINARY);
}

inline void inflightClear() { inflightCount = 0U; }
inline void inflightAdd(uint16_t id, uint32_t now) {
  if (inflightCount < QOS1_INFLIGHT_MAX) { inflightId[inflightCount] = id; inflightAt[inflightCount] = now; ++inflightCount; }
}
inline void inflightAck(uint16_t id) {
  for (uint8_t i = 0U; i < inflightCount; ++i) {
    if (inflightId[i] != id) continue;
    for (uint8_t j = i; j + 1U < inflightCount; ++j) { inflightId[j] = inflightId[j + 1U]; inflightAt[j] = inflightAt[j + 1U]; }
    --inflightCount;
    return;
  }
}
inline uint16_t allocPacketId() {
  if (++nextPacketId == 0U) nextPacketId = 1U;
  return nextPacketId;
}

// Encodes and sends one PUBLISH. `track` puts a QoS1 packet into the in-flight window that the link watchdog
// watches (a bridge message nobody PUBACKs means a half-open link). Uplink packets are tracked by mqtt_uplink.h
// instead: the broker deliberately withholds their PUBACK when the Worker did not store the event.
inline bool publishChannel(const char *channel, const char *payload, size_t length, bool track, uint16_t *idOut) {
  if (!connected || !channel) return false;
  const ChannelPolicy *policy = nullptr;
  for (const ChannelPolicy &candidate : POLICY)
    if (!strcmp(candidate.channel, channel)) { policy = &candidate; break; }
  if (!policy || length == 0U || length >= MayapProtocol::FRAME_NORMAL_CAP) return false;
  const uint32_t now = millis();
  if (track && policy->qos > 0U && inflightCount >= QOS1_INFLIGHT_MAX) return false;  // bounded; bridge retries next cycle
  char topic[64];
  const int n = snprintf(topic, sizeof(topic), "%s/%s/%s", TOPIC_ROOT, MayapRealtimeInternal::deviceId, channel);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(topic)) return false;
  const uint16_t id = policy->qos ? allocPacketId() : 0U;
  const size_t packet = MayapMqttWire::encodePublish(txMqtt(), PACKET_BUFFER, topic,
      reinterpret_cast<const uint8_t *>(payload), length, policy->qos, policy->retain, id);
  if (!sendPacket(packet)) return false;
  if (track && policy->qos > 0U) inflightAdd(id, now);
  if (idOut) *idOut = id;
  return true;
}

inline bool publishFromBridge(const char *channel, const char *payload, size_t length) {
  if (!connected || !channel) return false;
  // `bootstrap` hints are not part of the V2 topic contract and have no ACL entry.
  if (!strcmp(channel, "bootstrap")) return true;
  // alarm/heartbeat belong to the uplink; the bridge never publishes them.
  if (!strcmp(channel, "alarm") || !strcmp(channel, "heartbeat")) return false;
  return publishChannel(channel, payload, length, true, nullptr);
}

inline void buildIdentity() {
  snprintf(clientId, sizeof(clientId), "esp-%s", MayapRealtimeInternal::deviceId);
  snprintf(prefix, sizeof(prefix), "%s/%s/", TOPIC_ROOT, MayapRealtimeInternal::deviceId);
  prefixLength = strlen(prefix);
  snprintf(willTopic, sizeof(willTopic), "%spresence", prefix);
}

// Owner task only. Closing the socket releases the whole TLS working set so Cloud/OTA can
// use the heap; it is idempotent.
inline void stopClient(bool graceful) {
  if (connected && graceful && !linkFailed) {
    // A clean DISCONNECT suppresses the LWT, so only send it when the offline presence was
    // really written; otherwise drop the link and let the broker fire the LWT.
    if (MayapRealtimeInternal::publishPresence(false) &&
        sendPacket(MayapMqttWire::encodeDisconnect(txMqtt(), PACKET_BUFFER))) {
      txMqtt()[0] = 0x03U; txMqtt()[1] = 0xE8U;                // WebSocket CLOSE, status 1000
      sendFrame(2U, MayapMqttWs::CLOSE);
    }
  }
  net.stop();
  mayapSetMqttTlsResident(false);   // memory is back before any HTTPS session may start
  connected = false;
  MayapUplink::onLinkDown();
  linkFailed = false;
  carryLength = 0U;
  inflightClear();
  parser.reset();
  ws.reset();
}

// Dispatches one complete broker->device packet. PUBLISH goes straight to the bridge: this
// is the same task that owns it, so no ring/handoff is needed.
inline void handlePacket() {
  using namespace MayapMqttWire;
  switch (parser.type()) {
    case PUBLISH: {
      if (parser.truncated()) { ++droppedOversize; return; }  // sender retry/UNCERTAIN path handles it
      PublishView view;
      if (!parsePublish(parser.flags(), parser.body(), parser.bodyLength(), view)) { linkFailed = true; return; }
      if (view.qos == 1U) sendPacket(encodePuback(txMqtt(), PACKET_BUFFER, view.packetId));
      char channel[20];
      if (view.topicLength <= prefixLength || memcmp(view.topic, prefix, prefixLength) != 0 ||
          view.topicLength - prefixLength >= sizeof(channel)) { ++droppedForeign; return; }
      const size_t channelLength = view.topicLength - prefixLength;
      memcpy(channel, view.topic + prefixLength, channelLength);
      channel[channelLength] = '\0';
      MayapRealtimeInternal::dispatchApplicationMessage(channel, view.payload, view.payloadLength);
      mayapServiceBeat(MayapRecovery::Service::Mqtt);
      return;
    }
    case PUBACK:
      if (parser.bodyLength() == 2U) {
        const uint16_t id = static_cast<uint16_t>((parser.body()[0] << 8U) | parser.body()[1]);
        inflightAck(id);
        MayapUplink::onPuback(id);
      }
      return;
    default:
      return;  // PINGRESP and anything unexpected only prove the link is alive
  }
}

// WebSocket control frames from the broker: PING must be answered (same payload), CLOSE ends
// the link, PONG only proves it is alive.
inline bool handleControl() {
  using namespace MayapMqttWs;
  switch (ws.controlOpcode()) {
    case PING:
      memcpy(txMqtt(), ws.control(), ws.controlLength());
      return sendFrame(ws.controlLength(), PONG);
    case CLOSE:
      return false;
    default:
      return true;
  }
}

// Reads what is available without blocking and parses at most PUMP_PACKET_BUDGET MQTT packets
// (raw TLS bytes -> WebSocket frames -> MQTT stream). Returns false when the link must be
// dropped. `wantType` != 0 makes it return true as soon as a packet of that type is ready
// (handshake); other packets are ignored then, and any raw bytes read after the wanted packet
// are kept in `carry` for the next call (the WebSocket parser is rewound to match).
inline bool pump(uint8_t wantType = 0U) {
  using MayapMqttWs::FrameParser;
  uint8_t chunk[sizeof(carry)];
  uint8_t packets = 0U;
  while (packets < PUMP_PACKET_BUDGET) {
    size_t count = 0U;
    if (carryLength > 0U) {
      memcpy(chunk, carry, carryLength);
      count = carryLength;
      carryLength = 0U;
    } else {
      const int available = net.available();
      if (available <= 0) return net.connected() || wantType != 0U;
      const int got = net.read(chunk, static_cast<size_t>(available) < sizeof(chunk) ? static_cast<size_t>(available) : sizeof(chunk));
      if (got <= 0) return net.connected() || wantType != 0U;
      count = static_cast<size_t>(got);
      lastRxAt = millis();
    }
    size_t used = 0U;
    while (used < count) {
      FrameParser::Event event;
      used += ws.feed(chunk + used, count - used, event);
      if (event == FrameParser::Event::Fatal) return false;
      if (event == FrameParser::Event::Control) { if (!handleControl()) return false; continue; }
      if (event != FrameParser::Event::Payload) continue;
      const uint8_t *slice = ws.payload();
      const size_t sliceLength = ws.payloadLength();
      size_t off = 0U;
      while (off < sliceLength) {
        off += parser.feed(slice + off, sliceLength - off);
        if (parser.fatal()) return false;
        if (!parser.ready()) continue;
        ++packets;
        if (wantType != 0U) {
          if (parser.type() != wantType) continue;
          const size_t unusedSlice = sliceLength - off;
          ws.unconsume(unusedSlice);
          used -= unusedSlice;
          carryLength = count - used;                 // caller reads parser.body() before the next feed
          if (carryLength) memcpy(carry, chunk + used, carryLength);
          return true;
        }
        handlePacket();
        if (linkFailed) return false;
      }
    }
  }
  return true;
}

inline bool awaitPacket(uint8_t type) {
  const uint32_t startedAt = millis();
  parser.reset();
  while (MayapRecovery::age(millis(), startedAt) < STEP_TIMEOUT_MS) {
    if (pump(type) && parser.ready() && parser.type() == type) return true;
    if (parser.fatal() || !net.connected() || gateClosing() || mayapCloudTlsYieldRequested(millis())) return false;
    mayapServiceBeat(MayapRecovery::Service::Mqtt);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return false;
}

// HTTP/1.1 Upgrade to WebSocket on the open TLS socket (path /mqtt/<deviceId>, subprotocol mqtt).
// The response header is read byte by byte so no WebSocket/MQTT byte is ever over-read, held in
// rxBuffer (bounded), and verified: status 101, Upgrade, Sec-WebSocket-Accept, no extensions.
inline bool upgradeWebSocket() {
  using namespace MayapMqttWs;
  uint8_t nonce[16];
  for (uint8_t i = 0U; i < 4U; ++i) { const uint32_t r = esp_random(); memcpy(nonce + 4U * i, &r, 4U); }
  char key[KEY_BASE64 + 1U], accept[ACCEPT_BASE64 + 1U], path[48], host[80];
  makeKey(nonce, key);
  expectedAccept(key, accept);
  snprintf(path, sizeof(path), "/mqtt/%s", MayapRealtimeInternal::deviceId);
  if (brokerPort() == 443U) snprintf(host, sizeof(host), "%s", brokerHost());
  else snprintf(host, sizeof(host), "%s:%u", brokerHost(), static_cast<unsigned>(brokerPort()));
  const size_t request = buildUpgradeRequest(reinterpret_cast<char *>(txBuffer), sizeof(txBuffer), host, path, key);
  if (request == 0U || net.write(txBuffer, request) != request) { mayapSerialPrintf(false, "[MQTT] ws upgrade not sent\n"); return false; }
  size_t got = 0U;
  const uint32_t startedAt = millis();
  while (MayapRecovery::age(millis(), startedAt) < STEP_TIMEOUT_MS) {
    while (net.available() > 0 && got < UPGRADE_RESPONSE_MAX) {
      uint8_t byte = 0U;
      if (net.read(&byte, 1U) != 1) break;
      rxBuffer[got++] = byte;
      if (got >= 4U && memcmp(rxBuffer + got - 4U, "\r\n\r\n", 4U) == 0) {
        const Handshake result = checkUpgradeResponse(reinterpret_cast<const char *>(rxBuffer), got, accept);
        if (result != Handshake::Ok) mayapSerialPrintf(false, "[MQTT] ws upgrade refused (%u)\n", static_cast<unsigned>(result));
        lastRxAt = millis();
        return result == Handshake::Ok;
      }
    }
    if (got >= UPGRADE_RESPONSE_MAX || !net.connected() || gateClosing() || mayapCloudTlsYieldRequested(millis())) break;
    mayapServiceBeat(MayapRecovery::Service::Mqtt);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  mayapSerialPrintf(false, "[MQTT] ws upgrade timeout/oversize (%u bytes)\n", static_cast<unsigned>(got));
  return false;
}

// TLS + WebSocket + MQTT handshake. The TLS admission lease is held for its whole duration (the
// working set is ~40 KiB); the socket then stays open. Returns false on any failure.
enum class Connect : uint8_t { Ok, Busy, Failed };
inline Connect connectClient() {
  using namespace MayapMqttWire;
  MayapTlsOperation tls(MayapTlsKind::Mqtt);
  if (!tls) return Connect::Busy;
  mayapSetMqttTlsResident(true);   // from here until stopClient()/a failed attempt the socket counts as THE TLS context
  buildIdentity();
  if (!netConfigured) {
    // Same transport settings as the V2 owner.
    net.setCACert(TLS_ROOT_CA);
    net.setConnectionTimeout(5000);
    net.setHandshakeTimeout(8);
    netConfigured = true;
  }
  const uint32_t startedAt = millis();
  const uint32_t heapBefore = ESP.getFreeHeap();
  linkFailed = false;
  carryLength = 0U;
  parser.reset();
  ws.reset();
  if (!net.connect(brokerHost(), brokerPort())) {
    char reason[64] = "";
    net.lastError(reason, sizeof(reason));
    mayapSerialPrintf(false, "[MQTT] tls connect failed host=%s:%u err=%s heap=%lu largest=%lu\n", brokerHost(),
                      static_cast<unsigned>(brokerPort()), reason, static_cast<unsigned long>(ESP.getFreeHeap()),
                      static_cast<unsigned long>(ESP.getMaxAllocHeap()));
    net.stop();
    return Connect::Failed;
  }
  if (!upgradeWebSocket()) { net.stop(); return Connect::Failed; }
  const ConnectArgs args{clientId, MayapRealtimeInternal::deviceId, mayapMqttKey(), willTopic,
                         reinterpret_cast<const uint8_t *>(WILL_MESSAGE), sizeof(WILL_MESSAGE) - 1U, KEEPALIVE_SEC};
  if (!sendPacket(encodeConnect(txMqtt(), PACKET_BUFFER, args)) || !awaitPacket(CONNACK) ||
      parser.bodyLength() != 2U || parser.body()[1] != 0U) {
    mayapSerialPrintf(false, "[MQTT] connect refused/timeout code=%d\n",
                      parser.ready() && parser.type() == CONNACK && parser.bodyLength() == 2U ? static_cast<int>(parser.body()[1]) : -1);
    net.stop();
    return Connect::Failed;
  }
  char topics[4][64];
  static constexpr struct { const char *channel; uint8_t qos; } SUBSCRIBE[4] = {
    {"command", 1}, {"config/set", 1}, {"history/request", 1}, {"session", 0},
  };
  Subscription subs[4];
  for (uint8_t i = 0U; i < 4U; ++i) {
    snprintf(topics[i], sizeof(topics[i]), "%s%s", prefix, SUBSCRIBE[i].channel);
    subs[i] = Subscription{topics[i], SUBSCRIBE[i].qos};
  }
  if (!sendPacket(encodeSubscribe(txMqtt(), PACKET_BUFFER, 1U, subs, 4U)) || !awaitPacket(SUBACK) ||
      parser.bodyLength() != 6U) {
    mayapSerialPrintf(false, "[MQTT] subscribe failed\n");
    net.stop();
    return Connect::Failed;
  }
  for (uint8_t i = 0U; i < 4U; ++i) {
    if (parser.body()[2U + i] > 1U) {  // 0x80: the broker refused it (ACL)
      mayapSerialPrintf(false, "[MQTT] subscribe refused (%u)\n", static_cast<unsigned>(i));
      net.stop();
      return Connect::Failed;
    }
  }
  parser.reset();
  inflightClear();
  connected = true;
  MayapUplink::onLinkUp();
  lastRxAt = lastTxAt = millis();
  mayapSerialPrintf(false, "[MQTT] wss+mqtt up %lums heap=%lu->%lu largest=%lu task=%s core=%d prio=%u\n",
                    static_cast<unsigned long>(millis() - startedAt), static_cast<unsigned long>(heapBefore),
                    static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMaxAllocHeap()),
                    pcTaskGetName(nullptr), static_cast<int>(xPortGetCoreID()),
                    static_cast<unsigned>(uxTaskPriorityGet(nullptr)));
  return Connect::Ok;
}

inline void onConnected() {
  using namespace MayapRealtimeInternal;
  backoff.onSuccess();
  publishPresence(true);
  portENTER_CRITICAL(&realtimeMux);
  if (knownConfigValid) configDirty = true;
  portEXIT_CRITICAL(&realtimeMux);
  lastSnapshotPublishAt = 0U;
  forceSnapshotPublish = true;
  mayapSerialPrintf(false, "[MQTT] connected %s\n", deviceId);
}

// Publishes what the Cloud task queued (alarms, heartbeat), oldest first. A slot that cannot be sent for a
// reason other than a dead socket is failed so it can never wedge the queue.
inline void pumpUplink(uint32_t now) {
  MayapUplink::expire(now);
  for (uint8_t guard = 0U; guard < MayapUplink::SLOTS; ++guard) {
    const int8_t slot = MayapUplink::nextQueued();
    if (slot < 0) return;
    uint16_t id = 0U;
    const bool alarm = MayapUplink::kind(slot) == MayapUplink::Kind::Alarm;
    if (publishChannel(alarm ? "alarm" : "heartbeat", MayapUplink::payload(slot), MayapUplink::length(slot), false, &id)) {
      MayapUplink::markSent(slot, id, now);
    } else {
      if (linkFailed) return;   // the watchdog below drops the link; onLinkDown() fails the slots
      MayapUplink::markFailed(slot);
    }
  }
}

inline bool linkAlive(uint32_t now) {
  if (linkFailed || parser.fatal() || ws.fatal() || !net.connected()) return false;
  if (inflightCount > 0U && MayapRecovery::age(now, inflightAt[0]) >= QOS1_STUCK_MS) return false;
  // Broker silent for 1.5 keepalives: half-open link.
  return MayapRecovery::age(now, lastRxAt) < static_cast<uint32_t>(KEEPALIVE_SEC) * 1500UL;
}

}  // namespace MayapMqttInternal

inline void mayapMqttTransportBegin() {
  MayapRealtimeInternal::publishCallback = MayapMqttInternal::publishFromBridge;
  MayapMqttInternal::backoff.reset(millis());
  mayapSerialPrintf(false, "[MQTT] owner task=%s core=%d prio=%u native WSS (TLS+WebSocket+MQTT), no esp-mqtt\n", pcTaskGetName(nullptr),
                    static_cast<int>(xPortGetCoreID()), static_cast<unsigned>(uxTaskPriorityGet(nullptr)));
  if (!MayapMqttInternal::configured())
    mayapSerialPrintf(false, "[MQTT] idle: waiting for the per-device credential from /api/device/register\n");
}

inline bool mayapMqttTransportConnected() { return MayapMqttInternal::connected; }

// Owner mqttTask only. Honors the shared radio gate, Cloud TLS yield and memory
// pressure; the mutable controller state is never touched from here.
inline void mayapMqttTransportUpdate(uint32_t now) {
  using namespace MayapMqttInternal;
  const bool closing = gateClosing();
  const bool yielding = mayapCloudTlsYieldRequested(now);
  const bool pressure = mayapOnlineMemoryPressure();
  if (closing || mayapServiceIsolated(MayapRecovery::Service::Mqtt, now) || pressure || yielding) {
    if (connected)
      mayapSerialPrintf(false, "[MQTT] closed on purpose (%s) heap=%lu\n", closing ? "radio" : pressure ? "memory" : yielding ? "cloud-tls" : "isolated",
                        static_cast<unsigned long>(ESP.getFreeHeap()));
    stopClient(true);
    if (!closing) backoff.holdOff(now, yielding && !pressure ? YIELD_RESUME_MS : RETRY_STEPS_MS[0]);
    if (ioEntered) { mayapOnlineIoLeave(MayapRecovery::Service::Mqtt); ioEntered = false; }
    // An idle poll is not a drain ACK: the socket above is already closed.
    if (closing) mayapOnlineOwnerQuiet(MayapRecovery::Service::Mqtt);
    return;
  }
  if (!configured()) return;
  const NetworkStatus status = mayapGetRawNetworkStatus();
  const bool staOnline = status.requestedMode == ConnectivityMode::Online && status.connected;
  if (!staOnline) {
    stopClient(false);
    backoff.reset(now);
    staUpSince = 0U;
    return;
  }
  if (staUpSince == 0U) staUpSince = now ? now : 1U;
  if (!ioEntered) {
    if (!mayapOnlineIoEnter(MayapRecovery::Service::Mqtt)) return;
    ioEntered = true;
  }
  if (!connected) {
    if (!backoff.ready(now) || !clockValid()) return;  // certificate dates need real time
    if (MayapRecovery::age(now, staUpSince) < STA_STABLE_MS) return;   // a flapping Wi-Fi gets no TLS handshakes
    Connect result = connectClient();
    if (result != Connect::Ok) mayapSetMqttTlsResident(false);   // every failure path already ran net.stop()
    if (result == Connect::Failed && mayapCloudTlsYieldRequested(millis())) {
      // Cloud asked for the heap while this handshake ran: contention, not a broker failure.
      mayapSerialPrintf(false, "[MQTT] connect aborted: Cloud needs the TLS lease, retry soon\n");
      result = Connect::Busy;
    }
    if (result == Connect::Busy) { backoff.holdOff(millis(), BUSY_RETRY_MS); return; }
    if (result == Connect::Failed) { backoff.onFailure(millis()); return; }
    onConnected();
    return;
  }
  if (!linkAlive(now) || !pump()) {
    mayapSerialPrintf(false, "[MQTT] link lost (tx=%u fatal=%u open=%u inflight=%u)\n", static_cast<unsigned>(linkFailed),
                      static_cast<unsigned>(parser.fatal()), static_cast<unsigned>(net.connected()),
                      static_cast<unsigned>(inflightCount));
    stopClient(false);
    backoff.onFailure(now);
    return;
  }
  pumpUplink(now);
  if (MayapRecovery::age(now, lastTxAt) >= static_cast<uint32_t>(KEEPALIVE_SEC) * 500UL)
    sendPacket(MayapMqttWire::encodePingreq(txMqtt(), PACKET_BUFFER));
  mayapRealtimeUpdate(millis());
#if MAYAP_DIAGNOSTIC_SERIAL
  if (lastDiagAt == 0U || MayapRecovery::age(now, lastDiagAt) >= 10000UL) {
    lastDiagAt = now;
    mayapSerialPrintf(false, "[MQTT] up inflight=%u dropBig=%lu dropForeign=%lu\n", static_cast<unsigned>(inflightCount),
                      static_cast<unsigned long>(droppedOversize), static_cast<unsigned long>(droppedForeign));
  }
#endif
}

inline void mayapMqttTransportRecover(uint32_t now) {
  MayapMqttInternal::stopClient(false);
  MayapMqttInternal::backoff.onFailure(now);
}
