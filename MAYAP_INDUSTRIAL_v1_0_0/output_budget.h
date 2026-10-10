#pragma once
// Output switching budget (D5): counts ON/OFF edges of ONE output in a sliding window and flags minimum ON/OFF violations.
// DESIGN-ONLY helper, not wired. The numbers are NOT chosen here: they must come from the rated switching data of the real SSR /
// relay / contactor fitted to the machine (see audit/smart-thermal/phase8/D5_RELAY_BUDGET.md). Default = every limit 0 = disabled, so an
// instance that has not been given approved numbers can never abort anything.
//
// Use (once approved): one instance per physical class (solid-state SSR, electromechanical relay, contactor), fed from the OutputArbiter's
// ACTUAL state; AutoTune asks abortRequested() on every control cycle and cancels through its existing abort path. It never touches the
// emergency / High / sensor paths and never delays a cut: it can only ask the AutoTune to stop.
#include <cstdint>

namespace MayapSafety {

struct EdgeBudgetParams {
  uint32_t windowMs = 0;       // sliding window (0 = disabled)
  uint32_t maxEdges = 0;       // allowed edges (ON->OFF and OFF->ON) in the window (0 = disabled)
  uint32_t minOnMs = 0;        // an ON shorter than this is a violation (0 = not checked)
  uint32_t minOffMs = 0;
  uint16_t maxShortViolations = 0;   // 0 = violations are only counted, they never request an abort
};

class EdgeBudget {
 public:
  void configure(const EdgeBudgetParams &p) { p_ = p; reset(); }
  void reset() {
    for (uint8_t i = 0; i < kBuckets; ++i) bucket_[i] = 0;
    started_ = false; shortOn_ = shortOff_ = 0; abort_ = false; total_ = 0;
  }
  // Call with the ACTUAL output state, at least once per control cycle.
  void note(uint32_t now, bool state) {
    if (!started_) { started_ = true; state_ = state; stateSince_ = now; bucketStart_ = now; cur_ = 0; return; }
    advance(now);
    if (state != state_) {
      const uint32_t held = static_cast<uint32_t>(now - stateSince_);
      if (state_ && p_.minOnMs && held < p_.minOnMs) ++shortOn_;
      if (!state_ && p_.minOffMs && held < p_.minOffMs) ++shortOff_;
      state_ = state; stateSince_ = now;
      if (bucket_[cur_] < 0xFFFFU) ++bucket_[cur_];
      if (total_ < 0xFFFFFFFFU) ++total_;
    }
    abort_ = abort_ || evaluate();
  }
  uint32_t edgesInWindow() const { uint32_t n = 0; for (uint8_t i = 0; i < kBuckets; ++i) n += bucket_[i]; return n; }
  uint32_t totalEdges() const { return total_; }
  uint16_t shortOn() const { return shortOn_; }
  uint16_t shortOff() const { return shortOff_; }
  bool enabled() const { return p_.windowMs != 0U && p_.maxEdges != 0U; }
  // Latched until reset(): once the budget of this run is spent the AutoTune is asked to stop and stays asked.
  bool abortRequested() const { return abort_; }

 private:
  static constexpr uint8_t kBuckets = 30;
  void advance(uint32_t now) {
    if (!p_.windowMs) return;
    const uint32_t bw = p_.windowMs / kBuckets ? p_.windowMs / kBuckets : 1U;
    uint32_t steps = static_cast<uint32_t>(now - bucketStart_) / bw;
    if (steps >= kBuckets) { for (uint8_t i = 0; i < kBuckets; ++i) bucket_[i] = 0; bucketStart_ = now; return; }
    while (steps--) { cur_ = static_cast<uint8_t>((cur_ + 1U) % kBuckets); bucket_[cur_] = 0; bucketStart_ += bw; }
  }
  bool evaluate() const {
    if (enabled() && edgesInWindow() > p_.maxEdges) return true;
    if (p_.maxShortViolations && (shortOn_ + shortOff_) >= p_.maxShortViolations) return true;
    return false;
  }
  EdgeBudgetParams p_{};
  uint16_t bucket_[kBuckets] = {};
  uint8_t cur_ = 0;
  bool started_ = false, state_ = false, abort_ = false;
  uint32_t stateSince_ = 0, bucketStart_ = 0, total_ = 0;
  uint16_t shortOn_ = 0, shortOff_ = 0;
};

}  // namespace MayapSafety
