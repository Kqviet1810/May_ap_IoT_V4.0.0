// The hold level kept across a restart: CRC, two alternating slots with a power cut at every byte of a write, when a write is wanted, and when (and how much of) the level is given back.
//   g++ -std=c++11 -Wall -Wextra -Werror tests/thermal-hold-record.cpp -o /tmp/mayap-thermal-hold-record && /tmp/mayap-thermal-hold-record
// Then the same controller code in the closed-loop rig of the log of 05/11: a restart at noon starting at 12 % against starting at the kept level.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_hold_record.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h"

using namespace thermalhold;

struct Chip {
  Record slot[2];
  bool readable[2] = {true, true};
  Chip() { std::memset(slot, 0, sizeof(slot)); }
  bool load(Record &best) const { return pick(slot[0], readable[0], slot[1], readable[1], best) != 0U; }
  void save(float hold, uint32_t batch, size_t cutAfter = sizeof(Record)) {
    bool toA = true;
    const Record rec = nextToWrite(slot[0], readable[0], slot[1], readable[1], hold, 37.5f, 37.5f, 1000000UL, batch, 12.5f, 90.0f, toA);
    std::memcpy(&slot[toA ? 0 : 1], &rec, cutAfter);
  }
};

static void testRecord() {
  std::printf("[TEST] record: CRC, limits, newest of two slots, wrap-around...\n");
  Record r = make(7U, 3.0f, 37.5f, 37.5f, 1000U, 55U);
  assert(valid(r));
  Record t = r; t.hold = 3.1f; assert(!valid(t));
  t = r; t.sequence = 8U; assert(!valid(t));
  assert(!valid(make(1U, NAN, 37.5f, 37.5f, 1U, 5U)) && !valid(make(1U, -1.0f, 37.5f, 37.5f, 1U, 5U)) && !valid(make(1U, 101.0f, 37.5f, 37.5f, 1U, 5U)));
  assert(!valid(make(1U, 3.0f, 10.0f, 37.5f, 1U, 5U)) && !valid(make(1U, 3.0f, 37.5f, 60.0f, 1U, 5U)) && !valid(make(1U, 3.0f, 37.5f, 37.5f, 1U, 0U)));
  Record blank{}; assert(!valid(blank));
  // the compensation of the fan rides in the record: kept, limited, and part of the CRC
  Record f = make(3U, 20.0f, 37.5f, 37.5f, 1000U, 55U, 18.5f, 75.0f);
  assert(valid(f) && f.fanComp == 18.5f && f.fanLead == 75.0f);
  t = f; t.fanComp = 19.0f; assert(!valid(t));
  assert(!valid(make(1U, 3.0f, 37.5f, 37.5f, 1U, 5U, -1.0f, 50.0f)) && !valid(make(1U, 3.0f, 37.5f, 37.5f, 1U, 5U, 41.0f, 50.0f)) && !valid(make(1U, 3.0f, 37.5f, 37.5f, 1U, 5U, NAN, 50.0f)));
  assert(!valid(make(1U, 3.0f, 37.5f, 37.5f, 1U, 5U, 10.0f, -1.0f)) && !valid(make(1U, 3.0f, 37.5f, 37.5f, 1U, 5U, 10.0f, 361.0f)));
  t = f; t.magic = 0x4D4B4831UL; t.crc = recordCrc(t); assert(!valid(t));            // a record of the 2.9 layout is never read
  Record a = make(5U, 1.0f, 37.5f, 37.5f, 1U, 9U), b = make(6U, 2.0f, 37.5f, 37.5f, 1U, 9U), out{};
  assert(pick(a, true, b, true, out) == 2U && out.hold == 2.0f);
  assert(pick(a, true, b, false, out) == 1U);
  assert(pick(blank, true, blank, true, out) == 0U);
  Record wa = make(0xFFFFFFFFUL, 1.0f, 37.5f, 37.5f, 1U, 9U), wb = make(0U, 2.0f, 37.5f, 37.5f, 1U, 9U);
  assert(pick(wa, true, wb, true, out) == 2U);
  std::printf("  -> Pass\n");
}

