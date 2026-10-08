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
#include <sys/select.h>
#include <sys/time.h>
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
// Terminal command ACKs (channel "ack") are what the Web waits for; config/history reports and presence are bulk. Bulk
// publishers may never take the last QOS1_RESERVED_FOR_ACK window slots, so a config/history burst cannot starve an ACK.
constexpr uint8_t QOS1_RESERVED_FOR_ACK = 3U;
constexpr uint32_t QOS1_STUCK_MS = 15000UL;         // oldest in-flight packet without PUBACK for this long ...
constexpr uint32_t STUCK_SILENCE_MS = 10000UL;      // ... AND nothing at all received for this long = dead link; otherwise the entry just expires
constexpr uint32_t STEP_TIMEOUT_MS = 8000UL;        // WebSocket upgrade / CONNACK / SUBACK wait (a 1.3 s round trip was measured on a bad evening)
// A socket that cannot take bytes (TCP send window full: the path is stalled, not the broker) is NOT a dead link. The owner never
// waits inside net.write(): arduino-esp32 retries a stalled write for its whole socket timeout and then CLOSES the socket
// ("Closing connection on failed write"), which turned every 5 s hiccup into a 20-100 s outage. Frames are only written when
// select() says the socket is writable; otherwise Control frames wait in a small queue, Reliable frames are refused (the bridge /
// uplink retry on their own schedule) and Droppable frames (periodic snapshot / log) are dropped.
constexpr uint32_t TX_STALL_MS = 25000UL;           // nothing could be written for this long -> the uplink is really dead
constexpr uint32_t TX_STALL_SILENCE_MS = 10000UL;   // ... and the broker has been silent this long as well
constexpr uint8_t CTRL_QUEUE = 6U;                  // PUBACK / PINGREQ / PONG waiting for a writable socket
constexpr size_t CTRL_FRAME_MAX = 96U;              // finished (masked) WebSocket frame
// "received" acknowledgements are advisory (the terminal ack still follows): they are shed first when the QoS1 window is nearly full.
constexpr uint8_t ACK_ADVISORY_SHED_AT = 6U;
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
static uint32_t qos1Expired = 0U, refusedBulk = 0U, refusedAck = 0U;
static uint16_t brokerCloseCode = 0U;               // WebSocket CLOSE the broker sent (0 = none seen on this link)
static char brokerCloseReason[24] = "";
// Why the MQTT owner closed or lost its socket, counted for the whole uptime (printed with the 10 s diagnostic line).
enum class CloseKind : uint8_t { Radio, Memory, CloudTls, Isolated, Lost, COUNT };
static uint32_t closeCount[static_cast<uint8_t>(CloseKind::COUNT)] = {};
static uint8_t carry[64];                          // raw bytes that followed a handshake packet
static size_t carryLength = 0U;
// Reconnect ladder ("equal jitter": half..full of a doubling cap; AWS/Google guidance for MQTT/gRPC clients). The first retry is
// 0.5-1 s after a loss (the Web shows "reconnecting", the TLS working set is free again), then 1-2, 2-4, 4-8, 7.5-15 s, and only
// a broker that stayed unreachable for minutes climbs to 30 and 60 s. The old fixed 15 s first step plus a TLS handshake of
// 3-10 s was the entire outage after a 5 s write stall. The ladder resets only after the link stayed up STABLE_UP_MS, so a link
// that connects and dies at once keeps escalating instead of hammering the broker.
constexpr uint32_t RETRY_CAP_MS[] = {1000UL, 2000UL, 4000UL, 8000UL, 15000UL, 30000UL, 60000UL};
constexpr uint8_t RETRY_STEP_COUNT = sizeof(RETRY_CAP_MS) / sizeof(RETRY_CAP_MS[0]);
constexpr uint32_t STABLE_UP_MS = 30000UL;
constexpr uint32_t STA_STABLE_MS = 3000UL;          // Wi-Fi must have been up this long before a TLS handshake is tried (flap guard)
constexpr uint32_t YIELD_RESUME_MS = 3000UL;        // after Cloud released the lease: reconnect quickly (the Web shows "reconnecting" meanwhile)
constexpr uint32_t BUSY_RETRY_MS = 3000UL;          // TLS lease/heap busy is contention, not a broker failure: no backoff escalation
constexpr uint32_t HOLD_OFF_MS = 5000UL;            // deliberate close (memory pressure, isolation): short hold-off, not a failure
struct Retry {
  uint8_t step = 0U;
  uint32_t nextAttemptAt = 0U;
  unsigned failures = 0U;
  void reset(uint32_t now) { step = 0U; nextAttemptAt = now; }
  bool ready(uint32_t now) const { return static_cast<int32_t>(now - nextAttemptAt) >= 0; }
  void onFailure(uint32_t now) {
    const uint32_t cap = RETRY_CAP_MS[step];
    nextAttemptAt = now + cap / 2U + esp_random() % (cap / 2U + 1U);
    if (step + 1U < RETRY_STEP_COUNT) ++step;
    ++failures;
  }
  void onStable() { step = 0U; }
  // A deliberate stop (Cloud needs the heap/lease) is not a failure, but the next handshake waits too.
  void holdOff(uint32_t now, uint32_t ms = HOLD_OFF_MS) {
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

// What a frame is worth when the socket cannot take bytes right now (see the TX_STALL_MS comment above).
enum class Tx : uint8_t { Control, Reliable, Droppable };

static uint8_t ctrlQ[CTRL_QUEUE][CTRL_FRAME_MAX];
static uint8_t ctrlLen[CTRL_QUEUE] = {};
static uint8_t ctrlCount = 0U;
static bool txBlocked = false;                      // the last Reliable/Droppable frame was refused: socket not writable
static bool gateRefused = false;                    // the LAST sendFrame() was refused by the writability gate (not a link failure)
static uint32_t stallSince = 0U;                    // pending data and an unwritable socket since (0 = not stalled)
static uint32_t txRefused = 0U, txDropped = 0U, txCtrlQueued = 0U, advisoryShed = 0U;
static uint32_t writeMaxMs = 0U, loopMaxMs = 0U, heapLowWater = 0xFFFFFFFFUL;   // per diagnostic window
static const char *lostReason = "";

// Can the socket take bytes without waiting? On the device this is select() on the TLS socket's fd (the approach arduino-esp32
// maintainers recommend for socket timeouts, issue #5398): lwIP reports writable only when more than half of the TCP send buffer
// is free, so a frame of up to ~2.8 KB then fits entirely and mbedtls_ssl_write() returns at once. The host test fake overrides it.
template <typename C> inline auto txReadyProbe(C &client, int) -> decltype(client.hostTxReady()) { return client.hostTxReady(); }
template <typename C> inline bool txReadyProbe(C &client, long) {
  const int fd = client.fd();
  if (fd < 0) return true;                          // no descriptor: let the write itself report the problem
  fd_set writeSet;
  FD_ZERO(&writeSet);
  FD_SET(fd, &writeSet);
  struct timeval none;
  none.tv_sec = 0;
  none.tv_usec = 0;
  return select(fd + 1, nullptr, &writeSet, nullptr, &none) != 0;   // error (<0): the write reports it
}
inline bool txReady() { return txReadyProbe(net, 0); }

inline bool writeWhole(const uint8_t *data, size_t length) {
  const uint32_t startedAt = millis();
  const size_t wrote = net.write(data, length);
  const uint32_t took = MayapRecovery::age(millis(), startedAt);
  if (took > writeMaxMs) writeMaxMs = took;
  if (wrote != length) { linkFailed = true; lostReason = "tx-fail"; return false; }
  lastTxAt = millis();
  return true;
}
// Sends the Control frames that had to wait. Stops at the first unwritable moment; false only when a write really failed.
inline bool flushControl() {
  while (ctrlCount > 0U && txReady()) {
    if (!writeWhole(ctrlQ[0], ctrlLen[0])) return false;
    for (uint8_t i = 0U; i + 1U < ctrlCount; ++i) { memcpy(ctrlQ[i], ctrlQ[i + 1U], ctrlLen[i + 1U]); ctrlLen[i] = ctrlLen[i + 1U]; }
    --ctrlCount;
  }
  return true;
}

// Sends one MQTT packet that was encoded at txMqtt(): the WebSocket header is built in front of it
// and the payload is masked in place (RFC 6455: client frames are always masked).
inline bool sendFrame(size_t length, uint8_t opcode, Tx cls = Tx::Reliable) {
  size_t total = 0U;
  uint8_t *frame = MayapMqttWs::wrapClientFrame(txBuffer, TX_HEADROOM, length, opcode, esp_random(), &total);
  if (!frame) { linkFailed = true; lostReason = "frame"; return false; }
  gateRefused = false;
  if (!flushControl()) return false;
  if (ctrlCount > 0U || !txReady()) {
    if (cls == Tx::Control) {
      if (stallSince == 0U) stallSince = millis() ? millis() : 1U;
      lastTxAt = millis();                          // a queued keepalive counts as sent: one PINGREQ per keepalive window, not one per pass
      if (total > CTRL_FRAME_MAX || ctrlCount >= CTRL_QUEUE) {
        ++txDropped;                                // a lost PUBACK only makes the broker's own sweep drop the entry; it never breaks the link
        return true;
      }
      memcpy(ctrlQ[ctrlCount], frame, total);
      ctrlLen[ctrlCount++] = static_cast<uint8_t>(total);
      ++txCtrlQueued;
      return true;                                  // delivered later, in order, as soon as the socket is writable
    }
    gateRefused = true;
    txBlocked = true;
    if (stallSince == 0U) stallSince = millis() ? millis() : 1U;
    if (cls == Tx::Droppable) ++txDropped; else ++txRefused;
    return false;
  }
  if (!writeWhole(frame, total)) return false;
  txBlocked = false;
  return true;
}
inline bool sendPacket(size_t length, Tx cls = Tx::Reliable) {
  if (length == 0U) return false;
  return sendFrame(length, MayapMqttWs::BINARY, cls);
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
// Drops in-flight entries older than QOS1_STUCK_MS on a link that is demonstrably alive (it is still delivering bytes to us):
// the PUBACK is not coming, but that is not the socket's fault and closing a talking socket would only cost the Web its realtime.
inline void inflightExpire(uint32_t now) {
  while (inflightCount > 0U && MayapRecovery::age(now, inflightAt[0]) >= QOS1_STUCK_MS) {
    for (uint8_t j = 0U; j + 1U < inflightCount; ++j) { inflightId[j] = inflightId[j + 1U]; inflightAt[j] = inflightAt[j + 1U]; }
    --inflightCount;
    ++qos1Expired;
  }
}
inline uint16_t allocPacketId() {
  if (++nextPacketId == 0U) nextPacketId = 1U;
  return nextPacketId;
}

inline bool containsBounded(const char *text, size_t length, const char *needle) {
  const size_t n = strlen(needle);
  if (n == 0U || length < n) return false;
  for (size_t i = 0U; i + n <= length; ++i)
    if (memcmp(text + i, needle, n) == 0) return true;
  return false;
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
  if (track && policy->qos > 0U) {
    const bool terminalAck = !strcmp(channel, "ack");
    // "received" is advisory (the terminal ack of the same request follows): when the window is nearly full it is shed, so a burst
    // of quick commands (light toggled several times a second) keeps room for the acks the Web actually waits for.
    if (terminalAck && inflightCount >= ACK_ADVISORY_SHED_AT && containsBounded(payload, length, "\"phase\":\"received\"")) {
      ++advisoryShed;
      return true;
    }
    const uint8_t budget = terminalAck ? QOS1_INFLIGHT_MAX : static_cast<uint8_t>(QOS1_INFLIGHT_MAX - QOS1_RESERVED_FOR_ACK);
    if (inflightCount >= budget) {                      // bounded; the bridge retries with back-off, ACKs keep their reserve
      if (terminalAck) ++refusedAck; else ++refusedBulk;
      return false;
    }
  }
  char topic[64];
  const int n = snprintf(topic, sizeof(topic), "%s/%s/%s", TOPIC_ROOT, MayapRealtimeInternal::deviceId, channel);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(topic)) return false;
  const uint16_t id = policy->qos ? allocPacketId() : 0U;
  const size_t packet = MayapMqttWire::encodePublish(txMqtt(), PACKET_BUFFER, topic,
      reinterpret_cast<const uint8_t *>(payload), length, policy->qos, policy->retain, id);
  // Snapshots and logs are periodic and QoS0: the next one replaces a lost one, so they never wait for a stalled socket.
  const Tx cls = (policy->qos == 0U) ? Tx::Droppable : Tx::Reliable;
  if (!sendPacket(packet, cls)) return false;
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
        sendPacket(MayapMqttWire::encodeDisconnect(txMqtt(), PACKET_BUFFER), Tx::Droppable)) {
      txMqtt()[0] = 0x03U; txMqtt()[1] = 0xE8U;                // WebSocket CLOSE, status 1000
      sendFrame(2U, MayapMqttWs::CLOSE, Tx::Droppable);
    }
  }
  net.stop();
  mayapSetMqttTlsResident(false);   // memory is back before any HTTPS session may start
  connected = false;
  MayapUplink::onLinkDown();
  linkFailed = false;
  carryLength = 0U;
  ctrlCount = 0U;
  txBlocked = false;
  gateRefused = false;
  stallSince = 0U;
  inflightClear();
  parser.reset();
  ws.reset();
  brokerCloseCode = 0U;
  brokerCloseReason[0] = '\0';
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
      if (view.qos == 1U) sendPacket(encodePuback(txMqtt(), PACKET_BUFFER, view.packetId), Tx::Control);
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
        MayapUplink::onPuback(id, millis());
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
      return sendFrame(ws.controlLength(), PONG, Tx::Control);
    case CLOSE: {
      const uint8_t *body = ws.control();
      const size_t length = ws.controlLength();
      brokerCloseCode = length >= 2U ? static_cast<uint16_t>((body[0] << 8U) | body[1]) : 1005U;
      const size_t reasonLength = length > 2U ? (length - 2U < sizeof(brokerCloseReason) - 1U ? length - 2U : sizeof(brokerCloseReason) - 1U) : 0U;
      if (reasonLength) memcpy(brokerCloseReason, body + 2U, reasonLength);
      brokerCloseReason[reasonLength] = '\0';
      return false;
    }
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
//
// Every phase is timed (dns / tcp+tls / ws / mqtt), so a slow evening reads "tls=6100ms" instead of one opaque number, and name
// resolution is a phase of its own: lwIP DNS can block for many seconds and the address of the Cloudflare edge does not change
// between reconnects, so the address of the last handshake that completed is reused (and forgotten after any failure, so the next
// attempt resolves afresh). arduino-esp32 3.x treats a failed lookup as success and then dials 0.0.0.0, which surfaces as the
// unhelpful "Generic error"; the result is therefore checked here instead of trusted.
static uint32_t brokerIp = 0U;
static bool brokerIpTrusted = false;
static uint32_t connectedAt = 0U;
static bool stableLatched = true;
enum class Connect : uint8_t { Ok, Busy, Failed };
inline Connect connectClient() {
  using namespace MayapMqttWire;
  MayapTlsOperation tls(MayapTlsKind::Mqtt);
  if (!tls) return Connect::Busy;
  mayapSetMqttTlsResident(true);   // from here until stopClient()/a failed attempt the socket counts as THE TLS context
  buildIdentity();
  if (!netConfigured) {
    net.setCACert(TLS_ROOT_CA);
    // 10 s covers a TCP connect with one lost SYN (3 s retransmit) and is the safety net of any write; the TLS handshake gets
    // 20 s because it was measured at 4-10 s on a congested evening (8 s used to fail attempts that were about to succeed).
    net.setConnectionTimeout(10000);
    net.setHandshakeTimeout(20);
    netConfigured = true;
  }
  const uint32_t startedAt = millis();
  const uint32_t heapBefore = ESP.getFreeHeap();
  linkFailed = false;
  lostReason = "";
  carryLength = 0U;
  parser.reset();
  ws.reset();
  IPAddress ip;
  const bool cached = brokerIpTrusted && brokerIp != 0U;
  if (cached) {
    ip = IPAddress(brokerIp);
  } else if (WiFi.hostByName(brokerHost(), ip) != 1 || static_cast<uint32_t>(ip) == 0U) {
    mayapSerialPrintf(false, "[MQTT] dns failed host=%s after %lums\n", brokerHost(),
                      static_cast<unsigned long>(MayapRecovery::age(millis(), startedAt)));
    return Connect::Failed;                     // nothing was opened
  }
  const uint32_t dnsAt = millis();
  mayapServiceBeat(MayapRecovery::Service::Mqtt);
  if (!net.connect(ip, brokerPort(), brokerHost(), TLS_ROOT_CA, nullptr, nullptr)) {
    char reason[64] = "";
    net.lastError(reason, sizeof(reason));
    mayapSerialPrintf(false, "[MQTT] tls connect failed host=%s:%u err=%s ip=%s dns=%lums tls=%lums heap=%lu largest=%lu\n", brokerHost(),
                      static_cast<unsigned>(brokerPort()), reason, cached ? "cached" : "fresh",
                      static_cast<unsigned long>(MayapRecovery::age(dnsAt, startedAt)),
                      static_cast<unsigned long>(MayapRecovery::age(millis(), dnsAt)), static_cast<unsigned long>(ESP.getFreeHeap()),
                      static_cast<unsigned long>(ESP.getMaxAllocHeap()));
    brokerIpTrusted = false;                    // resolve afresh next time
    net.stop();
    return Connect::Failed;
  }
  const uint32_t tlsAt = millis();
  mayapServiceBeat(MayapRecovery::Service::Mqtt);
  if (!upgradeWebSocket()) { net.stop(); return Connect::Failed; }
  const uint32_t wsAt = millis();
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
  ctrlCount = 0U;
  txBlocked = false;
  stallSince = 0U;
  connected = true;
  brokerIp = static_cast<uint32_t>(ip);
  brokerIpTrusted = true;
  MayapUplink::onLinkUp();
  lastRxAt = lastTxAt = millis();
  const uint32_t doneAt = millis();
  mayapSerialPrintf(false, "[MQTT] wss+mqtt up %lums heap=%lu->%lu largest=%lu task=%s core=%d prio=%u dns=%lums%s tls=%lums ws=%lums mqtt=%lums\n",
                    static_cast<unsigned long>(MayapRecovery::age(doneAt, startedAt)), static_cast<unsigned long>(heapBefore),
                    static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMaxAllocHeap()),
                    pcTaskGetName(nullptr), static_cast<int>(xPortGetCoreID()),
                    static_cast<unsigned>(uxTaskPriorityGet(nullptr)),
                    static_cast<unsigned long>(MayapRecovery::age(dnsAt, startedAt)), cached ? "(cached)" : "",
                    static_cast<unsigned long>(MayapRecovery::age(tlsAt, dnsAt)), static_cast<unsigned long>(MayapRecovery::age(wsAt, tlsAt)),
                    static_cast<unsigned long>(MayapRecovery::age(doneAt, wsAt)));
  return Connect::Ok;
}

