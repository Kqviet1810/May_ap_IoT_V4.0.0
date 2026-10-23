// Thermal V2 digital-twin grid runner (G1).  SIMULATION ONLY.
// usage: v2grid selftest
//        v2grid run <plants.csv> <controller> <scenario[,scenario...]> [trace_dir]
//        v2grid ref <plants.csv> <scenario[,..]>          (the physical reference: fastest possible, no control)
#include "sim.h"
#include <fstream>
#include <iostream>
#include <sstream>
using namespace v2;

static std::vector<Plant> loadPlants(const char *path) {
  std::vector<Plant> v; std::ifstream f(path); std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#' || line.rfind("id,", 0) == 0) continue;
    std::stringstream ss(line); std::string c; std::vector<std::string> col;
    while (std::getline(ss, c, ',')) col.push_back(c);
    if (col.size() < 18) { std::fprintf(stderr, "bad plant line: %s\n", line.c_str()); std::exit(2); }
    Plant p; p.id = col[0];
    p.Pmax = atof(col[1].c_str()); p.Cf = atof(col[2].c_str()); p.Ce = atof(col[3].c_str()); p.hA = atof(col[4].c_str()); p.UA = atof(col[5].c_str());
    p.td = atof(col[6].c_str()); p.tl = atof(col[7].c_str()); p.Tamb = atof(col[8].c_str()); p.thirdC = atof(col[9].c_str()); p.thirdG = atof(col[10].c_str());
    p.sensorLagS = atof(col[11].c_str()); p.gainScale = atof(col[12].c_str()); p.fanCoupling = atoi(col[13].c_str()) != 0;
    p.sagPerDuty = atof(col[14].c_str()); p.sagStepAtS = atof(col[15].c_str()); p.sagStep = atof(col[16].c_str()); p.sensorOffset = atof(col[17].c_str());
    if (col.size() >= 20) { p.tailFrac = atof(col[18].c_str()); p.tailTauS = atof(col[19].c_str()); }          // E set (optional columns)
    v.push_back(p);
  }
  return v;
}
static Scenario scenario(const std::string &id) {
  Scenario s; s.id = id;
  if (id == "S1") { s.startAir = s.startEgg = 33; s.duration = 4 * 3600; }
  else if (id == "S2") { s.startAir = s.startEgg = 28; s.duration = 6 * 3600; }
  else if (id == "S3" || id == "S3p" || id == "S3w" || id == "S3q" || id == "S3r") { s.startAir = 33; s.startEgg = 37; s.duration = 3 * 3600; }
  else if (id == "S4") { s.warm = true; s.eventAt = 5400; s.door = -3; s.duration = 5400 + 2 * 3600; }
  else if (id == "S5a") { s.warm = true; s.eventAt = 5400; s.trayFrac = 0.25; s.duration = 5400 + 4 * 3600; }
  else if (id == "S5b") { s.warm = true; s.eventAt = 5400; s.trayFrac = 0.5; s.duration = 5400 + 4 * 3600; }
  else if (id == "S6a") { s.warm = true; s.eventAt = 7200; s.ventAt = 7200; s.ventFor = 300; s.ventKnown = 1; s.duration = 7200 + 2 * 3600; }
  else if (id == "S6b") { s.warm = true; s.eventAt = 5400; s.ventAt = 5400; s.ventFor = 300; s.ventKnown = 0; s.duration = 5400 + 2 * 3600; }
  else if (id == "S7") { s.warm = true; s.eventAt = 0; s.duration = 24 * 3600; s.embryoMax = 1500; s.ambientTo = 20; s.sagPerDuty = 0.08; }
  else if (id == "S8") { s.warm = true; s.eventAt = 5400; s.spStep = 0.5; s.duration = 5400 + 2 * 3600; }
  else if (id == "S9") { s.warm = true; s.eventAt = 5400; s.permitLossAt = 5400; s.permitLossFor = 600; s.duration = 5400 + 2 * 3600; }
  else if (id == "S11a" || id == "S11b" || id == "S11c") {            // a door held open 2 / 5 / 10 min: a sustained loss of ~1 kW/K (the owner's 5 min trial held 37.2 C at ~60 % of 20 kW)
    s.warm = true; s.eventAt = 5400; s.doorAt = 5400; s.doorG = 1000; s.doorFor = id == "S11a" ? 120 : (id == "S11b" ? 300 : 600); s.duration = 5400 + 2 * 3600;
  }
  else if (id.rfind("S10", 0) == 0) {            // G4 protection: S10a..S10g = fault kinds 1..7, injected into a settled oven
    s.warm = true; s.eventAt = 5400; s.fault = id.size() > 3 ? id[3] - 'a' + 1 : 1; s.faultAt = 5400; s.duration = 5400 + 2 * 3600;
    if (id == "S10h") { s.warm = false; s.startAir = s.startEgg = 28; s.fault = 1; s.faultAt = 800; s.eventAt = 0; s.duration = 4 * 3600; }   // frozen sensor WHILE climbing (cold, far below SP)
    else if (s.fault < 1 || s.fault > 7) { std::fprintf(stderr, "unknown scenario %s\n", id.c_str()); std::exit(2); }
  }
  else { std::fprintf(stderr, "unknown scenario %s\n", id.c_str()); std::exit(2); }
  return s;
}
static std::unique_ptr<Controller> makeController(const std::string &n) {
  if (n == "fastest") return std::unique_ptr<Controller>(new Fastest());
  if (n == "zero") return std::unique_ptr<Controller>(new Zero());
  if (n == "v2") return std::unique_ptr<Controller>(new V2Ctl());
  if (n == "bangbang") return std::unique_ptr<Controller>(new BangBang());
  if (n.rfind("const", 0) == 0) return std::unique_ptr<Controller>(new HoldSteady(static_cast<float>(atof(n.c_str() + 5))));
  std::fprintf(stderr, "unknown controller %s\n", n.c_str()); std::exit(2);
}
// the physical reference: 100 % from the event (and the exact analytic hold before it, for settled starts)
struct RefFastest : Controller {
  double holdPct, eventAt; RefFastest(double h, double e) : holdPct(h), eventAt(e) {}
  const char *name() const override { return "ref"; }
  float step(const CtlIn &in) override { return (in.now - 1000U) / 1000.0 < eventAt ? static_cast<float>(holdPct) : 100.0f; }
};
static void header() {
  std::printf("plant,scenario,controller,reach_s,over_c,under_c,settle_0p3_s,settle_0p1_s,recov_s,tail01_avg60,tail03,tail01_sensor,peak_air,high_samples,emergency_samples,sec_high,sec_emergency,e115,unsafe_ticks,first_on_s,max_cmd,mean_cmd,energy_kWh\n");
}
static void row(const Plant &p, const Scenario &s, const char *c, const Metrics &m) {
  std::printf("%s,%s,%s,%.0f,%.3f,%.3f,%.0f,%.0f,%.0f,%.1f,%.1f,%.1f,%.3f,%d,%d,%.0f,%.0f,%d,%u,%.1f,%.1f,%.2f,%.2f\n", p.id.c_str(), s.id.c_str(), c, m.reach_s, m.over_c, m.under_c,
              m.settle_0p3_s, m.settle_0p1_s, m.recov_s, m.tail_in_0p1_pct_avg60, m.tail_in_0p3_pct, m.tail_in_0p1_pct_sensor, m.peak_air, m.high_samples, m.emergency_samples,
              m.sec_high, m.sec_emergency, m.e115 ? 1 : 0, m.unsafe_ticks, m.first_on_s, m.max_cmd, m.mean_cmd, m.energy_kWh);
}

