#pragma once
// Persistent owner of the model Thermal Predictive V1 identified on the last good batch (a PRIOR for the next one).
//
// The physics of the oven (capacities, air<->egg coupling, delay, lag) does not change from batch to batch; the eggs' temperature and the
// disturbances do, and those are never stored. Same discipline as the launch plan and the thermal profile (same NVS namespace, own keys
// "pa"/"pb"):
//   * the sole flash writer is Arduino loop(), never the control task; the control task only copies a fixed-size record into a mailbox;
//   * two alternating slots, each written then read back and verified, so a power cut leaves the previous valid record;
//   * at most one save per MinIntervalMs and only when the model differs materially from the stored one.
#include "thermal_predictive.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace MayapPredictive {

constexpr uint32_t PredModelMagic = 0x50444D31;   // 'PDM1'
constexpr uint16_t PredModelVersion = 1;
constexpr uint32_t MinIntervalMs = 1800000UL;

#pragma pack(push, 1)
struct PredModelRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  float cf, ce, k, a, dead, tau, vp;
  float rms;           // identifier residual when it was stored, degC
  uint32_t signature;  // hardware compatibility (same as the thermal profile)
  uint32_t epoch;      // RTC epoch at save (0 = unknown)
  uint32_t sequence;
  uint32_t crc;
};
#pragma pack(pop)
static_assert(sizeof(PredModelRecord) == 56, "predictive model layout is persistent: bump PredModelVersion to change it");

inline uint32_t predModelCrc(const PredModelRecord &r) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&r);
  uint32_t c = 0xffffffffU;
  for (size_t n = 0; n < offsetof(PredModelRecord, crc); ++n) {
    c ^= bytes[n];
    for (unsigned b = 0; b < 8; ++b) c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U)));
  }
  return ~c;
}
inline bool predModelRangesValid(const PredModelRecord &r) {
  const float v[7] = {r.cf, r.ce, r.k, r.a, r.dead, r.tau, r.vp};
  const float lo[7] = {std::exp(Prior::lo[0]), std::exp(Prior::lo[1]), std::exp(Prior::lo[2]), std::exp(Prior::lo[3]), std::exp(Prior::lo[4]),
                       std::exp(Prior::lo[5]), Prior::lo[6]};
  const float hi[7] = {std::exp(Prior::hi[0]), std::exp(Prior::hi[1]), std::exp(Prior::hi[2]), std::exp(Prior::hi[3]), std::exp(Prior::hi[4]),
                       std::exp(Prior::hi[5]), Prior::hi[6]};
  for (int i = 0; i < 7; ++i) if (!std::isfinite(v[i]) || v[i] < lo[i] * 0.999f || v[i] > hi[i] * 1.001f) return false;
  return std::isfinite(r.rms) && r.rms >= 0.0f && r.rms <= 1.0f;
}
inline bool validPredModelRecord(const PredModelRecord &r) {
  return r.magic == PredModelMagic && r.version == PredModelVersion && r.size == sizeof(PredModelRecord) && r.crc == predModelCrc(r) &&
         predModelRangesValid(r);
}
inline PredModelRecord makePredModelRecord(const StoredModel &m, uint32_t signature, uint32_t epoch) {
  PredModelRecord r;
  std::memset(&r, 0, sizeof(r));
  r.magic = PredModelMagic; r.version = PredModelVersion; r.size = sizeof(PredModelRecord);
  r.cf = m.m.cf; r.ce = m.m.ce; r.k = m.m.k; r.a = m.m.a; r.dead = m.m.dead; r.tau = m.m.tau; r.vp = m.m.vp; r.rms = m.rms;
  r.signature = signature; r.epoch = epoch;
  r.crc = predModelCrc(r);
  return r;
}
inline StoredModel toStoredModel(const PredModelRecord &r) {
  StoredModel s;
  s.valid = true; s.m.cf = r.cf; s.m.ce = r.ce; s.m.k = r.k; s.m.a = r.a; s.m.dead = r.dead; s.m.tau = r.tau; s.m.vp = r.vp; s.rms = r.rms;
  return s;
}
// A stored model is used only on the same hardware signature and never after an abnormal reset.
inline bool predModelCompatible(const PredModelRecord &r, uint32_t signature, bool abnormalReset) {
  return !abnormalReset && validPredModelRecord(r) && r.signature == signature;
}
// Flash-wear policy (pure so it is unit-tested): save only a model that differs materially from the stored one.
inline bool shouldPersistPredModel(const PredModelRecord &live, const PredModelRecord &stored, bool storedValid, uint32_t sinceLastSaveMs,
                                   uint32_t minIntervalMs = MinIntervalMs) {
  if (!validPredModelRecord(live) || sinceLastSaveMs < minIntervalMs) return false;
  if (!storedValid || live.signature != stored.signature) return true;
  const auto rel = [](float a, float b) { return std::fabs(a - b) > 0.15f * b; };
  return rel(live.cf, stored.cf) || rel(live.ce, stored.ce) || rel(live.k, stored.k) || rel(live.dead, stored.dead) ||
         std::fabs(live.tau - stored.tau) > 8.0f || std::fabs(live.vp - stored.vp) > 3.0f || std::fabs(live.a - stored.a) > 0.4f;
}

