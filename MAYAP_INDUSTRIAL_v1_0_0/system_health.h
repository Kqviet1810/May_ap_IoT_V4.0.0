#pragma once
// Serial "HEALTH" command: one on-demand report of the chip's resources (no periodic log, no effect on control).
//  * heap (internal): free / lowest ever / largest block / total
//  * static RAM by region (from linker symbols; printed as n/a if a symbol is missing in this core version)
//  * stack peak use / size of every registered task
//  * control and HMI cycle time (last / max), reset with "HEALTH RESET"
//  * CPU load per core, ESTIMATED over a 2 s window (idle-hook timing, see below)
//  * chip temperature (die sensor: trend only, not the incubator temperature)
// Everything here is read-only diagnostics. The CPU sampler is only registered for the 2 s after the command and removed again, so normal
// operation pays nothing. Off-target (host tests) every function is an empty stub.
#include <stdint.h>

#if defined(ESP_PLATFORM)
#include <Arduino.h>
#include <esp_freertos_hooks.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_chip_info.h>
#include "serial_diagnostics.h"

// Weak so a missing linker symbol resolves to 0 (printed n/a) instead of breaking the link.
extern "C" {
extern char _data_start __attribute__((weak));
extern char _data_end __attribute__((weak));
extern char _bss_start __attribute__((weak));
extern char _bss_end __attribute__((weak));
extern char _iram_start __attribute__((weak));
extern char _iram_end __attribute__((weak));
}

