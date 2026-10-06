#pragma once

#include "config.h"
#include "network_io_guard.h"
#include "service_recovery.h"
#include "network_service.h"
#include "transaction_bridge.h"
#include <Arduino.h>
#include <mqtt_client.h>

// MQTT 3.1.1 over WSS to the broker, using the ESP-IDF esp-mqtt client (standard
// WebSocket transport, TLS verified against TLS_ROOT_CA). Contract:
// doc/MQTT_CONTRACT.md. This file is a transport only: request ids, signatures,
// replay protection and APPLIED/REJECTED stay in transaction_bridge.h, and the
// controller never depends on this link (Online services may stop at any time).
//
// Threading: esp-mqtt runs its own task. Its event handler only copies bounded
// inbound packets into a 2-slot ring and sets flags; every bridge call happens in
// mqttTask (the single owner), exactly like the previous transport.
//
// Credentials: MAYAP_BROKER_HOST and MAYAP_BROKER_FIXTURE_PASSWORD default to
// empty, which keeps the transport disabled. The fixture password is a MVP test
// credential supplied at build time (-D...), never tracked; production auth
// replaces it without changing the topic contract.

#ifndef MAYAP_BROKER_HOST
#define MAYAP_BROKER_HOST ""
#endif
#ifndef MAYAP_BROKER_PORT
#define MAYAP_BROKER_PORT 443
#endif
#ifndef MAYAP_BROKER_FIXTURE_PASSWORD
#define MAYAP_BROKER_FIXTURE_PASSWORD ""
#endif

