#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_load_ledger.h"
using namespace thermalload;

static void feedMinutes(Ledger &l, uint32_t &t, int minutes, bool settled, float hold, float sp) {
  for (int s = 0; s < minutes * 60; ++s) { l.feed(t, settled, hold, sp); ++t; }
}

int main() {
  // 1. a checkpoint is the mean of a 30 minute window with at least 10 settled minutes
  {
    Ledger l; uint32_t t = 100000U;
    feedMinutes(l, t, 20, true, 10.0f, 37.5f);
    assert(l.count() == 0U);                                   // the window is not over
    feedMinutes(l, t, 10, true, 12.0f, 37.5f);
    assert(l.count() == 1U && l.takeDirty() && !l.takeDirty());
    assert(std::fabs(l.checkpoint(0).hold - (20.0f * 10.0f + 10.0f * 12.0f) / 30.0f) < 0.01f);
    // a window with under 10 settled minutes gives nothing
    Ledger m; t = 0U;
    feedMinutes(m, t, 8, true, 10.0f, 37.5f); feedMinutes(m, t, 22, false, 99.0f, 37.5f);
    assert(m.count() == 0U);
    // unsettled time does not enter the mean
    Ledger n; t = 0U;
    feedMinutes(n, t, 15, true, 10.0f, 37.5f); feedMinutes(n, t, 15, false, 90.0f, 37.5f);
    assert(n.count() == 1U && std::fabs(n.checkpoint(0).hold - 10.0f) < 0.01f);
  }
  // 2. a pause in the feed (a restart) and a set-point change throw the window away
  {
    Ledger l; uint32_t t = 0U;
    feedMinutes(l, t, 20, true, 10.0f, 37.5f);
    t += 600U;                                                 // 10 minutes of nothing: the machine was off
    feedMinutes(l, t, 20, true, 10.0f, 37.5f);
    assert(l.count() == 0U);                                   // 20 + 20 minutes would have made a window; the pause cancelled the first part
    feedMinutes(l, t, 10, true, 10.0f, 37.5f);
    assert(l.count() == 1U);
    Ledger m; t = 0U;
    feedMinutes(m, t, 25, true, 10.0f, 37.5f); feedMinutes(m, t, 10, true, 10.0f, 37.0f);
    assert(m.count() == 0U);
  }
  // 3. advice: shaded, full confidence for 30 minutes, fading to nothing at 90, refused for another set-point or a backwards clock
  {
    Ledger l; uint32_t t = 0U;
    feedMinutes(l, t, 30, true, 20.0f, 37.5f);
    const uint32_t end = l.checkpoint(0).endS;
    Advice a = l.advise(end, 37.5f);
    assert(a.valid && std::fabs(a.holdPct - 17.0f) < 0.01f && a.confidence == 1.0f);
    a = l.advise(end + 1800U, 37.5f);  assert(a.valid && a.confidence == 1.0f);
    a = l.advise(end + 3600U, 37.5f);  assert(a.valid && std::fabs(a.confidence - 0.5f) < 0.01f);
    a = l.advise(end + 5400U, 37.5f);  assert(a.valid && a.confidence == 0.0f);
    a = l.advise(end + 5401U, 37.5f);  assert(!a.valid);
    assert(!l.advise(end, 37.0f).valid);                       // another set-point
    assert(!l.advise(end - 10U, 37.5f).valid);                 // the clock went backwards
    assert(!Ledger().advise(end, 37.5f).valid);                // nothing recorded
    // blend: prior 12, advice 17 at half confidence -> 14.5; no advice -> the prior
    Advice h; h.valid = true; h.holdPct = 17.0f; h.confidence = 0.5f;
    assert(std::fabs(Ledger::blend(12.0f, h) - 14.5f) < 1e-4f && Ledger::blend(12.0f, Advice()) == 12.0f);
  }
  // 4. trend: two checkpoints 30 minutes apart, rising 4 points -> the advice looks ahead, damped and bounded
  {
    Ledger l; uint32_t t = 0U;
    feedMinutes(l, t, 30, true, 10.0f, 37.5f); feedMinutes(l, t, 30, true, 14.0f, 37.5f);
    assert(l.count() == 2U);
    const Advice a = l.advise(l.checkpoint(1).endS + 1800U, 37.5f);               // 30 minutes after the last checkpoint: + 0.5 x 4 x 1 = + 2
    assert(a.valid && std::fabs(a.holdPct - 0.85f * 16.0f) < 0.02f);
    const Advice far = l.advise(l.checkpoint(1).endS + 5400U, 37.5f);             // the step is bounded by 5 points
    assert(std::fabs(far.holdPct - 0.85f * 19.0f) < 0.02f);
    // a falling trend never goes under 0
    Ledger f; t = 0U;
    feedMinutes(f, t, 30, true, 3.0f, 37.5f); feedMinutes(f, t, 30, true, 0.5f, 37.5f);
    assert(f.advise(t + 3600U, 37.5f).holdPct >= 0.0f);
    // checkpoints too far apart give no trend
    Ledger g; t = 0U;
    feedMinutes(g, t, 30, true, 10.0f, 37.5f); t += 7200U;
    feedMinutes(g, t, 30, true, 20.0f, 37.5f);
    assert(std::fabs(g.advise(t + 1800U, 37.5f).holdPct - 0.85f * 20.0f) < 0.02f);
  }
  // 5. the ring keeps the newest six
  {
    Ledger l; uint32_t t = 0U;
    for (int i = 0; i < 9; ++i) feedMinutes(l, t, 30, true, static_cast<float>(i), 37.5f);
    assert(l.count() == RING && std::fabs(l.checkpoint(RING - 1U).hold - 8.0f) < 0.01f && std::fabs(l.checkpoint(0).hold - 3.0f) < 0.01f);
  }
  // 6. persistence: round trip, CRC, every flipped bit is caught, a torn record is refused
  {
    Ledger l; uint32_t t = 5000U;
    feedMinutes(l, t, 30, true, 12.34f, 37.5f); feedMinutes(l, t, 30, true, 15.0f, 37.5f);
    const Record r = l.pack(7U);
    assert(Ledger::valid(r));
    Ledger m; assert(m.unpack(r) && m.count() == 2U);
    assert(std::fabs(m.checkpoint(0).hold - 12.34f) < 0.01f && m.checkpoint(1).endS == l.checkpoint(1).endS && std::fabs(m.checkpoint(1).sp - 37.5f) < 0.01f);
    uint8_t raw[sizeof(Record)]; std::memcpy(raw, &r, sizeof r);
    int caught = 0;
    for (size_t b = 0; b < sizeof raw * 8U; ++b) {
      uint8_t cp[sizeof(Record)]; std::memcpy(cp, raw, sizeof raw); cp[b / 8U] ^= static_cast<uint8_t>(1U << (b % 8U));
      Record bad; std::memcpy(&bad, cp, sizeof bad);
      if (!Ledger::valid(bad)) ++caught;
    }
    assert(caught == static_cast<int>(sizeof raw * 8U));
    Record empty; std::memset(&empty, 0xFF, sizeof empty); assert(!Ledger::valid(empty));
    Record zero; std::memset(&zero, 0, sizeof zero); assert(!Ledger::valid(zero));
  }
  // 7. hostile input
  {
    Ledger l; uint32_t t = 0U;
    l.feed(t, true, NAN, 37.5f); l.feed(t, true, 10.0f, NAN); l.feed(t, true, INFINITY, 37.5f);
    assert(l.count() == 0U);
    feedMinutes(l, t, 30, true, 500.0f, 37.5f);                 // an absurd level is clamped
    assert(l.count() == 1U && l.checkpoint(0).hold <= HOLD_MAX_PCT);
  }
  std::printf("thermal-load-ledger: OK\n");
  return 0;
}
