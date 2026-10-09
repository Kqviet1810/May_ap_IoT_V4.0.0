// Host test of the REAL TX arbiter code of the bridge. drainAckOutbox, serviceLiveSnapshot and the lane order of mayapRealtimeUpdate are
// sliced out of transaction_bridge.h by tools/test_runtime_buses.py (no model copy); the wire is a fake that is writable only at the
// moments a test scripts, and takes a scripted number of frames before it closes again (a nearly full TCP send window).
//
// What the field log showed: a 1-2 KB snapshot, retried on every pass, won every writable moment while the 300-byte terminal ack of a
// command waited in a back-off timer - so the Web saw its command acknowledged seconds late (a priority inversion), and a closed wire
// still cost the whole JSON document per retry. These tests pin the opposite behaviour.
#include <cassert>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static uint32_t clockMs = 100000U;
uint32_t millis() { return clockMs; }
static int realtimeMux = 0;
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
static std::vector<std::string> logLines;
void mayapSerialPrintf(bool, const char *format, ...) {
  char text[400];
  va_list args; va_start(args, format); vsnprintf(text, sizeof(text), format, args); va_end(args);
  logLines.push_back(text);
}
#include "mqtt_tx_arbiter.h"

constexpr uint8_t COMMAND_QUEUE_SIZE = 4U;
constexpr size_t REALTIME_REQUEST_ID_CAPACITY = 40U;
constexpr uint32_t REALTIME_SNAPSHOT_ACTIVE_INTERVAL_MS = 1000UL, REALTIME_SNAPSHOT_IDLE_INTERVAL_MS = 120000UL;
struct MachineRuntime { int unused; };
struct MayapNetworkBatchOperation { explicit operator bool() const { return true; } };

namespace MayapRealtimeInternal {
inline bool timeReached(uint32_t now, uint32_t target) { return static_cast<int32_t>(now - target) >= 0; }
#include "actual-arbiter-outbox.inc"   // AckOutboxItem + the ackOutbox array, as in production

static bool webSessionActive = true, forceSnapshotPublish = false;
static uint32_t lastSnapshotPublishAt = 0U, realtimeConfigRevision = 0U;
static bool knownRuntimeValid = true;
static MachineRuntime knownRuntime{};
static MayapTx::SnapshotPacer snapshotPacer;
static MayapTx::BridgeStats arbiterStats;

// ---- the fake wire ------------------------------------------------------------------------------------------------------------
static bool wireOpen = false;           // the socket is writable
static unsigned frameBudget = 0U;       // frames the wire takes before it closes again
static std::vector<std::string> published;
static unsigned snapshotsBuilt = 0U, acksBuilt = 0U, gateAsks[MayapTx::LANE_COUNT] = {};
static bool ackAlwaysFails = false;
static unsigned configCalls = 0U, logCalls = 0U, historyCalls = 0U;
static bool fakeGate(uint8_t lane) { ++gateAsks[lane]; return wireOpen && frameBudget > 0U; }
using TxGateCallback = bool (*)(uint8_t);
static TxGateCallback txGateCallback = fakeGate;
inline bool txGateOpen(MayapTx::Lane lane) { return txGateCallback == nullptr || txGateCallback(static_cast<uint8_t>(lane)); }

// What building and sending a frame costs: a frame is only built after the gate said yes (the code under test is responsible for that).
inline bool publishAck(const char *requestId, const char *, const char *, const char *, uint32_t, uint32_t, const uint8_t *) {
  ++acksBuilt;
  if (ackAlwaysFails || !wireOpen || frameBudget == 0U) return false;
  --frameBudget;
  published.push_back(std::string("ack:") + requestId);
  return true;
}
inline bool publishSnapshot(const MachineRuntime &, uint32_t) {
  ++snapshotsBuilt;
  if (!wireOpen || frameBudget == 0U) return false;
  --frameBudget;
  published.push_back("snapshot");
  return true;
}
inline void serviceSessionTimeout(uint32_t) {}
inline void expirePendingCommands(uint32_t) {}
inline void serviceConfigPublish() { ++configCalls; }
inline void serviceEventLogPublish() { ++logCalls; }
inline void serviceHistoryResponse() { ++historyCalls; }

#include "actual-arbiter-ack.inc"
#include "actual-arbiter-snapshot.inc"
}  // namespace MayapRealtimeInternal
#include "actual-arbiter-update.inc"

using namespace MayapRealtimeInternal;