namespace MayapMqttInternal {

constexpr char BROKER_HOST[] = MAYAP_BROKER_HOST;
constexpr uint16_t BROKER_PORT = MAYAP_BROKER_PORT;
constexpr char BROKER_PASSWORD[] = MAYAP_BROKER_FIXTURE_PASSWORD;
constexpr char TOPIC_ROOT[] = "mayap/v1";
constexpr uint16_t KEEPALIVE_SEC = 30U;            // contract: 30..120
constexpr int NETWORK_TIMEOUT_MS = 8000;
constexpr uint32_t CONNECT_DEADLINE_MS = 15000UL;
constexpr size_t PACKET_BUFFER = 2560U;            // topic + header + 2048 B payload
constexpr uint8_t INBOUND_SLOTS = 2U;
constexpr uint8_t QOS1_INFLIGHT_MAX = 4U;
constexpr uint32_t QOS1_STUCK_MS = 15000UL;
constexpr int ESP_MQTT_TASK_STACK = 6144;
constexpr UBaseType_t ESP_MQTT_TASK_PRIORITY = 2U;

struct Inbound {
  uint16_t length = 0U;
  char channel[20] = "";
  uint8_t data[MayapProtocol::FRAME_NORMAL_CAP];
};
static Inbound inbound[INBOUND_SLOTS];
static uint32_t inboundHead = 0U, inboundTail = 0U;  // producer: esp-mqtt, consumer: mqttTask

static esp_mqtt_client_handle_t client = nullptr;
static char uri[128] = "";
static char clientId[32] = "";
static char prefix[40] = "";
static size_t prefixLength = 0U;
static char willTopic[64] = "";
static constexpr char WILL_MESSAGE[] = "{\"online\":false}";

static volatile uint8_t evConnected = 0U, evDisconnected = 0U, evSubscribeRejected = 0U;
static volatile uint8_t qos1Inflight = 0U;
static volatile uint32_t droppedInbound = 0U, droppedOversize = 0U;
static bool connected = false, ioEntered = false;
static uint32_t qos1StuckSince = 0U, lastDiagAt = 0U;
static BackoffTimer backoff{};

struct ChannelPolicy { const char *channel; uint8_t qos; bool retain; };
// Publish QoS/retain per doc/MQTT_CONTRACT.md section 2. The broker enforces the
// same table; an unknown channel is never sent.
static constexpr ChannelPolicy POLICY[] = {
  {"presence", 1, true}, {"snapshot", 0, false}, {"ack", 1, false}, {"log", 0, false},
  {"config/reported", 1, false}, {"history/reported", 1, false},
};

inline bool configured() { return BROKER_HOST[0] != '\0' && BROKER_PASSWORD[0] != '\0'; }

inline void eventHandler(void *, esp_event_base_t, int32_t id, void *data) {
  auto *event = static_cast<esp_mqtt_event_handle_t>(data);
  switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED:
      __atomic_store_n(&evConnected, 1U, __ATOMIC_RELEASE);
      break;
    case MQTT_EVENT_DISCONNECTED:
      __atomic_store_n(&evDisconnected, 1U, __ATOMIC_RELEASE);
      break;
    case MQTT_EVENT_SUBSCRIBED:
      // SUBACK return code 0x80 means the broker refused the subscription (ACL).
      if (event->data && event->data_len > 0 && static_cast<uint8_t>(event->data[0]) == 0x80U)
        __atomic_store_n(&evSubscribeRejected, 1U, __ATOMIC_RELEASE);
      break;
    case MQTT_EVENT_PUBLISHED:
      if (__atomic_load_n(&qos1Inflight, __ATOMIC_ACQUIRE) > 0U)
        __atomic_fetch_sub(&qos1Inflight, 1U, __ATOMIC_ACQ_REL);
      break;
    case MQTT_EVENT_DATA: {
      // Whole packets only: a fragmented or oversized publish is dropped and the
      // sender's retry/UNCERTAIN path handles it (Transaction V2).
      if (event->current_data_offset != 0 || event->total_data_len != event->data_len ||
          event->data_len < 0 || static_cast<size_t>(event->data_len) > sizeof(Inbound::data) ||
          event->topic_len <= static_cast<int>(prefixLength) ||
          memcmp(event->topic, prefix, prefixLength) != 0) {
        __atomic_fetch_add(&droppedOversize, 1U, __ATOMIC_RELAXED);
        break;
      }
      const size_t channelLength = static_cast<size_t>(event->topic_len) - prefixLength;
      const uint32_t head = __atomic_load_n(&inboundHead, __ATOMIC_RELAXED);
      if (channelLength >= sizeof(Inbound::channel) ||
          head - __atomic_load_n(&inboundTail, __ATOMIC_ACQUIRE) >= INBOUND_SLOTS) {
        __atomic_fetch_add(&droppedInbound, 1U, __ATOMIC_RELAXED);
        break;
      }
      Inbound &slot = inbound[head % INBOUND_SLOTS];
      memcpy(slot.channel, event->topic + prefixLength, channelLength);
      slot.channel[channelLength] = '\0';
      memcpy(slot.data, event->data, static_cast<size_t>(event->data_len));
      slot.length = static_cast<uint16_t>(event->data_len);
      __atomic_store_n(&inboundHead, head + 1U, __ATOMIC_RELEASE);
      break;
    }
    default:
      break;
  }
}

inline bool publishFromBridge(const char *channel, const char *payload, size_t length) {
  if (!connected || !client || !channel) return false;
  // `bootstrap` hints are not part of the V2 topic contract and have no ACL entry.
  if (!strcmp(channel, "bootstrap")) return true;
  const ChannelPolicy *policy = nullptr;
  for (const ChannelPolicy &candidate : POLICY)
    if (!strcmp(candidate.channel, channel)) { policy = &candidate; break; }
  if (!policy || length == 0U || length >= MayapProtocol::FRAME_NORMAL_CAP) return false;
  const uint32_t now = millis();
  if (policy->qos > 0U) {
    if (__atomic_load_n(&qos1Inflight, __ATOMIC_ACQUIRE) >= QOS1_INFLIGHT_MAX) {
      if (qos1StuckSince == 0U) qos1StuckSince = now;
      return false;  // bounded: the bridge retries on the next owner cycle
    }
    qos1StuckSince = 0U;
  }
  char topic[64];
  const int n = snprintf(topic, sizeof(topic), "%s/%s/%s", TOPIC_ROOT,
                         MayapRealtimeInternal::deviceId, channel);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(topic)) return false;
  const int id = esp_mqtt_client_publish(client, topic, payload, static_cast<int>(length),
                                         policy->qos, policy->retain ? 1 : 0);
  if (id < 0) return false;
  if (policy->qos > 0U) __atomic_fetch_add(&qos1Inflight, 1U, __ATOMIC_ACQ_REL);
  return true;
}

