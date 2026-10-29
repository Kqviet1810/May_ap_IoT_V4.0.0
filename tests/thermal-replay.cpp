// Replays the machine's own PID log of the failed batch of 29/10 (tests/fixtures/real-logs/mayap-20261029-batch-failure.csv):
//  1. the E115 rule of firmware <= 2.3.4 trips on that log, at the time the machine tripped; the rule of 2.4.0 does not (the heater was
//     working: the temperature was coming back up, 33.3 -> 35.0 degC in 18 minutes);
//  2. the simulated oven that thermal-closed-loop.cpp uses for that day follows the logged temperature when it is driven with the logged
//     heater request, so what that test shows is about THIS oven and THIS batch, not about an arbitrary model.
#include "thermal-scenarios.h"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace thermalsim;

static int failures = 0;
#define CHECK(cond, ...)                         \
  do {                                           \
    if (!(cond)) {                               \
      ++failures;                                \
      std::printf("  FAIL %s:%d  %s  --  ", __FILE__, __LINE__, #cond); \
      std::printf(__VA_ARGS__);                  \
      std::printf("\n");                         \
    }                                            \
  } while (0)

struct Row {
  double t;        // seconds from the first row
  double pv, sp, requestPct, ssrAvgPct;
  bool ventFan, batch, heatMaster;
};

static std::string logDir() {
  std::string f = __FILE__;
  const size_t slash = f.find_last_of("/\\");
  return (slash == std::string::npos ? std::string(".") : f.substr(0, slash)) + "/fixtures/real-logs/";
}

static double isoSeconds(const std::string &s) {     // 2026-10-29T12:03:03.597Z -> seconds of the day
  const int h = std::atoi(s.substr(11, 2).c_str()), m = std::atoi(s.substr(14, 2).c_str());
  return h * 3600.0 + m * 60.0 + std::atof(s.substr(17, 6).c_str());
}

static std::vector<Row> loadMachineRows(const std::string &name) {
  std::vector<Row> rows;
  std::ifstream f(logDir() + name);
  std::string line;
  double t0 = -1.0;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#' || line.compare(0, 8, "time_utc") == 0) continue;
    std::vector<std::string> c;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, ',')) c.push_back(cell);
    if (c.size() < 17 || c[16] != "machine") continue;          // the rows the web page added are not the machine's own samples
    const double ts = isoSeconds(c[0]);
    if (t0 < 0.0) t0 = ts;
    Row r;
    r.t = ts - t0;
    r.pv = std::atof(c[2].c_str());
    r.sp = std::atof(c[3].c_str());
    r.requestPct = std::atof(c[5].c_str());
    r.ssrAvgPct = std::atof(c[6].c_str());
    r.ventFan = c[9] == "1";
    r.heatMaster = c[11] == "1";
    r.batch = c[12] == "1";
    rows.push_back(r);
  }
  return rows;
}

// Feeds a watch the logged stretch at 1 Hz: the SSR conducts the logged share of every second (the 60 s average of the machine's own SSR
// feedback), the temperature is interpolated between samples. Returns the time of the trip (-1 = none); `peakOnMs` = the largest energy
// the watch had counted without the temperature having answered.
template <class W>
static double replayWatch(const std::vector<Row> &rows, size_t lastRow, double &peakOnMs) {
  W w;
  peakOnMs = 0.0;
  double duty = 0.0;
  size_t k = 0;
  const double end = rows[lastRow].t;
  for (double t = 0.0; t <= end; t += 1.0) {
    while (k + 1 <= lastRow && rows[k + 1].t <= t) ++k;
    const Row &a = rows[k];
    const Row &b = rows[std::min(k + 1, lastRow)];
    const double span = b.t - a.t;
    const double u = span > 0.0 ? (t - a.t) / span : 0.0;
    const double pv = a.pv + (b.pv - a.pv) * u;
    const double avg = a.ssrAvgPct + (b.ssrAvgPct - a.ssrAvgPct) * u;
    duty += avg / 100.0;
    bool on = false;
    if (duty >= 1.0) { on = true; duty -= 1.0; }
    const bool demand = a.batch && a.heatMaster && pv < a.sp - HYST_C;
    w.update(demand, static_cast<float>(pv), static_cast<uint32_t>(t * 1000.0 + 0.5), on, static_cast<float>(MIN_RISE_C), E115_ON_MS);
    peakOnMs = std::max(peakOnMs, static_cast<double>(w.accumOnMs()));
    if (w.active()) return t;
  }
  return -1.0;
}

