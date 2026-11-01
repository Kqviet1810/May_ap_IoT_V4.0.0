// Compares three views of a 0.1 degC quantized sensor against the true temperature: the raw reading, today's median-3 + IIR 3/8 filter, and
// the quantization-aware observer (sensor_fine.h). Reports the RMS error of the temperature and of the slope.
//   g++ -std=c++17 -O2 tools/thermal-research/sensor-fine-bench.cpp -o /tmp/sfb && /tmp/sfb
#include "../../MAYAP_INDUSTRIAL_v1_0_0/sensor_fine.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <cstdlib>
#include <vector>
#include <algorithm>

struct Scn { const char *name; double (*T)(double); double (*dT)(double); };
static double ramp04(double t) { return 30.0 + 0.04 / 60.0 * t; }
static double dramp04(double) { return 0.04 / 60.0; }
static double ramp10(double t) { return 30.0 + 0.10 / 60.0 * t; }
static double dramp10(double) { return 0.10 / 60.0; }
static double ramp40(double t) { return 30.0 + 0.40 / 60.0 * t; }
static double dramp40(double) { return 0.40 / 60.0; }
static double hold(double t) { return 37.47 + 0.06 * std::sin(2 * M_PI * t / 600.0); }
static double dhold(double t) { return 0.06 * 2 * M_PI / 600.0 * std::cos(2 * M_PI * t / 600.0); }
static double approach(double t) { return 37.5 - 7.5 * std::exp(-t / 900.0); }
static double dapproach(double t) { return 7.5 / 900.0 * std::exp(-t / 900.0); }

static float median3(float a, float b, float c) { return std::max(std::min(a, b), std::min(std::max(a, b), c)); }

int main() {
  const Scn sc[] = {{"ramp 0.04/min", ramp04, dramp04}, {"ramp 0.10/min", ramp10, dramp10}, {"ramp 0.40/min", ramp40, dramp40},
                    {"hold +-0.06 slow wave", hold, dhold}, {"exp approach", approach, dapproach}};
  const double sigmas[] = {0.0, 0.02, 0.05};
  const double periods[] = {2.0, 1.0};
  std::printf("%-24s %5s %5s | RMS temp error [degC]: raw  today  fine | RMS slope error [degC/min]: today  fine\n", "scenario", "sig", "dt");
  for (const Scn &s : sc) for (double sg : sigmas) for (double dt : periods) {
    std::mt19937 rng(7); std::normal_distribution<double> n(0.0, 1.0);
    sensorfine::Params p; if (getenv("MINW")) p.minWindowS = atof(getenv("MINW")); if (getenv("MAXW")) p.maxWindowS = atof(getenv("MAXW")); if (getenv("STEPS")) p.stepsInWindow = atof(getenv("STEPS")); sensorfine::Estimator est; est.reset();
    float w[3]; int wc = 0, wi = 0; bool init = false; float iir = 0, prevIir = 0;
    double eRaw = 0, eToday = 0, eFine = 0, sToday = 0, sFine = 0; long cnt = 0;
    for (double t = 0; t < 2400; t += dt) {
      const double truth = s.T(t) + sg * n(rng);   // sensor noise acts BEFORE the quantizer
      const float q = static_cast<float>(std::round(truth * 10.0) / 10.0);
      w[wi] = q; wi = (wi + 1) % 3; if (wc < 3) ++wc;
      const float med = wc < 3 ? q : median3(w[0], w[1], w[2]);
      if (!init) { iir = med; prevIir = med; init = true; } else { prevIir = iir; iir += (med - iir) * 0.375f; }
      est.update(q, static_cast<float>(t), 0.1f, p);
      if (t < 300) continue;
      const double tr = s.T(t);  // truth without noise
      eRaw += (q - tr) * (q - tr);
      eToday += (iir - tr) * (iir - tr);
      eFine += (est.value() - tr) * (est.value() - tr);
      const double trs = s.dT(t) * 60.0;
      const double todaySlope = (iir - prevIir) / dt * 60.0;
      sToday += (todaySlope - trs) * (todaySlope - trs);
      sFine += (est.slopePerMin() - trs) * (est.slopePerMin() - trs);
      ++cnt;
    }
    std::printf("%-24s %5.2f %5.1f |                       %5.3f %5.3f %5.3f |                              %6.3f %6.3f\n", s.name, sg, dt,
                std::sqrt(eRaw / cnt), std::sqrt(eToday / cnt), std::sqrt(eFine / cnt), std::sqrt(sToday / cnt), std::sqrt(sFine / cnt));
  }
  return 0;
}
