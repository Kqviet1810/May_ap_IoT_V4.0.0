// Serial log modes (LOG CPU / LOG AI): the REAL serial_diagnostics.h queue and the REAL serial_log.h, driven against a fake Serial.
// Default build = production profile (MAYAP_DIAGNOSTIC_SERIAL 0): only the log channel and the whitelist exist. serial-log-diag.cpp builds the same test as a diagnostic build.
#include <cassert>
#include <cstdint>
#include <cstdarg>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <vector>
#ifndef MAYAP_DIAGNOSTIC_SERIAL
#define MAYAP_DIAGNOSTIC_SERIAL 0
#endif
#define SERIAL_DEBUG_DEFAULT_ON false
#define portMUX_INITIALIZER_UNLOCKED 0
using portMUX_TYPE = int;
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define vTaskDelay(x) ((void)(x))
static uint32_t clockMs = 100;
uint32_t millis() { return clockMs; }
struct FakeSerial {
  int room = 64; size_t maxWrite = 64; std::string output;
  int availableForWrite() { return room; }
  size_t write(const uint8_t *p, size_t n) { const size_t sent = std::min(maxWrite, n); output.append(reinterpret_cast<const char *>(p), sent); return sent; }
} Serial;
#include "actual-serial_diagnostics.inc"
#include "actual-serial_log.inc"
using namespace MayapSerialLog;

static void drainAll() { Serial.room = 64; Serial.maxWrite = 64; for (unsigned i = 0; i < 64; ++i) mayapSerialDrain(); }
static std::vector<std::string> lines(const std::string &text) {
  std::vector<std::string> out; size_t at = 0;
  while (at < text.size()) { const size_t end = text.find('\n', at); if (end == std::string::npos) break; out.push_back(text.substr(at, end - at)); at = end + 1; }
  return out;
}
static size_t commas(const std::string &s) { return static_cast<size_t>(std::count(s.begin(), s.end(), ',')); }
static AiSample sample() {
  AiSample s; s.stampMs = clockMs; s.day = 7; s.flags = FlBatch | FlAuto | FlSensorOk | FlMaster | FlAdaptive; s.sp = 37.5f; s.pv = 37.412f; s.raw = 37.4f; s.rh = 55.5f;
  s.req = 31.5f; s.avg60 = 29.8f; s.ssrOnMs = 123456789U; s.pidCorr = -1.25f; s.holdFf = 27.9f; s.ventFf = 0.5f; s.limLo = 0; s.limHi = 100;
  s.learnState = 3; s.confidence = 83; s.ventPhase = 1; s.gain = 0.0083f; s.delaySec = 23; s.holdPct = 27.97f; s.coastC = 0.19f; s.ventGain = 0.0012f; s.predErr = 0.36f;
  s.alarmMask = 0x80000001U; s.faultCode = 501; s.stateCode = 6; s.tuneState = 1; s.tuneProgress = 40; s.kp = 18; s.ki = 0.8f; s.kd = 45;
  return s;
}
static void run(uint32_t fromMs, uint32_t toMs) {   // the supervisor loop: every 50 ms drain, then service
  for (uint32_t t = fromMs; t <= toMs; t += 50U) { clockMs = t; mayapSerialDrain(); service(t); }
}

