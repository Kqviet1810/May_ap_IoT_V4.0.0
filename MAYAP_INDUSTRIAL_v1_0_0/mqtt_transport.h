#pragma once

#include "config.h"
#include "device_identity.h"
#include "network_io_guard.h"
#include "service_recovery.h"
#include "network_service.h"
#include "mqtt_wire.h"
#include "transaction_bridge.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <time.h>

// Native MQTT 3.1.1 over TLS (TCP, port 8883) to the broker. Contract:
// doc/MQTT_CONTRACT.md. Same shape as the V2 owner: ONE static WiFiClientSecure, touched
// only by mqttTask (static task pinned to core 0 by the sketch, same task as every Transaction
// V2 bridge call), bounded static buffers, no extra task, no heap churn per message.
// There is no WebSocket and no esp-mqtt: the protocol is mqtt_wire.h, host-tested against
// the broker's codec. The controller (core 1) never waits on any of this.
//
// Endpoint/credentials: nothing secret and nothing broker-specific is compiled in. The
// account Worker returns the broker host/port and this device's own password from
// /api/device/register; they live in NVS (device_identity.h). MAYAP_BROKER_HOST only
// provides a default for a private build. Until both exist the transport stays idle.
#ifndef MAYAP_BROKER_HOST
#define MAYAP_BROKER_HOST ""
#endif
#ifndef MAYAP_BROKER_PORT
#define MAYAP_BROKER_PORT 8883
#endif

namespace MayapMqttInternal {

constexpr char TOPIC_ROOT[] = "mayap/v1";
constexpr uint16_t KEEPALIVE_SEC = 30U;             // contract: 30..120
constexpr size_t PACKET_BUFFER = 2560U;             // topic + header + 2048 B payload
constexpr uint8_t QOS1_INFLIGHT_MAX = 4U;
constexpr uint32_t QOS1_STUCK_MS = 15000UL;
constexpr uint32_t STEP_TIMEOUT_MS = 5000UL;        // CONNACK / SUBACK wait
constexpr uint32_t CLOCK_VALID_AFTER = 1700000000UL; // TLS certificate dates need real time
constexpr uint8_t PUMP_PACKET_BUDGET = 8U;

static WiFiClientSecure net;                        // owner: mqttTask only
static uint8_t txBuffer[PACKET_BUFFER];
static uint8_t rxBuffer[PACKET_BUFFER];
static MayapMqttWire::StreamParser parser(rxBuffer, sizeof(rxBuffer));

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
static uint32_t lastRxAt = 0U, lastTxAt = 0U, lastDiagAt = 0U;
static uint32_t droppedOversize = 0U, droppedForeign = 0U;
static uint8_t carry[128];                          // bytes that followed a handshake packet
static size_t carryLength = 0U;
static BackoffTimer backoff{};

struct ChannelPolicy { const char *channel; uint8_t qos; bool retain; };
// Publish QoS/retain per doc/MQTT_CONTRACT.md section 2. The broker enforces the
// same table; an unknown channel is never sent.
static constexpr ChannelPolicy POLICY[] = {
  {"presence", 1, true}, {"snapshot", 0, false}, {"ack", 1, false}, {"log", 0, false},
  {"config/reported", 1, false}, {"history/reported", 1, false},
};

inline const char *brokerHost() {
  const char *provisioned = mayapMqttHost();
  return provisioned[0] ? provisioned : MAYAP_BROKER_HOST;
}
inline uint16_t brokerPort() {
  const uint16_t provisioned = mayapMqttPort();
  return provisioned ? provisioned : static_cast<uint16_t>(MAYAP_BROKER_PORT);
}
inline bool configured() { return brokerHost()[0] != '\0' && mayapMqttKey()[0] != '\0'; }
inline bool clockValid() { return static_cast<uint32_t>(time(nullptr)) > CLOCK_VALID_AFTER; }
inline bool gateClosing() { return mayapWifiPortalExclusiveRequested() || mayapRadioRecoveryRequested(); }

inline bool sendPacket(size_t length) {
  if (length == 0U) return false;
  if (net.write(txBuffer, length) != length) { linkFailed = true; return false; }
  lastTxAt = millis();
  return true;
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

inline bool publishFromBridge(const char *channel, const char *payload, size_t length) {
  if (!connected || !channel) return false;
  // `bootstrap` hints are not part of the V2 topic contract and have no ACL entry.
  if (!strcmp(channel, "bootstrap")) return true;
  const ChannelPolicy *policy = nullptr;
  for (const ChannelPolicy &candidate : POLICY)
    if (!strcmp(candidate.channel, channel)) { policy = &candidate; break; }
  if (!policy || length == 0U || length >= MayapProtocol::FRAME_NORMAL_CAP) return false;
  const uint32_t now = millis();
  if (policy->qos > 0U && inflightCount >= QOS1_INFLIGHT_MAX) return false;  // bounded; bridge retries next cycle
  char topic[64];
  const int n = snprintf(topic, sizeof(topic), "%s/%s/%s", TOPIC_ROOT, MayapRealtimeInternal::deviceId, channel);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(topic)) return false;
  const uint16_t id = policy->qos ? allocPacketId() : 0U;
  const size_t packet = MayapMqttWire::encodePublish(txBuffer, sizeof(txBuffer), topic,
      reinterpret_cast<const uint8_t *>(payload), length, policy->qos, policy->retain, id);
  if (!sendPacket(packet)) return false;
  if (policy->qos > 0U) inflightAdd(id, now);
  return true;
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
    if (MayapRealtimeInternal::publishPresence(false))
      sendPacket(MayapMqttWire::encodeDisconnect(txBuffer, sizeof(txBuffer)));
  }
  net.stop();
  connected = false;
  linkFailed = false;
  carryLength = 0U;
  inflightClear();
  parser.reset();
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
      if (view.qos == 1U) sendPacket(encodePuback(txBuffer, sizeof(txBuffer), view.packetId));
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
      if (parser.bodyLength() == 2U) inflightAck(static_cast<uint16_t>((parser.body()[0] << 8U) | parser.body()[1]));
      return;
    default:
      return;  // PINGRESP and anything unexpected only prove the link is alive
  }
}

