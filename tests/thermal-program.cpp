// Incubation thermal programme and periodic egg cooling: host tests of the design-only modules
// (MAYAP_INDUSTRIAL_v1_0_0/thermal_program.h, egg_cooling.h). Nothing here touches the firmware control path.
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_program.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/egg_cooling.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <cstring>

using namespace MayapProgram;

static int checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

static Program program(std::initializer_list<Stage> st) {
  Program p; p.enabled = true; p.count = 0;
  for (const Stage &s : st) p.stages[p.count++] = s;
  return p;
}

static void programTests() {
  Limits lim; const float high = 38.2f;
  // default: nothing is shipped, nothing is active, the set point passes through bit for bit
  { SetpointProgram sp; sp.configure(Program{}, lim, high); CHECK(!sp.active());
    CHECK(sp.update(1000, 5, 37.5f) == 37.5f); CHECK(sp.update(9999999, 12, 37.123f) == 37.123f); }
  // validation: each rule has a rejecting case
  CHECK(validate(program({{1, 375}}), lim, high) == Reject::None);
  CHECK(validate(program({}), lim, high) == Reject::Empty);
  CHECK(validate(program({{2, 375}}), lim, high) == Reject::FirstDay);
  CHECK(validate(program({{1, 375}, {1, 372}}), lim, high) == Reject::DayOrder);
  CHECK(validate(program({{1, 375}, {5, 370}, {4, 370}}), lim, high) == Reject::DayOrder);
  CHECK(validate(program({{1, 376}}), lim, high) == Reject::Range);          // 37.6 > 38.2 - 0.7
  CHECK(validate(program({{1, 299}}), lim, high) == Reject::Range);
  CHECK(validate(program({{1, 375}, {10, 368}}), lim, high) == Reject::Step); // 0.7 C step
  CHECK(validate(program({{1, 375}, {10, 370}}), lim, high) == Reject::None);
  CHECK(validate(program({{1, 375}}), lim, NAN) == Reject::NotFinite);
  { Program p = program({{1, 375}}); p.count = MaxStages + 1; CHECK(validate(p, lim, high) == Reject::TooMany); }
  // an invalid programme is not applied at all
  { SetpointProgram sp; sp.configure(program({{1, 399}}), lim, high); CHECK(!sp.active()); CHECK(sp.update(1000, 3, 37.5f) == 37.5f); }
  // slew: a 0.4 C stage change takes >= 48 min, never overshoots the target, never moves faster than the limit
  { SetpointProgram sp; sp.configure(program({{1, 375}, {4, 371}}), lim, high);
    uint32_t t = 1000; float prev = sp.update(t, 1, 37.5f); CHECK(std::fabs(prev - 37.5f) < 1e-4f);
    Event e; CHECK(sp.takeEvent(e) && e.stage == 0 && e.targetX10 == 375); CHECK(!sp.takeEvent(e));
    float worst = 0; bool reached = false; uint32_t reachedAt = 0;
    for (int i = 0; i < 4 * 3600; ++i) {                       // 4 h on day 4, 1 s steps
      t += 1000; const float v = sp.update(t, 4, 37.5f);
      worst = std::fmax(worst, std::fabs(v - prev)); prev = v;
      CHECK(v >= 37.1f - 1e-4f && v <= 37.5f + 1e-4f);
      if (!reached && std::fabs(v - 37.1f) < 1e-3f) { reached = true; reachedAt = static_cast<uint32_t>(i); }
    }
    CHECK(worst <= 0.5f / 3600.0f + 1e-6f);
    CHECK(reached && reachedAt >= 47 * 60 && reachedAt <= 49 * 60);
    CHECK(sp.takeEvent(e) && e.stage == 1 && e.targetX10 == 371); }
  // a stalled task is not a jump; NaN configured set point passes through; day 0 (no batch) restarts from the configured value
  { SetpointProgram sp; sp.configure(program({{1, 375}, {2, 371}}), lim, high);
    sp.update(1000, 2, 37.5f);
    const float v = sp.update(1000U + 3600U * 1000U, 2, 37.5f);        // 1 h silent gap
    CHECK(v >= 37.5f - 0.5f * 600.0f / 3600.0f - 1e-4f);               // dt is capped at 600 s
    CHECK(std::isnan(sp.update(5000000, 2, NAN)));
    CHECK(sp.update(6000000, 0, 36.0f) == 36.0f); }
  // millis() rollover
  { SetpointProgram sp; sp.configure(program({{1, 375}, {2, 371}}), lim, high);
    uint32_t t = 0xFFFFFFFFU - 5000U; sp.update(t, 2, 37.5f); float v = 0;
    for (int i = 0; i < 20; ++i) { t += 1000U; v = sp.update(t, 2, 37.5f); }
    CHECK(v < 37.5f && v > 37.4f); }
}

