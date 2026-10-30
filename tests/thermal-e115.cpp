// Execute the actual E115 full-bank energy accounting body from MachineController (the evidence rule itself is heater_response_watch.h).
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
struct E115Harness {
  struct { float targetTemp=37.5f,tempHysteresis=0.2f,heaterStuckMinRiseC=0.2f;
           uint16_t heaterStuckDurationSec=900; } config_;
  struct Input { bool heaterEnable=true; };
  struct Inputs { Input input;
           const Input &state() const { return input; } } inputs_;
  OutputArbiter outputs_;
#include "actual-burst-member.inc"
  bool batchRunning_=true,sensorUsable_=true;
  bool heaterNotHeatingActive_=false;
  float temperature_=30;
  HeaterResponseWatch heaterWatch_;
  uint32_t onMs() const { return heaterWatch_.accumOnMs(); }
  void update(uint32_t now) {
#include "actual-heater-evidence.inc"
  }
};
static void actuate(E115Harness &h,uint32_t now,float percent,bool permit=true) {
  clockMs=now;
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
    while(!h.heaterNotHeatingActive_ && last<20000000) {
      const uint32_t now=last+5;
      if(levels[PIN_OUT_HEATER_SSR]==HIGH)actualOn+=5;
      // Production order: updateAlarms accounts prior GPIO state, then heating.
      h.update(now);
      assert(h.onMs()==actualOn);
      assert(h.heaterNotHeatingActive_==(actualOn>=900000));
      actuate(h,now,power);last=now;
    }
    assert(h.heaterNotHeatingActive_ && actualOn==900000);
    std::printf("E115 cadence=5ms quantum=300ms request=%.0f%% actual_on=%u elapsed=%u PASS\n",power,actualOn,last-10000);
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
  std::puts("Actual E115: 5ms/300ms, 5/10/30/50/100% -> 900s actual GPIO ON; midburst cut140ms/no credit/resume PASS");
}
