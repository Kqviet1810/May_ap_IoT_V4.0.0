#pragma once
// What the thermal controller measured about THIS oven (the heater gain kAir and the transit time), kept in the external EEPROM so that a restart next to SV
// does not have to guess them (a guess wrong by 2.5 times is what left the oven 0.8 degC under SV for an hour after a restart and a door).
// Pure C++ (no Arduino types): MachineController reads and writes it through PersistentStore, tests/thermal-gain-record.cpp drives the same code.
//
//   Two slots (A, B), each one CRC-protected record; a save writes the slot that does NOT hold the newest valid record and reads it back, so a power cut in the
//   middle of a write leaves the other slot readable. A record is written only when a real climb has measured the numbers and they moved (see wanted()), at
//   most once in GAIN_SAVE_MIN_MS: a handful of writes a day at the very most, on a chip rated for a million.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace thermalgain {

constexpr uint32_t MAGIC = 0x4D4B4731UL;                 // "MKG1"
constexpr uint32_t GAIN_SAVE_MIN_MS = 600000UL;          // at most one write every 10 minutes
constexpr float GAIN_SAVE_REL = 0.03f;                   // ... and only when kAir moved by more than 3 %
constexpr float TAU_SAVE_S = 5.0f;                       // ... or the transit time by more than 5 s

// Same limits as thermalv3::restore() (they are repeated so that this file stands alone; the tests compare them).
constexpr float KAIR_MIN = 8.0e-5f / 3.0f, KAIR_MAX = 8.0e-5f * 10.0f;
constexpr float TAU_MIN = 150.0f * 0.6f, TAU_MAX = 150.0f * 1.5f;

struct Record {
  uint32_t magic;
  uint32_t sequence;
  float kAir;
  float tau;
  uint32_t crc;
};

inline uint32_t crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0U; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t k = 0U; k < 8U; ++k) crc = (crc & 1UL) ? ((crc >> 1U) ^ 0xEDB88320UL) : (crc >> 1U);
  }
  return ~crc;
}
inline uint32_t recordCrc(const Record &r) { return crc32(reinterpret_cast<const uint8_t *>(&r), offsetof(Record, crc)); }

inline bool valid(const Record &r) {
  return r.magic == MAGIC && r.crc == recordCrc(r) && std::isfinite(r.kAir) && r.kAir >= KAIR_MIN && r.kAir <= KAIR_MAX &&
         std::isfinite(r.tau) && (r.tau == 0.0f || (r.tau >= TAU_MIN && r.tau <= TAU_MAX));
}
inline Record make(uint32_t sequence, float kAir, float tau) {
  Record r{};
  r.magic = MAGIC; r.sequence = sequence; r.kAir = kAir; r.tau = tau;
  r.crc = recordCrc(r);
  return r;
}
inline bool newer(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }

// Of the two slots, the newest valid record. Returns 0 (none), 1 (A) or 2 (B).
inline uint8_t pick(const Record &a, bool readA, const Record &b, bool readB, Record &out) {
  const bool va = readA && valid(a), vb = readB && valid(b);
  if (va && (!vb || !newer(b.sequence, a.sequence))) { out = a; return 1U; }
  if (vb) { out = b; return 2U; }
  return 0U;
}
// The next save: the record to write (sequence one past the newest valid one) and the slot it goes to - the one that does NOT hold the newest valid record
// (A when neither is valid).
inline Record nextToWrite(const Record &a, bool readA, const Record &b, bool readB, float kAir, float tau, bool &toA) {
  Record newest{};
  const uint8_t have = pick(a, readA, b, readB, newest);
  toA = have != 1U;
  return make(have ? newest.sequence + 1UL : 1UL, kAir, tau);
}

// Should the numbers be written now? `measured` = a real climb has measured kAir since power-up (a manual or restored value does not count).
inline bool wanted(bool measured, bool haveSaved, float savedKAir, float savedTau, float kAir, float tau, uint32_t sinceLastSaveMs) {
  if (!measured || sinceLastSaveMs < GAIN_SAVE_MIN_MS) return false;
  if (!std::isfinite(kAir) || kAir < KAIR_MIN || kAir > KAIR_MAX) return false;
  if (!haveSaved) return true;
  return std::fabs(kAir - savedKAir) > GAIN_SAVE_REL * savedKAir || (tau > 0.0f && std::fabs(tau - savedTau) > TAU_SAVE_S);
}

// Manual choice of the heater gain (HMI NANG CAO > GIA NHIET): 0 = automatic, 1..7 = a level, x 1e-4 degC/s per % of the heater.
constexpr uint8_t MANUAL_LEVELS = 8U;
inline float manualKAir(uint8_t level) {
  static const float table[MANUAL_LEVELS] = {0.0f, 0.6e-4f, 0.8e-4f, 1.0e-4f, 1.4e-4f, 2.0e-4f, 3.0e-4f, 5.0e-4f};
  return level < MANUAL_LEVELS ? table[level] : 0.0f;
}

}  // namespace thermalgain
