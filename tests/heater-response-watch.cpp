// Unit tests of the E115 evidence class (MAYAP_INDUSTRIAL_v1_0_0/heater_response_watch.h), the same code MachineController runs every control cycle.
#include "../MAYAP_INDUSTRIAL_v1_0_0/heater_response_watch.h"
#include <cassert>
#include <cstdio>
#include <vector>

static constexpr float MIN_RISE = 0.3f;
static constexpr uint32_t REQUIRED_MS = 900000UL;

// Drives the watch at 1 Hz from `startMs`; `temp(t)` gives the temperature at second t, `on(t)` whether the SSR conducts that second.
// Returns the second at which the watch became active (-1 = never within `seconds`).
template <class T, class O>
static int run(HeaterResponseWatch &w, uint32_t startMs, int seconds, T temp, O on, bool demand = true) {
  for (int t = 0; t <= seconds; ++t) {
    w.update(demand, temp(t), startMs + static_cast<uint32_t>(t) * 1000UL, on(t), MIN_RISE, REQUIRED_MS);
    if (w.active()) return t;
  }
  return -1;
}

int main() {
  // 1. A flat temperature under a heater that is always on trips exactly when the configured full-power time has been delivered.
  {
    HeaterResponseWatch w;
    const int at = run(w, 1000U, 3000, [](int) { return 34.0f; }, [](int) { return true; });
    assert(at == 900);                       // the first update only anchors the window
    assert(w.accumOnMs() == REQUIRED_MS);
    std::printf("flat temperature, SSR always on: trips at %d s\n", at);
  }
  // 2. Partial duty: 30 % on delivers the same energy three times slower.
  {
    HeaterResponseWatch w;
    const int at = run(w, 0U, 6000, [](int) { return 34.0f; }, [](int t) { return t % 10 < 3; });
    assert(at >= 2995 && at <= 3010);
    std::printf("flat temperature, SSR 30 %% on: trips at %d s\n", at);
  }
  // 3. A rise of MIN_RISE above the reference starts a new window.
  {
    HeaterResponseWatch w;
    int at = run(w, 0U, 800, [](int) { return 34.0f; }, [](int) { return true; });
    assert(at < 0 && w.accumOnMs() == 800000UL);
    w.update(true, 34.29f, 801000UL, true, MIN_RISE, REQUIRED_MS);
    assert(w.accumOnMs() == 801000UL);       // 0.29 degC is not an answer yet
    w.update(true, 34.31f, 802000UL, true, MIN_RISE, REQUIRED_MS);
    assert(w.accumOnMs() == 0U && !w.active());
    assert(std::fabs(w.referenceC() - 34.31f) < 1e-6f);
  }
  // 4. The 29/10 batch: a door and a tray of cold eggs pull the oven from 36.4 down to 33.3 degC; the heater then lifts it back by 0.1 degC
  //    every ~100 s (the machine's log: 33.3 -> 35.0 in 18 minutes) while the SSR is on ~75 % of the time. The reference follows the
  //    temperature down, so the oven answers every ~5 minutes and the watch never gets near its limit. The rule of <= 2.3.4 compared with the 36.4
  //    at the start of the window and counted 900 s of heater energy 24 minutes into the recovery.
  {
    HeaterResponseWatch w;
    double peak = 0.0;
    auto temp = [](int t) {
      if (t < 200) return 36.4f - 0.0155f * static_cast<float>(t);          // the fall: 3.1 degC in 200 s
      return 33.3f + 0.0016f * static_cast<float>(t - 200);               // the slow lift
    };
    auto on = [](int t) { return t % 4 != 0; };
    for (int t = 0; t <= 3600; ++t) {
      w.update(true, temp(t), static_cast<uint32_t>(t) * 1000UL, on(t), MIN_RISE, REQUIRED_MS);
      peak = std::max(peak, static_cast<double>(w.accumOnMs()));
      assert(!w.active());
    }
    std::printf("recovery of 29/10: never active in an hour; most energy counted without an answer: %.0f s\n", peak / 1000.0);
    assert(peak < 0.5 * REQUIRED_MS);
  }
  // 5. The same fall and then NO lift (a heater that does nothing): it must trip, once 900 s of heater energy have been delivered without
  //    the temperature answering (the energy of the fall counts too: the window has been open since its start).
  {
    HeaterResponseWatch w;
    const int at = run(w, 0U, 3600, [](int t) { return t < 200 ? 36.4f - 0.0155f * static_cast<float>(t) : 33.3f; }, [](int) { return true; });
    assert(at == 900);
    std::printf("fall, then flat under a full heater: trips at %d s (bottom at 200 s)\n", at);
  }
  // 6. No demand drops every piece of evidence; so does a lost sensor value.
  {
    HeaterResponseWatch w;
    run(w, 0U, 500, [](int) { return 34.0f; }, [](int) { return true; });
    assert(w.accumOnMs() > 0U);
    w.update(false, 34.0f, 501000UL, true, MIN_RISE, REQUIRED_MS);
    assert(w.accumOnMs() == 0U && !w.active() && std::isnan(w.referenceC()));
    run(w, 502000U, 500, [](int) { return 34.0f; }, [](int) { return true; });
    w.update(true, NAN, 1003000UL, true, MIN_RISE, REQUIRED_MS);
    assert(w.accumOnMs() == 0U && w.riseC(NAN) == 0.0f);
  }
  // 7. A control task that was late is not heating continuously: one update is worth at most one second.
  {
    HeaterResponseWatch w;
    w.update(true, 34.0f, 0UL, true, MIN_RISE, REQUIRED_MS);
    w.update(true, 34.0f, 60000UL, true, MIN_RISE, REQUIRED_MS);          // 60 s without a call
    assert(w.accumOnMs() == 1000UL);
  }
  // 8. millis() wrapping around in the middle of a window.
  {
    HeaterResponseWatch w;
    const uint32_t start = UINT32_MAX - 450UL * 1000UL;
    const int at = run(w, start, 2000, [](int) { return 34.0f; }, [](int) { return true; });
    assert(at == 900);
  }
  // 9. A fault value that says how far the temperature is above the reference.
  {
    HeaterResponseWatch w;
    w.update(true, 34.0f, 0UL, false, MIN_RISE, REQUIRED_MS);
    w.update(true, 33.8f, 1000UL, false, MIN_RISE, REQUIRED_MS);          // the reference follows down
    w.update(true, 34.0f, 2000UL, false, MIN_RISE, REQUIRED_MS);
    assert(std::fabs(w.riseC(34.0f) - 0.2f) < 1e-5f);
  }
  std::puts("HEATER RESPONSE WATCH: ALL PASSED");
  return 0;
}