inline void onConnected() {
  using namespace MayapRealtimeInternal;
  connectedAt = millis();
  stableLatched = false;                        // the backoff ladder resets only after STABLE_UP_MS of this link
  MayapUplink::healthUp(millis());
  publishPresence(true);
  resetReportProgress();                        // the Web of this connection has seen none of a half-sent report
  portENTER_CRITICAL(&realtimeMux);
  if (knownConfigValid) configDirty = true;
  portEXIT_CRITICAL(&realtimeMux);
  lastSnapshotPublishAt = 0U;
  forceSnapshotPublish = true;
  mayapSerialPrintf(false, "[MQTT] connected %s\n", deviceId);
}

// QoS1 window slots a bulk report (config/history) may still take right now; 0 while the socket is stalled. The bridge asks BEFORE
// it builds a report, so a full window costs nothing instead of a rebuilt-and-discarded JSON document per retry.
inline uint8_t bulkSlotsFree() {
  if (!connected || txBlocked) return 0U;
  const uint8_t budget = static_cast<uint8_t>(QOS1_INFLIGHT_MAX - QOS1_RESERVED_FOR_ACK);
  return inflightCount >= budget ? 0U : static_cast<uint8_t>(budget - inflightCount);
}

// Publishes what the Cloud task queued (alarms, heartbeat), oldest first. A slot that cannot be sent for a
// reason other than a dead socket or a stalled uplink is failed so it can never wedge the queue.
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
      if (gateRefused) return;  // the socket cannot take bytes right now: the slot stays queued and is retried next pass
      MayapUplink::markFailed(slot);
    }
  }
}

