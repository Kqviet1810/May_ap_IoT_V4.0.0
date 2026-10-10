#include "../MAYAP_INDUSTRIAL_v1_0_0/output_budget.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
using namespace MayapSafety;
static int checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)
int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  // default = disabled: counts, never aborts
  { EdgeBudget b; bool s = false; for (uint32_t t = 0; t < 10000000UL; t += 50) { s = !s; b.note(t, s); } CHECK(!b.abortRequested() && !b.enabled() && b.totalEdges() > 1000); }
  // sliding window: 100 edges allowed per 10 minutes
  { EdgeBudgetParams p; p.windowMs = 600000UL; p.maxEdges = 100; EdgeBudget b; b.configure(p); bool s = false; uint32_t t = 1000;
    for (int i = 0; i < 100; ++i) { t += 5000; s = !s; b.note(t, s); } CHECK(b.edgesInWindow() <= 100 && !b.abortRequested());
    for (int i = 0; i < 5; ++i) { t += 1000; s = !s; b.note(t, s); } CHECK(b.abortRequested());                         // 105 edges inside 10 min
    b.reset(); CHECK(!b.abortRequested()); }
  { EdgeBudgetParams p; p.windowMs = 600000UL; p.maxEdges = 100; EdgeBudget b; b.configure(p); bool s = false; uint32_t t = 1000;   // same total, spread out: fine
    for (int i = 0; i < 400; ++i) { t += 20000; s = !s; b.note(t, s); } CHECK(!b.abortRequested() && b.totalEdges() == 399); }
  // minimum ON/OFF
  { EdgeBudgetParams p; p.minOnMs = 300; p.minOffMs = 300; p.maxShortViolations = 3; EdgeBudget b; b.configure(p);
    b.note(1000, true); b.note(1100, false); b.note(1500, true); b.note(1650, false); CHECK(b.shortOn() == 2 && !b.abortRequested());
    b.note(1700, true); CHECK(b.shortOff() == 1 && b.abortRequested()); }
  { EdgeBudgetParams p; p.minOnMs = 300; EdgeBudget b; b.configure(p); b.note(0, true); b.note(100, false); b.note(200, true); b.note(300, false); CHECK(b.shortOn() == 2 && !b.abortRequested()); }   // counted, no abort limit set
  // millis() rollover
  { EdgeBudgetParams p; p.windowMs = 600000UL; p.maxEdges = 10; EdgeBudget b; b.configure(p); bool s = false; uint32_t t = 0xFFFFFFFFU - 30000U;
    for (int i = 0; i < 8; ++i) { t += 10000U; s = !s; b.note(t, s); } CHECK(!b.abortRequested() && b.edgesInWindow() == 7);
    for (int i = 0; i < 6; ++i) { t += 1000U; s = !s; b.note(t, s); } CHECK(b.abortRequested()); }
  // a long silence empties the window
  { EdgeBudgetParams p; p.windowMs = 600000UL; p.maxEdges = 10; EdgeBudget b; b.configure(p); bool s = false; uint32_t t = 0;
    for (int i = 0; i < 9; ++i) { t += 1000; s = !s; b.note(t, s); } t += 3600000UL; b.note(t, s); CHECK(b.edgesInWindow() == 0 && !b.abortRequested()); }
  std::printf("Output switching budget (D5 helper, default disabled): %d checks PASS\n", checks);
  return 0;
}
