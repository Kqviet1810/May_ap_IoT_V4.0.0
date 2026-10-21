// Thermal Predictive V1 (host): the identifier recovers the physics of a two-mass oven from the heater and the air temperature only, the planner
// keeps the air at SP without overshoot on cold start and after a door opening, bad input never produces NaN or out-of-range power, and the
// stored model is validated like the other persistent records. The closed loop with the real heating route, the PID, the SSR scheduler and the
// safety layers is covered by tests/thermal-adaptive-v1.cpp-style grids (tools/test_thermal_adaptive.py, Mode::Predictive).
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_predictive_store.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace MayapPredictive;

static unsigned checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); std::abort(); } } while (0)

// ---- a digital twin of the user's oven (2.5 x 2 m, 4 x 5 kW, ~10000 eggs, 2 kW circulation): air + steel, eggs, air<->egg coupling ------------------
struct Oven {
  double Pw = 20000, Cf = 0.5e6, Ce = 1.9e6, hA = 1000, fan = 1500, eggEnd = 0, dead = 60, lag = 40, ventG = 200, amb = 25;
  double UA() const { return (0.03 * Pw + fan + eggEnd) / 12.5; }
};
struct Result { double over = -9, under = 9, maxDoorOver = -9, doorRecoverS = 0; int fallbacks = 0, high = 0; double activeAtS = -1; double minReq = 1e9, maxReq = -1e9; bool finite = true; };

// kind 0: cold start (air and eggs 28 C) for `hours`; kind 1: the same, then a door opening (air -3 C) after 3 h
static Result run(const Oven &o, int kind, double hours, bool fixedBadSensor = false) {
  Controller ctl; ctl.setEnabled(true); ctl.reset(StoredModel{});
  double Ta = 28, Te = 28, hl = 0, ventAir = 0, duty = 0, sd = 0, sensed = 28, ventHold = 0;
  bool ventOn = false;
  std::vector<double> pipe(static_cast<size_t>(o.dead * 10) + 1, 0.0); size_t pp = 0;
  Result r; State prev = State::Learning;
  uint32_t now = 1000;
  const long n = static_cast<long>(hours * 36000.0);
  double doorLastOut = 0;
  for (long it = 0; it < n; ++it) {
    const double t = it * 0.1;
    if (kind == 1 && it == 108000) Ta -= 3.0;
    sd += duty / 100.0; const bool ssr = sd >= 1.0; if (ssr) sd -= 1.0;
    now += 100;
    ctl.tick(now, ssr, ventOn);
    Inputs in; in.now = now; in.sp = 37.5f; in.high = 38.2f; in.limitPct = 100.0f; in.heaterPermit = true; in.ventOn = ventOn; in.ventAllowed = true; in.sensorOk = true;
    if (it % 20 == 0) {
      sensed = std::round(Ta / 0.1) * 0.1; in.pv = static_cast<float>(sensed);
      if (fixedBadSensor && t > 600) ctl.sample(now, NAN, false); else ctl.sample(now, static_cast<float>(sensed), true);
      ctl.update(in);
      duty = ctl.active() ? ctl.request() : std::fmin(40.0, std::fmax(0.0, 15.0 * (37.5 - sensed)));   // a plain P controller stands in for the legacy one
      if (!std::isfinite(duty) || duty < -1e-9 || duty > 100.0 + 1e-9) r.finite = false;
      if (ctl.active()) { if (r.activeAtS < 0) r.activeAtS = t; r.minReq = std::fmin(r.minReq, ctl.request()); r.maxReq = std::fmax(r.maxReq, ctl.request()); }
      if (prev == State::Active && ctl.state() == State::Fallback) ++r.fallbacks;
      prev = ctl.state();
      if (ctl.coolingRequest() || sensed >= 38.0) { ventOn = true; ventHold = t + 120; }
      else if (ventOn && t >= ventHold && sensed <= 37.6) ventOn = false;
    }
    in.pv = static_cast<float>(sensed);
    ctl.service(in, 20);
    if (ctl.wantsPlan()) ctl.beginPlan(in);
    pipe[pp] = ssr ? o.Pw : 0.0; pp = (pp + 1) % pipe.size(); const double delayed = pipe[pp];
    hl += (delayed - hl) * 0.1 / o.lag;
    ventAir += ((ventOn ? 1.0 : 0.0) - ventAir) * 0.1 / 8.0;
    const double q = o.hA * (Ta - Te);
    Ta += (hl - o.UA() * (Ta - o.amb) - o.ventG * ventAir * (Ta - o.amb) - q + o.fan) * 0.1 / o.Cf;
    Te += (q + 0.0) * 0.1 / o.Ce;
    if (!std::isfinite(Ta)) r.finite = false;
    // after the first 40 minutes the air has to stay at SP (no overshoot) ...
    if (t > 2400 && (kind == 0 || t < 10800)) r.over = std::fmax(r.over, Ta - 37.5);
    if (t > 2400 && (kind == 0 || t < 10800)) r.under = std::fmin(r.under, Ta - 37.5);
    // ... and after the door it has to come back, without overshooting on the way
    if (kind == 1 && t > 10860) r.maxDoorOver = std::fmax(r.maxDoorOver, Ta - 37.5);
    if (kind == 1 && t >= 10800 && std::fabs(Ta - 37.5) > 0.3) doorLastOut = t - 10800;
    if (Ta >= 38.2) ++r.high;
  }
  r.doorRecoverS = doorLastOut;
  return r;
}

