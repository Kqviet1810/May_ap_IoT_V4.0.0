// Heat-master latch proposal (D3): transition table, classification and "who may clear it", as executable checks. Design-only module.
#include "../MAYAP_INDUSTRIAL_v1_0_0/heat_latch.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
using namespace MayapSafety;

static int checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

static LatchInput at(uint32_t now) { LatchInput i; i.now = now; i.pv = 37.5f; i.sp = 37.5f; i.heaterOffForMs = 0; return i; }
static LatchInput high(uint32_t now, float duty) { LatchInput i = at(now); i.highEdge = true; i.highActive = true; i.meanCommandedDuty300s = duty; return i; }
static LatchInput emergency(uint32_t now, float duty) { LatchInput i = at(now); i.emergencyEdge = true; i.emergencyActive = true; i.highActive = true; i.meanCommandedDuty300s = duty; return i; }
static LatchInput ackFrom(uint32_t now, AckSource s) { LatchInput i = at(now); i.ack = s; i.pv = 37.0f; i.heaterOffForMs = 600000UL; return i; }

int main() {
  const uint32_t MIN = 60000UL;
  // ---- classification: coast and sensor errors never count -----------------------------------------
  { HeatLatch l; for (int k = 0; k < 50; ++k) { const LatchOutput o = l.update(high(1000 + k * 10 * MIN, 0.60f)); CHECK(o.lastEvent == EventClass::Coast && !o.holdMasterOpen); } }
  { HeatLatch l; for (int k = 0; k < 50; ++k) { LatchInput i = high(1000 + k * 10 * MIN, 0.0f); i.sensorFaultActive = true; const LatchOutput o = l.update(i); CHECK(o.lastEvent == EventClass::SensorError && !o.holdMasterOpen); } }
  // Emergency after a commanded run is the legacy emergency path only: no latch from this module
  { HeatLatch l; const LatchOutput o = l.update(emergency(1000, 0.8f)); CHECK(o.lastEvent == EventClass::Coast && !o.holdMasterOpen); }
  // ---- uncommanded events ----------------------------------------------------------------------------
  { HeatLatch l; CHECK(!l.update(high(1000, 0.0f)).holdMasterOpen);                      // first uncommanded High: counted, not latched
    const LatchOutput o = l.update(high(1000 + 20 * MIN, 0.02f));                        // second within 60 min: latch
    CHECK(o.holdMasterOpen && o.cause == Cause::RepeatedUncommanded && o.persistNow && o.persistedValue); }
  { HeatLatch l; l.update(high(1000, 0.0f)); CHECK(!l.update(high(1000 + 61 * MIN, 0.0f)).holdMasterOpen); }     // outside the window
  { HeatLatch l; const LatchOutput o = l.update(emergency(1000, 0.0f));                  // Emergency + uncommanded history: immediately, no counting
    CHECK(o.holdMasterOpen && o.cause == Cause::EmergencyUncommanded); }
  { HeatLatch l; LatchInput i = at(1000); i.unexplainedHeat = true; const LatchOutput o = l.update(i); CHECK(o.holdMasterOpen && o.cause == Cause::Unexplained); }
  { HeatLatch l; LatchInput i = at(1000); i.unexplainedHeat = true; i.sensorFaultActive = true; CHECK(!l.update(i).holdMasterOpen); }  // the sensor logic owns it
  // ---- clearing: only the local HMI, only when recovered -------------------------------------------------
  { HeatLatch l; l.update(emergency(1000, 0.0f));
    for (AckSource s : {AckSource::Web, AckSource::Mqtt, AckSource::Cloud, AckSource::Watchdog, AckSource::Boot, AckSource::None})
      CHECK(l.update(ackFrom(5000 + 100 * MIN, s)).holdMasterOpen);                      // refused, however good the conditions
    LatchInput hot = ackFrom(6000 + 100 * MIN, AckSource::HmiLocal); hot.pv = 38.2f; CHECK(l.update(hot).holdMasterOpen);          // too warm
    LatchInput busy = ackFrom(7000 + 100 * MIN, AckSource::HmiLocal); busy.heaterOffForMs = 60000UL; CHECK(l.update(busy).holdMasterOpen);   // heater not quiet
    LatchInput stillHigh = ackFrom(8000 + 100 * MIN, AckSource::HmiLocal); stillHigh.highActive = true; CHECK(l.update(stillHigh).holdMasterOpen);
    LatchInput nojournal = ackFrom(9000 + 100 * MIN, AckSource::HmiLocal); nojournal.journalWritable = false;
    LatchOutput o = l.update(nojournal); CHECK(o.holdMasterOpen && !o.ackAccepted);       // cannot record the clear: stays latched
    o = l.update(ackFrom(10000 + 100 * MIN, AckSource::HmiLocal));
    CHECK(!o.holdMasterOpen && o.ackAccepted && o.persistNow && !o.persistedValue && o.state == LatchState::Armed); }
  // ---- power loss: restore before the first update; a reboot is not an acknowledge -----------------------------------
  { HeatLatch l; l.restore(true); const LatchOutput o = l.update(at(1000)); CHECK(o.holdMasterOpen && o.cause == Cause::Restored);
    CHECK(l.update(ackFrom(2000, AckSource::Boot)).holdMasterOpen);
    HeatLatch fresh; fresh.restore(false); CHECK(!fresh.update(at(1000)).holdMasterOpen); }
  // ---- fuzz: OR-only, and nothing but the local HMI ever clears ------------------------------------------------
  { uint32_t s = 0x9E3779B9U; auto rnd = [&s]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
    HeatLatch l; bool wasLatched = false; uint32_t t = 1000; int trips = 0, clears = 0;
    for (int k = 0; k < 300000; ++k) {
      t += 1 + rnd() % 90000U;
      LatchInput i = at(t); i.pv = 36.0f + (rnd() % 400U) * 0.01f; i.heaterOffForMs = (rnd() % 3U) ? 600000UL : 1000UL; i.journalWritable = (rnd() % 50U) != 0;
      i.highEdge = (rnd() % 40U) == 0; i.emergencyEdge = (rnd() % 400U) == 0; i.highActive = i.highEdge || i.emergencyEdge || (rnd() % 30U) == 0; i.emergencyActive = i.emergencyEdge;
      i.meanCommandedDuty300s = (rnd() % 3U) ? 0.0f : 0.5f; i.sensorFaultActive = (rnd() % 7U) == 0; i.unexplainedHeat = (rnd() % 900U) == 0;
      i.ack = static_cast<AckSource>(rnd() % 7U);
      const LatchOutput o = l.update(i);
      if (wasLatched && !o.holdMasterOpen) { CHECK(i.ack == AckSource::HmiLocal && o.ackAccepted); ++clears; }   // only an accepted local acknowledge clears
      if (!wasLatched && o.holdMasterOpen) ++trips;
      CHECK(o.holdMasterOpen == (o.state == LatchState::Latched));
      wasLatched = o.holdMasterOpen;
    }
    CHECK(trips > 20 && clears > 5); }
  std::printf("Heat-master latch proposal (D3, design-only): %d checks PASS\n", checks);
  return 0;
}
