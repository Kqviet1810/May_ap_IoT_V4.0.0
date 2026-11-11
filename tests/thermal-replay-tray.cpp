// The real night of 10/11 (tests/fixtures/real-logs/mayap-20261110-tray-night.csv) replayed into the controller: at 21:50 a door and a tray of cold mass pulled the air from 37.4 to
// 32.3 degC. The 3.0.0 controller measured kAir and the transit time on the climb back: kAir 22e-5 -> 3e-5 (twice), then up to 80e-5, tau 103 -> 90 s, and every threshold derived
// from them went with it (the log shows kAir 5, 6, 11, 5, 25, 45 and tau 90 while the oven was the same oven). Since 3.0.1 only the climb from cold, before PV has been at SV, measures them.
// The replay is open loop (the oven does not answer the controller's own commands), so it checks what depends on the readings only: the numbers of the oven must not move.
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct Row { double t, pv, sp; bool fan; double kairLogged; };

static double seconds(const std::string &s) {                    // 2026-11-10T12:20:57.437Z -> seconds since the start of the month
  int y, mo, d, h, mi; double se;
  if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lfZ", &y, &mo, &d, &h, &mi, &se) != 6) return -1.0;
  return ((d * 24.0 + h) * 60.0 + mi) * 60.0 + se;
}

int main() {
  FILE *f = std::fopen("tests/fixtures/real-logs/mayap-20261110-tray-night.csv", "r");
  assert(f != nullptr);
  std::vector<Row> rows;
  char line[1024];
  bool header = false;
  while (std::fgets(line, sizeof line, f)) {
    if (line[0] == '#') continue;
    if (!header) { header = true; continue; }
    std::vector<std::string> c; std::string cur;
    for (char *p = line; *p && *p != '\n' && *p != '\r'; ++p) { if (*p == ',') { c.push_back(cur); cur.clear(); } else cur += *p; }
    c.push_back(cur);
    if (c.size() < 8) continue;
    Row r; r.t = seconds(c[0]); r.pv = std::atof(c[1].c_str()); r.sp = std::atof(c[2].c_str()); r.fan = std::atoi(c[3].c_str()) != 0;
    r.kairLogged = c[5].empty() ? NAN : std::atof(c[5].c_str());
    if (r.t >= 0.0 && std::isfinite(r.pv)) rows.push_back(r);
  }
  std::fclose(f);
  assert(rows.size() > 2000);

  thermalv3::Controller ctl; thermalv3::Input in;
  in.capPct = 100.0f; in.dtS = 1.0f; in.permit = true;
  ctl.restore(22.0e-5f, 103.0f);                                  // what the machine had stored and restored at that moment
  ctl.restoreHold(45.0f);
  double kMin = 1e9, kMax = 0.0, tMin = 1e9, tMax = 0.0, pvMin = 99.0, kLoggedMin = 1e9, kLoggedMax = 0.0;
  for (size_t i = 0; i + 1 < rows.size(); ++i) {
    const Row &a = rows[i], &b = rows[i + 1];
    const double gap = b.t - a.t;
    pvMin = std::min(pvMin, a.pv);
    if (std::isfinite(a.kairLogged)) { kLoggedMin = std::min(kLoggedMin, a.kairLogged); kLoggedMax = std::max(kLoggedMax, a.kairLogged); }
    if (gap > 120.0 || gap <= 0.0) continue;                      // a hole in the log
    for (double s = 0.0; s < gap; s += 1.0) {
      in.pv = static_cast<float>(a.pv + (b.pv - a.pv) * s / gap); in.sp = static_cast<float>(a.sp); in.ventFan = a.fan;
      ctl.step(in);
    }
    kMin = std::min(kMin, static_cast<double>(ctl.kAir())); kMax = std::max(kMax, static_cast<double>(ctl.kAir()));
    tMin = std::min(tMin, static_cast<double>(ctl.tauTransit())); tMax = std::max(tMax, static_cast<double>(ctl.tauTransit()));
  }
  std::printf("real night 10/11, %zu readings, the air fell to %.2f degC after the tray\n", rows.size(), pvMin);
  std::printf("  3.0.0 logged kAir %.0f .. %.0f e-5; this controller: %.1f .. %.1f e-5, transit time %.0f .. %.0f s (stored: 22e-5, 103 s)\n", kLoggedMin, kLoggedMax, kMin * 1e5, kMax * 1e5, tMin, tMax);
  assert(pvMin < 33.0);                                           // the tray is in the fixture
  assert(kLoggedMin < 5.0 && kLoggedMax > 40.0);                  // and so is what 3.0.0 made of it
  assert(kMin >= 0.95 * 22.0e-5 && kMax <= 1.05 * 22.0e-5);
  assert(tMin >= 100.0 && tMax <= 106.0);
  std::puts("THERMAL REPLAY (tray night): ALL PASSED");
  return 0;
}
