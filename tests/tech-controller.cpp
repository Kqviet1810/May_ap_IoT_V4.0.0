// REAL MachineController technical-access glue (sliced from machine_control.h by tools/test_tech_access.py) on fakes.
// Covers: PIN right/wrong/lock-out on HMI and Web (one shared counter), Web hidden/shown/revoked, Web request -> HMI yes / no /
// timeout / network loss, safety re-check at execution (batch, resume, autotune, storage), no write when nothing changed, undo
// record before every apply, restore + undo, power cut during the old-record write, remote callers cannot reach HMI-only commands.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))

#include "advanced_history.h"
#include "tech_request.h"
#include "tech_access.h"
#include "actual-tech-config.inc"
#include "actual-hmi-command-type.inc"

enum class HmiCommandSource { Local, Remote };
struct HmiCommand { uint32_t id = 0; HmiCommandType type = HmiCommandType::None; uint32_t createdAt = 0; uint16_t validForMs = 0, actuatorLeaseMs = 0;
  uint32_t alarmMask = 0; HmiCommandSource source = HmiCommandSource::Local; };

struct MachineConfig {
  float kp = 5, ki = 0.1f, kd = 30, tempOffset = 0, humidityOffset = 0, heaterStuckMinRiseC = 0.3f, tempRateLimitC = 2, autotuneBandC = 0.2f;
  uint16_t pidCycleSec = 2, heaterStuckDurationSec = 600, tempRateWindowSec = 300, tempOscillationWindowSec = 900;
  uint8_t maxHeaterPower = 100, tempOscillationCrossLimit = 6, autotuneRelayPowerPercent = 40;
  bool adaptiveThermalBalanceEnabled = false;
  float targetTemp = 37.5f, highTempAlarm = 38.2f, emergencyTemp = 39.0f;     // must never be touched by an advanced apply
};
static void sanitizeMachineConfig(MachineConfig &c) {
  c.kp = std::min(100.0f, std::max(0.0f, c.kp));
  c.maxHeaterPower = static_cast<uint8_t>(std::min<int>(100, std::max<int>(10, c.maxHeaterPower)));
}
static bool mayapPidHasAuthority(const MachineConfig &c) { return c.kp > 0 || c.ki > 0 || c.kd > 0; }
static bool timeReached(uint32_t now, uint32_t deadline) { return static_cast<int32_t>(now - deadline) >= 0; }

static unsigned checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); std::abort(); } } while (0)

// ---- fakes -----------------------------------------------------------------------------------------------------------------
struct FakeAuth {
  TechAccess::AuthRecord slot[2]; bool used[2] = {false, false}; uint32_t rng = 0xC0FFEEUL;
  TechAccess::LoadStatus load(TechAccess::AuthRecord &best) {
    bool have = false;
    for (int i = 0; i < 2; ++i) if (used[i] && TechAccess::recordValid(slot[i]) && (!have || static_cast<int32_t>(slot[i].sequence - best.sequence) > 0)) { best = slot[i]; have = true; }
    return have ? TechAccess::LoadStatus::Ok : TechAccess::LoadStatus::Blank;
  }
  bool save(TechAccess::AuthRecord &rec) {
    int target = 0;
    if (used[0] && used[1]) target = static_cast<int32_t>(slot[0].sequence - slot[1].sequence) > 0 ? 1 : 0; else if (used[0]) target = 1;
    slot[target] = rec; used[target] = true; return true;
  }
  uint32_t random32() { rng = rng * 1664525UL + 1013904223UL; return rng; }
};
struct FakeHistory {
  AdvancedHistory::Record slot[AdvancedHistory::Slots]; bool used[AdvancedHistory::Slots] = {false, false, false};
  int tearNext = -1;            // >= 0: the next write is a power cut after this many bytes (and reports failure)
  unsigned writes = 0;
  bool read(uint8_t s, AdvancedHistory::Record &r) { if (s >= AdvancedHistory::Slots || !used[s]) return false; r = slot[s]; return true; }
  bool write(uint8_t s, const AdvancedHistory::Record &r) {
    if (tearNext >= 0) {
      uint8_t buf[sizeof(AdvancedHistory::Record)]; memcpy(buf, &slot[s], sizeof(buf));
      memcpy(buf, &r, std::min<size_t>(static_cast<size_t>(tearNext), sizeof(buf))); memcpy(&slot[s], buf, sizeof(buf)); used[s] = true; tearNext = -1; return false;
    }
    slot[s] = r; used[s] = true; ++writes; return true;
  }
};
struct FakeStore { unsigned saves = 0; bool failSave = false;
  bool saveConfig(const MachineConfig &c, MachineConfig &readback) { if (failSave) return false; ++saves; readback = c; return true; } };
