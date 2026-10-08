// MQTT Primary + Cold Standby Alarm. POLICY UNDER TEST (changed deliberately from the earlier "a missed PUBACK = suspect link" design):
//   * a late or missing PUBACK is an INGEST problem; it never marks the transport suspect and never closes a healthy MQTT socket;
//   * only transport evidence (socket gone, no byte from the broker AND an unanswered probe) permits the HTTPS fallback, plus one
//     explicit last resort for a CRITICAL alarm the broker has not stored for 60 s over >= 3 attempts.
// The REAL send path of cloud_alert_link.h (awaitUplink, sendHeartbeat, sendAlarm,
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
// It publishes the same Health transitions as mqtt_transport.h: Up on connect, Closed on a deliberate yield, Lost when an
// established link dies, AttemptFailed when a connect attempt fails, Waiting while a retry is scheduled, Rx on traffic.
static bool brokerUp=true,brokerAcks=true,brokerSilent=false;
static uint32_t holdOffUntil=0,mqttConnects=0,mqttClosesForYield=0,mqttAckDelay=150,nextAttemptTry=0,probesSent=0;
struct Pending{uint16_t id;uint32_t at;};
static std::vector<Pending> pendingAcks;
static uint16_t nextPacketId=1;
static std::vector<std::string> publishedKinds;
static void closeMqtt(){mayapSetMqttTlsResident(false);MayapUplink::onLinkDown();pendingAcks.clear();}
static void deliberateClose(uint32_t holdMs){closeMqtt();holdOffUntil=clockMs+holdMs;MayapUplink::healthClosed(clockMs,holdOffUntil);}
static void loseLink(uint32_t holdMs){closeMqtt();holdOffUntil=clockMs+holdMs;MayapUplink::healthLost(clockMs,holdOffUntil);}
static void mqttOwnerStep(){
  const bool wifi=net.connected;
  if(mayapMqttTlsResident()){
    if(mayapCloudTlsYieldRequested(clockMs)){deliberateClose(3000);++mqttClosesForYield;return;}
    if(!brokerUp||!wifi){loseLink(15000);return;}
    if(!brokerSilent)MayapUplink::healthRx(clockMs);
    if(MayapUplink::takeProbeRequest()){MayapUplink::noteProbeSent(clockMs);++probesSent;}   // PINGREQ; the next healthRx (any byte) answers it
    MayapUplink::expire(clockMs);
    for(int8_t i;(i=MayapUplink::nextQueued())>=0;){
      publishedKinds.push_back(MayapUplink::kind(i)==MayapUplink::Kind::Alarm?"alarm":"heartbeat");
      MayapUplink::markSent(i,nextPacketId,clockMs);pendingAcks.push_back({nextPacketId,clockMs+mqttAckDelay});++nextPacketId;}
    for(size_t k=0;k<pendingAcks.size();){
      if(static_cast<int32_t>(clockMs-pendingAcks[k].at)>=0){if(brokerAcks&&!brokerSilent)MayapUplink::onPuback(pendingAcks[k].id,clockMs);pendingAcks.erase(pendingAcks.begin()+k);}
      else ++k;}
  } else if(wifi&&static_cast<int32_t>(clockMs-holdOffUntil)>=0&&!mayapCloudTlsYieldRequested(clockMs)){
    if(!brokerUp){                                         // the attempt fails; the transport schedules the next one
      holdOffUntil=clockMs+15000;MayapUplink::healthAttemptFailed(clockMs,holdOffUntil);
    } else {
      MayapTlsOperation tls(MayapTlsKind::Mqtt);
      if(tls){mayapSetMqttTlsResident(true);MayapUplink::onLinkUp();MayapUplink::healthUp(clockMs);++mqttConnects;}
    }
  } else if(wifi) MayapUplink::healthWaiting(clockMs,holdOffUntil);
  else MayapUplink::healthClosed(clockMs,0U);
  (void)nextAttemptTry;
}
static void simAdvance(uint32_t ms){for(uint32_t t=0;t<ms;t+=10){clockMs+=10;mqttOwnerStep();ESP.getFreeHeap();}}
void vTaskDelay(uint32_t ticks){simAdvance(ticks);}