int main() {
  // ---- whitelist -------------------------------------------------------------------------------------------------------
  assert(parseCommand("LOG CPU") == Command::Cpu && parseCommand("  LOG AI  ") == Command::Ai);
  assert(parseCommand("LOG OFF") == Command::Off && parseCommand("EXIT") == Command::Off);
  for (const char *bad : {"", "LOG", "LOG SHOW", "LOG CPU X", "LOGCPU", "TIME SET 2026-01-01 00:00:00", "BATCH START", "IN AUTO ON", "log cpu", "LOG  CPU", "FAULT CLEAR", "CONFIG"})
    assert(parseCommand(bad) == Command::None);
  { LineReader r; Command c = Command::None;
    for (char ch : std::string("log ai\r\n")) c = feedByte(r, ch);              // lower case and CR/LF are accepted
    assert(c == Command::Ai);
    for (char ch : std::string("BATCH START\n")) assert(feedByte(r, ch) == Command::None);
    Command last = Command::None; for (unsigned i = 0; i < 200; ++i) last = feedByte(r, 'A');   // overflow: discarded until the newline
    assert(last == Command::None && feedByte(r, '\n') == Command::None);
    for (char ch : std::string("LOG CPU\n")) c = feedByte(r, ch);                // the reader recovers
    assert(c == Command::Cpu); }

  // ---- exactly one mode ----------------------------------------------------------------------------------------------------
  assert(mode() == Mode::Off);
  apply(Command::Cpu); assert(mode() == Mode::Cpu);
  mayapSerialLogEnqueue("old cpu line\n", 13);
  apply(Command::Ai); assert(mode() == Mode::Ai);
  drainAll();
  assert(Serial.output.find("old cpu line") == std::string::npos);                   // lines of the previous mode never leak
  assert(Serial.output.find("[LOG] AI") != std::string::npos);
  Serial.output.clear();
  apply(Command::Off); assert(mode() == Mode::Off);
  drainAll(); assert(Serial.output == "[LOG] OFF\n");
  Serial.output.clear();
  service(clockMs);                                                                  // Off: nothing is produced
  drainAll(); assert(Serial.output.empty());
  assert(!mayapSerialDebugEnabled());

  // ---- AI line ---------------------------------------------------------------------------------------------------------------
  { char line[224]; AiSample s = sample(); clockMs = 5000;
    const size_t n = formatAiLine(line, sizeof(line), 42, clockMs, s, 3);
    assert(n > 0 && n < 224 && line[n - 1] == '\n');
    std::string text(line, n - 1);
    const size_t lastComma = text.rfind(',');
    assert(commas(text) == commas(AI_COLUMNS));                                      // same number of fields as the header
    assert(strtoul(text.c_str() + lastComma + 1, nullptr, 16) == crc8(text.c_str(), lastComma));
    assert(text.rfind("42,5000,", 0) == 0);
    // worst realistic widths still fit; NaN is printed, never crashes
    AiSample w = sample(); w.sp = 99.99f; w.pv = -99.999f; w.raw = 999.999f; w.rh = 100; w.req = 100; w.avg60 = 100; w.ssrOnMs = 4294967295U; w.pidCorr = -999.99f;
    w.holdFf = 100; w.ventFf = 100; w.limLo = 100; w.limHi = 100; w.gain = 0.9999f; w.delaySec = 9999; w.holdPct = 100; w.coastC = 99.999f; w.ventGain = 0.9999f; w.predErr = 99.99f;
    w.alarmMask = 0xFFFFFFFFU; w.faultCode = 65535; w.stateCode = 255; w.tuneState = 255; w.tuneProgress = 255; w.flags = 0xFFFF; w.learnState = 255; w.confidence = 255; w.ventPhase = 255; w.day = 255;
    assert(formatAiLine(line, sizeof(line), 4294967295U, 4294967295U, w, 4294967295U) > 0);
    AiSample nan = sample(); nan.pv = NAN; nan.raw = NAN; nan.rh = NAN; assert(formatAiLine(line, sizeof(line), 1, 100, nan, 0) > 0);
    char tiny[16]; assert(formatAiLine(tiny, sizeof(tiny), 1, 1, s, 0) == 0); }       // does not fit: refused, no overflow

  // ---- streaming through the supervisor service -----------------------------------------------------------------------------
  clockMs = 10000; Serial.output.clear();
  apply(Command::Ai); drainAll(); Serial.output.clear();
  publishAi(sample());                                                               // first snapshot after the mode started
  run(10050, 13650);
  { auto ls = lines(Serial.output);
    assert(ls.size() >= 6 && ls[0].rfind("#MAYAP-AI-LOG schema=1 fw=", 0) == 0);
    assert(ls[1].rfind("#cfg sp=37.50 kp=18.00", 0) == 0 && ls[2] + "\n" == std::string(AI_COLUMNS));
    unsigned data = 0; unsigned long expectSeq = 0;
    for (size_t i = 3; i < ls.size(); ++i) {
      assert(ls[i][0] != '#' && ls[i][0] != '[');
      const size_t lastComma = ls[i].rfind(',');
      assert(strtoul(ls[i].c_str() + lastComma + 1, nullptr, 16) == crc8(ls[i].c_str(), lastComma));
      assert(strtoul(ls[i].c_str(), nullptr, 10) == expectSeq++); ++data;
    }
    assert(data >= 3 && data <= 4); }                                                // one line per second, not faster
  // a live snapshot updates the line
  { AiSample s = sample(); s.pv = 38.0f; s.stampMs = clockMs; publishAi(s); Serial.output.clear(); run(clockMs + 50, clockMs + 1200);
    assert(Serial.output.find(",38.000,") != std::string::npos); }
  // snapshots are ignored unless LOG AI is the active mode
  apply(Command::Cpu); { AiSample s = sample(); s.pv = 11.111f; publishAi(s); } apply(Command::Ai);
  { Serial.output.clear(); run(clockMs + 50, clockMs + 1300); assert(Serial.output.find(",11.111,") == std::string::npos); }

  // ---- nobody reads the port: bounded, counted, no growth ------------------------------------------------------------------
  Serial.output.clear(); drainAll(); Serial.output.clear();
  const uint32_t droppedBefore = mayapSerialDroppedCount();
  Serial.room = 0;
  const uint32_t t0 = clockMs;
  for (uint32_t t = t0 + 50; t < t0 + 20000; t += 50) { clockMs = t; mayapSerialDrain(); service(t); publishAi(sample()); }
  assert(mayapSerialDroppedCount() > droppedBefore);                                 // lines were dropped, not queued without bound
  assert(mayapSerialLogHasRoom(1) == false);
  drainAll();
  { auto ls = lines(Serial.output); assert(ls.size() <= MayapSerialInternal::CAPACITY); }                       // at most the queue capacity ever reaches the port
  Serial.output.clear();
  clockMs += 100; service(clockMs); drainAll();
  run(clockMs + 50, clockMs + 2500);                                                 // later lines report drop= and the sequence gap
  { auto ls = lines(Serial.output); bool gap = false;
    for (auto &l : ls) if (l[0] != '#' && l[0] != '[') { const std::string f = l.substr(0, l.rfind(',')); const size_t p = f.rfind(','); if (strtoul(f.c_str() + p + 1, nullptr, 10) > droppedBefore) gap = true; }
    assert(gap); }

  // ---- self-protection ---------------------------------------------------------------------------------------------------------
  { uint8_t streak = 0;
    assert(!guardStep(5000, streak) && !guardStep(150000, streak) && !guardStep(150000, streak) && streak == 2);
    assert(guardStep(150000, streak));
    assert(!guardStep(5000, streak) && streak == 0); }
  { static volatile uint32_t cycle = 1000; setIdentity("1.1.9", &cycle, nullptr);
    apply(Command::Ai); drainAll(); Serial.output.clear(); publishAi(sample());
    cycle = 150000; clockMs += 50; service(clockMs); clockMs += 50; service(clockMs); assert(mode() == Mode::Ai);
    clockMs += 50; service(clockMs); assert(mode() == Mode::Off);                    // third slow pass: the log switches itself off
    drainAll(); assert(Serial.output.find("[LOG] AUTO-OFF: slow control cycle") != std::string::npos);
    cycle = 1000; }
  { volatile char probe = 0; (void)probe;
    const uintptr_t here = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    setIdentity("1.1.9", nullptr, reinterpret_cast<const void *>(here - (1U << 20)));
    apply(Command::Ai); drainAll(); Serial.output.clear(); publishAi(sample()); clockMs += 50; service(clockMs);
    assert(mode() == Mode::Ai);                                                      // plenty of stack: keeps running
    setIdentity("1.1.9", nullptr, reinterpret_cast<const void *>(here + 4096));
    clockMs += 50; service(clockMs);
    assert(mode() == Mode::Off);                                                     // too little stack left: switches off before formatting
    drainAll(); assert(Serial.output.find("low supervisor stack") != std::string::npos);
    setIdentity("1.1.9", nullptr, nullptr); }

#if MAYAP_DIAGNOSTIC_SERIAL
  // ---- diagnostic build: the machine text log is one of the modes ---------------------------------------------------------------
  drainAll(); Serial.output.clear();
  setMode(Mode::Machine); assert(mayapSerialDebugEnabled() && mode() == Mode::Machine);
  mayapSerialPrintf(false, "machine line\n");
  apply(Command::Ai);                                                                // starting LOG AI ends the machine log and drops its queue
  assert(!mayapSerialDebugEnabled() && mode() == Mode::Ai);
  mayapSerialPrintf(false, "must not appear\n");
  drainAll(); assert(Serial.output.find("machine line") == std::string::npos && Serial.output.find("must not appear") == std::string::npos);
  setMode(Mode::Machine); assert(mayapSerialDebugEnabled() && mode() == Mode::Machine);
  apply(Command::Cpu); assert(!mayapSerialDebugEnabled() && mode() == Mode::Cpu);
  apply(Command::Off); assert(!mayapSerialDebugEnabled() && mode() == Mode::Off);
#else
  // ---- production build: the generic text log does not exist ------------------------------------------------------------------
  mayapSerialPrintf(true, "never printed\n");
  drainAll(); assert(Serial.output.find("never printed") == std::string::npos);
  setMode(Mode::Off);
#endif
  puts("serial-log OK");
  return 0;
}
