// Oven simulation driver. SIMULATION ONLY: the controller is the REAL firmware heating route (see tests/thermal-adaptive-sim.h);
// only the oven (plant) is a model. One run per process; tools/oven_sim.py starts many of them.
//   oven_sim --header | oven_sim key value ... [--trace file.csv]
#include "../../tests/thermal-adaptive-sim.h"
#include <cstring>
#include <fstream>
#include <iomanip>
using namespace sim;

int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "--header") {
    std::cout << "class,mode,pass,high,emergency,overshoot,mae,p95,ripple,settling_s,rise_s,energy_j,confidence\n";
    return 0;
  }
  Plant p; Scenario sc; std::string tracePath;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i]; const double v = std::atof(argv[i + 1]);
    if (k == "--trace") tracePath = argv[i + 1];
    else if (k == "--eff") p.eff = v; else if (k == "--cap") p.capacity = v; else if (k == "--loss") p.loss = v;
    else if (k == "--dead") p.dead = v; else if (k == "--lag") p.lag = v; else if (k == "--amb") p.ambient = v;
    else if (k == "--res") p.resolution = v; else if (k == "--noise") p.noise = v; else if (k == "--bias") p.bias = v;
    else if (k == "--sp") sc.sp = static_cast<float>(v);
    else if (k == "--mode") sc.mode = static_cast<Mode>(static_cast<int>(v));
    else if (k == "--hours") sc.durationS = v * 3600;
    else if (k == "--event") sc.event = argv[i + 1];
    else if (k == "--eventAt") sc.eventAtS = v * 3600;
    else if (k == "--eventValue") sc.eventValue = v;
    else if (k == "--spBefore") sc.spBefore = static_cast<float>(v);
    else if (k == "--changeAt") sc.changeAtS = v * 3600;
    else if (k == "--effScale") sc.effScale = v; else if (k == "--lossScale") sc.lossScale = v; else if (k == "--capScale") sc.capScale = v;
    else if (k == "--ambDelta") sc.ambientDelta = v;
  }
  sc.label = "oven";
  std::ofstream trace;
  if (!tracePath.empty()) {
    trace.open(tracePath);
    trace << "label,mode,t,temp,fed,sp,duty,vent,conf,state,gain,delay,hold,coast,ventGain,predErr,mismatch,ventPhase,ff,integral,gate,req,holdWin,info\n";
    sc.trace = true; sc.traceOut = &trace;
  }
  const Result r = run(p, sc);
  std::cout << std::fixed << std::setprecision(5) << reachName(r.reach) << ',' << modeName(sc.mode) << ',' << (r.pass ? 1 : 0) << ',' << r.high << ','
            << r.emergency << ',' << r.overshoot << ',' << r.mae << ',' << r.p95 << ',' << r.ripple << ',' << r.settling << ',' << r.riseS << ','
            << r.energyJ << ',' << r.confidence << '\n';
  return 0;
}