class PredModelStorage {
 public:
  void offer(const PredModelRecord &record) {
    portENTER_CRITICAL(&mux_); pending_ = record; hasPending_ = true; portEXIT_CRITICAL(&mux_);
  }
  bool takeSeed(PredModelRecord &record) {
    portENTER_CRITICAL(&mux_);
    const bool ready = seedReady_;
    if (ready) { record = stored_; seedReady_ = false; }
    portEXIT_CRITICAL(&mux_);
    return ready;
  }
  uint32_t saves() const { return saves_; }
  void resetForTest() {
    portENTER_CRITICAL(&mux_); hasPending_ = seedReady_ = false; portEXIT_CRITICAL(&mux_);
    initialized_ = available_ = activeA_ = storedValid_ = false;
    sequence_ = lastSave_ = saves_ = 0;
    pending_ = PredModelRecord{}; stored_ = PredModelRecord{};
  }
  void service(uint32_t now) {
    if (!initialized_) {
      initialized_ = true;
      available_ = prefs_.begin("mayap_thermal", false);
      if (!available_) return;
      PredModelRecord a{}, b{};
      const bool va = prefs_.isKey("pa") && prefs_.getBytes("pa", &a, sizeof(a)) == sizeof(a) && validPredModelRecord(a);
      const bool vb = prefs_.isKey("pb") && prefs_.getBytes("pb", &b, sizeof(b)) == sizeof(b) && validPredModelRecord(b);
      if (va || vb) {
        const bool useA = !vb || (va && static_cast<int32_t>(a.sequence - b.sequence) >= 0);
        activeA_ = useA;
        stored_ = useA ? a : b;
        storedValid_ = true;
        sequence_ = stored_.sequence;
        portENTER_CRITICAL(&mux_); seedReady_ = true; portEXIT_CRITICAL(&mux_);
      }
      lastSave_ = now - MinIntervalMs;   // the first offer of this run may be saved at once
      return;
    }
    if (!available_) return;
    PredModelRecord record{};
    portENTER_CRITICAL(&mux_);
    const bool pending = hasPending_;
    if (pending) record = pending_;
    portEXIT_CRITICAL(&mux_);
    if (!pending) return;
    if (!validPredModelRecord(record) || !shouldPersistPredModel(record, stored_, storedValid_, UINT32_MAX)) {
      portENTER_CRITICAL(&mux_); hasPending_ = false; portEXIT_CRITICAL(&mux_);   // nothing material to save: drop it
      return;
    }
    if (now - lastSave_ < MinIntervalMs) return;   // wear bound: keep it pending until the interval has passed
    portENTER_CRITICAL(&mux_); hasPending_ = false; portEXIT_CRITICAL(&mux_);
    lastSave_ = now;   // a failed write is wear/backoff bounded as well
    record.sequence = sequence_ + 1U;
    record.crc = predModelCrc(record);
    if (!validPredModelRecord(record)) return;
    const char *key = activeA_ ? "pb" : "pa";
    PredModelRecord verify{};
    if (prefs_.putBytes(key, &record, sizeof(record)) == sizeof(record) &&
        prefs_.getBytes(key, &verify, sizeof(verify)) == sizeof(verify) && validPredModelRecord(verify) &&
        std::memcmp(&record, &verify, sizeof(record)) == 0) {
      activeA_ = !activeA_; sequence_ = record.sequence; stored_ = record; storedValid_ = true; ++saves_;
    }
  }

 private:
  Preferences prefs_;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  PredModelRecord pending_{}, stored_{};
  bool hasPending_ = false, seedReady_ = false;
  bool initialized_ = false, available_ = false, activeA_ = false, storedValid_ = false;
  uint32_t sequence_ = 0, lastSave_ = 0, saves_ = 0;
};
static PredModelStorage predModelStorage;
}  // namespace MayapPredictive
