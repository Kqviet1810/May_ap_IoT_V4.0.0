// Replay of the logged noon of 06/11 (08:00-13:03): the room is derived from the logged heater request (PV was flat, so request = load), the vent fan follows the log,
// the released controller runs in the rig. Compares the PV band, the hold curve and the activity with the log. Build from the repository root:
//   g++ -O2 -std=c++17 -DTHERMAL_SENSOR_NOISE_K=0.02 -I tests -I MAYAP_INDUSTRIAL_v1_0_0 tools/thermal-research/calib-noon.cpp -o /tmp/calib-noon && /tmp/calib-noon 200 1500
#define THERMAL_SCENARIOS_WITH_LEGACY
#include "../../tests/thermal-scenarios.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
using namespace thermalsim;
struct Row{double t,req,pv,hold;int fan;};
int main(int argc,char**argv){
  std::vector<Row> R; std::ifstream f("tools/thermal-research/calib-noon.csv"); std::string line;
  while(std::getline(f,line)){ std::stringstream ss(line); Row r; char c; ss>>r.t>>c>>r.req>>c>>r.pv>>c>>r.hold>>c>>r.fan; R.push_back(r);}
  const double UA=argc>1?atof(argv[1]):200.0, ventW=argc>2?atof(argv[2]):1500.0, tail=argc>3?atof(argv[3]):0.0;
  PlantParams p=u29Oven(); p.UA=UA; p.ventW=ventW;
  // smoothed logged request = the load the oven needed (PV was flat); ambient from it
  std::vector<double> load(R.size()); double e=R[0].req; for(size_t i=0;i<R.size();++i){ e+=(R[i].req-e)/30.0; load[i]=e; }
  p.Tamb = 37.5 - load[0]/100.0*p.Pmax/UA; Rig r(p,37.5,37.5); r.plant.settleAt(37.5);
  r.advance(3600.0);                                    // an hour settled at the first load so the controller has learned
  const size_t from=r.seen().size();
  std::vector<double> pv; std::vector<double> hold,req;
  for(size_t i=0;i<R.size();++i){
    r.plant.setAmbient(37.5 - load[i]/100.0*p.Pmax/UA);
    r.advance(10.0,false,R[i].fan!=0);
    pv.push_back(r.plant.sensor()); hold.push_back(r.ctl.pHold()); req.push_back(r.lastU());
  }
  // compare per 20 min block with the log
  std::printf(" block  | log pv mean max  hold | sim pv mean max  hold | log >37.55 | sim >37.55\n");
  double ssq=0; int n=0;
  for(size_t b=0;b<R.size();b+=120){
    size_t e2=std::min(R.size(),b+120); double lm=0,sm=0,lx=0,sx=0,lh=0,sh=0; int lo=0,so=0;
    for(size_t i=b;i<e2;++i){ lm+=R[i].pv; sm+=pv[i]; lx=std::max(lx,R[i].pv); sx=std::max(sx,pv[i]); lh+=R[i].hold; sh+=hold[i]; if(R[i].pv>37.55) ++lo; if(pv[i]>37.55) ++so; }
    int m=e2-b; std::printf(" %4zu m | %.3f %.2f %5.1f | %.3f %.2f %5.1f | %3d%% | %3d%%\n",b/6,lm/m,lx,lh/m,sm/m,sx,sh/m,100*lo/m,100*so/m);
    for(size_t i=b;i<e2;++i){ double d=pv[i]-R[i].pv; ssq+=d*d; ++n; }
  }
  // activity
  int chL=0,chS=0; for(size_t i=1;i<R.size();++i){ if(std::fabs(R[i].pv-R[i-1].pv)>1e-4)++chL; if(std::fabs(pv[i]-pv[i-1])>1e-4)++chS; }
  int revL=0,revS=0; double lastL=0,lastS=0; for(size_t i=1;i<R.size();++i){ double dl=R[i].req-R[i-1].req; if(dl!=0){ if(lastL!=0&&(dl>0)!=(lastL>0))++revL; lastL=dl;} double ds=std::round(req[i]*2)/2-std::round(req[i-1]*2)/2; if(ds!=0){ if(lastS!=0&&(ds>0)!=(lastS>0))++revS; lastS=ds;} }
  double hrs=R.size()*10.0/3600.0;
  std::printf("PV rms diff %.3f | PV changes/h log %.0f sim %.0f | request reversals/h log %.0f sim %.0f\n",std::sqrt(ssq/n),chL/hrs,chS/hrs,revL/hrs,revS/hrs);
}
