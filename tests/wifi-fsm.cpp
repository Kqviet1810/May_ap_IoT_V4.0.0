// Pure host test of the ONLY Wi-Fi decision logic (wifi_fsm.h): CONNECTING -> CONNECTED -> BACKOFF -> RECOVERY.
#include "../MAYAP_INDUSTRIAL_v1_0_0/wifi_fsm.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
using namespace MayapNetwork;
static const uint32_t LADDER[] = {1000, 2000, 4000, 8000, 16000, 30000, 60000};
static WifiFsm make() { return WifiFsm(LADDER, 7); }
static unsigned begins = 0, recovers = 0;
// advance virtual time in `step` ms slices, call update with fixed Wi-Fi facts, count actions; `onBegin` mimics the caller issuing the connect
static void drive(WifiFsm &f, uint32_t &now, uint32_t duration, bool associated, uint32_t gaveUpAt = 0) {
  for (uint32_t t = 0; t < duration; t += 250U) {
    now += 250U;
    const WifiAction a = f.update(now, associated, gaveUpAt, 0U);
    if (a == WifiAction::Begin) { ++begins; f.attemptStarted(now); }
    else if (a == WifiAction::Recover) { ++recovers; f.recoveryDone(now); }
  }
}
int main() {
  // 1) cold start: one Begin at once, association -> CONNECTED, and then NOTHING for an hour (no pointless reconnect).
  { WifiFsm f = make(); uint32_t now = 5000; begins = recovers = 0; f.reset(now);
    drive(f, now, 250, false); assert(begins == 1 && f.state() == WifiState::Connecting);
    drive(f, now, 3000, true); assert(f.state() == WifiState::Connected);
    const unsigned b = begins; drive(f, now, 3600000U, true);
    assert(begins == b && recovers == 0 && f.state() == WifiState::Connected); }
  // 2) real loss of a link that had been good for >= 30 s: the first retry is immediate (no penalty); the history is forgotten only
  //    after the NEW join has itself been stable for 30 s.
  { WifiFsm f = make(); uint32_t now = 1000; begins = recovers = 0; f.reset(now);
    drive(f, now, 250, false); drive(f, now, 31000, true); assert(f.stable() && f.failures() == 0);
    drive(f, now, 250, false); assert(begins == 2 && f.state() == WifiState::Connecting && f.failures() == 0);
    drive(f, now, 31000, true); assert(f.state() == WifiState::Connected && f.failures() == 0 && recovers == 0 && f.stable()); }
  // 3) flapping AP: joined for ~3 s, dropped, over and over. The ladder must CLIMB (1,2,4,8,16,30,60 s) - never one reconnect per second.
  { WifiFsm f = make(); uint32_t now = 1000; begins = recovers = 0; f.reset(now);
    uint32_t lastBegin = 0, minGap[10] = {0}; unsigned n = 0;
    for (uint32_t t = 0; t < 40UL * 60UL * 1000UL && n < 10; t += 250U) {
      now += 250U;
      // the link comes up 500 ms after each Begin and stays up 3 s
      static uint32_t joinAt = 0; bool assoc = false;
      if (lastBegin && now - lastBegin >= 500U && now - lastBegin < 3500U) assoc = true;
      const WifiAction a = f.update(now, assoc, 0, 0);
      if (a == WifiAction::Begin) { if (lastBegin) minGap[n++] = now - lastBegin; lastBegin = now; ++begins; f.attemptStarted(now); }
      else if (a == WifiAction::Recover) { ++recovers; lastBegin = now; f.recoveryDone(now); }
      (void)joinAt;
    }
    assert(n >= 8);
    // gap = 0.5 s join + 3 s up + ladder step: strictly growing until the 60 s cap, first retry never sooner than 1 s after the drop
    for (unsigned i = 1; i < 7; ++i) assert(minGap[i] > minGap[i - 1] || recovers > 0);
    assert(minGap[0] >= 3500U + 1000U - 250U);
    std::printf("flap: n=%u recovers=%u failures=%u gaps:", n, recovers, f.failures()); for (unsigned i=0;i<n;++i) std::printf(" %u", minGap[i]); std::puts("");
    assert(f.failures() >= 5U || recovers > 0); }
  // 3b) a join that dropped after 1, 2, 3, 5 s never clears the history; a join held for exactly 30 s does.
  { const uint32_t holds[] = {1000, 2000, 3000, 5000, 29750};
    for (uint32_t hold : holds) {
      WifiFsm f = make(); uint32_t now = 1000; begins = recovers = 0; f.reset(now);
      drive(f, now, 250, false);                                   // Begin #1, attemptStarted
      drive(f, now, hold, true); assert(f.state() == WifiState::Connected && !f.stable());
      drive(f, now, 250, false);                                   // dropped
      assert(f.failures() == 1U && f.state() == WifiState::Backoff && f.lossCount() == 1U); }
    WifiFsm f = make(); uint32_t now = 1000; f.reset(now); drive(f, now, 250, false);
    drive(f, now, 30250, true); assert(f.stable() && f.failures() == 0); }
  // 3c) history survives the flapping: 3 quick drops then a good 31 s link; the NEXT drop retries at once (history forgotten only now).
  { WifiFsm f = make(); uint32_t now = 1000; begins = recovers = 0; f.reset(now);
    for (int i = 0; i < 3; ++i) { drive(f, now, 250, false); drive(f, now, 2000, true); drive(f, now, 250, false); now = f.backoffUntil(); }
    assert(f.failures() == 3U);
    drive(f, now, 250, false); drive(f, now, 31000, true); assert(f.failures() == 0U && f.stable());
    const unsigned b = begins; drive(f, now, 250, false); assert(begins == b + 1U && f.state() == WifiState::Connecting); }
  // 3d) kinds of loss: station left the AP (physical) vs kept the AP but lost the lease (IP). Both retry the same way.
  { WifiFsm f = make(); uint32_t now = 1000; f.reset(now); drive(f, now, 250, false); drive(f, now, 31000, true);
    f.update(now + 250U, false, now + 200U, 0, 0U); assert(f.lastLoss() == LinkLoss::Physical);
    WifiFsm g = make(); now = 1000; g.reset(now); drive(g, now, 250, false); drive(g, now, 31000, true);
    g.update(now + 250U, false, 0U, 0, now + 200U); assert(g.lastLoss() == LinkLoss::Ip);
    assert(g.update(now + 250U, false, 0U, 0, now + 200U) == WifiAction::Begin || g.state() == WifiState::Backoff); }
  // 4) the driver reports THIS attempt over: the wait ends 4 s later (not 12 s); a stale report from an earlier attempt is ignored.
  { WifiFsm f = make(); uint32_t now = 1000; f.reset(now);
    assert(f.update(now, false, 0, 0) == WifiAction::Begin); f.attemptStarted(now);
    const uint32_t gaveUp = now + 500U;
    assert(f.update(gaveUp + 3999U, false, gaveUp, 0) == WifiAction::None && f.state() == WifiState::Connecting);
    assert(f.update(gaveUp + 4000U, false, gaveUp, 0) == WifiAction::None && f.state() == WifiState::Backoff && f.failures() == 1);
    // next attempt: the old report (older than the attempt) must not shorten it
    now = gaveUp + 4000U + 1000U; assert(f.update(now, false, gaveUp, 0) == WifiAction::Begin); f.attemptStarted(now);
    assert(f.update(now + 11999U, false, gaveUp, 0) == WifiAction::None && f.state() == WifiState::Connecting);
    assert(f.update(now + 12000U, false, gaveUp, 0) == WifiAction::None && f.state() == WifiState::Backoff); }
  // 5) backoff ladder with jitter: delays 1,2,4,8,16,30,60,60... s after each failed attempt (recovery disabled for this check).
  { WifiFsmConfig c; c.recoveryAfterFailures = 255U; c.recoveryAfterOutageMs = 2000000000U;
    WifiFsm f(LADDER, 7, c); uint32_t now = 0; f.reset(now);
    const uint32_t expect[] = {1000, 2000, 4000, 8000, 16000, 30000, 60000, 60000, 60000};
    for (unsigned i = 0; i < 9; ++i) {
      assert(f.update(now, false, 0, 0) == WifiAction::Begin); f.attemptStarted(now);
      now += 12000U; assert(f.update(now, false, 0, 250U) == WifiAction::None);
      assert(f.state() == WifiState::Backoff && f.backoffUntil() == now + expect[i] + 250U);
      now = f.backoffUntil() - 1U; assert(f.update(now, false, 0, 0) == WifiAction::None);   // not a millisecond early
      now += 1U;
    } }
  // 6) recovery is allowed only after 6 failed attempts or a 5-minute outage, never more often than every 120 s; the driver is never
  //    re-initialised by the FSM (it only ever asks for Begin or Recover).
  { WifiFsm f = make(); uint32_t now = 100000; begins = recovers = 0; f.reset(now);
    uint32_t firstRecoverAt = 0;
    for (uint32_t t = 0; t < 30UL * 60UL * 1000UL; t += 250U) {
      now += 250U; const WifiAction a = f.update(now, false, 0, 0);
      if (a == WifiAction::Begin) { ++begins; f.attemptStarted(now); }
      else if (a == WifiAction::Recover) { ++recovers; if (!firstRecoverAt) firstRecoverAt = now; f.recoveryDone(now); }
    }
    assert(recovers >= 3 && recovers <= 7);                       // a few per 30 min of total outage, not one per retry
    assert(begins > recovers * 3U);
    assert(firstRecoverAt - 100000U >= 120000U);                  // never within the first minutes (6 failures take > 2 min)
  }
  // 7) cooldown: a second RECOVERY cannot start within 120 s of the previous one even if the outage persists.
  { WifiFsm f = make(); uint32_t now = 1000000; f.reset(now);
    uint32_t last = 0; unsigned n = 0; uint32_t minGap = 0xFFFFFFFFU;
    for (uint32_t t = 0; t < 6UL * 3600UL * 1000UL; t += 250U) {
      now += 250U; const WifiAction a = f.update(now, false, 0, 0);
      if (a == WifiAction::Begin) f.attemptStarted(now);
      else if (a == WifiAction::Recover) { if (last && now - last < minGap) minGap = now - last; last = now; ++n; f.recoveryDone(now); }
    }
    assert(n >= 5 && minGap >= 120000U); }
  // 8) MQTT / Cloud / OTA are not inputs: with the station associated, a broker that has been down for hours changes nothing.
  { WifiFsm f = make(); uint32_t now = 1; begins = recovers = 0; f.reset(now); drive(f, now, 250, false); drive(f, now, 1000, true);
    const unsigned b = begins; bool mqttConnected = false; (void)mqttConnected;      // intentionally never passed to the FSM
    drive(f, now, 6UL * 3600UL * 1000UL, true);
    assert(begins == b && recovers == 0 && f.state() == WifiState::Connected); }
  // 9) the caller could not even issue the connect (driver refused): counted as a failed attempt, backs off (no tight loop).
  { WifiFsm f = make(); uint32_t now = 10; f.reset(now);
    assert(f.update(now, false, 0, 0) == WifiAction::Begin);
    f.attemptFailed(now, 0U); assert(f.state() == WifiState::Backoff && f.failures() == 1);
    assert(f.update(now + 999U, false, 0, 0) == WifiAction::None); assert(f.update(now + 1000U, false, 0, 0) == WifiAction::Begin); }
  // 10) millis() rollover in the middle of an attempt and of the ladder.
  { WifiFsm f = make(); uint32_t now = 0xFFFFFF00U; f.reset(now);
    assert(f.update(now, false, 0, 0) == WifiAction::Begin); f.attemptStarted(now);
    now += 12000U; f.update(now, false, 0, 0); assert(f.state() == WifiState::Backoff);
    assert(f.update(now + 1000U, false, 0, 0) == WifiAction::Begin); }
  // 11) reset() (Offline -> Online, portal closed): immediate attempt, failure history forgotten.
  { WifiFsm f = make(); uint32_t now = 50; f.reset(now);
    for (int i = 0; i < 4; ++i) { f.update(now, false, 0, 0); f.attemptStarted(now); now += 12000U; f.update(now, false, 0, 0); now = f.backoffUntil(); }
    assert(f.failures() == 4); f.reset(now); assert(f.failures() == 0 && f.update(now, false, 0, 0) == WifiAction::Begin); }
  std::puts("Wi-Fi FSM: cold start, loss/immediate retry, flapping, driver-gave-up, ladder, recovery gating + cooldown, MQTT-independence, rollover PASS");
}
