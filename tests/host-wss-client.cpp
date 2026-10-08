// Interop client: the REAL mqtt_transport.h / mqtt_wire.h / mqtt_ws.h driving a real TCP socket against the
// real broker Durable Object running on workerd (tools/e2e/firmware_wss_interop.cjs). TLS is the only layer
// not exercised (it is WiFiClientSecure's job on the device); framing, handshake, MQTT and the broker are real.
//   host-wss-client <deviceId> <password-hex> [seconds]
// Prints "LOG ...", "DELIVERED <channel> <payload>" lines; answers every `command` with an `ack`.
#define MAYAP_LIVE_SOCKET 1
#include "mayap_stubs.h"
#include <cstdlib>

std::vector<std::string> g_log;
char g_mqttKey[65] = "";
bool g_gateClosing = false, g_isolated = false, g_pressure = false, g_yield = false, g_ioEnterOk = true, g_tlsAllowed = true;
unsigned g_yieldAfterCalls = 0U;
unsigned g_beats = 0U, g_realtimeUpdates = 0U;
NetworkStatus g_networkStatus{ConnectivityMode::Online, true};
std::vector<MayapRealtimeInternal::Delivered> MayapRealtimeInternal::g_delivered;

#include "mqtt_transport.h"
#include <cstdlib>

int main(int argc, char **argv) {
  if (argc < 3) return 2;
  snprintf(MayapRealtimeInternal::deviceId, sizeof(MayapRealtimeInternal::deviceId), "%s", argv[1]);
  snprintf(g_mqttKey, sizeof(g_mqttKey), "%s", argv[2]);
  const uint32_t runMs = (argc > 3 ? static_cast<uint32_t>(atoi(argv[3])) : 30U) * 1000U;
  setvbuf(stdout, nullptr, _IOLBF, 0);
  mayapMqttTransportBegin();
  const uint32_t start = millis();
  size_t delivered = 0U;
  // UPLINK_ALARMS=n: once the link is up offer n alarms through the REAL MayapUplink mailbox (the path sendAlarmsUplink uses) and
  // report offer -> PUBACK time per alarm. UPLINK_WAIT_MS bounds how long each is awaited (the firmware's adaptive wait: 1.5-4 s).
  const int alarmsToSend = getenv("UPLINK_ALARMS") ? atoi(getenv("UPLINK_ALARMS")) : 0;
  const uint32_t waitMs = getenv("UPLINK_WAIT_MS") ? static_cast<uint32_t>(atoi(getenv("UPLINK_WAIT_MS"))) : 4000U;
  std::vector<std::string> pendingAcks; uint32_t ackRetryAt = 0U; unsigned long ackRefusals = 0U;
  int offeredCount = 0; int8_t slot = -1; uint32_t offeredAt = 0U, nextOfferAt = 0U;
  while (millis() - start < runMs) {
    mayapMqttTransportUpdate(millis());
    if (alarmsToSend > 0 && mayapMqttTransportConnected()) {
      const uint32_t now = millis();
      if (slot < 0 && offeredCount < alarmsToSend && static_cast<int32_t>(now - nextOfferAt) >= 0) {
        char json[200];
        snprintf(json, sizeof(json), "{\"event_id\":\"hostfw-%04d\",\"alarm_type\":\"FAULT_%d\",\"severity\":\"critical\",\"state\":\"active\",\"message\":\"host firmware alarm\"}",
                 offeredCount, 130 + offeredCount);
        slot = MayapUplink::offer(MayapUplink::Kind::Alarm, json, strlen(json), now);
        offeredAt = now;
        if (slot < 0) { printf("ALARM %d REFUSED\n", offeredCount); ++offeredCount; }
      } else if (slot >= 0) {
        const MayapUplink::State state = MayapUplink::peek(slot);
        if (state == MayapUplink::State::Acked || static_cast<uint32_t>(now - offeredAt) >= waitMs || state == MayapUplink::State::Failed) {
          const MayapUplink::State done = MayapUplink::finish(slot);
          printf("ALARM %d %s %lu\n", offeredCount, done == MayapUplink::State::Acked ? "ACKED" : "NOACK", static_cast<unsigned long>(now - offeredAt));
          if (done != MayapUplink::State::Acked) MayapUplink::requestProbe(now);   // what awaitUplink() does on a timeout
          slot = -1; ++offeredCount; nextOfferAt = now + 200U;
        }
      }
    }
    for (const auto &line : g_log) printf("LOG %s", line.c_str());
    g_log.clear();
    for (; delivered < MayapRealtimeInternal::g_delivered.size(); ++delivered) {
      const auto &d = MayapRealtimeInternal::g_delivered[delivered];
      printf("DELIVERED %s %s\n", d.channel.c_str(), d.payload.c_str());
      if (d.channel == "command")                       // the bridge's ack outbox: kept until the transport accepts it
        pendingAcks.push_back("{\"phase\":\"completed\",\"echo\":" + std::to_string(d.payload.size()) + "}");
    }
    while (!pendingAcks.empty()) {
      if (static_cast<int32_t>(millis() - ackRetryAt) < 0) break;
      if (!MayapMqttInternal::publishFromBridge("ack", pendingAcks.front().c_str(), pendingAcks.front().size())) {
        ++ackRefusals; ackRetryAt = millis() + 50U; break;      // same back-off idea as drainAckOutbox()
      }
      printf("ACKED 1\n");
      pendingAcks.erase(pendingAcks.begin());
    }
    usleep(20000);
  }
  {
    const MayapUplink::Health h = MayapUplink::healthSnapshot();
    printf("STAT ackRefusals=%lu inflight=%u refusedAck=%lu refusedBulk=%lu expired=%lu probes=%lu answered=%lu closes=%lu/%lu/%lu/%lu lost=%lu rtt=%u\n",
           ackRefusals, static_cast<unsigned>(MayapMqttInternal::inflightCount), static_cast<unsigned long>(MayapMqttInternal::refusedAck),
           static_cast<unsigned long>(MayapMqttInternal::refusedBulk), static_cast<unsigned long>(MayapMqttInternal::qos1Expired),
           static_cast<unsigned long>(h.probes), static_cast<unsigned long>(h.probesAnswered),
           static_cast<unsigned long>(MayapMqttInternal::closeCount[0]), static_cast<unsigned long>(MayapMqttInternal::closeCount[1]),
           static_cast<unsigned long>(MayapMqttInternal::closeCount[2]), static_cast<unsigned long>(MayapMqttInternal::closeCount[3]),
           static_cast<unsigned long>(MayapMqttInternal::closeCount[4]), static_cast<unsigned>(h.rttEwmaMs));
  }
  printf("DONE connected=%d\n", mayapMqttTransportConnected() ? 1 : 0);
  return 0;
}
