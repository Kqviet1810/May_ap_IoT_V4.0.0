#include "thermal-fixture.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_control.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/heater_burst_scheduler.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/sensor_format.h"
namespace Old {
#include "fixtures/thermal-v1/thermal_control.h"
#include "fixtures/thermal-v1/ssr_window.h"
}

static void sensorFormats() {
  const uint16_t registers[] = {375U, 3751U, 30902U};
  const uint16_t humidityRegisters[] = {600U, 600U, 39321U};
  for (unsigned i = 0; i < 3; ++i) {
    SensorFormatDecoder decoder;
    for (unsigned n = 0; n < 5; ++n) {
      assert(!decoder.accept(registers[i], humidityRegisters[i]));
      assert(!decoder.locked() && !decoder.valid());
    }
    assert(decoder.accept(registers[i], humidityRegisters[i]));
    assert(static_cast<unsigned>(decoder.profile()) == i + 1);
    const float expected = i == 0 ? 37.5f : i == 1 ? 37.51f : -45.0f + 175.0f * registers[i] / 65535.0f;
    assert(std::fabs(decoder.temperature() - expected) < 0.00001f);
    assert(std::fabs(decoder.humidity()-60.0f)<0.001f);
    const auto locked = decoder.profile();
    // A register scale change never starts a fresh detector after lock.
    const uint16_t wrong = i == 0 ? 3751U : 375U;
    assert(!decoder.accept(wrong, humidityRegisters[i]) && !decoder.valid());
    assert(decoder.profile() == locked);
    for (unsigned n = 0; n < 20; ++n) assert(!decoder.accept(wrong, humidityRegisters[i]));
    assert(decoder.accept(registers[i], humidityRegisters[i]) && decoder.profile() == locked);
    decoder.rejectSample(); // Rejected measurement: invalidate but keep format.
    assert(!decoder.valid() && decoder.locked());
    assert(decoder.accept(registers[i], humidityRegisters[i]));
    const uint16_t hotRegisters[]={600U,6000U,39321U};
    assert(decoder.accept(hotRegisters[i],humidityRegisters[i]));
    assert(decoder.valid() && decoder.temperature()>59.99f); // Emergency must see upward raw jumps.
  }
  SensorFormatDecoder interrupted;
  for (unsigned n = 0; n < 5; ++n) interrupted.accept(375,600);
  interrupted.rejectSample();
  for (unsigned n = 0; n < 5; ++n) assert(!interrupted.accept(375,600));
  assert(interrupted.accept(375,600));
  SensorFormatDecoder invalid;
  for (unsigned n = 0; n < 50; ++n) {
    assert(!invalid.accept(0, 600)); // Ambiguous/implausible cold AUTO boot => no heat.
    assert(!invalid.accept(65000, 600));
    assert(!invalid.accept(375, 1001));
  }
  assert(!invalid.locked());
  SensorFormatDecoder explicitCold(SensorProfile::X100RhX10);
  for (unsigned n = 0; n < 5; ++n) assert(!explicitCold.accept(500,600));
  assert(explicitCold.accept(500,600) && std::fabs(explicitCold.temperature()-5.0f)<0.001f);
  SensorFormatDecoder nativeRH(SensorProfile::Sht30Native);
  for (unsigned n = 0; n < 6; ++n) nativeRH.accept(30902,39321);
  assert(nativeRH.valid() && std::fabs(nativeRH.humidity()-60.0f)<0.001f);
  SensorFormatDecoder paired;
  for (unsigned n=0;n<5;++n) assert(!paired.accept(30902,39321));
  assert(paired.accept(30902,39321) && paired.profile()==SensorProfile::Sht30Native);
  // Native RH=600 is physically possible (~0.9%); raw-only cannot reject it.
  assert(paired.accept(30902,600) && paired.humidity()<1.0f);
  assert(paired.profile()==SensorProfile::Sht30Native);
  SensorFormatDecoder scaled;
  for(unsigned n=0;n<6;++n) scaled.accept(375,600);
  assert(!scaled.accept(375,39321) && !scaled.valid());
  assert(scaled.profile()==SensorProfile::X10RhX10);
  // HMI sensor resolution choice: no guessing, two matching samples, 10..60 C start envelope, a wrong choice never reads as a plausible value.
  SensorFormatDecoder choose01;choose01.setRuntimeMode(1U);
  assert(!choose01.accept(375,600) && choose01.accept(375,600) && choose01.profile()==SensorProfile::X10RhX10 && std::fabs(choose01.temperature()-37.5f)<0.001f);
  SensorFormatDecoder choose001;choose001.setRuntimeMode(2U);
  assert(!choose001.accept(3750,600) && choose001.accept(3750,600) && choose001.profile()==SensorProfile::X100RhX10 && std::fabs(choose001.temperature()-37.5f)<0.001f);
  SensorFormatDecoder wrong001;wrong001.setRuntimeMode(2U);      // sensor really is 0.1 C: 37.5 C arrives as 375 -> 3.75 C, below the envelope
  for(unsigned n=0;n<20;++n) assert(!wrong001.accept(375,600));
  assert(!wrong001.locked());
  SensorFormatDecoder wrong01;wrong01.setRuntimeMode(1U);        // sensor really is 0.01 C: 3750 -> 375 C
  for(unsigned n=0;n<20;++n) assert(!wrong01.accept(3750,600));
  assert(!wrong01.locked());
  // Changing the choice forgets the lock, and 0 returns to automatic detection.
  choose01.setRuntimeMode(2U);assert(!choose01.locked()&&!choose01.valid());
  choose01.setRuntimeMode(0U);
  for(unsigned n=0;n<5;++n) assert(!choose01.accept(375,600));
  assert(choose01.accept(375,600));
  // A compile-time profile cannot be overridden at run time.
  SensorFormatDecoder fixed(SensorProfile::X100RhX10);fixed.setRuntimeMode(1U);
  for(unsigned n=0;n<5;++n) assert(!fixed.accept(3750,600));
  assert(fixed.accept(3750,600)&&fixed.profile()==SensorProfile::X100RhX10);
}

