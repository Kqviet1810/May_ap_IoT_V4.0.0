// Host test of hold_keep.h: the record, the ring (torn write, wrap, clear, failures), when to write, when to use, the one-time seed, and the wear in 20 years.
#include <cstdio>
#include <vector>
#include "../MAYAP_INDUSTRIAL_v1_0_0/hold_keep.h"
using namespace HoldKeep;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::printf("FAIL %d: %s -- ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Fake {
  std::vector<uint8_t> mem = std::vector<uint8_t>(65536, 0xFF);
  std::vector<uint32_t> cellWrites = std::vector<uint32_t>(65536, 0);
  bool failWrites = false, failReads = false; int tearAfter = -1;       // tearAfter: bytes of the next write that land, then the "power" goes
  bool read(uint16_t a, void *d, size_t n) { if (failReads) return false; memcpy(d, &mem[a], n); return true; }
  bool write(uint16_t a, const void *s, size_t n) {
    if (failWrites) return false;
    size_t k = tearAfter >= 0 ? static_cast<size_t>(tearAfter) : n; tearAfter = -1;
    memcpy(&mem[a], s, k); for (size_t i = 0; i < k; ++i) ++cellWrites[a + i];
    return true;
  }
};
static Record rec(float hold, uint32_t epoch, float sp = 37.5f, uint8_t cap = 100) { Record r; r.hold = hold; r.epoch = epoch; r.sp = sp; r.cap = cap; return r; }

