#pragma once
// Persistence for the incubation programme and the egg-cooling policy/state (Smart Thermal phase 8, D6). Design + host-tested; NOT wired:
// the real EEPROM addresses must be allocated against the existing map (config A/B, batch, history, technical records) in a reviewed change.
//
// Two slots A/B per record kind, a CRC32 and a wrap-safe sequence number. A save writes ONLY the slot that does not hold the newest valid
// record, then reads it back and validates it; a torn write therefore damages only the new slot and the previous record survives.
// Unknown magic / version / CRC => the record is "absent" and the caller uses the defaults (everything OFF). Nothing is written when the
// content did not change, and a minimum interval between writes bounds the wear.
#include <cstdint>
#include <cstring>
#include "egg_cooling.h"
#include "thermal_program.h"

namespace MayapProgram {

#pragma pack(push, 1)
struct ProgramRecord {
  uint32_t magic = 0x4D475250U;            // 'PRGM'
  uint8_t version = 1, enabled = 0, count = 0, reserved = 0;
  uint8_t day[MaxStages] = {};
  int16_t targetX10[MaxStages] = {};
  uint32_t confirmedBatchId = 0;           // the batch the operator confirmed this programme for (0 = none)
  uint32_t seq = 0;
  uint32_t crc = 0;
};
struct PolicyRecord {
  uint32_t magic = 0x4C504345U;            // 'ECPL'
  uint8_t version = 1, enabled = 0, firstDay = 0, lastDay = 0;
  uint16_t periodMin = 0, durationMin = 0, maxDurationMin = 0;
  int16_t minTempX10 = 0;
  uint8_t resume = 0, reserved = 0;
  uint32_t seq = 0;
  uint32_t crc = 0;
};
#pragma pack(pop)

inline Program toProgram(const ProgramRecord &r) {
  Program p; p.enabled = r.enabled != 0; p.count = r.count > MaxStages ? MaxStages : r.count;
  for (uint8_t i = 0; i < p.count; ++i) p.stages[i] = Stage(r.day[i], r.targetX10[i]);
  return p;
}
inline ProgramRecord toRecord(const Program &p, uint32_t confirmedBatch) {
  ProgramRecord r; r.enabled = p.enabled ? 1 : 0; r.count = p.count > MaxStages ? MaxStages : p.count; r.confirmedBatchId = confirmedBatch;
  for (uint8_t i = 0; i < r.count; ++i) { r.day[i] = p.stages[i].fromDay; r.targetX10[i] = p.stages[i].targetX10; }
  return r;
}
inline CoolingPolicy toPolicy(const PolicyRecord &r) {
  CoolingPolicy p; p.enabled = r.enabled != 0; p.firstDay = r.firstDay; p.lastDay = r.lastDay; p.periodMin = r.periodMin; p.durationMin = r.durationMin;
  p.maxDurationMin = r.maxDurationMin; p.minTempX10 = r.minTempX10; p.resume = r.resume == 1 ? Resume::ResumeRemaining : Resume::Cancel;
  return validPolicy(p) ? p : CoolingPolicy{};
}
inline PolicyRecord toRecord(const CoolingPolicy &p) {
  PolicyRecord r; r.enabled = p.enabled ? 1 : 0; r.firstDay = p.firstDay; r.lastDay = p.lastDay; r.periodMin = p.periodMin; r.durationMin = p.durationMin;
  r.maxDurationMin = p.maxDurationMin; r.minTempX10 = p.minTempX10; r.resume = p.resume == Resume::ResumeRemaining ? 1 : 0;
  return r;
}

template <class Rec> inline uint32_t recCrc(const Rec &r) { return crc32(reinterpret_cast<const uint8_t *>(&r), static_cast<uint32_t>(sizeof(Rec) - sizeof(uint32_t))); }
template <class Rec> inline bool recValid(const Rec &r, const Rec &proto) { return r.magic == proto.magic && r.version == proto.version && r.crc == recCrc(r); }

enum class SaveResult : uint8_t { Written, Unchanged, TooSoon, Failed };

template <class Rec, class Dev>
class AbStore {
 public:
  AbStore(Dev &dev, uint16_t addrA, uint16_t addrB, uint32_t minIntervalMs) : dev_(dev), a_(addrA), b_(addrB), minMs_(minIntervalMs) {}
  // Newest valid record, or false (=> defaults, feature OFF).
  bool load(Rec &out) {
    Rec ra, rb; const Rec proto;
    const bool okA = dev_.read(a_, &ra, sizeof(Rec)) && recValid(ra, proto);
    const bool okB = dev_.read(b_, &rb, sizeof(Rec)) && recValid(rb, proto);
    if (!okA && !okB) { have_ = false; return false; }
    if (okA && okB) newestIsA_ = static_cast<int32_t>(ra.seq - rb.seq) >= 0; else newestIsA_ = okA;
    out = newestIsA_ ? ra : rb; cur_ = out; have_ = true; return true;
  }
  SaveResult save(const Rec &r, uint32_t now) {
    if (have_ && sameContent(r, cur_)) return SaveResult::Unchanged;
    if (written_ && static_cast<uint32_t>(now - lastWrite_) < minMs_) return SaveResult::TooSoon;
    Rec w = r; w.seq = have_ ? cur_.seq + 1U : 1U; w.crc = recCrc(w);
    const uint16_t addr = (have_ && newestIsA_) ? b_ : a_;          // never the slot that holds the newest record
    Rec back;
    if (!dev_.write(addr, &w, sizeof(Rec)) || !dev_.read(addr, &back, sizeof(Rec)) || std::memcmp(&back, &w, sizeof(Rec)) != 0) return SaveResult::Failed;
    ++writes_; written_ = true; lastWrite_ = now; cur_ = w; have_ = true; newestIsA_ = addr == a_;
    return SaveResult::Written;
  }
  uint32_t writes() const { return writes_; }
 private:
  static bool sameContent(const Rec &x, const Rec &y) {
    Rec a = x, b = y; a.seq = b.seq = 0; a.crc = b.crc = 0; return std::memcmp(&a, &b, sizeof(Rec)) == 0;
  }
  Dev &dev_; uint16_t a_, b_; uint32_t minMs_;
  Rec cur_{}; bool have_ = false, newestIsA_ = false, written_ = false; uint32_t lastWrite_ = 0, writes_ = 0;
};

}  // namespace MayapProgram
