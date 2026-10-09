// Technical access (PIN, lock-out, Web permission) and the 3-slot advanced history: host qualification.
// Links the real headers; the storage below is a byte-accurate two-slot / three-slot fake with torn-write injection.
#include "../MAYAP_INDUSTRIAL_v1_0_0/advanced_history.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace TechAccess;
static unsigned checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); std::abort(); } } while (0)

// ---- fake auth storage: two alternating slots, optional torn write ------------------------------------------------------
struct FakeAuthStore {
  AuthRecord slot[2];
  bool used[2] = {false, false};
  int failWrites = 0;          // > 0: the next save() fails (nothing written)
  int tearBytes = -1;          // >= 0: the next save() writes only this many bytes into the older slot and reports failure (power cut)
  uint32_t rng = 0x12345678UL;
  unsigned saves = 0;
  bool readError = false;
  LoadStatus load(AuthRecord &best) {
    if (readError) return LoadStatus::Error;
    bool have = false;
    for (int i = 0; i < 2; ++i)
      if (used[i] && recordValid(slot[i]) && (!have || static_cast<int32_t>(slot[i].sequence - best.sequence) > 0)) { best = slot[i]; have = true; }
    return have ? LoadStatus::Ok : LoadStatus::Blank;
  }
  bool save(AuthRecord &rec) {
    if (failWrites > 0) { --failWrites; return false; }
    int target = 0;
    if (used[0] && used[1]) target = static_cast<int32_t>(slot[0].sequence - slot[1].sequence) > 0 ? 1 : 0;
    else if (used[0]) target = 1;
    if (tearBytes >= 0) {
      uint8_t buf[sizeof(AuthRecord)];
      memcpy(buf, &slot[target], sizeof(buf));
      memcpy(buf, &rec, static_cast<size_t>(tearBytes) < sizeof(buf) ? static_cast<size_t>(tearBytes) : sizeof(buf));
      memcpy(&slot[target], buf, sizeof(buf)); used[target] = true; tearBytes = -1;
      return false;
    }
    slot[target] = rec; used[target] = true; ++saves;
    return true;
  }
  uint32_t random32() { rng = rng * 1664525UL + 1013904223UL; return rng; }
};