// ------------------------------------------------------------------------------------------------------------------------------
static CoolingPolicy policy() {
  CoolingPolicy p; p.enabled = true; p.firstDay = 3; p.lastDay = 6; p.periodMin = 240; p.durationMin = 20; p.maxDurationMin = 30;
  p.minTempX10 = 340; p.resume = Resume::Cancel; return p;
}
static CoolingInput inputAt(uint32_t batchStart, uint32_t secIntoBatch, float temp) {
  CoolingInput in; in.batchRunning = true; in.rtcValid = true; in.sensorValid = true; in.epoch = batchStart + secIntoBatch;
  in.batchStartEpoch = batchStart; in.tempC = temp; return in;
}

static void coolingTests() {
  const uint32_t B = 1800000000U, day = 86400U;
  // default OFF; an invalid policy is replaced by OFF
  { EggCooling c; CHECK(!c.policy().enabled);
    CHECK(!c.update(inputAt(B, 2 * day + 10, 37.5f)).heaterInhibit);
    CoolingPolicy bad = policy(); bad.minTempX10 = 250; c.configure(bad); CHECK(!c.policy().enabled);          // below the absolute floor
    bad = policy(); bad.maxDurationMin = 120; c.configure(bad); CHECK(!c.policy().enabled);
    bad = policy(); bad.periodMin = 30; c.configure(bad); CHECK(!c.policy().enabled);                           // period must exceed the guard
    bad = policy(); bad.lastDay = 40; c.configure(bad); CHECK(!c.policy().enabled);
    c.configure(policy()); CHECK(c.policy().enabled); }
  // schedule: outside the day window nothing; inside, one cooling window of exactly 20 min at the start of each 4 h slot
  { EggCooling c; c.configure(policy());
    CHECK(!c.update(inputAt(B, 1 * day + 10, 37.5f)).heaterInhibit);                        // day 2: before firstDay
    CHECK(!c.update(inputAt(B, 6 * day + 10, 37.5f)).heaterInhibit);                        // day 7: after lastDay
    c.update(inputAt(B, 2 * day - 60, 37.5f));                                              // arm the boot logic outside any window
    CoolingOutput o = c.update(inputAt(B, 2 * day + 0, 37.5f));                             // day 3 slot 12: starts
    CHECK(o.heaterInhibit && o.lowTempSuppress && o.phase == CoolPhase::Cooling);
    CHECK(c.update(inputAt(B, 2 * day + 19 * 60, 37.0f)).heaterInhibit);
    o = c.update(inputAt(B, 2 * day + 20 * 60, 36.5f));                                    // window over
    CHECK(!o.heaterInhibit && !o.lowTempSuppress && o.lastCancel == CancelReason::Done);
    CHECK(!c.update(inputAt(B, 2 * day + 21 * 60, 36.5f)).heaterInhibit);                  // not again in the same slot
    CHECK(!c.update(inputAt(B, 2 * day + 3 * 3600, 37.4f)).heaterInhibit);
    CHECK(c.update(inputAt(B, 2 * day + 4 * 3600, 37.4f)).heaterInhibit); }                // next slot
  // guards cancel at once and the slot is consumed (no restart in the same slot)
  struct G { const char *n; void (*mut)(CoolingInput &); CancelReason why; };
  static const G guards[] = {
    {"sensor", [](CoolingInput &i) { i.sensorValid = false; }, CancelReason::Sensor},
    {"nan", [](CoolingInput &i) { i.tempC = NAN; }, CancelReason::Sensor},
    {"rtc", [](CoolingInput &i) { i.rtcValid = false; }, CancelReason::Rtc},
    {"safety", [](CoolingInput &i) { i.safetyLatched = true; }, CancelReason::Safety},
    {"tune", [](CoolingInput &i) { i.autoTune = true; }, CancelReason::Tune},
    {"test", [](CoolingInput &i) { i.testMode = true; }, CancelReason::Test},
    {"batch", [](CoolingInput &i) { i.batchRunning = false; }, CancelReason::Batch},
  };
  for (const G &g : guards) {
    EggCooling c; c.configure(policy()); c.update(inputAt(B, 2 * day - 60, 37.5f));
    CHECK(c.update(inputAt(B, 2 * day + 5, 37.5f)).heaterInhibit);
    CoolingInput bad = inputAt(B, 2 * day + 60, 37.5f); g.mut(bad);
    CoolingOutput o = c.update(bad);
    CHECK(!o.heaterInhibit && !o.lowTempSuppress);
    CHECK(!c.update(inputAt(B, 2 * day + 120, 37.5f)).heaterInhibit);                      // consumed
    CHECK(c.update(inputAt(B, 2 * day + 4 * 3600 + 5, 37.5f)).heaterInhibit);              // next slot starts normally
    (void)g.why;
  }
  // minimum-temperature guard: cooling ends at 34.0 C and does not start when the chamber is already that cool
  { EggCooling c; c.configure(policy()); c.update(inputAt(B, 2 * day - 60, 37.5f));
    CHECK(c.update(inputAt(B, 2 * day + 5, 36.0f)).heaterInhibit);
    CoolingOutput o = c.update(inputAt(B, 2 * day + 300, 34.0f));
    CHECK(!o.heaterInhibit && o.lastCancel == CancelReason::MinTemp);
    CHECK(!c.update(inputAt(B, 2 * day + 400, 36.0f)).heaterInhibit);
    EggCooling d; d.configure(policy()); d.update(inputAt(B, 2 * day - 60, 34.1f));
    CHECK(!d.update(inputAt(B, 2 * day + 5, 34.1f)).heaterInhibit); }                       // 34.1 <= min + 0.2: not started
  // maximum-duration guard (the clock jumps forward inside the window)
  { CoolingPolicy p = policy(); p.durationMin = 30; p.maxDurationMin = 30; p.periodMin = 240;
    EggCooling c; c.configure(p); c.update(inputAt(B, 2 * day - 60, 37.5f));
    CHECK(c.update(inputAt(B, 2 * day + 5, 37.5f)).heaterInhibit);
    CoolingOutput o = c.update(inputAt(B, 2 * day + 5 + 30 * 60, 37.5f));
    CHECK(!o.heaterInhibit); }
  // RTC moving backwards ends the cycle
  { EggCooling c; c.configure(policy()); c.update(inputAt(B, 2 * day - 60, 37.5f));
    CHECK(c.update(inputAt(B, 2 * day + 100, 37.5f)).heaterInhibit);
    CHECK(!c.update(inputAt(B, 2 * day + 50, 37.5f)).heaterInhibit); }
  // power loss / reset: default policy cancels, a window that is open at boot is NOT started from scratch
  { EggCooling c; c.configure(policy()); c.update(inputAt(B, 2 * day - 60, 37.5f));
    c.update(inputAt(B, 2 * day + 5, 37.5f));
    const CoolingRecord rec = c.record(inputAt(B, 2 * day + 5, 37.5f)); CHECK(validRecord(rec) && rec.active == 1);
    EggCooling r; r.configure(policy()); r.restore(&rec, inputAt(B, 2 * day + 300, 37.0f));
    CHECK(!r.update(inputAt(B, 2 * day + 300, 37.0f)).heaterInhibit);
    CHECK(!r.update(inputAt(B, 2 * day + 400, 37.0f)).heaterInhibit);
    CHECK(r.update(inputAt(B, 2 * day + 4 * 3600 + 5, 37.4f)).heaterInhibit);               // next slot is normal
    EggCooling n; n.configure(policy());                                                    // no record at all, boot inside an open window
    CHECK(!n.update(inputAt(B, 2 * day + 300, 37.0f)).heaterInhibit); }
  // opt-in resume: only with a valid record, valid RTC, the same slot and time left; otherwise cancel
  { CoolingPolicy p = policy(); p.resume = Resume::ResumeRemaining;
    EggCooling c; c.configure(p); c.update(inputAt(B, 2 * day - 60, 37.5f)); c.update(inputAt(B, 2 * day + 5, 37.5f));
    const CoolingRecord rec = c.record(inputAt(B, 2 * day + 5, 37.5f));
    EggCooling r; r.configure(p); r.restore(&rec, inputAt(B, 2 * day + 300, 37.0f));
    CoolingOutput o = r.update(inputAt(B, 2 * day + 300, 37.0f));
    CHECK(o.heaterInhibit);                                                                 // resumed inside the approved window
    CHECK(!r.update(inputAt(B, 2 * day + 20 * 60, 37.0f)).heaterInhibit);                  // and it still ends at the window end
    EggCooling late; late.configure(p); late.restore(&rec, inputAt(B, 2 * day + 21 * 60, 37.0f));
    CHECK(!late.update(inputAt(B, 2 * day + 21 * 60, 37.0f)).heaterInhibit);                // window over: nothing
    CoolingInput noRtc = inputAt(B, 2 * day + 300, 37.0f); noRtc.rtcValid = false;
    EggCooling x; x.configure(p); x.restore(&rec, noRtc); CHECK(!x.update(noRtc).heaterInhibit);
    CoolingRecord torn = rec; torn.slot ^= 1;                                               // torn / corrupt record: CRC rejects it
    CHECK(!validRecord(torn));
    EggCooling y; y.configure(p); y.restore(&torn, inputAt(B, 2 * day + 300, 37.0f));
    CHECK(!y.update(inputAt(B, 2 * day + 300, 37.0f)).heaterInhibit);
    EggCooling z; z.configure(p); z.restore(&rec, inputAt(B, 2 * day + 300, 37.0f));        // a different slot than the record names
    CHECK(!z.update(inputAt(B, 2 * day + 4 * 3600 + 400, 37.0f)).heaterInhibit); }
  // fuzz: invariants hold for any input sequence
  { uint32_t s = 0x12345678U; auto rnd = [&s]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
    EggCooling c; CoolingPolicy p = policy(); p.resume = Resume::ResumeRemaining; c.configure(p);
    uint32_t t = 0; uint32_t coolingSteps = 0; bool seen[12] = {};
    for (int i = 0; i < 400000; ++i) {
      t += rnd() % 30U;
      CoolingInput in = inputAt(B, t * 7U % (8U * day), 33.0f + (rnd() % 800U) * 0.01f);
      in.batchRunning = (rnd() % 997U) != 0; in.rtcValid = (rnd() % 991U) != 0; in.sensorValid = (rnd() % 983U) != 0;
      in.safetyLatched = (rnd() % 971U) == 0; in.autoTune = (rnd() % 977U) == 0; in.testMode = (rnd() % 983U) == 0;
      if ((rnd() % 4001U) == 0) in.tempC = NAN;
      if ((rnd() % 5003U) == 0) { const CoolingRecord r = c.record(in); EggCooling n; n.configure(p); n.restore(&r, in); c = n; }
      const CoolingOutput o = c.update(in);
      seen[static_cast<uint8_t>(o.lastCancel)] = true;
      if (o.heaterInhibit) {
        ++coolingSteps;
        CHECK(o.phase == CoolPhase::Cooling && o.lowTempSuppress);
        CHECK(in.batchRunning && in.rtcValid && in.sensorValid && !in.safetyLatched && !in.autoTune && !in.testMode && std::isfinite(in.tempC));
        CHECK(in.tempC > p.minTempX10 * 0.1f);
        CHECK(o.coolingSec < static_cast<uint32_t>(p.maxDurationMin) * 60U);
      } else CHECK(!o.lowTempSuppress);
    }
    CHECK(coolingSteps > 2000);                                                              // the fuzz really cools
    CHECK(seen[static_cast<uint8_t>(CancelReason::MinTemp)] && seen[static_cast<uint8_t>(CancelReason::Rtc)] && seen[static_cast<uint8_t>(CancelReason::Sensor)] &&
          seen[static_cast<uint8_t>(CancelReason::Safety)] && seen[static_cast<uint8_t>(CancelReason::Done)]); }
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  programTests();
  coolingTests();
  std::printf("Thermal programme + egg cooling (design-only modules): %d checks PASS\n", checks);
  return 0;
}
