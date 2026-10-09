#pragma once
// "BAN GHI CU": the last three versions of the ADVANCED (technical) configuration, HMI only.
//
//   * Snapshot = PID gains and power limit, adaptive-balance switch, heater/thermal diagnostics, tune relay, calibration.
//     It deliberately EXCLUDES the PIN, the Web permission, batch data and every independent safety threshold
//     (High / Emergency / low alarm, sensor timeouts, outputs): restoring a record can never change those.
//   * Three independent slots in spare AT24C512 space (not the A/B config slots). Each slot is one CRC-protected record
//     with a sequence number; a push writes exactly ONE slot (the oldest or an invalid one) and reads it back, so a power cut
//     can lose at most the record being written - the other two stay valid. Rank 1 = newest sequence, rank 3 = oldest.
//   * A push is skipped when the snapshot equals the newest one, so repeated saves do not wear the chip.
//
// Pure C++ (the host test links this header). Storage contract: bool read(uint8_t slot, Record&); bool write(uint8_t slot, const Record&).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "tech_access.h"

namespace AdvancedHistory {

constexpr uint8_t Slots = 3;

struct Snapshot {
  float kp = 0, ki = 0, kd = 0;
  float tempOffset = 0, humidityOffset = 0;
  float heaterStuckMinRiseC = 0, tempRateLimitC = 0, autotuneBandC = 0;
  uint16_t pidCycleSec = 0, heaterStuckDurationSec = 0, tempRateWindowSec = 0, tempOscillationWindowSec = 0;
  uint8_t maxHeaterPower = 0, tempOscillationCrossLimit = 0, autotuneRelayPowerPercent = 0, adaptiveThermalBalanceEnabled = 0;
};
static_assert(sizeof(Snapshot) == 44, "Snapshot layout is part of the stored record");

inline bool same(const Snapshot &a, const Snapshot &b) { return memcmp(&a, &b, sizeof(Snapshot)) == 0; }

// Copy between a MachineConfig-like type and the snapshot (template: this header does not know MachineConfig).
template <class Cfg>
inline Snapshot fromConfig(const Cfg &c) {
  Snapshot s = Snapshot();
  s.kp = c.kp; s.ki = c.ki; s.kd = c.kd; s.tempOffset = c.tempOffset; s.humidityOffset = c.humidityOffset;
  s.heaterStuckMinRiseC = c.heaterStuckMinRiseC; s.tempRateLimitC = c.tempRateLimitC; s.autotuneBandC = c.autotuneBandC;
  s.pidCycleSec = c.pidCycleSec; s.heaterStuckDurationSec = c.heaterStuckDurationSec;
  s.tempRateWindowSec = c.tempRateWindowSec; s.tempOscillationWindowSec = c.tempOscillationWindowSec;
  s.maxHeaterPower = c.maxHeaterPower; s.tempOscillationCrossLimit = c.tempOscillationCrossLimit;
  s.autotuneRelayPowerPercent = c.autotuneRelayPowerPercent;
  s.adaptiveThermalBalanceEnabled = c.adaptiveThermalBalanceEnabled ? 1U : 0U;
  return s;
}
template <class Cfg>
inline void applyTo(Cfg &c, const Snapshot &s) {
  c.kp = s.kp; c.ki = s.ki; c.kd = s.kd; c.tempOffset = s.tempOffset; c.humidityOffset = s.humidityOffset;
  c.heaterStuckMinRiseC = s.heaterStuckMinRiseC; c.tempRateLimitC = s.tempRateLimitC; c.autotuneBandC = s.autotuneBandC;
  c.pidCycleSec = s.pidCycleSec; c.heaterStuckDurationSec = s.heaterStuckDurationSec;
  c.tempRateWindowSec = s.tempRateWindowSec; c.tempOscillationWindowSec = s.tempOscillationWindowSec;
  c.maxHeaterPower = s.maxHeaterPower; c.tempOscillationCrossLimit = s.tempOscillationCrossLimit;
  c.autotuneRelayPowerPercent = s.autotuneRelayPowerPercent;
  c.adaptiveThermalBalanceEnabled = s.adaptiveThermalBalanceEnabled != 0U;
}

// ---- field table: the single list of "technical" fields (HMI Nang cao, the Web technical request, the history snapshot) -----------
enum Field : uint8_t {
  F_KP, F_KI, F_KD, F_MAX_POWER, F_PID_CYCLE, F_ADAPTIVE, F_STUCK_RISE, F_STUCK_DURATION, F_RATE_LIMIT, F_RATE_WINDOW,
  F_OSC_CROSS, F_OSC_WINDOW, F_TUNE_POWER, F_TUNE_BAND, F_TEMP_OFFSET, F_HUM_OFFSET, F_COUNT
};
static_assert(F_COUNT == 16, "the field mask is 16 bits");
// JSON keys (same names as the configuration patch the Web already uses).
static const char *const FieldKeys[F_COUNT] = {
  "kp", "ki", "kd", "maxHeaterPower", "pidCycleSec", "adaptiveThermalBalanceEnabled", "heaterStuckMinRiseC", "heaterStuckDurationSec",
  "tempRateLimitC", "tempRateWindowSec", "tempOscillationCrossLimit", "tempOscillationWindowSec", "autotuneRelayPowerPercent",
  "autotuneBandC", "tempOffset", "humidityOffset"};
inline float fieldValue(const Snapshot &s, uint8_t f) {
  switch (f) {
    case F_KP: return s.kp; case F_KI: return s.ki; case F_KD: return s.kd; case F_MAX_POWER: return s.maxHeaterPower;
    case F_PID_CYCLE: return s.pidCycleSec; case F_ADAPTIVE: return s.adaptiveThermalBalanceEnabled;
    case F_STUCK_RISE: return s.heaterStuckMinRiseC; case F_STUCK_DURATION: return s.heaterStuckDurationSec;
    case F_RATE_LIMIT: return s.tempRateLimitC; case F_RATE_WINDOW: return s.tempRateWindowSec;
    case F_OSC_CROSS: return s.tempOscillationCrossLimit; case F_OSC_WINDOW: return s.tempOscillationWindowSec;
    case F_TUNE_POWER: return s.autotuneRelayPowerPercent; case F_TUNE_BAND: return s.autotuneBandC;
    case F_TEMP_OFFSET: return s.tempOffset; case F_HUM_OFFSET: return s.humidityOffset;
    default: return 0.0f;
  }
}
// Range of every field (the same limits sanitizeMachineConfig() enforces; a request outside them is refused, not clamped).
struct FieldLimit { float lo, hi; bool integer; };
static const FieldLimit FieldLimits[F_COUNT] = {
  {0.0f, 100.0f, false}, {0.0f, 20.0f, false}, {0.0f, 200.0f, false}, {10.0f, 100.0f, true}, {1.0f, 60.0f, true}, {0.0f, 1.0f, true},
  {0.05f, 5.0f, false}, {60.0f, 3600.0f, true}, {0.1f, 10.0f, false}, {30.0f, 1800.0f, true}, {2.0f, 30.0f, true}, {60.0f, 3600.0f, true},
  {10.0f, 80.0f, true}, {0.05f, 1.0f, false}, {-5.0f, 5.0f, false}, {-20.0f, 20.0f, false}};
// Set one field; false when the value is not finite or outside its range (nothing is changed then).
inline bool setField(Snapshot &s, uint8_t f, double value) {
  if (f >= F_COUNT || !(value == value) || value < FieldLimits[f].lo || value > FieldLimits[f].hi) return false;
  if (FieldLimits[f].integer && value != static_cast<double>(static_cast<long>(value))) return false;
  switch (f) {
    case F_KP: s.kp = static_cast<float>(value); break; case F_KI: s.ki = static_cast<float>(value); break;
    case F_KD: s.kd = static_cast<float>(value); break; case F_MAX_POWER: s.maxHeaterPower = static_cast<uint8_t>(value); break;
    case F_PID_CYCLE: s.pidCycleSec = static_cast<uint16_t>(value); break;
    case F_ADAPTIVE: s.adaptiveThermalBalanceEnabled = static_cast<uint8_t>(value); break;
    case F_STUCK_RISE: s.heaterStuckMinRiseC = static_cast<float>(value); break;
    case F_STUCK_DURATION: s.heaterStuckDurationSec = static_cast<uint16_t>(value); break;
    case F_RATE_LIMIT: s.tempRateLimitC = static_cast<float>(value); break;
    case F_RATE_WINDOW: s.tempRateWindowSec = static_cast<uint16_t>(value); break;
    case F_OSC_CROSS: s.tempOscillationCrossLimit = static_cast<uint8_t>(value); break;
    case F_OSC_WINDOW: s.tempOscillationWindowSec = static_cast<uint16_t>(value); break;
    case F_TUNE_POWER: s.autotuneRelayPowerPercent = static_cast<uint8_t>(value); break;
    case F_TUNE_BAND: s.autotuneBandC = static_cast<float>(value); break;
    case F_TEMP_OFFSET: s.tempOffset = static_cast<float>(value); break;
    case F_HUM_OFFSET: s.humidityOffset = static_cast<float>(value); break;
  }
  return true;
}
// Bitmask of the fields in which `a` and `b` differ (exact compare: both sides come from the same sanitised stores).
inline uint16_t differingMask(const Snapshot &a, const Snapshot &b) {
  uint16_t mask = 0;
  for (uint8_t f = 0; f < F_COUNT; ++f) if (fieldValue(a, f) != fieldValue(b, f)) mask = static_cast<uint16_t>(mask | (1U << f));
  return mask;
}
inline void copyFields(Snapshot &dst, const Snapshot &src, uint16_t mask) {
  for (uint8_t f = 0; f < F_COUNT; ++f) if (mask & (1U << f)) (void)setField(dst, f, fieldValue(src, f));
}

constexpr uint32_t Magic = 0x4D414848UL;   // "MAHH"
constexpr uint16_t Schema = 1;
struct Record {
  uint32_t magic = Magic;
  uint16_t schema = Schema;
  uint16_t size = 0;
  uint32_t sequence = 0;
  Snapshot payload;
  uint32_t crc = 0;
};
inline uint32_t recordCrc(const Record &r) { return TechAccess::crc32(reinterpret_cast<const uint8_t *>(&r), offsetof(Record, crc)); }
inline bool recordValid(const Record &r) {
  return r.magic == Magic && r.schema == Schema && r.size == sizeof(Record) && r.sequence != 0U && r.crc == recordCrc(r);
}
inline bool newer(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }

template <class Storage>
class Ring {
 public:
  explicit Ring(Storage &storage) : st_(storage) {}

