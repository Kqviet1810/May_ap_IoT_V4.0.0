// Restart-in-the-middle-of-a-batch ordering model. REAL code: MayapBoot::splashView / HomeGate (boot_policy.h) and the real
// mayapApplyStartupOutputPolicy (startup_output_policy.h). MODELLED (not the firmware): the controller asking for heat after a recovered
// batch, the HMI acknowledging operations on its first non-splash frame, and the "sensor usable" input. The heater interlocks inside
// MachineController (sensorUsable_, faults, batch/storage permits) are checked textually in tests/staged-boot.test.cjs.
#include "../MAYAP_INDUSTRIAL_v1_0_0/boot_policy.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/startup_output_policy.h"
#include <cassert>
#include <cstdio>
using namespace MayapBoot;
struct Request { bool heaterSsr=true, heatMaster=true, turnLeft=true, turnRight=true, light=true, humidifier=true, immediateMasterDrop=false,
                 circulationFan=false, ventFan=false, ventFanForceOn=false, ventFanBypassTiming=false, siren=false; };
struct Scenario { bool coordinatorAlive; uint32_t sensorUsableAtMs; bool operatorPress; };
int main() {
  const Scenario cases[] = {
    {true, 6000U, false},        // normal: sensor usable at 6 s, Home through the coordinator
    {true, 0xFFFFFFFFU, false},  // sensor never usable, coordinator alive: diagnostic at 30 s, never Home
    {true, 0xFFFFFFFFU, true},   // ... operator presses on the diagnostic screen: Home allowed, heater still locked by the sensor
    {false, 6000U, false},       // coordinator stalled, sensor fine: HMI shows E993 at 32 s, never Home, never "ready"
    {false, 0xFFFFFFFFU, false}, // coordinator stalled AND sensor lost at second 35
    {false, 0xFFFFFFFFU, true},  // stalled coordinator, operator override: Home view only, heater locked by the sensor
  };
  for (const Scenario &sc : cases) {
    HomeGate gate; gate.begin(0U);
    bool released = false, readyShown = false, homeRequested = false, operatorHome = false, opsReady = false;
    uint32_t readyAt = 0U, firstHomeAt = 0U;
    bool everShownReady = false;
    for (uint32_t t = 1U; t <= 120000U; ++t) {
      const bool usable = t >= sc.sensorUsableAtMs;
      if (sc.coordinatorAlive) {
        LocalInputs in; in.tasksHealthy = in.displayHealthy = true; in.sensorUsable = in.temperatureFinite = usable; in.sensorBootReason = usable ? 0U : 1U;
        const HomeGate::Phase ph = gate.update(t, in);
        if (ph == HomeGate::Phase::Diagnostic && homeRequested && !readyShown) { readyShown = true; readyAt = t - READY_DISPLAY_MS; }
        if (!readyShown && ph == HomeGate::Phase::Ready) { readyShown = true; readyAt = t; everShownReady = true; }
        if (readyShown && !released && t - readyAt >= READY_DISPLAY_MS) released = true;
      }
      const bool diagPublished = sc.coordinatorAlive && gate.phase() == HomeGate::Phase::Diagnostic;
      if (sc.operatorPress && t >= 40000U) { if (diagPublished) homeRequested = true; else operatorHome = true; }
      const SplashView v = splashView(t, released, everShownReady && readyShown, diagPublished, operatorHome);
      opsReady = v == SplashView::Home;                          // the HMI acknowledges operations on its first non-splash frame
      if (opsReady && !firstHomeAt) firstHomeAt = t;
      Request r; mayapApplyStartupOutputPolicy(r, opsReady, false, false);   // a recovered batch asks for everything at once
      if (!opsReady) assert(!r.heaterSsr && !r.heatMaster && !r.turnLeft && !r.turnRight && !r.light && !r.humidifier && r.immediateMasterDrop);
      // MachineController: heater needs sensorUsable_ regardless of the screen (normalMasterPermit/heaterPidConditions).
      const bool heaterPermit = opsReady && usable && r.heaterSsr;
      if (!usable) assert(!heaterPermit);
      // never Home (nor ready) before the sensor was usable unless the operator pressed on a diagnostic screen after 40 s
      if (opsReady && !usable) assert(sc.operatorPress && t >= 40000U);
      if (!sc.coordinatorAlive && !sc.operatorPress) assert(!opsReady);        // a dead coordinator never opens Home on its own
      if (v == SplashView::StalledDiagnostic) assert(t >= BOOT_DEADLINE_MS + COORDINATOR_STALL_GRACE_MS && !opsReady);
    }
    if (sc.coordinatorAlive && sc.sensorUsableAtMs == 6000U) assert(firstHomeAt >= 6000U + READY_HOLD_MS && firstHomeAt <= 30000U);
    if (!sc.operatorPress && sc.sensorUsableAtMs == 0xFFFFFFFFU) assert(firstHomeAt == 0U);
  }
  std::puts("Boot safety ordering: restart mid-batch, stalled coordinator at 35 s, lost sensor, operator override - outputs stay safe, Home only via gate PASS");
}
