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
#include "mqtt_tx_arbiter.h"

#ifdef MAYAP_LIVE_SOCKET   // tests/host-wss-client.cpp: real TCP to a local broker (TLS itself is not part of the test)
#include <arpa/inet.h>
#include <chrono>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
inline uint32_t millis() {
  using namespace std::chrono;
  return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
#else
extern uint32_t g_millis;
extern uint32_t g_epoch;
inline uint32_t millis() { return g_millis; }
inline time_t fake_time(time_t *) { return static_cast<time_t>(g_epoch); }
#define time(x) fake_time(x)
#endif

typedef uint32_t TickType_t;
typedef int portMUX_TYPE;
#define pdMS_TO_TICKS(x) (x)
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#ifdef MAYAP_LIVE_SOCKET
inline void vTaskDelay(TickType_t ticks) { usleep(ticks * 1000U); }
#else
inline void vTaskDelay(TickType_t ticks) { g_millis += ticks; }   // the owner task sleeps: time moves
#endif
inline const char *pcTaskGetName(void *) { return "mayap_mqtt"; }
inline int xPortGetCoreID() { return 0; }
inline unsigned uxTaskPriorityGet(void *) { return 2U; }
struct FakeEsp { uint32_t getFreeHeap() { return 90000U; } uint32_t getMaxAllocHeap() { return 40000U; } };
static FakeEsp ESP;

// Arduino IPAddress / WiFi.hostByName as used by the transport. Like arduino-esp32 3.x, a failed lookup returns a NON-ZERO error code.
struct IPAddress {
  uint32_t v = 0U;
  IPAddress() {}
  IPAddress(uint32_t a) : v(a) {}
  operator uint32_t() const { return v; }
};
inline bool &dnsOk() { static bool ok = true; return ok; }
inline unsigned &dnsLookups() { static unsigned n = 0U; return n; }
struct FakeWiFi {
  int hostByName(const char *host, IPAddress &ip) {
    ++dnsLookups();
    if (!dnsOk()) return 202;                       // EAI_FAIL: truthy, exactly the trap in the core
#ifdef MAYAP_LIVE_SOCKET
    in_addr a{};
    if (inet_pton(AF_INET, host, &a) != 1) return 202;
    ip = IPAddress(a.s_addr);
#else
    (void)host;
    ip = IPAddress(0x0100000AU);
#endif
    return 1;
  }
};
static FakeWiFi WiFi;

extern std::vector<std::string> g_log;
inline void mayapSerialPrintf(bool, const char *format, ...) {
  char text[512];
  va_list args; va_start(args, format); vsnprintf(text, sizeof(text), format, args); va_end(args);
  g_log.push_back(text);
}

constexpr char TLS_ROOT_CA[] = "fake-pem";
namespace MayapProtocol { constexpr size_t FRAME_NORMAL_CAP = 2048U; }

// ---- device identity (NVS) ----
extern char g_mqttKey[65];
inline const char *mayapMqttKey() { return g_mqttKey; }
inline uint32_t esp_random() { static uint32_t x = 0x2545F491U; x = x * 1664525U + 1013904223U; return x; }

// ---- service / online gates ----
namespace MayapRecovery {
enum class Service { Mqtt };
inline uint32_t age(uint32_t now, uint32_t then) { return now - then; }
}
extern bool g_gateClosing, g_isolated, g_pressure, g_yield, g_ioEnterOk, g_tlsAllowed;
extern unsigned g_yieldAfterCalls;   // >0: the Nth mayapCloudTlsYieldRequested() call flips g_yield (Cloud asks mid-handshake)
extern unsigned g_beats;
inline void mayapServiceBeat(MayapRecovery::Service) { ++g_beats; }
inline bool mayapServiceIsolated(MayapRecovery::Service, uint32_t) { return g_isolated; }
inline bool mayapOnlineMemoryPressure() { return g_pressure; }
inline bool mayapCloudTlsYieldRequested(uint32_t) {
  if (g_yieldAfterCalls > 0U && --g_yieldAfterCalls == 0U) g_yield = true;
  return g_yield;
}
// Exclusive-TLS bookkeeping of the real guard (network_io_guard.h): the WSS socket counts as THE TLS context.
inline bool &mqttTlsResidentRef() { static bool resident = false; return resident; }
inline unsigned &mqttTlsResidentFlips() { static unsigned flips = 0U; return flips; }
inline void mayapSetMqttTlsResident(bool resident) { if (mqttTlsResidentRef() != resident) ++mqttTlsResidentFlips(); mqttTlsResidentRef() = resident; }
inline bool mayapMqttTlsResident() { return mqttTlsResidentRef(); }
// Ordering evidence: how often an OPEN socket was closed while the transport no longer claimed to be the resident TLS context
// (that would let HTTPS start while the socket's memory is still being freed). Must stay 0.
inline unsigned &mqttTlsStopRaces() { static unsigned races = 0U; return races; }
inline void mayapOnlineOwnerQuiet(MayapRecovery::Service) {}
inline bool mayapOnlineIoEnter(MayapRecovery::Service) { return g_ioEnterOk; }
inline void mayapOnlineIoLeave(MayapRecovery::Service) {}
inline bool mayapWifiPortalExclusiveRequested() { return g_gateClosing; }
inline bool mayapRadioRecoveryRequested() { return false; }
enum class ConnectivityMode { Offline, Online };
struct NetworkStatus { ConnectivityMode requestedMode; bool connected; int8_t rssiDbm; };
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
#ifdef MAYAP_LIVE_SOCKET
struct WiFiClientSecure {
  int fd = -1; bool allowConnect = true; unsigned connects = 0U;
  void setCACert(const char *) {}
  void setConnectionTimeout(uint32_t) {}
  void setHandshakeTimeout(uint32_t) {}
  int lastError(char *buffer, size_t size) { snprintf(buffer, size, "socket"); return -1; }
  bool connect(IPAddress ip, uint16_t port, const char *, const char *, const char *, const char *) {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = ip.v;
    if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0) { if (fd >= 0) close(fd); fd = -1; return false; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    ++connects;
    return true;
  }
  bool hostTxReady() { if (fd < 0) return true; pollfd p{fd, POLLOUT, 0}; return poll(&p, 1, 0) != 0; }
  size_t write(const uint8_t *data, size_t length) {
    size_t done = 0U;
    while (done < length) {
      const ssize_t n = ::send(fd, data + done, length - done, MSG_NOSIGNAL);
      if (n > 0) { done += static_cast<size_t>(n); continue; }
      pollfd p{fd, POLLOUT, 0}; if (poll(&p, 1, 1000) <= 0) break;
    }
    return done;
  }
  int available() { if (fd < 0) return 0; pollfd p{fd, POLLIN, 0}; return poll(&p, 1, 0) > 0 ? 1024 : 0; }
  int read(uint8_t *buffer, size_t length) { const ssize_t n = ::recv(fd, buffer, length, 0); return n > 0 ? static_cast<int>(n) : 0; }
  bool connected() { if (fd < 0) return false; char c; const ssize_t n = ::recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT); return n != 0; }
  void stop() { if (fd >= 0) { close(fd); fd = -1; } }
};
#else
struct WiFiClientSecure {
  bool allowConnect = true, open = false, failWrite = false, txReady = true;
  unsigned connects = 0U, stops = 0U;
  std::string lastHost; uint16_t lastPort = 0U; uint32_t lastIp = 0U;
  std::vector<uint8_t> in;
  std::vector<std::vector<uint8_t>> sent;
  std::function<void(const std::vector<uint8_t> &)> onWrite;
  void setCACert(const char *) {}
  void setConnectionTimeout(uint32_t) {}
  void setHandshakeTimeout(uint32_t) {}
  int lastError(char *buffer, size_t size) { snprintf(buffer, size, "fake-tls-error"); return -1; }
  bool connect(IPAddress ip, uint16_t port, const char *host, const char *, const char *, const char *) {
    if (!allowConnect) return false;
    lastHost = host; lastPort = port; lastIp = ip.v; open = true; ++connects; return true;
  }
  bool hostTxReady() { return txReady; }
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
  void stop() { if (open && !mayapMqttTlsResident()) ++mqttTlsStopRaces(); open = false; in.clear(); ++stops; }
};

#endif

// ---- Transaction V2 bridge surface used by the transport ----
extern unsigned g_realtimeUpdates;
inline void mayapRealtimeUpdate(uint32_t) { ++g_realtimeUpdates; }
namespace MayapRealtimeInternal {
typedef bool (*PublishCallback)(const char *, const char *, size_t);
typedef uint8_t (*CapacityCallback)();
typedef bool (*TxGateCallback)(uint8_t);
static PublishCallback publishCallback = nullptr;
static CapacityCallback bulkCapacityCallback = nullptr;
static TxGateCallback txGateCallback = nullptr;
static MayapTx::SnapshotPacer snapshotPacer;
static MayapTx::BridgeStats arbiterStats;
static char deviceId[24] = "MAP-AABBCCDDEEFF";
static portMUX_TYPE realtimeMux = 0;
static bool knownConfigValid = true, configDirty = false, forceSnapshotPublish = false;
inline void resetReportProgress() {}
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