  // Scan the three slots; invalid/blank ones simply do not count.
  void begin() {
    for (uint8_t i = 0; i < Slots; ++i) {
      Record r;
      valid_[i] = st_.read(i, r) && recordValid(r);
      seq_[i] = valid_[i] ? r.sequence : 0U;
    }
  }
  uint8_t count() const { uint8_t n = 0; for (uint8_t i = 0; i < Slots; ++i) if (valid_[i]) ++n; return n; }

  // rank 0 = newest ... rank 2 = oldest (the HMI shows 1..3).
  bool get(uint8_t rank, Snapshot &out, uint32_t *sequence = nullptr) {
    uint8_t order[Slots]; const uint8_t n = sorted(order);
    if (rank >= n) return false;
    Record r;
    if (!st_.read(order[rank], r) || !recordValid(r)) return false;   // re-validated on every read
    out = r.payload; if (sequence) *sequence = r.sequence;
    return true;
  }

  // Store `snap` as the newest record. true = stored (or already the newest, nothing written). false = write failed.
  bool push(const Snapshot &snap) {
    uint8_t order[Slots]; const uint8_t n = sorted(order);
    if (n) {
      Record top;
      if (st_.read(order[0], top) && recordValid(top) && same(top.payload, snap)) return true;
    }
    // victim: an invalid slot first, otherwise the oldest
    uint8_t victim = 0; bool found = false;
    for (uint8_t i = 0; i < Slots; ++i) if (!valid_[i]) { victim = i; found = true; break; }
    if (!found) victim = order[n - 1U];
    uint32_t top = 0U; for (uint8_t i = 0; i < Slots; ++i) if (valid_[i] && newer(seq_[i], top)) top = seq_[i];
    Record r;
    r.size = sizeof(Record); r.sequence = top + 1U; if (r.sequence == 0U) r.sequence = 1U;
    r.payload = snap; r.crc = recordCrc(r);
    valid_[victim] = false;                    // the slot is being rewritten: until the read-back succeeds it is not trusted
    if (!st_.write(victim, r)) return false;
    Record back;
    if (!st_.read(victim, back) || !recordValid(back) || back.sequence != r.sequence || !same(back.payload, snap)) return false;
    valid_[victim] = true; seq_[victim] = r.sequence;
    return true;
  }

 private:
  // newest first
  uint8_t sorted(uint8_t order[Slots]) const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < Slots; ++i) if (valid_[i]) order[n++] = i;
    for (uint8_t a = 0; a + 1U < n; ++a)
      for (uint8_t b = a + 1U; b < n; ++b)
        if (newer(seq_[order[b]], seq_[order[a]])) { const uint8_t t = order[a]; order[a] = order[b]; order[b] = t; }
    return n;
  }
  Storage &st_;
  bool valid_[Slots] = {false, false, false};
  uint32_t seq_[Slots] = {0, 0, 0};
};

}  // namespace AdvancedHistory
