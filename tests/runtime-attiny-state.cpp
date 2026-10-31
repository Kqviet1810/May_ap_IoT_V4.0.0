// Actual controller update + actual sync policy, with deterministic bus results.
// The GPIO driver itself is exercised separately by runtime-attiny.cpp.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <deque>
#include <algorithm>
#include <climits>
#include "actual-attiny-config.inc"
#include "../MAYAP_INDUSTRIAL_v1_0_0/attiny_state_sync.h"

static uint32_t clockMs = 1000U;
static uint32_t millis() { return clockMs; }
static void mayapSerialPrintf(bool, const char *, ...) {}
static std::deque<uint8_t> queue;
static uint8_t active = 0U, completed = 0U, incoming = 0U;
static bool ready = false, acked = false, denyRequests = false;
static bool mayapAttinyBusCommandPending(uint8_t code) {
  return active == code || std::find(queue.begin(), queue.end(), code) != queue.end();
}
static bool mayapAttinyBusRequest(uint8_t code) {
  if (denyRequests) return false;
  if (mayapAttinyBusCommandPending(code)) return true;
  if (queue.size() >= 8U) return false;
  queue.push_back(code); return true;
}
static void mayapAttinyBusUpdate(uint32_t) {}
static void mayapAttinyBusHoldTxUntil(uint32_t) {}
static bool mayapAttinyBusTakeResult(uint8_t &code, bool &ok) {
  if (!ready) return false;
  code = completed; ok = acked; ready = false; return true;
}
static uint8_t mayapAttinyBusPollIncoming() { const uint8_t value = incoming; incoming = 0U; return value; }

namespace Mayap {
static uint32_t elapsedMs(uint32_t now, uint32_t then) { return now - then; }
static bool timeReached(uint32_t now, uint32_t target) { return static_cast<int32_t>(now - target) >= 0; }
enum class FaultCode { AttinyBusUnresponsive, SirenBatteryLow, AttinyStateUnsynced };
struct Faults {
  bool link = false, battery = false, sync = false;
  unsigned raises = 0U;
  void set(FaultCode code, bool value, uint32_t, int16_t = 0) {
    if (code == FaultCode::AttinyBusUnresponsive) link = value;
    if (code == FaultCode::SirenBatteryLow) battery = value;
    if (code == FaultCode::AttinyStateUnsynced) {
      if (value && !sync) ++raises;
      sync = value;
    }
  }
};
struct OutputState { bool turnLeft=false, turnRight=false, circulationFan=false, ventFan=false, heatMaster=false; };
struct Outputs { OutputState value; const OutputState &state() const { return value; } };
struct Runtime {
  bool attinyLinkHealthy=false, attinyBatchSynced=false, attinyStatusKnown=false;
  bool attinySirenBatteryLow=false, attinyCriticalActivityArmed=false;
  uint32_t attinyLastStatusAgeSec=0U;
};
struct Controller {
  bool batchRunning_=false, resumePending_=false, emergencyActive_=false;
  uint32_t bootAt_=1000U; bool sirenWanted_=false;
  Outputs outputs_; Faults faults_; Runtime runtime_;
  AttinyStateSync attinyBatchSync_, attinyActivitySync_;
  uint8_t attinySyncFaultDetail_ = 0U;
  bool attinySirenMirrorOn_=false, attinyActivityMirrorOn_=false, attinyActivityDesired_=false;
  bool attinyActivitySynced_=false, attinyTinyActivity_=false, attinyStartupProbePending_=true;
  bool attinyLinkChecked_=false, attinyLinkHealthy_=false, attinyStatusKnown_=false;
  bool attinyTinyBatch_=false, attinyTinySirenOn_=false, attinyBatchSynced_=false;
  bool attiny9vLow_=false, attiny9vCandidate_=false, attiny9vConfirmPending_=false;
  bool attinyStatusAwaiting_=false;
  uint32_t attinyLastStatusQueryAt_=0U, attinyLastStatusAt_=0U, attinyLastResyncAt_=0U;
  uint32_t attinyLastSirenAssertAt_=0U, attinyActivityOffSince_=0U;
  uint32_t attinyStatusDeadline_=0U, attiny9vConfirmAt_=0U;
#include "actual-attiny-controller.inc"
};
} // namespace Mayap
using Mayap::Controller;
static void tick(Controller &m, uint32_t advance = 10U) {
  clockMs += advance; m.updateAttinyLink(0U); // Deliberately stale caller clock.
}
static uint8_t activate() {
  assert(active == 0U && !queue.empty());
  active = queue.front(); queue.pop_front(); return active;
}
static void reply(Controller &m, uint8_t code, uint8_t flags, bool ok = true) {
  if (active == 0U) activate();
  if (active != code) std::fprintf(stderr, "reply expected=%u active=%u at=%lu\n", code, active, static_cast<unsigned long>(clockMs));
  assert(active == code && !ready);
  completed = active; active = 0U; acked = ok; ready = true;
  incoming = ok ? static_cast<uint8_t>(ATTINY_MSG_STATUS_BASE + flags) : 0U;
  tick(m, 350U);
}
static void steady(Controller &m, uint8_t flags = 0U, bool fan = false) {
  queue.clear(); active = completed = incoming = 0U;
  ready = acked = denyRequests = false; clockMs = 1000U;
  m.outputs_.value.circulationFan = fan;
  tick(m);
  if (fan) reply(m, ATTINY_MSG_ACTIVITY_ON, flags);
  reply(m, ATTINY_MSG_STATUS_QUERY, flags);
  assert(!m.faults_.sync && !m.faults_.link && queue.empty());
}
// These only emulate the external user action. updateAttinyLink is extracted
// verbatim from firmware; source-wiring tests also guard the real start/stop hooks.
static void start(Controller &m) {
  m.batchRunning_ = true; m.attinyBatchSync_.expect(true, millis());
  mayapAttinyBusRequest(ATTINY_MSG_BATCH_START);
  mayapAttinyBusRequest(ATTINY_MSG_STATUS_QUERY); tick(m);
}
static void stop(Controller &m, bool cooling = false) {
  m.batchRunning_ = false; m.attinyBatchSync_.expect(false, millis());
  m.outputs_.value.circulationFan = cooling;
  if (cooling) mayapAttinyBusRequest(ATTINY_MSG_ACTIVITY_ON);
  mayapAttinyBusRequest(ATTINY_MSG_BATCH_END); tick(m);
}