static void testFanWrite() {
  std::printf("[TEST] write rule: the compensation of the fan alone is worth a write when it moved...\n");
  const uint32_t ok = SETTLED_MIN_MS, gap = SAVE_MIN_MS;
  assert(!wanted(ok, gap, true, 20.0f, 9U, 20.0f, 9U, 10.0f, 10.2f));
  assert(wanted(ok, gap, true, 20.0f, 9U, 20.0f, 9U, 10.0f, 10.0f + SAVE_MOVE_PCT));
  assert(wanted(ok, gap, true, 20.0f, 9U, 20.0f, 9U, 10.0f, 9.0f));
  assert(!wanted(ok, gap - 1U, true, 20.0f, 9U, 20.0f, 9U, 10.0f, 15.0f));            // never more often than every 10 minutes
  std::printf("  -> Pass\n");
}

static void testPowerCuts() {
  std::printf("[TEST] save: alternates the slots, a power cut in the middle of a write loses nothing already kept...\n");
  Chip chip; Record got{};
  assert(!chip.load(got));
  chip.save(8.0f, 9U); assert(chip.load(got) && got.hold == 8.0f && got.sequence == 1U);
  chip.save(6.0f, 9U); assert(chip.load(got) && got.hold == 6.0f && got.sequence == 2U);
  assert(valid(chip.slot[0]) && valid(chip.slot[1]));
  chip.save(5.0f, 9U); assert(chip.load(got) && got.hold == 5.0f && got.sequence == 3U);
  for (size_t cut = 0U; cut <= sizeof(Record); ++cut) {
    Chip c = chip; c.save(4.0f, 9U, cut); Record r{};
    assert(c.load(r));
    if (cut == sizeof(Record)) assert(r.hold == 4.0f && r.sequence == 4U); else assert(r.hold == 5.0f && r.sequence == 3U);
  }
  Chip dead = chip; dead.readable[0] = dead.readable[1] = false; assert(!dead.load(got));
  std::printf("  -> Pass (all %u cuts of a write)\n", static_cast<unsigned>(sizeof(Record) + 1U));
}

static void testWhenToWrite() {
  std::printf("[TEST] when a write is wanted: settled 10 minutes, at most every 10 minutes, a moved level or another batch...\n");
  assert(!wanted(599999UL, 900000UL, false, 0.0f, 0U, 5.0f, 9U));            // not settled long enough
  assert(!wanted(900000UL, 599999UL, false, 0.0f, 0U, 5.0f, 9U));            // too soon after the last write
  assert(wanted(900000UL, 600000UL, false, 0.0f, 0U, 5.0f, 9U));             // first
  assert(!wanted(900000UL, 900000UL, true, 5.0f, 9U, 5.4f, 9U));             // 0.4 points: unchanged
  assert(wanted(900000UL, 900000UL, true, 5.0f, 9U, 5.5f, 9U));              // 0.5 points
  assert(wanted(900000UL, 900000UL, true, 5.0f, 8U, 5.0f, 9U));              // another batch
  assert(!wanted(900000UL, 900000UL, false, 0.0f, 0U, NAN, 9U) && !wanted(900000UL, 900000UL, false, 0.0f, 0U, 5.0f, 0U));
  std::printf("  -> Pass (ceiling %u writes a day)\n", static_cast<unsigned>(86400000UL / SAVE_MIN_MS));
}

