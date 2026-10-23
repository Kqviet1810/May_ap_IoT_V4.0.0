#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include "../MAYAP_INDUSTRIAL_v1_0_0/turn_schedule_policy.h"
#include "actual-turn-enums.inc"
enum class TrayPosition { Unknown, Left, Right };
enum class TurnDirection { Left, Right };
enum class TurnPhase { Idle, Fault, MovingLeft, MovingRight, DeadtimeLeft, DeadtimeRight };
enum class BatchPhase { Prestart, Homing, Running };
constexpr unsigned TURN_LOCKDOWN_DAYS=3, POST_COOL_MS=1000,
 TURN_LIMIT_RELEASE_TIMEOUT_MS=1000, TURN_INPUT_CONFLICT_MS=1000, TURN_DIRECTION_DEADTIME_MS=100;
constexpr bool HOME_TO_LEFT=true;
uint32_t elapsedMs(uint32_t n,uint32_t t) { return n-t; }
bool timeReached(uint32_t n,uint32_t t) { return int32_t(n-t)>=0; }
void mayapSerialPrintf(bool,const char*,...) {}
struct InputState { bool autoMode=true,limitLeft=true,limitRight=false,turnLeft=false,turnRight=false,circulationFan=true; };
struct Controller {
 uint8_t webTurnDir_=0; uint32_t webTurnUntil_=0; bool webOverrideLive(uint32_t u,uint32_t n) const { return u!=0U && (int32_t)(n-u)<0; }
 struct { uint32_t now=1000000; bool ok=true; bool valid() const { return ok; } uint32_t epoch() const { return now; } } rtc_;
 struct { InputState value; const InputState& state() const { return value; } } inputs_;
 struct { bool inhibit=false; bool turningInhibited() const { return inhibit; } } faults_;
 struct { struct State { bool heaterSsr=false; } value; State state() const { return value; } } outputs_;
 struct { bool turningEnabled=true,manualTurnReanchorsSchedule=false; uint16_t turnIntervalMin=120,turnMaxRunSec=30,totalIncubationDays=21; TurnDirection nextDirection=TurnDirection::Right; } config_;
 struct { template<class... T> void push(T...) {} } eventLog_;
 struct { bool setTurnFaultStreak(unsigned) { return true; } } safetyJournal_;
 bool batchRunning_=true,needHome_=true,previousAutoMode_=true,testModeActive_=false,turnFaultLatched_=false,
 highTemperatureActive_=false,emergencyActive_=false,sensorUsable_=true,manualTurnRearmRequired_=false,
 resumePending_=false,moveIsHoming_=false,moveCounts_=false;
 uint32_t lastTurnAt_=0,lastTurnEpoch_=1000000,nextTurnAt_=0,turnAnchorWaitSince_=0,
 postCoolUntil_=0,moveStartedAt_=0,deadtimeUntil_=0,manualConflictSince_=0,turnCountBatch_=0,batchElapsed=0;
 uint16_t turnCountToday_=0; unsigned turnFaultStreak_=0,saves=0;
 TrayPosition trayPosition_=TrayPosition::Unknown,moveOrigin_=TrayPosition::Unknown;
 TurnPhase turnPhase_=TurnPhase::Idle; BatchPhase batchPhase_=BatchPhase::Homing;
 uint32_t elapsedBatchSec(uint32_t) const { return batchElapsed; }
 bool saveBatchRecord() { ++saves; return true; }
 void latchTurnFault(FaultCode,const char*) { turnFaultLatched_=true; stopTurn(true); }
 #include "actual-turning.inc"
};
int main() {
 Controller c;
 c.rtc_.now+=70*60; c.updateTurning(5000);
 assert(!c.needHome_ && c.lastTurnEpoch_==1000000 && c.nextTurnAt_==5000+50*60000);
 // Reboot/WDT/outage resumes the same epoch, irrespective of new millis origin.
 for (uint32_t uptime : {uint32_t(20),uint32_t(5000)}) {
  Controller reboot; reboot.rtc_.now+=75*60; reboot.updateTurning(uptime);
  assert(reboot.lastTurnEpoch_==1000000 && reboot.nextTurnAt_==uptime+45*60000);
 }
 Controller overdue; overdue.rtc_.now+=121*60; overdue.updateTurning(5000); overdue.updateTurning(5001);
 assert(overdue.turnPhase_==TurnPhase::DeadtimeRight);
 // Mid-travel power loss requires homing, but preserves the schedule anchor.
 Controller middle; middle.inputs_.value.limitLeft=false; middle.rtc_.now+=70*60;
 middle.updateTurning(5000); middle.updateTurning(5100);
 assert(middle.moveIsHoming_ && middle.turnPhase_==TurnPhase::MovingLeft);
 middle.inputs_.value.limitLeft=true; middle.updateTurning(5200);
 assert(!middle.needHome_ && middle.lastTurnEpoch_==1000000 && middle.nextTurnAt_==5200+50*60000);
 // No physical turn during Manual -> Auto: no reanchor.
 c.inputs_.value.autoMode=false; c.processInputModeTransition(6000);
 c.inputs_.value.autoMode=true; c.processInputModeTransition(7000);
 assert(c.lastTurnEpoch_==1000000 && c.nextTurnAt_==7000+50*60000);
 c.config_.turnIntervalMin=180; c.scheduleNextTurnFromAnchor(8000);
 assert(c.nextTurnAt_==8000+110*60000);
 // Manual completion persists iff explicitly configured; never increments auto count.
 c.moveCounts_=false; c.completeTurn(9000,TrayPosition::Right);
 assert(c.lastTurnEpoch_==1000000 && c.saves==0);
 c.config_.manualTurnReanchorsSchedule=true; c.completeTurn(10000,TrayPosition::Left);
 assert(c.lastTurnEpoch_==c.rtc_.now && c.saves==1 && c.turnCountBatch_==0);
 // Invalid RTC: waiting for a valid persisted epoch must NOT imply due now.
 Controller badRtc; badRtc.rtc_.ok=false; badRtc.updateTurning(5000); badRtc.updateTurning(5001);
 assert(badRtc.nextTurnAt_==0 && badRtc.turnPhase_==TurnPhase::Idle);
 badRtc.rtc_.ok=true; badRtc.rtc_.now+=70*60; badRtc.updateTurning(6000);
 assert(badRtc.nextTurnAt_==6000+50*60000 && badRtc.turnPhase_==TurnPhase::Idle);
 // RTC never recovers: bounded one-interval wait, no busy-loop turn storm.
 Controller lost; lost.rtc_.ok=false; lost.updateTurning(5000); lost.updateTurning(5001);
 lost.updateTurning(5000+120*60000);
 assert(lost.turnPhase_==TurnPhase::DeadtimeRight);
 // Safety conditions still outrank an overdue deadline.
 for (unsigned fault=0;fault<5;++fault) {
  Controller blocked; blocked.rtc_.now+=121*60;
  if(fault==0) blocked.faults_.inhibit=true;
  if(fault==1) blocked.sensorUsable_=false;
  if(fault==2) blocked.highTemperatureActive_=true;
  if(fault==3) blocked.emergencyActive_=true;
  if(fault==4) blocked.batchElapsed=18*86400;
  blocked.updateTurning(5000); blocked.updateTurning(5001);
  assert(blocked.turnPhase_==TurnPhase::Idle);
 }
 // Signed deadline arithmetic across millis wrap; epoch is unaffected.
 Controller wrap; wrap.rtc_.now+=119*60; wrap.updateTurning(0xFFFF8000U);
 const uint32_t due=uint32_t(0xFFFF8000U+60000U);
 wrap.updateTurning(due-1); assert(wrap.turnPhase_==TurnPhase::Idle);
 wrap.updateTurning(due); assert(wrap.turnPhase_==TurnPhase::DeadtimeRight);
 // Shorter interval can become overdue immediately, still uses original anchor.
 Controller shorter; shorter.rtc_.now+=70*60; shorter.updateTurning(5000);
 shorter.config_.turnIntervalMin=60; shorter.scheduleNextTurnFromAnchor(6000);
 shorter.updateTurning(6000); assert(shorter.turnPhase_==TurnPhase::DeadtimeRight);
 // Manual -> Auto in mid-travel homes first without erasing overdue state.
 Controller mode; mode.rtc_.now+=121*60; mode.inputs_.value.limitLeft=false;
 mode.inputs_.value.autoMode=false; mode.processInputModeTransition(5000);
 mode.inputs_.value.autoMode=true; mode.processInputModeTransition(6000);
 mode.updateTurning(6000); mode.updateTurning(6100);
 mode.inputs_.value.limitLeft=true; mode.updateTurning(6200); mode.updateTurning(6201);
 assert(mode.lastTurnEpoch_==1000000 && mode.turnPhase_==TurnPhase::DeadtimeRight);
 std::puts("Production turning scheduler regression PASS");
}
