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
unsigned g_beats = 0U, g_realtimeUpdates = 0U;
NetworkStatus g_networkStatus{ConnectivityMode::Online, true};
std::vector<MayapRealtimeInternal::Delivered> MayapRealtimeInternal::g_delivered;

#include "mqtt_transport.h"

int main(int argc, char **argv) {
  if (argc < 3) return 2;
  snprintf(MayapRealtimeInternal::deviceId, sizeof(MayapRealtimeInternal::deviceId), "%s", argv[1]);
  snprintf(g_mqttKey, sizeof(g_mqttKey), "%s", argv[2]);
  const uint32_t runMs = (argc > 3 ? static_cast<uint32_t>(atoi(argv[3])) : 30U) * 1000U;
  setvbuf(stdout, nullptr, _IOLBF, 0);
  mayapMqttTransportBegin();
  const uint32_t start = millis();
  size_t delivered = 0U;
  while (millis() - start < runMs) {
    mayapMqttTransportUpdate(millis());
    for (const auto &line : g_log) printf("LOG %s", line.c_str());
    g_log.clear();
    for (; delivered < MayapRealtimeInternal::g_delivered.size(); ++delivered) {
      const auto &d = MayapRealtimeInternal::g_delivered[delivered];
      printf("DELIVERED %s %s\n", d.channel.c_str(), d.payload.c_str());
      if (d.channel == "command") {
        const std::string ack = "{\"phase\":\"completed\",\"echo\":" + std::to_string(d.payload.size()) + "}";
        printf("ACKED %d\n", MayapMqttInternal::publishFromBridge("ack", ack.c_str(), ack.size()) ? 1 : 0);
      }
    }
    usleep(20000);
  }
  printf("DONE connected=%d\n", mayapMqttTransportConnected() ? 1 : 0);
  return 0;
}
