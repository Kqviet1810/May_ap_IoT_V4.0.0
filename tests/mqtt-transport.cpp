// Host test of the REAL mqtt_transport.h + mqtt_wire.h + mqtt_ws.h (copied beside tests/stubs/mqtt by
// tests/mqtt-transport.test.cjs): MQTT-over-WebSocket-over-TLS client behaviour against a scripted
// socket that plays the Cloudflare broker (HTTP Upgrade, masked client frames, unmasked server frames).
#include "mayap_stubs.h"
#include <cassert>
#include <cstdlib>
#include <tuple>

uint32_t g_millis = 100000U, g_epoch = 1800000000U;
std::vector<std::string> g_log;
char g_mqttKey[65] = "";
bool g_gateClosing = false, g_isolated = false, g_pressure = false, g_yield = false, g_ioEnterOk = true, g_tlsAllowed = true;
unsigned g_yieldAfterCalls = 0U;
unsigned g_beats = 0U, g_realtimeUpdates = 0U;
NetworkStatus g_networkStatus{ConnectivityMode::Online, true, -45};
std::vector<MayapRealtimeInternal::Delivered> MayapRealtimeInternal::g_delivered;

#include "mqtt_transport.h"
#include <algorithm>

using namespace MayapMqttInternal;
using MayapMqttWire::StreamParser;

static bool logged(const char *needle) { for (auto &l : g_log) if (l.find(needle) != std::string::npos) return true; return false; }
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); for (auto &l : g_log) fprintf(stderr, "  log: %s", l.c_str()); exit(1); } } while (0)

// ---- the broker side of the WebSocket ---------------------------------------------------------
enum class UpgradeMode { Good, BadAccept, Status403, Extension, WrongProtocol, Oversize, NoTerminator };
static UpgradeMode g_upgradeMode = UpgradeMode::Good;
static bool g_upgraded = false, g_clientFrameBad = false, g_silentBroker = false;
static std::string g_request;
static unsigned g_clientPings = 0U, g_clientPongs = 0U, g_clientCloses = 0U;
static std::vector<std::vector<uint8_t>> g_clientPongPayloads;

static std::vector<uint8_t> serverFrame(const std::vector<uint8_t> &payload, uint8_t opcode = 2, bool fin = true, int lengthForm = 0) {
  std::vector<uint8_t> f;
  f.push_back(static_cast<uint8_t>((fin ? 0x80 : 0) | opcode));
  const size_t n = payload.size();
  if (lengthForm == 8) { f.push_back(127); for (int i = 7; i >= 0; --i) f.push_back(static_cast<uint8_t>(static_cast<uint64_t>(n) >> (8 * i))); }
  else if (n < 126 && lengthForm == 0) f.push_back(static_cast<uint8_t>(n));
  else { f.push_back(126); f.push_back(static_cast<uint8_t>(n >> 8)); f.push_back(static_cast<uint8_t>(n)); }
  f.insert(f.end(), payload.begin(), payload.end());
  return f;
}
static void injectBytes(const std::vector<uint8_t> &b) { net.in.insert(net.in.end(), b.begin(), b.end()); }
static void inject(std::initializer_list<uint8_t> bytes) { injectBytes(serverFrame(std::vector<uint8_t>(bytes))); }
static void injectPublish(const std::string &topic, const std::string &payload, uint8_t qos, uint16_t id) {
  uint8_t buffer[4096];
  size_t n = MayapMqttWire::encodePublish(buffer, sizeof(buffer), topic.c_str(), reinterpret_cast<const uint8_t *>(payload.data()), payload.size(), qos, false, id);
  CHECK(n > 0);
  injectBytes(serverFrame(std::vector<uint8_t>(buffer, buffer + n)));
}

struct ClientFrame { uint8_t opcode; bool fin; bool masked; std::vector<uint8_t> payload; };
static bool parseClientFrame(const std::vector<uint8_t> &b, ClientFrame &out) {
  if (b.size() < 6) return false;
  out.fin = (b[0] & 0x80) != 0; out.opcode = b[0] & 0x0F; out.masked = (b[1] & 0x80) != 0;
  size_t len = b[1] & 0x7F, pos = 2;
  if (len == 126) { len = (static_cast<size_t>(b[2]) << 8) | b[3]; pos = 4; }
  else if (len == 127) return false;
  if (!out.masked || b.size() != pos + 4 + len || (b[0] & 0x70)) return false;
  out.payload.resize(len);
  for (size_t i = 0; i < len; ++i) out.payload[i] = static_cast<uint8_t>(b[pos + 4 + i] ^ b[pos + (i & 3)]);
  return true;
}

struct Packet { uint8_t type, flags; std::vector<uint8_t> body; };
static std::vector<Packet> decodeSent(size_t from = 0) {
  std::vector<Packet> out;
  uint8_t buffer[4096];
  for (size_t i = from; i < net.sent.size(); ++i) {
    ClientFrame frame;
    if (!parseClientFrame(net.sent[i], frame)) continue;    // the HTTP request is not a frame
    if (frame.opcode != 2) continue;
    StreamParser parser(buffer, sizeof(buffer));
    size_t used = 0;
    while (used < frame.payload.size()) {
      used += parser.feed(frame.payload.data() + used, frame.payload.size() - used);
      if (parser.ready()) out.push_back({parser.type(), parser.flags(), std::vector<uint8_t>(parser.body(), parser.body() + parser.bodyLength())});
    }
  }
  return out;
}
static std::string str(const uint8_t *p, size_t n) { return std::string(reinterpret_cast<const char *>(p), n); }