int main() {
  unsigned transitions = 0U;
  // A START response confirms batch, but its separate activity bit may still
  // be from the pre-handoff state. Confirm the corrective activity command
  // before calling that intermediate state E503.
  Controller intermediate; steady(intermediate); start(intermediate);
  reply(intermediate, ATTINY_MSG_BATCH_START,
        ATTINY_STATUS_FLAG_BATCH | ATTINY_STATUS_FLAG_ACTIVITY);
  assert(!intermediate.faults_.sync && intermediate.attinyActivitySync_.pending());
  reply(intermediate, ATTINY_MSG_STATUS_QUERY,
        ATTINY_STATUS_FLAG_BATCH | ATTINY_STATUS_FLAG_ACTIVITY);
  reply(intermediate, ATTINY_MSG_ACTIVITY_OFF, ATTINY_STATUS_FLAG_BATCH);
  assert(!intermediate.faults_.sync && !intermediate.attinyActivitySync_.pending());
  stop(intermediate);
  reply(intermediate, ATTINY_MSG_BATCH_END, ATTINY_STATUS_FLAG_ACTIVITY);
  assert(!intermediate.faults_.sync && intermediate.attinyActivitySync_.pending());
  reply(intermediate, ATTINY_MSG_STATUS_QUERY, ATTINY_STATUS_FLAG_ACTIVITY);
  reply(intermediate, ATTINY_MSG_ACTIVITY_OFF, 0U);
  assert(!intermediate.faults_.sync && !intermediate.attinyActivitySync_.pending());
  Controller badActivity; steady(badActivity); start(badActivity);
  reply(badActivity, ATTINY_MSG_BATCH_START,
        ATTINY_STATUS_FLAG_BATCH | ATTINY_STATUS_FLAG_ACTIVITY);
  reply(badActivity, ATTINY_MSG_STATUS_QUERY,
        ATTINY_STATUS_FLAG_BATCH | ATTINY_STATUS_FLAG_ACTIVITY);
  reply(badActivity, ATTINY_MSG_ACTIVITY_OFF,
        ATTINY_STATUS_FLAG_BATCH | ATTINY_STATUS_FLAG_ACTIVITY);
  assert(badActivity.faults_.sync && badActivity.attinyActivitySync_.fault());
  // An outside-batch activity command can still be queued when START arrives.
  // Desired activity remains false on both sides of START, so expect(false)
  // alone does not mark a transition. The old ON reply is not a real fault.
  Controller queuedActivity; steady(queuedActivity);
  mayapAttinyBusRequest(ATTINY_MSG_ACTIVITY_ON);
  start(queuedActivity);
  reply(queuedActivity, ATTINY_MSG_ACTIVITY_ON, ATTINY_STATUS_FLAG_ACTIVITY);
  assert(!queuedActivity.faults_.sync && queuedActivity.attinyActivitySync_.pending());
  reply(queuedActivity, ATTINY_MSG_BATCH_START, ATTINY_STATUS_FLAG_BATCH | ATTINY_STATUS_FLAG_ACTIVITY);
  reply(queuedActivity, ATTINY_MSG_STATUS_QUERY, ATTINY_STATUS_FLAG_BATCH | ATTINY_STATUS_FLAG_ACTIVITY);
  reply(queuedActivity, ATTINY_MSG_ACTIVITY_OFF, ATTINY_STATUS_FLAG_BATCH);
  assert(!queuedActivity.faults_.sync && !queuedActivity.attinyActivitySync_.pending());
  // No transient E503 on ordinary start/stop, even with stale cached status.
  Controller m; steady(m);
  for (unsigned i = 0U; i < 100U; ++i) {
    start(m);
    assert(m.attinyBatchSync_.pending() && !m.runtime_.attinyBatchSynced && !m.faults_.sync);
    reply(m, ATTINY_MSG_BATCH_START, ATTINY_STATUS_FLAG_BATCH);
    reply(m, ATTINY_MSG_STATUS_QUERY, ATTINY_STATUS_FLAG_BATCH);
    stop(m); assert(!m.faults_.sync);
    reply(m, ATTINY_MSG_BATCH_END, 0U); reply(m, ATTINY_MSG_STATUS_QUERY, 0U);
    assert(m.faults_.raises == 0U && !m.attinyBatchSync_.pending()); transitions += 2U;
  }
  // Post-cooling handoff: activity must arm before batch is disarmed.
  Controller cooling; steady(cooling); start(cooling);
  reply(cooling, 1U, 1U); reply(cooling, 5U, 1U); stop(cooling, true);
  assert(queue.front() == ATTINY_MSG_ACTIVITY_ON);
  reply(cooling, 6U, ATTINY_STATUS_FLAG_BATCH | ATTINY_STATUS_FLAG_ACTIVITY);
  assert(!cooling.faults_.sync && cooling.attinyBatchSync_.pending());
  reply(cooling, 2U, ATTINY_STATUS_FLAG_ACTIVITY); reply(cooling, 5U, ATTINY_STATUS_FLAG_ACTIVITY);
  assert(cooling.runtime_.attinyCriticalActivityArmed && cooling.faults_.raises == 0U);
  cooling.outputs_.value.circulationFan = false; tick(cooling);
  tick(cooling, ATTINY_ACTIVITY_OFF_CONFIRM_MS);
  assert(cooling.attinyActivitySync_.pending() && !cooling.faults_.sync);
  reply(cooling, 7U, 0U); reply(cooling, 5U, 0U);
  assert(!cooling.runtime_.attinyCriticalActivityArmed && cooling.faults_.raises == 0U);
  // Starting while manual activity was armed: the intermediate reply still has
  // activity ON until the separate ACTIVITY_OFF command finishes.
  Controller manual; steady(manual, ATTINY_STATUS_FLAG_ACTIVITY, true); start(manual);
  reply(manual, 1U, 9U); reply(manual, 5U, 9U);
  assert(manual.attinyActivitySync_.pending() && !manual.faults_.sync);
  reply(manual, 7U, 1U); assert(manual.faults_.raises == 0U);
  // A heartbeat already in flight is not confirmation of a new batch command.
  Controller old; steady(old); mayapAttinyBusRequest(5U); activate(); start(old);
  reply(old, 5U, 0U); assert(old.attinyBatchSync_.pending() && !old.faults_.sync);
  reply(old, 1U, 1U); assert(!old.attinyBatchSync_.pending() && old.faults_.raises == 0U);
  // START/STOP/START while the first START is active. A matching early reply
  // cannot settle the transition while the old STOP is still queued.
  Controller rapid; steady(rapid); start(rapid); activate(); stop(rapid); start(rapid);
  reply(rapid, 1U, 1U); assert(rapid.attinyBatchSync_.pending() && !rapid.faults_.sync);
  reply(rapid, 5U, 1U); assert(rapid.attinyBatchSync_.pending());
  reply(rapid, 2U, 0U); assert(!rapid.faults_.sync && queue.front() == 1U);
  reply(rapid, 1U, 1U); reply(rapid, 5U, 1U);
  assert(!rapid.attinyBatchSync_.pending() && rapid.faults_.raises == 0U);
  // Genuine wrong status after the requested command must raise E503 at once.
  Controller wrong; steady(wrong); start(wrong); reply(wrong, 1U, 0U);
  assert(wrong.faults_.sync && !wrong.faults_.link && !wrong.runtime_.attinyBatchSynced);
  // A new operation/retry must not hide an already established fault.
  stop(wrong); assert(wrong.faults_.sync);
  reply(wrong, 5U, 0U); assert(wrong.faults_.sync);
  reply(wrong, 2U, 0U); assert(!wrong.faults_.sync);
  // A failed state command still raises E501 and E503; a fresh correct query
  // may recover if Tiny actually applied the command but its reply was lost.
  Controller missing; steady(missing); start(missing); reply(missing, 1U, 0U, false);
  assert(missing.faults_.link && missing.faults_.sync);
  reply(missing, 5U, 1U); assert(!missing.faults_.link && !missing.faults_.sync);
  // A steady-state mismatch is not granted an arbitrary grace period.
  Controller drift; steady(drift); start(drift); reply(drift, 1U, 1U); reply(drift, 5U, 1U);
  mayapAttinyBusRequest(5U); reply(drift, 5U, 0U); assert(drift.faults_.sync);
  // If queue admission fails, E503 is bounded even without any bus result.
  Controller blocked; steady(blocked); denyRequests = true; start(blocked);
  tick(blocked, Mayap::AttinyStateSync::TRANSITION_TIMEOUT_MS - 20U); assert(!blocked.faults_.sync);
  tick(blocked, 20U); assert(blocked.faults_.sync);
  // Policy deadline wrap, late old snapshots, and independent batch/activity.
  Mayap::AttinyStateSync batch, activity;
  batch.expect(false, UINT32_MAX - 100U); batch.expect(true, UINT32_MAX - 90U);
  batch.expect(true, 3000U); // Same desired value must not reset the timer.
  batch.update(7909U); assert(batch.fault() && batch.pending());
  batch.observe(true, false, 7910U); assert(batch.fault() && batch.pending());
  activity.expect(false, 0U); activity.expect(true, 1U); activity.commandFailed();
  batch.observe(true, true, 7920U); assert(!batch.fault() && activity.fault());
  activity.observe(true, true, 7920U); assert(!activity.fault());
  // A boot-time mismatch has no operator-transition grace period.
  queue.clear(); active = completed = incoming = 0U; ready = false;
  denyRequests = false; clockMs = 1000U;
  Controller boot; tick(boot); reply(boot, 5U, ATTINY_STATUS_FLAG_BATCH);
  assert(boot.faults_.sync && !boot.faults_.link);
  // E502's existing two-sample battery confirmation remains independent.
  Controller battery; steady(battery); mayapAttinyBusRequest(5U);
  reply(battery, 5U, ATTINY_STATUS_FLAG_9V_LOW); assert(!battery.faults_.battery);
  tick(battery, ATTINY_9V_CONFIRM_MS);
  reply(battery, 5U, ATTINY_STATUS_FLAG_9V_LOW);
  assert(battery.faults_.battery && !battery.faults_.sync);
  std::printf("Actual Tiny controller: %u normal transitions + cooling/manual handoff, old replies, rapid reversals, wrong flags, failures, deadline and rollover PASS\n", transitions);
}
