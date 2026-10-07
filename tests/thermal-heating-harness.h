#pragma once
#ifndef MAYAP_ADAPTIVE_OBSERVER_ONLY
#define MAYAP_ADAPTIVE_OBSERVER_ONLY 0
#endif
// Execute the actual MachineController heating method, not a mirrored gate expression.
#include "thermal-fixture.h"
#include <deque>
#include <cstddef>
#include <vector>
#ifdef THERMAL_V3_BASELINE
#include "fixtures/thermal-v3-baseline/thermal_control.h"
#else
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_control.h"
#endif
#include "../MAYAP_INDUSTRIAL_v1_0_0/heater_burst_scheduler.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/startup_output_policy.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_adaptive_v1.h"
static uint32_t clockMs=0;
uint32_t millis() { return clockMs; }
bool timeReached(uint32_t now,uint32_t then) { return static_cast<int32_t>(now-then)>=0; }
constexpr int LOW=0,HIGH=1,OUTPUT=2;
static int levels[64]{};
static bool bootReady=true,trip=false,maintenance=false;
bool mayapBootOperationsReady() { return bootReady; }
bool mayapSystemTripLatched() { return trip; }
bool mayapFirmwareMaintenanceActive() { return maintenance; }
void digitalWrite(uint8_t pin,int value) { assert(pin<64);levels[pin]=value; }
void pinMode(uint8_t pin,int mode) { assert(pin<64 && mode==OUTPUT && levels[pin]==LOW); }
template<class T,unsigned N> class FixedRing {
 public:
  void push(const T &v) { if(q.size()==N){q.pop_front();++overflow;}q.push_back(v); }
  bool pop(T &v){if(q.empty())return false;v=q.front();q.pop_front();return true;}
  uint32_t overflowCount()const{return overflow;}
 private:std::deque<T> q;uint32_t overflow=0;
};
#include "actual-output.inc"
#include "actual-heating-constants.inc"
#include "actual-heating-config.inc"
constexpr uint8_t HEATER_GROUP_COUNT=1;
struct InputState { bool light=false,autoMode=true,circulationFan=true,heaterEnable=true; };
struct FakeInputs { InputState in; const InputState &state()const{return in;} };
struct FakeRtc { bool valid()const{return false;} uint32_t epoch()const{return 0;} };
struct FakeFaults {
  bool drop=false,inhibit=false,cooling=false;
  bool masterDropRequired()const{return drop;}
  bool ssrInhibited()const{return inhibit;}
  bool circulationForced()const{return cooling;}
  bool ventForced()const{return cooling;}
};
uint8_t ventProfileDutyPercent(const HeatingConfig &,uint32_t,bool){return 0;}
enum class TurnPhase { Idle,MovingLeft,MovingRight };
struct Harness {
  FakeInputs inputs_;FakeRtc rtc_;FakeFaults faults_;
  HeatingConfig config_;
  OutputArbiter outputs_;
  ThermalController pid_;
#ifndef THERMAL_V3_BASELINE
  ThermalStartupController startupHeat_;
#endif
  RelayAutoTune autotune_;
#include "actual-burst-member.inc"
  struct {float heaterPower=0;} runtime_;
  bool testModeActive_=false,lightWebOverrideActive_=false,lightWebOverrideRefInput_=false,lightWebOverrideValue_=false;
  bool resumeConfirmationRequired_=false,batchRunning_=true,prevBatchRunningForOutputs_=false;
  bool sensorUsable_=true,humidityLowActive_=false,previousFanCommand_=false;
  bool emergencyActive_=false,highTemperatureActive_=false,ventTemperatureActive_=false;
  bool batchClearPending_=false,safetyJournalFaultLatched_=false,storageFaultLatched_=false,storageDegraded_=false,abnormalResetLatched_=false;
  bool newSensorSample_=true,batchOverdueSirenActive_=false,sirenSelfTestActive_=false;
  uint32_t circFanStaggerUntil_=0,postCoolUntil_=0,sensorStartupGraceUntil_=0,fanOnSince_=0,heatRestartNotBefore_=0,sirenMutedUntil_=0;
  float temperature_=25,humidity_=60,pidPower_=0;
  TurnPhase turnPhase_=TurnPhase::Idle;
  unsigned testCalls=0;
  uint32_t elapsedBatchSec(uint32_t)const{return 0;}
  void updateTestModeOutputs(uint32_t now){++testCalls;outputs_.forceSafe(now);}
  MayapThermal::AdaptiveV1 thermalV1_;MayapThermal::VentInfo ventInfo_{};bool ventForcedRun_=false;
  void publishThermalLearning(uint32_t){}
  void trackAdaptiveEnergy(uint32_t){}
  float updateAdaptiveBalance(uint32_t,bool,bool,bool){return config_.maxHeaterPower;}
  bool adaptiveCoolingRequested()const{return false;}
#ifdef THERMAL_V3_BASELINE
#include "fixtures/thermal-v3-baseline/heating.inc"
#else
#include "actual-heating.inc"
#endif
  void cycle(uint32_t now,bool newSample=true){clockMs=now;newSensorSample_=newSample;updateHeatingAndOutputs(now);}
  void warm(){outputs_.begin();for(uint32_t t=1000;t<=15000;t+=50)cycle(t,t%2000==0);}
};
