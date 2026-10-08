// MQTT Primary + Cold Standby Alarm: the REAL send path of cloud_alert_link.h (awaitUplink, sendHeartbeat, sendAlarm,
// sendAlarmsUplink, sendAlarmsHttps, sendAlarms, drainOutbox) with the REAL TLS admission guard, uplink mailbox, fallback
// policy and Wi-Fi power policy, against a simulated MQTT owner, a simulated Cloudflare Worker and a heap model built from
// the bench numbers (idle 125 kB, resident MQTT -43 kB, HTTPS handshake peak -71 kB).
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <ArduinoJson.h>
#define portMUX_INITIALIZER_UNLOCKED 0
using portMUX_TYPE=int;
#define portENTER_CRITICAL(x) ((void)x)
#define portEXIT_CRITICAL(x) ((void)x)
#define pdMS_TO_TICKS(x) (x)
uint32_t clockMs=1000000;
uint32_t millis(){return clockMs;}
void mayapSerialPrintf(bool,const char *,...){}
uint32_t esp_random(){static uint32_t x=12345;x=x*1664525U+1013904223U;return x>>8;}
struct String{std::string s;const char *c_str()const{return s.c_str();}String &operator=(const char *x){s=x;return *this;}};
namespace MayapRecovery{enum class Service{Cloud};}
static unsigned beats=0;
void mayapServiceBeat(MayapRecovery::Service){++beats;}
static bool mqttResidentNow();
struct EspModel{
  static constexpr uint32_t IDLE=125000,MQTT=43000,HTTPS_PEAK=71000;
  bool httpsActive=false;uint32_t minFree=0xFFFFFFFFU,minLargest=0xFFFFFFFFU;
  uint32_t free(){return IDLE-(mqttResidentNow()?MQTT:0)-(httpsActive?HTTPS_PEAK:0);}
  uint32_t largest(){return httpsActive||mqttResidentNow()?31732:47092;}
  uint32_t getFreeHeap(){const uint32_t f=free();if(f<minFree)minFree=f;return f;}
  uint32_t getMaxAllocHeap(){const uint32_t l=largest();if(l<minLargest)minLargest=l;return l;}
} ESP;
#include "actual-network_io_guard.inc"
#include "actual-mqtt_uplink.inc"
#include "actual-alarm_fallback_policy.inc"
#include "actual-wifi_power_policy.inc"
#include "actual-cloud_alarm_receipt.inc"
static bool mqttResidentNow(){return mayapMqttTlsResident();}
constexpr uint8_t CLOUD_OUTBOX_SIZE=16;
constexpr uint32_t CLOUD_MIN_SEND_GAP_MS=3000;
struct BackoffTimer {uint32_t next=0;bool ready(uint32_t t)const{return int32_t(t-next)>=0;}
 void reset(uint32_t t){next=t;}void onSuccess(){next=0;}void onFailure(uint32_t t){next=t+1000;}};