static void enqueueAck(const char *id) {
  for (AckOutboxItem &slot : ackOutbox) {
    if (slot.used) continue;
    slot = AckOutboxItem{};
    slot.used = true;
    snprintf(slot.requestId, sizeof(slot.requestId), "%s", id);
    snprintf(slot.result, sizeof(slot.result), "applied");
    slot.completedAt = millis();
    return;
  }
  assert(false && "ack outbox full");
}
static unsigned outboxCount() { unsigned n = 0U; for (const AckOutboxItem &slot : ackOutbox) if (slot.used) ++n; return n; }
static void reset() {
  for (AckOutboxItem &slot : ackOutbox) slot = AckOutboxItem{};
  webSessionActive = true; forceSnapshotPublish = false; lastSnapshotPublishAt = 0U; knownRuntimeValid = true;
  snapshotPacer = MayapTx::SnapshotPacer(); arbiterStats = MayapTx::BridgeStats();
  wireOpen = false; frameBudget = 0U; published.clear(); snapshotsBuilt = acksBuilt = 0U;
  for (unsigned &n : gateAsks) n = 0U;
  ackAlwaysFails = false; configCalls = logCalls = historyCalls = 0U;
  ackWaitSince = ackRetryAt = ackLogAt = ackRefusedTotal = ackRefusedLogged = 0U; snapshotRetryAt = 0U;
  logLines.clear();
  clockMs = 100000U;
}
static void pass(uint32_t advance = 20U) { clockMs += advance; mayapRealtimeUpdate(clockMs); }

