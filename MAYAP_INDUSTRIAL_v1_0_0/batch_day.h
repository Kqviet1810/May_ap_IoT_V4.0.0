#pragma once
// The day counter of a batch. The machine counts the seconds a batch has run (MachineController::elapsedBeforeStartSec_, written to the EEPROM at every
// checkpoint and read back after a power cut); the day shown on the HMI and the Web is that count / 86400 + 1. Two things need that count to start or move:
//   * a batch started from day N (an earlier batch of the same eggs was ended by mistake, or the eggs came from another incubator);
//   * the day of a running batch corrected (day N of the eggs, not of the machine).
// Pure C++ (no Arduino types): tests/batch-day.cpp drives the same functions.
//
// Setting the day of a running batch keeps the time of day: the day then turns over at the same hour as before, so the turning schedule and the daily
// counters do not jump. Only whole days move.
#include <cstdint>

namespace batchday {

constexpr uint32_t DAY_SEC = 86400UL;
constexpr uint8_t DAY_MIN = 1U;

// A day is valid when it is inside the batch (1 .. total days): a day past the end would be an overdue batch from the first second.
inline bool valid(uint32_t day, uint8_t totalDays) { return day >= DAY_MIN && totalDays >= DAY_MIN && day <= totalDays; }

// Seconds a batch starting at `day` has already run (day 1 = 0).
inline uint32_t startElapsed(uint32_t day) { return day <= DAY_MIN ? 0UL : (day - 1UL) * DAY_SEC; }

// The running count after the day is set: whole days replaced, the time inside the day kept.
inline uint32_t setElapsed(uint32_t currentElapsedSec, uint32_t day) { return startElapsed(day) + currentElapsedSec % DAY_SEC; }

inline uint32_t dayOf(uint32_t elapsedSec) { return elapsedSec / DAY_SEC + 1UL; }

}  // namespace batchday
