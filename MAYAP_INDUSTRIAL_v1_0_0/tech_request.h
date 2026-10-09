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


// Heavy technical details for the HMI / Web. They are NOT part of MachineRuntime (which is copied into ~8 static mailboxes);
// the controller publishes ONE copy here and the HMI / bridge read it only while they need it.
struct Detail {
  uint8_t historyCount = 0;
  AdvancedHistory::Snapshot history[AdvancedHistory::Slots];   // rank 0 = newest; HMI only, the Web never reads it
  AdvancedHistory::Snapshot pending;                           // values the Web asked for (full snapshot with the request applied)
  char resultId[RequestIdCapacity] = "";                       // requestId of the last Web technical request
};
inline Detail &sharedDetail() { static Detail d; return d; }
inline portMUX_TYPE &detailMux() { static portMUX_TYPE m = portMUX_INITIALIZER_UNLOCKED; return m; }
// Controller side: copy only when something changed (nothing is written while the HMI/Web are idle).
inline void publishDetail(const Detail &next) {
  portENTER_CRITICAL(&detailMux());
  if (memcmp(&sharedDetail(), &next, sizeof(Detail)) != 0) sharedDetail() = next;
  portEXIT_CRITICAL(&detailMux());
}
inline void readDetail(Detail &out) {
  portENTER_CRITICAL(&detailMux());
  out = sharedDetail();
  portEXIT_CRITICAL(&detailMux());
}

}  // namespace MayapTech