static void testRestore() {
  std::printf("[TEST] restore: only a short stop of the same batch with the oven where it was; 0.9 of the level...\n");
  const Record r = make(3U, 5.0f, 37.52f, 37.5f, 100000UL, 77U);
  auto go = [&](bool ok, uint32_t batch, bool rtc, uint32_t now, float pv, float sp) { return restored(r, ok, batch, rtc, now, pv, sp); };
  assert(std::fabs(go(true, 77U, true, 100000UL + 600UL, 37.50f, 37.5f) - 4.5f) < 1.0e-5f);   // 10 minutes later, same oven: 0.9 x 5
  assert(go(false, 77U, true, 100600UL, 37.5f, 37.5f) < 0.0f);                                // no valid record
  assert(go(true, 78U, true, 100600UL, 37.5f, 37.5f) < 0.0f);                                 // another batch
  assert(go(true, 77U, true, 100000UL + 1801UL, 37.5f, 37.5f) < 0.0f);                        // stopped too long
  assert(go(true, 77U, true, 99000UL, 37.5f, 37.5f) < 0.0f);                                  // the clock went back
  assert(go(true, 77U, true, 100600UL, 37.5f, 37.6f) < 0.0f);                                 // another set-point
  assert(go(true, 77U, true, 100600UL, 36.9f, 37.5f) < 0.0f);                                 // the oven cooled (far from SV)
  assert(go(true, 77U, true, 100600UL, 38.1f, 37.5f) < 0.0f);                                 // ... or is far over
  assert(go(true, 77U, false, 0UL, 37.45f, 37.5f) > 0.0f);                                    // no clock: the PV decides (it is where it was)
  const Record far = make(3U, 5.0f, 36.9f, 37.5f, 0U, 77U);
  assert(restored(far, true, 77U, false, 0UL, 37.5f, 37.5f) < 0.0f);                          // no clock and the PV moved more than 0.5: refused
  assert(restored(make(1U, 40.0f, 37.5f, 37.5f, 1U, 5U), true, 5U, false, 0UL, 37.5f, 37.5f) <= HOLD_MAX_PCT);
  std::printf("  -> Pass\n");
}

// The controller: a restart on a hot noon starting at the kept level instead of 12 % (the rig of the log of 05/11, one oven, noise-free).
static double restartOver(float startHold, double room) {
  thermalv3::Controller c;
  if (startHold >= 0.0f) c.restoreHold(startHold);
  const double dt = 0.1, sp = 37.5, ua = 220.0, cf = 1.3e6, pmax = 16000.0, delay = 110.0;
  double air = sp, heaterW = ua * (sp - room), maxOver = -9.0;
  std::vector<double> pipe(static_cast<size_t>(delay / dt), heaterW);
  size_t idx = 0; double acc = 0.0;
  for (long i = 0; i < 40L * 60L * 10L; ++i) {
    const double t = i * dt;
    if (std::fmod(t, 1.0) < dt) {
      thermalv3::Input in; in.pv = static_cast<float>(std::lround(air * 10.0) / 10.0); in.sp = static_cast<float>(sp); in.dtS = 1.0f; in.permit = true; in.capPct = 100.0f;
      acc = c.step(in).powerPct;
    }
    const double p = acc / 100.0 * pmax, delayed = pipe[idx]; pipe[idx] = p; idx = (idx + 1) % pipe.size();
    heaterW += (delayed - heaterW) * dt / 40.0;
    air += (heaterW - ua * (air - room)) / cf * dt;
    if (t > 120.0) maxOver = std::max(maxOver, air - sp);
  }
  return maxOver;
}
static void testControllerRestart() {
  std::printf("[TEST] controller: a restart at noon starts at the kept level instead of 12 %%...\n");
  const double over12 = restartOver(-1.0f, 36.0), overKept = restartOver(2.0f, 36.0);       // the oven needs about 2 %
  std::printf("  hold 12 %% start: +%.2f degC over SV, kept 2 %%: +%.2f\n", over12, overKept);
  assert(overKept < over12 && overKept < 0.10);
  thermalv3::Controller c; const float before = c.pHold();
  c.restoreHold(NAN); assert(c.pHold() == before);                // refused: not a number
  c.restoreHold(-1.0f); assert(c.pHold() == before);
  c.restoreHold(101.0f); assert(c.pHold() == before);             // more than the heater can give
  c.restoreHold(4.5f); assert(std::fabs(c.pHold() - 4.5f) < 1.0e-6f);
  std::printf("  -> Pass\n");
}

int main() {
  std::printf("=== Thermal hold record ===\n");
  testRecord();
  testFanWrite();
  testPowerCuts();
  testWhenToWrite();
  testRestore();
  testControllerRestart();
  std::printf("THERMAL HOLD RECORD: ALL PASSED\n");
  return 0;
}
