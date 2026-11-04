// Open-loop replay of a real night log (firmware 2.5.8, 02/11: tests/fixtures/real-logs/mayap-20261102-night-2.5.8.csv) into the thermal controller.
// The first half hour of that log stood 0.10..0.24 degC under SV for 28 minutes at a flat 10 % request: Anti-Stuck never fired, because a 0.1 degC reading
// steps every 30..100 s and every step reset its "the reading is flat" test (the longest unbroken flat stretch was 97 s of the 120 s it needed). The hold level
// crept 0.07 %/min instead of 1.5 %/min. The replay feeds the logged PV (and the fan flag) and starts from the logged hold level.
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct Row { float pv, req, hold; int fan; };

int main() {
  std::ifstream f("tests/fixtures/real-logs/mayap-20261102-night-2.5.8.csv");
  assert(f.good());
  std::string line; std::getline(f, line); std::getline(f, line);
  std::vector<Row> all;
  while (std::getline(f, line)) {
    std::vector<std::string> c; std::stringstream ss(line); std::string x;
    while (std::getline(ss, x, ',')) c.push_back(x);
    if (c.size() < 20 || c[16] != "machine") continue;
    Row r; r.pv = static_cast<float>(atof(c[2].c_str())); r.req = static_cast<float>(atof(c[5].c_str())); r.hold = static_cast<float>(atof(c[18].c_str())); r.fan = atoi(c[9].c_str());
    all.push_back(r);
  }
  assert(all.size() >= 170);
  thermalv3::Controller c; thermalv3::Input in; in.sp = 37.5f; in.dQ = 0.1f; in.capPct = 100; in.dtS = 0.1f; in.permit = true;
  float maxCmd = 0.0f;
  for (int k = 0; k + 1 < 170; ++k) for (int i = 0; i < 100; ++i) {
    const float a = static_cast<float>(i) / 100.0f;
    in.pv = all[k].pv * (1.0f - a) + all[k + 1].pv * a; in.ventFan = all[k].fan != 0;
    const float u = c.step(in).powerPct;
    if (u > maxCmd) maxCmd = u;
  }
  std::printf("night log, 28 min under SV: hold %.1f -> %.1f %% (logged 2.5.8: %.1f -> %.1f), largest command %.1f %%\n", all[0].hold, c.pHold(), all[0].hold, all[169].hold, maxCmd);
  assert(c.pHold() >= all[0].hold + 3.5f);      // the stall is answered (2.5.8 and 2.7.0 gained 2 %)
  // 2.9.2: 75 %, was 25 %. This is an OPEN-LOOP replay: the recorded PV does not answer the commands, so a controller with the real gain of this oven (9e-5, not the 2.9.1 prior
  // that was 2.7 x too high) winds its hold up for the whole 28 minutes (9.5 -> 47 %, command 61 %), where in the closed loop the same gap closes in 6 minutes and the command never
  // goes above hold + 8 % (tools/thermal-research/log-oven.cpp --gate: reach <= 10 min, no overshoot). Full power is still not the answer to a 0.1 degC gap.
  assert(maxCmd <= 75.0f);
  std::printf("thermal-stall-log: ok\n");
  return 0;
}