inline void buildIdentity() {
  snprintf(clientId, sizeof(clientId), "esp-%s", MayapRealtimeInternal::deviceId);
  snprintf(prefix, sizeof(prefix), "%s/%s/", TOPIC_ROOT, MayapRealtimeInternal::deviceId);
  prefixLength = strlen(prefix);
  snprintf(willTopic, sizeof(willTopic), "%spresence", prefix);
  if (BROKER_PORT == 443U)
    snprintf(uri, sizeof(uri), "wss://%s/mqtt/%s", BROKER_HOST, MayapRealtimeInternal::deviceId);
  else
    snprintf(uri, sizeof(uri), "wss://%s:%u/mqtt/%s", BROKER_HOST,
             static_cast<unsigned>(BROKER_PORT), MayapRealtimeInternal::deviceId);
}

// Owner task only. Releases the TLS working set so Cloud/OTA can use the heap.
inline void stopClient(bool graceful) {
  if (!client) { connected = false; return; }
  if (graceful && connected) {
    MayapRealtimeInternal::publishPresence(false);  // clean DISCONNECT suppresses the LWT
    esp_mqtt_client_disconnect(client);
    vTaskDelay(pdMS_TO_TICKS(150));
  }
  esp_mqtt_client_stop(client);
  esp_mqtt_client_destroy(client);
  client = nullptr;
  connected = false;
  __atomic_store_n(&qos1Inflight, 0U, __ATOMIC_RELEASE);
  qos1StuckSince = 0U;
}

inline bool gateClosing() {
  return mayapWifiPortalExclusiveRequested() || mayapRadioRecoveryRequested();
}

// Starts the client and waits (beating the supervisor) until the TLS+MQTT
// handshake resolves. The TLS admission lease is held for the whole handshake.
inline bool connectClient(uint32_t now) {
  MayapTlsOperation tls(MayapTlsKind::Mqtt);
  if (!tls) return false;
  buildIdentity();
  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = uri;
  cfg.broker.verification.certificate = TLS_ROOT_CA;
  cfg.credentials.username = MayapRealtimeInternal::deviceId;
  cfg.credentials.client_id = clientId;
  cfg.credentials.authentication.password = BROKER_PASSWORD;
  cfg.session.keepalive = KEEPALIVE_SEC;
  cfg.session.disable_clean_session = false;
  cfg.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
  cfg.session.last_will.topic = willTopic;
  cfg.session.last_will.msg = WILL_MESSAGE;
  cfg.session.last_will.msg_len = static_cast<int>(sizeof(WILL_MESSAGE) - 1U);
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.network.disable_auto_reconnect = true;  // reconnects go through admission + backoff
  cfg.network.timeout_ms = NETWORK_TIMEOUT_MS;
  cfg.buffer.size = static_cast<int>(PACKET_BUFFER);
  cfg.buffer.out_size = static_cast<int>(PACKET_BUFFER);
  cfg.task.priority = ESP_MQTT_TASK_PRIORITY;
  cfg.task.stack_size = ESP_MQTT_TASK_STACK;

  __atomic_store_n(&evConnected, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&evDisconnected, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&evSubscribeRejected, 0U, __ATOMIC_RELEASE);
  client = esp_mqtt_client_init(&cfg);
  if (!client) return false;
  esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, eventHandler, nullptr);
  if (esp_mqtt_client_start(client) != ESP_OK) { stopClient(false); return false; }

  const uint32_t startedAt = millis();
  while (!__atomic_load_n(&evConnected, __ATOMIC_ACQUIRE)) {
    if (__atomic_load_n(&evDisconnected, __ATOMIC_ACQUIRE) ||
        MayapRecovery::age(millis(), startedAt) >= CONNECT_DEADLINE_MS ||
        gateClosing() || mayapCloudTlsYieldRequested(millis())) {
      stopClient(false);
      return false;
    }
    mayapServiceBeat(MayapRecovery::Service::Mqtt);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  (void)now;
  return true;
}

