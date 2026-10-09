#include "../MAYAP_INDUSTRIAL_v1_0_0/boot_policy.h"
#include <assert.h>
#include <stdio.h>
#include <initializer_list>
using namespace MayapBoot;

static void seal(Diagnostic &d) { d.checksum = checksum(d); }

int main() {
  Diagnostic empty{};
  Diagnostic d = beginDiagnostic(empty, 1U, ResetKind::PowerOn);
  assert(valid(d) && d.recoveryLevel == 0U && !d.bootCompleted);
  Diagnostic slot0 = d, slot1 = d;
  slot0.sequence = UINT32_MAX; seal(slot0);
  slot1.sequence = 0U; seal(slot1);
  assert(newestSlot(slot0, slot1) == 1U); // retained counter rollover
  slot1.checksum ^= 1U;
  assert(newestSlot(slot0, slot1) == 0U); // interrupted write keeps previous slot
  d.lastBootStage = Stage::Wifi;
  seal(d);
  for (uint32_t n = 1U; n <= 5U; ++n) {
    d = beginDiagnostic(d, 7U, ResetKind::Unexpected);
    assert(valid(d) && d.consecutiveFailedBoots == n);
    assert(d.recoveryLevel == (n < 3U ? n : 3U));
    if (n == 1U) assert(d.previousBootStage == Stage::Wifi && d.previousResetReason == 1U);
  }
  // Operator OTA/rollback is intentional; faults raised by firmware still count.
  for (RestartReason reason : {RestartReason::ArduinoOta, RestartReason::InternetOta, RestartReason::Rollback}) {
    d.plannedRestartReason = reason;
    strcpy(d.restartDetail, "operator action");
    seal(d);
    Diagnostic next = beginDiagnostic(d, 3U, ResetKind::Software);
    assert(next.consecutiveFailedBoots == d.consecutiveFailedBoots);
    assert(next.previousPlannedRestartReason == reason && next.plannedRestartReason == RestartReason::None);
    assert(!strcmp(next.previousRestartDetail, "operator action"));
    assert(next.bootCompleted == 0U);
  }
  for (ResetKind kind : {ResetKind::Unexpected, ResetKind::Brownout}) {
    Diagnostic next = beginDiagnostic(d, 9U, kind);
    assert(next.consecutiveFailedBoots == d.consecutiveFailedBoots + 1U);
    assert(next.previousPlannedRestartReason == d.plannedRestartReason); // breadcrumb, not an exemption
  }
  d.plannedRestartReason = RestartReason::HealthMonitor;
  d.bootCompleted = 1U;
  seal(d);
  Diagnostic next = beginDiagnostic(d, 3U, ResetKind::Software);
  assert(next.consecutiveFailedBoots == d.consecutiveFailedBoots + 1U);
  // Success is not a free pass for repeated runtime panic/WDT loops.
  next = beginDiagnostic(d, 7U, ResetKind::Unexpected);
  assert(next.consecutiveFailedBoots == d.consecutiveFailedBoots + 1U);
  next = beginDiagnostic(d, 1U, ResetKind::PowerOn);
  assert(next.consecutiveFailedBoots == 0U);
  d.checksum ^= 1U;
  assert(!valid(d));
  next = beginDiagnostic(d, 7U, ResetKind::Unexpected);
  assert(next.recoveryLevel == 0U);
  d = beginDiagnostic(empty, 1U, ResetKind::PowerOn);
  d.consecutiveFailedBoots = UINT32_MAX;
  seal(d);
  next = beginDiagnostic(d, 7U, ResetKind::Unexpected);
  assert(next.consecutiveFailedBoots == UINT32_MAX && next.recoveryLevel == 3U);

  Stability local;
  local.update(100U, true);
  assert(!local.held(25099U, SUCCESS_STABLE_MS));
  assert(local.held(25100U, SUCCESS_STABLE_MS));
  local.update(25101U, false);
  assert(!local.held(100000U, SUCCESS_STABLE_MS));
  local.update(30000U, true);
  assert(!local.held(54999U, SUCCESS_STABLE_MS));
  assert(local.held(55000U, SUCCESS_STABLE_MS));
  assert(!local.held(629999U, FAILURES_CLEAR_MS));
  assert(local.held(630000U, FAILURES_CLEAR_MS));
  local.update(UINT32_MAX - 100U, false);
  local.update(UINT32_MAX - 100U, true);
  assert(local.held(29899U, SUCCESS_STABLE_MS)); // millis rollover

  for (uint32_t level = 0U; level < 4U; ++level) {
    Sequencer flow;
    flow.begin(level, 0U);
    Stability healthy;
    healthy.update(1000U, true);
    while (flow.stage() != Stage::LocalSettle) flow.advance(1000U);
    assert(!flow.releaseNetwork(1000U + flow.networkDelay() - 1U, healthy));
    assert(flow.releaseNetwork(1000U + flow.networkDelay(), healthy));
    assert(flow.homeBeforeNetwork() == (level >= 2U));
    if (level == 3U) assert(flow.networkDelay() == 45000U);
    if (level == 1U) assert(flow.networkDelay() > LOCAL_SETTLE_MS && flow.serviceGap() > SERVICE_GAP_MS);
    healthy.update(45000U, false);
    assert(!flow.releaseNetwork(90000U, healthy));
    healthy.update(100000U, true);
    assert(flow.releaseNetwork(100000U + flow.networkDelay(), healthy));
    flow.advance(150000U);
    assert(flow.stage() == Stage::Wifi);
    assert(!flow.wifiDone(150000U, true, true));
    assert(!flow.wifiDone(160000U, false, false));
    assert(flow.wifiDone(150000U + flow.serviceGap(), true, false)); // offline/timeout
    flow.advance(160000U);
    assert(flow.stage() == Stage::Mqtt);
    assert(!flow.mqttDone(160000U, true, false));
    assert(flow.mqttDone(160000U + flow.serviceGap(), true, false));
    flow.advance(170000U); assert(flow.stage() == Stage::Cloud);
    flow.advance(180000U); assert(flow.stage() == Stage::Ota);
    flow.advance(190000U); assert(flow.stage() == Stage::Running);
    flow.advance(200000U); assert(flow.stage() == Stage::Running);
  }
  // Level 0 starts the radio tasks 1.5 s after the local tasks are healthy; crash-loop levels keep their long, conservative waits.
  { Sequencer f; f.begin(0U, 0U);
    assert(f.networkDelay() == 1500U && f.serviceGap() == 250U);
    Sequencer g; g.begin(3U, 0U); assert(g.networkDelay() == 45000U && g.serviceGap() == 3000U); }
  // Home gate: valid stable sensor + local safety, else diagnostic after 30 s (never "ready").
  { LocalInputs ok; ok.tasksHealthy = ok.displayHealthy = ok.sensorUsable = ok.temperatureFinite = true; ok.sensorBootReason = 0U;
    LocalInputs bad = ok; bad.sensorUsable = false; bad.sensorBootReason = 1U;
    HomeGate g; g.begin(500U);
    assert(g.update(1000U, bad) == HomeGate::Phase::Waiting && g.reason() == BlockReason::SensorNoResponse && blockCode(g.reason()) == 101U);
    assert(g.update(30499U, bad) == HomeGate::Phase::Waiting);
    assert(g.update(30500U, bad) == HomeGate::Phase::Diagnostic);
    assert(g.update(31000U, ok) == HomeGate::Phase::Diagnostic);                     // must hold READY_HOLD_MS first
    assert(g.update(31000U + READY_HOLD_MS, ok) == HomeGate::Phase::Ready);
    assert(g.update(99999U, bad) == HomeGate::Phase::Ready); }                       // a later sensor loss is an alarm, not a boot problem
  // Splash view: NO timeout of its own. Coordinator stalled, sensor not READY at second 35 -> HMI-local E993 diagnostic, never Home/ready.
  { assert(blockCode(BlockReason::Coordinator) == 993U);
    for (uint32_t t = 0; t < 120000U; t += 250U) {                                   // coordinator dead: released=false, ready=false, no diagnostic
      const SplashView v = splashView(t, false, false, false, false);
      if (t < BOOT_DEADLINE_MS + COORDINATOR_STALL_GRACE_MS) assert(v == SplashView::Logo);
      else assert(v == SplashView::StalledDiagnostic);                                // t=35 s and beyond: always the diagnostic
    }
    assert(splashView(35000U, false, false, false, false) == SplashView::StalledDiagnostic);
    assert(splashView(35000U, false, false, true, false) == SplashView::Diagnostic);  // coordinator alive: its own diagnosis
    assert(splashView(35000U, true, false, false, false) == SplashView::Home);        // only a coordinator release (sensor + safety) opens Home
    assert(splashView(35000U, false, true, false, false) == SplashView::Logo);        // Ready reached: no flicker to a diagnostic during release
    assert(splashView(35000U, false, false, false, true) == SplashView::Home);        // operator override from the E993 screen, never before it
    assert(splashView(10000U, false, false, false, true) == SplashView::Logo);        // a stray press before the deadline cannot skip the gate
    assert(splashView(31999U, false, false, false, true) == SplashView::Logo); }
  puts("boot policy: reset classification, L0-L3, offline admission, stability and rollover PASS");
}
