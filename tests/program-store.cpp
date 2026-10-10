// A/B persistence of the incubation programme, the cooling policy and the cooling state: torn writes at every byte, corruption,
// sequence wrap, unknown version, wear accounting. Design-only (nothing in the firmware uses it yet).
#include "../MAYAP_INDUSTRIAL_v1_0_0/program_store.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
using namespace MayapProgram;

static int checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

struct MemDev {
  uint8_t mem[256]; int failAfter = -1; unsigned long bytesWritten = 0;
  MemDev() { std::memset(mem, 0xFF, sizeof(mem)); }
  bool read(uint16_t a, void *p, uint16_t n) { if (a + n > sizeof(mem)) return false; std::memcpy(p, mem + a, n); return true; }
  bool write(uint16_t a, const void *p, uint16_t n) {          // power cut after failAfter bytes: the prefix lands, the call reports failure
    if (a + n > sizeof(mem)) return false;
    const uint8_t *s = static_cast<const uint8_t *>(p);
    for (uint16_t i = 0; i < n; ++i) { if (failAfter >= 0 && static_cast<int>(i) >= failAfter) return false; mem[a + i] = s[i]; ++bytesWritten; }
    return true;
  }
};

static Program prog(std::initializer_list<Stage> st) { Program p; p.enabled = true; for (const Stage &s : st) p.stages[p.count++] = s; return p; }

