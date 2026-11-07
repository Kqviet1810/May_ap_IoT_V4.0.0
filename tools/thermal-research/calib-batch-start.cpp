// Which plant reproduces the slow batch start of 05/11 14:37 (PV flat at 37.0 for 30 min while the request rose 16 -> 40 %)? The plant is driven by the LOGGED request and its PV is
// compared with the logged PV: (0) a weak heater (gain scale), (1) delivered power capped, (2) a constant extra sink (cold eggs). Result: see doc in the report; cold eggs do not fit.
//   g++ -O2 -std=c++17 tools/thermal-research/calib-batch-start.cpp -o /tmp/calib-start && /tmp/calib-start tools/thermal-research/calib-batch-start.csv
#include "../../tests/thermal-sim.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
using namespace thermalsim;
struct Row{double t,req,pv,hold;int fan;};
int main(int argc,char**argv){
  std::vector<Row> R; std::ifstream f(argv[1]); std::string line;
  while(std::getline(f,line)){ std::stringstream ss(line); Row r; char c; ss>>r.t>>c>>r.req>>c>>r.pv>>c>>r.hold>>c>>r.fan; R.push_back(r);}
  const double tEnd=R.back().t;
  struct Res{double rms; double s,usat,Q,egg,UA,p0; int model;};
  std::vector<Res> all;
  for(int model=0;model<3;++model)
  for(double s: (model==0? std::vector<double>{0.15,0.2,0.25,0.3,0.4,0.5,0.7,1.0}: std::vector<double>{1.0}))
  for(double usat: (model==1? std::vector<double>{10,13,16,20,25,30,40}: std::vector<double>{1000}))
  for(double Q: (model==2? std::vector<double>{0,-500,-1000,-1500,-2000,-3000,-4000,-5000,-6000}: std::vector<double>{0}))
  for(double egg: {26.0,30.0,34.0,37.0})
  for(double UA: {100.0,200.0,300.0})
  for(double p0: {0.0,8.0,16.0}){
    PlantParams p=u29Oven(); p.Pmax*=s; p.UA=UA; p.Q0=Q;
    Plant pl(p,37.2,egg);
    pl.primeHeater(p0/ s>100?100:p0/s);   // p0 given as % of the nominal 16 kW
    double se=0; int n=0; size_t k=0;
    for(long i=0;i<(long)(tEnd*10);++i){
      double t=i*0.1; while(k+1<R.size() && R[k+1].t<=t) ++k;
      double u=std::min(R[k].req,usat);
      pl.step(u/ (1.0) ,false,false);
      if(i%100==0 && t>=60){ double e=pl.sensor()-R[k].pv; se+=e*e; ++n; }
    }
    all.push_back({std::sqrt(se/n),s,usat,Q,egg,UA,p0,model});
  }
  for(int model=0;model<3;++model){
    std::vector<Res> m; for(auto&r:all) if(r.model==model) m.push_back(r);
    std::sort(m.begin(),m.end(),[](const Res&a,const Res&b){return a.rms<b.rms;});
    const char*nm[3]={"weak heater (gain scale s)","saturation (delivered <= usat %)","extra sink Q0 W"};
    std::printf("== %s: best 3\n",nm[model]);
    for(int i=0;i<3&&i<(int)m.size();++i) std::printf("  rms %.3f  s=%.2f usat=%.0f Q0=%.0f egg=%.0f UA=%.0f p0=%.0f\n",m[i].rms,m[i].s,m[i].usat,m[i].Q,m[i].egg,m[i].UA,m[i].p0);
  }
}
