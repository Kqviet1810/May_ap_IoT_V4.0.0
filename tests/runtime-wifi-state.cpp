#include <cstdint>
#include <cassert>
#include <cstdio>
#include <cstdarg>
enum class NetworkStateCode : uint8_t { Offline, NotConfigured, Connecting, Connected };
enum class ConnectivityMode : uint8_t { Offline, Online };
struct NetworkStatus { ConnectivityMode requestedMode; NetworkStateCode state; bool credentialsConfigured,connected; int8_t rssiDbm; };
static volatile uint8_t requestedMode=static_cast<uint8_t>(ConnectivityMode::Online);
static uint32_t clockMs=0;
uint32_t millis() { return clockMs; }
void mayapSerialPrintf(bool,const char*,...) {}
bool credentialsConfigured() { return true; }
struct { bool connected=false; bool isConnected() const { return connected; } int RSSI() const { return -50; } uint32_t localIP() const { return 1234; } } WiFi;
constexpr int WIFI_PS_NONE=0, WIFI_PS_MIN_MODEM=1, ESP_OK=0;
static bool wifiPowerModeAppliedValid=false;
static unsigned powerCalls=0; static int powerResult=ESP_OK, lastPsMode=-1;
int esp_wifi_set_ps(int mode) { ++powerCalls; lastPsMode=mode; return powerResult; }
namespace MayapNetworkInternal {}
#include "actual-wifi-globals.inc"
static MayapWifiPower::Mode wifiPowerModeApplied=MayapWifiPower::Mode::Performance;
#include "actual-wifi-publish.inc"
#include "actual-wifi-getters.inc"
void sample(uint32_t time,bool raw) {
 clockMs=time; WiFi.connected=raw; publish(raw?NetworkStateCode::Connected:NetworkStateCode::Connecting,raw,raw?-50:-127);
}
int main() {
 sample(0,true); assert(!mayapGetNetworkStatus().connected && mayapGetNetworkStatus().state==NetworkStateCode::Connecting);
 assert(mayapGetRawNetworkStatus().connected); sample(250,true); sample(500,true); sample(750,true);
 assert(publishedConnected);
 // Short driver glitch must not appear in HMI/Fault/Web snapshot.
 sample(1000,false); assert(publishedConnected);
 assert(!mayapGetRawNetworkStatus().connected && mayapGetNetworkStatus().connected);
 sample(1250,false); sample(1500,false); sample(1750,true); assert(publishedConnected);
 // Continuous four-second failure is published once, not on the first poll.
 sample(2000,false);
 for(uint32_t t=2250;t<6000;t+=250) { sample(t,false); assert(publishedConnected); }
 sample(6000,false); assert(!publishedConnected);
 // One positive sample during router reboot cannot flicker UI online.
 sample(6250,true); assert(!publishedConnected);
 sample(6500,false); assert(!publishedConnected);
 sample(6750,true); sample(7000,true); sample(7250,true); sample(7500,true);
 assert(publishedConnected);
 for(unsigned n=0;n<1000;++n) {
  uint32_t t=10000+n*2000; sample(t,false); sample(t+250,false);sample(t+500,true);sample(t+750,true);
  assert(publishedConnected);
 }
 // Offline is a deliberate setting, not a flap: immediate publication.
 clockMs=3000000; publish(NetworkStateCode::Offline,false); assert(!publishedConnected);
 // Deep recovery/owner drain can skip station polls: down grace still expires.
 sample(4000000,true); sample(4000250,true); sample(4000500,true); sample(4000750,true);
 sample(4001000,false); clockMs=4005000; tickStableWifi(); assert(!publishedConnected);
 // A single additional driver sample cannot qualify a reconnect (needs >=3).
 sample(5000000,true); clockMs=5001000; tickStableWifi(); assert(!publishedConnected);
 // Millis wrap is deterministic.
 publish(NetworkStateCode::Offline,false);
 sample(0xFFFFFF00U,true); sample(0xFFFFFFFAU,true); sample(244,true); sample(494,true); assert(publishedConnected);
 sample(1000,false); sample(5000,false); assert(!publishedConnected);
 // Multi-hour failure and Wi-Fi connected/Internet dead do not invent Wi-Fi edges.
 for(uint32_t t=6000;t<8*3600000U;t+=250) { sample(t,false); assert(!publishedConnected); }
 sample(8*3600000U,true);sample(8*3600000U+250,true);sample(8*3600000U+500,true);sample(8*3600000U+750,true);
 for(uint32_t t=8*3600000U+1000;t<9*3600000U;t+=250) { sample(t,true); assert(publishedConnected); }
 // Recovery/isolation must observe a new driver loss even if station service skips.
 WiFi.connected=false; clockMs=9*3600000U; tickStableWifi();
 assert(!mayapGetRawNetworkStatus().connected && mayapGetRawNetworkStatus().state==NetworkStateCode::Connecting);
 assert(publishedConnected); WiFi.connected=true;
 // Internet and owner drain can fail while the STA remains associated.
 // Presentation must remain Wi-Fi connected; network admission still closes.
 clockMs=9*3600000U; publish(NetworkStateCode::Connecting,false);
 clockMs+=4000; publish(NetworkStateCode::Connecting,false);
 assert(publishedConnected && !mayapGetRawNetworkStatus().connected);
 // Power policy (wifi_power_policy.h), applied only by networkTask: PERFORMANCE for the first 15 minutes after
 // power-up and while the Web is in use, MIN_MODEM after 15 idle minutes, PERFORMANCE again at the first activity.
 using namespace MayapWifiPower;
 const uint32_t MIN=60000U;
 clockMs=2*60*MIN;                                   // 2 h after boot, never any Web activity
 WiFi.connected=true; applyWifiPowerMode(); applyWifiPowerMode();
 assert(powerCalls==1 && lastPsMode==WIFI_PS_MIN_MODEM && wifiPowerModeApplied==Mode::Save);
 WiFi.connected=false; applyWifiPowerMode(); assert(powerCalls==1 && !wifiPowerModeAppliedValid);
 WiFi.connected=true; powerResult=-1; applyWifiPowerMode(); assert(!wifiPowerModeAppliedValid);   // driver refused: retry
 powerResult=ESP_OK; applyWifiPowerMode(); applyWifiPowerMode(); assert(powerCalls==3 && lastPsMode==WIFI_PS_MIN_MODEM);
 noteWebActivity(clockMs); applyWifiPowerMode();      // a Web tab opens: awake at once
 assert(powerCalls==4 && lastPsMode==WIFI_PS_NONE && wifiPowerModeApplied==Mode::Performance);
 clockMs+=15*MIN-1U; applyWifiPowerMode(); assert(powerCalls==4);          // 14 min 59 s: still awake
 noteWebActivity(clockMs); clockMs+=15*MIN-1U; applyWifiPowerMode(); assert(powerCalls==4);   // activity restarts the 15 min
 clockMs+=1U; applyWifiPowerMode(); assert(powerCalls==5 && lastPsMode==WIFI_PS_MIN_MODEM);
 noteAlarmActivity(clockMs); applyWifiPowerMode();    // an alarm in flight wakes the radio
 assert(powerCalls==6 && lastPsMode==WIFI_PS_NONE);
 clockMs+=2*MIN-1U; applyWifiPowerMode(); assert(powerCalls==6);
 clockMs+=1U; applyWifiPowerMode(); assert(powerCalls==7 && lastPsMode==WIFI_PS_MIN_MODEM);
 // Power-up: the first 15 minutes are awake, whatever millis() says about wrap-around later.
 Internal::webAt=0U; Internal::alarmSeen=0U;
 assert(desired(14*MIN)==Mode::Performance && desired(15*MIN)==Mode::Save);
 Internal::webAt=0xFFFFFFFFU-MIN; assert(desired(10*MIN)==Mode::Performance && desired(14*MIN+MIN)==Mode::Save);
 std::puts("Production Wi-Fi flap publication: raw admission, router reboot, 1000 reconnects, 8h loss, Internet-only failure and wrap PASS");
}