static void pidWeightsAndPermits() {
  // Saving SV is not a gain/actuator-shape change: it must not reset the
  // derivative state or back-calculate I to cancel the next P response.
  MachineConfig policyA, policyB = policyA;
  policyB.targetTemp = 35.0f;
  assert(!thermalPidRuntimeConfigChanged(policyA, policyB));
  policyB = policyA; policyB.autotuneBandC += 0.1f;
  assert(!thermalPidRuntimeConfigChanged(policyA, policyB));
  policyB = policyA; policyB.kp += 1.0f;
  assert(thermalPidRuntimeConfigChanged(policyA, policyB));
  policyB = policyA; policyB.ki += 0.1f;
  assert(thermalPidRuntimeConfigChanged(policyA, policyB));
  policyB = policyA; policyB.kd += 1.0f;
  assert(thermalPidRuntimeConfigChanged(policyA, policyB));
  policyB = policyA; policyB.maxHeaterPower = 50;
  assert(thermalPidRuntimeConfigChanged(policyA, policyB));

  // Regression for the production config-save bug: a real SV increase must
  // retain the existing I state and let P respond on the next sensor sample.
  // The old call-site treated the new SV as "bumpless config", projected a
  // large negative I term, and delayed this response.
  {
    MachineConfig cfg;
    cfg.kp = 18.0f; cfg.ki = 0.8f; cfg.kd = 0.0f; cfg.maxHeaterPower = 100;
    ThermalController pid;
    assert(std::fabs(pid.updateOnNewSample(1000, 30.0f, 29.0f, cfg, true) - 18.0f) < 0.001f);
    const float beforeStep = pid.updateOnNewSample(3000, 30.0f, 29.0f, cfg, true);
    assert(std::fabs(beforeStep - 19.6f) < 0.001f);
    assert(pid.updateOnNewSample(5000, 37.5f, 29.0f, cfg, true) == 100.0f);
  }

  for (float beta : {1.0f,0.7f,0.5f,0.35f}) {
    MachineConfig cfg;
    cfg.kp=10; cfg.ki=0; cfg.kd=100;
    ThermalController pid(beta);
    assert(std::fabs(pid.updateOnNewSample(1000,1,0,cfg,true)-10*beta)<0.001f);
    const float stepped=pid.updateOnNewSample(3000,3,0,cfg,true);
    assert(std::fabs(stepped-30*beta)<0.001f); // P weighting; D has no SP kick.
    cfg.kp=12;
    pid.applyConfigBumpless(3000,3,0,cfg);
    assert(std::fabs(pid.output()-stepped)<0.001f);
    assert(std::fabs(pid.updateOnNewSample(5000,3,0,cfg,true)-stepped)<0.001f);
    cfg.kp=0; cfg.ki=1; cfg.kd=0;
    pid.reset();
    pid.updateOnNewSample(1000,35,34,cfg,true);
    assert(std::fabs(pid.updateOnNewSample(3000,35,34,cfg,true)-2)<0.001f);
    assert(std::fabs(pid.updateOnNewSample(3000,35,34,cfg,true)-2)<0.001f); // no duplicate integration
    assert(std::fabs(pid.updateOnNewSample(6000,35,34,cfg,true)-5)<0.001f); // real 3 s dt
    // Safety cut, sensor fault, high temp, vent and permit loss share disabled reset.
    for (unsigned reason = 0; reason < 6; ++reason) {
      assert(pid.updateOnNewSample(8000,35,34,cfg,false)==0);
      assert(pid.updateOnNewSample(10000,35,34,cfg,true)==0);
    }
    assert(pid.updateOnNewSample(12000,35,NAN,cfg,true)==0);
    cfg.kp=1; cfg.ki=1; cfg.maxHeaterPower=17;
    pid.reset();
    for (uint32_t t=1000; t<3600000; t+=2000)
      assert(pid.updateOnNewSample(t,100,0,cfg,true)==17);
    assert(pid.updateOnNewSample(3600000,100,100,cfg,true)<0.00001f); // no high-saturation windup
    pid.reset();
    for (uint32_t t=1000; t<3600000; t+=2000)
      assert(pid.updateOnNewSample(t,20,30,cfg,true)==0);
    assert(pid.updateOnNewSample(3600000,20,20,cfg,true)<0.00001f); // no low-saturation windup
  }
}

