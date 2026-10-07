// Host test of the REAL mqtt_transport.h + mqtt_wire.h (copied beside tests/stubs/mqtt by
// tests/mqtt-transport.test.cjs): native MQTT/TLS client behaviour against a scripted socket.
#include "mayap_stubs.h"
#include <cassert>
#include <cstdlib>

uint32_t g_millis = 100000U, g_epoch = 1800000000U;
std::vector<std::string> g_log;
char g_mqttKey[65] = ""; char g_mqttHost[64] = ""; uint16_t g_mqttPort = 0U;
bool g_gateClosing = false, g_isolated = false, g_pressure = false, g_yield = false, g_ioEnterOk = true, g_tlsAllowed = true;
unsigned g_beats = 0U, g_realtimeUpdates = 0U;
NetworkStatus g_networkStatus{ConnectivityMode::Online, true};
std::vector<MayapRealtimeInternal::Delivered> MayapRealtimeInternal::g_delivered;

#include "mqtt_transport.h"
#include <algorithm>

using namespace MayapMqttInternal;
using MayapMqttWire::StreamParser;

static bool logged(const char *needle) { for (auto &l : g_log) if (l.find(needle) != std::string::npos) return true; return false; }
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); for (auto &l : g_log) fprintf(stderr, "  log: %s", l.c_str()); exit(1); } } while (0)

struct Packet { uint8_t type, flags; std::vector<uint8_t> body; };
static std::vector<Packet> decodeSent(size_t from = 0) {
  std::vector<Packet> out;
  uint8_t buffer[4096];
  for (size_t i = from; i < net.sent.size(); ++i) {
    StreamParser parser(buffer, sizeof(buffer));
    const auto &bytes = net.sent[i];
    size_t used = 0;
    while (used < bytes.size()) {
      used += parser.feed(bytes.data() + used, bytes.size() - used);
      if (parser.ready()) out.push_back({parser.type(), parser.flags(), std::vector<uint8_t>(parser.body(), parser.body() + parser.bodyLength())});
    }
  }
  return out;
}
static std::string str(const uint8_t *p, size_t n) { return std::string(reinterpret_cast<const char *>(p), n); }
static void inject(std::initializer_list<uint8_t> bytes) { net.in.insert(net.in.end(), bytes.begin(), bytes.end()); }
static void injectPublish(const std::string &topic, const std::string &payload, uint8_t qos, uint16_t id) {
  uint8_t buffer[4096];
  size_t n = MayapMqttWire::encodePublish(buffer, sizeof(buffer), topic.c_str(), reinterpret_cast<const uint8_t *>(payload.data()), payload.size(), qos, false, id);
  CHECK(n > 0);
  net.in.insert(net.in.end(), buffer, buffer + n);
}

