#define MAYAP_AUTOTUNE_LEGACY 1
// Same uncalibrated LIGHT/MEDIUM/HEAVY plant as OLD/NEW control-only matrix.
// Tune loop enforces real AutoTune safety/abort and saves through real config I/O.
// Post-validation is CONTROL-ONLY / NO PRODUCTION SAFETY INTERVENTION; crossings
// mark a BAD candidate rather than pretending the firmware would keep heating.
#include "thermal-autotune-harness.h"
#include "actual-filter.inc"
#include <iostream>
#include <iomanip>
#include <fstream>
struct Plant{const char *name;double capacity,loss;};
struct Model {
  Plant plant;double ambient,temp,watts=0;std::vector<double> delay;size_t cursor=0;
  Model(Plant p,double a,unsigned dead):plant(p),ambient(a),temp(a),delay(dead*10,0){}
  void step(bool on){
    const double delivered=on?16000:0,delayed=delay[cursor];delay[cursor]=delivered;cursor=(cursor+1)%delay.size();
    watts+=(delayed-watts)*0.1/8;temp+=(watts-plant.loss*(temp-ambient))*0.1/plant.capacity;
    assert(std::isfinite(temp));
  }
};
struct Metrics{double mae=0,p95=0,ripple=0,overshoot=0;int settling=-1;bool high=false,emergency=false;};
static Metrics post(Plant plant,double ambient,unsigned dead,double resolution,const MachineConfig &cfg){
  Model model(plant,ambient,dead);ThermalController pid;HeaterBurstScheduler burst(1,HEATER_BURST_QUANTUM_MS);
  ActualSensorFilter filter;double power=0;std::vector<double> errors,tail;int lastOutside=0;Metrics m;
  for(unsigned tick=0;tick<108000;++tick){
    const uint32_t ms=1000+tick*100;const double t=tick*0.1;
    if(tick%20==0){
      filter.updateFilter(static_cast<float>(std::round(model.temp/resolution)*resolution),60);
      if(t>=12)power=pid.updateOnNewSample(ms,cfg.targetTemp,filter.value(),cfg,true);
    }
    assert(std::isfinite(power) && power>=0 && power<=cfg.maxHeaterPower);
    model.step(burst.update(ms,power,t>=12).groupA);
    const double e=model.temp-cfg.targetTemp;
    m.overshoot=std::max(m.overshoot,e);m.high=m.high || model.temp>=cfg.highTempAlarm;
    m.emergency=m.emergency || model.temp>=cfg.emergencyTemp;
    if(std::fabs(e)>0.15)lastOutside=static_cast<int>(t);
    if(t>=9000 && tick%10==0){errors.push_back(std::fabs(e));tail.push_back(model.temp);}
  }
  for(double e:errors){m.mae+=e;}
  m.mae/=errors.size();std::sort(errors.begin(),errors.end());
  m.p95=errors[static_cast<size_t>(0.95*(errors.size()-1))];
  m.ripple=*std::max_element(tail.begin(),tail.end())-*std::min_element(tail.begin(),tail.end());
  m.settling=lastOutside<9000?lastOutside+1:-1;return m;
}
static void run(Plant plant,double ambient,unsigned dead,double resolution,uint8_t relay,uint8_t preheat,double initial,std::ofstream &cycles){
  TuneHarness h(preheat);h.config_.autotuneRelayPowerPercent=relay;
  Model model(plant,ambient,dead);model.temp=initial;ActualSensorFilter filter;
  filter.updateFilter(static_cast<float>(initial),60);h.sample(initial,filter.value());
  const char *message=nullptr;assert(h.startAutoTune(1000,message));
  double peak=model.temp,peakAfterOff=-1;bool high=false,emergency=false;uint32_t serial=0,last=1000;
  for(unsigned tick=0;tick<=AUTOTUNE_TOTAL_MAX_MS/100+1;++tick){
    const uint32_t ms=1000+tick*100;
    if(tick%20==0){const float raw=static_cast<float>(std::round(model.temp/resolution)*resolution);
      filter.updateFilter(raw,60);h.sample(raw,filter.value());}
    h.cycle(ms,tick%20==0);last=ms;
    assert(!h.outputs_.state().heaterSsr || (h.autotune_.running() && h.sensorUsable_));
    if(h.autotune_.cycleSerial()!=serial){serial=h.autotune_.cycleSerial();const auto &c=h.autotune_.lastCycle();
      cycles<<plant.name<<','<<ambient<<','<<dead<<','<<resolution<<','<<unsigned(relay)<<','<<unsigned(preheat)<<','<<serial<<','
        <<c.heatMs*0.001<<','<<c.coolMs*0.001<<','<<c.high<<','<<c.low<<','<<c.amplitude<<','<<c.periodMs*0.001<<','
        <<(c.coolMs?double(c.heatMs)/c.coolMs:0)<<','<<autoTuneReasonName(h.autotune_.rejection())<<','<<(initial==ambient?"COLD":"NEAR_SP")<<','<<initial<<'\n';}
    model.step(h.outputs_.state().heaterSsr);peak=std::max(peak,model.temp);
    if(h.autotune_.phase()==AutoTunePhase::Cooling)peakAfterOff=std::max(peakAfterOff,model.temp);
    high=high || model.temp>=h.config_.highTempAlarm;emergency=emergency || model.temp>=h.config_.emergencyTemp;
    if(!h.autotune_.running())break;
  }
  const bool completedPreheat=h.autotune_.phase()!=AutoTunePhase::Preheat && h.autotune_.firstUpperMs()>0;
  const double preheatTime=h.autotune_.preheatMs()?h.autotune_.preheatMs()*0.001:completedPreheat?0:(last-1000)*0.001;
  const bool success=h.autotune_.state()==AutoTuneState::Success;
  assert(!h.outputs_.state().heaterSsr && h.runtime_.heaterPower==0);
  assert(last-1000<=AUTOTUNE_TOTAL_MAX_MS);
  // Existing delay/8s lag can continue heating AFTER OFF; report the whole coast.
  for(unsigned n=0;n<1200;++n){model.step(false);peak=std::max(peak,model.temp);
    peakAfterOff=std::max(peakAfterOff,model.temp);high=high || model.temp>=h.config_.highTempAlarm;
    emergency=emergency || model.temp>=h.config_.emergencyTemp;}
  MachineConfig stored;assert(h.store_.loadConfig(stored));
  if(!success){assert(h.config_.kp==18 && h.config_.ki==0.8f && h.config_.kd==45 && stored.kp==18 && h.store_.saves==0);}
  else assert(h.store_.saves==1 && stored.kp==h.config_.kp && stored.ki==h.config_.ki && stored.kd==h.config_.kd);
  const auto &r=h.autotune_.result();Metrics def{},tuned{};
  if(success){MachineConfig defaults;def=post(plant,ambient,dead,resolution,defaults);tuned=post(plant,ambient,dead,resolution,h.config_);}
  if(!success){def.mae=def.p95=def.ripple=def.overshoot=NAN;tuned.mae=tuned.p95=tuned.ripple=tuned.overshoot=NAN;}
  const bool bad=success && (tuned.emergency || tuned.settling<0 || tuned.ripple>1.0);
  const bool holdingInsufficient=relay*160.0<=plant.loss*(h.config_.targetTemp-ambient);
  std::cout<<plant.name<<','<<ambient<<','<<dead<<','<<resolution<<','<<unsigned(relay)<<','<<unsigned(preheat)<<','
    <<preheatTime<<','<<success<<','<<autoTuneReasonName(h.autotune_.reason())<<','
    <<unsigned(h.autotune_.cycleCount())<<','<<r.amplitude<<','<<r.periodSec<<','<<r.heatSec<<','<<r.coolSec<<','
    <<(r.coolSec?r.heatSec/r.coolSec:0)<<','<<r.ku<<','<<(success?h.config_.kp:0)<<','<<(success?h.config_.ki:0)<<','
    <<(success?h.config_.kd:0)<<','<<r.gainScale<<','<<(last-1000)*0.001<<','<<high<<','<<emergency<<','
    <<tuned.mae<<','<<tuned.p95<<','<<tuned.ripple<<','<<tuned.overshoot<<','<<(h.autotune_.firstUpperMs()?h.autotune_.firstUpperMs()*0.001:-1)<<','
    <<h.config_.targetTemp<<','<<h.config_.autotuneBandC<<','
    <<h.config_.targetTemp+h.config_.autotuneBandC<<','<<h.config_.highTempAlarm<<','
    <<h.config_.highTempAlarm-h.config_.targetTemp-h.config_.autotuneBandC<<','<<peakAfterOff<<','<<peak<<','
    <<h.autotune_.relayHigh()<<','<<h.autotune_.relayLow()<<','<<holdingInsufficient<<','
    <<def.mae<<','<<def.p95<<','<<def.ripple<<','<<def.overshoot<<','<<def.settling<<','<<def.high<<','<<def.emergency<<','
    <<tuned.settling<<','<<tuned.high<<','<<tuned.emergency<<','
    <<(success?(bad?"BAD":"UNVERIFIED_CANDIDATE"):h.autotune_.reason()==AutoTuneReason::SafetyAbort?"SAFETY_ABORT":"FAIL_CLEANLY")<<','
    <<autoTuneReasonName(h.autotune_.rejection())<<','<<(initial==ambient?"COLD":"NEAR_SP")<<','<<initial<<",CONTROL-ONLY POST VALIDATION / PHYSICAL UNVERIFIED\n";
}
static void disturbances(){
  for(double initial:{25.,37.4})for(unsigned disturbance=0;disturbance<3;++disturbance){
    TuneHarness h;Model model({"light",180000,120},25,15);model.temp=initial;
    ActualSensorFilter filter;filter.updateFilter(initial,60);h.sample(initial,filter.value());
    const char *message=nullptr;assert(h.startAutoTune(1000,message));
    for(unsigned tick=0;tick<=120;++tick){
      if(tick%20==0){float raw=std::round(model.temp*10)/10;filter.updateFilter(raw,60);h.sample(raw,filter.value());}
      if(tick==120){
        if(disturbance==0)h.sample(NAN,NAN); // one invalid PV sample, never sent to relay math
        if(disturbance==1)h.sensorUsable_=false; // confirmed lost sensor validity gate
        if(disturbance==2)h.faults_.inhibit=true;
      }
      h.cycle(1000+tick*100,tick%20==0);model.step(h.outputs_.state().heaterSsr);
    }
    assert(h.autotune_.state()==AutoTuneState::Failed && !h.outputs_.state().heaterSsr && h.store_.saves==0);
    assert(h.config_.kp==18 && h.config_.ki==0.8f && h.config_.kd==45);
    assert(h.autotune_.reason()==(disturbance==2?AutoTuneReason::SafetyAbort:AutoTuneReason::SensorAbort));
    h.sensorUsable_=true;h.sample(37.4f,37.4f);h.faults_.inhibit=false;h.cycle(14000,true);
    assert(!h.outputs_.state().heaterSsr && !h.autotune_.running()); // No automatic resume.
  }
}
int main(int argc,char **argv){
  disturbances();
  assert(argc==2);std::ofstream cycles(argv[1]);assert(cycles);
  cycles<<"plant,ambient,deadtime,sensor_resolution,relay_power,preheat_power,cycle,heat_s,cool_s,high,low,amplitude,period_s,heat_cool_ratio,rejection,start_condition,initial_temperature\n";
  std::cout<<std::fixed<<std::setprecision(6);
  std::cout<<"plant,ambient,deadtime,sensor_resolution,relay_power,preheat_power,preheat_time_s,success,failure_reason,cycle_count,mean_amplitude,mean_period_s,mean_heat_s,mean_cool_s,heat_cool_ratio,Ku,Kp,Ki,Kd,gain_scale,total_tune_time_s,high_crossed,emergency_crossed,post_tune_mae,post_tune_p95,post_tune_ripple,post_tune_overshoot,first_upper_cross_s,target,band,upper_threshold,high_alarm,margin_to_high,peak_after_off,peak,actual_relay_high,actual_relay_low,holding_power_insufficient,default_mae,default_p95,default_ripple,default_overshoot,default_settling_s,default_high_crossed,default_emergency_crossed,post_tune_settling_s,post_tune_high_crossed,post_tune_emergency_crossed,candidate,rejection,start_condition,initial_temperature,post_model_scope\n";
#include "actual-plants.inc"
  for(const auto &plant:plants)for(double ambient:{20.,25.,28.})for(unsigned dead:{5U,15U,30U,60U})
    for(double resolution:{0.1,0.01})for(uint8_t relay:{20U,30U,40U})for(uint8_t preheat:{30U,40U,50U})
      run(plant,ambient,dead,resolution,relay,preheat,ambient,cycles);
  // Separate warmed-chamber qualification isolates relay/timeout behavior from
  // cold preheat. Same plant, no fabricated PV; temperature then evolves in-loop.
  for(const auto &plant:plants)for(double ambient:{20.,25.,28.})for(unsigned dead:{5U,15U,30U,60U})
    for(double resolution:{0.1,0.01})for(uint8_t relay:{20U,30U,40U})
      run(plant,ambient,dead,resolution,relay,AUTOTUNE_PREHEAT_POWER_PERCENT,37.0,cycles);
}
