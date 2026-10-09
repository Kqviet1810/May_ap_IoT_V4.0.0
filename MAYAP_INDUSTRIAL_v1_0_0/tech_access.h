#pragma once
// Technical access ("Nang cao") for HMI and Web: one 4-digit PIN, one lock-out counter, one authority.
//
//   * The PIN is chosen on the HMI only (first use creates it, there is NO factory PIN) and is stored as
//     HMAC-SHA256 chains over a per-device random salt (never the digits, never logged).
//   * Wrong tries are counted in the persisted record, so a power cycle does not reset them. After
//     MaxAttempts wrong tries the access is locked for LockBaseMs, doubling for every further lock-out up to LockMaxMs.
//     HMI and Web share ONE counter: alternating the two surfaces gives no extra tries.
//   * An HMI session is RAM only: it ends on exit, on idle timeout, and on every reboot.
//   * The Web gate has two levels, both owned by the HMI: the permission switch (default HIDDEN, persisted) and a
//     short Web session that exists only after the right PIN was verified HERE, on the ESP32, while the switch is ON.
//     Switching the permission OFF revokes the Web session at once.
//   * Honest limit: a 4-digit PIN has 10^4 values. This is an access gate against casual/accidental change and
//     remote misuse, not protection against someone who reads the EEPROM chip.
//
// Pure C++ (no Arduino types): the host tests link this very header. Storage is a template parameter.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace TechAccess {

constexpr uint8_t PinDigits = 4;
constexpr uint8_t MaxAttempts = 5;
constexpr uint32_t LockBaseMs = 60000UL;
constexpr uint32_t LockMaxMs = 900000UL;
constexpr uint32_t HmiIdleMs = 120000UL;          // HMI session ends after this much inactivity
constexpr uint32_t WebSessionMs = 600000UL;       // Web technical session (absolute, not extended by use)
constexpr uint16_t HashRounds = 256;