int main() {
  // 1. The priority inversion. A command finished at t=0 and its terminal ack waits; the snapshot also comes due. The wire is closed for
  //    a while and nothing is built for it (gate before build); then it opens for ONE frame only (a nearly full send window): that
  //    frame must be the ack, and only the next opening may carry the snapshot.
  reset();
  lastSnapshotPublishAt = clockMs;                       // a snapshot was just sent
  enqueueAck("R1");
  for (int i = 0; i < 60; ++i) pass();                   // 1.2 s with the wire closed: the ack waits, the snapshot is due from t=1 s
  assert(published.empty());
  assert(snapshotsBuilt == 0U && acksBuilt == 0U);       // a closed wire costs no JSON, no HMAC
  assert(outboxCount() == 1U && arbiterStats.ackWireWaits > 0U);
  wireOpen = true; frameBudget = 1U;
  pass();
  assert(published.size() == 1U && published[0] == "ack:R1");                // the ack won the only writable frame
  assert(outboxCount() == 0U && arbiterStats.ackSent == 1U);
  assert(snapshotsBuilt == 0U);                                              // the snapshot did not even try
  assert(snapshotPacer.congestionEvents() == 1U && snapshotPacer.extraMs() == MayapTx::SNAPSHOT_GAP_STEP_UP_MS);   // the closed wire stretched it one step
  frameBudget = 1U;
  pass(400U);                                                                 // base 1 s + the 0.5 s allowance have passed since the last snapshot
  assert(published.size() == 2U && published[1] == "snapshot");               // next opening: the snapshot, which was held back
  assert(arbiterStats.snapshotsSent == 1U);

  // 2. Retry is edge driven, never timer driven: the ack goes out on the FIRST pass after the wire opens, however long it was closed.
  reset();
  enqueueAck("R2");
  for (int i = 0; i < 400; ++i) pass(25U);               // 10 s closed: no back-off timer may have been built up
  assert(published.empty() && outboxCount() == 1U);
  wireOpen = true; frameBudget = 4U;
  pass(1U);
  assert(published.size() >= 1U && published[0] == "ack:R2" && outboxCount() == 0U);

  // 3. Lanes below the ack stand aside while it waits (config, log, history, snapshot), but never longer than ACK_PRIORITY_MAX_MS.
  reset();
  lastSnapshotPublishAt = clockMs - 5000U;               // snapshot long overdue
  ackAlwaysFails = true; wireOpen = true; frameBudget = 100U;   // gate open, but the ack publish itself fails (e.g. link closing under us)
  enqueueAck("R3");
  const unsigned configBefore = configCalls;
  for (int i = 0; i < 100; ++i) pass(20U);               // 2 s
  assert(published.empty() && snapshotsBuilt == 0U);     // everything below the ack stood aside
  assert(configCalls == configBefore && logCalls == 0U && historyCalls == 0U);
  assert(arbiterStats.laneAckSkips > 0U && arbiterStats.snapshotAckSkips > 0U);
  for (int i = 0; i < 40; ++i) pass(20U);                // past 2.5 s: the hold expires, telemetry flows again
  assert(configCalls > configBefore && snapshotsBuilt >= 1U && !published.empty() && published.back() == "snapshot");
  assert(outboxCount() == 1U);                           // the failing ack is kept, not lost
  assert(arbiterStats.ackPublishFailed > 0U && arbiterStats.ackPublishFailed < 60U);   // paused 100 ms between failures: no storm
  {
    bool failLogged = false; for (const std::string &l : logLines) if (l.find("ACK PUB FAIL") != std::string::npos) failLogged = true;
    assert(failLogged);
  }

  // 4. Snapshot pacing. Closed wire when a snapshot is due = one congestion episode, however many passes it lasts; the interval grows
  //    one step per episode (never past SNAPSHOT_MAX_GAP_MS) and shrinks again after clean sends. Nothing is built while closed.
  reset();
  lastSnapshotPublishAt = clockMs;
  for (int episode = 1; episode <= 3; ++episode) {
    const unsigned builtBefore = snapshotsBuilt;
    for (int i = 0; i < 160; ++i) pass(20U);             // closed throughout (3.2 s): due at base+allowance, never built
    assert(snapshotsBuilt == builtBefore);
    assert(snapshotPacer.congestionEvents() == static_cast<uint32_t>(episode));   // one episode per closed stretch, not one per pass
    wireOpen = true; frameBudget = 1U;
    pass(20U);                                           // opens: sent (closes the episode)
    assert(snapshotsBuilt == builtBefore + 1U);
    wireOpen = false; frameBudget = 0U;
  }
  assert(snapshotPacer.extraMs() >= MayapTx::SNAPSHOT_GAP_STEP_UP_MS);
  assert(snapshotPacer.extraMs() <= MayapTx::SNAPSHOT_MAX_GAP_MS);
  assert(snapshotPacer.allowance(REALTIME_SNAPSHOT_ACTIVE_INTERVAL_MS) + REALTIME_SNAPSHOT_ACTIVE_INTERVAL_MS <= MayapTx::SNAPSHOT_MAX_GAP_MS);
  // clean sends: the wire is open whenever the snapshot is due -> the allowance melts away
  wireOpen = true;
  const uint32_t stretched = snapshotPacer.extraMs();
  for (int i = 0; i < 4000 && snapshotPacer.extraMs() > 0U; ++i) { frameBudget = 1U; pass(20U); }
  assert(snapshotPacer.extraMs() == 0U && stretched > 0U);

  // 5. A forced snapshot (a command just finished) skips the interval but not the minimum gap: five results within the window share one.
  reset();
  wireOpen = true; frameBudget = 100U; lastSnapshotPublishAt = clockMs - 10000U; forceSnapshotPublish = true;
  pass(1U);
  assert(snapshotsBuilt == 1U && !forceSnapshotPublish);
  for (int i = 0; i < 5; ++i) { forceSnapshotPublish = true; pass(20U); }      // 100 ms of results
  assert(snapshotsBuilt == 1U && forceSnapshotPublish);                         // still waiting for the minimum gap
  pass(MayapTx::SNAPSHOT_FORCED_MIN_GAP_MS);
  assert(snapshotsBuilt == 2U && !forceSnapshotPublish);

  // 6. A snapshot whose publish fails although the wire said "open" is not rebuilt on every pass.
  reset();
  lastSnapshotPublishAt = clockMs - 10000U;
  // The gate says "open" but the frame is refused anyway (the link closed under us): the publisher sees a closed wire.
  struct Toggle { static bool gateAlwaysOpen(uint8_t) { return true; } };
  txGateCallback = Toggle::gateAlwaysOpen; wireOpen = false;
  for (int i = 0; i < 20; ++i) pass(10U);                // 200 ms
  assert(snapshotsBuilt <= 3U);                          // retried at most every SNAPSHOT_FAILED_RETRY_MS
  txGateCallback = fakeGate;

  // 7. With no transport attached (host tests, other transports) the gate is always open.
  reset();
  txGateCallback = nullptr; wireOpen = true; frameBudget = 100U; enqueueAck("R7");
  pass(20U);
  assert(!published.empty() && published[0] == "ack:R7");
  txGateCallback = fakeGate;

  // 8. Pure policy pieces.
  {
    MayapTx::PubackQueue q;
    for (uint16_t i = 1U; i <= MayapTx::PUBACK_QUEUE; ++i) assert(q.push(i));
    assert(!q.push(99U) && q.rejected() == 1U && q.size() == MayapTx::PUBACK_QUEUE && q.high() == MayapTx::PUBACK_QUEUE);
    for (uint16_t i = 1U; i <= MayapTx::PUBACK_QUEUE; ++i) { assert(q.front() == i); q.pop(); }
    assert(q.empty() && q.push(7U) && q.front() == 7U);                    // wraps correctly after a full cycle
    assert(MayapTx::ackHolds(1000U, 1000U + MayapTx::ACK_PRIORITY_MAX_MS - 1U) && !MayapTx::ackHolds(1000U, 1000U + MayapTx::ACK_PRIORITY_MAX_MS));
    assert(!MayapTx::ackHolds(0U, 5U));
  }

  printf("realtime arbiter host tests PASS\n");
  return 0;
}