static void shaVectors() {
  uint8_t out[32];
  auto hex = [&](const uint8_t *d) { std::string s; char b[3]; for (int i = 0; i < 32; ++i) { std::snprintf(b, 3, "%02x", d[i]); s += b; } return s; };
  { Sha256 s; s.update(reinterpret_cast<const uint8_t *>("abc"), 3); s.final(out);
    CHECK(hex(out) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"); }
  { Sha256 s; s.final(out); CHECK(hex(out) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"); }
  { Sha256 s; const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"; s.update(reinterpret_cast<const uint8_t *>(m), strlen(m)); s.final(out);
    CHECK(hex(out) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"); }
  { Sha256 s; std::vector<uint8_t> a(1000, 'a'); for (int i = 0; i < 1000; ++i) s.update(a.data(), a.size()); s.final(out);
    CHECK(hex(out) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"); }
  // RFC 4231 test case 1 and 2
  { uint8_t key[20]; memset(key, 0x0b, 20); hmacSha256(key, 20, reinterpret_cast<const uint8_t *>("Hi There"), 8, out);
    CHECK(hex(out) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"); }
  { hmacSha256(reinterpret_cast<const uint8_t *>("Jefe"), 4, reinterpret_cast<const uint8_t *>("what do ya want for nothing?"), 28, out);
    CHECK(hex(out) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"); }
}

static void pinAndLockout() {
  FakeAuthStore st; Gate<FakeAuthStore> g(st);
  uint32_t t = 1000;
  g.begin(t);
  CHECK(!g.pinConfigured() && g.verify(t, "1234") == Result::NotSet);
  CHECK(!g.webAllowed());                                         // default: hidden
  CHECK(g.setPin(t, "12a4") == Result::Invalid && g.setPin(t, "123") == Result::Invalid && g.setPin(t, "12345") == Result::Invalid);
  CHECK(g.setPin(t, "4821") == Result::Ok && g.pinConfigured() && g.hmiUnlocked(t));
  // the digits never reach the record: no ASCII "4821", and the record is only salt + hash
  { const uint8_t *raw = reinterpret_cast<const uint8_t *>(&st.slot[0]); bool leak = false;
    for (size_t i = 0; i + 4 <= sizeof(AuthRecord); ++i) if (!memcmp(raw + i, "4821", 4)) leak = true;
    CHECK(!leak); }
  g.lockHmi();
  CHECK(!g.hmiUnlocked(t));
  CHECK(g.verify(t, "0000") == Result::Wrong && g.failCount() == 1);
  CHECK(g.verify(t, "4821") == Result::Ok && g.failCount() == 0);       // success clears the strikes (persisted)
  // five wrong tries -> locked 60 s; the right PIN is refused during the lock and does not extend it
  for (int i = 0; i < 4; ++i) CHECK(g.verify(t, "1111") == Result::Wrong);
  CHECK(g.verify(t, "1111") == Result::Locked && g.lockedOut(t));
  CHECK(g.verify(t + 30000, "4821") == Result::Locked);
  CHECK(g.lockRemainingMs(t + 30000) == 30000UL);
  CHECK(g.verify(t + LockBaseMs, "4821") == Result::Ok);                  // window over
  // second lock-out of a streak doubles (120 s), third 240 s ... capped at 15 min
  uint32_t now = t + LockBaseMs;
  uint32_t expected = LockBaseMs;
  for (int level = 1; level <= 7; ++level) {
    for (int i = 0; i < 5; ++i) (void)g.verify(now, "9999");
    CHECK(g.lockedOut(now));
    if (level >= 2) { expected = expected * 2U > LockMaxMs ? LockMaxMs : expected * 2U; }
    CHECK(g.lockRemainingMs(now) == expected);
    now += expected;                // window ends; failures keep escalating because no success happened
    CHECK(!g.lockedOut(now));
  }
  CHECK(expected == LockMaxMs);
  // a power cycle in the middle of a lock restarts it (never shortens it)
  for (int i = 0; i < 5; ++i) (void)g.verify(now, "9999");
  CHECK(g.lockedOut(now));
  { Gate<FakeAuthStore> g2(st); g2.begin(now + 50000UL); CHECK(g2.lockedOut(now + 50000UL)); CHECK(g2.lockRemainingMs(now + 50000UL) == LockMaxMs);
    CHECK(g2.verify(now + 50000UL, "4821") == Result::Locked); }
  // the strike counter survives a reboot too (3 wrong tries, reboot, 2 more -> locked)
  { FakeAuthStore s2; Gate<FakeAuthStore> a(s2); a.begin(0); CHECK(a.setPin(0, "2580") == Result::Ok); a.lockHmi();
    for (int i = 0; i < 3; ++i) CHECK(a.verify(10, "0000") == Result::Wrong);
    Gate<FakeAuthStore> b(s2); b.begin(500);
    CHECK(b.verify(500, "0000") == Result::Wrong); CHECK(b.verify(500, "0000") == Result::Locked); }
}

static void sessions() {
  FakeAuthStore st; Gate<FakeAuthStore> g(st);
  uint32_t t = 5000; g.begin(t);
  CHECK(g.setPin(t, "1357") == Result::Ok);
  // HMI session: idle timeout, touch extends, relock on exit, reboot always locked
  CHECK(g.hmiUnlocked(t + HmiIdleMs - 1));
  g.touchHmi(t + HmiIdleMs - 1);
  CHECK(g.hmiUnlocked(t + 2 * HmiIdleMs - 2));
  CHECK(!g.hmiUnlocked(t + 3 * HmiIdleMs));
  CHECK(g.unlockHmi(t + 3 * HmiIdleMs, "1357") == Result::Ok);
  g.lockHmi(); CHECK(!g.hmiUnlocked(t + 3 * HmiIdleMs));
  { Gate<FakeAuthStore> r(st); r.begin(0); CHECK(!r.hmiUnlocked(0)); }
  // change PIN needs a live HMI session; the old PIN stops working
  CHECK(g.setPin(t + 4 * HmiIdleMs, "2468") == Result::NotUnlocked);
  CHECK(g.unlockHmi(t + 4 * HmiIdleMs, "1357") == Result::Ok);
  CHECK(g.setPin(t + 4 * HmiIdleMs, "2468") == Result::Ok);
  g.lockHmi();
  CHECK(g.verify(t + 4 * HmiIdleMs, "1357") == Result::Wrong && g.verify(t + 4 * HmiIdleMs, "2468") == Result::Ok);
  // Web: hidden by default, a hidden Web cannot even try the PIN (no strike)
  const uint32_t w = t + 5 * HmiIdleMs;
  CHECK(!g.webAllowed() && g.unlockWeb(w, "2468") == Result::WebHidden && g.failCount() == 0);
  CHECK(g.setWebAllowed(w, true) == Result::NotUnlocked);              // only an unlocked HMI session may grant it
  CHECK(g.unlockHmi(w, "2468") == Result::Ok && g.setWebAllowed(w, true) == Result::Ok && g.webAllowed());
  CHECK(!g.webSessionActive(w));
  CHECK(g.unlockWeb(w, "0000") == Result::Wrong && !g.webSessionActive(w));
  CHECK(g.unlockWeb(w, "2468") == Result::Ok && g.webSessionActive(w + WebSessionMs - 1));
  CHECK(!g.webSessionActive(w + WebSessionMs));                          // absolute expiry
  // revoke: switching OFF ends the session in the same call; permission is persisted across a reboot
  CHECK(g.unlockWeb(w + WebSessionMs, "2468") == Result::Ok);
  CHECK(g.webSessionActive(w + WebSessionMs));
  CHECK(g.unlockHmi(w + WebSessionMs, "2468") == Result::Ok);
  CHECK(g.setWebAllowed(w + WebSessionMs, false) == Result::Ok && !g.webSessionActive(w + WebSessionMs) && !g.webAllowed());
  CHECK(g.setWebAllowed(w + WebSessionMs, true) == Result::Ok);
  { Gate<FakeAuthStore> r(st); r.begin(0); CHECK(r.webAllowed()); CHECK(!r.webSessionActive(0)); }   // permission persists, sessions never
  // changing the PIN ends a Web session; HMI and Web share ONE strike counter
  CHECK(g.unlockWeb(w + WebSessionMs, "2468") == Result::Ok);
  CHECK(g.setPin(w + WebSessionMs, "1111") == Result::Ok && !g.webSessionActive(w + WebSessionMs));
  g.lockHmi();
  const uint32_t x = w + WebSessionMs;
  CHECK(g.unlockWeb(x, "0000") == Result::Wrong && g.unlockHmi(x, "0001") == Result::Wrong && g.unlockWeb(x, "0002") == Result::Wrong &&
        g.unlockHmi(x, "0003") == Result::Wrong && g.unlockWeb(x, "0004") == Result::Locked);
  // millis rollover
  { FakeAuthStore s2; Gate<FakeAuthStore> r(s2); const uint32_t n0 = 0xFFFFFF00UL; r.begin(n0);
    CHECK(r.setPin(n0, "9090") == Result::Ok); CHECK(r.hmiUnlocked(n0 + 100));            // crosses 2^32
    CHECK(!r.hmiUnlocked(n0 + HmiIdleMs + 5000)); }
}

static void storagePowerCut() {
  // torn write at every byte offset while changing the PIN: after the "reboot" one of the two PINs is valid, never neither and never both
  const int bytes = static_cast<int>(sizeof(AuthRecord));
  for (int cut = 0; cut <= bytes; ++cut) {
    FakeAuthStore st; Gate<FakeAuthStore> g(st);
    g.begin(0); CHECK(g.setPin(0, "1234") == Result::Ok);
    CHECK(g.unlockHmi(10, "1234") == Result::Ok);
    st.tearBytes = cut;
    const Result r = g.setPin(20, "5678");
    Gate<FakeAuthStore> boot(st); boot.begin(30);
    CHECK(boot.pinConfigured());
    const bool oldOk = boot.verify(40, "1234") == Result::Ok, newOk = boot.verify(40, "5678") == Result::Ok;
    CHECK(oldOk != newOk);                                    // exactly one
    if (r == Result::Ok) CHECK(newOk);
    else if (cut < bytes) CHECK(oldOk);                           // a torn write never replaces the valid record
  }
  // write failure on a wrong try still counts (RAM) - the failure of the chip cannot grant extra tries
  { FakeAuthStore st; Gate<FakeAuthStore> g(st); g.begin(0); CHECK(g.setPin(0, "1234") == Result::Ok); g.lockHmi();
    st.failWrites = 100;
    for (int i = 0; i < 4; ++i) CHECK(g.verify(1, "0000") == Result::Wrong);
    CHECK(g.verify(1, "0000") == Result::Locked); }
  // the right PIN is not refused because the strike counters could not be cleared
  { FakeAuthStore st; Gate<FakeAuthStore> g(st); g.begin(0); CHECK(g.setPin(0, "1234") == Result::Ok); g.lockHmi();
    CHECK(g.verify(1, "0000") == Result::Wrong); st.failWrites = 100; CHECK(g.verify(1, "1234") == Result::Ok); }
  // an unreadable chip is NOT "no PIN": nothing can be created or verified until a retry reads it
  { FakeAuthStore st; { Gate<FakeAuthStore> g(st); g.begin(0); CHECK(g.setPin(0, "1234") == Result::Ok); }
    st.readError = true; Gate<FakeAuthStore> g(st); g.begin(10);
    CHECK(g.storageError() && g.pinConfigured() && g.verify(10, "1234") == Result::StorageError && g.setPin(10, "9999") == Result::StorageError);
    CHECK(g.unlockWeb(10, "1234") == Result::StorageError && !g.webAllowed());
    st.readError = false; g.retryLoad(20); CHECK(!g.storageError() && g.verify(20, "1234") == Result::Ok); }
  // setPin / setWebAllowed report a storage error and change nothing
  { FakeAuthStore st; Gate<FakeAuthStore> g(st); g.begin(0); CHECK(g.setPin(0, "1234") == Result::Ok);
    st.failWrites = 1; CHECK(g.setPin(1, "9999") == Result::StorageError); g.lockHmi(); CHECK(g.verify(2, "1234") == Result::Ok);
    CHECK(g.unlockHmi(3, "1234") == Result::Ok); st.failWrites = 1; CHECK(g.setWebAllowed(4, true) == Result::StorageError && !g.webAllowed()); }
}

// ---- advanced history ----------------------------------------------------------------------------------------------------
struct FakeConfig {
  float kp = 18, ki = 0.8f, kd = 45, tempOffset = 0, humidityOffset = 0, heaterStuckMinRiseC = 0.5f, tempRateLimitC = 2, autotuneBandC = 0.2f;
  uint16_t pidCycleSec = 10, heaterStuckDurationSec = 600, tempRateWindowSec = 300, tempOscillationWindowSec = 900;
  uint8_t maxHeaterPower = 100, tempOscillationCrossLimit = 8, autotuneRelayPowerPercent = 30;
  bool adaptiveThermalBalanceEnabled = true;
  float targetTemp = 37.5f, highTempAlarm = 38.2f, emergencyTemp = 39.0f;   // NOT part of the snapshot
};
struct FakeHistStore {
  AdvancedHistory::Record slot[AdvancedHistory::Slots];
  bool written[AdvancedHistory::Slots] = {false, false, false};
  int tearBytes = -1; int failWrites = 0; unsigned writes = 0;
  bool read(uint8_t s, AdvancedHistory::Record &r) { if (!written[s]) { r = AdvancedHistory::Record(); r.magic = 0xFFFFFFFFUL; return true; } r = slot[s]; return true; }
  bool write(uint8_t s, const AdvancedHistory::Record &r) {
    if (failWrites > 0) { --failWrites; return false; }
    if (tearBytes >= 0) { uint8_t buf[sizeof(r)]; memcpy(buf, &slot[s], sizeof(buf)); memcpy(buf, &r, static_cast<size_t>(tearBytes) < sizeof(buf) ? static_cast<size_t>(tearBytes) : sizeof(buf));
      memcpy(&slot[s], buf, sizeof(buf)); written[s] = true; tearBytes = -1; return false; }
    slot[s] = r; written[s] = true; ++writes; return true;
  }
};
static AdvancedHistory::Snapshot snapKp(float kp) { FakeConfig c; c.kp = kp; return AdvancedHistory::fromConfig(c); }

static void historyTests() {
  using namespace AdvancedHistory;
  { FakeConfig c; c.kp = 22; c.maxHeaterPower = 70; c.tempOffset = -0.4f; const Snapshot s = fromConfig(c); FakeConfig d; applyTo(d, s);
    CHECK(d.kp == 22 && d.maxHeaterPower == 70 && d.tempOffset == -0.4f && d.targetTemp == 37.5f && d.highTempAlarm == 38.2f && d.emergencyTemp == 39.0f); }
  FakeHistStore st; Ring<FakeHistStore> h(st); h.begin();
  CHECK(h.count() == 0);
  Snapshot s;
  CHECK(!h.get(0, s));
  CHECK(h.push(snapKp(10)) && h.push(snapKp(11)) && h.push(snapKp(12)));
  CHECK(h.count() == 3);
  CHECK(h.get(0, s) && s.kp == 12 && h.get(1, s) && s.kp == 11 && h.get(2, s) && s.kp == 10 && !h.get(3, s));
  // the fourth push replaces the OLDEST and nothing else; identical-to-newest pushes write nothing
  const unsigned w0 = st.writes;
  CHECK(h.push(snapKp(12)) && st.writes == w0);
  CHECK(h.push(snapKp(13)) && st.writes == w0 + 1);
  CHECK(h.get(0, s) && s.kp == 13 && h.get(1, s) && s.kp == 12 && h.get(2, s) && s.kp == 11);
  // reload from "EEPROM": the same order
  { Ring<FakeHistStore> r(st); r.begin(); CHECK(r.count() == 3 && r.get(0, s) && s.kp == 13 && r.get(2, s) && s.kp == 11); }
  // a failed write loses nothing that was already valid
  st.failWrites = 1; CHECK(!h.push(snapKp(14)));
  { Ring<FakeHistStore> r(st); r.begin(); CHECK(r.get(0, s) && s.kp == 13 && r.get(1, s) && s.kp == 12); }
  // torn write at every offset: the two newest records always survive intact, the ring stays ordered, no duplicate sequence
  const int bytes = static_cast<int>(sizeof(Record));
  for (int cut = 0; cut <= bytes; ++cut) {
    FakeHistStore s2; Ring<FakeHistStore> r(s2); r.begin();
    CHECK(r.push(snapKp(1)) && r.push(snapKp(2)) && r.push(snapKp(3)));
    s2.tearBytes = cut; (void)r.push(snapKp(4));
    Ring<FakeHistStore> boot(s2); boot.begin();
    Snapshot a, b;
    CHECK(boot.count() >= 2);
    CHECK(boot.get(0, a));
    if (cut >= bytes) CHECK(a.kp == 4);                            // a fully written torn "cut" is a complete record
    else {
      CHECK(a.kp == 3);                                            // the newest valid record is untouched
      CHECK(boot.get(1, b) && b.kp == 2);                          // and so is the next one
    }
  }
  // a snapshot carries nothing safety-independent
  static_assert(sizeof(Snapshot) == 44, "");
}

int main() {
  shaVectors();
  pinAndLockout();
  sessions();
  storagePowerCut();
  historyTests();
  std::printf("tech access + advanced history: SHA/HMAC vectors, PIN, lock-out, sessions, Web gate, torn writes, 3-slot ring (%u checks) PASS\n", checks);
  return 0;
}