struct FakeAutoTune { bool run = false; bool running() const { return run; } };
struct FakeRuntime { bool networkConnected = true; TechStatus tech; };

struct Ctl {
  MachineConfig config_;
  FakeRuntime runtime_;
  FakeStore store_;
  FakeAutoTune autotune_;
  FakeAuth auth_;
  FakeHistory hist_;
  TechAccess::Gate<FakeAuth> techGate_{auth_};
  AdvancedHistory::Ring<FakeHistory> history_{hist_};
  bool batchRunning_ = false, resumePending_ = false, batchClearPending_ = false, safetyJournalFaultLatched_ = false;
  bool storageFaultLatched_ = false, storageDegraded_ = false;
  int autoTuneStarts = 0, commits = 0;
  bool startAutoTune(uint32_t, const char *&message) { ++autoTuneStarts; message = "DA KHOI DONG"; return true; }
  void commitSavedConfig(uint32_t, const MachineConfig &, const MachineConfig &readback) { config_ = readback; ++commits; }
  void latchStorageFault(const char *) { storageFaultLatched_ = true; }
  void clearStorageDegraded(uint32_t) { storageDegraded_ = false; }
#include "actual-tech-members.inc"
#include "actual-tech-funcs.inc"

  // ---- test helpers (not part of the firmware) ----
  bool run(uint32_t now, HmiCommandType type, bool remote, uint32_t param, std::string &msg) {
    HmiCommand c; c.type = type; c.alarmMask = param; c.source = remote ? HmiCommandSource::Remote : HmiCommandSource::Local;
    const char *m = "";
    const bool ok = handleTechCommand(now, c, m);
    msg = m ? m : "";
    serviceTech(now);
    return ok;
  }
};

static bool stage(MayapTech::Kind kind, std::initializer_list<std::pair<uint8_t, double>> fields, const char *id, const MachineConfig &base) {
  AdvancedHistory::Snapshot values = AdvancedHistory::fromConfig(base);
  uint16_t mask = 0;
  for (auto &f : fields) { CHECK(AdvancedHistory::setField(values, f.first, f.second)); mask = static_cast<uint16_t>(mask | (1U << f.first)); }
  return MayapTech::stage(kind, mask, values, id);
}

// Fresh controller: PIN 1234 set on the "HMI", HMI unlocked at t=1000.
static void fresh(Ctl &c, uint32_t &now) {
  MayapTech::discard();
  now = 1000;
  c.techGate_.begin(now);
  std::string m;
  CHECK(c.run(now, HmiCommandType::TechPinSet, false, 1234, m));        // first PIN: allowed without a session, opens the HMI session
  CHECK(c.runtime_.tech.hmiUnlocked && c.runtime_.tech.pinSet && !c.runtime_.tech.webAllowed);   // Web permission defaults to HIDDEN
}
static void grantWeb(Ctl &c, uint32_t now) {
  std::string m;
  CHECK(c.run(now, HmiCommandType::TechWebSet, false, 1, m));
  CHECK(c.run(now, HmiCommandType::TechPinVerify, true, 1234, m));
  CHECK(c.runtime_.tech.webAllowed && c.runtime_.tech.webSession);
}

