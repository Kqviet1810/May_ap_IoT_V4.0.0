// Unit tests of the fine temperature estimate built from a 0.1 degC quantized reading (MAYAP_INDUSTRIAL_v1_0_0/sensor_fine.h).
#include "../MAYAP_INDUSTRIAL_v1_0_0/sensor_fine.h"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace sensorfine;

static float quant(double v) { return static_cast<float>(std::round(v * 10.0) / 10.0); }

int main() {
  Params p;
  // 1) steady climb 0.10 degC/min: slope within 0.02 degC/min and the value clearly finer than the staircase
  {
    Estimator e; e.reset();
    double errRaw = 0, errFine = 0; int n = 0; float slope = 0;
    for (double t = 0; t < 1800; t += 2.0) {
      const double truth = 30.0 + 0.10 / 60.0 * t;
      const float q = quant(truth);
      e.update(q, static_cast<float>(t), 0.1f, p);
      if (t >= 300) { errRaw += (q - truth) * (q - truth); errFine += (e.value() - truth) * (e.value() - truth); ++n; slope = e.slopePerMin(); }
    }
    const double rawRms = std::sqrt(errRaw / n), fineRms = std::sqrt(errFine / n);
    std::printf("climb: raw rms %.4f fine rms %.4f slope %.4f degC/min\n", rawRms, fineRms, slope);
    assert(fineRms < rawRms * 0.5);
    assert(std::fabs(slope - 0.10f) < 0.02f);
  }
  // 2) flat: slope stays near zero and the value stays inside the reported cell
  {
    Estimator e; e.reset();
    for (double t = 0; t < 900; t += 2.0) e.update(37.5f, static_cast<float>(t), 0.1f, p);
    assert(std::fabs(e.slopePerMin()) < 0.005f);
    assert(std::fabs(e.value() - 37.5f) < 0.01f);
  }
  // 3) a fall (door open) is followed: slope negative and large, the value tracks within one step
  {
    Estimator e; e.reset();
    for (double t = 0; t < 600; t += 2.0) {
      const double truth = t < 300 ? 37.5 : 37.5 - 0.3 / 60.0 * (t - 300);  // -0.3 degC/min after 300 s
      e.update(quant(truth), static_cast<float>(t), 0.1f, p);
      if (t > 420) { assert(e.slopePerMin() < -0.2f); assert(std::fabs(e.value() - truth) < 0.1); }
    }
  }
  // 4) a time gap or a clock restart starts over without garbage; a NaN reading is ignored
  {
    Estimator e; e.reset();
    for (double t = 0; t < 200; t += 2.0) e.update(36.0f, static_cast<float>(t), 0.1f, p);
    e.update(NAN, 202.0f, 0.1f, p);
    assert(std::fabs(e.value() - 36.0f) < 0.02f);
    e.update(38.0f, 500.0f, 0.1f, p);   // 300 s gap
    assert(!e.ready() || std::fabs(e.value() - 38.0f) < 0.02f);
    assert(std::fabs(e.slopePerMin()) < 0.01f);
    e.update(38.0f, 1.0f, 0.1f, p);     // time went backwards
    assert(std::fabs(e.value() - 38.0f) < 0.02f);
  }
  // 5) bounded: hostile input never gives a non-finite or absurd slope
  {
    Estimator e; e.reset();
    for (int i = 0; i < 2000; ++i) {
      e.update((i % 2) ? 20.0f : 60.0f, static_cast<float>(i) * 1.0f, 0.1f, p);
      assert(std::isfinite(e.value()) && std::fabs(e.slopePerS()) <= p.maxSlope + 1e-6f);
    }
  }
  std::printf("sensor-fine: ok\n");
  return 0;
}
