#pragma once
// Host stand-ins for everything mqtt_transport.h pulls from the Arduino/ESP32 world and from
// the rest of the firmware. mqtt_transport.h and mqtt_wire.h are the REAL production files
// (copied next to these stubs by tests/mqtt-transport.test.cjs); only their collaborators are fake.
#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

extern uint32_t g_millis;
extern uint32_t g_epoch;
inline uint32_t millis() { return g_millis; }
inline time_t fake_time(time_t *) { return static_cast<time_t>(g_epoch); }
#define time(x) fake_time(x)

typedef uint32_t TickType_t;
typedef int portMUX_TYPE;
#define pdMS_TO_TICKS(x) (x)
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
inline void vTaskDelay(TickType_t ticks) { g_millis += ticks; }   // the owner task sleeps: time moves
inline const char *pcTaskGetName(void *) { return "mayap_mqtt"; }
inline int xPortGetCoreID() { return 0; }
inline unsigned uxTaskPriorityGet(void *) { return 2U; }
struct FakeEsp { uint32_t getFreeHeap() { return 90000U; } uint32_t getMaxAllocHeap() { return 40000U; } };
static FakeEsp ESP;

extern std::vector<std::string> g_log;
inline void mayapSerialPrintf(bool, const char *format, ...) {
  char text[512];
  va_list args; va_start(args, format); vsnprintf(text, sizeof(text), format, args); va_end(args);
  g_log.push_back(text);
}

constexpr char TLS_ROOT_CA[] = "fake-pem";
namespace MayapProtocol { constexpr size_t FRAME_NORMAL_CAP = 2048U; }

// ---- device identity (NVS) ----
extern char g_mqttKey[65]; extern char g_mqttHost[64]; extern uint16_t g_mqttPort;
inline const char *mayapMqttKey() { return g_mqttKey; }
inline const char *mayapMqttHost() { return g_mqttHost; }
inline uint16_t mayapMqttPort() { return g_mqttPort; }

// ---- service / online gates ----
namespace MayapRecovery {
enum class Service { Mqtt };
inline uint32_t age(uint32_t now, uint32_t then) { return now - then; }
}
extern bool g_gateClosing, g_isolated, g_pressure, g_yield, g_ioEnterOk, g_tlsAllowed;
extern unsigned g_beats;
inline void mayapServiceBeat(MayapRecovery::Service) { ++g_beats; }
inline bool mayapServiceIsolated(MayapRecovery::Service, uint32_t) { return g_isolated; }
inline bool mayapOnlineMemoryPressure() { return g_pressure; }
inline bool mayapCloudTlsYieldRequested(uint32_t) { return g_yield; }
inline void mayapOnlineOwnerQuiet(MayapRecovery::Service) {}
inline bool mayapOnlineIoEnter(MayapRecovery::Service) { return g_ioEnterOk; }
inline void mayapOnlineIoLeave(MayapRecovery::Service) {}
inline bool mayapWifiPortalExclusiveRequested() { return g_gateClosing; }
inline bool mayapRadioRecoveryRequested() { return false; }
enum class ConnectivityMode { Offline, Online };
struct NetworkStatus { ConnectivityMode requestedMode; bool connected; };
extern NetworkStatus g_networkStatus;
inline NetworkStatus mayapGetRawNetworkStatus() { return g_networkStatus; }
enum class MayapTlsKind { Mqtt, Cloud };
class MayapTlsOperation {
 public:
  explicit MayapTlsOperation(MayapTlsKind = MayapTlsKind::Mqtt) {}
  explicit operator bool() const { return g_tlsAllowed; }
};
struct BackoffTimer {
  uint32_t next = 0U; unsigned failures = 0U, successes = 0U;
  void reset(uint32_t now) { next = now; }
  bool ready(uint32_t now) const { return now >= next; }
  void onFailure(uint32_t now) { ++failures; next = now + 5000U; }
  void onSuccess() { ++successes; }
};

// ---- the TLS socket ----
struct WiFiClientSecure {
  bool allowConnect = true, open = false, failWrite = false;
  unsigned connects = 0U, stops = 0U;
  std::vector<uint8_t> in;
  std::vector<std::vector<uint8_t>> sent;
  std::function<void(const std::vector<uint8_t> &)> onWrite;
  void setCACert(const char *) {}
  void setConnectionTimeout(uint32_t) {}
  void setHandshakeTimeout(uint32_t) {}
  int lastError(char *buffer, size_t size) { snprintf(buffer, size, "fake-tls-error"); return -1; }
  bool connect(const char *, uint16_t) { if (!allowConnect) return false; open = true; ++connects; return true; }
  size_t write(const uint8_t *data, size_t length) {
    if (failWrite || !open) return 0U;
    sent.emplace_back(data, data + length);
    if (onWrite) onWrite(sent.back());
    return length;
  }
  int available() { return open ? static_cast<int>(in.size()) : 0; }
  int read(uint8_t *buffer, size_t length) {
    if (!open || in.empty()) return 0;
    const size_t n = length < in.size() ? length : in.size();
    memcpy(buffer, in.data(), n);
    in.erase(in.begin(), in.begin() + static_cast<long>(n));
    return static_cast<int>(n);
  }
  bool connected() { return open; }
  void stop() { open = false; in.clear(); ++stops; }
};

// ---- Transaction V2 bridge surface used by the transport ----
extern unsigned g_realtimeUpdates;
inline void mayapRealtimeUpdate(uint32_t) { ++g_realtimeUpdates; }
namespace MayapRealtimeInternal {
typedef bool (*PublishCallback)(const char *, const char *, size_t);
static PublishCallback publishCallback = nullptr;
static char deviceId[24] = "MAP-AABBCCDDEEFF";
static portMUX_TYPE realtimeMux = 0;
static bool knownConfigValid = true, configDirty = false, forceSnapshotPublish = false;
static uint32_t lastSnapshotPublishAt = 77U;
struct Delivered { std::string channel, payload; };
extern std::vector<Delivered> g_delivered;
inline void dispatchApplicationMessage(const char *channel, const uint8_t *payload, size_t length) {
  g_delivered.push_back({channel, std::string(reinterpret_cast<const char *>(payload), length)});
}
inline bool publishPresence(bool online) {
  const char *json = online ? "{\"online\":true}" : "{\"online\":false}";
  return publishCallback && publishCallback("presence", json, strlen(json));
}
}  // namespace MayapRealtimeInternal
