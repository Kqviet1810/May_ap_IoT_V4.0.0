// The record of what the thermal controller measured about the oven (heater gain, transit time), kept in the external EEPROM: the same code MachineController runs
// (thermal_gain_record.h) against a two-slot memory that can lose power in the middle of a write; and the controller's restore() that reads it.
//   g++ -std=c++11 -Wall -Wextra -Werror tests/thermal-gain-record.cpp -o /tmp/mayap-thermal-gain-record && /tmp/mayap-thermal-gain-record
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_gain_record.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h"

using namespace thermalgain;

// Two slots with the semantics of PersistentStore::saveThermalGain / loadThermalGain (readRecord / writeRecord are replaced by array copies).
struct Chip {
  Record slot[2];
  bool readable[2] = {true, true};
  Chip() { std::memset(slot, 0, sizeof(slot)); }
  bool load(Record &best) const { return pick(slot[0], readable[0], slot[1], readable[1], best) != 0U; }
  // `cutAfter` bytes reach the chip before the power goes (sizeof(Record) = a complete write)
  void save(float kAir, float tau, size_t cutAfter = sizeof(Record)) {
    bool toA = true;
    const Record rec = nextToWrite(slot[0], readable[0], slot[1], readable[1], kAir, tau, toA);
    std::memcpy(&slot[toA ? 0 : 1], &rec, cutAfter);
  }
};

static void testRecordBasics() {
  std::printf("[TEST] record: CRC, limits, newest of two slots, wrap-around...\n");
  Record r = make(7U, 1.0e-4f, 150.0f);
  assert(valid(r));
  Record t = r; t.kAir = 1.1e-4f; assert(!valid(t));                        // a flipped value
  t = r; t.sequence = 8U; assert(!valid(t));                                // a flipped sequence
  t = r; t.crc ^= 1U; assert(!valid(t));
  assert(!valid(make(1U, NAN, 150.0f)));
  assert(!valid(make(1U, KAIR_MIN * 0.5f, 150.0f)) && !valid(make(1U, KAIR_MAX * 2.0f, 150.0f)));
  assert(!valid(make(1U, 1.0e-4f, 20.0f)) && !valid(make(1U, 1.0e-4f, 1000.0f)));
  assert(valid(make(1U, 1.0e-4f, 0.0f)));                                   // tau 0 = never measured
  Record blank{}; assert(!valid(blank));
  Record a = make(5U, 1.0e-4f, 150.0f), b = make(6U, 2.0e-4f, 140.0f), out{};
  assert(pick(a, true, b, true, out) == 2U && out.kAir == 2.0e-4f);
  assert(pick(a, true, b, false, out) == 1U && out.kAir == 1.0e-4f);        // B unreadable
  Record bad = b; bad.crc ^= 4U;
  assert(pick(a, true, bad, true, out) == 1U);                              // B corrupt
  assert(pick(blank, true, blank, true, out) == 0U);
  assert(pick(a, false, b, false, out) == 0U);
  Record wa = make(0xFFFFFFFFUL, 1.0e-4f, 0.0f), wb = make(0U, 2.0e-4f, 0.0f);   // the sequence wrapped: 0 is newer than 0xFFFFFFFF
  assert(pick(wa, true, wb, true, out) == 2U);
  std::printf("  -> Pass\n");
}

static void testSaveAlternatesAndSurvivesPowerCuts() {
  std::printf("[TEST] save: alternates the slots, a power cut in the middle of a write loses nothing already kept...\n");
  Chip chip;
  Record got{};
  assert(!chip.load(got));                                                  // a new chip: nothing kept
  chip.save(1.0e-4f, 150.0f);
  assert(chip.load(got) && got.kAir == 1.0e-4f && got.sequence == 1U);
  chip.save(1.2e-4f, 148.0f);
  assert(chip.load(got) && got.kAir == 1.2e-4f && got.sequence == 2U);
  assert(valid(chip.slot[0]) && valid(chip.slot[1]));                       // both slots in use, one each
  chip.save(1.3e-4f, 147.0f);
  assert(chip.load(got) && got.kAir == 1.3e-4f && got.sequence == 3U);
  // every possible cut of the next write: what was kept before is still there, or the new record is complete
  for (size_t cut = 0U; cut <= sizeof(Record); ++cut) {
    Chip c = chip;
    c.save(1.4e-4f, 146.0f, cut);
    Record r{};
    assert(c.load(r));
    if (cut == sizeof(Record)) assert(r.kAir == 1.4e-4f && r.sequence == 4U);
    else assert(r.kAir == 1.3e-4f && r.sequence == 3U);                     // the old record survives any partial write
  }
  // the chip cannot be read at all: nothing is returned, nothing invented
  Chip dead = chip; dead.readable[0] = dead.readable[1] = false;
  assert(!dead.load(got));
  std::printf("  -> Pass (all %u cuts of a write)\n", static_cast<unsigned>(sizeof(Record) + 1U));
}

