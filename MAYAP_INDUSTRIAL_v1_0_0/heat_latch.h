#pragma once
// Heat-master latch: PROPOSAL (D3), design-only module. NOT included by the firmware, NOT wired, NOT approved.
// It exists so that the state table, the classification rules and the "who may clear it" rule are executable and tested BEFORE the safety
// contract is changed. Wiring it would add ONE OR-term to the heat-master permit (a latch can only remove heat) and one persisted
// SafetyJournal key written with the same read-back-verified pattern as the turn-mechanical-check latch.
//
// What it is NOT: it does not replace, delay or gate the Emergency / High / sensor / E115 paths. Those act first, exactly as today.
//
// Classification of an overheating event (what the firmware can actually know):
//   COAST          High after the heater was commanded: mean commanded duty over the preceding 300 s >= CoastDuty. Heat in flight, not a fault.
//   SENSOR_ERROR   a sensor fault (lost / invalid / suspect / frozen / E115) was active around the event: the sensor logic owns it.
//   UNCOMMANDED    High (or Emergency) with the heater commanded (almost) OFF before it: heat that nobody asked for = suspected SSR leak/stuck.
//   UNEXPLAINED    the unexplained-heat detector fired (heater OFF for a long time, PV above SP and still rising), with or without a High.
// Latch rule (parameters are PROPOSALS, to be fixed by the approver, not hard-coded constants of the physics):
//   * UNEXPLAINED fires                                  -> latch
//   * Emergency preceded by an UNCOMMANDED history       -> latch immediately (no counting)
//   * UNCOMMANDED High  x HighCount within HighWindowMs  -> latch
//   * COAST and SENSOR_ERROR events never count towards the latch.
// Clearing: ONLY an operator acknowledge originating from the local HMI, and only while the recovery conditions hold
// (PV below SP + AckMarginC, heater commanded OFF for AckQuietMs, no Emergency/High, journal writable). Web, MQTT, Cloud, the watchdog,
// a reboot and a power cycle can NOT clear it; a reboot restores it from the journal before the heater is ever permitted.
#include <cmath>
#include <cstdint>

namespace MayapSafety {

enum class AckSource : uint8_t { None, HmiLocal, Web, Mqtt, Cloud, Watchdog, Boot };
enum class LatchState : uint8_t { Armed, Latched };
enum class Cause : uint8_t { None, Unexplained, EmergencyUncommanded, RepeatedUncommanded, Restored };
enum class EventClass : uint8_t { None, Coast, SensorError, Uncommanded };

struct LatchParams {
  float coastDuty = 0.15f;              // mean commanded duty before the event at/above which it is a coast
  uint8_t highCount = 2;                // uncommanded High events ...
  uint32_t highWindowMs = 3600000UL;    // ... within this window
  float ackMarginC = 0.5f;              // PV must be below SP + this to accept an acknowledge
  uint32_t ackQuietMs = 300000UL;       // heater commanded OFF for this long before an acknowledge is accepted
};

struct LatchInput {
  uint32_t now = 0;
  bool highEdge = false, emergencyEdge = false, emergencyActive = false, highActive = false;
  float meanCommandedDuty300s = 0.0f;   // from the ACTUAL arbiter command history
  bool sensorFaultActive = false;       // lost / invalid / suspect / frozen / E115 around the event
  bool unexplainedHeat = false;         // detector output
  float pv = NAN, sp = NAN;
  uint32_t heaterOffForMs = 0;
  bool journalWritable = true;
  AckSource ack = AckSource::None;      // an acknowledge request and where it came from
};

struct LatchOutput {
  bool holdMasterOpen = false;          // OR into the heat-master permit: can only remove heat
  bool persistNow = false;              // caller writes the journal key (read-back verified) when this is true
  bool persistedValue = false;
  LatchState state = LatchState::Armed;
  Cause cause = Cause::None;
  EventClass lastEvent = EventClass::None;
  bool ackAccepted = false;
};

class HeatLatch {
 public:
  explicit HeatLatch(const LatchParams &p = LatchParams()) : p_(p) {}
  // Boot: the persisted value is restored BEFORE the first update, so the heater is never permitted while a latch is stored.
  // An unreadable journal is not "no latch": the existing SafetyJournalUnavailable fault already holds the heat off.
  void restore(bool storedLatched) { if (storedLatched) { state_ = LatchState::Latched; cause_ = Cause::Restored; } }

  LatchOutput update(const LatchInput &in) {
    LatchOutput o;
    EventClass ev = EventClass::None;
    if (in.highEdge || in.emergencyEdge) ev = classify(in);
    o.lastEvent = ev;
    if (state_ == LatchState::Armed) {
      bool trip = false; Cause c = Cause::None;
      if (in.unexplainedHeat && !in.sensorFaultActive) { trip = true; c = Cause::Unexplained; }
      if (ev == EventClass::Uncommanded) {
        if (in.emergencyEdge) { trip = true; c = Cause::EmergencyUncommanded; }
        else { pushEdge(in.now); if (countInWindow(in.now) >= p_.highCount) { trip = true; c = Cause::RepeatedUncommanded; } }
      }
      if (trip) { state_ = LatchState::Latched; cause_ = c; o.persistNow = true; o.persistedValue = true; }
    } else if (in.ack != AckSource::None) {
      // Every source except the local HMI is refused, whatever the conditions.
      if (in.ack == AckSource::HmiLocal && recoveryOk(in)) {
        state_ = LatchState::Armed; cause_ = Cause::None; for (uint8_t i = 0; i < kMaxEdges; ++i) edgeT_[i] = 0U; o.persistNow = true; o.persistedValue = false; o.ackAccepted = true;
        if (!in.journalWritable) { state_ = LatchState::Latched; cause_ = Cause::Restored; o.persistNow = false; o.ackAccepted = false; }   // cannot record the clear: stay latched
      }
    }
    o.state = state_; o.cause = cause_; o.holdMasterOpen = state_ == LatchState::Latched;
    return o;
  }
  LatchState state() const { return state_; }

 private:
  EventClass classify(const LatchInput &in) const {
    if (in.sensorFaultActive) return EventClass::SensorError;
    if (in.meanCommandedDuty300s >= p_.coastDuty) return EventClass::Coast;
    return EventClass::Uncommanded;
  }
  bool recoveryOk(const LatchInput &in) const {
    if (in.emergencyActive || in.highActive) return false;
    if (!std::isfinite(in.pv) || !std::isfinite(in.sp)) return false;
    if (in.pv >= in.sp + p_.ackMarginC) return false;
    return in.heaterOffForMs >= p_.ackQuietMs;
  }
  void pushEdge(uint32_t now) {
    for (uint8_t i = 0; i < kMaxEdges; ++i) if (edgeT_[i] != 0U && static_cast<uint32_t>(now - edgeT_[i]) > p_.highWindowMs) edgeT_[i] = 0U;
    for (uint8_t i = 0; i < kMaxEdges; ++i) if (edgeT_[i] == 0U) { edgeT_[i] = now ? now : 1U; return; }
    edgeT_[0] = now ? now : 1U;
  }
  uint8_t countInWindow(uint32_t now) const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < kMaxEdges; ++i) if (edgeT_[i] != 0U && static_cast<uint32_t>(now - edgeT_[i]) <= p_.highWindowMs) ++n;
    return n;
  }
  static constexpr uint8_t kMaxEdges = 8;
  LatchParams p_;
  LatchState state_ = LatchState::Armed;
  Cause cause_ = Cause::None;
  uint32_t edgeT_[kMaxEdges] = {};
};

}  // namespace MayapSafety
