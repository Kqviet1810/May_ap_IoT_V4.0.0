#pragma once
// Serial log modes. Exactly ONE mode is active at any time (a single variable, not independent switches):
//   Off      nothing is written
//   Machine  the existing text log (diagnostic builds only: "SERIAL" / "LOG", leave with "EXIT")
//   Cpu      "LOG CPU": heap, stack peaks, cycle times, chip temperature every 10 s and CPU load per core (2 s sample) every 60 s
//   Ai       "LOG AI":  one CSV line per second with the quantities needed to train / evaluate a thermal model offline
// Starting a mode ends the previous one and discards its queued lines. After a restart the mode is Off.
//
// What makes it safe to leave in a production build:
//  * the only input a production build accepts is the whitelist below ("LOG CPU", "LOG AI", "LOG OFF", "EXIT"); anything else is ignored
//  * the control task never formats or writes a log line: it copies a small snapshot (and only while "LOG AI" is active); the supervisor task
//    formats at most ONE line per pass and is the only Serial writer (mayapSerialDrain); a full queue drops the line and counts it
//  * if the control cycle stays slow (>= GUARD_CYCLE_US, three consecutive passes) the log switches itself off
//  * nothing here reads or writes outputs, configuration or storage
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "serial_diagnostics.h"
#if defined(ESP_PLATFORM)
#include "system_health.h"
#endif