// ---- SHA-256 / HMAC-SHA256 (portable; the same code runs on the host and on the ESP32) -------------------------
class Sha256 {
 public:
  Sha256() { reset(); }
  void reset() {
    static const uint32_t init[8] = {0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL, 0xa54ff53aUL, 0x510e527fUL, 0x9b05688cUL, 0x1f83d9abUL, 0x5be0cd19UL};
    for (uint8_t i = 0; i < 8; ++i) h_[i] = init[i];
    bits_ = 0; fill_ = 0;
  }
  void update(const uint8_t *data, size_t length) {
    bits_ += static_cast<uint64_t>(length) * 8U;
    while (length) {
      const size_t take = (64U - fill_) < length ? (64U - fill_) : length;
      memcpy(block_ + fill_, data, take);
      fill_ = static_cast<uint8_t>(fill_ + take); data += take; length -= take;
      if (fill_ == 64U) { compress(); fill_ = 0; }
    }
  }
  void final(uint8_t out[32]) {
    const uint64_t bits = bits_;
    uint8_t pad = 0x80;
    update(&pad, 1);
    pad = 0;
    while (fill_ != 56U) update(&pad, 1);
    uint8_t len[8];
    for (uint8_t i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (56U - 8U * i));
    update(len, 8);
    for (uint8_t i = 0; i < 8; ++i) {
      out[4 * i] = static_cast<uint8_t>(h_[i] >> 24); out[4 * i + 1] = static_cast<uint8_t>(h_[i] >> 16);
      out[4 * i + 2] = static_cast<uint8_t>(h_[i] >> 8); out[4 * i + 3] = static_cast<uint8_t>(h_[i]);
    }
  }
 private:
  static uint32_t rotr(uint32_t x, uint8_t n) { return (x >> n) | (x << (32U - n)); }
  void compress() {
    static const uint32_t k[64] = {
      0x428a2f98UL,0x71374491UL,0xb5c0fbcfUL,0xe9b5dba5UL,0x3956c25bUL,0x59f111f1UL,0x923f82a4UL,0xab1c5ed5UL,
      0xd807aa98UL,0x12835b01UL,0x243185beUL,0x550c7dc3UL,0x72be5d74UL,0x80deb1feUL,0x9bdc06a7UL,0xc19bf174UL,
      0xe49b69c1UL,0xefbe4786UL,0x0fc19dc6UL,0x240ca1ccUL,0x2de92c6fUL,0x4a7484aaUL,0x5cb0a9dcUL,0x76f988daUL,
      0x983e5152UL,0xa831c66dUL,0xb00327c8UL,0xbf597fc7UL,0xc6e00bf3UL,0xd5a79147UL,0x06ca6351UL,0x14292967UL,
      0x27b70a85UL,0x2e1b2138UL,0x4d2c6dfcUL,0x53380d13UL,0x650a7354UL,0x766a0abbUL,0x81c2c92eUL,0x92722c85UL,
      0xa2bfe8a1UL,0xa81a664bUL,0xc24b8b70UL,0xc76c51a3UL,0xd192e819UL,0xd6990624UL,0xf40e3585UL,0x106aa070UL,
      0x19a4c116UL,0x1e376c08UL,0x2748774cUL,0x34b0bcb5UL,0x391c0cb3UL,0x4ed8aa4aUL,0x5b9cca4fUL,0x682e6ff3UL,
      0x748f82eeUL,0x78a5636fUL,0x84c87814UL,0x8cc70208UL,0x90befffaUL,0xa4506cebUL,0xbef9a3f7UL,0xc67178f2UL};
    uint32_t w[64];
    for (uint8_t i = 0; i < 16; ++i)
      w[i] = (static_cast<uint32_t>(block_[4 * i]) << 24) | (static_cast<uint32_t>(block_[4 * i + 1]) << 16) |
             (static_cast<uint32_t>(block_[4 * i + 2]) << 8) | block_[4 * i + 3];
    for (uint8_t i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (uint8_t i = 0; i < 64; ++i) {
      const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = h + S1 + ch + k[i] + w[i];
      const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = S0 + mj;
      h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
  }
  uint32_t h_[8];
  uint8_t block_[64];
  uint8_t fill_;
  uint64_t bits_;
};

inline void hmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *msg, size_t msgLen, uint8_t out[32]) {
  uint8_t k[64] = {0};
  if (keyLen > 64U) { Sha256 s; s.update(key, keyLen); s.final(k); } else memcpy(k, key, keyLen);
  uint8_t ipad[64], opad[64];
  for (uint8_t i = 0; i < 64; ++i) { ipad[i] = static_cast<uint8_t>(k[i] ^ 0x36); opad[i] = static_cast<uint8_t>(k[i] ^ 0x5c); }
  uint8_t inner[32];
  Sha256 a; a.update(ipad, 64); a.update(msg, msgLen); a.final(inner);
  Sha256 b; b.update(opad, 64); b.update(inner, 32); b.final(out);
}

inline uint32_t crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320UL & (0U - (crc & 1U)));
  }
  return ~crc;
}

// ---- persisted record -----------------------------------------------------------------------------------------------
constexpr uint32_t Magic = 0x4D415041UL;   // "MAPA"
constexpr uint16_t Schema = 1;
struct AuthRecord {
  uint32_t magic = Magic;
  uint16_t schema = Schema;
  uint16_t size = 0;
  uint32_t sequence = 0;
  uint8_t salt[16] = {0};
  uint8_t hash[32] = {0};
  uint8_t flags = 0;          // bit0: Web technical access permitted (default 0 = hidden)
  uint8_t failCount = 0;      // consecutive wrong tries since the last success (persisted)
  uint8_t lockLevel = 0;      // number of lock-outs since the last success (persisted)
  uint8_t reserved = 0;
  uint32_t crc = 0;
};
constexpr uint8_t FlagWebAllowed = 0x01;

inline uint32_t recordCrc(const AuthRecord &r) { return crc32(reinterpret_cast<const uint8_t *>(&r), offsetof(AuthRecord, crc)); }
inline bool recordValid(const AuthRecord &r) {
  return r.magic == Magic && r.schema == Schema && r.size == sizeof(AuthRecord) && r.crc == recordCrc(r);
}

// Storage contract (two alternating slots with sequence, supplied by the firmware or by the test):
//   LoadStatus load(AuthRecord &best) - Ok: newest valid record; Blank: both slots READ fine but hold no valid record (PIN not set yet);
//                                       Error: the chip could not be read (never mistaken for "no PIN": the gate stays closed)
//   bool save(AuthRecord &rec)   - rec.sequence/size/crc are filled by the caller; the storage writes the OLDER slot, reads back, verifies
//   uint32_t random32()
enum class LoadStatus : uint8_t { Ok, Blank, Error };
enum class Result : uint8_t { Ok, Wrong, Locked, NotSet, Invalid, StorageError, WebHidden, NotUnlocked };

