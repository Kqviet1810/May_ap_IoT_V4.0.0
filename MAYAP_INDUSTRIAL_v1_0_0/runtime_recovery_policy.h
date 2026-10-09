#pragma once
#include <stdint.h>

namespace MayapRecovery {
enum class Service : uint8_t { Network, Mqtt, Cloud, Ota, Count };
enum class Action : uint8_t { None, Reinit, Isolate, Degraded };
constexpr uint32_t SERVICE_TIMEOUT_MS[] = {30000U, 60000U, 120000U, 180000U};
constexpr uint32_t ISOLATE_AFTER_MS = 60000U;
// Retained as the point at which the HMI declares an online subsystem degraded.
// Communication failure is never a valid reason to restart the controller.
constexpr uint32_t RESTART_AFTER_MS = 300000U;
constexpr uint32_t ISOLATE_PAUSE_MS = 30000U;
constexpr uint32_t WIFI_COOLDOWN_MS = 120000U;
constexpr uint32_t WIFI_OFFLINE_MS = 300000U;
inline uint32_t age(uint32_t now, uint32_t then) { return static_cast<uint32_t>(now - then); }
// How long a service has been silent. A service on the other core may beat BETWEEN the supervisor reading `now` and reading
// the beat, so `beat` can be a few ms NEWER than `now`: plain unsigned subtraction would turn that into ~4.29e9 ms of silence
// and restart a perfectly healthy service (seen on hardware: a 57 ms old MQTT link was torn down). A beat from the "future" is
// zero silence.
inline uint32_t silence(uint32_t now, uint32_t beat) {
  const int32_t delta = static_cast<int32_t>(static_cast<uint32_t>(now - beat));
  return delta > 0 ? static_cast<uint32_t>(delta) : 0U;
}
inline bool due(uint32_t now, uint32_t when) { return static_cast<int32_t>(now - when) >= 0; }

class ServiceWatch {
 public:
  Action update(uint32_t now, bool admitted, uint32_t beat, uint32_t ack, uint32_t timeout) {
    if (!admitted) return Action::None;
    if (ack != lastAck_) {
      lastAck_ = ack; failing_ = false; isolated_ = false; degraded_ = false;
    }
    const bool stale = silence(now, beat) > timeout;
    if (!stale) {
      failing_ = false; isolated_ = false; degraded_ = false;
      return Action::None;
    }
    if (!failing_) { failing_ = true; faultAt_ = now; return Action::Reinit; }
    if (!isolated_ && age(now, faultAt_) >= ISOLATE_AFTER_MS) {
      isolated_ = true; return Action::Isolate;
    }
    if (!degraded_ && age(now, faultAt_) >= RESTART_AFTER_MS) {
      degraded_ = true; return Action::Degraded;
    }
    return Action::None;
  }
 private:
  uint32_t lastAck_ = 0U;
  uint32_t faultAt_ = 0U;
  bool failing_ = false;
  bool isolated_ = false;
  bool degraded_ = false;
};

}  // namespace MayapRecovery
