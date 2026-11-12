// Execute the actual E115 body from MachineController: the full-bank on-time accounting of the evidence counter (door warning, frozen sensor, push of the hold) and, since 3.0.1, the
// rule that raises E115 (HeaterFallWatch: two hours at the ceiling and a fall of 1 degC). Both rules themselves are in heater_response_watch.h.
#include "thermal-fixture.h"
#include <deque>
#include <cstddef>
#include "../MAYAP_INDUSTRIAL_v1_0_0/heater_burst_scheduler.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/heater_response_watch.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/startup_output_policy.h"
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

constexpr uint8_t HEATER_GROUP_COUNT=1;
// Stand-ins for what the evidence block reads besides the watch: the controller's door inference and the fault table (code 116).
enum class FaultCode { DoorOpenSuspected };
struct DoorStub { bool door=false; bool doorDetected() const { return door; } };
struct V3Stub { DoorStub c; const DoorStub &controller() const { return c; } };
struct FaultStub { bool doorWarning=false;
  void set(FaultCode, bool on, uint32_t, int16_t = 0) { doorWarning = on; } };
struct E115Harness {
  V3Stub thermalV3_;
  FaultStub faults_;
  bool doorSuspected_=false;
  struct { float targetTemp=37.5f,tempHysteresis=0.2f,heaterStuckMinRiseC=0.2f;
           uint16_t heaterStuckDurationSec=900; uint8_t maxHeaterPower=100; } config_;
  struct { float heaterPower=0; } runtime_;
  struct Input { bool heaterEnable=true; };
  struct Inputs { Input input;
           const Input &state() const { return input; } } inputs_;
  OutputArbiter outputs_;
#include "actual-burst-member.inc"
  bool batchRunning_=true,sensorUsable_=true;
  bool heaterNotHeatingActive_=false;
  float temperature_=30;
  HeaterResponseWatch heaterWatch_;
  HeaterFallWatch heaterFallWatch_;
  uint32_t onMs() const { return heaterWatch_.accumOnMs(); }
  void update(uint32_t now) {
#include "actual-heater-evidence.inc"
  }
};
static void actuate(E115Harness &h,uint32_t now,float percent,bool permit=true) {
  clockMs=now;
  h.runtime_.heaterPower=percent;
  OutputRequest request{};request.heatMaster=true;request.circulationFan=true;
  request.heaterSsr=h.heaterBurst_.update(now,percent,permit).groupA;
  h.outputs_.update(now,request);
  assert(h.outputs_.state().heaterSsr==(levels[PIN_OUT_HEATER_SSR]==HIGH));
}
static void prepare(E115Harness &h) {
  h.outputs_.begin();actuate(h,1000,0);actuate(h,10000,0);h.update(10000);
}
int main() {
  for(float power:{5.f,10.f,30.f,50.f,100.f}) {
    E115Harness h;prepare(h);uint32_t actualOn=0,last=10000;
    actuate(h,last,power);
    while(h.onMs()<900000 && last<20000000) {
      const uint32_t now=last+5;
      if(levels[PIN_OUT_HEATER_SSR]==HIGH)actualOn+=5;
      // Production order: updateAlarms accounts prior GPIO state, then heating.
      h.update(now);
      assert(h.onMs()==actualOn);
      assert(!h.heaterNotHeatingActive_);                      // E115 no longer comes from the on-time: a flat temperature at a low request is nothing for it
      actuate(h,now,power);last=now;
    }
    assert(actualOn==900000 && h.onMs()==900000);
    std::printf("on-time evidence cadence=5ms quantum=300ms request=%.0f%% actual_on=%u elapsed=%u PASS\n",power,actualOn,last-10000);
    h.sensorUsable_=false;h.update(last+5);
    assert(!h.heaterNotHeatingActive_ && h.onMs()==0);
  }
  E115Harness cut;prepare(cut);uint32_t at=10000;
  actuate(cut,at,30);
  while(!cut.outputs_.state().heaterSsr){at+=5;cut.update(at);actuate(cut,at,30);}
  const uint32_t before=cut.onMs();
  // Inhibit arrives after 135 ms ON; next 5 ms control cycle cuts at 140 ms.
  for(uint32_t dt=5;dt<=140;dt+=5){cut.update(at+dt);actuate(cut,at+dt,30,dt<140);}
  assert(!cut.outputs_.state().heaterSsr && cut.onMs()==before+140);
  const uint32_t evidence=cut.onMs();
  for(uint32_t dt=145;dt<=300;dt+=5){cut.update(at+dt);actuate(cut,at+dt,30,false);}
  assert(cut.onMs()==evidence);
  // Fresh 30% earns no compensation burst at resume or before 600 ms.
  for(uint32_t dt=305;dt<905;dt+=5){cut.update(at+dt);actuate(cut,at+dt,30);assert(!cut.outputs_.state().heaterSsr);}
  assert(cut.onMs()==evidence);
  // Door suspected (the controller's inference): code 116 "close the door" comes at a third of the E115 evidence, E115 itself waits one more window,
  // a recovery that shows a rise clears both, and a dead heater is still found within two windows.
  {
    E115Harness d;d.temperature_=34.0f;prepare(d);d.thermalV3_.c.door=true;
    uint32_t t=10000;int warnAt=-1,tripAt=-1;
    for(int sec=1;sec<=4000&&tripAt<0;++sec){
      for(int k=0;k<200;++k){t+=5;d.update(t);actuate(d,t,100);}   // 100 % command, 1 s per outer step
      if(warnAt<0&&d.faults_.doorWarning)warnAt=sec;
      if(d.heaterNotHeatingActive_)tripAt=sec;
    }
    assert(tripAt<0);                                                          // a flat temperature is not E115 any more (3.0.1): the heater would have to be falling at the ceiling for two hours
    // The scheduler's start-up ramp shifts the warning a little: it comes at a third of the evidence.
    assert(warnAt>=290&&warnAt<=520);
    std::printf("door suspected, flat temperature: warning 116 at %d s, no E115 PASS\n",warnAt);
    E115Harness r;r.temperature_=34.0f;prepare(r);r.thermalV3_.c.door=true;t=10000;bool warned=false;
    for(int sec=1;sec<=1200;++sec){
      if(sec==400)r.thermalV3_.c.door=false;                                  // the door is closed: the controller's inference ends
      if(sec>=400)r.temperature_=34.0f+0.002f*static_cast<float>(sec-400);   // and the oven climbs back
      for(int k=0;k<200;++k){t+=5;r.update(t);actuate(r,t,100);}
      if(r.faults_.doorWarning)warned=true;
      assert(!r.heaterNotHeatingActive_);
    }
    assert(warned&&!r.faults_.doorWarning&&!r.doorSuspected_);
    std::puts("door suspected, then recovery: warning goes up, clears on the rise, no E115 PASS");
  }
  // The rule that raises E115 through the actual body: the request at the ceiling (100 %), the temperature falling 10 degC an hour (a dead heater): E115 after two hours;
  // the same request over a flat or rising temperature: never.
  {
    E115Harness f;f.temperature_=36.0f;prepare(f);uint32_t t=10000;int tripAt=-1;
    for(int sec=1;sec<=4*3600&&tripAt<0;++sec){
      f.temperature_=36.0f-10.0f/3600.0f*static_cast<float>(sec);
      for(int k=0;k<200;++k){t+=5;f.update(t);actuate(f,t,100);}
      if(f.heaterNotHeatingActive_)tripAt=sec;
    }
    assert(tripAt>=7200&&tripAt<=7200+300);
    E115Harness g;g.temperature_=34.0f;prepare(g);t=10000;
    for(int sec=1;sec<=3*3600;++sec){
      g.temperature_=34.0f+0.0003f*static_cast<float>(sec);
      for(int k=0;k<200;++k){t+=5;g.update(t);actuate(g,t,100);}
      assert(!g.heaterNotHeatingActive_);
    }
    std::printf("E115 through the actual body: request 100 %% and a fall of 10 degC/h: at %d s; a creeping rise for 3 h: none PASS\n",tripAt);
  }
  std::puts("Actual E115: 5ms/300ms, 5/10/30/50/100% -> 900s actual GPIO ON counted; midburst cut140ms/no credit/resume; two hours at the ceiling and a fall -> E115 PASS");
}