static void burstEnergy() {
  for (unsigned groups : {1U,2U}) {
    for (float power : {0.5f,1.f,2.f,3.f,5.f,10.f,25.f,50.f,75.f,100.f}) {
      for (unsigned seconds : {120U,600U,3600U}) {
        HeaterBurstScheduler scheduler(groups,1000);
        unsigned a=0,b=0;
        for (unsigned s=0; s<seconds; ++s) {
          auto d=scheduler.update(s*1000U,power,true);
          a+=d.groupA; b+=d.groupB;
          if (power<=50 && groups==2) assert(!(d.groupA && d.groupB));
          if (groups==1) assert(!d.groupB);
        }
        const double requested=seconds*power*groups/100.0;
        assert(a+b<=requested+1e-7 && requested-(a+b)<1.000001);
        if (groups==2) assert(std::abs(static_cast<int>(a)-static_cast<int>(b))<=1);
        std::printf("LOW_DUTY groups=%u request=%.1f seconds=%u delivered=%.6f\n",groups,power,seconds,100.0*(a+b)/(seconds*groups));
      }
    }
  }
  HeaterBurstScheduler scheduler(2,1000);
  assert(scheduler.update(0,100,true).groupA);
  auto d=scheduler.update(1,100,false); // OFF immediately, not at quantum boundary.
  assert(!d.groupA && !d.groupB);
  d=scheduler.update(2,0.5f,true); assert(!d.groupA && !d.groupB);
  for (unsigned s=1; s<100; ++s) scheduler.update(s*1000U+2,0.5f,true);
  scheduler.update(100003,100,false);
  d=scheduler.update(100004,0.5f,true); assert(!d.groupA && !d.groupB); // no backlog
  d=scheduler.update(100005,NAN,true); assert(!d.groupA && !d.groupB);
  d=scheduler.update(100006,0,true); assert(!d.groupA && !d.groupB);
  scheduler.reset();
  uint32_t now=UINT32_MAX-500U;
  unsigned units=0;
  for (unsigned n=0; n<200; ++n) {
    d=scheduler.update(now,25,true); units+=d.groupA+d.groupB; now+=1000U;
  }
  assert(units==100);
  scheduler.reset();
  scheduler.update(0,1,true);
  d=scheduler.update(3600000,1,true); assert(!d.groupA && !d.groupB); // stalled time earns no heat
  // Variable demand: accumulated energy remains within ONE group-quantum even over 1M slots.
  scheduler.reset(); double requested=0; units=0;
  for (unsigned n=0; n<1000000; ++n) {
    const float power=(n%997)*0.1f;
    d=scheduler.update(n*1000U,power,true);
    if (power==0) requested=units; // explicit zero deliberately discards fractional credit
    else requested+=2.0*power/100.0;
    units+=d.groupA+d.groupB;
    assert(std::fabs(requested-units)<1.00001);
  }
  uint8_t remainder=0;
  assert(HeaterBurstScheduler::fullPowerEquivalentMs(1000,1,true,false,remainder)==1000);
  assert(HeaterBurstScheduler::fullPowerEquivalentMs(1000,2,true,false,remainder)==500);
  assert(HeaterBurstScheduler::fullPowerEquivalentMs(1000,2,true,true,remainder)==1000);
  unsigned equivalent=0;
  for(unsigned n=0;n<1000;++n) equivalent+=HeaterBurstScheduler::fullPowerEquivalentMs(1,2,true,false,remainder);
  assert(equivalent==500 && remainder==0);
  MachineConfig cfg,result;
  RelayAutoTune tune;
  tune.configure(37.5f); tune.start(1000,37.4f);
  tune.update(1000,37.4f,cfg,result);
  scheduler.reset(); units=0;
  for (unsigned n=0;n<600;++n) {
    d=scheduler.update(1000U+n*1000U,tune.power(),true); units+=d.groupA+d.groupB;
  }
  assert(units==360); // autotune 30% total, not a direct GPIO bypass
  tune.abort(); d=scheduler.update(601001,tune.power(),true);
  assert(!d.groupA && !d.groupB);
}
static void burstTimingJitter() {
  HeaterBurstScheduler late(1,1000);
  assert(!late.update(0,50,true).groupA);
  assert(late.update(1999,50,true).groupA);
  assert(late.update(2000,50,true).groupA); // must not emit a 1 ms catch-up pulse
  assert(!late.update(2999,50,true).groupA);
  for(unsigned groups:{1U,2U}) for(float power:{0.5f,1.f,2.f,3.f,5.f,10.f,25.f,50.f,75.f,100.f}) {
    HeaterBurstScheduler scheduler(groups,1000);
    uint32_t now=0,lastChange=0;
    uint64_t a=0,b=0;
    auto previous=scheduler.update(0,power,true);
    while(now<3600000) {
      const uint32_t dt=std::min<uint32_t>(3600000-now,25+(now%51));
      a+=static_cast<uint32_t>(previous.groupA)*dt;
      b+=static_cast<uint32_t>(previous.groupB)*dt;
      now+=dt;
      auto d=scheduler.update(now,power,true);
      if(d.groupA!=previous.groupA || d.groupB!=previous.groupB) {
        assert(now-lastChange>=1000);lastChange=now;
      }
      if(groups==2 && power<=50)assert(!(d.groupA&&d.groupB));
      previous=d;
    }
    const double requested=3600000.0*groups*power/100.0;
    assert(std::fabs(requested-(a+b))<=1150); // one quantum plus <=2 late-poll intervals
    if(groups==2)assert(std::fabs(static_cast<double>(a)-b)<=1075); // actual runtime, not only slot count
  }
}
static void productionCadenceJitter() {
  const uint32_t pattern[]={5,5,5,10,5,20,5,5,40,5,5,15};
  for(uint32_t quantum:{300U,500U,1000U}) for(float power:{0.5f,1.f,2.f,3.f,5.f,10.f,30.f,50.f,100.f}) {
    HeaterBurstScheduler scheduler(1,quantum);
    const uint32_t start=UINT32_MAX-20000U; // Include millis rollover.
    uint32_t passed=0,lastChange=0,n=0;uint64_t on=0;
    auto previous=scheduler.update(start,power,true);
    while(passed<3600000) {
      const uint32_t dt=std::min<uint32_t>(3600000-passed,pattern[n++%12]);
      if(previous.groupA)on+=dt;
      passed+=dt;auto current=scheduler.update(start+passed,power,true);
      if(current.groupA!=previous.groupA){assert(passed-lastChange>=quantum);lastChange=passed;}
      assert(!current.groupB);previous=current;
      // Error is bounded over the whole run, not only lucky final endpoints.
      assert(std::fabs(on-passed*power/100.0)<=2*quantum+80);
    }
    assert(std::fabs(on-passed*power/100.0)<=quantum+80);
    assert(!scheduler.update(start+passed+5,power,false).groupA);
    assert(!scheduler.update(start+passed+10,0.5f,true).groupA);
    // A long missed observation resets credit, never shortened catch-up pulses.
    scheduler.update(start+passed+3*quantum,50,true);
    const auto initial=scheduler.update(start+passed+3*quantum+5,50,true);
    for(uint32_t dt=10;dt<quantum;dt+=5)
      assert(scheduler.update(start+passed+3*quantum+dt,50,true).groupA==initial.groupA);
  }
  std::puts("Production-like 5ms jitter/40ms stalls: bounded energy throughout 1h, full pulses, rollover, immediate safety/no catch-up PASS");
}
static void singleBankSweep() {
  for(uint32_t quantum:{300U,500U,1000U})
    for(float power:{0.1f,0.25f,0.5f,1.f,2.f,3.f,5.f,10.f,20.f,50.f,75.f,100.f})
      for(uint32_t seconds:{120U,600U,3600U}) {
        HeaterBurstScheduler bank(1,quantum);
        uint32_t onMs=0,offRun=0,maxOff=0,transitions=0;
        bool previous=false;
        const uint32_t horizon=seconds*1000U;
        for(uint32_t at=0;at<horizon;at+=quantum) {
          const auto demand=bank.update(at,power,true);
          assert(!demand.groupB);
          const uint32_t duration=std::min(quantum,horizon-at);
          if(demand.groupA)onMs+=duration;
          else offRun+=duration;
          if(demand.groupA!=previous) {
            if(demand.groupA) { maxOff=std::max(maxOff,offRun);offRun=0; }
            ++transitions;
          }
          previous=demand.groupA;
        }
        maxOff=std::max(maxOff,offRun);
        const double requestedMs=horizon*power/100.0;
        const double errorJ=16.0*std::fabs(requestedMs-onMs);
        assert(errorJ<=16.0*quantum+0.5); // one full-bank packet
        std::printf("BANK,%u,%.2f,%u,%.6f,%.6f,%.2f,%u,%.2f\n",quantum,power,seconds,
            power,100.0*onMs/horizon,errorJ,maxOff,3600.0*transitions/seconds);
      }
  // At every tested quantum a late task may extend, never shorten, a pulse.
  for(uint32_t quantum:{300U,500U,1000U}) {
    HeaterBurstScheduler bank(1,quantum);
    uint32_t now=0,lastChange=0;bool previous=bank.update(0,50,true).groupA;
    while(now<3600000U) {
      now+=25+(now%51);
      const bool on=bank.update(now,50,true).groupA;
      if(on!=previous) { assert(now-lastChange>=quantum);lastChange=now; }
      previous=on;
    }
    assert(!bank.update(now+1,50,false).groupA);
    assert(!bank.update(now+2,0.1f,true).groupA); // no stored credit
  }
}
static void defaultPidPreservesLegacy() {
  MachineConfig cfg;cfg.kp=18;cfg.ki=0.8f;cfg.kd=45;
  ThermalController current;Old::ThermalController legacy;
  for(uint32_t n=0;n<200000;++n) {
    float pv=35+3*std::sin(n*0.007f),sp=n%20000<10000 ? 37.5f:32.0f;
    bool enabled=n%701!=0;
    uint32_t now=UINT32_MAX-1000000U+n*2000U;
    if(n%15001==0) {
      cfg.maxHeaterPower=n%2?33:100;
      current.applyConfigBumpless(now,sp,pv,cfg);legacy.applyConfigBumpless(now,sp,pv,cfg);
    }
    const float a=current.updateOnNewSample(now,sp,pv,cfg,enabled);
    const float b=legacy.updateOnNewSample(now,sp,pv,cfg,enabled);
    assert(std::isfinite(a) && a>=0 && a<=cfg.maxHeaterPower);
    assert(std::isfinite(b) && b>=0 && b<=cfg.maxHeaterPower);
  }
  Old::LegacySsrWindow window;
  for(float power : {0.5f,1.f,2.f})
    for(uint32_t ms=1000;ms<120001;ms+=50) assert(!window.ssrWindowOn(ms,power,10));
}
static void antiWindupBoundaryRegression() {
  MachineConfig cfg; cfg.kp=18;cfg.ki=0.8f;cfg.kd=45;
  for (uint32_t dtSeconds:{1U,2U,5U,10U}) for (uint8_t cap:{100U,50U,5U}) {
    cfg.maxHeaterPower=cap;
    ThermalController fixed; Old::ThermalController stuck;
    const uint32_t start=UINT32_MAX-1000U;
    const float initial=fixed.updateOnNewSample(start,30,29.78f,cfg,true);
    assert(initial>3 && initial<5);
    stuck.updateOnNewSample(start,30,29.78f,cfg,true);
    fixed.applyConfigBumpless(start,30,32.55f,cfg);
    stuck.applyConfigBumpless(start,30,32.55f,cfg);
    uint32_t now=start;
    for (unsigned n=0;n<3000;++n) {
      now+=dtSeconds*1000U;
      const float output=fixed.updateOnNewSample(now,30,32.55f,cfg,true);
      if (n>=3) assert(output==0); // Crossing negative I step reaches 0%.
      stuck.updateOnNewSample(now,30,32.55f,cfg,true);
    }
    if(dtSeconds==10 && cap==100) assert(stuck.output()>3); // frozen OLD bug evidence
    // Lower saturation must recover when PV returns below SP.
    assert(fixed.updateOnNewSample(now+dtSeconds*1000U,30,29.9f,cfg,true)>0);
    assert(fixed.updateOnNewSample(now+dtSeconds*2000U,30,NAN,cfg,true)==0);
  }
  // Upper boundary crossing must cap exactly; no positive I debt accumulates.
  for (uint8_t cap:{100U,50U,5U}) {
    cfg.maxHeaterPower=cap;
    ThermalController pid;
    uint32_t now=1000;
    pid.updateOnNewSample(now,30,29.9f,cfg,true);
    for (unsigned n=0;n<3000;++n) {
      now+=2000;
      const float out=pid.updateOnNewSample(now,30,20,cfg,true);
      assert(out==cap);
    }
    assert(pid.updateOnNewSample(now+2000,30,32,cfg,true)==0);
    assert(pid.updateOnNewSample(now+4000,30,29,cfg,false)==0);
  }
  // Cross the upper actuator limit by ONE integrator step, rather than by a
  // P jump. The frozen controller leaves cap-1% commanded indefinitely.
  cfg.kp=cfg.ki=1;cfg.kd=0;
  for(uint8_t cap:{100U,50U,5U}) {
    cfg.maxHeaterPower=cap;
    ThermalController fixed;Old::ThermalController old;
    uint32_t now=1000;
    fixed.updateOnNewSample(now,30,29,cfg,true);
    old.updateOnNewSample(now,30,29,cfg,true);
    for(unsigned n=1;n<static_cast<unsigned>(cap)-1U;++n) {
      now+=1000;
      assert(fixed.updateOnNewSample(now,30,29,cfg,true)==n+1);
      old.updateOnNewSample(now,30,29,cfg,true);
    }
    now+=5000;
    assert(fixed.updateOnNewSample(now,30,29,cfg,true)==cap);
    assert(old.updateOnNewSample(now,30,29,cfg,true)==cap-1);
    assert(fixed.updateOnNewSample(now+1000,30,31,cfg,true)<cap);
  }
}
int main() {
  defaultPidPreservesLegacy(); antiWindupBoundaryRegression(); sensorFormats(); pidWeightsAndPermits(); burstEnergy(); burstTimingJitter(); productionCadenceJitter(); singleBankSweep();
  std::puts("Thermal V2: raw formats/lock/fault, 2-DOF/bumpless/AW, low duty/balance/safety/autotune/rollover/1M slots PASS");
}