// Reads what is available without blocking and parses at most PUMP_PACKET_BUDGET packets.
// Returns false when the link must be dropped. `wantType` != 0 makes it return true as soon
// as a packet of that type is ready (handshake); other packets are ignored then, and any
// bytes read after the wanted packet are kept in `carry` for the next call.
inline bool pump(uint8_t wantType = 0U) {
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
      used += parser.feed(chunk + used, count - used);
      if (parser.fatal()) return false;
      if (!parser.ready()) continue;
      ++packets;
      if (wantType != 0U) {
        if (parser.type() != wantType) continue;
        carryLength = count - used;                 // caller reads parser.body() before the next feed
        if (carryLength) memcpy(carry, chunk + used, carryLength);
        return true;
      }
      handlePacket();
      if (linkFailed) return false;
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

// TLS + MQTT handshake. The TLS admission lease is held for its whole duration (the
// working set is ~40 KiB); the socket then stays open. Returns false on any failure.
inline bool connectClient() {
  using namespace MayapMqttWire;
  MayapTlsOperation tls(MayapTlsKind::Mqtt);
  if (!tls) return false;
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
  if (!net.connect(brokerHost(), brokerPort())) {
    char reason[64] = "";
    net.lastError(reason, sizeof(reason));
    mayapSerialPrintf(false, "[MQTT] tls connect failed host=%s:%u err=%s heap=%lu largest=%lu\n", brokerHost(),
                      static_cast<unsigned>(brokerPort()), reason, static_cast<unsigned long>(ESP.getFreeHeap()),
                      static_cast<unsigned long>(ESP.getMaxAllocHeap()));
    net.stop();
    return false;
  }
  const ConnectArgs args{clientId, MayapRealtimeInternal::deviceId, mayapMqttKey(), willTopic,
                         reinterpret_cast<const uint8_t *>(WILL_MESSAGE), sizeof(WILL_MESSAGE) - 1U, KEEPALIVE_SEC};
  if (!sendPacket(encodeConnect(txBuffer, sizeof(txBuffer), args)) || !awaitPacket(CONNACK) ||
      parser.bodyLength() != 2U || parser.body()[1] != 0U) {
    mayapSerialPrintf(false, "[MQTT] connect refused/timeout code=%d\n",
                      parser.ready() && parser.type() == CONNACK && parser.bodyLength() == 2U ? static_cast<int>(parser.body()[1]) : -1);
    net.stop();
    return false;
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
  if (!sendPacket(encodeSubscribe(txBuffer, sizeof(txBuffer), 1U, subs, 4U)) || !awaitPacket(SUBACK) ||
      parser.bodyLength() != 6U) {
    mayapSerialPrintf(false, "[MQTT] subscribe failed\n");
    net.stop();
    return false;
  }
  for (uint8_t i = 0U; i < 4U; ++i) {
    if (parser.body()[2U + i] > 1U) {  // 0x80: the broker refused it (ACL)
      mayapSerialPrintf(false, "[MQTT] subscribe refused (%u)\n", static_cast<unsigned>(i));
      net.stop();
      return false;
    }
  }
  parser.reset();
  inflightClear();
  connected = true;
  lastRxAt = lastTxAt = millis();
  mayapSerialPrintf(false, "[MQTT] tls+mqtt up %lums heap=%lu->%lu largest=%lu task=%s core=%d prio=%u\n",
                    static_cast<unsigned long>(millis() - startedAt), static_cast<unsigned long>(heapBefore),
                    static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMaxAllocHeap()),
                    pcTaskGetName(nullptr), static_cast<int>(xPortGetCoreID()),
                    static_cast<unsigned>(uxTaskPriorityGet(nullptr)));
  return true;
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

inline bool linkAlive(uint32_t now) {
  if (linkFailed || parser.fatal() || !net.connected()) return false;
  if (inflightCount > 0U && MayapRecovery::age(now, inflightAt[0]) >= QOS1_STUCK_MS) return false;
  // Broker silent for 1.5 keepalives: half-open link.
  return MayapRecovery::age(now, lastRxAt) < static_cast<uint32_t>(KEEPALIVE_SEC) * 1500UL;
}

}  // namespace MayapMqttInternal

inline void mayapMqttTransportBegin() {
  MayapRealtimeInternal::publishCallback = MayapMqttInternal::publishFromBridge;
  MayapMqttInternal::backoff.reset(millis());
  mayapSerialPrintf(false, "[MQTT] owner task=%s core=%d prio=%u native TLS, no esp-mqtt\n", pcTaskGetName(nullptr),
                    static_cast<int>(xPortGetCoreID()), static_cast<unsigned>(uxTaskPriorityGet(nullptr)));
  if (!MayapMqttInternal::configured())
    mayapSerialPrintf(false, "[MQTT] idle: waiting for broker host + per-device credential from /api/device/register\n");
}

inline bool mayapMqttTransportConnected() { return MayapMqttInternal::connected; }

// Owner mqttTask only. Honors the shared radio gate, Cloud TLS yield and memory
// pressure; the mutable controller state is never touched from here.
inline void mayapMqttTransportUpdate(uint32_t now) {
  using namespace MayapMqttInternal;
  const bool closing = gateClosing();
  if (closing || mayapServiceIsolated(MayapRecovery::Service::Mqtt, now) ||
      mayapOnlineMemoryPressure() || mayapCloudTlsYieldRequested(now)) {
    stopClient(true);
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
    return;
  }
  if (!ioEntered) {
    if (!mayapOnlineIoEnter(MayapRecovery::Service::Mqtt)) return;
    ioEntered = true;
  }
  if (!connected) {
    if (!backoff.ready(now) || !clockValid()) return;  // certificate dates need real time
    if (!connectClient()) { backoff.onFailure(millis()); return; }
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
  if (MayapRecovery::age(now, lastTxAt) >= static_cast<uint32_t>(KEEPALIVE_SEC) * 500UL)
    sendPacket(MayapMqttWire::encodePingreq(txBuffer, sizeof(txBuffer)));
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