int main() {
  static_assert(sizeof(ProgramRecord) == 4 + 4 + 8 + 16 + 4 + 4 + 4, "ProgramRecord layout");
  static_assert(sizeof(PolicyRecord) == 4 + 4 + 6 + 2 + 2 + 4 + 4, "PolicyRecord layout");
  // ---- empty device: everything OFF ---------------------------------------------------------------
  { MemDev d; AbStore<ProgramRecord, MemDev> s(d, 0, 64, 0); ProgramRecord r; CHECK(!s.load(r)); }
  // ---- round trip, wear: unchanged content is not written -------------------------------------------
  { MemDev d; AbStore<ProgramRecord, MemDev> s(d, 0, 64, 0);
    const ProgramRecord a = toRecord(prog({{1, 375}, {10, 372}}), 42);
    CHECK(s.save(a, 1000) == SaveResult::Written); CHECK(s.save(a, 2000) == SaveResult::Unchanged); CHECK(s.writes() == 1);
    AbStore<ProgramRecord, MemDev> s2(d, 0, 64, 0); ProgramRecord r; CHECK(s2.load(r));
    const Program p = toProgram(r); CHECK(p.enabled && p.count == 2 && p.stages[1].targetX10 == 372 && r.confirmedBatchId == 42);
    ProgramRecord b = a; b.confirmedBatchId = 43; CHECK(s2.save(b, 3000) == SaveResult::Written && s2.writes() == 1);
    AbStore<ProgramRecord, MemDev> s3(d, 0, 64, 0); CHECK(s3.load(r) && r.confirmedBatchId == 43 && r.seq == 2); }
  // ---- torn write at EVERY byte: the previous record survives, never garbage ------------------------------------
  for (int cut = 0; cut < static_cast<int>(sizeof(ProgramRecord)); ++cut) {
    MemDev d; AbStore<ProgramRecord, MemDev> s(d, 0, 64, 0);
    const ProgramRecord a = toRecord(prog({{1, 375}}), 7), b = toRecord(prog({{1, 375}, {5, 372}}), 8);
    CHECK(s.save(a, 1000) == SaveResult::Written); CHECK(s.save(toRecord(prog({{1, 374}}), 7), 2000) == SaveResult::Written);   // A and B both populated
    d.failAfter = cut; const SaveResult res = s.save(b, 3000); d.failAfter = -1; CHECK(res == SaveResult::Failed);
    AbStore<ProgramRecord, MemDev> r2(d, 0, 64, 0); ProgramRecord got; CHECK(r2.load(got));
    CHECK(got.count == 1 && got.targetX10[0] == 374 && got.confirmedBatchId == 7);              // the last complete record, whole
  }
  // ---- both slots corrupted / unknown version => absent (defaults) -----------------------------------------------
  { MemDev d; AbStore<ProgramRecord, MemDev> s(d, 0, 64, 0); s.save(toRecord(prog({{1, 375}}), 1), 0); s.save(toRecord(prog({{1, 374}}), 1), 1);
    d.mem[10] ^= 0x40; d.mem[64 + 10] ^= 0x01; AbStore<ProgramRecord, MemDev> r(d, 0, 64, 0); ProgramRecord g; CHECK(!r.load(g));
    MemDev e; ProgramRecord v2 = toRecord(prog({{1, 375}}), 1); v2.version = 2; v2.seq = 5; v2.crc = recCrc(v2); std::memcpy(e.mem, &v2, sizeof(v2));
    AbStore<ProgramRecord, MemDev> r2(e, 0, 64, 0); CHECK(!r2.load(g)); }                         // a newer schema is not guessed at
  // ---- one slot corrupted: the other one is used ---------------------------------------------------------------------
  { MemDev d; AbStore<ProgramRecord, MemDev> s(d, 0, 64, 0); s.save(toRecord(prog({{1, 375}}), 1), 0); s.save(toRecord(prog({{1, 374}}), 1), 1);
    d.mem[20] ^= 0xFF; AbStore<ProgramRecord, MemDev> r(d, 0, 64, 0); ProgramRecord g; CHECK(r.load(g));
    CHECK(g.targetX10[0] == 375 || g.targetX10[0] == 374); }
  // ---- sequence wrap ----------------------------------------------------------------------------------------------------
  { MemDev d; ProgramRecord x = toRecord(prog({{1, 375}}), 1); x.seq = 0xFFFFFFFFU; x.crc = recCrc(x); std::memcpy(d.mem, &x, sizeof(x));
    AbStore<ProgramRecord, MemDev> s(d, 0, 64, 0); ProgramRecord g; CHECK(s.load(g) && g.seq == 0xFFFFFFFFU);
    CHECK(s.save(toRecord(prog({{1, 374}}), 1), 10) == SaveResult::Written);
    AbStore<ProgramRecord, MemDev> r(d, 0, 64, 0); CHECK(r.load(g) && g.seq == 0U && g.targetX10[0] == 374); }       // 0 is "newer" than 0xFFFFFFFF
  // ---- minimum interval bounds the wear ---------------------------------------------------------------------------
  { MemDev d; AbStore<ProgramRecord, MemDev> s(d, 0, 64, 60000UL);
    CHECK(s.save(toRecord(prog({{1, 375}}), 1), 1000) == SaveResult::Written);
    CHECK(s.save(toRecord(prog({{1, 374}}), 1), 2000) == SaveResult::TooSoon);
    CHECK(s.save(toRecord(prog({{1, 374}}), 1), 62000) == SaveResult::Written); CHECK(s.writes() == 2); }
  // ---- cooling policy and cooling state share the mechanism; an invalid stored policy turns OFF --------------------------------
  { MemDev d; AbStore<PolicyRecord, MemDev> s(d, 0, 64, 0);
    CoolingPolicy p; p.enabled = true; p.firstDay = 3; p.lastDay = 6; p.periodMin = 240; p.durationMin = 20; p.maxDurationMin = 30; p.minTempX10 = 340;
    CHECK(s.save(toRecord(p), 0) == SaveResult::Written);
    AbStore<PolicyRecord, MemDev> r(d, 0, 64, 0); PolicyRecord g; CHECK(r.load(g)); const CoolingPolicy q = toPolicy(g); CHECK(q.enabled && q.durationMin == 20 && q.resume == Resume::Cancel);
    p.minTempX10 = 250; CHECK(!toPolicy(toRecord(p)).enabled);                                  // below the absolute floor: stored but never trusted
    MemDev e; AbStore<PolicyRecord, MemDev> n(e, 0, 64, 0); CHECK(!n.load(g)); CHECK(!toPolicy(PolicyRecord{}).enabled); }
  { MemDev d; AbStore<CoolingRecord, MemDev> s(d, 0, 64, 0); CoolingRecord c; c.active = 1; c.slot = 5; c.slotStartEpoch = 1800000000U; c.coolStartEpoch = 1800000100U; seal(c);
    CHECK(s.save(c, 0) == SaveResult::Written); AbStore<CoolingRecord, MemDev> r(d, 0, 64, 0); CoolingRecord g; CHECK(r.load(g) && g.active == 1 && g.coolStartEpoch == 1800000100U && validRecord(g)); }
  // ---- a year of cooling: writes stay bounded (start + end of 6 cycles a day) -----------------------------------------------------
  { MemDev d; AbStore<CoolingRecord, MemDev> s(d, 0, 64, 0); unsigned n = 0;
    for (int day = 0; day < 28; ++day) for (int k = 0; k < 6; ++k) { CoolingRecord c; c.active = 1; c.slot = static_cast<uint16_t>(day * 6 + k); c.slotStartEpoch = 1800000000U + day * 86400U + k * 14400U; c.coolStartEpoch = c.slotStartEpoch + 3; seal(c);
      if (s.save(c, n++ * 1000U) == SaveResult::Written) { CoolingRecord e = c; e.active = 0; seal(e); s.save(e, n++ * 1000U); } }
    CHECK(s.writes() == 28U * 6U * 2U); }                                                         // 336 writes per batch of 28 days, 2 per cycle
  std::printf("Programme / cooling persistence (A/B, CRC, torn write at every byte, seq wrap): %d checks PASS\n", checks);
  return 0;
}