namespace MayapHealth {
constexpr uint8_t MAX_TASKS = 8U;
constexpr int64_t CPU_WINDOW_US = 2000000LL;
// An idle-loop iteration takes a few microseconds; a longer gap between two calls means another task or an interrupt ran in between.
constexpr int64_t IDLE_GAP_US = 100LL;

struct TaskEntry { const char *name; TaskHandle_t handle; uint32_t stackBytes; };
struct State {
  TaskEntry tasks[MAX_TASKS];
  uint8_t taskCount = 0U;
  volatile uint32_t *ctrlLast = nullptr, *ctrlMax = nullptr, *hmiLast = nullptr, *hmiMax = nullptr;
  volatile int64_t idleUs[2] = {0, 0};
  volatile int64_t lastCallUs[2] = {0, 0};
  int64_t windowStartUs = 0;
  bool sampling = false;
};
inline State &state() { static State s; return s; }

// Runs inside the idle task of each core while sampling. Returning true keeps the core from sleeping between ticks, so the loop spins
// and the gaps are measurable (only during the 2 s window).
inline bool idleHook() {
  State &s = state();
  const uint32_t core = static_cast<uint32_t>(xPortGetCoreID()) & 1U;
  const int64_t now = esp_timer_get_time();
  const int64_t last = s.lastCallUs[core];
  if (last != 0 && now - last < IDLE_GAP_US) s.idleUs[core] = s.idleUs[core] + (now - last);
  s.lastCallUs[core] = now;
  return true;
}

inline void addTask(const char *name, TaskHandle_t handle, uint32_t stackBytes) {
  State &s = state();
  if (!handle || s.taskCount >= MAX_TASKS) return;
  s.tasks[s.taskCount++] = TaskEntry{name, handle, stackBytes};
}
inline void setCycleCounters(volatile uint32_t *ctrlLast, volatile uint32_t *ctrlMax,
                             volatile uint32_t *hmiLast, volatile uint32_t *hmiMax) {
  State &s = state();
  s.ctrlLast = ctrlLast; s.ctrlMax = ctrlMax; s.hmiLast = hmiLast; s.hmiMax = hmiMax;
}
inline void resetPeaks() {
  State &s = state();
  if (s.ctrlMax) *s.ctrlMax = 0U;
  if (s.hmiMax) *s.hmiMax = 0U;
}

inline void startCpuSample() {
  State &s = state();
  if (s.sampling) return;
  s.idleUs[0] = 0; s.idleUs[1] = 0; s.lastCallUs[0] = 0; s.lastCallUs[1] = 0;
  s.windowStartUs = esp_timer_get_time();
  s.sampling = true;
  (void)esp_register_freertos_idle_hook_for_cpu(idleHook, 0);
  (void)esp_register_freertos_idle_hook_for_cpu(idleHook, 1);
}

// Called from the control loop's serial service (diagnostic builds only): finishes the 2 s CPU window.
inline void service() {
  State &s = state();
  if (!s.sampling) return;
  const int64_t elapsed = esp_timer_get_time() - s.windowStartUs;
  if (elapsed < CPU_WINDOW_US) return;
  esp_deregister_freertos_idle_hook_for_cpu(idleHook, 0);
  esp_deregister_freertos_idle_hook_for_cpu(idleHook, 1);
  s.sampling = false;
  float busy[2];
  for (uint8_t c = 0; c < 2U; ++c) {
    float idle = 100.0f * static_cast<float>(s.idleUs[c]) / static_cast<float>(elapsed);
    if (idle < 0.0f) idle = 0.0f;
    if (idle > 100.0f) idle = 100.0f;
    busy[c] = 100.0f - idle;
  }
  mayapSerialPrintf(false, "[HEALTH] cpu core0=%.0f%% core1=%.0f%% (estimate over %lu ms; core1 = control + HMI + supervisor, core0 = network/MQTT/cloud/OTA)\n",
                    static_cast<double>(busy[0]), static_cast<double>(busy[1]), static_cast<unsigned long>(elapsed / 1000));
}

inline void printRegion(const char *label, const char *begin, const char *end) {
  if (begin && end && end >= begin)
    mayapSerialPrintf(false, "[HEALTH]   %s = %lu B\n", label, static_cast<unsigned long>(end - begin));
  else
    mayapSerialPrintf(false, "[HEALTH]   %s = n/a\n", label);
}

inline void print(uint32_t nowMs) {
  State &s = state();
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  mayapSerialPrintf(false, "[HEALTH] uptime=%lus cores=%u cpu=%luMHz temp=%.1fC (chip die, trend only)\n",
                    static_cast<unsigned long>(nowMs / 1000UL), static_cast<unsigned>(chip.cores),
                    static_cast<unsigned long>(getCpuFrequencyMhz()), static_cast<double>(temperatureRead()));
  mayapSerialPrintf(false, "[HEALTH] heap internal: free=%lu min=%lu largest=%lu total=%lu B\n",
                    static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                    static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
                    static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                    static_cast<unsigned long>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL)));
  mayapSerialPrintf(false, "[HEALTH] static RAM by region (stacks of the tasks below are inside these):\n");
  printRegion(".data (initialised DRAM)", &_data_start, &_data_end);
  printRegion(".bss  (zeroed DRAM)", &_bss_start, &_bss_end);
  printRegion("IRAM  (code/data in IRAM)", &_iram_start, &_iram_end);
  for (uint8_t i = 0; i < s.taskCount; ++i) {
    const uint32_t freeMin = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(s.tasks[i].handle));
    const uint32_t size = s.tasks[i].stackBytes;
    const uint32_t used = size > freeMin ? size - freeMin : 0U;
    mayapSerialPrintf(false, "[HEALTH]   stack %-5s peak %lu / %lu B (%lu%%) free-min %lu B\n", s.tasks[i].name,
                      static_cast<unsigned long>(used), static_cast<unsigned long>(size),
                      static_cast<unsigned long>(size ? (100UL * used) / size : 0UL), static_cast<unsigned long>(freeMin));
  }
  if (s.ctrlLast && s.ctrlMax && s.hmiLast && s.hmiMax)
    mayapSerialPrintf(false, "[HEALTH] cycle us: control last=%lu max=%lu | hmi last=%lu max=%lu (HEALTH RESET clears max)\n",
                      static_cast<unsigned long>(*s.ctrlLast), static_cast<unsigned long>(*s.ctrlMax),
                      static_cast<unsigned long>(*s.hmiLast), static_cast<unsigned long>(*s.hmiMax));
  mayapSerialPrintf(false, "[HEALTH] sampling CPU for 2 s...\n");
  startCpuSample();
}
}  // namespace MayapHealth

#else  // host build: nothing to measure
namespace MayapHealth {
inline void addTask(const char *, void *, uint32_t) {}
inline void setCycleCounters(volatile uint32_t *, volatile uint32_t *, volatile uint32_t *, volatile uint32_t *) {}
inline void resetPeaks() {}
inline void service() {}
inline void print(uint32_t) {}
}  // namespace MayapHealth
#endif
