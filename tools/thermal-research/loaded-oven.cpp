// A LOADED oven (field log of 04/11 12:11, tests/fixtures/real-logs/mayap-20261104-loaded-oscillation.csv): cold-ish eggs, a heater gain of the air of 2e-5 per % once the eggs take
// their share (a fit of that log: 2.2e-5, delay 90 s, hold 17 %; the empty oven of 29/10 has 9e-5), hold ~30 %. Start 35.6 degC, SP 37.5. 2.9.1 / 2.9.2 believed a gain 4 x too high and
// a delay 2.3 x too long and swung the command between 0 and 100 %; the numbers of an oven are LEARNED (PlantLearner), not typed.
//   g++ -O2 -std=c++11 -I MAYAP_INDUSTRIAL_v1_0_0 -I tests tools/thermal-research/loaded-oven.cpp -o /tmp/loaded-oven && /tmp/loaded-oven --gate
// Environment: UA (450), CE (3.5e6), TD, TL, EGGD (2), MIN (150); any argument but --gate prints a line per minute.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <string>
#include "sensor_fine.h"
#include "thermal_v3.h"
#include "thermal-sim.h"
static double gauss(uint32_t &x){auto u=[&x](){x^=x<<13;x^=x>>17;x^=x<<5;return (x+1.0)/4294967297.0;};return std::sqrt(-2.0*std::log(u()))*std::cos(6.283185307179586*u());}
static int runOne(int argc,char**argv,double &pp,double &du){
  using namespace thermalsim;
  PlantParams p=u29Oven(); p.UA=getenv("UA")?atof(getenv("UA")):450; p.Ce=getenv("CE")?atof(getenv("CE")):3.5e6; p.ventW=100;
  if(getenv("TD")) p.td=atof(getenv("TD"));
  if(getenv("TL")) p.tl=atof(getenv("TL"));
  const double eggDelta=getenv("EGGD")?atof(getenv("EGGD")):2.0, mins=getenv("MIN")?atof(getenv("MIN")):60;
  Plant pl(p,35.6,35.6-eggDelta);
  thermalv3::Controller c; sensorfine::Estimator fv,fs; sensorfine::Params pv,ps; pv.minWindowS=10; pv.maxWindowS=30;
  uint32_t rng=12345u; float pvFine=35.6f; const bool verbose=argc>1 && std::string(argv[1])!="--gate";
  double sumU=0,swing=0; int nsw=0; double lastU=0, mx=0, mn=99;
  for(long i=0;i<(long)(mins*600);++i){ double t=i*0.1;
    if(i%10==0){ double q=std::lround((pl.sensor()+0.025*gauss(rng))*10.0)/10.0; fv.update((float)q,(float)t,0.1f,pv); fs.update((float)q,(float)t,0.1f,ps);}
    if(fv.ready()) pvFine=fv.valueAt((float)(t-std::floor(t)));
    thermalv3::Input in; in.pv=pvFine; in.sp=37.5f; in.dtS=0.1f; in.permit=true; in.capPct=100; in.slopeFine=fs.ready()?fs.slopePerS():NAN;
    float u=c.step(in).powerPct; pl.step(u,false,false);
    if(t>(mins-50)*60){ mx=std::max(mx,pl.air()); mn=std::min(mn,pl.air()); if(i%10==0){ if(std::fabs(u-lastU)>0) swing+=std::fabs(u-lastU); ++nsw; lastU=u; sumU+=u;} }
    if(verbose && i%600==0) std::printf("t=%3.0f min air=%.2f pv=%.2f u=%5.1f hold=%5.1f kair=%.1e tau=%.0f pred=%.2f id=%d\n",t/60,pl.air(),pvFine,u,c.pHold(),c.kAir(),c.tauTransit(),c.predicted(),(int)c.identified());
  }
  pp=mx-mn; du=swing/std::max(1,nsw);
  std::printf("last 50 min of %.0f: air %.2f..%.2f (p-p %.2f) | mean |du| per s %.2f %% | mean u %.0f %% | kair %.1e tau %.0f\n",mins,mn,mx,mx-mn,swing/std::max(1,nsw),sumU/std::max(1,nsw),c.kAir(),c.tauTransit());
  return 0;
}
int main(int argc,char**argv){
  const bool gate=argc>1 && std::string(argv[1])=="--gate";
  if(!gate){ double pp,du; return runOne(argc,argv,pp,du); }
  // the gate: three loaded ovens (delay + lag 190, 130 and 100 s); after 100 minutes the air stays within 0.25 degC and the command no longer swings
  const char *ce[3]={"3.5e6","2.5e6","3.5e6"}, *ua[3]={"450","300","450"}, *td[3]={"110","110","60"}, *tl[3]={"80","80","40"};
  bool ok=true;
  for(int k=0;k<3;++k){ setenv("CE",ce[k],1); setenv("UA",ua[k],1); setenv("TD",td[k],1); setenv("TL",tl[k],1); setenv("MIN","150",1);
    double pp=0,du=0; runOne(1,argv,pp,du); const bool good=pp<=0.25 && du<=0.25; ok=ok&&good;
    std::printf("loaded-oven gate %d: peak to peak %.2f degC (<= 0.25), mean |du| %.2f %%/s (<= 0.25): %s\n",k,pp,du,good?"OK":"FAIL"); }
  return ok?0:1;
}
