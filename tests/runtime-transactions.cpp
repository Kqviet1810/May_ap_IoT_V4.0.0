// Force controller consumption at each HMI unlock using actual production code.
#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <string>
#include <ctime>
using std::min;
template<class T>T constrain(T v,T a,T b){return std::max(a,std::min(v,b));}
static uint32_t clockMs=100;uint32_t millis(){return clockMs;}
bool timeReached(uint32_t a,uint32_t b){return static_cast<int32_t>(a-b)>=0;}
uint32_t elapsedMs(uint32_t a,uint32_t b){return a-b;}
static int realtimeMux=0,hmiApiMux=1,hmiDepth=0;static bool autoRun=false,inController=false;
void controller();void enter(int *p){if(p==&hmiApiMux)++hmiDepth;}
void leave(int *p){if(p==&hmiApiMux)--hmiDepth;if(autoRun&&!hmiDepth&&!inController)controller();}
#define portENTER_CRITICAL(p) enter(p)
#define portEXIT_CRITICAL(p) leave(p)
constexpr uint8_t COMMAND_QUEUE_SIZE=4;
constexpr size_t REALTIME_REQUEST_ID_CAPACITY=40;
constexpr uint32_t AlarmNone=0,ALARM_KNOWN_MASK=0xffff,REALTIME_COMMAND_ACK_TIMEOUT_MS=8000,REALTIME_CONFIG_SAVE_ACK_TIMEOUT_MS=8000;
constexpr uint16_t COMMAND_DEFAULT_VALID_MS=5000,COMMAND_AUTOTUNE_VALID_MS=5000;
#include "actual-hmi-command-type.inc"
enum class HmiCommandSource{Local,Remote};enum class BuzzerCue{Error};
struct HmiCommand{uint32_t id=0;HmiCommandType type=HmiCommandType::None;uint32_t createdAt=0;uint16_t validForMs=0,actuatorLeaseMs=0;uint32_t alarmMask=0;HmiCommandSource source=HmiCommandSource::Local;};
// Technical-access pieces are the REAL headers (pure C++). MachineConfig only carries the fields the bridge reads.
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#include "advanced_history.h"
#include "tech_request.h"
struct MachineConfig{float kp=5,ki=0.1f,kd=30,tempOffset=0,humidityOffset=0,heaterStuckMinRiseC=0.3f,tempRateLimitC=2,autotuneBandC=0.2f;
 uint16_t pidCycleSec=2,heaterStuckDurationSec=600,tempRateWindowSec=300,tempOscillationWindowSec=900;
 uint8_t maxHeaterPower=100,tempOscillationCrossLimit=6,autotuneRelayPowerPercent=40;bool adaptiveThermalBalanceEnabled=false,humidifierInstalled=false;};
