#define MAYAP_AUTOTUNE_LEGACY 1
#include "thermal-autotune-harness.h"
static void assertOld(const TuneHarness &h){assert(h.config_.kp==18 && h.config_.ki==0.8f && h.config_.kd==45);}
static void begin(TuneHarness &h,float pv=37.4f){
  h.sample(pv,pv);const char *message=nullptr;assert(h.startAutoTune(1000,message));h.cycle(1000);
}
static void oscillate(TuneHarness &h){
  for(uint32_t t=41000;t<=401000 && h.autotune_.running();t+=40000){
    const float pv=((t-41000)/40000)%2?36.9f:38.1f;h.sample(pv,pv);h.cycle(t);
  }
}
static void eventOutsideBatch(){
  TuneHarness h;assert(!h.eventLog_.loggingEnabled());serialEnabled=true;diagnosticLines.clear();
  const uint32_t before=h.eventLog_.sequence();
  h.eventLog_.push(0,EventType::Network,70);h.eventLog_.push(0,EventType::OutputChanged,200);
  assert(h.eventLog_.sequence()==before);begin(h);oscillate(h);
  assert(h.autotune_.state()==AutoTuneState::Success && h.store_.saves==1);
  HmiEventSnapshot events;h.eventLog_.snapshotRecent(clockMs,events);assert(events.count==2);
  assert(events.items[0].code==static_cast<uint16_t>(EventCode::AutoTuneSuccess));
  assert(events.items[1].code==static_cast<uint16_t>(EventCode::AutoTuneStarted));
  assert(diagnosticLines.size()<30); // Transitions/cycles/result only, no control-tick spam.
  diagnosticLines.clear();serialEnabled=false;h.cycle(clockMs+5);assert(diagnosticLines.empty());
  h.eventLog_.push(clockMs,EventType::Boot,1);assert(h.eventLog_.sequence()==before+3);
}
static void preheatAndTimeouts(){
  for(uint8_t cap:{20U,30U,50U,100U}){
    TuneHarness h;h.config_.maxHeaterPower=cap;begin(h,25);
    assert(h.autotune_.phase()==AutoTunePhase::Preheat && h.autotune_.power()==std::min<unsigned>(AUTOTUNE_PREHEAT_POWER_PERCENT,cap));
    assert(h.autotune_.cycleCount()==0 && h.autotune_.result().ku==0);
    h.sample(37.3f,37.3f);h.cycle(11000);
    assert(h.autotune_.phase()==AutoTunePhase::Heating && h.autotune_.power()==std::min<unsigned>(30,cap));
    assert(h.autotune_.preheatMs()==10000 && h.autotune_.cycleSerial()==0);
    oscillate(h);assert(h.autotune_.state()==AutoTuneState::Success);
    const auto &r=h.autotune_.result();
    assert(std::fabs(r.ku-4*(h.autotune_.relayHigh()-h.autotune_.relayLow())*0.5/(PI*r.amplitude))<0.001);
    assert(std::fabs(h.config_.kd/h.config_.kp-r.periodSec/6.3f)<0.0001f);
    assert(std::fabs(h.config_.ki-h.config_.kp/(2.2f*r.periodSec))<0.000001f);
    assert(r.gainScale>=1 && h.config_.kp>0 && h.config_.ki>0 && h.config_.kd>0);
  }
  TuneHarness cold;begin(cold,25);cold.cycle(1000+AUTOTUNE_PREHEAT_MAX_MS,false);
  assert(cold.autotune_.reason()==AutoTuneReason::PreheatTimeout && !cold.outputs_.state().heaterSsr);assertOld(cold);
  TuneHarness phase;begin(phase);phase.cycle(1000+AUTOTUNE_PHASE_MAX_MS,false);
  assert(phase.autotune_.reason()==AutoTuneReason::PhaseTimeout);assertOld(phase);
  TuneHarness total;begin(total);total.cycle(1000+AUTOTUNE_TOTAL_MAX_MS,false);
  assert(total.autotune_.reason()==AutoTuneReason::TotalTimeout);assertOld(total);
  // Genuine zero timestamp / millis wrap must not be mistaken for "no upper".
  MachineConfig cfg,result;RelayAutoTune wrap;wrap.configure(37.5f);wrap.start(0U-40000U,37.4f);
  wrap.update(0U-40000U,37.4f,cfg,result);
  for(uint32_t n=0;n<10 && wrap.running();++n)wrap.update(n*40000U,n%2?36.9f:38.1f,cfg,result);
  assert(wrap.state()==AutoTuneState::Success);
}
static void safetyAndReset(){
  for(bool preheat:{false,true})for(unsigned reason=0;reason<17;++reason){
    TuneHarness h;begin(h,preheat?25:37.4f);
    uint32_t at=1000;
    while(!h.outputs_.state().heaterSsr && at<30000){at+=5;h.cycle(at,false);}
    assert(h.outputs_.state().heaterSsr);at+=135;
    switch(reason){
      case 0:h.sensorUsable_=false;break;case 1:h.sample(NAN,NAN);break;
      case 2:h.highTemperatureActive_=true;break;case 3:h.emergencyActive_=true;break;
      case 4:h.inputs_.in.heaterEnable=false;break;case 5:h.inputs_.in.autoMode=false;break;
      case 6:h.storageFaultLatched_=true;break;case 7:h.storageDegraded_=true;break;
      case 8:h.faults_.drop=true;break;case 9:h.faults_.inhibit=true;break;
      case 10:maintenance=true;break;case 11:trip=true;break;case 12:bootReady=false;break;
      case 13:h.batchRunning_=true;break;case 14:h.safetyJournalFaultLatched_=true;break;
      case 15:h.batchClearPending_=true;break;case 16:h.rawTemperature_=h.config_.highTempAlarm;break;
    }
    h.cycle(at+5,false);assert(!h.outputs_.state().heaterSsr && h.runtime_.heaterPower==0);assertOld(h);
    assert(h.autotune_.state()==AutoTuneState::Failed && h.store_.saves==0);
    HmiEventSnapshot events;h.eventLog_.snapshotRecent(at+5,events);
    assert(events.count==2 && events.items[0].code==static_cast<uint16_t>(EventCode::AutoTuneFailed));
    trip=maintenance=false;bootReady=true;
    // Power/reset constructs fresh transient states: tune never resumes.
    TuneHarness reboot;std::memcpy(reboot.store_.bytes,h.store_.bytes,sizeof(h.store_.bytes));
    assert(reboot.store_.loadConfig(reboot.config_));assertOld(reboot);
    assert(reboot.autotune_.state()==AutoTuneState::Idle);
    assert(!reboot.outputs_.state().heaterSsr);
    assert(!reboot.heaterBurst_.update(at+10,0.5f,true).groupA);
  }
}
static void savesAndRejections(){
  for(int cut=0;cut<=static_cast<int>(sizeof(ConfigRecordV1));++cut){
    TuneHarness h;begin(h);h.store_.writeBudget=cut;oscillate(h);
    MachineConfig saved;assert(h.store_.loadConfig(saved));
    if(cut==static_cast<int>(sizeof(ConfigRecordV1))){
      assert(h.autotune_.state()==AutoTuneState::Success && saved.kp==h.config_.kp && saved.ki==h.config_.ki && saved.kd==h.config_.kd);
    } else {
      assert(h.autotune_.reason()==AutoTuneReason::SaveFailed && h.storageFaultLatched_);assertOld(h);
      assert(saved.kp==18 && saved.ki==0.8f && saved.kd==45);
    }
  }
  MachineConfig cfg,result;RelayAutoTune tune;cfg.autotuneBandC=0.05f;
  tune.configure(37.5f);tune.start(1000,37.5f);tune.update(1000,37.5f,cfg,result);
  for(unsigned n=0;n<12;++n)tune.update(41000+n*40000,n%2?37.44f:37.56f,cfg,result);
  assert(tune.rejection()==AutoTuneReason::AmplitudeTooSmall && tune.running());
  assert(result.kp==18);tune.checkTimeout(1000+AUTOTUNE_TOTAL_MAX_MS);assert(tune.reason()==AutoTuneReason::TotalTimeout);
  cfg.autotuneBandC=0.2f;tune.start(1000,37.4f);tune.update(1000,37.4f,cfg,result);
  for(unsigned n=0;n<12;++n)tune.update(2000+n*1000,n%2?36.9f:38.1f,cfg,result);
  assert(tune.rejection()==AutoTuneReason::PeriodTooSmall && tune.running());
  tune.start(1000,37.4f);tune.update(1000,37.4f,cfg,result);
  for(unsigned n=0;n<16;++n)tune.update(41000+n*40000,n%2?(n%4==1?35.5f:37.2f):38.1f,cfg,result);
  assert(tune.rejection()==AutoTuneReason::NonRepeatable && tune.running());
  tune.checkTimeout(1000+AUTOTUNE_TOTAL_MAX_MS);assert(tune.reason()==AutoTuneReason::TotalTimeout);
  cfg.autotuneRelayPowerPercent=0;tune.start(1000,37.4f);
  assert(!tune.update(1000,37.4f,cfg,result) && tune.reason()==AutoTuneReason::InvalidKu);
}
int main(){eventOutsideBatch();preheatAndTimeouts();safetyAndReset();savesAndRejections();
  std::puts("Actual AutoTune: preheat/capped actual swing, rolling quality, 34 immediate cuts, bounded deadlines, atomic save/power-cut/reset, outside-batch events and bounded diagnostics PASS");}