int main() {
  std::string m;
  uint32_t now;

  // 1. Web is HIDDEN by default: its PIN is refused with WEB_HIDDEN and no request can ever be parked.
  { Ctl c; fresh(c, now);
    CHECK(!c.run(now, HmiCommandType::TechPinVerify, true, 1234, m) && m == "WEB_HIDDEN");
    CHECK(stage(MayapTech::Kind::Config, {{AdvancedHistory::F_KP, 9.0}}, "r1", c.config_));
    CHECK(!c.run(now, HmiCommandType::TechRequestSubmit, true, 0, m) && m == "WEB_HIDDEN");
    CHECK(c.runtime_.tech.pendingKind == 0);
    MayapTech::Staged leftover; CHECK(!MayapTech::take(leftover));       // the refusal also dropped the parked values
  }

  // 2. Wrong PINs on the Web and the HMI share ONE counter; the 5th failure locks BOTH.
  { Ctl c; fresh(c, now); grantWeb(c, now);
    c.techGate_.lockHmi();
    for (int i = 1; i <= 2; ++i) { CHECK(!c.run(now, HmiCommandType::TechPinVerify, true, 1111, m)); CHECK(m == (std::string("WRONG_PIN ") + std::to_string(i) + "/5")); }
    CHECK(!c.run(now, HmiCommandType::TechPinVerify, false, 2222, m) && m.rfind("SAI MA 3/5", 0) == 0);   // HMI attempt adds to the same counter
    CHECK(!c.run(now, HmiCommandType::TechPinVerify, true, 3333, m));
    CHECK(!c.run(now, HmiCommandType::TechPinVerify, false, 4444, m));
    CHECK(c.runtime_.tech.lockRemainingS > 0);
    CHECK(!c.run(now, HmiCommandType::TechPinVerify, true, 1234, m) && m.rfind("LOCKED", 0) == 0);       // right PIN refused while locked
    CHECK(!c.run(now, HmiCommandType::TechPinVerify, false, 1234, m) && m.rfind("BI KHOA", 0) == 0);
    // lock survives a reboot (persisted strikes): a new gate on the same storage is still locked
    Ctl d; d.auth_ = c.auth_; d.techGate_.begin(now + 1000);
    CHECK(d.techGate_.lockedOut(now + 1000));
  }

  // 3. Request -> HMI says NO: nothing changes, result Declined.
  { Ctl c; fresh(c, now); grantWeb(c, now);
    const MachineConfig before = c.config_;
    CHECK(stage(MayapTech::Kind::Config, {{AdvancedHistory::F_KP, 9.0}, {AdvancedHistory::F_MAX_POWER, 70}}, "req-no", c.config_));
    CHECK(c.run(now, HmiCommandType::TechRequestSubmit, true, 0, m) && m == "PENDING_HMI");
    CHECK(c.runtime_.tech.pendingKind == 1 && c.runtime_.tech.pendingMask == ((1U << AdvancedHistory::F_KP) | (1U << AdvancedHistory::F_MAX_POWER)));
    CHECK(c.store_.saves == 0 && c.config_.kp == before.kp);              // the request itself applies NOTHING
    MayapTech::Detail d; MayapTech::readDetail(d); CHECK(d.pending.kp == 9.0f && d.pending.maxHeaterPower == 70 && std::string(d.resultId) == "req-no" && c.runtime_.tech.resultState == TechResult::Pending);
    CHECK(!c.run(now, HmiCommandType::TechRequestSubmit, true, 0, m));    // nothing staged any more
    CHECK(c.run(now + 5000, HmiCommandType::TechDecision, false, 0, m));
    CHECK(c.runtime_.tech.pendingKind == 0 && c.runtime_.tech.resultState == TechResult::Declined);
    MayapTech::readDetail(d); CHECK(std::string(d.resultId) == "req-no");
    CHECK(c.store_.saves == 0 && c.config_.kp == before.kp && c.commits == 0);
  }

  // 4. Request -> HMI says YES: applied once, undo record written first, only the technical fields change.
  { Ctl c; fresh(c, now); grantWeb(c, now);
    CHECK(stage(MayapTech::Kind::Config, {{AdvancedHistory::F_KP, 9.0}, {AdvancedHistory::F_TEMP_OFFSET, 0.5}}, "req-yes", c.config_));
    CHECK(c.run(now, HmiCommandType::TechRequestSubmit, true, 0, m));
    CHECK(c.run(now + 1000, HmiCommandType::TechDecision, false, 1, m) && m == "DA AP DUNG");
    CHECK(c.config_.kp == 9.0f && c.config_.tempOffset == 0.5f && c.config_.ki == 0.1f);
    CHECK(c.config_.targetTemp == 37.5f && c.config_.highTempAlarm == 38.2f && c.config_.emergencyTemp == 39.0f);   // safety thresholds untouched
    CHECK(c.store_.saves == 1 && c.commits == 1 && c.runtime_.tech.resultState == TechResult::Applied);
    CHECK(c.runtime_.tech.historyCount == 1);                              // the replaced values are the newest old record
    MayapTech::Detail d; MayapTech::readDetail(d); CHECK(d.history[0].kp == 5.0f && d.history[0].tempOffset == 0.0f && std::string(d.resultId) == "req-yes");
  }

  // 5. 60 s without an answer: the request cancels itself and nothing is applied (a late "yes" has nothing to confirm).
  { Ctl c; fresh(c, now); grantWeb(c, now);
    CHECK(stage(MayapTech::Kind::Config, {{AdvancedHistory::F_KP, 9.0}}, "req-late", c.config_));
    CHECK(c.run(now, HmiCommandType::TechRequestSubmit, true, 0, m));
    c.serviceTech(now + 59000); CHECK(c.runtime_.tech.pendingKind == 1);
    c.serviceTech(now + 60000); CHECK(c.runtime_.tech.pendingKind == 0 && c.runtime_.tech.resultState == TechResult::Expired);
    CHECK(!c.run(now + 61000, HmiCommandType::TechDecision, false, 1, m));
    CHECK(c.store_.saves == 0 && c.config_.kp == 5.0f);
  }

  // 6. HMI turns the Web permission off: session + pending request die at once, a late yes cannot revive them.
  { Ctl c; fresh(c, now); grantWeb(c, now);
    CHECK(stage(MayapTech::Kind::Config, {{AdvancedHistory::F_KP, 9.0}}, "req-rev", c.config_));
    CHECK(c.run(now, HmiCommandType::TechRequestSubmit, true, 0, m));
    CHECK(c.run(now + 100, HmiCommandType::TechWebSet, false, 0, m));
    CHECK(!c.runtime_.tech.webAllowed && !c.runtime_.tech.webSession && c.runtime_.tech.pendingKind == 0 && c.runtime_.tech.resultState == TechResult::Revoked);
    CHECK(!c.run(now + 200, HmiCommandType::TechDecision, false, 1, m));
    CHECK(c.store_.saves == 0 && c.config_.kp == 5.0f);
    // hidden again: the Web is refused as at the start, and a persisted permission survives a reboot
    CHECK(!c.run(now + 300, HmiCommandType::TechPinVerify, true, 1234, m) && m == "WEB_HIDDEN");
    Ctl d; d.auth_ = c.auth_; d.techGate_.begin(now + 5000); CHECK(!d.techGate_.webAllowed());
    c.run(now + 400, HmiCommandType::TechWebSet, false, 1, m);
    Ctl e; e.auth_ = c.auth_; e.techGate_.begin(now + 5000); CHECK(e.techGate_.webAllowed());
    CHECK(!e.techGate_.webSessionActive(now + 5000));                      // a reboot never keeps a Web session
  }

  // 7. Safety conditions are checked AGAIN at "yes": a batch that started meanwhile blocks the apply (and at submit time too).
  { Ctl c; fresh(c, now); grantWeb(c, now);
    c.batchRunning_ = true;
    CHECK(stage(MayapTech::Kind::Config, {{AdvancedHistory::F_KP, 9.0}}, "req-batch", c.config_));
    CHECK(!c.run(now, HmiCommandType::TechRequestSubmit, true, 0, m) && m == "BATCH_LOCKED");
    c.batchRunning_ = false;
    CHECK(stage(MayapTech::Kind::Config, {{AdvancedHistory::F_KP, 9.0}}, "req-batch2", c.config_));
    CHECK(c.run(now, HmiCommandType::TechRequestSubmit, true, 0, m));
    c.batchRunning_ = true;                                                // batch starts while the HMI is deciding
    CHECK(!c.run(now + 1000, HmiCommandType::TechDecision, false, 1, m));
    CHECK(c.store_.saves == 0 && c.config_.kp == 5.0f && c.runtime_.tech.resultState == TechResult::Failed);
    c.batchRunning_ = false; c.resumePending_ = true;                      // pending recovery counts as a running batch
    CHECK(stage(MayapTech::Kind::Config, {{AdvancedHistory::F_KP, 9.0}}, "req-resume", c.config_));
    CHECK(!c.run(now + 2000, HmiCommandType::TechRequestSubmit, true, 0, m) && m == "BATCH_LOCKED");
    c.resumePending_ = false; c.autotune_.run = true; c.storageDegraded_ = false;
    CHECK(stage(MayapTech::Kind::Config, {{AdvancedHistory::F_KP, 9.0}}, "req-tune", c.config_));
    CHECK(c.run(now + 3000, HmiCommandType::TechRequestSubmit, true, 0, m));
    CHECK(!c.run(now + 3100, HmiCommandType::TechDecision, false, 1, m) && m == "AUTO TUNE DANG CHAY");
    c.autotune_.run = false;
    for (int which = 0; which < 4; ++which) {                              // storage / safety-journal faults block the apply too
      c.batchClearPending_ = which == 0; c.safetyJournalFaultLatched_ = which == 1; c.storageFaultLatched_ = which == 2; c.storageDegraded_ = which == 3;
      CHECK(stage(MayapTech::Kind::Config, {{AdvancedHistory::F_KP, 9.0}}, "req-fault", c.config_));
      CHECK(c.run(now + 4000, HmiCommandType::TechRequestSubmit, true, 0, m));
      CHECK(!c.run(now + 4100, HmiCommandType::TechDecision, false, 1, m) && m == "LOI AN TOAN/BO NHO");
    }
    CHECK(c.store_.saves == 0 && c.config_.kp == 5.0f);
  }

  // 8. Wi-Fi / Cloud lost: the Web session and its pending request end; local control and the HMI session are untouched.
  { Ctl c; fresh(c, now); grantWeb(c, now);
    CHECK(stage(MayapTech::Kind::Config, {{AdvancedHistory::F_KP, 9.0}}, "req-net", c.config_));
    CHECK(c.run(now, HmiCommandType::TechRequestSubmit, true, 0, m));
    c.runtime_.networkConnected = false; c.serviceTech(now + 500);
    CHECK(c.runtime_.tech.pendingKind == 0 && c.runtime_.tech.resultState == TechResult::Expired && !c.runtime_.tech.webSession);
    CHECK(c.runtime_.tech.hmiUnlocked);
    c.runtime_.networkConnected = true;
    CHECK(stage(MayapTech::Kind::Config, {{AdvancedHistory::F_KP, 9.0}}, "req-net2", c.config_));
    CHECK(!c.run(now + 1000, HmiCommandType::TechRequestSubmit, true, 0, m) && m == "WEB_LOCKED");   // must unlock again
  }

  // 9. A Web client can never reach an HMI-only command.
  { Ctl c; fresh(c, now); grantWeb(c, now);
    for (HmiCommandType t : {HmiCommandType::TechPinSet, HmiCommandType::TechWebSet, HmiCommandType::TechDecision, HmiCommandType::AdvancedRestore}) {
      CHECK(!c.run(now, t, true, 1, m));
    }
    CHECK(c.runtime_.tech.webAllowed && c.runtime_.tech.pinSet);
    CHECK(!c.run(now, HmiCommandType::TechRequestSubmit, false, 0, m) && m == "CHI WEB DUOC GUI");      // and the HMI cannot fake a request
    // the Web may only END its own session
    CHECK(c.run(now, HmiCommandType::TechLock, true, 0, m) && !c.runtime_.tech.webSession && c.runtime_.tech.hmiUnlocked);
  }

  // 10. Smart AutoTune request: parked, started only on HMI yes, never twice.
  { Ctl c; fresh(c, now); grantWeb(c, now);
    CHECK(stage(MayapTech::Kind::AutoTune, {}, "req-tune", c.config_));
    CHECK(c.run(now, HmiCommandType::TechRequestSubmit, true, 0, m) && m == "PENDING_HMI" && c.runtime_.tech.pendingKind == 2);
    CHECK(c.autoTuneStarts == 0);
    CHECK(c.run(now + 100, HmiCommandType::TechDecision, false, 1, m) && c.autoTuneStarts == 1);
    CHECK(!c.run(now + 200, HmiCommandType::TechDecision, false, 1, m) && c.autoTuneStarts == 1);
    // No on a tune request
    CHECK(stage(MayapTech::Kind::AutoTune, {}, "req-tune2", c.config_));
    CHECK(c.run(now + 300, HmiCommandType::TechRequestSubmit, true, 0, m));
    CHECK(c.run(now + 400, HmiCommandType::TechDecision, false, 0, m) && c.autoTuneStarts == 1);
    // Web session ended before "yes": not started
    CHECK(stage(MayapTech::Kind::AutoTune, {}, "req-tune3", c.config_));
    CHECK(c.run(now + 500, HmiCommandType::TechRequestSubmit, true, 0, m));
    c.run(now + 600, HmiCommandType::TechLock, true, 0, m);
    CHECK(!c.run(now + 700, HmiCommandType::TechDecision, false, 1, m) && c.autoTuneStarts == 1);
  }

  // 11. Nothing changed => nothing is written; invalid PID is refused before any record; the same values never fill the history.
  { Ctl c; fresh(c, now); grantWeb(c, now);
    const AdvancedHistory::Snapshot same = AdvancedHistory::fromConfig(c.config_);
    const char *msg = "";
    CHECK(c.applyAdvancedSnapshot(now, same, msg) && std::string(msg) == "KHONG DOI" && c.store_.saves == 0 && c.hist_.writes == 0);
    AdvancedHistory::Snapshot zero = same; zero.kp = zero.ki = zero.kd = 0;
    CHECK(!c.applyAdvancedSnapshot(now, zero, msg) && std::string(msg) == "PID KHONG HOP LE" && c.store_.saves == 0 && c.hist_.writes == 0);
    CHECK(!c.run(now, HmiCommandType::TechRequestSubmit, true, 0, m));      // empty staging
    CHECK(stage(MayapTech::Kind::Config, {{AdvancedHistory::F_KP, 5.0}}, "req-same", c.config_));
    CHECK(!c.run(now, HmiCommandType::TechRequestSubmit, true, 0, m) && m == "NO_CHANGE");
  }

  // 12. Save failure: config unchanged, a clear error, nothing half-applied.
  { Ctl c; fresh(c, now); grantWeb(c, now);
    c.store_.failSave = true;
    CHECK(stage(MayapTech::Kind::Config, {{AdvancedHistory::F_KP, 9.0}}, "req-fail", c.config_));
    CHECK(c.run(now, HmiCommandType::TechRequestSubmit, true, 0, m));
    CHECK(!c.run(now + 100, HmiCommandType::TechDecision, false, 1, m) && m == "LOI LUU CAU HINH");
    CHECK(c.config_.kp == 5.0f && c.commits == 0 && c.storageFaultLatched_ && c.runtime_.tech.resultState == TechResult::Failed);
  }

  // 13. Old records: newest 3 kept, restore applies one AND stores the replaced config (undo), only an unlocked HMI may restore.
  { Ctl c; fresh(c, now);
    const char *msg = "";
    for (int i = 1; i <= 4; ++i) {                                         // four applies -> only the last three old records remain
      AdvancedHistory::Snapshot s = AdvancedHistory::fromConfig(c.config_); s.kp = 10.0f + i;
      CHECK(c.applyAdvancedSnapshot(now, s, msg));
    }
    CHECK(c.history_.count() == 3 && c.historyCount_ == 3);
    MayapTech::Detail d; c.serviceTech(now); MayapTech::readDetail(d);
    CHECK(d.history[0].kp == 13.0f && d.history[1].kp == 12.0f && d.history[2].kp == 11.0f);   // newest .. oldest; kp 5 and 10 are gone
    const float current = c.config_.kp; CHECK(current == 14.0f);
    CHECK(c.run(now, HmiCommandType::AdvancedRestore, false, 2, m) && m == "DA KHOI PHUC");     // restore the OLDEST record (kp 11)
    CHECK(c.config_.kp == 11.0f);
    c.serviceTech(now); MayapTech::readDetail(d);
    CHECK(d.history[0].kp == 14.0f);                                       // the replaced config is the newest record -> undo works
    CHECK(c.run(now, HmiCommandType::AdvancedRestore, false, 0, m) && c.config_.kp == 14.0f);   // undo
    CHECK(c.config_.targetTemp == 37.5f && c.config_.emergencyTemp == 39.0f && c.config_.highTempAlarm == 38.2f);
    // locked HMI / missing record / running batch / remote caller: refused, nothing written
    const unsigned writes = c.hist_.writes, saves = c.store_.saves;
    c.techGate_.lockHmi();
    CHECK(!c.run(now, HmiCommandType::AdvancedRestore, false, 0, m) && m == "CAN NHAP MA KY THUAT");
    CHECK(c.run(now, HmiCommandType::TechPinVerify, false, 1234, m));
    CHECK(!c.run(now, HmiCommandType::AdvancedRestore, false, 7, m) && m == "KHONG CO BAN GHI");
    c.batchRunning_ = true;
    CHECK(!c.run(now, HmiCommandType::AdvancedRestore, false, 0, m) && m == "DANG AP - KHONG DOI PID");
    CHECK(!c.run(now, HmiCommandType::AdvancedRestore, true, 0, m));
    CHECK(c.hist_.writes == writes && c.store_.saves == saves);
    // HMI session expires after 120 s of silence: no restore without the PIN again
    c.batchRunning_ = false;
    CHECK(!c.techGate_.hmiUnlocked(now + 130000));
    CHECK(!c.run(now + 130000, HmiCommandType::AdvancedRestore, false, 0, m));
  }

  // 14. Power cut while an old record is written: the apply is refused (config untouched) and the other records stay valid.
  { Ctl c; fresh(c, now);
    const char *msg = "";
    for (int i = 1; i <= 3; ++i) { AdvancedHistory::Snapshot s = AdvancedHistory::fromConfig(c.config_); s.kp = 20.0f + i; CHECK(c.applyAdvancedSnapshot(now, s, msg)); }
    CHECK(c.history_.count() == 3);
    const float before = c.config_.kp; const unsigned saves = c.store_.saves;
    c.hist_.tearNext = 20;                                                 // power dies after 20 bytes of the new record
    AdvancedHistory::Snapshot s = AdvancedHistory::fromConfig(c.config_); s.kp = 40.0f;
    CHECK(!c.applyAdvancedSnapshot(now, s, msg) && std::string(msg) == "LOI LUU BAN GHI CU");
    CHECK(c.config_.kp == before && c.store_.saves == saves);
    Ctl d; d.hist_ = c.hist_; d.history_.begin();                          // "reboot": rescan the ring
    CHECK(d.history_.count() == 2);                                        // only the record being written was lost
    AdvancedHistory::Snapshot a, b;
    CHECK(d.history_.get(0, a) && d.history_.get(1, b) && a.kp == 22.0f && b.kp == 21.0f);
  }

  std::printf("tech controller glue (real MachineController code): PIN shared lock-out, Web hidden/shown/revoke, request yes/no/timeout/network loss, "
              "safety re-check, no-change/no-write, undo record, restore + undo, power cut in history write (%u checks) PASS\n", checks);
  return 0;
}
