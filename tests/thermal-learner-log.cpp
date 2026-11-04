// The loaded oven of the field log of 04/11 (tests/fixtures/real-logs/mayap-20261104-loaded-oscillation.csv, firmware 2.9.2): the controller believed a heater gain of 3e-5 and a
// transit time of 211 s and swung the command between 0 and 100 % under a PV that hardly moved. A fit of that log by hand gives a gain of 2.2e-5 per % and a delay of 90 s; the
// PlantLearner (thermal_v3.h) is fed the logged PV and command, one second at a time, and must find the same numbers from the oscillation itself.
//   g++ -std=c++11 -O2 -Wall -Wextra -Werror tests/thermal-learner-log.cpp -o /tmp/learner-log && /tmp/learner-log      (from the repository root)
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h"

struct Row { double t; double pv, req; int fan; };

static double epochOf(const std::string &s) {                      // 2026-11-04T11:34:32.085Z -> seconds of that day
  return atof(s.substr(11, 2).c_str()) * 3600.0 + atof(s.substr(14, 2).c_str()) * 60.0 + atof(s.substr(17, 6).c_str());
}

int main() {
  std::ifstream f("tests/fixtures/real-logs/mayap-20261104-loaded-oscillation.csv");
  assert(f.good());
  std::string line;
  std::getline(f, line); std::getline(f, line);
  std::vector<Row> rows;
  while (std::getline(f, line)) {
    std::vector<std::string> c; std::stringstream ss(line); std::string x;
    while (std::getline(ss, x, ',')) c.push_back(x);
    if (c.size() < 10 || c[2].empty() || c[5].empty()) continue;
    Row r; r.t = epochOf(c[0]); r.pv = atof(c[2].c_str()); r.req = atof(c[5].c_str()); r.fan = atoi(c[9].c_str());
    rows.push_back(r);
  }
  assert(rows.size() > 1000);
  const double t0 = 11.0 * 3600.0 + 34.0 * 60.0 + 32.0;           // the start of the heating of that run
  thermalv3::PlantLearner learner; learner.reset();
  size_t k = 0; long sec = 0; int fits = 0; float kLast = 0, dLast = 0;
  for (double t = t0; t < rows.back().t; t += 1.0, ++sec) {
    while (k + 1 < rows.size() && rows[k + 1].t <= t) ++k;
    learner.push(static_cast<float>(rows[k].pv), static_cast<float>(rows[k].req), rows[k].fan == 0);
    if (sec % 30 == 0) { float kk, dd, r2; if (learner.fit(kk, dd, r2)) { ++fits; kLast = kk; dLast = dd; } }
  }
  std::printf("learned from the log: %d fits, last kAir %.2e per %%, delay %.0f s (a fit by hand: 2.2e-5, 90 s; the controller of 2.9.2 believed 3e-5 and 211 s)\n", fits, kLast, dLast);
  assert(fits >= 5);
  assert(kLast > 1.6e-5f && kLast < 3.6e-5f);
  assert(dLast >= 60.0f && dLast <= 135.0f);
  std::printf("thermal-learner-log: ok\n");
  return 0;
}
