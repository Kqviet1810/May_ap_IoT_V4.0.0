#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include "../MAYAP_INDUSTRIAL_v1_0_0/runtime_recovery_policy.h"
static uint32_t clockMs=1;
uint32_t millis() { return clockMs; }
void mayapSerialPrintf(bool, const char *, ...) {}
#include "actual-services.inc"
enum class ConnectivityMode : uint8_t { Offline, Online };
enum class NetworkStateCode { Offline, NotConfigured, Connecting };
constexpr int WIFI_OFF=0, WIFI_STA=1, WL_CONNECTED=3;
constexpr const char *NETWORK_WIFI_HOSTNAME="mayap-test";
static bool portal=false, configured=true;
static uint8_t portalListeners=0;
bool mayapWifiPortalExclusiveRequested() { return portal; }
struct FakeWifi {
  unsigned off=0, sta=0, disconnects=0, reconnects=0, begins=0;
  bool reconnectResult=true, connected=false;
  int radioMode=WIFI_STA;
  int getMode() { return radioMode; }
  bool isConnected() { return connected; }
  int status() { return connected ? WL_CONNECTED : 0; }
  bool softAPdisconnect(bool) { assert(mayapOnlineOwnersDrained()); assert(portalListeners==0); return true; }
  bool setAutoReconnect(bool enabled) { assert(!enabled); return true; }
  bool disconnect(bool eraseRadio, bool eraseCredentials) {
    assert(mayapOnlineOwnersDrained());
    assert(!eraseRadio && !eraseCredentials); ++disconnects; return true;
  }
  bool mode(int mode) {
    assert(mayapOnlineOwnersDrained()); radioMode=mode;
    if (mode==WIFI_OFF) ++off;
    else { assert(mode==WIFI_STA); ++sta; }
    return true;
  }
  bool setHostname(const char *) { return true; }
  bool reconnect() { assert(mayapOnlineOwnersDrained()); ++reconnects; return reconnectResult; }
  bool begin(const char *, const char *) { assert(mayapOnlineOwnersDrained()); ++begins; return true; }
} WiFi;
namespace MayapNetworkInternal {
static MayapRecovery::WifiRecovery deepPolicy;
enum class DeepPhase : uint8_t { Idle, Quiesce, OffWait, Isolated };
static DeepPhase deepPhase=DeepPhase::Idle;
static uint32_t deepPhaseAt=0, connectionStartedAt=0;
static volatile uint32_t staDisconnectAt=0U;
static bool deepRequested=false, radioActive=true;
static bool wifiPowerModeAppliedValid=false;
static uint8_t requestedMode=static_cast<uint8_t>(ConnectivityMode::Online);
static char activeSsid[33]="test";
static char activePassword[65]="pass";
struct Backoff {
  unsigned failures=0;
  void reset(uint32_t) {}
  void onFailure(uint32_t) { ++failures; }
  void onSuccess() {}
} staBackoff;
bool credentialsConfigured() { return configured; }
void publish(NetworkStateCode, bool connected) { assert(!connected); }
void stopRadio() {
  WiFi.setAutoReconnect(false);
  (void)WiFi.disconnect(false,false);
  (void)WiFi.mode(WIFI_OFF);
  radioActive=false;
  wifiPowerModeAppliedValid=false;
}
}
#include "actual-network.inc"

enum class WifiPortalState { Idle, Starting, ApActive, Testing, Success, Failed };
constexpr uint32_t NETWORK_TASK_PERIOD_MS=250, WIFI_PORTAL_QUIESCE_TIMEOUT_MS=10000,
 WIFI_PORTAL_MAX_OPEN_MS=300000, WIFI_PORTAL_TEST_TIMEOUT_MS=20000;