// The broker side of the handshake: CONNACK for CONNECT, SUBACK for SUBSCRIBE.
static uint8_t g_connackCode = 0U; static bool g_silentBroker = false; static uint8_t g_subackFirst = 1U;
static void installBroker() {
  net.onWrite = [](const std::vector<uint8_t> &bytes) {
    if (g_silentBroker) return;
    if ((bytes[0] >> 4) == 1) inject({0x20, 0x02, 0x00, g_connackCode});
    if ((bytes[0] >> 4) == 8) inject({0x90, 0x06, 0x00, 0x01, g_subackFirst, 0x01, 0x01, 0x00});
  };
}
static void resetWorld() {
  net = WiFiClientSecure();
  parser.reset(); connected = false; linkFailed = false; carryLength = 0U; inflightCount = 0U; nextPacketId = 1U;
  droppedOversize = droppedForeign = 0U; backoff = BackoffTimer();
  g_log.clear(); MayapRealtimeInternal::g_delivered.clear(); g_realtimeUpdates = 0U;
  g_connackCode = 0U; g_silentBroker = false; g_subackFirst = 1U;
  g_gateClosing = g_isolated = g_pressure = g_yield = false; g_ioEnterOk = g_tlsAllowed = true;
  g_networkStatus = NetworkStatus{ConnectivityMode::Online, true};
  strcpy(g_mqttKey, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  strcpy(g_mqttHost, "broker.example.com"); g_mqttPort = 8883U; g_epoch = 1800000000U;
  installBroker();
}
static void tick(uint32_t ms) { g_millis += ms; mayapMqttTransportUpdate(g_millis); }
static void connectNow() {
  mayapMqttTransportBegin();
  tick(1);   // ioEnter + connect + handshake + presence
  CHECK(mayapMqttTransportConnected());
}

int main() {
  // 1. Idle until the Worker has provisioned host + credential; no TLS attempt before real time.
  resetWorld(); g_mqttHost[0] = '\0';
  mayapMqttTransportBegin(); tick(10); tick(10);
  CHECK(net.connects == 0U && !mayapMqttTransportConnected());
  resetWorld(); g_epoch = 1000U;
  mayapMqttTransportBegin(); tick(10); tick(10);
  CHECK(net.connects == 0U);                              // clock not valid: certificate dates unusable
  g_epoch = 1800000000U; tick(10);
  CHECK(mayapMqttTransportConnected());

  // 2. Handshake contents: CONNECT (clean session, LWT QoS1 retained, creds), SUBSCRIBE x4, presence online.
  resetWorld(); connectNow();
  auto sent = decodeSent();
  CHECK(sent.size() >= 3U);
  const Packet &connect = sent[0];
  CHECK(connect.type == 1 && str(connect.body.data() + 2, 4) == "MQTT" && connect.body[6] == 4U);
  CHECK(connect.body[7] == (0x80 | 0x40 | 0x20 | 0x08 | 0x04 | 0x02));
  CHECK(((connect.body[8] << 8) | connect.body[9]) == 30);
  CHECK(str(connect.body.data() + 12, 20) == "esp-MAP-AABBCCDDEEFF");
  const std::string connectText = str(connect.body.data(), connect.body.size());
  CHECK(connectText.find("mayap/v1/MAP-AABBCCDDEEFF/presence") != std::string::npos);
  CHECK(connectText.find("{\"online\":false}") != std::string::npos);
  CHECK(connectText.find(g_mqttKey) != std::string::npos);
  const Packet &subscribe = sent[1];
  CHECK(subscribe.type == 8 && subscribe.flags == 2);
  const std::string subs = str(subscribe.body.data(), subscribe.body.size());
  for (const char *channel : {"/command", "/config/set", "/history/request", "/session"})
    CHECK(subs.find(std::string("mayap/v1/MAP-AABBCCDDEEFF") + channel) != std::string::npos);
  CHECK(subscribe.body.back() == 0U);                     // session is QoS0
  const Packet &presence = sent[2];
  CHECK(presence.type == 3 && presence.flags == (0x02 | 0x01));   // QoS1, retained
  CHECK(inflightCount == 1U);                             // presence awaits its PUBACK
  CHECK(g_realtimeUpdates == 0U);

  // 3. QoS1 PUBACK clears the in-flight slot; QoS0 does not use one; bounded to 4; unknown channels refused.
  inject({0x40, 0x02, 0x00, static_cast<uint8_t>(nextPacketId)});
  tick(1);
  CHECK(inflightCount == 0U && g_realtimeUpdates >= 1U);
  CHECK(publishFromBridge("snapshot", "{\"t\":1}", 7U) && inflightCount == 0U);
  CHECK(!publishFromBridge("command", "{}", 2U));          // Web->device topic: never published by the device
  CHECK(!publishFromBridge("ack", "", 0U));
  for (int i = 0; i < 4; ++i) CHECK(publishFromBridge("ack", "{\"phase\":\"completed\"}", 22U));
  CHECK(inflightCount == 4U && !publishFromBridge("ack", "{}", 2U));
  CHECK(publishFromBridge("snapshot", "{}", 2U));          // QoS0 still flows when the QoS1 window is full
  CHECK(publishFromBridge("bootstrap", "{}", 2U));         // swallowed: not part of the topic contract
  for (uint16_t id = nextPacketId - 3U; id <= nextPacketId; ++id) inject({0x40, 0x02, static_cast<uint8_t>(id >> 8U), static_cast<uint8_t>(id)});
  tick(1);
  CHECK(inflightCount == 0U);

  // 4. Inbound: QoS1 command is PUBACKed then delivered to the bridge in this same task; foreign topics and
  //    oversized packets are dropped without desynchronising the stream.
  resetWorld(); connectNow(); net.sent.clear();
  injectPublish("mayap/v1/MAP-AABBCCDDEEFF/command", "{\"v\":2,\"requestId\":\"R1\"}", 1U, 42U);
  injectPublish("mayap/v1/OTHER/command", "{\"x\":1}", 1U, 43U);
  injectPublish("mayap/v1/MAP-AABBCCDDEEFF/session", "{\"active\":true}", 0U, 0U);
  injectPublish("mayap/v1/MAP-AABBCCDDEEFF/command", std::string(3000, 'x'), 0U, 0U);       // > 2120: truncated
  injectPublish("mayap/v1/MAP-AABBCCDDEEFF/config/set", "{\"after\":\"big\"}", 0U, 0U);
  tick(1);
  CHECK(MayapRealtimeInternal::g_delivered.size() == 3U);
  CHECK(MayapRealtimeInternal::g_delivered[0].channel == "command" && MayapRealtimeInternal::g_delivered[0].payload == "{\"v\":2,\"requestId\":\"R1\"}");
  CHECK(MayapRealtimeInternal::g_delivered[1].channel == "session");
  CHECK(MayapRealtimeInternal::g_delivered[2].channel == "config/set" && MayapRealtimeInternal::g_delivered[2].payload == "{\"after\":\"big\"}");
  CHECK(droppedOversize == 1U && droppedForeign == 1U);
  auto acks = decodeSent();
  unsigned pubacks = 0; for (auto &p : acks) if (p.type == 4) ++pubacks;
  CHECK(pubacks == 2U);                                   // 42 and the foreign 43 (no endless redelivery)

  // 5. Keepalive: PINGREQ after half the interval of silence; a broker silent for 1.5 keepalives is a dead link.
  resetWorld(); connectNow(); net.sent.clear();
  inject({0x40, 0x02, 0x00, static_cast<uint8_t>(nextPacketId)}); tick(1);
  net.sent.clear();
  tick(16000);
  bool ping = false; for (auto &p : decodeSent()) if (p.type == 12) ping = true;
  CHECK(ping);
  inject({0xD0, 0x00}); tick(1);                          // PINGRESP keeps it alive
  tick(30000); CHECK(mayapMqttTransportConnected());
  tick(30000); tick(30000);
  CHECK(logged("link lost") && backoff.failures >= 1U && net.stops >= 1U);   // silent broker: dropped (then reconnected after backoff)

  // 6. A QoS1 message nobody acknowledges for 15 s drops the link (the bridge/Web then settle UNCERTAIN).
  resetWorld(); connectNow();
  inject({0x40, 0x02, 0x00, static_cast<uint8_t>(nextPacketId)}); tick(1);
  CHECK(publishFromBridge("ack", "{\"x\":1}", 7U));
  for (int i = 0; i < 30; ++i) { inject({0xD0, 0x00}); tick(600); }
  CHECK(logged("link lost") && logged("inflight=1"));

  // 7. Refusals and failures back off; nothing is left half-open.
  resetWorld(); g_connackCode = 5U; mayapMqttTransportBegin(); tick(1); tick(1);
  CHECK(!mayapMqttTransportConnected() && backoff.failures == 1U && !net.open);
  resetWorld(); g_subackFirst = 0x80U; mayapMqttTransportBegin(); tick(1); tick(1);
  CHECK(!mayapMqttTransportConnected() && backoff.failures == 1U && !net.open);
  resetWorld(); net.allowConnect = false; mayapMqttTransportBegin(); tick(1); tick(1);
  CHECK(!mayapMqttTransportConnected() && backoff.failures == 1U);
  resetWorld(); g_silentBroker = true; mayapMqttTransportBegin(); tick(1); tick(1);
  CHECK(!mayapMqttTransportConnected() && backoff.failures == 1U && !net.open);
  resetWorld(); g_tlsAllowed = false; mayapMqttTransportBegin(); tick(1); tick(1);
  CHECK(net.connects == 0U);                              // no TLS lease: no handshake (Cloud/OTA own the heap)

  // 8. Gates: Wi-Fi/portal/yield/isolation stop the client; a graceful stop publishes offline then DISCONNECT only
  //    when the offline presence was really written (otherwise the broker must fire the LWT).
  resetWorld(); connectNow(); net.sent.clear();
  g_yield = true; tick(1);
  sent = decodeSent();
  CHECK(!mayapMqttTransportConnected() && !net.open && sent.size() == 2U);
  CHECK(sent[0].type == 3 && str(sent[0].body.data(), sent[0].body.size()).find("\"online\":false") != std::string::npos);
  CHECK(sent[1].type == 14);
  resetWorld(); connectNow(); net.sent.clear(); net.failWrite = true;
  g_gateClosing = true; tick(1);
  CHECK(!net.open && decodeSent().empty());              // nothing could be written: abrupt close, LWT fires
  resetWorld(); connectNow(); g_networkStatus.connected = false; tick(1);
  CHECK(!mayapMqttTransportConnected() && !net.open);
  resetWorld(); connectNow(); g_pressure = true; tick(1);
  CHECK(!mayapMqttTransportConnected());

  // 9. Recover (supervisor request) and reconnect after backoff: a fresh handshake, in-flight cleared.
  resetWorld(); connectNow();
  mayapMqttTransportRecover(g_millis);
  CHECK(!mayapMqttTransportConnected() && inflightCount == 0U);
  tick(100); CHECK(!mayapMqttTransportConnected());       // backoff running
  tick(6000); tick(1);
  CHECK(mayapMqttTransportConnected() && net.connects == 2U);   // initial + one fresh handshake after the backoff
  printf("mqtt transport host tests PASS\n");
  return 0;
}
