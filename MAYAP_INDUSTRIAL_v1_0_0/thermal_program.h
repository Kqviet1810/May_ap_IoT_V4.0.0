#pragma once
// Incubation thermal program: per-day set-point stages with hard limits (Smart Thermal phase 6).
//
// DESIGN ONLY, NOT WIRED INTO THE FIRMWARE: nothing includes this header yet, so it costs no Flash/RAM and cannot change the control
// path. Integration (batch day -> set point, HMI/Web schema, EEPROM record, event log) needs a reviewed approval.
//
// No species/day schedule is shipped. A temperature programme is a biological decision: the table is EMPTY by default and a programme
// is only accepted if it passes validate(). With no programme the set point is the configured one, bit for bit.
//
// What the module guarantees (pure, no allocation, no float surprises):
//   * every stage target lies inside [minTargetC, min(maxTargetC, highAlarm - HighMarginC)]: never at or above a safety threshold;
//   * consecutive stages differ by at most maxStepC;
//   * the applied set point slews toward the stage target at most maxSlewCPerHour (a step of the table is never a step of the set point);
//   * NaN/Inf/out-of-range input leaves the configured set point untouched;
//   * a programme NEVER changes the set point of a batch by itself: configure() only stores it. It acts on a batch only after
//     activateForBatch(batchId) (an explicit operator confirmation recorded for THAT batch). A different batch id, a stop, or a
//     reboot without the persisted confirmation (see program_store.h) leaves the configured set point alone.
#include <cmath>
#include <cstdint>

namespace MayapProgram {

constexpr uint8_t MaxStages = 8;
constexpr float HighMarginC = 0.7f;          // the programme can never sit closer than this to the High alarm

struct Limits {
  float minTargetC = 30.0f;
  float maxTargetC = 38.0f;
  float maxStepC = 0.5f;                     // between two consecutive stages
  float maxSlewCPerHour = 0.5f;              // applied set point rate
};

struct Stage {
  uint8_t fromDay = 0;                       // 1-based incubation day on which the stage starts
  int16_t targetX10 = 0;                     // 0.1 degC
  Stage() {}
  Stage(uint8_t day, int16_t target) : fromDay(day), targetX10(target) {}
};

struct Program {
  bool enabled = false;
  uint8_t count = 0;
  Stage stages[MaxStages]{};
};

enum class Reject : uint8_t { None, Empty, FirstDay, DayOrder, TooMany, Range, Step, NotFinite };

inline Reject validate(const Program &p, const Limits &lim, float highAlarmC) {
  if (!p.enabled) return Reject::None;
  if (!std::isfinite(highAlarmC) || !std::isfinite(lim.minTargetC) || !std::isfinite(lim.maxTargetC) ||
      !std::isfinite(lim.maxStepC) || !std::isfinite(lim.maxSlewCPerHour)) return Reject::NotFinite;
  if (p.count == 0) return Reject::Empty;
  if (p.count > MaxStages) return Reject::TooMany;
  if (p.stages[0].fromDay != 1) return Reject::FirstDay;
  const float hi = std::fmin(lim.maxTargetC, highAlarmC - HighMarginC);
  for (uint8_t i = 0; i < p.count; ++i) {
    if (i > 0 && p.stages[i].fromDay <= p.stages[i - 1].fromDay) return Reject::DayOrder;
    const float t = p.stages[i].targetX10 * 0.1f;
    if (t < lim.minTargetC || t > hi) return Reject::Range;
    if (i > 0 && std::fabs(t - p.stages[i - 1].targetX10 * 0.1f) > lim.maxStepC + 1e-4f) return Reject::Step;
  }
  return Reject::None;
}

struct Event { uint8_t stage = 0; int16_t targetX10 = 0; bool pending = false; };

// Applied set point = configured set point until a valid programme is active, then a rate-limited follower of the stage target.
class SetpointProgram {
 public:
  void configure(const Program &p, const Limits &lim, float highAlarmC) {
    prog_ = p; lim_ = lim; valid_ = p.enabled && validate(p, lim, highAlarmC) == Reject::None;
    started_ = false; stage_ = 0xFF; event_ = Event{};
    armed_ = false;                                   // a (re)configured programme is never active until confirmed again
  }
  bool active() const { return valid_; }
  bool armedFor(uint32_t batchId) const { return valid_ && armed_ && armedBatch_ == batchId; }
  // Operator confirmation for ONE batch (batchId must be non-zero). Returns false if there is no valid programme.
  bool activateForBatch(uint32_t batchId) { if (!valid_ || batchId == 0U) return false; armed_ = true; armedBatch_ = batchId; started_ = false; return true; }
  void deactivate() { armed_ = false; started_ = false; }
  uint32_t armedBatch() const { return armed_ ? armedBatch_ : 0U; }
  // `day`: 1-based incubation day (0 = no batch); `batchId`: the running batch; `now`: millis(). Returns the set point to use.
  float update(uint32_t now, uint8_t day, float configuredSp, uint32_t batchId) {
    if (!armedFor(batchId) || day == 0 || !std::isfinite(configuredSp)) { started_ = false; return configuredSp; }
    uint8_t idx = 0;
    for (uint8_t i = 0; i < prog_.count; ++i) if (day >= prog_.stages[i].fromDay) idx = i;
    const float target = prog_.stages[idx].targetX10 * 0.1f;
    if (!started_) { started_ = true; applied_ = configuredSp; lastMs_ = now; stage_ = 0xFF; }
    if (idx != stage_) { stage_ = idx; event_.stage = idx; event_.targetX10 = prog_.stages[idx].targetX10; event_.pending = true; }
    float dt = static_cast<float>(static_cast<uint32_t>(now - lastMs_)) * 0.001f;
    lastMs_ = now;
    if (dt > 600.0f) dt = 600.0f;                                  // a long stall must not become a long jump
    const float maxMove = lim_.maxSlewCPerHour * dt / 3600.0f;
    const float d = target - applied_;
    applied_ += std::fabs(d) <= maxMove ? d : (d > 0 ? maxMove : -maxMove);
    return applied_;
  }
  bool takeEvent(Event &e) { if (!event_.pending) return false; e = event_; event_.pending = false; return true; }

 private:
  Program prog_{};
  Limits lim_{};
  bool valid_ = false, started_ = false, armed_ = false;
  uint32_t armedBatch_ = 0;
  uint8_t stage_ = 0xFF;
  uint32_t lastMs_ = 0;
  float applied_ = 0;
  Event event_{};
};

}  // namespace MayapProgram
