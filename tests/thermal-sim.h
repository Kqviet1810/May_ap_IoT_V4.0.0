#pragma once
// Closed-loop test rig for the Thermal V3 controller. HOST TESTS ONLY (never compiled into the firmware).
//
// The plant is the two-node oven of tools/thermal-v3/plants.json: the AIR (with the carts, capacity Cf) is heated through a
// transport delay td and a first-order lag tl; it loses heat to the room (UA), to the fan (ventW) and through an open door, and
// it exchanges heat with the EGGS (capacity Ce, coupling hA). Added to it: a slow share of the heater power (tailFrac, tailTau:
// the stored heat of the elements), a sensor lag, and the production sensor chain (0.1 degC quantisation, median-3, IIR 3/8 on a
// 2 s sample). A cold tray is a step of the egg capacity at a lower temperature.
//
// This is a way to catch STRUCTURAL faults of the control law across a wide family of ovens. It is not the judge of one real
// oven: the real logs (tests/fixtures/*.csv, replayed by tests/thermal-replay.cpp) are.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace thermalsim {

struct PlantParams {
  double Pmax = 16000, Cf = 500000, Ce = 1900000, hA = 1300, UA = 250, td = 100, tl = 40;
  double Tamb = 28, ventW = 2500, tailFrac = 0, tailTau = 120, sensorLag = 0, doorW = 1500;
  double Q0 = 0;   // heat that is always there (circulation fans, the room's radiation) [W]; 0 in every oven of the families
};

class Plant {
 public:
  Plant(const PlantParams &p, double airC, double eggC) : p_(p), Ce_(p.Ce), Ta_(airC), Te_(eggC), sens_(airC) {
    pipe_.assign(std::max<size_t>(1, static_cast<size_t>(std::lround(p.td / DT))), 0.0);
  }
  static constexpr double DT = 0.1;

  // The oven in thermal equilibrium at `c` degC: air = eggs = sensor = c, the heater delivering exactly what the walls lose.
  void settleAt(double c) {
    Ta_ = Te_ = sens_ = c;
    const double w = p_.UA * (c - p_.Tamb);
    heater_ = heater2_ = w;
    std::fill(pipe_.begin(), pipe_.end(), w);
  }
  // The heater has been delivering `pct` percent of Pmax for a long time (the start of a recorded stretch).
  void primeHeater(double pct) {
    const double w = std::max(0.0, std::min(100.0, pct)) / 100.0 * p_.Pmax * heaterHealth_;
    heater_ = heater2_ = w;
    std::fill(pipe_.begin(), pipe_.end(), w);
  }
  void setEggTemp(double c) { Te_ = c; }
  void setAirTemp(double c) { Ta_ = sens_ = c; }
  void step(double uPct, bool vent, bool door) {
    const double pwr = std::max(0.0, std::min(100.0, uPct)) / 100.0 * p_.Pmax * heaterHealth_;
    const double delayed = pipe_[idx_];
    pipe_[idx_] = pwr;
    idx_ = (idx_ + 1) % pipe_.size();
    heater_ += (delayed - heater_) * DT / std::max(0.1, p_.tl);
    double eff = heater_;
    if (p_.tailFrac > 0.0) {
      heater2_ += (delayed - heater2_) * DT / std::max(0.1, p_.tailTau);
      eff = (1.0 - p_.tailFrac) * heater_ + p_.tailFrac * heater2_;
    }
    vent_ += ((vent ? 1.0 : 0.0) - vent_) * DT / 8.0;
    const double ventLoss = (p_.ventW / 12.5) * vent_ * (Ta_ - p_.Tamb);
    const double doorLoss = door ? p_.doorW * (Ta_ - p_.Tamb) : 0.0;
    const double qEgg = p_.hA * (Ta_ - Te_);
    const double wall = p_.UA * (Ta_ - p_.Tamb);
    Ta_ += (eff + p_.Q0 - wall - qEgg - ventLoss - doorLoss) / p_.Cf * DT;
    Te_ += qEgg / Ce_ * DT;
    sens_ += p_.sensorLag > 0.0 ? (Ta_ - sens_) * DT / p_.sensorLag : (Ta_ - sens_);
  }
  // A tray at `tempC` whose capacity is `fraction` of the eggs already inside.
  void insertColdMass(double fraction, double tempC) {
    const double add = fraction * p_.Ce;
    Te_ = (Ce_ * Te_ + add * tempC) / (Ce_ + add);
    Ce_ += add;
  }
  void setHeaterHealth(double h) { heaterHealth_ = h; }   // 0 = a dead heater / open SSR
  double air() const { return Ta_; }
  double egg() const { return Te_; }
  double sensor() const { return sens_; }
  const PlantParams &params() const { return p_; }
  void setAmbient(double c) { p_.Tamb = c; }                 // the room around the oven warms up (a hot afternoon) or cools down

 private:
  PlantParams p_;
  std::vector<double> pipe_;
  size_t idx_ = 0;
  double Ce_, Ta_, Te_, sens_;
  double heater_ = 0.0, heater2_ = 0.0, vent_ = 0.0, heaterHealth_ = 1.0;
};