// Liveness is judged from the socket and from bytes the broker sent, never from a PUBACK alone:
//   * a failed write, a protocol error or a closed socket                        -> dead
//   * nothing received for 1.5 keepalives (PINGRESP would have arrived)         -> dead (half-open)
//   * a QoS1 packet unacknowledged for QOS1_STUCK_MS while ALSO silent          -> dead
//   * nothing could be WRITTEN for TX_STALL_MS while the broker is silent too   -> dead (the uplink is jammed)
//   * a QoS1 packet unacknowledged for QOS1_STUCK_MS on a link that still delivers bytes -> alive; the entry just expires
// The reason is kept in lostReason so every loss is explained in the log.
inline bool linkAlive(uint32_t now) {
  if (linkFailed || parser.fatal() || ws.fatal() || !net.connected()) {
    if (!lostReason[0]) lostReason = linkFailed ? "tx-fail" : (parser.fatal() || ws.fatal()) ? "protocol" : "socket-closed";
    return false;
  }
  const uint32_t silence = MayapRecovery::age(now, lastRxAt);
  if (inflightCount > 0U && MayapRecovery::age(now, inflightAt[0]) >= QOS1_STUCK_MS) {
    if (silence >= STUCK_SILENCE_MS) { lostReason = "no-puback-silent"; return false; }
    inflightExpire(now);
  }
  if (stallSince != 0U && MayapRecovery::age(now, stallSince) >= TX_STALL_MS && silence >= TX_STALL_SILENCE_MS) {
    lostReason = "tx-stall";
    return false;
  }
  // Broker silent for 1.5 keepalives: half-open link.
  if (silence >= static_cast<uint32_t>(KEEPALIVE_SEC) * 1500UL) { lostReason = "silent"; return false; }
  return true;
}