static uint8_t g_connackCode = 0U; static uint8_t g_subackFirst = 1U;
static void answerUpgrade(const std::string &request) {
  const size_t k = request.find("Sec-WebSocket-Key: ");
  CHECK(k != std::string::npos);
  const std::string key = request.substr(k + 19, 24);
  char accept[MayapMqttWs::ACCEPT_BASE64 + 1];
  MayapMqttWs::expectedAccept(key.c_str(), accept);
  std::string response;
  switch (g_upgradeMode) {
    case UpgradeMode::Status403: response = "HTTP/1.1 403 Forbidden\r\n\r\n"; break;
    case UpgradeMode::NoTerminator: response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"; break;
    case UpgradeMode::Oversize: response = "HTTP/1.1 101 Switching Protocols\r\nX-Pad: " + std::string(900, 'a') + "\r\n\r\n"; break;
    default:
      response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                 std::string(g_upgradeMode == UpgradeMode::BadAccept ? "AAAAAAAAAAAAAAAAAAAAAAAAAAA=" : accept) + "\r\n";
      response += g_upgradeMode == UpgradeMode::WrongProtocol ? "Sec-WebSocket-Protocol: chat\r\n" : "Sec-WebSocket-Protocol: mqtt\r\n";
      if (g_upgradeMode == UpgradeMode::Extension) response += "Sec-WebSocket-Extensions: permessage-deflate\r\n";
      response += "\r\n";
  }
  net.in.insert(net.in.end(), response.begin(), response.end());
  if (g_upgradeMode == UpgradeMode::Good) g_upgraded = true;
}
static void installBroker() {
  net.onWrite = [](const std::vector<uint8_t> &bytes) {
    if (bytes.size() > 4 && memcmp(bytes.data(), "GET ", 4) == 0) {   // a (new) opening handshake
      g_upgraded = false; g_request.assign(bytes.begin(), bytes.end()); answerUpgrade(g_request);
      return;
    }
    if (!g_upgraded) return;
    ClientFrame frame;
    if (!parseClientFrame(bytes, frame)) { g_clientFrameBad = true; return; }   // unmasked / malformed client frame
    if (frame.opcode == 9) { ++g_clientPings; return; }
    if (frame.opcode == 10) { ++g_clientPongs; g_clientPongPayloads.push_back(frame.payload); return; }
    if (frame.opcode == 8) { ++g_clientCloses; return; }
    if (frame.opcode != 2 || g_silentBroker || frame.payload.empty()) return;
    if ((frame.payload[0] >> 4) == 1) inject({0x20, 0x02, 0x00, g_connackCode});
    if ((frame.payload[0] >> 4) == 8) inject({0x90, 0x06, 0x00, 0x01, g_subackFirst, 0x01, 0x01, 0x00});
  };
}
static void resetWorld() {
  net = WiFiClientSecure();
  parser.reset(); ws.reset(); connected = false; linkFailed = false; staUpSince = 0U; carryLength = 0U; inflightCount = 0U; nextPacketId = 1U;
  droppedOversize = droppedForeign = 0U; qos1Expired = refusedBulk = refusedAck = 0U; backoff = Retry();
  ctrlCount = 0U; txBlocked = false; gateRefused = false; stallSince = 0U; txRefused = txDropped = txCtrlQueued = advisoryShed = 0U;
  writeMaxMs = loopMaxMs = 0U; heapLowWater = 0xFFFFFFFFUL; lostReason = ""; brokerIp = 0U; brokerIpTrusted = false; connectedAt = 0U; stableLatched = true;
  dnsOk() = true; dnsLookups() = 0U;
  g_log.clear(); MayapRealtimeInternal::g_delivered.clear(); g_realtimeUpdates = 0U;
  g_connackCode = 0U; g_silentBroker = false; g_subackFirst = 1U; g_upgradeMode = UpgradeMode::Good;
  g_upgraded = false; g_clientFrameBad = false; g_request.clear(); g_clientPings = g_clientPongs = g_clientCloses = 0U; g_clientPongPayloads.clear();
  g_gateClosing = g_isolated = g_pressure = g_yield = false; g_ioEnterOk = g_tlsAllowed = true; g_yieldAfterCalls = 0U;
  g_networkStatus = NetworkStatus{ConnectivityMode::Online, true, -45};
  strcpy(g_mqttKey, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  g_epoch = 1800000000U;
  installBroker();
}
static void tick(uint32_t ms) { g_millis += ms; mayapMqttTransportUpdate(g_millis); }
static void connectNow() {
  mayapMqttTransportBegin();
  tick(1);                // Wi-Fi first seen up
  tick(STA_STABLE_MS);    // stable: ioEnter + TLS + upgrade + MQTT handshake + presence
  CHECK(mayapMqttTransportConnected());
}

int main() {
  // 1. Idle until the Worker has provisioned the credential; no TLS attempt before real time.
  resetWorld(); g_mqttKey[0] = '\0';
  mayapMqttTransportBegin(); tick(10); tick(10);
  CHECK(net.connects == 0U && !mayapMqttTransportConnected());
  resetWorld(); g_epoch = 1000U;
  mayapMqttTransportBegin(); tick(10); tick(10);
  CHECK(net.connects == 0U);                              // clock not valid: certificate dates unusable
  g_epoch = 1800000000U; tick(10); tick(STA_STABLE_MS);
  CHECK(mayapMqttTransportConnected());

  // 1b. WebSocket opening handshake: SNI host + port 443, GET /mqtt/<deviceId>, mqtt subprotocol, random key.
  resetWorld(); connectNow();
  CHECK(net.lastHost == MAYAP_BROKER_HOST && net.lastPort == 443U);
  CHECK(g_request.compare(0, 37, "GET /mqtt/MAP-AABBCCDDEEFF HTTP/1.1\r\n") == 0);
  CHECK(g_request.find("Upgrade: websocket\r\n") != std::string::npos && g_request.find("Connection: Upgrade\r\n") != std::string::npos);
  CHECK(g_request.find("Sec-WebSocket-Version: 13\r\n") != std::string::npos && g_request.find("Sec-WebSocket-Protocol: mqtt\r\n") != std::string::npos);
  CHECK(g_request.find(std::string("Host: ") + MAYAP_BROKER_HOST + "\r\n") != std::string::npos);
  CHECK(g_request.size() >= 4 && g_request.compare(g_request.size() - 4, 4, "\r\n\r\n") == 0);
  CHECK(!g_clientFrameBad);                               // every client frame so far was masked and well formed

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

  // 3. QoS1 PUBACK clears the in-flight slot; QoS0 does not use one; bounded to QOS1_INFLIGHT_MAX; unknown channels refused.
  inject({0x40, 0x02, 0x00, static_cast<uint8_t>(nextPacketId)});
  tick(1);
  CHECK(inflightCount == 0U && g_realtimeUpdates >= 1U);
  CHECK(publishFromBridge("snapshot", "{\"t\":1}", 7U) && inflightCount == 0U);
  CHECK(!publishFromBridge("command", "{}", 2U));          // Web->device topic: never published by the device
  CHECK(!publishFromBridge("ack", "", 0U));
  for (unsigned i = 0; i < QOS1_INFLIGHT_MAX; ++i) CHECK(publishFromBridge("ack", "{\"phase\":\"completed\"}", 22U));
  CHECK(inflightCount == QOS1_INFLIGHT_MAX && !publishFromBridge("ack", "{}", 2U));
  CHECK(publishFromBridge("snapshot", "{}", 2U));          // QoS0 still flows when the QoS1 window is full
  CHECK(publishFromBridge("bootstrap", "{}", 2U));         // swallowed: not part of the topic contract
  for (uint16_t id = nextPacketId - (QOS1_INFLIGHT_MAX - 1U); id <= nextPacketId; ++id) inject({0x40, 0x02, static_cast<uint8_t>(id >> 8U), static_cast<uint8_t>(id)});
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

  // 6. A QoS1 message nobody acknowledges does NOT drop a link that is still talking (PINGRESP keeps arriving): the entry just
  //    expires and the window is free again. Only "unacknowledged AND silent" is a dead socket.
  resetWorld(); connectNow();
  inject({0x40, 0x02, 0x00, static_cast<uint8_t>(nextPacketId)}); tick(1);
  CHECK(publishFromBridge("ack", "{\"x\":1}", 7U));
  for (int i = 0; i < 30; ++i) { inject({0xD0, 0x00}); tick(600); }
  CHECK(mayapMqttTransportConnected() && !logged("link lost"));
  CHECK(inflightCount == 0U && qos1Expired == 1U);
  resetWorld(); connectNow();
  inject({0x40, 0x02, 0x00, static_cast<uint8_t>(nextPacketId)}); tick(1);
  CHECK(publishFromBridge("ack", "{\"x\":1}", 7U));
  for (int i = 0; i < 20; ++i) tick(1000);                                   // unacknowledged for 20 s and the broker is silent
  CHECK(logged("link lost") && logged("inflight=1"));

  // 6b. Terminal ACKs keep a reserve of the QoS1 window: config/history/presence ("bulk") may never take the last
  //     QOS1_RESERVED_FOR_ACK slots, so a report burst cannot starve a command's ACK. Refusals are counted, not silent.
  resetWorld(); connectNow();
  inject({0x40, 0x02, 0x00, static_cast<uint8_t>(nextPacketId)}); tick(1);
  CHECK(inflightCount == 0U);
  unsigned bulk = 0U; while (publishFromBridge("config/reported", "{\"c\":1}", 8U)) ++bulk;
  CHECK(bulk == QOS1_INFLIGHT_MAX - QOS1_RESERVED_FOR_ACK && refusedBulk == 1U);
  for (unsigned i = 0; i < QOS1_RESERVED_FOR_ACK; ++i) CHECK(publishFromBridge("ack", "{\"p\":1}", 8U));   // the reserve is still there
  CHECK(inflightCount == QOS1_INFLIGHT_MAX && !publishFromBridge("ack", "{}", 2U) && refusedAck == 1U);

  // 6c. Transport probe: a late acknowledgement asks for ONE PINGREQ; ANY byte back proves the broker alive; an unanswered probe
  //     stays "pending" (the Cloud task's evidence for a half-open socket) and is not repeated sooner than PROBE_MIN_GAP_MS.
  {
    using namespace MayapUplink;
    resetWorld(); Internal::health = Health{}; connectNow(); net.sent.clear();
    const uint32_t beforeProbes = healthSnapshot().probes;
    requestProbe(g_millis); tick(1);
    unsigned pings = 0; for (auto &p : decodeSent()) if (p.type == 12) ++pings;
    CHECK(pings == 1U && healthSnapshot().probes == beforeProbes + 1U && probePending(healthSnapshot()));
    net.sent.clear(); requestProbe(g_millis); tick(1);                       // unanswered: no second PINGREQ
    pings = 0; for (auto &p : decodeSent()) if (p.type == 12) ++pings;
    CHECK(pings == 0U && probePending(healthSnapshot()));
    inject({0xD0, 0x00}); tick(1);                                           // PINGRESP
    CHECK(!probePending(healthSnapshot()) && healthSnapshot().probesAnswered == 1U);
    net.sent.clear(); tick(PROBE_MIN_GAP_MS + 1U); requestProbe(g_millis); tick(1);
    pings = 0; for (auto &p : decodeSent()) if (p.type == 12) ++pings;
    CHECK(pings == 1U);
    // a probe answered by something other than PINGRESP (a command) counts too
    injectPublish("mayap/v1/MAP-AABBCCDDEEFF/session", "{\"a\":1}", 0U, 0U); tick(1);
    CHECK(!probePending(healthSnapshot()));
  }

  // 6d. Broker CLOSE frames are recorded (code + reason) and appear in the "link lost" line: no more anonymous drops.
  resetWorld(); connectNow();
  injectBytes(serverFrame(std::vector<uint8_t>{0x03, 0xF5, 'S', 'L', 'O', 'W'}, 8)); tick(1);
  CHECK(logged("link lost") && logged("brokerClose=1013 'SLOW'"));

  // 7. Refusals and failures back off; nothing is left half-open.
  resetWorld(); g_connackCode = 5U; mayapMqttTransportBegin(); tick(1); tick(STA_STABLE_MS);
  CHECK(!mayapMqttTransportConnected() && backoff.failures == 1U && !net.open);
  resetWorld(); g_subackFirst = 0x80U; mayapMqttTransportBegin(); tick(1); tick(STA_STABLE_MS);
  CHECK(!mayapMqttTransportConnected() && backoff.failures == 1U && !net.open);
  resetWorld(); net.allowConnect = false; mayapMqttTransportBegin(); tick(1); tick(STA_STABLE_MS);
  CHECK(!mayapMqttTransportConnected() && backoff.failures == 1U);
  resetWorld(); g_silentBroker = true; mayapMqttTransportBegin(); tick(1); tick(STA_STABLE_MS);
  CHECK(!mayapMqttTransportConnected() && backoff.failures == 1U && !net.open);
  resetWorld(); g_tlsAllowed = false; mayapMqttTransportBegin(); tick(1); tick(STA_STABLE_MS);
  CHECK(net.connects == 0U);                              // no TLS lease: no handshake (Cloud/OTA own the heap)

  // 8. Gates: Wi-Fi/portal/yield/isolation stop the client; a graceful stop publishes offline then DISCONNECT only
  //    when the offline presence was really written (otherwise the broker must fire the LWT).
  resetWorld(); connectNow(); net.sent.clear();
  MayapUplink::setYieldWhy(MayapUplink::YieldWhy::AlarmHalfOpen); const uint32_t tlsClosesBefore = closeCount[static_cast<uint8_t>(CloseKind::CloudTls)];
  g_yield = true; tick(1);
  CHECK(logged("closed on purpose (cloud-tls) why=alarm:half-open-confirmed") && closeCount[static_cast<uint8_t>(CloseKind::CloudTls)] == tlsClosesBefore + 1U);
  MayapUplink::setYieldWhy(MayapUplink::YieldWhy::Other);
  sent = decodeSent();
  // A yield for the TLS slot is a PLANNED short close: no "offline" announcement and no DISCONNECT, so the broker keeps the device in
  // its reconnect grace (the Web shows "reconnecting") instead of declaring it offline.
  CHECK(!mayapMqttTransportConnected() && !net.open && sent.empty());
  // A radio change (Wi-Fi portal / credentials) is the one close that says goodbye: offline presence, then DISCONNECT.
  resetWorld(); connectNow(); net.sent.clear(); g_gateClosing = true; tick(1);
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
  // A supervisor re-init is a request, not a failure: no backoff step on top of its own pause, the very next pass reconnects.
  tick(1);
  CHECK(mayapMqttTransportConnected() && net.connects == 2U);   // initial + one fresh handshake right away
  // 10. WebSocket upgrade failures never reach MQTT and back off with nothing left open.
  for (UpgradeMode mode : {UpgradeMode::BadAccept, UpgradeMode::Status403, UpgradeMode::Extension, UpgradeMode::WrongProtocol,
                           UpgradeMode::Oversize, UpgradeMode::NoTerminator}) {
    resetWorld(); g_upgradeMode = mode; mayapMqttTransportBegin(); tick(1); tick(STA_STABLE_MS);
    CHECK(!mayapMqttTransportConnected() && backoff.failures >= 1U && !net.open);
    CHECK(decodeSent().empty());                          // no MQTT packet was sent over a failed upgrade
  }

  // 11. Server PING is answered with a masked PONG carrying the same payload; PONG/CLOSE handling.
  resetWorld(); connectNow(); net.sent.clear();
  injectBytes(serverFrame(std::vector<uint8_t>{'h', 'i', '!'}, 9));
  tick(1);
  CHECK(g_clientPongs == 1U && g_clientPongPayloads[0] == (std::vector<uint8_t>{'h', 'i', '!'}) && !g_clientFrameBad);
  injectBytes(serverFrame(std::vector<uint8_t>{}, 10)); tick(1);
  CHECK(mayapMqttTransportConnected());
  injectBytes(serverFrame(std::vector<uint8_t>{0x03, 0xE8}, 8)); tick(1);
  CHECK(!mayapMqttTransportConnected() && !net.open && logged("link lost"));

  // 12. MQTT packets split across WebSocket frames, several packets in one frame, 16-bit and 64-bit lengths,
  //     empty frames, and a frame boundary in the middle of a packet at the byte level.
  resetWorld(); connectNow(); net.sent.clear();
  {
    uint8_t buffer[4096];
    const std::string payload(300, 'p');
    size_t n = MayapMqttWire::encodePublish(buffer, sizeof(buffer), "mayap/v1/MAP-AABBCCDDEEFF/command", reinterpret_cast<const uint8_t *>(payload.data()), payload.size(), 0, false, 0);
    CHECK(n > 0);
    std::vector<uint8_t> whole(buffer, buffer + n);
    // fragmented message: binary(!fin) + continuation + continuation(fin), with an empty frame between
    injectBytes(serverFrame(std::vector<uint8_t>(whole.begin(), whole.begin() + 100), 2, false));
    injectBytes(serverFrame(std::vector<uint8_t>(), 0, false));
    injectBytes(serverFrame(std::vector<uint8_t>(whole.begin() + 100, whole.begin() + 250), 0, false));
    injectBytes(serverFrame(std::vector<uint8_t>(whole.begin() + 250, whole.end()), 0, true));
    // two packets in one frame, 64-bit length form
    std::vector<uint8_t> two = whole; two.insert(two.end(), whole.begin(), whole.end());
    injectBytes(serverFrame(two, 2, true, 8));
    tick(1);
    CHECK(MayapRealtimeInternal::g_delivered.size() == 3U);
    for (auto &d : MayapRealtimeInternal::g_delivered) CHECK(d.channel == "command" && d.payload == payload);
    CHECK(mayapMqttTransportConnected());
  }
  // The same stream delivered one byte at a time.
  resetWorld(); connectNow();
  {
    std::vector<uint8_t> frame = serverFrame(std::vector<uint8_t>{0xD0, 0x00}), pub;
    uint8_t buffer[512];
    const size_t n = MayapMqttWire::encodePublish(buffer, sizeof(buffer), "mayap/v1/MAP-AABBCCDDEEFF/session", reinterpret_cast<const uint8_t *>("{\"a\":1}"), 7, 0, false, 0);
    pub = serverFrame(std::vector<uint8_t>(buffer, buffer + n));
    frame.insert(frame.end(), pub.begin(), pub.end());
    for (uint8_t b : frame) { net.in.push_back(b); tick(1); }
    CHECK(MayapRealtimeInternal::g_delivered.size() == 1U && MayapRealtimeInternal::g_delivered[0].payload == "{\"a\":1}");
  }

  // 13. Protocol violations from the server drop the link (never a desynchronised stream).
  const std::vector<std::vector<uint8_t>> bad = {
    {0x82, 0x82, 0, 0, 0, 0, 0xD0, 0x00},                       // masked server frame
    {0x81, 0x01, 'x'},                                           // text frame
    {0xC2, 0x01, 0x00},                                          // RSV1 (compression nobody negotiated)
    {0x89, 0x7E, 0x00, 0x80},                                    // control frame with extended length
    {0x09, 0x00},                                                // fragmented control frame
    {0x80, 0x00},                                                // continuation without a message
    {0x82, 0x7F, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00},   // 64-bit length with high bits set
    {0x83, 0x00},                                                // reserved opcode
  };
  for (const auto &frame : bad) {
    resetWorld(); connectNow();
    injectBytes(frame); tick(1);
    CHECK(!mayapMqttTransportConnected() && !net.open && logged("link lost"));
  }

  // 14. Every frame the device ever sent was masked, FIN, and carried a known opcode (checked cumulatively).
  resetWorld(); connectNow(); mayapRealtimeUpdate(0);
  for (int i = 0; i < 20; ++i) { publishFromBridge("snapshot", "{\"t\":1}", 7U); tick(1); }
  publishFromBridge("ack", std::string(2000, 'a').c_str(), 2000U);
  CHECK(!g_clientFrameBad);
  g_gateClosing = true; tick(1);
  CHECK(g_clientCloses == 1U && !g_clientFrameBad);          // graceful stop (radio change): DISCONNECT then WebSocket CLOSE

  // 15. Realtime never storms and never idles: after a failed handshake the retries climb 0.5-1 s, 1-2 s, 2-4 s, 4-8 s, 7.5-15 s
  //     (equal jitter), a Wi-Fi that has been up for less than STA_STABLE_MS gets no TLS attempt, and a Cloud yield holds the next one off too.
  resetWorld(); g_connackCode = 5U; mayapMqttTransportBegin();
  tick(1); tick(STA_STABLE_MS - 500U); CHECK(net.connects == 0U);   // Wi-Fi up for too short a time: no handshake yet
  tick(500U); CHECK(net.connects == 1U && backoff.failures == 1U);
  {
    const uint32_t cap[] = {1000U, 2000U, 4000U, 8000U, 15000U, 30000U, 60000U};
    for (unsigned gap = 0U; gap < 6U; ++gap) {
      const unsigned before = net.connects;
      uint32_t waited = 0U;
      while (net.connects == before && waited < 70000U) { tick(100); waited += 100U; }
      CHECK(net.connects == before + 1U);
      CHECK(waited + 100U >= cap[gap] / 2U && waited <= cap[gap] + 100U);   // inside [cap/2, cap] of this rung
    }
  }
  unsigned attempts = net.connects;
  for (int i = 0; i < 240; ++i) tick(1000);                 // four minutes of a dead broker: the ladder tops out at 30-60 s, no storm
  CHECK(net.connects <= attempts + 10U);
  // The ladder resets only after the link stayed up STABLE_UP_MS: a link that connects and dies at once keeps escalating.
  resetWorld(); connectNow(); const uint8_t stepAfterFirst = backoff.step;
  backoff.step = 3U;                                         // as if three attempts had failed before this one succeeded
  for (unsigned i = 0U; i < 25U; ++i) { tick(1000); lastRxAt = g_millis; inflightClear(); }
  CHECK(backoff.step == 3U);                                 // up for 25 s: still not trusted
  for (unsigned i = 0U; i < 6U; ++i) { tick(1000); lastRxAt = g_millis; inflightClear(); }
  CHECK(mayapMqttTransportConnected() && backoff.step == 0U);   // up for 31 s: the ladder starts over
  (void)stepAfterFirst;
  resetWorld(); connectNow(); g_yield = true; tick(1); g_yield = false;
  tick(1000); CHECK(!mayapMqttTransportConnected());        // right after a Cloud yield: briefly held off
  tick(YIELD_RESUME_MS); tick(1000); CHECK(mayapMqttTransportConnected());   // ...then back within seconds, not 15 s+
  // A busy TLS lease (Cloud mid-handshake) is contention: retried every BUSY_RETRY_MS, never escalates the backoff.
  resetWorld(); g_tlsAllowed = false; mayapMqttTransportBegin(); tick(1); tick(STA_STABLE_MS);
  for (int i = 0; i < 40; ++i) tick(1000);
  CHECK(net.connects == 0U && backoff.failures == 0U && backoff.step == 0U);
  g_tlsAllowed = true; tick(BUSY_RETRY_MS + 1000U); CHECK(mayapMqttTransportConnected());
  resetWorld(); connectNow(); g_networkStatus.connected = false; tick(1);
  g_networkStatus.connected = true; tick(1); tick(STA_STABLE_MS - 500U);
  CHECK(!mayapMqttTransportConnected());                   // Wi-Fi flapped: waits for stability again
  tick(STA_STABLE_MS); CHECK(mayapMqttTransportConnected());

  // 16. Cloud asking for the lease while the handshake runs aborts that attempt but is contention, not a
  //     broker failure: no backoff escalation, retried within seconds once the yield is over.
  resetWorld(); g_silentBroker = true; g_yieldAfterCalls = 4U; mayapMqttTransportBegin(); tick(1); tick(STA_STABLE_MS);
  CHECK(net.connects == 1U && !mayapMqttTransportConnected() && backoff.failures == 0U && backoff.step == 0U);
  CHECK(logged("connect aborted"));
  g_yield = false; g_silentBroker = false; g_yieldAfterCalls = 0U;
  tick(BUSY_RETRY_MS + 1000U); CHECK(mayapMqttTransportConnected());

  // 17. Uplink (alarms / heartbeat on mayap/v1/<id>/alarm|heartbeat): published QoS1 in FIFO order, a PUBACK means
  //     stored, a missing PUBACK never drops the link (the broker withholds it when the Worker did not store
  //     the event), and losing the socket fails whatever was queued or in flight.
  {
    using namespace MayapUplink;
    auto clearSlots = [] { onLinkDown(); for (int8_t i = 0; i < static_cast<int8_t>(SLOTS); ++i) finish(i); };
    auto publishes = [](size_t from) {
      std::vector<std::pair<std::string, std::string>> out; std::vector<uint16_t> ids; std::vector<uint8_t> flags;
      for (const Packet &p : decodeSent(from)) {
        MayapMqttWire::PublishView v;
        if (p.type != 3 || !MayapMqttWire::parsePublish(p.flags, p.body.data(), p.body.size(), v)) continue;
        out.emplace_back(str(v.topic, v.topicLength), str(v.payload, v.payloadLength)); ids.push_back(v.packetId); flags.push_back(p.flags);
      }
      return std::make_tuple(out, ids, flags);
    };
    resetWorld(); clearSlots(); connectNow();
    const std::string prefix = std::string("mayap/v1/") + MayapRealtimeInternal::deviceId + "/";
    CHECK(available(g_millis));
    const char *alarm1 = "{\"event_id\":\"e1\",\"alarm_type\":\"FAULT_130\",\"state\":\"active\"}";
    const char *alarm2 = "{\"event_id\":\"e2\",\"alarm_type\":\"FAULT_130\",\"state\":\"resolved\"}";
    const char *beat = "{\"batch_running\":true}";
    const int8_t s1 = offer(Kind::Alarm, alarm1, strlen(alarm1), g_millis), s2 = offer(Kind::Alarm, alarm2, strlen(alarm2), g_millis),
                 s3 = offer(Kind::Heartbeat, beat, strlen(beat), g_millis);
    CHECK(s1 >= 0 && s2 >= 0 && s3 >= 0 && peek(s1) == State::Queued);
    net.sent.clear(); const size_t before = net.sent.size();
    inflightClear();                                                                   // presence PUBACK is not what this checks
    tick(20);
    auto sentOut = publishes(before);
    const auto &topics = std::get<0>(sentOut); const auto &ids = std::get<1>(sentOut); const auto &flags = std::get<2>(sentOut);
    CHECK(topics.size() == 3U);
    CHECK(topics[0].first == prefix + "alarm" && topics[0].second == alarm1);          // oldest first: active before resolved
    CHECK(topics[1].first == prefix + "alarm" && topics[1].second == alarm2);
    CHECK(topics[2].first == prefix + "heartbeat" && topics[2].second == beat);
    CHECK(flags[0] == 0x02 && flags[2] == 0x02);                                       // QoS1, DUP=0, retain=0
    CHECK(peek(s1) == State::Sent && peek(s3) == State::Sent && inflightCount == 0U);   // uplink is not in the watchdog window
    for (int i = 0; i < 25; ++i) tick(1000);                                           // 25 s without any PUBACK: link stays up
    CHECK(mayapMqttTransportConnected() && net.open);
    inject({0x40, 0x02, static_cast<uint8_t>(ids[0] >> 8U), static_cast<uint8_t>(ids[0])}); tick(1);
    CHECK(peek(s1) == State::Acked && peek(s2) == State::Sent);
    CHECK(finish(s1) == State::Acked && peek(s1) == State::Free);                      // collected
    CHECK(finish(s2) == State::Sent && peek(s2) == State::Orphan);                     // Cloud gave up waiting
    inject({0x40, 0x02, static_cast<uint8_t>(ids[1] >> 8U), static_cast<uint8_t>(ids[1])}); tick(1);
    CHECK(peek(s2) == State::Free);                                                    // late PUBACK of an orphan frees it
    // Bounded: four slots, an oversized payload is refused, the bridge itself can never publish uplink channels.
    clearSlots(); onLinkUp();
    for (uint8_t i = 0; i < SLOTS; ++i) CHECK(offer(Kind::Alarm, alarm1, strlen(alarm1), g_millis) >= 0);
    CHECK(offer(Kind::Alarm, alarm1, strlen(alarm1), g_millis) == -1);
    clearSlots(); onLinkUp();
    CHECK(offer(Kind::Alarm, std::string(PAYLOAD_MAX, 'x').c_str(), PAYLOAD_MAX, g_millis) == -1);
    CHECK(!publishFromBridge("alarm", alarm1, strlen(alarm1)) && !publishFromBridge("heartbeat", beat, strlen(beat)));
    // A dead socket fails queued and in-flight slots at once and uplink becomes unavailable (Cloud falls back to HTTPS).
    resetWorld(); clearSlots(); connectNow();
    const int8_t q1 = offer(Kind::Alarm, alarm1, strlen(alarm1), g_millis); tick(20);
    const int8_t q2 = offer(Kind::Alarm, alarm2, strlen(alarm2), g_millis);
    CHECK(peek(q1) == State::Sent && peek(q2) == State::Queued);
    g_networkStatus.connected = false; tick(1);
    CHECK(!available(g_millis) && peek(q1) == State::Failed && peek(q2) == State::Failed);
    CHECK(finish(q1) == State::Failed && finish(q2) == State::Failed);
    CHECK(offer(Kind::Alarm, alarm1, strlen(alarm1), g_millis) == -1);                 // no link: nothing can be queued
    // A missing PUBACK never makes the uplink unavailable: only the owner's link state does.
    resetWorld(); clearSlots(); connectNow();
    CHECK(available(g_millis));
    const int8_t late = offer(Kind::Alarm, alarm1, strlen(alarm1), g_millis); tick(20);
    for (int i = 0; i < 40; ++i) { inject({0xD0, 0x00}); tick(1000); }          // 40 s, broker talking, never a PUBACK for the alarm
    CHECK(available(g_millis) && mayapMqttTransportConnected() && !logged("link lost"));
    finish(late); resetWorld(); clearSlots(); connectNow();
    // A slot nobody collects cannot live forever.
    const int8_t old = offer(Kind::Alarm, alarm1, strlen(alarm1), g_millis); tick(20); CHECK(peek(old) == State::Sent);
    expire(g_millis + SLOT_MAX_AGE_MS + 1U); CHECK(peek(old) == State::Failed);
    clearSlots();
  }

  // 18. Exclusive TLS: the socket counts as the one TLS context from the start of the handshake until it is closed
  //     (any failure path included), so an HTTPS session can only start after MQTT released its memory.
  resetWorld(); mqttTlsResidentRef() = false;
  CHECK(!mayapMqttTlsResident());
  connectNow(); CHECK(mayapMqttTlsResident() && mayapMqttTransportConnected());
  g_yield = true; tick(1); CHECK(!mayapMqttTlsResident() && !net.open);              // Cloud asked: closed and released
  g_yield = false;
  for (uint8_t refusedCode : {5U, 4U}) {                                              // broker refuses CONNECT: never left resident
    resetWorld(); g_connackCode = refusedCode; mayapMqttTransportBegin(); tick(1); tick(STA_STABLE_MS);
    CHECK(!mayapMqttTransportConnected() && !mayapMqttTlsResident() && !net.open);
  }
  resetWorld(); net.allowConnect = false; mayapMqttTransportBegin(); tick(1); tick(STA_STABLE_MS);
  CHECK(!mayapMqttTlsResident());                                                     // TLS connect failure
  resetWorld(); g_silentBroker = true; mayapMqttTransportBegin(); tick(1); tick(STA_STABLE_MS);
  CHECK(!mayapMqttTlsResident());                                                     // handshake timeout
  resetWorld(); connectNow(); CHECK(mayapMqttTlsResident());
  g_networkStatus.connected = false; tick(1); CHECK(!mayapMqttTlsResident());         // Wi-Fi lost
  resetWorld(); connectNow(); g_pressure = true; tick(1); CHECK(!mayapMqttTlsResident());   // memory pressure
  g_pressure = false;
  resetWorld(); connectNow(); CHECK(mayapMqttTlsResident());                           // link failure (broker silent past the keep-alive)
  mayapMqttTransportRecover(g_millis); CHECK(!mayapMqttTlsResident() && !net.open);     // supervisor recovery path
  resetWorld(); connectNow(); g_gateClosing = true; tick(1); CHECK(!mayapMqttTlsResident() && !net.open);   // graceful drain (OTA/portal)
  g_gateClosing = false;
  // Ordering: on EVERY close path above the socket was released while the transport still counted as THE resident TLS context,
  // and the flag dropped only afterwards - so the Cloud task could never open an HTTPS session on memory that was still in use.
  CHECK(mqttTlsStopRaces() == 0U);

  // 19. Link health published for alarm routing: Up on connect; a deliberate close is Closed (no failure, short hold-off); an
  //     attempt that fails or an established link that dies is Down with a failure count; losses are remembered for flapping.
  {
    using namespace MayapUplink;
    resetWorld(); Internal::health = Health{}; connectNow();
    Health h = healthSnapshot(); CHECK(h.link == Link::Up && h.failures == 0U && h.rttEwmaMs == 0U);
    g_yield = true; tick(1); h = healthSnapshot();
    CHECK(h.link == Link::Closed && h.failures == 0U && h.nextAttemptAt != 0U && recentLosses(h, g_millis, 120000U) == 0U);
    g_yield = false;
    resetWorld(); net.allowConnect = false; mayapMqttTransportBegin(); tick(1); tick(STA_STABLE_MS); h = healthSnapshot();
    CHECK(h.link == Link::Down && h.failures >= 1U && h.nextAttemptAt != 0U);
    // A supervisor re-init is a request, not evidence against the transport: Closed, no failure, not a loss.
    resetWorld(); connectNow(); mayapMqttTransportRecover(g_millis); h = healthSnapshot();
    CHECK(h.link == Link::Closed && h.failures == 0U && recentLosses(h, g_millis, 120000U) == 0U);
    // A socket that dies under us IS a loss, remembered for the flapping window.
    resetWorld(); connectNow(); net.open = false; tick(1); h = healthSnapshot();
    CHECK(h.link == Link::Down && h.failures == 1U && recentLosses(h, g_millis, 120000U) == 1U && logged("link lost reason=socket-closed"));
    CHECK(recentLosses(h, g_millis + 130000U, 120000U) == 0U);                    // old losses age out of the flapping window
    for (int n = 0; n < 2; ++n) { resetWorld(); connectNow(); net.open = false; tick(1); }
    h = healthSnapshot(); CHECK(recentLosses(h, g_millis, 120000U) >= 3U);       // three quick losses = flapping
  }

  // 20. The owner never waits inside net.write(): a socket that cannot take bytes (TCP window full) neither blocks the task nor kills
  //     the link. Control frames wait in order, Reliable frames are refused (their owners retry), Droppable ones are dropped, and only
  //     a socket that stays unwritable for TX_STALL_MS while the broker is silent too is declared dead - with the reason in the log.
  {
    resetWorld(); connectNow(); net.sent.clear(); inflightClear();
    net.txReady = false;
    CHECK(!publishFromBridge("ack", "{\"v\":2}", 7U) && gateRefused && !linkFailed && txRefused == 1U);
    CHECK(!publishFromBridge("snapshot", "{\"t\":1}", 7U) && txDropped == 1U && !linkFailed);
    CHECK(net.sent.empty() && inflightCount == 0U && mayapMqttTransportConnected());     // nothing written, nothing tracked, link intact
    CHECK(bulkSlotsFree() == 0U);                                                       // the bridge is told to wait
    injectPublish(std::string("mayap/v1/") + MayapRealtimeInternal::deviceId + "/command", "{\"x\":1}", 1, 77);
    tick(5); lastRxAt = g_millis;
    CHECK(ctrlCount == 1U && net.sent.empty() && mayapMqttTransportConnected());         // the PUBACK for the command waits in the control queue
    net.txReady = true; tick(5);
    {
      const std::vector<Packet> out = decodeSent();
      CHECK(ctrlCount == 0U && stallSince == 0U && !txBlocked && out.size() >= 1U && out[0].type == 4 && out[0].body.size() == 2U &&
            ((out[0].body[0] << 8) | out[0].body[1]) == 77);                             // flushed in order once the socket recovered
    }
    CHECK(bulkSlotsFree() == QOS1_INFLIGHT_MAX - QOS1_RESERVED_FOR_ACK);

    // Alarms: a queued uplink slot is NOT failed because the socket is momentarily unwritable - it goes out as soon as it can.
    {
      using namespace MayapUplink;
      resetWorld(); onLinkDown(); for (int8_t i = 0; i < static_cast<int8_t>(SLOTS); ++i) finish(i);
      connectNow(); inflightClear();
      const char *alarm = "{\"event_id\":\"g1\",\"alarm_type\":\"FAULT_130\",\"state\":\"active\"}";
      const int8_t slot = offer(Kind::Alarm, alarm, strlen(alarm), g_millis);
      CHECK(slot >= 0);
      net.sent.clear(); net.txReady = false; tick(20); lastRxAt = g_millis;
      CHECK(peek(slot) == State::Queued && net.sent.empty() && mayapMqttTransportConnected());
      net.txReady = true; tick(20);
      CHECK(peek(slot) == State::Sent && !decodeSent().empty());
      onLinkDown(); finish(slot);
    }

    // A stalled uplink with a live downlink stays up; with a silent broker too it is declared dead after TX_STALL_MS.
    resetWorld(); connectNow(); inflightClear();
    net.txReady = false; publishFromBridge("ack", "{\"v\":2}", 7U);
    for (unsigned i = 0U; i < 40U; ++i) { tick(1000); lastRxAt = g_millis; }
    CHECK(mayapMqttTransportConnected());                                                // 40 s unwritable but the broker keeps talking
    resetWorld(); connectNow(); inflightClear();
    net.txReady = false; publishFromBridge("ack", "{\"v\":2}", 7U);
    for (unsigned i = 0U; i < 24U && mayapMqttTransportConnected(); ++i) tick(1000);
    CHECK(mayapMqttTransportConnected());                                                // 24 s: not yet
    for (unsigned i = 0U; i < 4U && mayapMqttTransportConnected(); ++i) tick(1000);
    CHECK(!mayapMqttTransportConnected() && logged("link lost reason=tx-stall"));
    // The reason of a plain write failure is explained too, and the socket recovers into a fresh connection quickly.
    resetWorld(); connectNow(); net.failWrite = true; publishFromBridge("ack", "{\"v\":2}", 7U); tick(1);
    CHECK(!mayapMqttTransportConnected() && logged("link lost reason=tx-fail"));
    net.failWrite = false; tick(1100); tick(100);
    CHECK(mayapMqttTransportConnected());                                                // first retry is within ~1 s
  }

  // 21. "received" acknowledgements are advisory: shed when the QoS1 window is nearly full, never the terminal ack.
  {
    resetWorld(); connectNow(); inflightClear(); net.sent.clear();
    const char *received = "{\"v\":2,\"phase\":\"received\",\"requestId\":\"r1\"}";
    const char *completed = "{\"v\":2,\"phase\":\"completed\",\"requestId\":\"r1\"}";
    CHECK(publishFromBridge("ack", received, strlen(received)) && inflightCount == 1U && advisoryShed == 0U);   // room: sent as always
    while (inflightCount < ACK_ADVISORY_SHED_AT) CHECK(publishFromBridge("ack", completed, strlen(completed)));
    const size_t sentBefore = net.sent.size();
    CHECK(publishFromBridge("ack", received, strlen(received)) && advisoryShed == 1U && net.sent.size() == sentBefore && inflightCount == ACK_ADVISORY_SHED_AT);
    CHECK(publishFromBridge("ack", completed, strlen(completed)) && inflightCount == ACK_ADVISORY_SHED_AT + 1U);  // terminal ack still goes out
    CHECK(bulkSlotsFree() == 0U);                                                                                  // bulk reports wait for the window
  }

  // 22. Name resolution: reused after a good handshake, forgotten after any failure, and a failed lookup (which arduino-esp32 reports
  //     as a non-zero "error code") is a failed attempt, never a dial to 0.0.0.0.
  {
    resetWorld(); connectNow();
    CHECK(dnsLookups() == 1U && net.lastIp == 0x0100000AU && !logged("(cached)"));
    net.open = false; tick(1); tick(1100); tick(100);                                    // lost, reconnect: the address is reused
    CHECK(mayapMqttTransportConnected() && dnsLookups() == 1U && logged("(cached)"));
    resetWorld(); dnsOk() = false; mayapMqttTransportBegin(); tick(1); tick(STA_STABLE_MS);
    CHECK(dnsLookups() == 1U && net.connects == 0U && backoff.failures == 1U && logged("dns failed"));
    dnsOk() = true; for (unsigned i = 0U; i < 12U && !mayapMqttTransportConnected(); ++i) tick(100);
    CHECK(mayapMqttTransportConnected() && dnsLookups() == 2U);
    // A failed connect through a remembered address drops it: the next attempt resolves afresh.
    brokerIpTrusted = true; brokerIp = 0x0200000AU; net.stop(); connected = false; net.allowConnect = false; backoff.nextAttemptAt = g_millis;
    tick(1);
    CHECK(!brokerIpTrusted && backoff.failures >= 1U);
  }

  // 23. The loop probe and phase timing are cheap observables: the heap low-water mark is tracked per pass.
  resetWorld(); connectNow(); tick(1);
  CHECK(heapLowWater != 0xFFFFFFFFUL && logged("dns=") && logged("tls=") && logged("ws=") && logged("mqtt="));

  printf("mqtt transport host tests PASS\n");
  return 0;
}
