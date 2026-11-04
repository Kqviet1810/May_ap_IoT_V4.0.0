// Unit tests of the way out of E111 / E112 (MAYAP_INDUSTRIAL_v1_0_0/clear_mean.h), the code MachineController runs every cycle.
//   g++ -std=c++11 -Wall -Wextra -Werror tests/clear-mean.cpp -o /tmp/clear-mean && /tmp/clear-mean
#include <cassert>
#include <cmath>
#include <cstdio>
#include "../MAYAP_INDUSTRIAL_v1_0_0/clear_mean.h"

// Feeds one reading per second from `read(t)` for `seconds`; returns the second at which the window first said "clear" (-1: never).
template <class F>
static int firstClear(MeanClear &m, F read, int seconds, float level, uint32_t windowMs, float guard = 0.15f) {
  for (int t = 0; t < seconds; ++t) {
    // five calls a second, as the control task does: the window must not count the same second twice
    for (int k = 0; k < 5; ++k) if (m.update(static_cast<uint32_t>(t) * 1000UL + static_cast<uint32_t>(k) * 200UL, true, read(t), level, windowMs, guard)) return t;
  }
  return -1;
}

int main() {
  // The levels. High 37.7 and Emergency 37.8 at SV 37.5 (the settings of the field photos): the way out is SV + 0.05, not SV.
  assert(std::fabs(MeanClear::level(37.8f, 0.3f, 37.5f) - 37.55f) < 1e-4f);
  assert(std::fabs(MeanClear::level(37.7f, 0.2f, 37.5f) - 37.55f) < 1e-4f);
  // The factory lines: the plain hysteresis (the floor is lower than trip - H).
  assert(std::fabs(MeanClear::level(39.0f, 0.3f, 37.5f) - 38.7f) < 1e-4f);
  assert(std::fabs(MeanClear::level(38.2f, 0.2f, 37.5f) - 38.0f) < 1e-4f);
  // A line only 0.12 over SV: never closer than 0.1 to the line.
  assert(std::fabs(MeanClear::level(37.62f, 0.3f, 37.5f) - 37.52f) < 1e-4f);

  const float level = MeanClear::level(37.8f, 0.3f, 37.5f);

  // 1. The oven stands at SV and the 0.1 degC reading flickers 37.5 / 37.6 (mean 37.55): the old rule (every sample <= 37.5) never clears, the mean does, after the window.
  {
    MeanClear m; m.reset();
    const int t = firstClear(m, [](int s) { return (s % 2) ? 37.6f : 37.5f; }, 120, level, 30000UL);
    assert(t >= 29 && t <= 31);
    int everySample = 0;                                   // what the old timer needed: 30 s with no sample over 37.5
    for (int s = 0; s < 120; ++s) if ((s % 2) ? 37.6f > 37.5f : false) everySample = 0; else ++everySample;
    assert(everySample < 30);
    std::printf("clear-mean: flicker 37.5/37.6 clears after %d s (the every-sample rule never does)\n", t);
  }
  // 2. A reading that stays over the level never clears.
  {
    MeanClear m; m.reset();
    assert(firstClear(m, [](int) { return 37.8f; }, 300, level, 30000UL) < 0);
    MeanClear n; n.reset();
    assert(firstClear(n, [](int s) { return (s % 3) ? 37.6f : 37.7f; }, 300, level, 30000UL) < 0);     // mean 37.63 > 37.55
  }
  // 3. A single sample far over the level keeps the alarm up until it has left the window.
  {
    MeanClear n; n.reset();
    for (int s = 0; s < 100; ++s) (void)n.update(static_cast<uint32_t>(s) * 1000UL, true, 37.4f, level, 30000UL, 0.15f);
    assert(n.update(100000UL, true, 37.4f, level, 30000UL, 0.15f));            // clear before the spike
    assert(!n.update(101000UL, true, 38.5f, level, 30000UL, 0.15f));
    int t = -1;
    for (int s = 102; s < 160 && t < 0; ++s) if (n.update(static_cast<uint32_t>(s) * 1000UL, true, 37.4f, level, 30000UL, 0.15f)) t = s;
    assert(t >= 130 && t <= 132);                                              // 30 s after the spike
  }
  // 4. An invalid reading starts the window again.
  {
    MeanClear m; m.reset();
    for (int s = 0; s < 40; ++s) (void)m.update(static_cast<uint32_t>(s) * 1000UL, true, 37.4f, level, 30000UL, 0.15f);
    assert(m.update(40000UL, true, 37.4f, level, 30000UL, 0.15f));
    assert(!m.update(41000UL, false, 37.4f, level, 30000UL, 0.15f));
    assert(!m.update(42000UL, true, 37.4f, level, 30000UL, 0.15f));            // one sample only
    assert(!m.update(43000UL, true, NAN, level, 30000UL, 0.15f));
  }
  // 5. The High line: a 10 s window.
  {
    MeanClear m; m.reset();
    const float lv = MeanClear::level(38.2f, 0.2f, 37.5f);
    const int t = firstClear(m, [](int s) { return (s % 2) ? 38.0f : 37.9f; }, 60, lv, 10000UL);
    assert(t >= 9 && t <= 11);
  }
  std::printf("clear-mean: OK\n");
  return 0;
}