int main() {
  // record round trip, bad CRC, wrong magic
  { uint8_t b[16]; Record r = rec(43.4f, 1700000000U, 37.5f, 90); r.seq = 77; encode(r, b); Record o;
    CHECK(decode(b, o) && o.seq == 77 && o.epoch == 1700000000U && o.cap == 90 && fabsf(o.hold - 43.4f) < 0.06f && fabsf(o.sp - 37.5f) < 0.006f, "round trip");
    b[11] ^= 1; CHECK(!decode(b, o), "flipped bit must fail"); b[11] ^= 1; b[0] = 0xFF; CHECK(!decode(b, o), "blank magic must fail"); }
  // blank EEPROM, first append, restart finds it, torn write falls back to the previous record
  { Fake f; Ring<Fake> r(f, 0x4000);
    CHECK(r.begin() && !r.hasRecord(), "blank ring has no record");
    CHECK(r.append(rec(40.0f, 1000)) && r.append(rec(41.0f, 1300)), "two writes");
    Ring<Fake> r2(f, 0x4000); CHECK(r2.begin() && r2.hasRecord() && r2.latest().seq == 2 && fabsf(r2.latest().hold - 41.0f) < 0.06f, "newest record found after a restart");
    f.tearAfter = 9; CHECK(r2.append(rec(42.0f, 1600)), "torn write");
    Ring<Fake> r3(f, 0x4000); CHECK(r3.begin() && r3.latest().seq == 2, "a torn record is ignored, the previous one stands (seq %u)", (unsigned)r3.latest().seq);
    CHECK(r3.append(rec(43.0f, 1900)), "the next write goes on after the torn slot"); Ring<Fake> r4(f, 0x4000); CHECK(r4.begin() && fabsf(r4.latest().hold - 43.0f) < 0.06f, "and is the newest");
    CHECK(r4.clear() && !r4.hasRecord(), "clear"); Ring<Fake> r5(f, 0x4000); CHECK(r5.begin() && !r5.hasRecord(), "nothing left after clear"); }
  // wrap: 1300 writes, the newest wins, the sequence passes the ring several times
  { Fake f; Ring<Fake> r(f, 0x4000); r.begin(); for (int i = 1; i <= 1300; ++i) r.append(rec(10.0f + (i % 50), 1000U + i));
    Ring<Fake> r2(f, 0x4000); CHECK(r2.begin() && r2.latest().seq == 1300 && r2.latest().epoch == 2300U, "wrap: newest after 1300 writes (seq %u)", (unsigned)r2.latest().seq); }
  // failures: three in a row switch the ring off quietly
  { Fake f; Ring<Fake> r(f, 0x4000); r.begin(); f.failWrites = true;
    CHECK(!r.append(rec(40, 1)) && r.ready(), "one failure is not the end"); CHECK(!r.append(rec(40, 2)) && r.ready(), "two"); CHECK(!r.append(rec(40, 3)) && !r.ready(), "three: off");
    f.failWrites = false; CHECK(!r.append(rec(40, 4)), "stays off until the next start"); Fake g; g.failReads = true; Ring<Fake> q(g, 0x4000); CHECK(!q.begin() && !q.ready(), "unreadable chip: off"); }
  // when to write
  {
    Keeper k;
    auto quiet = [&](int n, float e, bool door, bool fan, bool idle, float hold, bool fault, bool rtc) { for (int i = 0; i < n; ++i) k.tick(e, door, fan, idle, hold, fault, rtc); };
    quiet(599, 0.05f, false, false, true, 43.0f, false, true); CHECK(!k.steady(), "599 s is not steady");
    quiet(1, 0.05f, false, false, true, 43.0f, false, true); CHECK(k.steady(), "600 s is steady");
    quiet(1, 0.2f, false, false, true, 43.0f, false, true); CHECK(!k.steady(), "error over 0.15 stops it");
    quiet(600, 0.0f, false, false, true, 43.0f, false, true); quiet(1, 0.0f, false, true, true, 43.0f, false, true); CHECK(!k.steady(), "fan");
    quiet(600, 0.0f, false, false, true, 43.0f, false, true); quiet(1, 0.0f, true, false, true, 43.0f, false, true); CHECK(!k.steady(), "door");
    quiet(600, 0.0f, false, false, false, 43.0f, false, true); CHECK(!k.steady(), "judge not idle");
    quiet(600, 0.0f, false, false, true, 43.0f, true, true); CHECK(!k.steady(), "heater fault");
    quiet(600, 0.0f, false, false, true, 43.0f, false, false); CHECK(!k.steady(), "rtc invalid");
    for (int i = 0; i < 700; ++i) k.tick(0.0f, false, false, true, 43.0f + static_cast<float>(i / 100) * 1.5f, false, true);
    CHECK(!k.steady(), "hold moving 1.5 points each 100 s never settles");
  }
  // due(): every 5 minutes, planned events after a minute, never when not steady, 400 a day
  { Keeper k; for (int i = 0; i < 700; ++i) k.tick(0.0f, false, false, true, 43.0f, false, true);
    CHECK(k.due(1000, false), "first record"); k.saved(1000); CHECK(!k.due(1100, false), "not before 5 min"); CHECK(!k.due(1030, true), "not even planned before a minute"); CHECK(k.due(1100, true), "planned after a minute");
    CHECK(k.due(1300, false), "periodic after 5 min"); Keeper n; CHECK(!n.due(5000, false), "not steady, no write");
    Keeper c; for (int i = 0; i < 700; ++i) c.tick(0.0f, false, false, true, 43.0f, false, true); uint32_t now = 0; int writes = 0;
    for (int i = 0; i < 2000; ++i) { now += 61; c.tick(0.0f, false, false, true, 43.0f, false, true); if (now - 0 < 86400U && c.due(now, true)) { c.saved(now); ++writes; } }
    CHECK(writes <= (int)MAX_PER_DAY, "cap per 24 h: %d writes", writes); }
  // restore: each rule
  { Record r = rec(44.0f, 10000); float h = 0; const uint32_t now = 10000 + 600;
    CHECK(decideRestore(true, r, true, now, 37.5f, 100, h) == Why::Ok && h > 43.9f, "10 min old: used");
    CHECK(decideRestore(false, r, true, now, 37.5f, 100, h) == Why::NoRecord, "no record");
    CHECK(decideRestore(true, r, false, now, 37.5f, 100, h) == Why::RtcInvalid, "rtc invalid");
    CHECK(decideRestore(true, r, true, 10000 + 1201, 37.5f, 100, h) == Why::TooOld, "21 min old: not used"); CHECK(decideRestore(true, r, true, 10000 + 1200, 37.5f, 100, h) == Why::Ok, "exactly 20 min: used");
    CHECK(decideRestore(true, r, true, 9000, 37.5f, 100, h) == Why::Future, "clock went back");
    CHECK(decideRestore(true, r, true, now, 37.9f, 100, h) == Why::SpChanged, "set point moved 0.4"); CHECK(decideRestore(true, r, true, now, 37.7f, 100, h) == Why::Ok, "set point moved 0.2 is fine");
    CHECK(decideRestore(true, r, true, now, 37.5f, 80, h) == Why::CapChanged, "ceiling changed");
    CHECK(decideRestore(true, rec(95.0f, 10000), true, now, 37.5f, 100, h) == Why::Range, "hold out of range"); CHECK(decideRestore(true, rec(1.0f, 10000), true, now, 37.5f, 100, h) == Why::Range, "hold too small"); }
  // 20 years at the real rate (every 5 min) and at the cap (400 a day): the busiest cell
  { for (int mode = 0; mode < 2; ++mode) { Fake f; Ring<Fake> r(f, 0x4000); r.begin(); const long writes = mode == 0 ? 20L * 365L * 288L : 20L * 365L * 400L;
      for (long i = 0; i < writes; ++i) r.append(rec(43.0f, 1000000U + (uint32_t)i));
      uint32_t mx = 0; for (uint32_t c : f.cellWrites) mx = c > mx ? c : mx;
      std::printf("20 years %s: %ld writes, busiest cell %u writes (rated 100,000 cautious / 1,000,000 typical)\n", mode == 0 ? "every 5 min" : "at the 400/day cap", writes, mx);
      CHECK(mx <= 6000U, "wear of the busiest cell %u", mx); } }
  if (failures == 0) std::printf("HOLD KEEP: ALL PASSED\n");
  return failures;
}
