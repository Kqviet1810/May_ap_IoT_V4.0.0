#pragma once

// Pure, bounded energy-error pulse-density scheduler; caller provides integer
// types/isfinite. Power is TOTAL average power of the configured heater bank.
class HeaterBurstScheduler {
 public:
  struct Demand { bool groupA = false; bool groupB = false; };
  // Current GPIO1 drives both SSRs; production passes one logical 16 kW bank.
  explicit HeaterBurstScheduler(uint8_t groups = 1U, uint32_t quantumMs = 300U)
      : groups_(groups == 2U ? 2U : 1U),
        quantumMs_(quantumMs < 300U ? 300U : (quantumMs > 60000U ? 60000U : quantumMs)) {}

  void reset() {
    active_ = false;
    balance_ = 0;
    demand_ = Demand{};
    // Keep the bounded A/B runtime difference across cuts, without heat credit.
  }

  Demand update(uint32_t now, float requestedPowerPercent, bool permitted) {
    const uint32_t dt = static_cast<uint32_t>(now - observedAt_);
    if (active_) {
      if (dt >= quantumMs_ * 2U) {
        reset(); // Unknown historical actuation earns no catch-up heat.
      } else {
        const int64_t delivered = (static_cast<int64_t>(demand_.groupA) + demand_.groupB) * Unit;
        balance_ += (requestedUnits_ - delivered) * dt;
        if (groups_ == 2U)
          groupBalanceMs_ += (static_cast<int32_t>(demand_.groupA) - demand_.groupB) * static_cast<int32_t>(dt);
      }
    }
    observedAt_ = now;
    if (!permitted || !isfinite(requestedPowerPercent) || requestedPowerPercent <= 0.0f) {
      reset();
      return demand_;
    }
    if (requestedPowerPercent > 100.0f) requestedPowerPercent = 100.0f;
    // Fixed point avoids recurring floating-point boundary loss at e.g. 30%.
    // Input quantization <= 0.00000005 percentage point.
    requestedUnits_ = static_cast<int64_t>(requestedPowerPercent * 10000000.0 + 0.5) * groups_;
    if (active_ && static_cast<uint32_t>(now - quantumAt_) < quantumMs_) return demand_;
    active_ = true;
    quantumAt_ = now; // No shortened quantum to catch up after a late poll.
    const int64_t quantumEnergy = Unit * quantumMs_;
    // Demand changes inside a quantum can leave up to two group-quanta of
    // error. Bound it even during prolonged saturation; never store a backlog.
    if (balance_ > 2 * quantumEnergy) balance_ = 2 * quantumEnergy;
    if (balance_ < -2 * quantumEnergy) balance_ = -2 * quantumEnergy;
    int64_t units = (balance_ + requestedUnits_ * quantumMs_) / quantumEnergy;
    const uint8_t maximum = groups_ == 2U && requestedPowerPercent > 50.0f ? 2U : 1U;
    if (units < 0) units = 0;
    if (units > maximum) units = maximum;
    demand_ = Demand{};
    if (units == 2) {
      demand_.groupA = demand_.groupB = true;
    } else if (units == 1) {
      demand_.groupA = groups_ == 1U || groupBalanceMs_ <= 0;
      demand_.groupB = groups_ == 2U && !demand_.groupA;
    }
    return demand_;
  }

 private:
  static constexpr int64_t Unit = 1000000000LL;
  const uint8_t groups_;
  const uint32_t quantumMs_;
  bool active_ = false;
  uint32_t quantumAt_ = 0U;
  uint32_t observedAt_ = 0U;
  int32_t groupBalanceMs_ = 0; // Shortest-used group leads; bounded by one burst.
  int64_t requestedUnits_ = 0;
  int64_t balance_ = 0; // Desired minus delivered energy, bounded to +/-2 quanta.
  Demand demand_{};
};
