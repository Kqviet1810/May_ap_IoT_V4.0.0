#pragma once
// The ONLY decision logic for the station link (pure C++, the host tests drive exactly this code).
//
//   CONNECTING --associated--> CONNECTED
//   CONNECTING --attempt failed (driver gave up / 12 s)--> BACKOFF
//   CONNECTED  --link lost--> BACKOFF (first retry at once, no penalty)
//   BACKOFF    --ladder elapsed--> CONNECTING            (light attempt: plain reconnect, no owner drain, no driver re-init)
//   BACKOFF    --ladder elapsed AND recovery due--> RECOVERY (heavy: owners drain first, then one in-place reconnect)
//   RECOVERY   --done--> CONNECTING
//
// Inputs are Wi-Fi facts only (associated, the driver's own "attempt over" report, time). MQTT / Cloud / OTA outcomes are NOT inputs:
// a broker or HTTPS failure can never cause a Wi-Fi action. There is one owner (networkTask) and one escalation path; the old
// per-attempt deep-recovery request, the supervisor "network silent" request and the separate 3-cycle isolation are gone.
#include <stdint.h>

namespace MayapNetwork {

enum class WifiState : uint8_t { Connecting, Connected, Backoff, Recovery };
enum class WifiAction : uint8_t { None, Begin, Recover };

struct WifiFsmConfig {
  uint32_t connectTimeoutMs = 12000U;       // one association attempt
  uint32_t driverGaveUpMs = 4000U;          // the driver reported THIS attempt over (STA_DISCONNECTED) this long ago
  uint8_t recoveryAfterFailures = 6U;       // consecutive failed attempts before the heavy path is allowed
  uint32_t recoveryAfterOutageMs = 300000U; // or one continuous outage this long
  uint32_t recoveryCooldownMs = 120000U;    // never more often than this
};

class WifiFsm {
 public:
  explicit WifiFsm(const uint32_t *ladderMs, uint8_t ladderSteps, const WifiFsmConfig &cfg = WifiFsmConfig())
      : ladder_(ladderMs), steps_(ladderSteps ? ladderSteps : 1U), cfg_(cfg) {}

  // Boot, "back online", portal closed: try at once, forget the old failure history.
  void reset(uint32_t now) {
    state_ = WifiState::Backoff; backoffUntil_ = now; failures_ = 0U; step_ = 0U; outageActive_ = false; outageSince_ = now;
  }
  WifiState state() const { return state_; }
  uint8_t failures() const { return failures_; }
  uint32_t recoveries() const { return recoveries_; }
  uint32_t backoffUntil() const { return backoffUntil_; }
  // The caller has just issued a connect (boot start / plain reconnect): the attempt clock starts now.
  void attemptStarted(uint32_t now) { state_ = WifiState::Connecting; attemptAt_ = now; }
  // The heavy path finished (owners drained, reconnect issued).
  void recoveryDone(uint32_t now) {
    lastRecoveryAt_ = now; recovered_ = true; ++recoveries_; failures_ = 0U; outageSince_ = now;
    attemptStarted(now);
  }

  // The caller could not even issue the connect (driver refused hostname/mode): same as a failed attempt.
  void attemptFailed(uint32_t now, uint32_t jitterMs) {
    if (failures_ < 255U) ++failures_;
    backoffUntil_ = now + ladder_[step_] + jitterMs;
    if (step_ + 1U < steps_) ++step_;
    state_ = WifiState::Backoff;
  }

  // `associated`: the driver says the station is joined and has an IP. `gaveUpAt`: millis() of the driver's last "attempt over"
  // report (0 = none). `jitterMs`: 0..BACKOFF_JITTER_MAX_MS from the caller's random source (0 in tests).
  WifiAction update(uint32_t now, bool associated, uint32_t gaveUpAt, uint32_t jitterMs) {
    if (associated) {
      if (state_ != WifiState::Connected) { state_ = WifiState::Connected; }
      failures_ = 0U; step_ = 0U; outageActive_ = false;
      return WifiAction::None;
    }
    if (state_ == WifiState::Connected) {
      // Real loss. Retry at once; the ladder only starts if that retry fails.
      state_ = WifiState::Backoff; backoffUntil_ = now; outageActive_ = true; outageSince_ = now;
    }
    if (!outageActive_) { outageActive_ = true; outageSince_ = now; }
    switch (state_) {
      case WifiState::Connecting: {
        const bool driverOver = gaveUpAt != 0U && static_cast<int32_t>(gaveUpAt - attemptAt_) >= 0 &&
                                static_cast<uint32_t>(now - gaveUpAt) >= cfg_.driverGaveUpMs;
        if (!driverOver && static_cast<uint32_t>(now - attemptAt_) < cfg_.connectTimeoutMs) return WifiAction::None;
        attemptFailed(now, jitterMs);
        return WifiAction::None;
      }
      case WifiState::Backoff:
        if (static_cast<int32_t>(now - backoffUntil_) < 0) return WifiAction::None;
        if (recoveryDue(now)) { state_ = WifiState::Recovery; return WifiAction::Recover; }
        return WifiAction::Begin;       // caller issues the connect, then calls attemptStarted()
      case WifiState::Recovery: return WifiAction::None;   // the caller owns the transaction until recoveryDone()
      default: return WifiAction::None;
    }
  }

 private:
  bool recoveryDue(uint32_t now) const {
    if (recovered_ && static_cast<uint32_t>(now - lastRecoveryAt_) < cfg_.recoveryCooldownMs) return false;
    return failures_ >= cfg_.recoveryAfterFailures ||
           (outageActive_ && static_cast<uint32_t>(now - outageSince_) >= cfg_.recoveryAfterOutageMs);
  }
  const uint32_t *ladder_;
  uint8_t steps_;
  WifiFsmConfig cfg_;
  WifiState state_ = WifiState::Backoff;
  uint32_t backoffUntil_ = 0U, attemptAt_ = 0U, outageSince_ = 0U, lastRecoveryAt_ = 0U, recoveries_ = 0U;
  uint8_t failures_ = 0U, step_ = 0U;
  bool outageActive_ = false, recovered_ = false;
};

}  // namespace MayapNetwork