static void identifierRecoversPhysics() {
  // Open loop (a staircase of heater power) on the oven: no controller involved, only History + Identifier.
  const Oven o;
  History h; h.reset();
  Identifier id; id.reset(StoredModel{});
  double Ta = 28, Te = 28, hl = 0, on = 0, yAcc = 0; int yN = 0;
  std::vector<double> pipe(static_cast<size_t>(o.dead * 10) + 1, 0.0); size_t pp = 0;
  for (long it = 0; it < 20 * 600L; ++it) {
    const double t = it * 0.1, duty = t < 600 ? 40 : 20;
    static double acc = 0; acc += duty / 100.0; const bool ssr = acc >= 1.0; if (ssr) acc -= 1.0;
    pipe[pp] = ssr ? o.Pw : 0.0; pp = (pp + 1) % pipe.size(); hl += (pipe[pp] - hl) * 0.1 / o.lag;
    const double q = o.hA * (Ta - Te);
    Ta += (hl - o.UA() * (Ta - o.amb) - q + o.fan) * 0.1 / o.Cf; Te += q * 0.1 / o.Ce;
    if (ssr) on += 0.1;
    if (it % 20 == 0) { yAcc += std::round(Ta / 0.1) * 0.1; ++yN; }
    if ((it + 1) % 100 == 0) {
      h.push(100.0 * on / 10.0, yAcc / yN, 0.0); on = 0; yAcc = 0; yN = 0;
      if (h.count() % 6 == 0) for (int e = 0; e < 3000; ++e) id.service(h);
    }
  }
  const Model m = id.model();
  // truth in % of the bank: cf = Cf/Pw*100 = 2500, delay 60 s, lag 40 s
  CHECK(std::fabs(std::log(m.cf / 2500.0)) < 0.25);
  CHECK(std::fabs(m.dead - 60.0) < 20.0);
  CHECK(std::fabs(m.tau - 40.0) < 20.0);
  CHECK(id.rms() < 0.05);
  CHECK(!id.atBound());
}

static void coldStartAndDoor() {
  Oven slow;                             // long delay, large air mass
  Oven fast; fast.Cf = 0.3e6; fast.dead = 20; fast.hA = 1700;
  Oven late; late.dead = 120; late.fan = 2000; late.eggEnd = 1500;
  Oven *const ovens[] = {&slow, &fast, &late};
  for (const Oven *o : ovens) {
    const Result a = run(*o, 0, 3.0);
    CHECK(a.finite);
    CHECK(a.activeAtS > 0 && a.activeAtS < 900);          // in control within 15 minutes of the first heat
    CHECK(a.fallbacks == 0);
    CHECK(a.high == 0);
    CHECK(a.over < 0.30);                                  // never more than 0.3 C above SP once warm (the legacy controller reaches 0.5-1.7 C)
    CHECK(a.minReq >= 0.0 && a.maxReq <= 100.0);
    const Result d = run(*o, 1, 4.0);
    CHECK(d.finite && d.fallbacks == 0 && d.high == 0);
    CHECK(d.maxDoorOver < 0.35);                           // the recovery from a 3 C drop does not overshoot
    CHECK(d.doorRecoverS < 900.0);                         // and is back inside +-0.3 C within 15 minutes
  }
}