namespace MayapNetworkInternal {
enum class PortalPhase { Idle, Quiescing, Starting, ApActive, Testing, Success, Failed };
static PortalPhase portalPhase=PortalPhase::Idle;
static uint8_t portalRequestFlag=0,portalCancelFlag=0,portalOtaQuiescedFlag=0;
static bool pendingCredentialsReady=false;
static uint32_t portalQuiesceStartedAt_=0,portalOpenedAt=0,portalTestStartedAt=0,portalResultUntil_=0;
static char portalApName[20]="test",pendingSsid[33]="new",pendingPassword[65]="bad";
struct Server { uint8_t bit; void stop(){portalListeners &= ~bit;} void processNextRequest(){} void handleClient(){} } portalServer{1},portalDns{2};
inline bool timeReached(uint32_t now,uint32_t when){return static_cast<int32_t>(now-when)>=0;}
inline uint32_t elapsedMs(uint32_t now,uint32_t then){return now-then;}
void portalCrashMark(uint8_t){} void portalCrashClear(){}
void publishPortalState(WifiPortalState,const char*){}
bool saveCredentials(const char*,const char*){return true;}
void portalBeginStarting(uint32_t now){assert(mayapOnlineOwnersDrained());portalListeners=3;portalPhase=PortalPhase::ApActive;portalOpenedAt=now;}
void serviceStarting(uint32_t){}
#include "actual-portal.inc"
}
void quietAll(){
 mayapSetRadioOtaQuiesced(true);
 for(uint8_t i=1;i<4;i++)mayapOnlineOwnerQuiet(static_cast<MayapRecovery::Service>(i));
}
void testPortal(){
 using namespace MayapNetworkInternal;
 for(bool succeeds:{false,true}){
  mayapServiceAdmit(MayapRecovery::Service::Cloud);mayapServiceAdmit(MayapRecovery::Service::Ota);
  portalRequestFlag=1;portalPhase=PortalPhase::Idle;
  assert(mayapOnlineIoEnter(MayapRecovery::Service::Cloud));
  servicePortal(++clockMs);assert(mayapRadioRecoveryRequested());
  quietAll();portalOtaQuiescedFlag=1;clockMs+=300;
  servicePortal(clockMs);assert(portalPhase==PortalPhase::Quiescing); // HTTPS has not returned.
  mayapOnlineIoLeave(MayapRecovery::Service::Cloud);quietAll();servicePortal(++clockMs);
  assert(portalPhase==PortalPhase::ApActive);
  pendingCredentialsReady=true;servicePortal(++clockMs);assert(portalPhase==PortalPhase::Testing);
  WiFi.connected=succeeds;
  clockMs+=WIFI_PORTAL_TEST_TIMEOUT_MS;servicePortal(clockMs);
  if(succeeds){assert(portalPhase==PortalPhase::Success);clockMs+=8000;servicePortal(clockMs);}
  else {assert(portalPhase==PortalPhase::Failed);servicePortal(++clockMs);portalCancelFlag=1;servicePortal(++clockMs);}
  assert(portalPhase==PortalPhase::Idle&&!mayapRadioRecoveryRequested());
  const unsigned prior=WiFi.begins;
  assert(mayapOnlineIoEnter(MayapRecovery::Service::Mqtt)); // immediate reconnect after portal close
  assert(WiFi.begins==prior); // portal exit performs no second STA teardown/start
  mayapOnlineIoLeave(MayapRecovery::Service::Mqtt);
 }
 // Cancel/timeout while an owner never acknowledges cannot touch the radio.
 portalPhase=PortalPhase::Idle;portalRequestFlag=1;servicePortal(++clockMs);
 clockMs+=WIFI_PORTAL_QUIESCE_TIMEOUT_MS;portalOtaQuiescedFlag=0;servicePortal(clockMs);
 assert(portalPhase==PortalPhase::Idle&&!mayapRadioRecoveryRequested());
 portalRequestFlag=1;servicePortal(++clockMs);portalCancelFlag=1;servicePortal(++clockMs);
 assert(portalPhase==PortalPhase::Idle&&!mayapRadioRecoveryRequested());
}
void testLongOutage(){
 using namespace MayapNetworkInternal;
 portal=false;configured=true;requestedMode=static_cast<uint8_t>(ConnectivityMode::Online);radioActive=true;
 WiFi.reconnectResult=false;deepPolicy=MayapRecovery::WifiRecovery{};deepPhase=DeepPhase::Idle;
 const unsigned outageAttempts=WiFi.reconnects, radioOffs=WiFi.off;
 for(uint32_t t=0;t<12U*3600000U;t+=250U){
  clockMs+=250; // virtual network trace, not a physical control timing claim
  mayapRequestWifiDeepRecovery();mayapNetworkDeepRecoveryUpdate(clockMs,false);
  quietAll();mayapNetworkDeepRecoveryUpdate(clockMs,false);
  for(uint8_t i=0;i<4;i++)mayapServiceBeat(static_cast<MayapRecovery::Service>(i));
  mayapServiceSupervisorUpdate(clockMs);
 }
 assert(WiFi.off==radioOffs);
 assert(WiFi.reconnects>outageAttempts);
 assert(WiFi.reconnects-outageAttempts<=1U+12U*3600000U/MayapRecovery::WIFI_COOLDOWN_MS);
 // An owner stuck in an active TLS operation cannot be falsely drained.
 deepPhase=DeepPhase::Idle;deepPolicy=MayapRecovery::WifiRecovery{};mayapRadioQuiesceEnd();
 assert(mayapOnlineIoEnter(MayapRecovery::Service::Mqtt));mayapRequestWifiDeepRecovery();
 assert(mayapNetworkDeepRecoveryUpdate(++clockMs,false));quietAll();
 const unsigned before=WiFi.reconnects;
 for(unsigned i=0;i<1000;i++){clockMs+=1000;assert(mayapNetworkDeepRecoveryUpdate(clockMs,false));mayapServiceSupervisorUpdate(clockMs);}
 assert(WiFi.reconnects==before&&!mayapOnlineOwnersDrained());
 mayapOnlineIoLeave(MayapRecovery::Service::Mqtt);quietAll();mayapNetworkDeepRecoveryUpdate(++clockMs,false);
 assert(WiFi.reconnects==before+1);
}
int main() {
  using namespace MayapRecovery;
  clockMs=1000000;
  mayapServiceSupervisorUpdate(clockMs);
  assert(mayapServiceDegradedMask()==0U);
  mayapServiceAdmit(Service::Mqtt);
  clockMs+=60001;
  mayapServiceSupervisorUpdate(clockMs);
  assert(mayapServiceRecoveryRequested(Service::Mqtt));
  clockMs+=60000;
  mayapServiceSupervisorUpdate(clockMs);
  assert(mayapServiceIsolated(Service::Mqtt,clockMs));
  clockMs+=240000;
  mayapServiceSupervisorUpdate(clockMs);
  assert((mayapServiceDegradedMask() & (1U << static_cast<uint8_t>(Service::Mqtt))) != 0U);
  mayapServiceRecoveryComplete(Service::Mqtt);
  assert(mayapServiceDegradedMask()==0U);

  mayapSetRadioOtaQuiesced(false);
  mayapRequestWifiDeepRecovery();
  assert(mayapNetworkDeepRecoveryUpdate(clockMs,false));
  assert(mayapRadioRecoveryRequested() && WiFi.off==0 && WiFi.reconnects==0);
  assert(mayapNetworkDeepRecoveryUpdate(++clockMs,true));
  assert(WiFi.reconnects==0);
  mayapSetRadioOtaQuiesced(true);
  mayapOnlineOwnerQuiet(Service::Mqtt);
  mayapOnlineOwnerQuiet(Service::Mqtt);
  assert(!mayapNetworkDeepRecoveryUpdate(++clockMs,false));
  assert(WiFi.reconnects==1 && WiFi.off==0 && WiFi.disconnects==0);
  assert(!mayapRadioRecoveryRequested());

  mayapRequestWifiDeepRecovery();
  mayapOnlineOwnerQuiet(Service::Mqtt);
  assert(!mayapNetworkDeepRecoveryUpdate(++clockMs,false));
  assert(WiFi.reconnects==1 && WiFi.off==0);
  clockMs += WIFI_COOLDOWN_MS;
  mayapRequestWifiDeepRecovery();
  assert(mayapNetworkDeepRecoveryUpdate(clockMs,false));
  mayapOnlineOwnerQuiet(Service::Mqtt);
  assert(!mayapNetworkDeepRecoveryUpdate(++clockMs,false));
  assert(WiFi.reconnects==2 && WiFi.off==0);

  MayapNetworkInternal::requestedMode=static_cast<uint8_t>(ConnectivityMode::Offline);
  MayapNetworkInternal::radioActive=true;
  assert(mayapNetworkDeepRecoveryUpdate(++clockMs,false));
  assert(mayapRadioRecoveryRequested() && WiFi.off==0);
  assert(mayapNetworkDeepRecoveryUpdate(++clockMs,true));
  assert(WiFi.off==0);
  mayapOnlineOwnerQuiet(Service::Mqtt);
  assert(!mayapNetworkDeepRecoveryUpdate(++clockMs,false));
  assert(WiFi.off==1 && WiFi.disconnects==1 && !MayapNetworkInternal::radioActive);
  assert(!mayapRadioRecoveryRequested());

  MayapNetworkInternal::requestedMode=static_cast<uint8_t>(ConnectivityMode::Online);
  MayapNetworkInternal::radioActive=true;
  portal=true; mayapRequestWifiDeepRecovery();
  mayapOnlineOwnerQuiet(Service::Mqtt);
  assert(!mayapNetworkDeepRecoveryUpdate(++clockMs,false));
  assert(WiFi.off==1 && WiFi.reconnects==2);
  portal=false; configured=false;
  assert(mayapNetworkDeepRecoveryUpdate(++clockMs,false));
  mayapOnlineOwnerQuiet(Service::Mqtt);
  assert(!mayapNetworkDeepRecoveryUpdate(++clockMs,false));
  assert(WiFi.off==2);

  testPortal();testLongOutage();
  std::puts("Actual service/radio: HTTPS drain, portal success/failure/exit race, 12h reconnect failure with bounded attempts, stale TLS owner and explicit-offline shutdown PASS");
}
