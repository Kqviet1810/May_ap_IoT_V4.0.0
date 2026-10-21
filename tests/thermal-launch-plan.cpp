// Launch plan persistence (host): record validity, compatibility, wear policy, two alternating slots with read-back, power cut at every byte.
// The controller side (what is measured, when it is offered, how it is used) is covered by the closed-loop suites.
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_launch_plan.h"
#include <cassert>
#include <cstdio>

using namespace MayapThermal;

static unsigned checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); std::abort(); } } while (0)

static LaunchPlan goodPlan() {
  LaunchPlan p; p.valid = true; p.gain = 0.012f; p.delaySec = 150.0f; p.coastCal = 1.2f; p.calMeasured = true; p.holdPct = 20.0f;
  return p;
}

static void recordTests() {
  const LaunchPlanRecord r = makeLaunchPlanRecord(goodPlan(), 0x1234U, 1800000000U);
  CHECK(validLaunchPlanRecord(r));
  CHECK(r.gain == 0.012f && r.delaySec == 150.0f && r.calMeasured == 1U && r.holdPct == 20.0f);
  // a bit flip anywhere in the record is rejected (CRC covers everything before it; the CRC itself is checked against it)
  for (size_t b = 0; b < sizeof(r); ++b) {
    LaunchPlanRecord bad = r;
    reinterpret_cast<uint8_t *>(&bad)[b] ^= 1U;
    CHECK(!validLaunchPlanRecord(bad));
  }
  // out-of-range content with a correct CRC is rejected as well
  struct Case { float gain, delay, cal, hold; } cases[] = {
    {0.0f, 150, 1.2f, 20}, {0.0001f, 150, 1.2f, 20}, {0.9f, 150, 1.2f, 20}, {NAN, 150, 1.2f, 20},
    {0.012f, -1, 1.2f, 20}, {0.012f, 300, 1.2f, 20}, {0.012f, NAN, 1.2f, 20},
    {0.012f, 150, 0.1f, 20}, {0.012f, 150, 9.0f, 20}, {0.012f, 150, NAN, 20},
    {0.012f, 150, 1.2f, -1}, {0.012f, 150, 1.2f, 101}, {0.012f, 150, 1.2f, NAN}};
  for (const Case &c : cases) {
    LaunchPlan p = goodPlan(); p.gain = c.gain; p.delaySec = c.delay; p.coastCal = c.cal; p.holdPct = c.hold;
    CHECK(!validLaunchPlanRecord(makeLaunchPlanRecord(p, 1U, 0U)));
  }
  // a different layout / version is never read
  LaunchPlanRecord v = r; v.version = 2; v.crc = launchPlanCrc(v); CHECK(!validLaunchPlanRecord(v));
  LaunchPlanRecord s = r; s.size = 40; s.crc = launchPlanCrc(s); CHECK(!validLaunchPlanRecord(s));
  // hardware signature and abnormal reset
  CHECK(launchPlanCompatible(r, 0x1234U, false));
  CHECK(!launchPlanCompatible(r, 0x1235U, false));
  CHECK(!launchPlanCompatible(r, 0x1234U, true));
}

static void policyTests() {
  const LaunchPlanRecord stored = makeLaunchPlanRecord(goodPlan(), 7U, 0U);
  LaunchPlan p = goodPlan();
  CHECK(!shouldPersistLaunchPlan(makeLaunchPlanRecord(p, 7U, 0U), stored, true, MinIntervalMs));             // identical: no write
  CHECK(shouldPersistLaunchPlan(makeLaunchPlanRecord(p, 7U, 0U), stored, false, MinIntervalMs));             // nothing stored yet
  CHECK(!shouldPersistLaunchPlan(makeLaunchPlanRecord(p, 7U, 0U), stored, false, MinIntervalMs - 1U));       // wear bound
  CHECK(shouldPersistLaunchPlan(makeLaunchPlanRecord(p, 8U, 0U), stored, true, MinIntervalMs));              // other hardware
  p.gain = 0.012f * 1.05f; CHECK(!shouldPersistLaunchPlan(makeLaunchPlanRecord(p, 7U, 0U), stored, true, MinIntervalMs));
  p.gain = 0.012f * 1.2f;  CHECK(shouldPersistLaunchPlan(makeLaunchPlanRecord(p, 7U, 0U), stored, true, MinIntervalMs));
  p = goodPlan(); p.delaySec = 165.0f; CHECK(shouldPersistLaunchPlan(makeLaunchPlanRecord(p, 7U, 0U), stored, true, MinIntervalMs));
  p = goodPlan(); p.holdPct = 24.0f;   CHECK(shouldPersistLaunchPlan(makeLaunchPlanRecord(p, 7U, 0U), stored, true, MinIntervalMs));
  p = goodPlan(); p.coastCal = 1.5f;   CHECK(shouldPersistLaunchPlan(makeLaunchPlanRecord(p, 7U, 0U), stored, true, MinIntervalMs));
  p = goodPlan(); p.calMeasured = false; CHECK(shouldPersistLaunchPlan(makeLaunchPlanRecord(p, 7U, 0U), stored, true, MinIntervalMs));
}