static HmiCommand commandQueue[4];static uint8_t commandTail=0,commandHead=0,commandCount=0,commandOutstandingCount=0;static uint32_t nextCommandId=1;
bool commandConflictLocked(HmiCommandType){return false;}void showToast(const char*,bool){}void buzzerPlayCue(BuzzerCue){}
#include "actual-transaction-hmi.inc"
void mayapRealtimeConfirmCommand(uint32_t,bool,const char*);
namespace MayapRealtimeInternal {
static bool activeAckKeyValid=true;static uint8_t activeAckKey[32]={7};static char activeOperation[40]="light.toggle";
static uint32_t bootId=123,lastCommandSequence=0;static char lastCommandRequestId[40]="";
static bool knownRuntimeValid=true;static struct{uint32_t alarmMask=0;struct{bool webAllowed=false,webSession=false;}tech;}knownRuntime;
static MachineConfig knownConfig;static bool knownConfigValid=true;
static bool configDirty=false;
static uint32_t realtimeConfigRevision=0,lastVerifiedConfigRevision=0;
static char lastVerifiedConfigRequestId[40]="";
#include "actual-transaction-state.inc"
static std::vector<std::string>acks;static bool bulkRoom=true;bool bulkHasRoom(){return bulkRoom;}
bool publishAck(const char *,const char *,const char *,const char * = "",uint32_t=0,uint32_t=0,const uint8_t * = nullptr);
bool publishJson(const char *,const JsonDocument &,bool);
static char deviceId[]="MAP-1234567890AB";
static uint32_t lastDeviceCompletedAt=0;static bool forceSnapshotPublish=false;
const char *ackCode(const char *r,const char*){return r;}
const char *ackFriendlyMessage(const char*,const char *m){return m;}
// Crypto is an injected HAL here; production HMAC vectors are covered in Node.
struct mbedtls_md_info_t{};constexpr int MBEDTLS_MD_SHA256=1;
const mbedtls_md_info_t *mbedtls_md_info_from_type(int){static mbedtls_md_info_t info;return &info;}
int mbedtls_md_hmac(const mbedtls_md_info_t*,const uint8_t*,size_t,const uint8_t*,size_t,uint8_t *digest){memset(digest,7,32);return 0;}
#include "actual-transaction-terminal.inc"
bool realtimeCommandChannelTrusted(){return true;}
static bool publishCallback=true;
constexpr uint32_t TEMP_HISTORY_SAMPLE_SEC=300;
struct MayapTemperatureHistoryPoint{uint32_t epoch=0;int16_t temperatureX10=375;};
uint8_t mayapTemperatureHistoryReadStatus(uint32_t n,MayapTemperatureHistoryPoint&p){p.epoch=n*300;return 2;}
static bool historyResponsePending=false,historySignedAck=true,historyReadError=false,failSend=false;
static uint16_t historyWindowMinutes=30,historyCursor=0,historyCandidateCount=0,historySampleCount=0;
static uint32_t historySnapshotEpoch=1800000000;static char historyRequestId[40]="history";static uint8_t historyAckKey[32]={7};
static std::vector<uint16_t>cursors;static uint32_t lastAckRevision=0;
bool publishJson(const char *channel,const JsonDocument &doc,bool){if(failSend)return false;if(!strcmp(channel,"ack")){acks.push_back(doc["result"].as<std::string>());lastAckRevision=doc["revision"].as<uint32_t>();}else cursors.push_back(doc["cursor"].as<uint16_t>());return true;}
#include "actual-transaction-dispatch.inc"
}
#include "actual-transaction-confirm.inc"
static int executions=0,saves=0;
void controller(){inController=true;
 while(commandCount){const auto cmd=commandQueue[commandHead];commandHead=(commandHead+1)%4;--commandCount;--commandOutstandingCount;++executions;mayapRealtimeConfirmCommand(cmd.id,true,"APPLIED");}

 inController=false;
}
using namespace MayapRealtimeInternal;
void reset(){autoRun=false;for(auto &p:pendingCommands)p=PendingCommand{};for(auto &a:ackOutbox)a=AckOutboxItem{};
 pendingConfigSave=PendingConfigSave{};
 for(auto &t:terminalCache) { t=TerminalResult{}; }
 terminalCursor=0;
 commandTail=commandHead=commandCount=commandOutstandingCount=0;executions=saves=0;acks.clear();clockMs=100;}