namespace MayapCloudInternal {
static bool knownRuntimeValid=true;
enum class NotifyLevel:uint8_t {Info,Warning,Critical,System};
uint32_t elapsedMs(uint32_t t,uint32_t p){return t-p;}
bool timeReached(uint32_t t,uint32_t p){return int32_t(t-p)>=0;}
struct Runtime{float temperature=37.5f,humidity=60;bool batchRunning=true,lightOn=false;} processingRuntime;
static uint32_t eventBootHigh=0xAAAA,eventBootLow=0xBBBB;
#include "actual-cloud-outbox.inc"
#include "actual-cloud-enqueue.inc"
enum class ConnectivityMode{Offline,Online};
struct NetworkStatus {ConnectivityMode requestedMode=ConnectivityMode::Online;bool connected=true;} net;
NetworkStatus mayapGetRawNetworkStatus(){return net;}
static bool registered=true;
static void touchUnused(){(void)outboxCriticalDropped;(void)lastRequestFinishedAt;(void)outboxDropped;}
const char *severityText(NotifyLevel l){return l==NotifyLevel::Critical?"critical":l==NotifyLevel::Warning?"warning":"info";}
const char *mayapDeviceIdText(){return "MAP-441BF6E051D0";}
const char *mayapDeviceSecret(){return "device-key";}

// ---- simulated MQTT owner (the real transport is covered by tests/mqtt-transport.cpp) ----
static bool brokerUp=true,brokerAcks=true;
static uint32_t holdOffUntil=0,mqttConnects=0,mqttClosesForYield=0;
struct Pending{uint16_t id;uint32_t at;};
static std::vector<Pending> pendingAcks;
static uint16_t nextPacketId=1;
static std::vector<std::string> publishedKinds;
static void closeMqtt(){mayapSetMqttTlsResident(false);MayapUplink::onLinkDown();pendingAcks.clear();}
static void mqttOwnerStep(){
  const bool wifi=net.connected;
  if(mayapMqttTlsResident()){
    if(mayapCloudTlsYieldRequested(clockMs)){closeMqtt();holdOffUntil=clockMs+3000;++mqttClosesForYield;return;}
    if(!brokerUp||!wifi){closeMqtt();holdOffUntil=clockMs+15000;return;}
    MayapUplink::expire(clockMs);
    for(int8_t i;(i=MayapUplink::nextQueued())>=0;){
      publishedKinds.push_back(MayapUplink::kind(i)==MayapUplink::Kind::Alarm?"alarm":"heartbeat");
      MayapUplink::markSent(i,nextPacketId,clockMs);pendingAcks.push_back({nextPacketId,clockMs+150});++nextPacketId;}
    for(size_t k=0;k<pendingAcks.size();){
      if(static_cast<int32_t>(clockMs-pendingAcks[k].at)>=0){if(brokerAcks)MayapUplink::onPuback(pendingAcks[k].id);pendingAcks.erase(pendingAcks.begin()+k);}
      else ++k;}
  } else if(brokerUp&&wifi&&static_cast<int32_t>(clockMs-holdOffUntil)>=0&&!mayapCloudTlsYieldRequested(clockMs)){
    MayapTlsOperation tls(MayapTlsKind::Mqtt);
    if(tls){mayapSetMqttTlsResident(true);MayapUplink::onLinkUp();++mqttConnects;}
  }
}
static void advance(uint32_t ms){for(uint32_t t=0;t<ms;t+=10){clockMs+=10;mqttOwnerStep();ESP.getFreeHeap();}}
void vTaskDelay(uint32_t ticks){advance(ticks);}

// ---- simulated Cloudflare Worker over HTTPS ----
static bool serverUp=true;
static std::vector<uint32_t> httpsAt;
static unsigned httpsCalls=0,httpsOverlap=0;
static std::vector<std::string> durableIds;
bool postJson(const char *path,const JsonDocument &doc,const char *,String *response,int *code,bool urgent){
  requestDeferred=true;
  MayapTlsOperation op(MayapTlsKind::Cloud,urgent);
  if(!op){if(code)*code=0;return false;}
  requestDeferred=false;
  if(mayapMqttTlsResident())++httpsOverlap;                 // must never happen
  ++httpsCalls;httpsAt.push_back(clockMs);
  ESP.httpsActive=true;ESP.getFreeHeap();
  advance(2500);                                            // handshake + request; MQTT keeps trying meanwhile
  if(mayapMqttTlsResident())++httpsOverlap;
  ESP.httpsActive=false;
  if(!serverUp){if(code)*code=-1;return false;}
  JsonDocument reply;reply["success"]=true;
  if(!strcmp(path,"/api/device/alarms")){
    JsonArray results=reply["results"].to<JsonArray>();
    for(JsonVariantConst e:doc["events"].as<JsonArrayConst>()){
      JsonObject r=results.add<JsonObject>();r["event_id"]=e["event_id"].as<const char *>();r["durable"]=true;
      durableIds.push_back(e["event_id"].as<const char *>());}
  } else {reply["durable"]=true;reply["event_id"]=doc["event_id"].as<const char *>();durableIds.push_back(doc["event_id"].as<const char *>());}
  std::string body;serializeJson(reply,body);if(response)*response=body.c_str();
  if(code)*code=200;
  return true;
}
#include "actual-cloud-send.inc"
#include "actual-cloud-drain.inc"
static void cloudRun(uint32_t ms){                          // the Cloud task: drain every 100 ms, release the yield when idle
  for(uint32_t t=0;t<ms;t+=100){
    drainOutbox(clockMs);
    if(outboxCount==0&&!requestDeferred)mayapReleaseCloudTlsYield();
    advance(100);}
}
static void resetWorld(){
  touchUnused();
  closeMqtt();for(int8_t i=0;i<static_cast<int8_t>(MayapUplink::SLOTS);++i)MayapUplink::finish(i);
  outboxHead=outboxTail=outboxCount=0;
  brokerUp=brokerAcks=serverUp=true;net.connected=true;registered=true;
  holdOffUntil=0;httpsAt.clear();durableIds.clear();publishedKinds.clear();httpsCalls=httpsOverlap=0;
  fallbackGate=MayapAlarmFallback::Gate{};
  MayapUplink::onLinkDown();mayapReleaseCloudTlsYield();
  MayapNetworkIoInternal::cloudYieldRetryAt=MayapNetworkIoInternal::cloudUrgentRetryAt=0;
  clockMs+=120000;
}
}
int main(){
 using namespace MayapCloudInternal;
 // 1. Healthy MQTT: alarms and heartbeat travel over it; HTTPS never wakes, no HTTPS socket/context ever exists.
 resetWorld();advance(3000);assert(mayapMqttTlsResident());
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",true,29.9f,63.7f);
 enqueueRaw("LIGHT",NotifyLevel::Warning,false,"on",false,0,0);
 cloudRun(3000);
 assert(outboxCount==0&&httpsCalls==0&&mayapMqttTlsResident());
 assert(publishedKinds.size()==2&&publishedKinds[0]=="alarm");
 assert(sendHeartbeat()&&httpsCalls==0&&publishedKinds.back()=="heartbeat");
 assert(mayapTlsContextsMax()==1&&mayapTlsOverlapViolations()==0);   // only the MQTT socket ever existed
 // 2. MQTT down, critical alarm: nothing for the 10 s grace (reconnect first), then ONE HTTPS request after MQTT released its heap.
 resetWorld();brokerUp=false;advance(2000);assert(!mayapMqttTlsResident());
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);
 cloudRun(9000);assert(httpsCalls==0&&outboxCount==1);
 cloudRun(4000);assert(httpsCalls==1&&outboxCount==0&&durableIds.size()==1);
 // 3. Routine alarms wait a full minute for the realtime link.
 resetWorld();brokerUp=false;advance(1000);
 enqueueRaw("WIFI_SIGNAL_WEAK",NotifyLevel::Warning,false,"weak",false,0,0);
 cloudRun(59000);assert(httpsCalls==0);cloudRun(6000);assert(httpsCalls==1&&outboxCount==0);
 // 4. MQTT up but the broker never acknowledges (stuck link): uplink turns suspect, then the fallback closes MQTT first.
 resetWorld();advance(3000);assert(mayapMqttTlsResident());brokerAcks=false;
 const uint32_t closesBefore=mqttClosesForYield;
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);
 cloudRun(25000);
 assert(httpsCalls==1&&outboxCount==0&&httpsOverlap==0&&mqttClosesForYield>closesBefore);
 // 5. Cloudflare down too: the fallback backs off 30 s, 1, 2, 5, 10 min (+jitter) - no reconnect storm in an hour.
 resetWorld();brokerUp=false;serverUp=false;advance(1000);
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);
 cloudRun(3600000);
 assert(httpsCalls>=8&&httpsCalls<=11&&outboxCount==1);
 for(size_t i=1;i<httpsAt.size();++i){
   const uint32_t gap=httpsAt[i]-httpsAt[i-1]-2500;          // the request itself takes 2.5 s
   const size_t rung=i-1<4?i-1:4;
   assert(gap+200>=MayapAlarmFallback::BACKOFF_MS[rung]);}
 assert(mqttConnects>0);                                        // MQTT kept retrying between fallback attempts
 // 6. MQTT recovers: the stuck alarm goes over it, the fallback sleeps and its back-off resets; later alarms stay on MQTT.
 brokerUp=true;serverUp=false;const unsigned callsAtRecovery=httpsCalls;
 cloudRun(120000);
 assert(outboxCount==0&&httpsCalls==callsAtRecovery&&!fallbackGate.armed);
 enqueueRaw("FAULT_131",NotifyLevel::Critical,false,"x",false,0,0);
 cloudRun(15000);assert(outboxCount==0&&httpsCalls==callsAtRecovery);
 // 7. No Wi-Fi: nothing is attempted (and nothing is penalised); it flows once the network is back.
 resetWorld();brokerUp=false;net.connected=false;advance(1000);
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);
 cloudRun(300000);assert(httpsCalls==0&&outboxCount==1);
 net.connected=true;cloudRun(20000);assert(httpsCalls==1&&outboxCount==0);
 // 8. Not registered: no HTTPS fallback (the Worker would answer 404), the alarm is simply kept.
 resetWorld();brokerUp=false;registered=false;advance(1000);
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);
 cloudRun(120000);assert(httpsCalls==0&&outboxCount==1);
 // 9. Heartbeat is MQTT only: with the link down it is skipped silently (no HTTPS, no Cloud back-off).
 resetWorld();brokerUp=false;advance(1000);
 assert(!sendHeartbeat()&&requestDeferred&&httpsCalls==0);
 // 10. Batching over the fallback: a backlog leaves in few requests, preserving per-alarm order, never overlapping MQTT.
 resetWorld();brokerUp=false;advance(1000);
 for(int n=0;n<10;++n){char id[16];snprintf(id,sizeof(id),"ALARM_%d",n);enqueueRaw(id,NotifyLevel::Critical,false,"m",false,0,0);}
 cloudRun(60000);assert(outboxCount==0&&httpsCalls<=3&&durableIds.size()==10);
 // Whole run: never two TLS contexts, never an HTTPS request while MQTT was resident, heap never below the bench floor.
 assert(mayapTlsContextsMax()==1&&mayapTlsOverlapViolations()==0&&httpsOverlap==0);
 assert(ESP.minFree>=50000);
 std::printf("Alarm fallback: MQTT primary, HTTPS cold standby (grace %lus/%lus, back-off ladder, Wi-Fi/unregistered gates), exclusive TLS contexts_max=%u overlap=%lu denied=%lu, model min free heap %lu B, min largest block %lu B PASS\n",
   static_cast<unsigned long>(MayapAlarmFallback::CRITICAL_GRACE_MS/1000),static_cast<unsigned long>(MayapAlarmFallback::ROUTINE_GRACE_MS/1000),
   static_cast<unsigned>(mayapTlsContextsMax()),static_cast<unsigned long>(mayapTlsOverlapViolations()),static_cast<unsigned long>(mayapTlsOverlapDenied()),
   static_cast<unsigned long>(ESP.minFree),static_cast<unsigned long>(ESP.minLargest));
}