namespace MayapSerialLog {
enum class Mode : uint8_t { Off = 0, Machine = 1, Cpu = 2, Ai = 3 };
enum class Command : uint8_t { None = 0, Off, Cpu, Ai };

constexpr uint32_t AI_PERIOD_MS = 1000U;
constexpr uint32_t HEADER_PERIOD_MS = 600000U;       // the CSV header is repeated so a logger started later can still decode the stream
constexpr uint32_t CPU_BLOCK_PERIOD_MS = 10000U;
constexpr uint32_t CPU_SAMPLE_PERIOD_MS = 60000U;
constexpr uint32_t GUARD_CYCLE_US = 100000U;         // a quarter of the supervisor's control-deadline trip
constexpr uint8_t GUARD_STREAK = 3U;
constexpr uintptr_t MIN_FREE_STACK = 2048U;         // the supervisor stack (4 KB) must have this much left before a line is formatted
constexpr uint8_t AI_SCHEMA = 1U;

// Flag bits of the "fl" column.
enum : uint16_t { FlBatch = 1U << 0, FlAuto = 1U << 1, FlSensorOk = 1U << 2, FlSsrNow = 1U << 3, FlMaster = 1U << 4, FlCircFan = 1U << 5,
                  FlVentFan = 1U << 6, FlHumidifier = 1U << 7, FlLight = 1U << 8, FlAdaptive = 1U << 9, FlSmart = 1U << 10, FlSiren = 1U << 11,
                  FlTurning = 1U << 12, FlTuning = 1U << 13 };

// Plain copy of the quantities of the AI log (filled by the control task at the runtime-publish cadence, 200 ms).
struct AiSample {
  uint32_t stampMs = 0U;
  uint8_t day = 0U;
  uint16_t flags = 0U;
  float sp = NAN, pv = NAN, raw = NAN, rh = NAN;
  float req = 0.0f, avg60 = 0.0f;            // final heater request [%], 60 s moving average of the REAL SSR on-share [%]
  uint32_t ssrOnMs = 0U;                     // cumulative ms the heater (SSR and contactor) was on, sampled at the publish cadence
  float pidCorr = 0.0f, holdFf = 0.0f, ventFf = 0.0f, limLo = 0.0f, limHi = 100.0f;
  uint8_t learnState = 0U, confidence = 0U, ventPhase = 0U;
  float gain = 0.0f, delaySec = 0.0f, holdPct = 0.0f, coastC = 0.0f, ventGain = 0.0f, predErr = 0.0f;
  uint32_t alarmMask = 0U;
  uint16_t faultCode = 0U;
  uint8_t stateCode = 0U, tuneState = 0U, tuneProgress = 0U;
  float kp = 0.0f, ki = 0.0f, kd = 0.0f;     // configuration, printed in the header block
};

struct State {
  volatile uint8_t mode = (MAYAP_DIAGNOSTIC_SERIAL && SERIAL_DEBUG_DEFAULT_ON) ? 1U : 0U;   // production builds always start Off
  volatile uint32_t epoch = 1U;              // bumped on every mode change: the supervisor restarts its schedule
  AiSample sample;
  bool sampleValid = false;
  const volatile uint32_t *controlCycleUs = nullptr;
  uintptr_t stackBase = 0U;                  // lowest address of the supervisor stack (0 = unknown: the check is skipped)
  uint32_t guardTrips = 0U;
  char fw[16] = "";
  // supervisor-side schedule (only touched by service())
  uint32_t seenEpoch = 0U, nextAiAt = 0U, nextHeaderAt = 0U, nextBlockAt = 0U, nextSampleAt = 0U, seq = 0U;
  uint8_t headerStep = 0U;                   // header lines still to write (one per pass)
  uint8_t guardStreak = 0U;
};
static State gState;
static portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;

inline Mode mode() { return static_cast<Mode>(__atomic_load_n(&gState.mode, __ATOMIC_ACQUIRE)); }
inline bool aiActive() { return mode() == Mode::Ai; }
inline void setIdentity(const char *firmware, const volatile uint32_t *controlCycleUs, const void *supervisorStackBase) {
  snprintf(gState.fw, sizeof(gState.fw), "%s", firmware ? firmware : "");
  gState.controlCycleUs = controlCycleUs;
  gState.stackBase = reinterpret_cast<uintptr_t>(supervisorStackBase);
}

inline void enqueueText(const char *text) { (void)mayapSerialLogEnqueue(text, strlen(text)); }

// The single place where the mode changes. Called from the control task (command handler) and from the supervisor (self-protection).
inline void setMode(Mode m) {
  if (mode() == m) return;
  __atomic_store_n(&gState.mode, static_cast<uint8_t>(m), __ATOMIC_RELEASE);
  mayapSetSerialDebugEnabled(m == Mode::Machine);   // the machine text log is one of the modes
  mayapSerialLogDrop();                              // lines of the previous mode never leak into the new one
  __atomic_add_fetch(&gState.epoch, 1U, __ATOMIC_ACQ_REL);
}
inline void apply(Command c) {
  switch (c) {
    case Command::Off: setMode(Mode::Off); enqueueText("[LOG] OFF\n"); break;
    case Command::Cpu: setMode(Mode::Cpu); enqueueText("[LOG] CPU (heap/stack/cycles every 10 s, load every 60 s; LOG OFF or EXIT to stop)\n"); break;
    case Command::Ai: setMode(Mode::Ai); enqueueText("[LOG] AI (1 CSV line per second; LOG OFF or EXIT to stop)\n"); break;
    case Command::None: break;
  }
}

// ---- command parsing (production whitelist) ------------------------------------------------------------------------------
// `line` is upper-cased by the caller. Exactly "LOG CPU", "LOG AI", "LOG OFF" or "EXIT" (surrounding spaces ignored); anything else is None.
inline Command parseCommand(const char *line) {
  if (!line) return Command::None;
  while (*line == ' ') ++line;
  size_t length = strlen(line);
  while (length && line[length - 1U] == ' ') --length;
  auto is = [&](const char *word) { return strlen(word) == length && !strncmp(line, word, length); };
  if (is("LOG CPU")) return Command::Cpu;
  if (is("LOG AI")) return Command::Ai;
  if (is("LOG OFF") || is("EXIT")) return Command::Off;
  return Command::None;
}

// Byte-wise line reader of the production build (24 bytes, no heap). Returns the parsed command when a line ends, otherwise None.
struct LineReader { char buffer[24]; uint8_t length = 0U; bool discard = false; };
inline Command feedByte(LineReader &r, char c) {
  if (c == '\r') return Command::None;
  if (c == '\n') {
    Command result = Command::None;
    if (!r.discard) {
      r.buffer[r.length] = '\0';
      for (uint8_t i = 0U; i < r.length; ++i) if (r.buffer[i] >= 'a' && r.buffer[i] <= 'z') r.buffer[i] = static_cast<char>(r.buffer[i] - 'a' + 'A');
      result = parseCommand(r.buffer);
    }
    r.length = 0U; r.discard = false;
    return result;
  }
  if (r.discard) return Command::None;
  if (r.length + 1U < sizeof(r.buffer)) r.buffer[r.length++] = c;
  else { r.length = 0U; r.discard = true; }
  return Command::None;
}
#if defined(ESP_PLATFORM)
// Production input: at most 32 bytes per control cycle, only the whitelist is acted upon.
inline void pollInput() {
  static LineReader reader;
  uint8_t budget = 32U;
  while (budget-- && Serial.available() > 0) {
    const Command c = feedByte(reader, static_cast<char>(Serial.read()));
    if (c != Command::None) apply(c);
  }
}
#endif

// ---- AI log ---------------------------------------------------------------------------------------------------------------
inline uint8_t crc8(const char *data, size_t length) {   // CRC-8, polynomial 0x07
  uint8_t crc = 0U;
  for (size_t i = 0; i < length; ++i) {
    crc = static_cast<uint8_t>(crc ^ static_cast<uint8_t>(data[i]));
    for (uint8_t b = 0U; b < 8U; ++b) crc = static_cast<uint8_t>((crc & 0x80U) ? ((crc << 1) ^ 0x07U) : (crc << 1));
  }
  return crc;
}
constexpr char AI_COLUMNS[] =
    "seq,t,age,day,sp,pv,raw,rh,req,ssrms,avg60,fl,pid,hff,vff,lo,hi,ts,tc,tg,td,th,tk,tv,te,vp,al,fc,sc,at,ap,drop,crc\n";
// One data line ending in ",<crc8 of everything before the last comma>\n". Returns the length, 0 if it did not fit.
inline size_t formatAiLine(char *out, size_t capacity, uint32_t seq, uint32_t nowMs, const AiSample &s, uint32_t dropped) {
  const int32_t signedAge = static_cast<int32_t>(nowMs - s.stampMs);   // the control task may publish a moment after the supervisor read the clock
  const uint32_t ageMs = signedAge > 0 ? static_cast<uint32_t>(signedAge) : 0U;
  const int n = snprintf(out, capacity,
      "%lu,%lu,%lu,%u,%.2f,%.3f,%.3f,%.1f,%.1f,%lu,%.1f,%x,%.2f,%.2f,%.2f,%.0f,%.0f,%u,%u,%.4f,%.0f,%.1f,%.3f,%.4f,%.2f,%u,%lx,%u,%u,%u,%u,%lu",
      static_cast<unsigned long>(seq), static_cast<unsigned long>(nowMs), static_cast<unsigned long>(ageMs), static_cast<unsigned>(s.day),
      static_cast<double>(s.sp), static_cast<double>(s.pv), static_cast<double>(s.raw), static_cast<double>(s.rh), static_cast<double>(s.req),
      static_cast<unsigned long>(s.ssrOnMs), static_cast<double>(s.avg60), static_cast<unsigned>(s.flags), static_cast<double>(s.pidCorr),
      static_cast<double>(s.holdFf), static_cast<double>(s.ventFf), static_cast<double>(s.limLo), static_cast<double>(s.limHi),
      static_cast<unsigned>(s.learnState), static_cast<unsigned>(s.confidence), static_cast<double>(s.gain), static_cast<double>(s.delaySec),
      static_cast<double>(s.holdPct), static_cast<double>(s.coastC), static_cast<double>(s.ventGain), static_cast<double>(s.predErr),
      static_cast<unsigned>(s.ventPhase), static_cast<unsigned long>(s.alarmMask), static_cast<unsigned>(s.faultCode), static_cast<unsigned>(s.stateCode),
      static_cast<unsigned>(s.tuneState), static_cast<unsigned>(s.tuneProgress), static_cast<unsigned long>(dropped));
  if (n <= 0 || static_cast<size_t>(n) + 5U >= capacity) return 0U;   // ",XX\n" + terminator
  const int m = snprintf(out + n, capacity - static_cast<size_t>(n), ",%02X\n", static_cast<unsigned>(crc8(out, static_cast<size_t>(n))));
  return m > 0 ? static_cast<size_t>(n) + static_cast<size_t>(m) : 0U;
}
// Control task: copy the snapshot, only while LOG AI is active (one atomic load otherwise).
inline void publishAi(const AiSample &s) {
  if (!aiActive()) return;
  portENTER_CRITICAL(&gMux);
  gState.sample = s;
  gState.sampleValid = true;
  portEXIT_CRITICAL(&gMux);
}

// ---- supervisor side ------------------------------------------------------------------------------------------------------
// Pure decision used by the self-protection (host-tested): true when the slow-cycle streak reaches the limit.
inline bool guardStep(uint32_t cycleUs, uint8_t &streak) {
  if (cycleUs >= GUARD_CYCLE_US) { if (streak < 255U) ++streak; } else streak = 0U;
  return streak >= GUARD_STREAK;
}

inline void serviceAi(State &g, uint32_t now) {
  if (g.headerStep) {                                        // header lines, one per pass
    char text[224];
    AiSample cfg; bool have;
    portENTER_CRITICAL(&gMux); cfg = g.sample; have = g.sampleValid; portEXIT_CRITICAL(&gMux);
    int n = 0;
    if (g.headerStep == 3U)
      n = snprintf(text, sizeof(text), "#MAYAP-AI-LOG schema=%u fw=%s period_ms=%lu ssrms=cumulative heater-on ms (5 Hz sampling) age=snapshot age ms fl=flag bits\n",
                   static_cast<unsigned>(AI_SCHEMA), g.fw, static_cast<unsigned long>(AI_PERIOD_MS));
    else if (g.headerStep == 2U) {
      if (have) n = snprintf(text, sizeof(text), "#cfg sp=%.2f kp=%.2f ki=%.3f kd=%.2f\n", static_cast<double>(cfg.sp), static_cast<double>(cfg.kp),
                             static_cast<double>(cfg.ki), static_cast<double>(cfg.kd));
      else n = snprintf(text, sizeof(text), "#cfg n/a (no snapshot yet)\n");
    } else {
      n = snprintf(text, sizeof(text), "%s", AI_COLUMNS);
    }
    if (n > 0 && mayapSerialLogHasRoom(1U) && mayapSerialLogEnqueue(text, static_cast<size_t>(n))) --g.headerStep;
    return;
  }
  if (static_cast<int32_t>(now - g.nextHeaderAt) >= 0) { g.headerStep = 3U; g.nextHeaderAt = now + HEADER_PERIOD_MS; return; }
  if (static_cast<int32_t>(now - g.nextAiAt) < 0) return;
  g.nextAiAt = now + AI_PERIOD_MS;                           // no catch-up bursts after a stall
  AiSample s; bool have;
  portENTER_CRITICAL(&gMux); s = g.sample; have = g.sampleValid; portEXIT_CRITICAL(&gMux);
  if (!have) return;                                         // no snapshot published yet
  char line[224];
  const size_t length = formatAiLine(line, sizeof(line), g.seq++, now, s, mayapSerialDroppedCount());   // seq gaps + drop= show lost lines
  if (length) (void)mayapSerialLogEnqueue(line, length);     // a full queue drops the line and counts it
}

#if defined(ESP_PLATFORM)
inline void serviceCpu(State &g, uint32_t now) {
  if (g.headerStep) {
    char text[200];
    const int n = g.headerStep == 2U
        ? snprintf(text, sizeof(text), "#MAYAP-CPU-LOG fw=%s block_ms=%lu load_sample_ms=%lu (load = idle-hook estimate over 2 s)\n", g.fw,
                   static_cast<unsigned long>(CPU_BLOCK_PERIOD_MS), static_cast<unsigned long>(CPU_SAMPLE_PERIOD_MS))
        : 0;
    if (g.headerStep == 2U) { if (n > 0 && mayapSerialLogHasRoom(1U) && mayapSerialLogEnqueue(text, static_cast<size_t>(n))) --g.headerStep; }
    else if (mayapSerialLogHasRoom(1U)) { MayapHealth::emitStaticRegions(); --g.headerStep; }
    return;
  }
  if (static_cast<int32_t>(now - g.nextHeaderAt) >= 0) { g.headerStep = 2U; g.nextHeaderAt = now + HEADER_PERIOD_MS; return; }
  if (static_cast<int32_t>(now - g.nextBlockAt) >= 0 && mayapSerialLogHasRoom(3U)) {
    g.nextBlockAt = now + CPU_BLOCK_PERIOD_MS;
    MayapHealth::emitBlock(now, mayapSerialDroppedCount());
    return;
  }
  if (static_cast<int32_t>(now - g.nextSampleAt) >= 0) {
    g.nextSampleAt = now + CPU_SAMPLE_PERIOD_MS;
    MayapHealth::startCpuSample(true);                       // finished (and printed) by MayapHealth::service() two seconds later
  }
}
#endif

// Called by the supervisor task every pass (50 ms), right after mayapSerialDrain(). Bounded: at most one queued entry per call.
inline void service(uint32_t now) {
#if MAYAP_SERIAL_WRITER
  State &g = gState;
  const Mode m = mode();
#if defined(ESP_PLATFORM)
  MayapHealth::service(m == Mode::Cpu);                     // always finishes a running CPU sample: the idle hooks never stay registered
#endif
  if (m != Mode::Cpu && m != Mode::Ai) { g.guardStreak = 0U; return; }
  if (g.controlCycleUs && guardStep(*g.controlCycleUs, g.guardStreak)) {   // the log must never be the reason for a slow control cycle
    ++g.guardTrips;
    g.guardStreak = 0U;
    setMode(Mode::Off);
    char text[80];
    const int n = snprintf(text, sizeof(text), "[LOG] AUTO-OFF: slow control cycle (trips=%lu)\n", static_cast<unsigned long>(g.guardTrips));
    if (n > 0) (void)mayapSerialLogEnqueue(text, static_cast<size_t>(n));
    return;
  }
  if (g.stackBase) {                                         // formatting needs stack: never take it from the watchdog task's reserve
    const uintptr_t sp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    if (sp < g.stackBase || sp - g.stackBase < MIN_FREE_STACK) {
      setMode(Mode::Off);
      enqueueText("[LOG] AUTO-OFF: low supervisor stack\n");
      return;
    }
  }
  const uint32_t epoch = __atomic_load_n(&g.epoch, __ATOMIC_ACQUIRE);
  if (epoch != g.seenEpoch) {                               // a new mode started: restart the schedule
    g.seenEpoch = epoch; g.seq = 0U; g.guardStreak = 0U;
    g.nextAiAt = now; g.nextBlockAt = now; g.nextSampleAt = now + CPU_BLOCK_PERIOD_MS; g.nextHeaderAt = now + HEADER_PERIOD_MS;
    g.headerStep = m == Mode::Ai ? 3U : 2U;
    return;
  }
  if (m == Mode::Ai) serviceAi(g, now);
#if defined(ESP_PLATFORM)
  else serviceCpu(g, now);
#endif
#else
  (void)now;
#endif
}
}  // namespace MayapSerialLog