JsonDocument command(){JsonDocument d;d["v"]=2;d["requestId"]="cmd";d["bootId"]=123;d["expiresAt"]=time(nullptr)+20;d["action"]="light_toggle";return d;}
JsonDocument techCommand(const char *action){JsonDocument d=command();d["action"]=action;return d;}
static void techState(bool allowed,bool session){knownRuntime.tech.webAllowed=allowed;knownRuntime.tech.webSession=session;MayapTech::discard();}
static const HmiCommand &lastQueued(){return commandQueue[(commandTail+3)%4];}
// Web technical access at the MQTT edge: hidden/locked never parks anything, a request is only PARKED (never executed),
// the PIN travels as digits to the ESP32, queue/tracker saturation never leaves a parked request behind.
static void techTests(){
 // hidden: every technical entry is refused before anything is staged or queued
 reset();techState(false,false);
 { JsonDocument d=techCommand("tech_request");d["kind"]="config";d["fields"]["kp"]=9.0;handleCommandMessage(d);assert(acks.back()=="rejected");assert(commandCount==0);MayapTech::Staged s;assert(!MayapTech::take(s)); }
 { JsonDocument d=techCommand("autotune_start");handleCommandMessage(d);assert(acks.back()=="rejected"&&commandCount==0);MayapTech::Staged s;assert(!MayapTech::take(s)); }
 // permission ON but Web not unlocked: still refused
 reset();techState(true,false);
 { JsonDocument d=techCommand("tech_request");d["kind"]="config";d["fields"]["kp"]=9.0;handleCommandMessage(d);assert(acks.back()=="rejected"&&commandCount==0); }
 // unlocked: a valid config request is parked and queued as TechRequestSubmit, never as a config save
 reset();techState(true,true);
 { JsonDocument d=techCommand("tech_request");d["kind"]="config";d["fields"]["kp"]=9.0;d["fields"]["maxHeaterPower"]=70;handleCommandMessage(d);
   assert(acks.back()=="accepted"&&commandCount==1);
   assert(lastQueued().type==HmiCommandType::TechRequestSubmit&&lastQueued().source==HmiCommandSource::Remote);
   MayapTech::Staged s;assert(MayapTech::take(s));assert(s.kind==MayapTech::Kind::Config);
   assert(s.mask==((1U<<AdvancedHistory::F_KP)|(1U<<AdvancedHistory::F_MAX_POWER)));assert(s.values.kp==9.0f&&s.values.maxHeaterPower==70);
   assert(s.values.ki==knownConfig.ki); }                             // untouched fields keep the device value
 // a parked request blocks the next one (one pending request) and the queue is not touched
 reset();techState(true,true);
 { JsonDocument d=techCommand("tech_request");d["kind"]="config";d["fields"]["kp"]=9.0;handleCommandMessage(d);const uint8_t queued=commandCount;
   JsonDocument e=techCommand("tech_request");e["requestId"]="cmd2";e["kind"]="config";e["fields"]["ki"]=1.0;handleCommandMessage(e);
   assert(acks.back()=="busy"&&commandCount==queued); }
 // out-of-range / unknown / non numeric fields are refused, nothing is clamped silently
 for(int variant=0;variant<5;variant++){
   reset();techState(true,true);JsonDocument d=techCommand("tech_request");d["kind"]="config";
   if(variant==0)d["fields"]["kp"]=500.0;else if(variant==1)d["fields"]["notAField"]=1;else if(variant==2)d["fields"]["kp"]="9";
   else if(variant==3)d["fields"]["maxHeaterPower"]=70.5;else d["kind"]="bogus";
   handleCommandMessage(d);assert(acks.back()=="invalid"&&commandCount==0);MayapTech::Staged s;assert(!MayapTech::take(s)); }
 // a legacy autotune_start is only a request now
 reset();techState(true,true);
 { JsonDocument d=techCommand("autotune_start");handleCommandMessage(d);assert(acks.back()=="accepted"&&commandCount==1);
   assert(lastQueued().type==HmiCommandType::TechRequestSubmit&&lastQueued().type!=HmiCommandType::AutoTuneStart);
   MayapTech::Staged s;assert(MayapTech::take(s)&&s.kind==MayapTech::Kind::AutoTune); }
 // saturation: a full tracker / full queue leaves nothing parked
 reset();techState(true,true);for(auto &p:pendingCommands)p.used=true;
 { JsonDocument d=techCommand("tech_request");d["kind"]="config";d["fields"]["kp"]=9.0;handleCommandMessage(d);assert(acks.back()=="busy");MayapTech::Staged s;assert(!MayapTech::take(s)); }
 reset();techState(true,true);commandOutstandingCount=4;
 { JsonDocument d=techCommand("tech_request");d["kind"]="config";d["fields"]["kp"]=9.0;handleCommandMessage(d);assert(acks.back()=="busy");MayapTech::Staged s;assert(!MayapTech::take(s)); }
 // PIN unlock: digits only, forwarded as a number to the ESP32; malformed PINs never reach the queue
 reset();
 { const char *bad[]={"123","12345","12a4","",nullptr};
   for(const char **p=bad;*p;++p){reset();JsonDocument d=techCommand("tech_unlock");d["pin"]=*p;handleCommandMessage(d);assert(acks.back()=="invalid"&&commandCount==0);}
   reset();JsonDocument d=techCommand("tech_unlock");d["pin"]="0427";handleCommandMessage(d);
   assert(acks.back()=="accepted"&&lastQueued().type==HmiCommandType::TechPinVerify&&lastQueued().alarmMask==427UL&&lastQueued().source==HmiCommandSource::Remote); }
 // a Web client can NOT reach the HMI-only commands by name
 reset();techState(true,true);
 { const char *hmiOnly[]={"tech_pin_set","tech_decision","advanced_restore","tech_web_set","tech_set_pin",nullptr};
   for(const char **p=hmiOnly;*p;++p){reset();JsonDocument d=techCommand(*p);handleCommandMessage(d);assert(acks.back()=="unsupported"&&commandCount==0);} }
}
int main(){
 reset();autoRun=true;handleCommandMessage(command());assert(executions==1&&pendingCommands[0].completed&&pendingCommands[0].commandId!=0);flushCompletedTransactions();assert(!pendingCommands[0].used&&ackOutbox[0].signedAck&&!strcmp(ackOutbox[0].requestId,"cmd"));
 reset();for(auto &p:pendingCommands)p.used=true;autoRun=true;handleCommandMessage(command());assert(executions==0&&commandCount==0&&acks.back()=="busy");
 reset();commandOutstandingCount=4;handleCommandMessage(command());for(const auto&p:pendingCommands)assert(!p.used);
 reset();handleCommandMessage(command());for(auto &a:ackOutbox)a.used=true;controller();expirePendingCommands(9000);assert(pendingCommands[0].completed&&pendingCommands[0].used);ackOutbox[0].used=false;flushCompletedTransactions();assert(!pendingCommands[0].used&&!strcmp(ackOutbox[0].requestId,"cmd"));
 reset();handleCommandMessage(command());clockMs=8200;expirePendingCommands(clockMs);assert(pendingCommands[0].used&&pendingCommands[0].uncertainSent);clockMs=10000;controller();flushCompletedTransactions();assert(!pendingCommands[0].used&&!strcmp(ackOutbox[0].result,"applied"));
 reset();handleCommandMessage(command());expirePendingCommands(8200);expirePendingCommands(120101);assert(!pendingCommands[0].used);
 reset();historyCursor=historySampleCount=0;historyCandidateCount=18;historyResponsePending=true;failSend=true;serviceHistoryResponse();assert(historyCursor==0&&historySampleCount==0&&historyResponsePending&&acks.empty());
 failSend=false;serviceHistoryResponse();assert(historyCursor==12&&historySampleCount==12&&historyResponsePending);failSend=true;serviceHistoryResponse();assert(historyCursor==12&&historySampleCount==12&&historyResponsePending&&acks.empty());failSend=false;serviceHistoryResponse();assert(historyCursor==18&&historySampleCount==18&&!historyResponsePending&&acks.back()=="applied");assert(cursors.size()==2&&cursors[0]==0&&cursors[1]==12);
 historyCandidateCount=0;historyResponsePending=true;failSend=true;acks.clear();serviceHistoryResponse();assert(historyResponsePending&&acks.empty());failSend=false;serviceHistoryResponse();assert(!historyResponsePending&&acks.back()=="applied");
 // History is a bulk lane: with no room on the wire/window nothing is read from the EEPROM and nothing is published, the request just waits.
 reset();historyCursor=historySampleCount=0;historyCandidateCount=18;historyResponsePending=true;failSend=false;bulkRoom=false;acks.clear();serviceHistoryResponse();assert(historyCursor==0&&historySampleCount==0&&historyResponsePending&&acks.empty());
 bulkRoom=true;serviceHistoryResponse();assert(historyCursor==12&&historyResponsePending);serviceHistoryResponse();assert(historyCursor==18&&!historyResponsePending);
 reset();failSend=false;publishAck("cache","expired","");assert(terminalCursor==0);publishAck("cache","accepted","");assert(terminalCursor==0);publishAck("cache","applied","APPLIED","light.toggle");assert(terminalCursor==1);
 // A retry of the same terminal result (the ack outbox publishes again until the wire accepts it) is not news: it must not ask for another
 // forced snapshot (the old code reset the snapshot clock on every retry, so a snapshot was due on every pass and kept beating the ack).
 forceSnapshotPublish=false;publishAck("cache","applied","APPLIED","light.toggle");publishAck("cache","applied","APPLIED","light.toggle");assert(!forceSnapshotPublish&&terminalCursor==1);
 publishAck("fresh","applied","APPLIED","light.toggle");assert(forceSnapshotPublish&&terminalCursor==2);   // a NEW result still asks for one
 forceSnapshotPublish=false;publishAck("fresh","rejected","BUSY","light.toggle");assert(forceSnapshotPublish);                 // same request, different verdict: news
 terminalCursor=1;
 for(int i=0;i<100;i++) { assert(replayTerminal("cache")); }
 assert(terminalCursor==1&&!strcmp(terminalCache[0].requestId,"cache")&&!strcmp(terminalCache[0].result,"applied"));
 techTests();
 puts("Actual Web technical-access edge: hidden/locked refusal, parked-only requests, bounded fields, PIN digits, saturation PASS");
 puts("Actual transactions: immediate-controller admission, saturated trackers/outbox, late completion and failed history chunks PASS");
}