// ------------------------------------------------------------------------------------------------------------------------------
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { ++fails; std::printf("FAIL: %s  [%s]\n", msg, #c); } else std::printf("ok:   %s\n", msg); } while (0)
static int selftest() {
  // T1 analytic: single node, no eggs, delay td, starting at ambient (no loss before the heat arrives). Reach time from the first heater-on instant.
  { Plant p; p.id = "T1"; p.Pmax = 16000; p.Cf = 500000; p.UA = 250; p.hA = 0; p.td = 100; p.tl = 0.1; p.Tamb = 28; p.Ce = 1e9;
    Scenario s = scenario("S1"); s.startAir = s.startEgg = 28; s.duration = 1200; Fastest f; Sim sim(p, s, f); Metrics m = sim.run();
    const double Tinf = 28 + 16000.0 / 250, tau = 500000.0 / 250;
    const double ana = -tau * std::log((Tinf - 37.2) / (Tinf - 28.0));       // seconds of heating needed, after the delay
    const double meas = m.reach_s - m.first_on_s - 100;
    std::printf("      analytic %.1f s vs measured %.1f s after delay (first_on %.1f s)\n", ana, meas, m.first_on_s);
    CHECK(std::fabs(meas - ana) < 0.03 * ana + 3.0, "T1 single-node rise time matches the closed form within 3 %"); }
  // T2 energy balance: lossless air at ambient, constant 50 %: dT*Cf == delivered energy (less the energy still in the delay line)
  { Plant p; p.id = "T2"; p.Pmax = 16000; p.Cf = 2000000; p.UA = 0; p.hA = 0; p.td = 20; p.tl = 0.1; p.Ce = 1e9; p.Tamb = 30;
    Scenario s = scenario("S1"); s.startAir = s.startEgg = 30; s.duration = 1200; HoldSteady h(50); Sim sim(p, s, h);
    Metrics m = sim.run();
    const double dT = (m.peak_air - 30.0);   // monotone rise: peak == end value
    const double E_J = m.energy_kWh * 3.6e6, inLine = 8000.0 * 20;
    std::printf("      dT*Cf = %.3f MJ, delivered = %.3f MJ (in delay line %.2f MJ), mean_cmd %.2f %%\n", dT * 2000000 / 1e6, E_J / 1e6, inLine / 1e6, m.mean_cmd);
    CHECK(std::fabs(dT * 2000000 - (E_J - inLine)) < 0.03 * E_J, "T2 energy conservation (air heat gained == delivered - in the delay line) within 3 %");
    CHECK(std::fabs(E_J / (16000.0 * (1200 - m.first_on_s)) - 0.5) < 0.02, "T2 real burst scheduler delivers 50 % duty within 2 points"); }
  // T3 burst scheduler accuracy at the powers V2 will use (hold-like and mid)
  for (float pct : {5.f, 12.f, 30.f, 70.f}) {
    Plant p; p.id = "T3"; p.Pmax = 16000; p.Cf = 5e7; p.UA = 0; p.hA = 0; p.td = 5; p.tl = 0.1; p.Ce = 1e9; p.Tamb = 30;
    Scenario s = scenario("S1"); s.startAir = s.startEgg = 30; s.duration = 1200; HoldSteady h(pct); Sim sim(p, s, h); Metrics m = sim.run();
    const double duty = 100.0 * m.energy_kWh * 3.6e6 / (16000.0 * (1200 - m.first_on_s));
    char msg[96]; std::snprintf(msg, sizeof(msg), "T3 burst duty at %.0f %% -> %.2f %% (within 1 point)", pct, duty);
    CHECK(std::fabs(duty - pct) < 1.0, msg); }
  // T4 settled start stays settled with the analytic hold (plant steady-state equations + warm init)
  { Plant p; p.id = "T4"; p.Pmax = 16000; p.Cf = 500000; p.UA = 250; p.hA = 1300; p.td = 100; p.tl = 40; p.Ce = 1.9e6; p.Tamb = 28;
    Scenario s = scenario("S4"); s.door = 0; s.eventAt = 3000; s.duration = 7200;
    HoldSteady h(100.0 * (250 * 9.5) / 16000.0); Sim sim(p, s, h); Metrics m = sim.run();
    std::printf("      settled drift: over %.3f under %.3f\n", m.over_c, m.under_c);
    CHECK(m.over_c < 0.06 && m.under_c > -0.25, "T4 settled plant stays within 0.25 C for 2 h at the analytic hold (quantised sensor + production burst)"); }
  // T5 protection: 100 % forever must be cut by the PRODUCTION layers, never commanded through them
  { Plant p; p.id = "T5"; p.Pmax = 16000; p.Cf = 500000; p.UA = 250; p.hA = 1300; p.td = 100; p.tl = 40; p.Ce = 1.9e6; p.Tamb = 28;
    Scenario s = scenario("S2"); s.startAir = s.startEgg = 36; s.duration = 7200; Fastest f; Sim sim(p, s, f); Metrics m = sim.run();
    std::printf("      peak %.2f C, high_samples %d, emergency %d\n", m.peak_air, m.high_samples, m.emergency_samples);
    CHECK(m.unsafe_ticks == 0, "T5 no heating command while the protection layer blocks");
    CHECK(m.high_samples >= 1, "T5 the High layer does act on a runaway request"); }
  // T6 E115: a heater that cannot raise the air must trip the production watchdog and then stay off
  { Plant p; p.id = "T6"; p.Pmax = 200; p.Cf = 5e6; p.UA = 250; p.hA = 1300; p.td = 20; p.tl = 10; p.Ce = 1.9e6; p.Tamb = 28;
    Scenario s = scenario("S1"); s.startAir = s.startEgg = 30; s.duration = 3 * 3600; Fastest f; Sim sim(p, s, f); Metrics m = sim.run();
    CHECK(m.e115, "T6 E115 trips when 900 s of full power does not raise the air 0.3 C");
    CHECK(m.unsafe_ticks == 0, "T6 no heating command after E115"); }
  std::printf("%s (%d failure(s))\n", fails ? "SELFTEST FAILED" : "SELFTEST PASSED", fails);
  return fails ? 1 : 0;
}

int main(int argc, char **argv) {
  if (argc >= 2 && !std::strcmp(argv[1], "selftest")) return selftest();
  if (argc >= 5 && !std::strcmp(argv[1], "run")) {
    std::vector<Plant> plants = loadPlants(argv[2]); std::string cn = argv[3]; std::stringstream ss(argv[4]); std::string sid;
    header();
    while (std::getline(ss, sid, ',')) for (const Plant &p : plants) {
      Scenario s = scenario(sid); std::unique_ptr<Controller> c = makeController(cn);
      if ((sid == "S3p" || sid == "S3w" || sid == "S3q" || sid == "S3r") && cn == "v2") {      // restart with the Prior stored by an earlier run: p = the same oven (S3p), w = ANOTHER oven of the set (S3w, the stored path does not fit)
        size_t src = 0; for (size_t i = 0; i < plants.size(); ++i) if (plants[i].id == p.id) src = i;
        if (sid == "S3w") src = (src + plants.size() / 2) % plants.size();
        Plant lp = plants[src];
        if (sid == "S3q") lp.gainScale *= 0.7;       // stored when the heater was 30 % weaker than it is now (element replaced)
        if (sid == "S3r") lp.gainScale *= 1.3;       // stored when the heater was 30 % stronger than it is now (element ageing)
        V2Ctl learner; Scenario ls = scenario("S1"); ls.duration = 2 * 3600; Sim lsim(lp, ls, learner); lsim.run();
        thermalv2::Prior pr;
        if (learner.slot.controller().exportPrior(pr)) static_cast<V2Ctl *>(c.get())->slot.controller().importPrior(pr);
        else std::fprintf(stderr, "no prior from %s\n", plants[src].id.c_str());
      }
      if (s.warm && cn != "fastest" && cn != "zero" && cn.rfind("const", 0) != 0) {      // a real controller meets its disturbances after a real cold start
        const double P = 4 * 3600.0; s.coldStart = true; s.startAir = s.startEgg = 33; s.eventAt += P; s.duration += P;
        if (s.ventAt >= 0) s.ventAt += P;
        if (s.doorAt >= 0) s.doorAt += P;
        if (s.permitLossAt >= 0) s.permitLossAt += P;
        if (s.faultAt >= 0) s.faultAt += P;
      }
      Sim sim(p, s, *c);
      std::ofstream tr; if (argc >= 6) { tr.open(std::string(argv[5]) + "/" + p.id + "_" + sid + ".csv"); tr << "t,air,egg,pv,cmd,on,high,e115,vent\n"; sim.trace = &tr; }
      row(p, s, cn.c_str(), sim.run());
    }
    return 0;
  }
  if (argc >= 4 && !std::strcmp(argv[1], "ref")) {
    std::vector<Plant> plants = loadPlants(argv[2]); std::stringstream ss(argv[3]); std::string sid; header();
    while (std::getline(ss, sid, ',')) for (const Plant &p : plants) {
      Scenario s = scenario(sid);
      const double hold = 100.0 * (p.UA * (s.SP - p.Tamb)) / (p.Pmax);          // analytic settled hold (air=egg, no embryo heat)
      RefFastest r(hold, s.warm ? s.eventAt : 0.0); Sim sim(p, s, r); sim.stopAtReach = true; row(p, s, "ref", sim.run());
    }
    return 0;
  }
  std::fprintf(stderr, "usage: v2grid selftest | run <plants.csv> <ctl> <scn,..> [trace_dir] | ref <plants.csv> <scn,..>\n");
  return 2;
}
