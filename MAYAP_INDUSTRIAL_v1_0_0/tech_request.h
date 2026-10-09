#pragma once
// Staged technical request: the MQTT owner task validates a signed Web request and parks the values here; the control task takes
// them when the matching TechRequestSubmit command arrives. Nothing is applied by the Web: the request only becomes a PENDING
// approval that the HMI (Yes/No, 60 s) decides. One slot, no allocation.
#include "advanced_history.h"
#include <cstring>

namespace MayapTech {

enum class Kind : uint8_t { None = 0, Config = 1, AutoTune = 2 };
constexpr uint32_t RequestTtlMs = 60000UL;
constexpr uint8_t RequestIdCapacity = 40;

struct Staged {
  bool used = false;
  Kind kind = Kind::None;
  uint16_t mask = 0;
  AdvancedHistory::Snapshot values;
  char requestId[RequestIdCapacity] = "";
};
inline Staged &staged() { static Staged s; return s; }
inline portMUX_TYPE &stagedMux() { static portMUX_TYPE m = portMUX_INITIALIZER_UNLOCKED; return m; }

// false = a previous request is still waiting to be taken (the caller answers "busy").
inline bool stage(Kind kind, uint16_t mask, const AdvancedHistory::Snapshot &values, const char *requestId) {
  bool ok = false;
  portENTER_CRITICAL(&stagedMux());
  if (!staged().used) {
    Staged &s = staged();
    s.used = true; s.kind = kind; s.mask = mask; s.values = values;
    snprintf(s.requestId, sizeof(s.requestId), "%s", requestId ? requestId : "");
    ok = true;
  }
  portEXIT_CRITICAL(&stagedMux());
  return ok;
}
inline bool take(Staged &out) {
  bool ok = false;
  portENTER_CRITICAL(&stagedMux());
  if (staged().used) { out = staged(); staged().used = false; ok = true; }
  portEXIT_CRITICAL(&stagedMux());
  return ok;
}
inline void discard() {
  portENTER_CRITICAL(&stagedMux());
  staged().used = false;
  portEXIT_CRITICAL(&stagedMux());
}

}  // namespace MayapTech