// While the socket was unwritable: notice the moment it recovers (flush what waited, forget the stall).
inline void serviceTxStall() {
  if (stallSince == 0U) return;
  if (ctrlCount == 0U && !txBlocked) { stallSince = 0U; return; }
  if (!txReady()) return;
  if (!flushControl()) return;
  txBlocked = false;
  if (ctrlCount == 0U) stallSince = 0U;
}

// Measures how long one pass of the owner task takes and the lowest free heap it saw (diagnostic window).
struct LoopProbe {
  uint32_t startedAt;
  LoopProbe() : startedAt(millis()) {
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < heapLowWater) heapLowWater = freeHeap;
  }
  ~LoopProbe() {
    const uint32_t took = MayapRecovery::age(millis(), startedAt);
    if (took > loopMaxMs) loopMaxMs = took;
  }
};

}  // namespace MayapMqttInternal

inline void mayapMqttTransportBegin() {
  MayapRealtimeInternal::publishCallback = MayapMqttInternal::publishFromBridge;
  MayapRealtimeInternal::bulkCapacityCallback = MayapMqttInternal::bulkSlotsFree;
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
  const LoopProbe probe;
  const bool closing = gateClosing();
  const bool yielding = mayapCloudTlsYieldRequested(now);
  const bool pressure = mayapOnlineMemoryPressure();
  if (closing || mayapServiceIsolated(MayapRecovery::Service::Mqtt, now) || pressure || yielding) {
    if (connected) {
      const CloseKind kind = closing ? CloseKind::Radio : pressure ? CloseKind::Memory : yielding ? CloseKind::CloudTls : CloseKind::Isolated;
      ++closeCount[static_cast<uint8_t>(kind)];
      const MayapUplink::Health h = MayapUplink::healthSnapshot();
      mayapSerialPrintf(false, "[MQTT] closed on purpose (%s) why=%s rxAge=%lums probe=%s inflight=%u heap=%lu\n",
                        closing ? "radio" : pressure ? "memory" : yielding ? "cloud-tls" : "isolated",
                        kind == CloseKind::CloudTls ? MayapUplink::yieldWhyText(MayapUplink::yieldWhy()) : "n/a",
                        static_cast<unsigned long>(h.lastRxAt ? MayapRecovery::age(now, h.lastRxAt) : 0UL),
                        MayapUplink::probePending(h) ? "unanswered" : h.probes ? "answered" : "none",
                        static_cast<unsigned>(inflightCount), static_cast<unsigned long>(ESP.getFreeHeap()));
    }
    // Only a radio change (Wi-Fi portal / credentials) announces "offline" and says goodbye; a yield for the TLS slot or a memory
    // pause just closes the socket, so the broker keeps the device in its reconnect grace and the Web shows "reconnecting".
    stopClient(closing);
    if (!closing) backoff.holdOff(now, yielding && !pressure ? YIELD_RESUME_MS : HOLD_OFF_MS);
    MayapUplink::healthClosed(now, backoff.nextAttemptAt);   // deliberate: a short hold-off, not a failure
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
    MayapUplink::healthClosed(now, 0U);                       // no Wi-Fi: nothing to classify, the Cloud task checks Wi-Fi itself
    return;
  }
  if (staUpSince == 0U) staUpSince = now ? now : 1U;
  if (!ioEntered) {
    if (!mayapOnlineIoEnter(MayapRecovery::Service::Mqtt)) return;
    ioEntered = true;
  }
  if (!connected) {
    // Publish when the next attempt is due so alarm routing can tell "back in a few seconds" from "gone".
    const uint32_t stableAt = staUpSince + STA_STABLE_MS;
    MayapUplink::healthWaiting(now, static_cast<int32_t>(stableAt - backoff.nextAttemptAt) > 0 ? stableAt : backoff.nextAttemptAt);
    if (!backoff.ready(now) || !clockValid()) return;  // certificate dates need real time
    if (MayapRecovery::age(now, staUpSince) < STA_STABLE_MS) return;   // a flapping Wi-Fi gets no TLS handshakes
    Connect result = connectClient();
    if (result != Connect::Ok) mayapSetMqttTlsResident(false);   // every failure path already ran net.stop()
    if (result == Connect::Failed && mayapCloudTlsYieldRequested(millis())) {
      // Cloud asked for the heap while this handshake ran: contention, not a broker failure.
      mayapSerialPrintf(false, "[MQTT] connect aborted: Cloud needs the TLS lease, retry soon\n");
      result = Connect::Busy;
    }
    if (result == Connect::Busy) { backoff.holdOff(millis(), BUSY_RETRY_MS); MayapUplink::healthWaiting(millis(), backoff.nextAttemptAt); return; }
    if (result == Connect::Failed) {
      backoff.onFailure(millis());
      MayapUplink::healthAttemptFailed(millis(), backoff.nextAttemptAt);
      return;
    }
    onConnected();
    return;
  }
  if (!stableLatched && MayapRecovery::age(now, connectedAt) >= STABLE_UP_MS) { backoff.onStable(); stableLatched = true; }
  serviceTxStall();
  if (!linkAlive(now) || !pump()) {
    ++closeCount[static_cast<uint8_t>(CloseKind::Lost)];
    if (!lostReason[0]) lostReason = brokerCloseCode ? "broker-close" : "read-fail";
    mayapSerialPrintf(false, "[MQTT] link lost reason=%s (tx=%u fatal=%u open=%u inflight=%u rxAge=%lums brokerClose=%u '%s' refused=%lu dropped=%lu ctrlQ=%u stall=%lums up=%lums)\n",
                      lostReason, static_cast<unsigned>(linkFailed), static_cast<unsigned>(parser.fatal()), static_cast<unsigned>(net.connected()),
                      static_cast<unsigned>(inflightCount), static_cast<unsigned long>(MayapRecovery::age(millis(), lastRxAt)),
                      static_cast<unsigned>(brokerCloseCode), brokerCloseReason, static_cast<unsigned long>(txRefused),
                      static_cast<unsigned long>(txDropped), static_cast<unsigned>(ctrlCount),
                      static_cast<unsigned long>(stallSince ? MayapRecovery::age(millis(), stallSince) : 0UL),
                      static_cast<unsigned long>(MayapRecovery::age(millis(), connectedAt)));
    stopClient(false);
    backoff.onFailure(millis());      // fresh time: `now` is from the top of the pass and the pass may have blocked
    MayapUplink::healthLost(millis(), backoff.nextAttemptAt);
    return;
  }
  MayapUplink::healthRx(lastRxAt);
  pumpUplink(now);
  // On-demand transport probe: the Cloud task saw an acknowledgement come late and wants independent evidence. One PINGREQ;
  // ANY byte back (PINGRESP, a PUBACK, a command) proves the broker is alive. Rate limited by the uplink mailbox itself.
  if (MayapUplink::takeProbeRequest() && sendPacket(MayapMqttWire::encodePingreq(txMqtt(), PACKET_BUFFER), Tx::Control)) {
    MayapUplink::noteProbeSent(now);
  } else if (MayapRecovery::age(now, lastTxAt) >= static_cast<uint32_t>(KEEPALIVE_SEC) * 500UL)
    sendPacket(MayapMqttWire::encodePingreq(txMqtt(), PACKET_BUFFER), Tx::Control);
  mayapRealtimeUpdate(millis());
#if MAYAP_DIAGNOSTIC_SERIAL
  if (lastDiagAt == 0U || MayapRecovery::age(now, lastDiagAt) >= 10000UL) {
    lastDiagAt = now;
    const MayapUplink::Health h = MayapUplink::healthSnapshot();
    mayapSerialPrintf(false, "[MQTT] up inflight=%u dropBig=%lu dropForeign=%lu\n", static_cast<unsigned>(inflightCount),
                      static_cast<unsigned long>(droppedOversize), static_cast<unsigned long>(droppedForeign));
    mayapSerialPrintf(false, "[MQTT-STAT] closes radio=%lu mem=%lu cloud-tls=%lu iso=%lu lost=%lu | qos1 expired=%lu refusedBulk=%lu refusedAck=%lu | probes=%lu answered=%lu rttEwma=%ums\n",
                      static_cast<unsigned long>(closeCount[0]), static_cast<unsigned long>(closeCount[1]), static_cast<unsigned long>(closeCount[2]),
                      static_cast<unsigned long>(closeCount[3]), static_cast<unsigned long>(closeCount[4]), static_cast<unsigned long>(qos1Expired),
                      static_cast<unsigned long>(refusedBulk), static_cast<unsigned long>(refusedAck), static_cast<unsigned long>(h.probes),
                      static_cast<unsigned long>(h.probesAnswered), static_cast<unsigned>(h.rttEwmaMs));
    // Per 10 s window: how often the socket could not take a frame, the slowest write and task pass, the lowest free heap and the signal.
    mayapSerialPrintf(false, "[MQTT-TX] refused=%lu dropped=%lu ctrlQueued=%lu shedAck=%lu writeMax=%lums loopMax=%lums heapLow=%lu rssi=%d\n",
                      static_cast<unsigned long>(txRefused), static_cast<unsigned long>(txDropped), static_cast<unsigned long>(txCtrlQueued),
                      static_cast<unsigned long>(advisoryShed), static_cast<unsigned long>(writeMaxMs), static_cast<unsigned long>(loopMaxMs),
                      static_cast<unsigned long>(heapLowWater), static_cast<int>(status.rssiDbm));
    writeMaxMs = 0U;
    loopMaxMs = 0U;
    heapLowWater = 0xFFFFFFFFUL;
  }
#endif
}

// The supervisor asked for a fresh start of the MQTT owner. That is a request, not a failure: close the socket and reconnect at
// once (the old code also pushed the retry out by a whole backoff step on top of the supervisor's own pause).
inline void mayapMqttTransportRecover(uint32_t now) {
  mayapSerialPrintf(false, "[MQTT] owner re-init requested by the supervisor: link closed, reconnecting at once\n");
  MayapMqttInternal::stopClient(false);
  MayapMqttInternal::backoff.nextAttemptAt = now;
  MayapUplink::healthClosed(now, now);
}
