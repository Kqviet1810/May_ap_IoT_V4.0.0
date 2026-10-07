#pragma once
#include <stdint.h>

namespace MayapLightAlarm {

// "Light left on during a batch" policy, kept pure so it is host-testable. Switching the light on and
// off is normal operation and sends nothing; only a light that stays on for `delayMs` without a break
// raises the alarm, then it reminds every `repeatMs`, and "resolved" is sent only if the alarm was raised.
enum class Action : uint8_t { None, Raise, Remind, Resolve };

struct Tracker {
  bool onActive = false;   // condition (batch running AND light on) currently true
  uint32_t onSince = 0U;   // when it became true
  bool raised = false;     // an alarm was actually sent for this episode
  uint32_t lastSentAt = 0U;
};

inline bool reached(uint32_t now, uint32_t target) { return static_cast<int32_t>(now - target) >= 0; }

inline void reset(Tracker &t) { t = Tracker{}; }

// Decides the next action. The caller enqueues the notification and calls commit() only when it was
// accepted, so a full queue simply retries on the next pass.
inline Action step(Tracker &t, bool enabled, bool condition, uint32_t now, uint32_t delayMs, uint32_t repeatMs) {
  if (!enabled) { reset(t); return Action::None; }
  if (!condition) {
    t.onActive = false;
    return t.raised ? Action::Resolve : Action::None;
  }
  if (!t.onActive) { t.onActive = true; t.onSince = now; }
  if (!t.raised) return reached(now, t.onSince + delayMs) ? Action::Raise : Action::None;
  return reached(now, t.lastSentAt + repeatMs) ? Action::Remind : Action::None;
}

inline void commit(Tracker &t, Action action, uint32_t now) {
  if (action == Action::Raise) { t.raised = true; t.lastSentAt = now; }
  else if (action == Action::Remind) t.lastSentAt = now;
  else if (action == Action::Resolve) t.raised = false;
}

}  // namespace MayapLightAlarm