inline bool pinFormatValid(const char *pin) {
  if (!pin) return false;
  for (uint8_t i = 0; i < PinDigits; ++i) if (pin[i] < '0' || pin[i] > '9') return false;
  return pin[PinDigits] == '\0';
}
inline bool timeReached(uint32_t now, uint32_t when) { return static_cast<int32_t>(now - when) >= 0; }
inline void secureZero(void *p, size_t n) { volatile uint8_t *v = static_cast<volatile uint8_t *>(p); while (n--) *v++ = 0; }

template <class Storage>
class Gate {
 public:
  explicit Gate(Storage &storage) : st_(storage) {}

  // Boot: load the record. A pending lock-out (failCount at/over the limit) starts its countdown now, so a reboot never shortens it.
  void begin(uint32_t now) {
    hmiUnlocked_ = false; webSession_ = false; lockUntil_ = 0; locked_ = false;
    const LoadStatus status = st_.load(rec_);
    loaded_ = status == LoadStatus::Ok;
    storageError_ = status == LoadStatus::Error;
    if (loaded_ && rec_.failCount >= MaxAttempts) startLock(now);
  }
  // A read error is not "no PIN": it keeps the gate closed (verify -> StorageError, no PIN can be created) until a retry reads the chip.
  bool storageError() const { return storageError_; }
  void retryLoad(uint32_t now) { if (storageError_) begin(now); }
  bool pinConfigured() const { return loaded_ || storageError_; }
  bool webAllowed() const { return loaded_ && (rec_.flags & FlagWebAllowed) != 0U; }
  uint8_t failCount() const { return loaded_ ? rec_.failCount : 0U; }

  bool lockedOut(uint32_t now) {
    if (locked_ && timeReached(now, lockUntil_)) {
      locked_ = false;
      rec_.failCount = 0;      // a fresh window of tries; the persisted copy is rewritten by the next try (a reboot before that re-locks)
    }
    return locked_;
  }
  uint32_t lockRemainingMs(uint32_t now) { return lockedOut(now) ? static_cast<uint32_t>(lockUntil_ - now) : 0U; }

  // Verify the digits (HMI or Web). Counts a wrong try BEFORE answering (persisted); success clears the counters.
  Result verify(uint32_t now, const char *pin) {
    if (storageError_) return Result::StorageError;
    if (!loaded_) return Result::NotSet;
    if (!pinFormatValid(pin)) return Result::Invalid;
    if (lockedOut(now)) return Result::Locked;
    uint8_t digest[32];
    derive(rec_.salt, pin, digest);
    uint8_t diff = 0;
    for (uint8_t i = 0; i < 32; ++i) diff = static_cast<uint8_t>(diff | (digest[i] ^ rec_.hash[i]));   // constant time
    secureZero(digest, sizeof(digest));
    if (diff == 0U) {
      if (rec_.failCount != 0U || rec_.lockLevel != 0U) {
        AuthRecord next = rec_; next.failCount = 0; next.lockLevel = 0;
        if (!persist(next)) { rec_.failCount = 0; rec_.lockLevel = 0; }   // the right PIN is never refused because the clear could not be saved
      }
      return Result::Ok;
    }
    AuthRecord next = rec_;
    if (next.failCount < 255U) ++next.failCount;
    const bool lockNow = next.failCount >= MaxAttempts;
    if (lockNow && next.lockLevel < 255U) ++next.lockLevel;
    (void)persist(next);          // a failed write must not give an extra try: the RAM counters below still apply
    if (rec_.failCount < next.failCount) rec_ = next;   // persist() only updates rec_ on success
    if (lockNow) { startLock(now); return Result::Locked; }
    return Result::Wrong;
  }

  // HMI session ----------------------------------------------------------------------------------------------------
  Result unlockHmi(uint32_t now, const char *pin) {
    const Result r = verify(now, pin);
    if (r == Result::Ok) { hmiUnlocked_ = true; hmiTouched_ = now; }
    return r;
  }
  bool hmiUnlocked(uint32_t now) {
    if (hmiUnlocked_ && timeReached(now, hmiTouched_ + HmiIdleMs)) hmiUnlocked_ = false;
    return hmiUnlocked_;
  }
  void touchHmi(uint32_t now) { if (hmiUnlocked(now)) hmiTouched_ = now; }
  void lockHmi() { hmiUnlocked_ = false; }

