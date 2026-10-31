// Unit tests of what the thermal controller keeps across a restart (MAYAP_INDUSTRIAL_v1_0_0/thermal_learn.h) and of the way the controller takes it
// back (thermal_v3.h importOven / importHold). Only a state that proved itself is stored; a damaged or foreign one is ignored as a whole.
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_learn.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace thermallearn;

static Snapshot sample() {
  Snapshot s;
  s.pHold = 11.5f; s.kAir = 2.4e-4f; s.tau = 142.0f; s.pipeLen = 191U; s.sp = 37.5f; s.capPct = 100U;
  s.batchEpoch = 1793400000UL; s.savedEpoch = 1793450000UL;
  return s;
}

// Feeds one second at a time for `seconds`; returns the second at which the tracker asked for a write (-1 = never).
template <class Fn>
static int run(StableTracker &t, const StableTracker::Params &p, uint32_t startMs, int seconds, Fn at) {
  for (int s = 0; s <= seconds; ++s) {
    bool ok; float pv, sp, hold;
    at(s, ok, pv, sp, hold);
    if (t.update(startMs + static_cast<uint32_t>(s) * 1000UL, ok, pv, sp, hold, p)) return s;
  }
  return -1;
}

int main() {
  // 1. The snapshot survives a round trip, to the resolution it is kept in.
  {
    uint8_t blob[BLOB_SIZE];
    assert(encode(sample(), blob));
    Snapshot r;
    assert(decode(blob, BLOB_SIZE, r));
    assert(std::fabs(r.pHold - 11.5f) < 0.006f && std::fabs(r.kAir - 2.4e-4f) < 2e-8f && std::fabs(r.tau - 142.0f) < 0.06f);
    assert(r.pipeLen == 191U && r.capPct == 100U && std::fabs(r.sp - 37.5f) < 0.006f);
    assert(r.batchEpoch == 1793400000UL && r.savedEpoch == 1793450000UL);
  }
  // 2. Anything damaged is refused as a whole: every single flipped bit, a wrong length, a wrong version or magic, values out of range.
  {
    uint8_t blob[BLOB_SIZE];
    assert(encode(sample(), blob));
    Snapshot r;
    for (size_t i = 0; i < BLOB_SIZE; ++i)
      for (int b = 0; b < 8; ++b) {
        uint8_t bad[BLOB_SIZE];
        std::memcpy(bad, blob, BLOB_SIZE);
        bad[i] = static_cast<uint8_t>(bad[i] ^ (1U << b));
        assert(!decode(bad, BLOB_SIZE, r));
      }
    assert(!decode(blob, BLOB_SIZE - 1U, r) && !decode(nullptr, BLOB_SIZE, r));
    Snapshot bad = sample(); bad.kAir = 1e-2f; uint8_t o[BLOB_SIZE]; assert(!encode(bad, o));
    bad = sample(); bad.tau = 30.0f; assert(!encode(bad, o));
    bad = sample(); bad.pHold = 120.0f; assert(!encode(bad, o));
    bad = sample(); bad.pipeLen = 400U; assert(!encode(bad, o));
    bad = sample(); bad.capPct = 0U; assert(!encode(bad, o));
    bad = sample(); bad.kAir = NAN; assert(!encode(bad, o));
  }
  // 3. What may come back: the oven constants for the same heater cap, the hold level only for the same batch at about the same SV.
  {
    const Snapshot s = sample();
    assert(ovenUsable(s, 100U) && !ovenUsable(s, 80U));
    assert(holdUsable(s, 100U, 1793400000UL, 37.5f));
    assert(!holdUsable(s, 100U, 1793400001UL, 37.5f));          // another batch
    assert(!holdUsable(s, 100U, 0U, 37.5f));                    // no batch
    assert(!holdUsable(s, 80U, 1793400000UL, 37.5f));           // another heater cap
    assert(!holdUsable(s, 100U, 1793400000UL, 38.8f));          // SV moved by more than a degree
  }
  // 4. Two stable hours before the first write, not a second earlier; a write that is not confirmed is tried again after ten minutes.
  {
    StableTracker t; const StableTracker::Params p;
    const int at = run(t, p, 1000U, 9000, [](int, bool &ok, float &pv, float &sp, float &hold) { ok = true; pv = 37.52f; sp = 37.5f; hold = 12.0f; });
    assert(at >= 7199 && at <= 7202);
    // not confirmed: asks again after 10 minutes (the caller's write failed)
    int again = -1;
    for (int s = at + 1; s <= at + 900 && again < 0; ++s)
      if (t.update(1000U + static_cast<uint32_t>(s) * 1000UL, true, 37.5f, 37.5f, 12.0f, p)) again = s;
    assert(again >= at + 599 && again <= at + 602);
    std::printf("stable 2 h: write at %d s, asked again at +%d s\n", at, again - at);
  }
  // 5. After a confirmed write nothing more for two hours; then only when the hold level moved by more than 1.5 %.
  {
    StableTracker t; const StableTracker::Params p;
    uint32_t now = 0U; int writes = 0; float hold = 12.0f;
    for (int s = 0; s < 6 * 3600; ++s) {
      now += 1000U;
      if (s == 3 * 3600) hold = 14.0f;                             // 2 % up, once
      if (t.update(now, true, 37.5f, 37.5f, hold, p)) { ++writes; t.saved(hold); }
    }
    assert(writes == 2);                                           // the first at 2 h, one more once 2 stable hours passed after the move
  }
  // 6. Any break counts from zero: an alarm (ok = false), a reading away from SV, a silence longer than 5 s.
  {
    StableTracker t; const StableTracker::Params p;
    const int a = run(t, p, 0U, 12000, [](int s, bool &ok, float &pv, float &sp, float &hold) { ok = s != 3600; pv = 37.5f; sp = 37.5f; hold = 12.0f; });
    assert(a >= 3600 + 7199 && a <= 3600 + 7202 - 0);              // the stretch restarted at 3600 s
    StableTracker u;
    const int b = run(u, p, 0U, 13000, [](int s, bool &ok, float &pv, float &sp, float &hold) { ok = true; pv = s == 5000 ? 37.75f : 37.5f; sp = 37.5f; hold = 12.0f; });
    assert(b >= 5000 + 7199 && b <= 5000 + 7202);
    StableTracker w; uint32_t now = 0U; int got = -1;
    for (int s = 0; s < 12000 && got < 0; ++s) {
      now += (s == 4000) ? 20000U : 1000U;                         // the control loop stalled for 20 s
      if (w.update(now, true, 37.5f, 37.5f, 12.0f, p)) got = s;
    }
    assert(got >= 4000 + 7199 - 25 && got <= 4000 + 7202);
  }
  // 7. A hold level that is still moving is not a state to keep (0.1 % a minute: 3 % in the last half hour).
  {
    StableTracker t; const StableTracker::Params p;
    const int at = run(t, p, 0U, 4 * 3600, [](int s, bool &ok, float &pv, float &sp, float &hold) { ok = true; pv = 37.5f; sp = 37.5f; hold = 20.0f - 0.1f * static_cast<float>(s) / 60.0f; });
    assert(at < 0);
  }
  // 8. A snapshot that was cleared (by hand, or after a heater fault) is written again after the next stable two hours.
  {
    StableTracker t; const StableTracker::Params p;
    uint32_t now = 0U; int writes = 0;
    for (int s = 0; s < 5 * 3600 + 120; ++s) {
      now += 1000U;
      if (s == 3 * 3600) t.invalidate();
      if (t.update(now, true, 37.5f, 37.5f, 12.0f, p)) { ++writes; t.saved(12.0f); }
    }
    assert(writes == 2);
  }
  // 9. The same across the wrap of the 32-bit millisecond clock.
  {
    StableTracker t; const StableTracker::Params p;
    const int at = run(t, p, 0xFFFFFFFFUL - 3600000UL, 9000, [](int, bool &ok, float &pv, float &sp, float &hold) { ok = true; pv = 37.5f; sp = 37.5f; hold = 12.0f; });
    assert(at >= 7199 && at <= 7202);
  }
  // 10. The controller takes the oven constants as the start of the next climb (still unmeasured), and the hold level of a resumed batch as measured.
  {
    thermalv3::Controller c;
    assert(!c.identified());
    c.importOven(2.4e-4f, 142.0f, 191U);
    assert(std::fabs(c.kAir() - 2.4e-4f) < 1e-9f && std::fabs(c.tauTransit() - 142.0f) < 1e-3f && !c.identified());
    c.importOven(1.0f, 1000.0f, 5000U);                            // nonsense is clamped, never trusted
    assert(c.kAir() <= 7.5e-4f && c.tauTransit() <= 220.0f);
    c.importOven(2.4e-4f, 142.0f, 191U);
    c.importHold(11.5f, true);
    assert(std::fabs(c.pHold() - 11.5f) < 1e-4f && c.identified());
    // and it runs from there: a restart next to SV with the restored numbers holds without a swing
    thermalv3::Input in; in.sp = 37.5f; in.pv = 37.5f; in.dQ = 0.1f; in.capPct = 100.0f; in.dtS = 0.1f; in.permit = true;
    float last = 0.0f;
    for (int i = 0; i < 6000; ++i) last = c.step(in).powerPct;     // 10 minutes at SV
    assert(last > 8.0f && last < 15.0f);
    std::printf("restored hold 11.5 %% at SV: %.1f %% after ten minutes\n", last);
  }
  std::printf("thermal-learn: OK\n");
  return 0;
}