static void testWhenToWrite() {
  std::printf("[TEST] when a write is wanted: only a MEASURED value that moved, at most every 10 minutes...\n");
  assert(!wanted(false, false, 0.0f, 0.0f, 1.0e-4f, 150.0f, 3600000UL));    // restored / typed value: never written back
  assert(!wanted(true, false, 0.0f, 0.0f, 1.0e-4f, 150.0f, 599999UL));      // too soon
  assert(wanted(true, false, 0.0f, 0.0f, 1.0e-4f, 150.0f, 600000UL));       // first measurement
  assert(!wanted(true, true, 1.00e-4f, 150.0f, 1.02e-4f, 152.0f, 3600000UL));   // 2 % and 2 s: unchanged
  assert(wanted(true, true, 1.00e-4f, 150.0f, 1.05e-4f, 150.0f, 3600000UL));    // kAir moved 5 %
  assert(wanted(true, true, 1.00e-4f, 150.0f, 1.00e-4f, 160.0f, 3600000UL));    // tau moved 10 s
  assert(!wanted(true, true, 1.00e-4f, 150.0f, 1.00e-4f, 0.0f, 3600000UL));     // tau unknown: not a change
  assert(!wanted(true, false, 0.0f, 0.0f, NAN, 150.0f, 3600000UL));
  assert(!wanted(true, false, 0.0f, 0.0f, KAIR_MAX * 2.0f, 150.0f, 3600000UL));
  // at the most one write per GAIN_SAVE_MIN_MS: a day of 10-minute ticks with a value that keeps moving is 144 writes, the absolute ceiling
  std::printf("  -> Pass (ceiling %u writes a day)\n", static_cast<unsigned>(86400000UL / GAIN_SAVE_MIN_MS));
}

static void testManualLevels() {
  std::printf("[TEST] manual levels: automatic = 0, then increasing, all inside what the controller accepts...\n");
  assert(manualKAir(0U) == 0.0f);
  float prev = 0.0f;
  for (uint8_t i = 1U; i < MANUAL_LEVELS; ++i) {
    const float k = manualKAir(i);
    assert(k > prev && k >= KAIR_MIN && k <= KAIR_MAX);
    prev = k;
  }
  assert(manualKAir(MANUAL_LEVELS) == 0.0f && manualKAir(255U) == 0.0f);
  std::printf("  -> Pass (%.1e .. %.1e)\n", static_cast<double>(manualKAir(1U)), static_cast<double>(manualKAir(MANUAL_LEVELS - 1U)));
}

static void testControllerLimitsAgree() {
  std::printf("[TEST] the record's limits are the controller's own, and restore() takes exactly what the record allows...\n");
  assert(KAIR_MIN == thermalv3::KAIR_PRIOR * thermalv3::KAIR_LO && KAIR_MAX == thermalv3::KAIR_PRIOR * thermalv3::KAIR_HI);
  assert(TAU_MIN == thermalv3::TAU_PRIOR * thermalv3::TAU_LO && TAU_MAX == thermalv3::TAU_PRIOR * thermalv3::TAU_HI);
  thermalv3::Controller c;
  const float prior = c.kAir(), tauPrior = c.tauTransit();
  c.restore(NAN, 0.0f); assert(c.kAir() == prior && !c.identified());
  c.restore(KAIR_MAX * 2.0f, 9999.0f); assert(c.kAir() == prior && c.tauTransit() == tauPrior && !c.identified());
  c.restore(1.0e-4f, 0.0f);                                                // kAir only: the transit time stays the prior
  assert(c.kAir() == 1.0e-4f && c.identified() && c.tauTransit() == tauPrior && !c.kAirMeasured() && !c.tauMeasured());
  c.restore(1.0e-4f, 120.0f); assert(c.tauTransit() == 120.0f && !c.tauMeasured());   // a prior only: the next clean climb measures it again
  c.reset(); assert(c.kAir() == prior && !c.identified());
  std::printf("  -> Pass\n");
}

int main() {
  std::printf("=== Thermal gain record ===\n");
  testRecordBasics();
  testSaveAlternatesAndSurvivesPowerCuts();
  testWhenToWrite();
  testManualLevels();
  testControllerLimitsAgree();
  std::printf("THERMAL GAIN RECORD: ALL PASSED\n");
  return 0;
}