  // Create the PIN (first use: no session needed) or change it (HMI session required). Only the HMI calls this.
  Result setPin(uint32_t now, const char *newPin) {
    if (!pinFormatValid(newPin)) return Result::Invalid;
    if (storageError_) return Result::StorageError;
    if (loaded_ && !hmiUnlocked(now)) return Result::NotUnlocked;
    AuthRecord next = loaded_ ? rec_ : AuthRecord{};
    const uint32_t r0 = st_.random32(), r1 = st_.random32(), r2 = st_.random32(), r3 = st_.random32();
    const uint32_t rr[4] = {r0, r1, r2, r3};
    memcpy(next.salt, rr, sizeof(next.salt));
    derive(next.salt, newPin, next.hash);
    next.failCount = 0; next.lockLevel = 0;
    if (!persist(next)) return Result::StorageError;
    hmiUnlocked_ = true; hmiTouched_ = now;
    webSession_ = false;                 // a changed PIN ends every Web session
    locked_ = false;
    return Result::Ok;
  }

  // Web permission switch (HMI only). OFF revokes the Web session in the same call.
  Result setWebAllowed(uint32_t now, bool allowed) {
    if (!loaded_) return Result::NotSet;
    if (!hmiUnlocked(now)) return Result::NotUnlocked;
    AuthRecord next = rec_;
    if (allowed) next.flags = static_cast<uint8_t>(next.flags | FlagWebAllowed);
    else next.flags = static_cast<uint8_t>(next.flags & ~FlagWebAllowed);
    if (next.flags != rec_.flags && !persist(next)) return Result::StorageError;
    if (!allowed) revokeWeb();
    return Result::Ok;
  }

  // Web session ----------------------------------------------------------------------------------------------------
  Result unlockWeb(uint32_t now, const char *pin) {
    if (storageError_) return Result::StorageError;
    if (!loaded_) return Result::NotSet;
    if (!webAllowed()) return Result::WebHidden;     // hidden: not even a PIN try is accepted
    const Result r = verify(now, pin);
    if (r == Result::Ok) { webSession_ = true; webUntil_ = now + WebSessionMs; }
    return r;
  }
  bool webSessionActive(uint32_t now) {
    if (webSession_ && (!webAllowed() || timeReached(now, webUntil_))) webSession_ = false;
    return webSession_;
  }
  void revokeWeb() { webSession_ = false; }
  uint32_t webRemainingMs(uint32_t now) {
    return webSessionActive(now) ? static_cast<uint32_t>(webUntil_ - now) : 0U;
  }

  const AuthRecord &record() const { return rec_; }

 private:
  void startLock(uint32_t now) {
    uint32_t ms = LockBaseMs;
    for (uint8_t i = 1; i < rec_.lockLevel && ms < LockMaxMs; ++i) ms *= 2U;
    if (ms > LockMaxMs) ms = LockMaxMs;
    locked_ = true; lockUntil_ = now + ms;
    // failCount stays >= MaxAttempts in the record until the window ends: a reboot restarts (never shortens) the lock.
  }
  bool persist(AuthRecord next) {
    next.magic = Magic; next.schema = Schema; next.size = sizeof(AuthRecord);
    next.sequence = rec_.sequence + 1U;
    next.crc = recordCrc(next);
    if (!st_.save(next)) return false;
    rec_ = next; loaded_ = true;
    return true;
  }
  static void derive(const uint8_t salt[16], const char *pin, uint8_t out[32]) {
    uint8_t msg[48];
    memcpy(msg, "MAYAP-TECH-PIN-V1", 17);
    memcpy(msg + 17, pin, PinDigits);
    uint8_t acc[32];
    hmacSha256(salt, 16, msg, 17U + PinDigits, acc);
    for (uint16_t i = 1; i < HashRounds; ++i) {
      uint8_t next[32];
      hmacSha256(salt, 16, acc, 32, next);
      memcpy(acc, next, 32);
    }
    memcpy(out, acc, 32);
    secureZero(msg, sizeof(msg)); secureZero(acc, sizeof(acc));
  }

  Storage &st_;
  AuthRecord rec_{};
  bool loaded_ = false;
  bool storageError_ = false;
  bool locked_ = false;
  uint32_t lockUntil_ = 0;
  bool hmiUnlocked_ = false;
  uint32_t hmiTouched_ = 0;
  bool webSession_ = false;
  uint32_t webUntil_ = 0;
};

}  // namespace TechAccess
