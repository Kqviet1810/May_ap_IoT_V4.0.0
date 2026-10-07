#pragma once
#include "thermal_profile.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>

// Persistent owner of the thermal profile. Same discipline as MayapAdaptive::ModelStorage
// (which it sits beside, in the same NVS namespace, with its own keys):
//   * the sole flash writer is Arduino loop(), never the control task;
//   * the control task only copies a fixed-size record into a mailbox;
//   * two alternating slots, each written then read back and verified, so a power cut
//     leaves the previous valid profile;
//   * shouldPersistProfile() rate-limits (>= 1 h) and requires a material change, so a
//     slowly drifting estimate never causes continuous flash writes.
namespace MayapThermal {
class ProfileStorage {
 public:
  void offer(const ThermalProfile &profile) {
    portENTER_CRITICAL(&mux_); pending_ = profile; hasPending_ = true; portEXIT_CRITICAL(&mux_);
  }
  void discardPending() { portENTER_CRITICAL(&mux_); hasPending_ = false; portEXIT_CRITICAL(&mux_); }
  bool takeInvalid() {
    portENTER_CRITICAL(&mux_); const bool v = invalidRecord_; invalidRecord_ = false; portEXIT_CRITICAL(&mux_);
    return v;
  }
  bool takeSeed(ThermalProfile &profile) {
    portENTER_CRITICAL(&mux_);
    const bool ready = seedReady_;
    if (ready) { profile = stored_; seedReady_ = false; }
    portEXIT_CRITICAL(&mux_);
    return ready;
  }
  uint32_t saves() const { return saves_; }
  // Host tests only: forget everything (the Preferences stub is cleared by the test).
  void resetForTest() {
    portENTER_CRITICAL(&mux_);
    hasPending_ = seedReady_ = invalidRecord_ = false;
    portEXIT_CRITICAL(&mux_);
    initialized_ = available_ = activeA_ = storedValid_ = false;
    sequence_ = lastSave_ = saves_ = 0;
    pending_ = ThermalProfile{}; stored_ = ThermalProfile{};
  }
  void service(uint32_t now) {
    if (!initialized_) {
      initialized_ = true;
      available_ = prefs_.begin("mayap_thermal", false);
      if (!available_) return;
      ThermalProfile a{}, b{};
      const bool ha = prefs_.isKey("pa"), hb = prefs_.isKey("pb");
      const bool va = ha && prefs_.getBytes("pa", &a, sizeof(a)) == sizeof(a) && validProfile(a);
      const bool vb = hb && prefs_.getBytes("pb", &b, sizeof(b)) == sizeof(b) && validProfile(b);
      if ((ha && !va) || (hb && !vb)) {
        portENTER_CRITICAL(&mux_); invalidRecord_ = true; portEXIT_CRITICAL(&mux_);
      }
      if (va || vb) {
        const bool useA = !vb || (va && static_cast<int32_t>(a.sequence - b.sequence) >= 0);
        activeA_ = useA;
        stored_ = useA ? a : b;
        storedValid_ = true;
        sequence_ = stored_.sequence;
        portENTER_CRITICAL(&mux_); seedReady_ = true; portEXIT_CRITICAL(&mux_);
      }
      lastSave_ = now;
      return;
    }
    if (!available_) return;
    ThermalProfile profile{};
    portENTER_CRITICAL(&mux_);
    const bool pending = hasPending_;
    if (pending) profile = pending_;
    portEXIT_CRITICAL(&mux_);
    if (!pending) return;
    const uint32_t since = now - lastSave_;
    if (!shouldPersistProfile(profile, stored_, storedValid_, since)) return;
    portENTER_CRITICAL(&mux_); hasPending_ = false; portEXIT_CRITICAL(&mux_);
    lastSave_ = now;  // a failed write is wear/backoff bounded as well
    profile.sequence = sequence_ + 1U;
    sealProfile(profile);
    if (!validProfile(profile)) return;
    const char *key = activeA_ ? "pb" : "pa";
    ThermalProfile verify{};
    if (prefs_.putBytes(key, &profile, sizeof(profile)) == sizeof(profile) &&
        prefs_.getBytes(key, &verify, sizeof(verify)) == sizeof(verify) && validProfile(verify) &&
        std::memcmp(&profile, &verify, sizeof(profile)) == 0) {
      activeA_ = !activeA_; sequence_ = profile.sequence; stored_ = profile; storedValid_ = true; ++saves_;
    }
  }

 private:
  Preferences prefs_;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  ThermalProfile pending_{}, stored_{};   // the boot seed is stored_ itself (no third copy)
  bool hasPending_ = false, seedReady_ = false, invalidRecord_ = false;
  bool initialized_ = false, available_ = false, activeA_ = false, storedValid_ = false;
  uint32_t sequence_ = 0, lastSave_ = 0, saves_ = 0;
};
static ProfileStorage profileStorage;
}  // namespace MayapThermal