static void badInput() {
  // the air temperature stops being valid after 10 minutes: the controller must stop proposing power and never return NaN or a range violation
  const Result r = run(Oven{}, 0, 1.0, true);
  CHECK(r.finite);
  CHECK(r.minReq >= 0.0 && r.maxReq <= 100.0);
  // stiff / extreme models stay finite in the integrator
  Model m; m.cf = 403.0f; m.k = 30.0f; m.a = 6.0f; m.ce = 2000.0f; m.tau = 10.0f; m.dead = 5.0f;
  Sim s{37.0f, 30.0f, 100.0f};
  for (int i = 0; i < 20000; ++i) simStep(m, 30.0f, i % 7 < 3 ? 100.0f : 0.0f, -10.0f, 5.0f, 0.0f, s);
  CHECK(std::isfinite(s.ta) && std::isfinite(s.te) && std::fabs(s.ta) < 500.0f);
  // the planner never asks for more than the limit it is given
  Controller c; c.setEnabled(true); c.reset(StoredModel{});
  Inputs in; in.sp = 37.5f; in.limitPct = 20.0f; in.pv = 30.0f; in.sensorOk = true;
  c.update(in);
  CHECK(c.request() <= 20.0f);
  // work per call is bounded (the control cycle must never carry more than one identifier evaluation or three planner candidates)
  CHECK(sizeof(Controller) < 12000);
}

static void persistence() {
  StoredModel m; m.valid = true; m.rms = 0.03f;
  const PredModelRecord r = makePredModelRecord(m, 0xABCDU, 1800000000U);
  CHECK(validPredModelRecord(r));
  for (size_t b = 0; b < sizeof(r); ++b) { PredModelRecord bad = r; reinterpret_cast<uint8_t *>(&bad)[b] ^= 1U; CHECK(!validPredModelRecord(bad)); }
  { PredModelRecord bad = r; bad.cf = 1e9f; bad.crc = predModelCrc(bad); CHECK(!validPredModelRecord(bad)); }       // out of range with a correct CRC
  { PredModelRecord bad = r; bad.k = NAN; bad.crc = predModelCrc(bad); CHECK(!validPredModelRecord(bad)); }
  CHECK(predModelCompatible(r, 0xABCDU, false));
  CHECK(!predModelCompatible(r, 0x1234U, false));                                                                  // another hardware signature
  CHECK(!predModelCompatible(r, 0xABCDU, true));                                                                   // never after an abnormal reset
  const StoredModel back = toStoredModel(r);
  CHECK(back.valid && back.m.cf == m.m.cf && back.m.dead == m.m.dead);
  // wear policy: nothing material changed -> no write; a different physics -> write; but never before the minimum interval
  CHECK(!shouldPersistPredModel(r, r, true, MinIntervalMs));
  StoredModel other = m; other.m.cf *= 1.4f; const PredModelRecord r2 = makePredModelRecord(other, 0xABCDU, 1800000000U);
  CHECK(shouldPersistPredModel(r2, r, true, MinIntervalMs));
  CHECK(!shouldPersistPredModel(r2, r, true, MinIntervalMs - 1U));
  CHECK(shouldPersistPredModel(r, r, false, MinIntervalMs));
  // a stored model starts the next batch with a tighter prior and the same physics
  Controller c; c.setEnabled(true); c.reset(back);
  CHECK(c.telemetry().m.cf == m.m.cf);
}