// ---- simulated Cloudflare Worker over HTTPS ----
static bool serverUp=true,serverStores=true;
static int serverCode=-1;                                   // what the device sees while the Worker/Internet is unreachable
static std::vector<int> ageMsSeen;
static std::vector<uint32_t> httpsAt;
static unsigned httpsCalls=0,httpsOverlap=0,beaconCalls=0;
static std::vector<uint32_t> beaconAt;
static uint32_t lastHeartbeatAt=0;
static BackoffTimer cloudBackoff;
constexpr uint32_t CLOUD_HEARTBEAT_INTERVAL_MS=60000;
static std::vector<std::string> durableIds;
bool postJson(const char *path,const JsonDocument &doc,const char *,String *response=nullptr,int *code=nullptr,bool urgent=false){
  requestDeferred=true;
  MayapTlsOperation op(MayapTlsKind::Cloud,urgent);
  if(!op){if(code)*code=0;return false;}
  requestDeferred=false;
  if(mayapMqttTlsResident())++httpsOverlap;                 // must never happen
  ++httpsCalls;httpsAt.push_back(clockMs);
  ESP.httpsActive=true;ESP.getFreeHeap();
  simAdvance(2500);                                            // handshake + request; MQTT keeps trying meanwhile
  if(mayapMqttTlsResident())++httpsOverlap;
  ESP.httpsActive=false;
  if(!serverUp){if(code)*code=serverCode;return false;}
  JsonDocument reply;reply["success"]=true;
  if(!strcmp(path,"/api/device/heartbeat")){++beaconCalls;beaconAt.push_back(clockMs);}
  else if(!strcmp(path,"/api/device/alarms")){
    JsonArray results=reply["results"].to<JsonArray>();
    for(JsonVariantConst e:doc["events"].as<JsonArrayConst>()){
      JsonObject r=results.add<JsonObject>();r["event_id"]=e["event_id"].as<const char *>();r["durable"]=serverStores;
      ageMsSeen.push_back(e["age_ms"]|-1);
      if(serverStores)durableIds.push_back(e["event_id"].as<const char *>());}
  } else {reply["durable"]=serverStores;reply["event_id"]=doc["event_id"].as<const char *>();ageMsSeen.push_back(doc["age_ms"]|-1);
    if(serverStores)durableIds.push_back(doc["event_id"].as<const char *>());}
  std::string body;serializeJson(reply,body);if(response)*response=body.c_str();
  if(code)*code=200;
  return true;
}
#include "actual-cloud-send.inc"
#include "actual-cloud-drain.inc"
#include "actual-cloud-heartbeat.inc"
static void cloudRun(uint32_t ms,bool heartbeats=false){   // the Cloud task: drain every 100 ms, optionally heartbeat every 60 s, release the yield when idle
  const uint32_t end=clockMs+ms;                            // wall clock: a blocking send (PUBACK wait, HTTPS) consumes the budget too
  while(static_cast<int32_t>(clockMs-end)<0){
    drainOutbox(clockMs);if(heartbeats)serviceHeartbeat(clockMs);
    if(outboxCount==0&&!requestDeferred)mayapReleaseCloudTlsYield();
    simAdvance(100);}
}
static void resetWorld(){
  touchUnused();
  closeMqtt();for(int8_t i=0;i<static_cast<int8_t>(MayapUplink::SLOTS);++i)MayapUplink::finish(i);
  outboxHead=outboxTail=outboxCount=0;
  brokerUp=brokerAcks=serverUp=serverStores=true;brokerSilent=false;serverCode=-1;ageMsSeen.clear();uplinkMisses=0;firstMissAt=0;mqttAckDelay=150;net.connected=true;registered=true;
  holdOffUntil=0;probesSent=0;mqttClosesForYield=0;httpsAt.clear();durableIds.clear();publishedKinds.clear();httpsCalls=httpsOverlap=beaconCalls=0;beaconAt.clear();lastHeartbeatAt=0;lastUplinkOkAt=0;nextBeaconAt=0;beaconRetryPending=false;cloudBackoff=BackoffTimer{};
  fallbackGate=MayapAlarmFallback::Gate{};
  MayapUplink::onLinkDown();MayapUplink::Internal::health=MayapUplink::Health{};MayapUplink::healthClosed(clockMs,0U);mayapReleaseCloudTlsYield();
  MayapNetworkIoInternal::cloudYieldRetryAt=MayapNetworkIoInternal::cloudUrgentRetryAt=0;
  clockMs+=120000;
}
static void connect(){simAdvance(3000);assert(mayapMqttTlsResident());}
static uint32_t firstHttpsDelay(uint32_t t0){return httpsAt.empty()?0xFFFFFFFFU:httpsAt[0]-t0;}
}
int main(){
 using namespace MayapCloudInternal;
 using MayapAlarmFallback::Cause;
 // 0. The invariant behind the field bug, exhaustively: whatever the acknowledgement history, a link that is up and whose broker is
 //    talking (a byte within HALF_OPEN_RX_MS) NEVER routes to HTTPS and never counts as a confirmed-bad transport - except the one
 //    explicit last resort (critical && starved). Beacons likewise never fire on such a link.
 {
  using namespace MayapAlarmFallback;
  uint32_t cases=0;
  for(uint32_t rx:{0u,1u,5000u,14999u,15001u,19999u,20000u})
   for(int probePending=0;probePending<2;++probePending)for(uint32_t probeAge:{0u,3999u,4000u,60000u})
    for(int starved=0;starved<2;++starved)for(int critical=0;critical<2;++critical)for(int fb=0;fb<2;++fb)
     for(uint32_t oldest:{0u,1000u,60000u,3600000u}){
      LinkView v;v.wifiUp=true;v.registered=true;v.kind=LinkView::Kind::Up;v.rxAgeMs=rx;v.probePending=probePending;v.probeAgeMs=probeAge;
      v.starved=starved;v.fallbackSessionActive=fb;
      Gate g;Cause c=Cause::Down;bool wantProbe=true;
      const Route r=decide(g,clockMs,v,oldest,critical,&c,&wantProbe);
      ++cases;
      if(starved&&critical){assert(r==Route::Https&&c==Cause::BrokerNotStoring);}
      else {assert(r==Route::Mqtt&&c==Cause::Healthy&&!wantProbe);assert(!beaconDue(clockMs+1000000u,1000000u,0u,v));}
     }
  for(uint32_t rx:{20001u,25000u,29999u}){             // silent < 30 s: never an immediate verdict; the probe decides
   LinkView v;v.wifiUp=true;v.registered=true;v.kind=LinkView::Kind::Up;v.rxAgeMs=rx;
   Gate g;Cause c;bool wantProbe=false;
   assert(decide(g,clockMs,v,100000u,true,&c,&wantProbe)==Route::Wait&&c==Cause::Probing&&wantProbe&&!beaconDue(clockMs+1000000u,1000000u,0u,v));
   v.probePending=true;v.probeAgeMs=3999;assert(decide(g,clockMs,v,100000u,true,&c,&wantProbe)==Route::Wait&&c==Cause::Probing&&!wantProbe);
   v.probeAgeMs=4000;assert(decide(g,clockMs,v,0u,true,&c)==Route::Https&&c==Cause::HalfOpen&&beaconDue(clockMs+1000000u,1000000u,0u,v));
   Gate gr;assert(decide(gr,clockMs,v,0u,false,&c)==Route::Wait);   // routine: 20 s grace as for Down
  }
  for(uint32_t rx:{30000u,45000u,600000u}){            // silent >= 30 s (two keepalive rounds): confirmed without a probe
   LinkView v;v.wifiUp=true;v.registered=true;v.kind=LinkView::Kind::Up;v.rxAgeMs=rx;
   Gate g;Cause c;assert(decide(g,clockMs,v,0u,true,&c)==Route::Https&&c==Cause::HalfOpen&&beaconDue(clockMs+1000000u,1000000u,0u,v));
  }
  assert(cases==7*2*4*2*2*2*4);
 }
 // 1. Healthy MQTT: alarms and heartbeat travel over it; HTTPS never wakes, no HTTPS socket/context ever exists.
 resetWorld();connect();
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",true,29.9f,63.7f);
 enqueueRaw("LIGHT",NotifyLevel::Warning,false,"on",false,0,0);
 cloudRun(3000);
 assert(outboxCount==0&&httpsCalls==0&&mayapMqttTlsResident());
 assert(publishedKinds.size()==2&&publishedKinds[0]=="alarm");
 assert(sendHeartbeat()&&httpsCalls==0&&publishedKinds.back()=="heartbeat");
 assert(!lastAlarmTiming.https&&lastAlarmTiming.delivered);
 assert(mayapTlsContextsMax()==1&&mayapTlsOverlapViolations()==0);   // only the MQTT socket ever existed
 const uint32_t mqttRoute=lastAlarmTiming.routeToAckMs;
 // 2. MQTT confirmed gone (an established link died, next attempt 15 s away): a CRITICAL alarm no longer waits - HTTPS at the first
 //    drain, not after the former 10 s grace. Routine alarms still wait 20 s.
 resetWorld();connect();loseLink(15000);simAdvance(500);
 uint32_t t0=clockMs;
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);
 cloudRun(5000);
 const uint32_t downCriticalMs=firstHttpsDelay(t0);
 assert(httpsCalls==1&&outboxCount==0&&durableIds.size()==1&&downCriticalMs<=200);
 assert(lastAlarmTiming.https&&lastAlarmTiming.cause==Cause::Down&&lastAlarmTiming.delivered);
 // 3. A link that is only closed on purpose and back within 6 s (OTA/pressure hold-off) is WAITED for: the critical alarm goes over
 //    MQTT, no HTTPS handshake, no second TLS. Longer than 6 s counts as gone.
 resetWorld();connect();deliberateClose(3000);simAdvance(100);t0=clockMs;
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);
 cloudRun(8000);
 const uint32_t recoverMs=clockMs-t0;(void)recoverMs;
 assert(httpsCalls==0&&outboxCount==0&&publishedKinds.size()==1);
 resetWorld();connect();deliberateClose(20000);simAdvance(100);t0=clockMs;
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);
 cloudRun(5000);
 assert(httpsCalls==1&&outboxCount==0&&firstHttpsDelay(t0)<=200&&lastAlarmTiming.cause==Cause::Down);
 // 4. Routine alarms: 20 s when the link is gone, a full minute when it is about to return.
 resetWorld();connect();loseLink(15000);brokerUp=false;simAdvance(500);t0=clockMs;
 enqueueRaw("WIFI_SIGNAL_WEAK",NotifyLevel::Warning,false,"weak",false,0,0);
 cloudRun(19000);assert(httpsCalls==0);cloudRun(4000);assert(httpsCalls==1&&outboxCount==0);
 const uint32_t downRoutineMs=firstHttpsDelay(t0);
 resetWorld();connect();deliberateClose(3000);simAdvance(100);
 enqueueRaw("WIFI_SIGNAL_WEAK",NotifyLevel::Warning,false,"weak",false,0,0);
 cloudRun(10000);assert(httpsCalls==0&&outboxCount==0);          // MQTT came back and carried it
 // 5. THE FIELD BUG, reproduced: the link is up and the broker is talking, but the alarm's PUBACK does not come in time (D1/ingest slow,
 //    429 cooldown, queue busy). Before: suspect() -> HTTPS -> MQTT closed on purpose (cloud-tls) -> Web offline. Now: the event is kept
 //    and retried over MQTT; a probe cross-checks the socket; HTTPS never wakes; MQTT is never closed; no TLS overlap.
 resetWorld();connect();
 enqueueRaw("FAULT_131",NotifyLevel::Warning,false,"x",false,0,0);cloudRun(3000);assert(outboxCount==0);   // seeds the RTT EWMA
 assert(MayapUplink::ackWaitMs()==MayapUplink::ACK_WAIT_MIN_MS);
 brokerAcks=false;t0=clockMs;uint32_t closes=mqttClosesForYield;
 enqueueRaw("FAULT_130",NotifyLevel::Warning,false,"off",false,0,0);
 cloudRun(40000);                                                    // routine alarm: never starves into HTTPS
 assert(httpsCalls==0&&outboxCount==1&&httpsOverlap==0&&mqttClosesForYield==closes&&mayapMqttTlsResident());
 assert(probesSent>=1&&uplinkMisses>=2&&ackTimeouts>=2);              // transport cross-checked, ingest misses counted, nothing closed
 assert(MayapUplink::healthSnapshot().probesAnswered>=1);             // the broker answered every probe: the link is healthy
 {unsigned alarmsPublished=0;for(const std::string&k:publishedKinds)if(k=="alarm")++alarmsPublished;assert(alarmsPublished>=3&&alarmsPublished<=40);}   // retried over MQTT, bounded
 brokerAcks=true;cloudRun(30000);assert(outboxCount==0&&httpsCalls==0&&mqttClosesForYield==closes);   // ingest recovers: delivered over MQTT
 //    A slow ingest (PUBACK 6 s, beyond the 4 s wait): same outcome. The orphaned PUBACK frees its slot.
 resetWorld();connect();mqttAckDelay=6000;closes=mqttClosesForYield;
 enqueueRaw("FAULT_130",NotifyLevel::Warning,false,"off",false,0,0);enqueueRaw("FAULT_131",NotifyLevel::Warning,false,"x",false,0,0);
 cloudRun(120000);
 assert(httpsCalls==0&&mqttClosesForYield==closes&&mayapMqttTlsResident()&&outboxCount==2);
 mqttAckDelay=1500;cloudRun(60000);assert(outboxCount==0&&httpsCalls==0&&mqttClosesForYield==closes);
 //    100 alarms in a controlled stream over a healthy link whose PUBACKs take 3.8 s (inside the wait) and then 2 s: all delivered by MQTT.
 resetWorld();connect();closes=mqttClosesForYield;
 for(int n=0;n<100;++n){char id[16];snprintf(id,sizeof(id),"STREAM_%d",n%7);
   enqueueRaw(id,NotifyLevel::Critical,(n/7)%2==1,"m",false,0,0);mqttAckDelay=n<50?3800:2000;cloudRun(3000);}
 cloudRun(20000);
 assert(outboxCount==0&&httpsCalls==0&&mqttClosesForYield==closes&&mayapMqttTlsResident()&&durableIds.empty());
 //    The Worker/D1 being down is invisible to the device: the broker stored the event and acknowledged it.
 resetWorld();connect();serverUp=false;serverCode=503;closes=mqttClosesForYield;
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);cloudRun(5000);
 assert(outboxCount==0&&httpsCalls==0&&mqttClosesForYield==closes&&lastAlarmTiming.delivered&&!lastAlarmTiming.https);
 //    Without any RTT sample the wait is still the 4 s cap.
 resetWorld();connect();brokerAcks=false;
 enqueueRaw("FAULT_130",NotifyLevel::Warning,false,"off",false,0,0);t0=clockMs;
 cloudRun(6000);assert(ackTimeouts>=1&&httpsCalls==0&&mqttClosesForYield==0&&clockMs-t0>=6000);
 // 5b. LAST RESORT, explicit and rare: a CRITICAL alarm the broker has not stored for 60 s over >= 3 attempts, link proven alive by
 //     traffic and probes. Only then may HTTPS take the TLS slot, on the critical ladder, with the reason on record.
 resetWorld();connect();brokerAcks=false;t0=clockMs;closes=mqttClosesForYield;
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);
 cloudRun(55000);assert(httpsCalls==0&&mqttClosesForYield==closes&&outboxCount==1);      // patient for the first minute
 cloudRun(60000);
 const uint32_t starvedMs=firstHttpsDelay(t0);
 assert(httpsCalls==1&&outboxCount==0&&httpsOverlap==0&&mqttClosesForYield==closes+1);
 assert(lastAlarmTiming.https&&lastAlarmTiming.cause==Cause::BrokerNotStoring&&starvedMs>=MayapAlarmFallback::STARVE_MS&&starvedMs<=MayapAlarmFallback::STARVE_MS+20000);
 assert(uplinkMisses==0&&firstMissAt==0);                                                // delivered: the streak is over
 //     ...and MQTT is given back at once when the fallback finished (not after a 15 s lease)
 {const uint32_t t1=clockMs;brokerAcks=true;cloudRun(12000);assert(mayapMqttTlsResident());(void)t1;}
 const uint32_t noAckColdMs=starvedMs;
 // 6. Half-open, proven independently of any acknowledgement: the broker has sent nothing for > 20 s AND the probe (PINGREQ) goes
 //    unanswered for 4 s. Only then is the transport called gone.
 resetWorld();connect();brokerSilent=true;simAdvance(25000);t0=clockMs;
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);
 cloudRun(15000);
 assert(probesSent>=1&&httpsCalls==1&&outboxCount==0&&lastAlarmTiming.cause==Cause::HalfOpen);
 assert(firstHttpsDelay(t0)>=MayapAlarmFallback::PROBE_WAIT_MS-200&&firstHttpsDelay(t0)<=MayapAlarmFallback::PROBE_WAIT_MS+2500);   // waited for the probe, no longer
 //    Silent for less than the half-open window: no verdict, no HTTPS (the alarm waits for the pending link rather than guessing).
 resetWorld();connect();brokerSilent=true;simAdvance(10000);
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);cloudRun(8000);
 assert(httpsCalls==0&&mayapMqttTlsResident());
 // 7. A silent-but-answering broker (probe answered) is a healthy transport whatever the acknowledgements say: no verdict at all.
 resetWorld();connect();brokerAcks=false;uplinkMisses=0;
 assert(!sendHeartbeat());assert(!sendHeartbeat());assert(heartbeatTimeouts==2U);
 enqueueRaw("FAULT_130",NotifyLevel::Warning,false,"off",false,0,0);
 cloudRun(10000);
 assert(httpsCalls==0&&mqttClosesForYield==0&&mayapMqttTlsResident());
 // 8. Flapping: three losses inside two minutes. Even though each reconnect is "due in 3 s", the fallback does not wait for a
 //    link that keeps dying.
 resetWorld();
 for(int n=0;n<3;++n){connect();loseLink(3000);simAdvance(3500);}
 connect();loseLink(3000);simAdvance(100);t0=clockMs;
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);
 cloudRun(5000);
 assert(httpsCalls==1&&firstHttpsDelay(t0)<=200&&lastAlarmTiming.cause==Cause::Flapping);
 // 9. Cloudflare down too (any reason: Worker, D1, DNS, Internet): the CRITICAL ladder 5/15/30/60/120/300 s (+jitter, separate from
 //    the routine ladder) bounds the retries - no reconnect storm in an hour - and nothing is lost.
 for(int mode=0;mode<3;++mode){
   resetWorld();connect();loseLink(15000);brokerUp=false;serverUp=false;serverCode=mode==0?-1:mode==1?500:503;simAdvance(500);
   enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);
   cloudRun(3600000);
   assert(httpsCalls>=14&&httpsCalls<=19&&outboxCount==1);
   for(size_t i=1;i<httpsAt.size();++i){
     const uint32_t gap=httpsAt[i]-httpsAt[i-1]-2500;          // the request itself takes 2.5 s
     const size_t rung=i-1<MayapAlarmFallback::CRITICAL_STEPS?i-1:MayapAlarmFallback::CRITICAL_STEPS-1;
     assert(gap+200>=MayapAlarmFallback::CRITICAL_BACKOFF_MS[rung]);}
 }
 const unsigned stormCalls=httpsCalls;
 // 9b. The Worker answers 200 but did not store (D1 fault): the alarm is NOT removed, it is retried on the ladder.
 resetWorld();brokerUp=false;serverStores=false;simAdvance(1000);
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);
 cloudRun(60000);assert(httpsCalls>=3&&outboxCount==1&&durableIds.empty());
 serverStores=true;cloudRun(400000);assert(outboxCount==0&&durableIds.size()==1);
 // 9c. Separate ladders: a failing routine fallback never delays a new critical alarm.
 resetWorld();brokerUp=false;serverUp=false;simAdvance(1000);
 enqueueRaw("WIFI_SIGNAL_WEAK",NotifyLevel::Warning,false,"weak",false,0,0);
 cloudRun(26000);assert(httpsCalls==1);                         // failed, routine now backs off >= 30 s
 t0=clockMs;httpsAt.clear();
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);
 cloudRun(3000);assert(httpsCalls==2&&firstHttpsDelay(t0)<=200);
 // 10. MQTT recovers: the stuck alarm goes over it, the fallback sleeps and both ladders reset; later alarms stay on MQTT.
 brokerUp=true;serverUp=false;const unsigned callsAtRecovery=httpsCalls;
 cloudRun(120000);
 assert(outboxCount<=1&&!fallbackGate.armed&&httpsCalls<=callsAtRecovery+2);
 cloudRun(120000);assert(outboxCount==0&&!fallbackGate.armed);
 const unsigned callsAfter=httpsCalls;
 enqueueRaw("FAULT_131",NotifyLevel::Critical,false,"x",false,0,0);
 cloudRun(15000);assert(outboxCount==0&&httpsCalls==callsAfter&&!lastAlarmTiming.https);
 // 11. No Wi-Fi: nothing is attempted (and nothing is penalised); it flows once the network is back.
 resetWorld();connect();brokerUp=false;net.connected=false;simAdvance(1000);
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);
 cloudRun(300000);assert(httpsCalls==0&&outboxCount==1);
 net.connected=true;cloudRun(20000);assert(httpsCalls==1&&outboxCount==0);
 // 11b. Wi-Fi flaps every 5 s for 2 minutes with a critical alarm pending: bounded attempts, no overlap, delivered at the end.
 resetWorld();connect();enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);brokerUp=false;
 for(int n=0;n<24;++n){net.connected=(n%2)==1;cloudRun(5000);}
 net.connected=true;brokerUp=true;cloudRun(60000);
 assert(outboxCount==0&&durableIds.size()<=1&&httpsOverlap==0);
 // 12. Not registered: no HTTPS fallback (the Worker would answer 404), the alarm is simply kept.
 resetWorld();connect();loseLink(15000);brokerUp=false;registered=false;simAdvance(1000);
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);
 cloudRun(120000);assert(httpsCalls==0&&outboxCount==1);
 // 13. Heartbeat is MQTT only: with the link down it is skipped silently (no HTTPS, no Cloud back-off).
 resetWorld();connect();loseLink(15000);simAdvance(1000);
 assert(!sendHeartbeat()&&requestDeferred&&httpsCalls==0);
 // 13b. Beacon: with the MQTT path dead and no alarms, the Worker still hears from the machine over HTTPS - one small request per ~2 min,
 //      only after 90 s of MQTT silence - so a broker outage is not mistaken for a dead machine (false DEVICE_OFFLINE). Healthy MQTT: none.
 resetWorld();connect();cloudRun(600000,true);
 assert(beaconCalls==0&&httpsCalls==0&&lastUplinkOkAt!=0);                                  // heartbeats ride MQTT
 resetWorld();connect();loseLink(15000);brokerUp=false;simAdvance(500);
 cloudRun(1800000,true);
 const unsigned beacons30min=beaconCalls;
 assert(beacons30min>=12&&beacons30min<=15&&httpsOverlap==0);                               // one per ~2 min (60 s heartbeat tick), never a storm
 for(size_t i=1;i<beaconAt.size();++i){const uint32_t gap=beaconAt[i]-beaconAt[i-1];assert(gap>=MayapAlarmFallback::BEACON_INTERVAL_MS&&gap<=150000);}   // always inside the Worker's 180 s threshold
 brokerUp=true;cloudRun(300000,true);                                                         // MQTT returns: beacons stop
 const unsigned beaconsAfterRecovery=beaconCalls;
 cloudRun(600000,true);assert(beaconCalls==beaconsAfterRecovery);
 // G. A healthy socket whose heartbeat PUBACKs do not come back (ingest slow/stuck) NEVER beacons: no HTTPS, no TLS yield, MQTT stays up
 //    for the whole half hour. (Before: lastUplinkOk went stale, the suspect link made the beacon due, HTTPS closed MQTT.)
 resetWorld();connect();brokerAcks=false;
 cloudRun(1800000,true);
 assert(beaconCalls==0&&httpsCalls==0&&mqttClosesForYield==0&&mayapMqttTlsResident()&&heartbeatTimeouts>=25);
 //    ...and when the transport is really half-open (silent + unanswered probe) the beacon does come, because the Worker would otherwise
 //    declare the machine offline.
 resetWorld();connect();brokerSilent=true;
 cloudRun(900000,true);
 assert(beaconCalls>=5&&httpsOverlap==0);
 // A link that is merely closed for a moment (Recovering) never beacons.
 resetWorld();connect();deliberateClose(3000);simAdvance(100);lastUplinkOkAt=clockMs-MayapAlarmFallback::BEACON_SILENT_MS-1;
 cloudRun(61000,true);assert(beaconCalls==0);
 // 14. Concurrent alarms: a backlog with raise+recover pairs leaves in few requests, per-alarm order preserved (the raise id is
 //     delivered before its recovery id), none lost, never overlapping MQTT; events carry their age.
 resetWorld();connect();loseLink(15000);simAdvance(1000);
 for(int n=0;n<5;++n){char id[16];snprintf(id,sizeof(id),"ALARM_%d",n);
   enqueueRaw(id,NotifyLevel::Critical,false,"m",false,0,0);enqueueRaw(id,NotifyLevel::Critical,true,"ok",false,0,0);}
 cloudRun(60000);
 assert(outboxCount==0&&httpsCalls<=3&&durableIds.size()==10);
 {std::vector<unsigned long> seq;                         // delivery order of the sequence numbers inside the event ids
  for(const std::string &id:durableIds)seq.push_back(std::stoul(id.substr(id.rfind('-')+1),nullptr,16));
  for(size_t i=0;i<seq.size();++i)for(size_t j=0;j<seq.size();++j)
    if(seq[j]==seq[i]+1&&((seq[i]-seq[0])%2)==0)assert(i<j);}   // each raise (even offset) reaches the Worker before its recovery
 for(int age:ageMsSeen)assert(age>=0);
 // 15. Outbox full during a long outage: routine admissions are refused (counted) before the critical headroom is used; critical
 //     events keep their slots; everything queued is delivered in order once the path returns.
 resetWorld();connect();loseLink(15000);brokerUp=false;serverUp=false;simAdvance(1000);
 const uint32_t droppedBefore=outboxDropped;
 for(int n=0;n<20;++n){char id[16];snprintf(id,sizeof(id),"WARN_%d",n);enqueueRaw(id,NotifyLevel::Warning,false,"w",false,0,0);}
 assert(outboxCount==CLOUD_OUTBOX_SIZE-4U&&outboxDropped-droppedBefore==8U);
 for(int n=0;n<4;++n){char id[16];snprintf(id,sizeof(id),"CRIT_%d",n);assert(enqueueRaw(id,NotifyLevel::Critical,false,"c",false,0,0));}
 assert(outboxCount==CLOUD_OUTBOX_SIZE&&!enqueueRaw("CRIT_X",NotifyLevel::Critical,false,"c",false,0,0));   // visible refusal, never a silent overwrite
 cloudRun(1800000);serverUp=true;brokerUp=true;cloudRun(900000);
 size_t viaMqtt=0;for(const std::string &k:publishedKinds)if(k=="alarm")++viaMqtt;
 assert(outboxCount==0&&durableIds.size()+viaMqtt>=CLOUD_OUTBOX_SIZE);   // every queued event reached the Worker (HTTPS receipts + MQTT PUBACKs)
 // Whole run: never two TLS contexts, never an HTTPS request while MQTT was resident, heap never below the bench floor.
 assert(mayapTlsContextsMax()==1&&mayapTlsOverlapViolations()==0&&httpsOverlap==0);
 assert(ESP.minFree>=50000);
 std::printf("Alarm fallback: MQTT primary, HTTPS cold standby PASS\n"
   "  critical fallback delay: link gone %lums (was >=10000), starved-critical last resort %lums (first minute always MQTT), routine gone %lums, recovering link: MQTT carries it (HTTPS 0)\n"
   "  late/missing PUBACK on a talking link: 0 HTTPS, 0 MQTT closes, probes answered; MQTT route->ack %lums; storm bound %u HTTPS attempts/h on the critical ladder;\n"
   "  exclusive TLS contexts_max=%u overlap=%lu denied=%lu; model min free heap %lu B, min largest block %lu B\n",
   static_cast<unsigned long>(downCriticalMs),static_cast<unsigned long>(noAckColdMs),static_cast<unsigned long>(downRoutineMs),
   static_cast<unsigned long>(mqttRoute),stormCalls,
   static_cast<unsigned>(mayapTlsContextsMax()),static_cast<unsigned long>(mayapTlsOverlapViolations()),static_cast<unsigned long>(mayapTlsOverlapDenied()),
   static_cast<unsigned long>(ESP.minFree),static_cast<unsigned long>(ESP.minLargest));
}