static LaunchPlanRecord planWith(float gain, float hold) {
  LaunchPlan p = goodPlan(); p.gain = gain; p.holdPct = hold;
  return makeLaunchPlanRecord(p, 7U, 0U);
}

static void storageTests() {
  Preferences::records().clear(); Preferences::budget() = -1;
  LaunchPlanStorage st; st.resetForTest(); st.service(0);
  LaunchPlanRecord seed{};
  CHECK(!st.takeSeed(seed));                                   // nothing stored on a new machine
  st.offer(planWith(0.012f, 20.0f)); st.service(1000);
  CHECK(st.saves() == 1U);
  CHECK(Preferences::records().count("la") == 1U && Preferences::records().count("lb") == 0U);
  // a plan that differs materially goes to the OTHER slot, the first stays valid; within the wear interval it waits
  st.offer(planWith(0.016f, 24.0f)); st.service(2000);
  CHECK(st.saves() == 1U);
  st.service(2000U + MinIntervalMs);
  CHECK(st.saves() == 2U && Preferences::records().count("lb") == 1U);
  // an immaterial change is dropped, never written
  st.offer(planWith(0.0161f, 24.1f)); st.service(2000U + 3U * MinIntervalMs);
  CHECK(st.saves() == 2U);
  // reboot: the newest slot is the seed
  LaunchPlanStorage boot; boot.resetForTest(); boot.service(0);
  CHECK(boot.takeSeed(seed) && validLaunchPlanRecord(seed) && seed.gain == 0.016f && seed.holdPct == 24.0f);
  CHECK(!boot.takeSeed(seed));                                 // handed over once
  // sequence wrap: the slot with the larger (wrapping) sequence wins
  {
    LaunchPlanRecord a = planWith(0.012f, 20.0f), b = planWith(0.02f, 20.0f);
    a.sequence = 0xFFFFFFFFU; a.crc = launchPlanCrc(a); b.sequence = 0U; b.crc = launchPlanCrc(b);
    Preferences::records().clear();
    Preferences::records()["la"].assign(reinterpret_cast<unsigned char *>(&a), reinterpret_cast<unsigned char *>(&a) + sizeof(a));
    Preferences::records()["lb"].assign(reinterpret_cast<unsigned char *>(&b), reinterpret_cast<unsigned char *>(&b) + sizeof(b));
    LaunchPlanStorage w; w.resetForTest(); w.service(0);
    CHECK(w.takeSeed(seed) && seed.gain == 0.02f);
  }
  // a corrupt newest slot falls back to the older valid one
  {
    Preferences::records().clear();
    LaunchPlanStorage s2; s2.resetForTest(); s2.service(0);
    s2.offer(planWith(0.012f, 20.0f)); s2.service(1000);
    s2.offer(planWith(0.02f, 30.0f)); s2.service(1000U + MinIntervalMs);
    CHECK(s2.saves() == 2U);
    Preferences::records()["lb"][5] ^= 0x40;
    LaunchPlanStorage s3; s3.resetForTest(); s3.service(0);
    CHECK(s3.takeSeed(seed) && seed.gain == 0.012f);
  }
  // power cut at every byte of the second write: the first plan survives and is the seed
  for (int cut = 0; cut < static_cast<int>(sizeof(LaunchPlanRecord)); ++cut) {
    Preferences::records().clear(); Preferences::budget() = -1;
    LaunchPlanStorage s4; s4.resetForTest(); s4.service(0);
    s4.offer(planWith(0.012f, 20.0f)); s4.service(1000);
    CHECK(s4.saves() == 1U);
    Preferences::budget() = cut;
    s4.offer(planWith(0.02f, 30.0f)); s4.service(1000U + MinIntervalMs);
    Preferences::budget() = -1;
    CHECK(s4.saves() == 1U);                                   // the torn write was not counted as a save
    LaunchPlanStorage s5; s5.resetForTest(); s5.service(0);
    CHECK(s5.takeSeed(seed) && validLaunchPlanRecord(seed) && seed.gain == 0.012f && seed.holdPct == 20.0f);
  }
  Preferences::budget() = -1;
}

int main() {
  recordTests();
  policyTests();
  storageTests();
  std::printf("Launch plan: record CRC/ranges/layout, compatibility, wear policy, two slots with read-back, sequence wrap, torn write at every byte (%u checks) PASS\n", checks);
  return 0;
}
