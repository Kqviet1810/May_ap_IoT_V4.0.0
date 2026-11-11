// The day counter of a batch: start from a day, set the day of a running batch (the time of day stays), limits.
//   g++ -std=c++11 -Wall -Wextra -Werror tests/batch-day.cpp -o /tmp/mayap-batch-day && /tmp/mayap-batch-day
#include <cassert>
#include <cstdio>
#include "../MAYAP_INDUSTRIAL_v1_0_0/batch_day.h"

using namespace batchday;

int main() {
  std::printf("=== Batch day ===\n");
  assert(startElapsed(0U) == 0UL && startElapsed(1U) == 0UL && startElapsed(2U) == 86400UL && startElapsed(5U) == 4UL * 86400UL);
  assert(dayOf(0UL) == 1UL && dayOf(86399UL) == 1UL && dayOf(86400UL) == 2UL && dayOf(startElapsed(5U)) == 5UL);
  assert(valid(1U, 21U) && valid(21U, 21U) && !valid(0U, 21U) && !valid(22U, 21U) && !valid(1U, 0U));
  // a batch ended by mistake on day 5 and started again from day 5: the machine shows day 5 at once and counts on
  assert(dayOf(startElapsed(5U) + 3600UL) == 5UL);
  // a running batch, 2 h 30 min into its day 1, moved to day 4: day 4, still 2 h 30 min into the day
  const uint32_t cur = 2UL * 3600UL + 1800UL, moved = setElapsed(cur, 4U);
  assert(dayOf(moved) == 4UL && moved % DAY_SEC == cur);
  // set back to the same day: nothing moves; to an earlier day: goes back, the time of day stays
  assert(setElapsed(4UL * 86400UL + 777UL, 5U) == 4UL * 86400UL + 777UL);
  assert(setElapsed(9UL * 86400UL + 777UL, 3U) == 2UL * 86400UL + 777UL);
  // 32-bit: the largest allowed batch is far from the wrap-around
  assert(startElapsed(45U) < 0xFFFFFFFFUL / 4UL);
  std::printf("BATCH DAY: ALL PASSED\n");
  return 0;
}
