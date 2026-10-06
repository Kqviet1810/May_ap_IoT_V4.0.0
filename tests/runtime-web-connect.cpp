#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../MAYAP_INDUSTRIAL_v1_0_0/realtime_publish_policy.h"

struct Fault { uint8_t severity = 0; };
struct Runtime {
  float temperature = 37.5f, humidity = 58.0f;
  uint32_t alarmMask = 0;
  uint16_t primaryFaultCode = 0;
  uint8_t activeFaultCount = 0, activeFaultDisplayCount = 0;
  Fault activeFaults[1];
  bool batchRunning = false, heaterOn = false, circulationFanOn = false;
  bool ventFanOn = false, humidifierOn = false, lightOn = false, sirenOn = false;
  char machineState[20] = "READY";
};

int main() {
  using namespace MayapRealtimePublish;
  Runtime rt;
  BootstrapCadence cadence;
  auto first = bootstrapState(rt, 7);
  assert(cadence.due(100, first));
  cadence.attempted(100, first, true);
  assert(!cadence.due(2099, first));
  rt.lightOn = true;
  auto changed = bootstrapState(rt, 7);
  assert(!cadence.due(2099, changed));
  assert(cadence.due(2100, changed));
  cadence.attempted(2100, changed, false);
  assert(!cadence.due(2101, changed));
  assert(cadence.due(4100, changed));
  cadence.attempted(4100, changed, true);
  assert(!cadence.due(5000, changed));
  assert(cadence.due(34100, changed));
  cadence.reset();
  assert(cadence.due(5000, changed));
  assert(changed.outputs & 32U);
  assert(changed.revision == 7U);
  std::puts("Generic publish cadence PASS");
}