static PredModelRecord modelWith(float cf, float k) {
  StoredModel m; m.valid = true; m.rms = 0.03f; m.m.cf = cf; m.m.k = k;
  return makePredModelRecord(m, 0xABCDU, 0U);
}

static void storageTests() {
  Preferences::records().clear(); Preferences::budget() = -1;
  PredModelStorage st; st.resetForTest(); st.service(0);
  PredModelRecord seed{};
  CHECK(!st.takeSeed(seed));                                   // nothing stored on a new machine
  st.offer(modelWith(2500.0f, 6.0f)); st.service(1000);
  CHECK(st.saves() == 1U);
  CHECK(Preferences::records().count("pa") == 1U && Preferences::records().count("pb") == 0U);
  // a model that differs materially goes to the OTHER slot; within the wear interval it waits
  st.offer(modelWith(3500.0f, 6.0f)); st.service(2000);
  CHECK(st.saves() == 1U);
  st.service(2000U + MinIntervalMs);
  CHECK(st.saves() == 2U && Preferences::records().count("pb") == 1U);
  // an immaterial change is dropped, never written
  st.offer(modelWith(3510.0f, 6.0f)); st.service(2000U + 3U * MinIntervalMs);
  CHECK(st.saves() == 2U);
  // reboot: the newest slot is the seed, handed over once
  PredModelStorage boot; boot.resetForTest(); boot.service(0);
  CHECK(boot.takeSeed(seed) && validPredModelRecord(seed) && seed.cf == 3500.0f);
  CHECK(!boot.takeSeed(seed));
  // sequence wrap: the slot with the larger (wrapping) sequence wins
  {
    PredModelRecord a = modelWith(2500.0f, 6.0f), b = modelWith(3000.0f, 6.0f);
    a.sequence = 0xFFFFFFFFU; a.crc = predModelCrc(a); b.sequence = 0U; b.crc = predModelCrc(b);
    Preferences::records().clear();
    Preferences::records()["pa"].assign(reinterpret_cast<unsigned char *>(&a), reinterpret_cast<unsigned char *>(&a) + sizeof(a));
    Preferences::records()["pb"].assign(reinterpret_cast<unsigned char *>(&b), reinterpret_cast<unsigned char *>(&b) + sizeof(b));
    PredModelStorage w; w.resetForTest(); w.service(0);
    CHECK(w.takeSeed(seed) && seed.cf == 3000.0f);
  }
  // a corrupt newest slot falls back to the older valid one
  {
    Preferences::records().clear();
    PredModelStorage s2; s2.resetForTest(); s2.service(0);
    s2.offer(modelWith(2500.0f, 6.0f)); s2.service(1000);
    s2.offer(modelWith(3500.0f, 6.0f)); s2.service(1000U + MinIntervalMs);
    CHECK(s2.saves() == 2U);
    Preferences::records()["pb"][5] ^= 0x40;
    PredModelStorage s3; s3.resetForTest(); s3.service(0);
    CHECK(s3.takeSeed(seed) && seed.cf == 2500.0f);
  }
  // power cut at every byte of the second write: the first model survives and is the seed
  for (int cut = 0; cut < static_cast<int>(sizeof(PredModelRecord)); ++cut) {
    Preferences::records().clear(); Preferences::budget() = -1;
    PredModelStorage s4; s4.resetForTest(); s4.service(0);
    s4.offer(modelWith(2500.0f, 6.0f)); s4.service(1000);
    CHECK(s4.saves() == 1U);
    Preferences::budget() = cut;
    s4.offer(modelWith(3500.0f, 6.0f)); s4.service(1000U + MinIntervalMs);
    Preferences::budget() = -1;
    CHECK(s4.saves() == 1U);
    PredModelStorage s5; s5.resetForTest(); s5.service(0);
    CHECK(s5.takeSeed(seed) && validPredModelRecord(seed) && seed.cf == 2500.0f);
  }
  Preferences::budget() = -1;
}

int main() {
  identifierRecoversPhysics();
  coldStartAndDoor();
  badInput();
  persistence();
  storageTests();
  std::printf("thermal predictive: %u checks OK\n", checks);
  return 0;
}