// The production sensor path: a 0.1 degC reading every 2 s, median of the last three, then the 3/8 IIR (Q8).
class SensorChain {
 public:
  // stepC = 0.1: the sensor of this machine (a x10 module read as 0.1 degC); 0.01: a x100 module (the controller is told through Input::dQ).
  explicit SensorChain(double stepC = 0.1) : scale_(0.1 / stepC) {}
  void sample(double physicalC) {
    const int16_t tenth = static_cast<int16_t>(std::lround(physicalC * 10.0 * scale_));
    win_[wi_] = tenth;
    wi_ = static_cast<uint8_t>((wi_ + 1U) % 3U);
    if (n_ < 3U) ++n_;
    int32_t target;
    if (n_ == 1U) target = win_[0] * 256L;
    else if (n_ == 2U) target = (static_cast<int32_t>(win_[0]) + win_[1]) * 128L;
    else {
      const int16_t a = win_[0], b = win_[1], c = win_[2];
      const int16_t m = (a > b) ? ((b > c) ? b : ((a > c) ? c : a)) : ((a > c) ? a : ((b > c) ? c : b));
      target = m * 256L;
    }
    if (!init_) { q8_ = target; init_ = true; return; }
    q8_ += ((target - q8_) * 3) / 8;
  }
  float value() const { return static_cast<float>(q8_ / (2560.0 * scale_)); }
 private:
  double scale_;
  int16_t win_[3] = {0, 0, 0};
  uint8_t wi_ = 0, n_ = 0;
  int32_t q8_ = 0;
  bool init_ = false;
};

inline uint32_t xorshift32(uint32_t &x) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }

// Ovens drawn uniformly from wide ranges (a family, not one oven). Same generator for every set; only the seed and the ranges differ.
inline std::vector<PlantParams> family(uint32_t seed, int count, bool slowOvens) {
  std::vector<PlantParams> out;
  uint32_t x = seed;
  auto u = [&x]() { return xorshift32(x) / 4294967296.0; };
  auto in = [&u](double lo, double hi) { return lo + (hi - lo) * u(); };
  while (static_cast<int>(out.size()) < count) {
    PlantParams p;
    if (slowOvens) {
      p.Pmax = in(16000, 22000); p.Cf = in(800000, 1800000); p.Ce = in(1200000, 2400000); p.hA = in(800, 1500);
      p.UA = in(30, 150); p.td = in(60, 170); p.tl = in(25, 55);
      p.tailFrac = in(0, 0.4); p.tailTau = in(60, 240); p.sensorLag = in(0, 90);
    } else {
      p.Pmax = in(14000, 22000); p.Cf = in(250000, 1300000); p.Ce = in(1000000, 2400000); p.hA = in(900, 1800);
      p.UA = in(30, 400); p.td = in(40, 200); p.tl = in(25, 55);
    }
    if (p.UA * 9.5 / p.Pmax > 0.9) continue;     // the hold would need more than 90 % of the heater: not an oven
    out.push_back(p);
  }
  return out;
}

// The oven of the 29/10 batch (MAYAP 20 kW class, 16 kW SSR bank, ~10 000 eggs), fitted to tests/fixtures/real-logs/mayap-20261029-batch-failure.csv:
// the model driven by the logged heater request reproduces the logged PV with 0.10 degC RMS over 31 minutes (the door held open, a tray as large as
// the eggs already inside put in cold, a vent run). Driven by the 2.3.4 controller it fails the way the machine did (see thermal-closed-loop.cpp).
// Not a measurement of the oven: several of its numbers (hA, UA, the delay split between td and tl) are only loosely determined by that one stretch.
inline PlantParams u29Oven() {
  PlantParams p;
  p.Pmax = 16000; p.Cf = 1.3e6; p.Ce = 1.55e6; p.hA = 1650; p.UA = 200; p.td = 110; p.tl = 80;
  p.Tamb = 28; p.ventW = 1500; p.doorW = 2750;
  return p;
}

inline std::vector<PlantParams> setA() {
  struct R { double Pmax, Cf, hA, UA, td, tl; };
  static const R rows[] = {
      {16000, 300000, 1700, 350, 60, 30}, {16000, 500000, 1300, 250, 100, 40}, {16000, 1000000, 1000, 200, 150, 50},
      {16000, 1200000, 1000, 31, 150, 50}, {20000, 500000, 1300, 250, 100, 40}, {20000, 300000, 1700, 350, 60, 30},
      {16000, 800000, 1000, 300, 180, 50}, {16000, 300000, 1000, 200, 120, 40}, {20000, 1200000, 1000, 31, 150, 50},
      {16000, 500000, 1700, 350, 180, 30}, {16000, 800000, 1300, 250, 60, 50}, {20000, 1000000, 1700, 200, 140, 40}};
  std::vector<PlantParams> out;
  for (const R &r : rows) {
    PlantParams p;
    p.Pmax = r.Pmax; p.Cf = r.Cf; p.hA = r.hA; p.UA = r.UA; p.td = r.td; p.tl = r.tl;
    out.push_back(p);
  }
  return out;
}

}  // namespace thermalsim