inline void onConnected() {
  using namespace MayapRealtimeInternal;
  connected = true;
  backoff.onSuccess();
  // Clean session: every connect re-subscribes. QoS follows the contract.
  static constexpr struct { const char *channel; int qos; } SUBSCRIBE[] = {
    {"command", 1}, {"config/set", 1}, {"history/request", 1}, {"session", 0},
  };
  char topic[64];
  bool ok = true;
  for (const auto &subscription : SUBSCRIBE) {
    snprintf(topic, sizeof(topic), "%s%s", prefix, subscription.channel);
    ok = esp_mqtt_client_subscribe_single(client, topic, subscription.qos) >= 0 && ok;
  }
  if (!ok) { stopClient(false); backoff.onFailure(millis()); return; }
  publishPresence(true);
  portENTER_CRITICAL(&realtimeMux);
  if (knownConfigValid) configDirty = true;
  portEXIT_CRITICAL(&realtimeMux);
  lastSnapshotPublishAt = 0U;
  forceSnapshotPublish = true;
  mayapSerialPrintf(false, "[MQTT] connected %s\n", deviceId);
}

inline void drainInbound() {
  for (;;) {
    const uint32_t tail = __atomic_load_n(&inboundTail, __ATOMIC_RELAXED);
    if (tail == __atomic_load_n(&inboundHead, __ATOMIC_ACQUIRE)) return;
    Inbound &slot = inbound[tail % INBOUND_SLOTS];
    MayapRealtimeInternal::dispatchApplicationMessage(slot.channel, slot.data, slot.length);
    __atomic_store_n(&inboundTail, tail + 1U, __ATOMIC_RELEASE);
    mayapServiceBeat(MayapRecovery::Service::Mqtt);
  }
}

}  // namespace MayapMqttInternal

inline void mayapMqttTransportBegin() {
  MayapRealtimeInternal::publishCallback = MayapMqttInternal::publishFromBridge;
  MayapMqttInternal::backoff.reset(millis());
  if (!MayapMqttInternal::configured())
    mayapSerialPrintf(false,
        "[MQTT] disabled: define MAYAP_BROKER_HOST and MAYAP_BROKER_FIXTURE_PASSWORD at build time\n");
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
  if (!client) {
    if (!backoff.ready(now)) return;
    if (!connectClient(now)) { backoff.onFailure(millis()); return; }
    onConnected();
    return;
  }
  if (__atomic_load_n(&evDisconnected, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&evSubscribeRejected, __ATOMIC_ACQUIRE) ||
      (qos1StuckSince != 0U && MayapRecovery::age(now, qos1StuckSince) >= QOS1_STUCK_MS)) {
    mayapSerialPrintf(false, "[MQTT] link lost (disc=%u rejected=%u)\n",
                      static_cast<unsigned>(evDisconnected), static_cast<unsigned>(evSubscribeRejected));
    stopClient(false);
    backoff.onFailure(now);
    return;
  }
  drainInbound();
  mayapRealtimeUpdate(millis());
#if MAYAP_DIAGNOSTIC_SERIAL
  if (lastDiagAt == 0U || MayapRecovery::age(now, lastDiagAt) >= 10000UL) {
    lastDiagAt = now;
    mayapSerialPrintf(false, "[MQTT] up inflight=%u dropFull=%lu dropBig=%lu\n",
                      static_cast<unsigned>(qos1Inflight),
                      static_cast<unsigned long>(droppedInbound),
                      static_cast<unsigned long>(droppedOversize));
  }
#endif
}

inline void mayapMqttTransportRecover(uint32_t now) {
  MayapMqttInternal::stopClient(false);
  MayapMqttInternal::backoff.onFailure(now);
}