static void testE115OnTheRealLog(const std::vector<Row> &rows) {
  std::printf("[1] E115 on the machine's log of 29/10\n");
  // The machine cut the heater when heat_master fell to 0 (12:34:29 = 1886 s into the log).
  size_t machineTrip = 0;
  for (size_t i = 0; i < rows.size(); ++i) if (!rows[i].heatMaster) { machineTrip = i; break; }
  CHECK(machineTrip > 0, "the log must contain the moment the machine cut the heater");
  std::printf("  the machine cut the heater at %.0f s into the log (PV %.2f, SP %.1f)\n", rows[machineTrip].t, rows[machineTrip].pv, rows[machineTrip].sp);
  double peakOld = 0, peakNew = 0;
  const double tripOld = replayWatch<LegacyHeaterWatch>(rows, machineTrip - 1, peakOld);
  const double tripNew = replayWatch<HeaterResponseWatch>(rows, machineTrip - 1, peakNew);
  std::printf("  rule of 2.3.4: trips at %6.0f s (most energy counted without an answer: %.0f s)\n", tripOld, peakOld / 1000.0);
  std::printf("  rule of 2.4.0: trips at %6.0f s (most energy counted without an answer: %.0f s)\n", tripNew, peakNew / 1000.0);
  CHECK(tripOld > 0.0 && std::fabs(tripOld - rows[machineTrip].t) < 240.0, "the old rule must trip about when the machine did (%.0f vs %.0f)", tripOld, rows[machineTrip].t);
  CHECK(tripNew < 0.0, "the new rule must not trip while the temperature is coming back up");
  CHECK(peakNew < 0.65 * E115_ON_MS, "the new rule must stay far from its limit on this log (peak %.0f s of 900 s)", peakNew / 1000.0);
  // And it still takes a heater that does nothing: the same log with the temperature held flat at its lowest point. The log is continued at
  // its last heater power for 20 more minutes: the rule of 2.4.0 reads the temperature smoothed over a minute (sensor noise must not hide a
  // dead heater, see heater-response-watch.cpp), which delays its trip a little past the moment the machine cut the heater.
  std::vector<Row> flat(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(machineTrip));
  size_t lowest = 0;
  for (size_t i = 0; i < flat.size(); ++i) if (flat[i].pv < flat[lowest].pv) lowest = i;
  for (size_t i = lowest; i < flat.size(); ++i) flat[i].pv = rows[lowest].pv;
  const Row last = flat.back();
  for (int k = 1; k <= 120; ++k) { Row r = last; r.t = last.t + 10.0 * k; flat.push_back(r); }
  double peakFlat = 0, peakFlatOld = 0;
  const double tripFlat = replayWatch<HeaterResponseWatch>(flat, flat.size() - 1, peakFlat);
  const double tripFlatOld = replayWatch<LegacyHeaterWatch>(flat, flat.size() - 1, peakFlatOld);
  std::printf("  the same heater power with the temperature held flat from its lowest point (%.0f s): the rule of 2.3.4 trips at %.0f s, the rule of 2.4.0 at %.0f s\n",
              rows[lowest].t, tripFlatOld, tripFlat);
  CHECK(tripFlat > rows[lowest].t, "a flat temperature under full heater energy must still trip (got %.0f)", tripFlat);
  CHECK(tripFlatOld > 0.0 && tripFlat <= tripFlatOld + 120.0, "the smoothing may delay the trip by two minutes at most (%.0f vs %.0f)", tripFlat, tripFlatOld);
}

static void testOvenFollowsTheLog(const std::vector<Row> &rows) {
  std::printf("[2] The simulated oven driven by the logged heater request\n");
  // Start: the air a hair under what the sensor reads, eggs 34.85, the heater has been at the first request for a long time.
  Plant pl(u29Oven(), rows[0].pv + 0.09, 34.85);
  pl.primeHeater(rows[0].requestPct);
  SensorChain chain;
  for (int i = 0; i < 8; ++i) chain.sample(rows[0].pv);
  size_t k = 0;
  double sse = 0.0, worst = 0.0;
  int n = 0;
  const double tEnd = 1860.0, doorOpen = 406.0, doorShut = 676.0;
  bool inserted = false;
  for (long i = 0; i < static_cast<long>(tEnd * 10.0); ++i) {
    const double t = i * 0.1;
    while (k + 1 < rows.size() && rows[k + 1].t <= t) ++k;
    const bool door = t >= doorOpen && t < doorShut;
    if (!inserted && t >= doorShut) { pl.insertColdMass(1.1, 22.0); inserted = true; }
    pl.step(rows[k].requestPct, rows[k].ventFan, door);
    if (i % 20 == 0) chain.sample(pl.sensor());
    if (i % 100 == 0 && i > 0) {
      const size_t j = static_cast<size_t>(std::lround(t / 10.0));
      if (j < rows.size()) {
        const double e = chain.value() - rows[j].pv;
        sse += e * e;
        worst = std::max(worst, std::fabs(e));
        ++n;
      }
    }
  }
  const double rms = std::sqrt(sse / std::max(1, n));
  std::printf("  %d samples, RMS %.3f degC, worst %.2f degC\n", n, rms, worst);
  CHECK(n > 150, "the comparison must cover the logged stretch (n=%d)", n);
  CHECK(rms < 0.12, "RMS %.3f", rms);
  CHECK(worst < 0.35, "worst %.2f", worst);
}

int main() {
  std::printf("=== Thermal replay of the machine's own log ===\n");
  const std::vector<Row> rows = loadMachineRows("mayap-20261029-batch-failure.csv");
  CHECK(rows.size() == 230U, "expected 230 machine rows, got %zu", rows.size());
  if (rows.size() != 230U) { std::printf("THERMAL REPLAY: FAILED (log not found next to %s?)\n", __FILE__); return 1; }
  testE115OnTheRealLog(rows);
  testOvenFollowsTheLog(rows);
  if (failures) {
    std::printf("THERMAL REPLAY: %d FAILED\n", failures);
    return 1;
  }
  std::printf("THERMAL REPLAY: ALL PASSED\n");
  return 0;
}
