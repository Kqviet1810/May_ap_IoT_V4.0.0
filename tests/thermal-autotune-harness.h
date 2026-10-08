#pragma once
#ifndef MAYAP_ADAPTIVE_OBSERVER_ONLY
#define MAYAP_ADAPTIVE_OBSERVER_ONLY 0
#endif
// Real config load/save, EventLog, AutoTune update and heating/output bodies.
// Host replaces only clocks, GPIO, EEPROM I/O and UI notification sinks.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <deque>
#include <vector>
#include <string>
using std::isfinite;
static uint32_t clockMs=0;
uint32_t millis(){return clockMs;}
uint32_t elapsedMs(uint32_t now,uint32_t then){return now-then;}
bool timeReached(uint32_t now,uint32_t then){return static_cast<int32_t>(now-then)>=0;}
int constrain(int v,int lo,int hi){return std::max(lo,std::min(hi,v));}
float clampFloat(float v,float lo,float hi){return std::max(lo,std::min(hi,v));}
constexpr float PI=3.14159265358979323846f;
#include "actual-config.inc"
#include "actual-tune-extra.inc"
#include "actual-autotune-constants.inc"
enum class AutoTuneState:uint8_t{Idle=0,Running=1,Success=2,Failed=3};
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_control.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_smart_autotune.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/heater_burst_scheduler.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/startup_output_policy.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_adaptive_v1.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_profile_storage.h"
constexpr int LOW=0,HIGH=1,OUTPUT=2;
static int levels[64]{};
static bool bootReady=true,trip=false,maintenance=false,serialEnabled=false;
static std::vector<std::string> diagnosticLines;
bool mayapBootOperationsReady(){return bootReady;}
bool mayapSystemTripLatched(){return trip;}
bool mayapFirmwareMaintenanceActive(){return maintenance;}
void digitalWrite(uint8_t pin,int v){assert(pin<64);levels[pin]=v;}
void pinMode(uint8_t pin,int mode){assert(pin<64 && mode==OUTPUT && levels[pin]==LOW);}
void mayapSerialPrintf(bool,const char *format,...){
  if(!serialEnabled)return;
  char line[256];va_list args;va_start(args,format);vsnprintf(line,sizeof(line),format,args);va_end(args);
  diagnosticLines.emplace_back(line);
}
template<class T,unsigned N> class FixedRing {
 public:void push(const T &v){if(q.size()==N){q.pop_front();++overflow;}q.push_back(v);}
  bool pop(T &v){if(q.empty())return false;v=q.front();q.pop_front();return true;}
  uint32_t overflowCount()const{return overflow;}
 private:std::deque<T> q;uint32_t overflow=0;
};
#include "actual-output.inc"
#include "actual-event.inc"
struct TuneStore {
  uint8_t bytes[1024];bool ready_=true,configCacheValid_=false,configCurrentIsA_=false;
  uint32_t configSequence_=0;PackedMachineConfigV1 configPayload_{};
  int writeBudget=-1;unsigned saves=0;
  TuneStore(){std::memset(bytes,0xff,sizeof(bytes));}
  template<class T> bool readRecord(uint16_t addr,T &r)const {
    assert(addr+sizeof(r)<=sizeof(bytes));std::memcpy(&r,bytes+addr,sizeof(r));return true;
  }
  template<class T> bool writeRecord(uint16_t addr,const T &r){
    ++saves;assert(addr+sizeof(r)<=sizeof(bytes));
    const size_t count=writeBudget<0?sizeof(r):std::min(sizeof(r),static_cast<size_t>(writeBudget));
    std::memcpy(bytes+addr,&r,count);return count==sizeof(r);
  }
#include "actual-config-load.inc"
};
void hmiSetConfig(const MachineConfig &){}
void mayapRealtimeSetConfig(const MachineConfig &){}
void mayapCloudSetConfig(const MachineConfig &){}
void mayapSetConnectivityMode(ConnectivityMode){}
struct InputState{bool light=false,autoMode=true,circulationFan=true,heaterEnable=true;};
struct FakeInputs{InputState in;const InputState &state()const{return in;}};
struct FakeRtc{bool usable=true;bool valid()const{return usable;}uint32_t epoch()const{return 1800000000;}};
struct FakeFaults{
  bool drop=false,inhibit=false,cooling=false;
  bool masterDropRequired()const{return drop;}bool ssrInhibited()const{return inhibit;}
  bool circulationForced()const{return cooling;}bool ventForced()const{return cooling;}
};
enum class TurnPhase{Idle,MovingLeft,MovingRight};
constexpr uint8_t HEATER_GROUP_COUNT=1;
struct TuneHarness {
  FakeInputs inputs_;FakeRtc rtc_;FakeFaults faults_;MachineConfig config_;
  TuneStore store_;EventLog eventLog_;OutputArbiter outputs_;
  ThermalController pid_;ThermalStartupController startupHeat_;AutoTuneEngine autotune_;
#include "actual-burst-member.inc"
  struct{float heaterPower=0;bool adaptiveEnabled=false,adaptiveSelfHeating=false;
    uint8_t adaptiveState=0,lastAdaptiveReason=0;
    float adaptiveConfidence=0,adaptiveLoadIndex=0,adaptiveCoastRiseC=0,adaptiveCoastTimeSec=0;
    float adaptiveHoldPowerPct=0,effectiveMaxPowerPct=0,adaptiveApproachBandC=0,adaptiveCoolingDemand=0;
    uint32_t observerValidWindows=0;
    uint8_t thermalLearnState=0,thermalConfidence=0,thermalVentPhase=0;
    float thermalGain=0,thermalDelaySec=0,thermalCoastC=0,thermalHoldPct=0,thermalVentGain=0,thermalPredictionError=0;}runtime_;
  bool testModeActive_=false,lightWebOverrideActive_=false,lightWebOverrideRefInput_=false,lightWebOverrideValue_=false;
  bool resumeConfirmationRequired_=false,resumePending_=false,batchRunning_=false,prevBatchRunningForOutputs_=false;
  bool sensorUsable_=true,humidityLowActive_=false,previousFanCommand_=false;
  bool emergencyActive_=false,highTemperatureActive_=false,ventTemperatureActive_=false;
  bool batchClearPending_=false,safetyJournalFaultLatched_=false,storageFaultLatched_=false,storageDegraded_=false,abnormalResetLatched_=false;
  bool newSensorSample_=true,batchOverdueSirenActive_=false,sirenSelfTestActive_=false;
  uint32_t circFanStaggerUntil_=0,postCoolUntil_=0,sensorStartupGraceUntil_=0,fanOnSince_=0,heatRestartNotBefore_=0,sirenMutedUntil_=0;
  float temperature_=25,rawTemperature_=25,humidity_=60,pidPower_=0;
  TurnPhase turnPhase_=TurnPhase::Idle;
  uint32_t batchElapsedSec_=0;
  uint32_t elapsedBatchSec(uint32_t)const{return batchElapsedSec_;}
  void updateTestModeOutputs(uint32_t now){outputs_.forceSafe(now);}
  void latchStorageFault(const char *){storageFaultLatched_=true;}
#include "actual-tune-start.inc"
#include "actual-tune-cancel.inc"
#include "actual-tune-update.inc"
#ifdef MAYAP_TEST_ADAPTIVE
  MayapAdaptive::AdaptiveThermalSupervisor adaptiveThermal_;
  MayapThermal::AdaptiveV1 thermalV1_;MayapThermal::VentInfo ventInfo_{};bool ventForcedRun_=false;
  uint32_t thermalDiagnosticAt_=0;
  MayapThermal::LearnState thermalLoggedState_=MayapThermal::LearnState::Unlearned;bool thermalMismatchLogged_=false;
  uint32_t adaptiveDiagnosticAt_=0,adaptiveSignature_=0;
  bool adaptiveSignatureSeen_=false;
  MayapAdaptive::State adaptiveLoggedState_=MayapAdaptive::State::Disabled;
  struct {uint8_t sensorProfile()const{return 1;}} sensor_;
#include "actual-adaptive.inc"
#else
  MayapThermal::AdaptiveV1 thermalV1_;MayapThermal::VentInfo ventInfo_{};bool ventForcedRun_=false;
  void publishThermalLearning(uint32_t){}
  uint32_t thermalSignature()const{return 0;}
  struct{uint32_t epoch()const{return 1800000000;}}rtcEpoch_;
  void trackAdaptiveEnergy(uint32_t){}
  float updateAdaptiveBalance(uint32_t,bool,bool,bool){return config_.maxHeaterPower;}
  bool adaptiveCoolingRequested()const{return false;}
#endif
#include "actual-heating.inc"
  explicit TuneHarness(uint8_t preheat=AUTOTUNE_PREHEAT_POWER_PERCENT):autotune_(preheat){outputs_.begin();MachineConfig back;assert(store_.saveConfig(config_,back));store_.saves=0;}
  void cycle(uint32_t now,bool sample=true){clockMs=now;newSensorSample_=sample;updateAutoTune(now);updateHeatingAndOutputs(now);}
  void sample(float raw,float filtered){rawTemperature_=raw;temperature_=filtered;}
};
