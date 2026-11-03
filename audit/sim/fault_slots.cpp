// AUDIT ARTIFACT ONLY - khong phai production code.
// Mo hinh hoa NGUYEN VAN co che cap phat slot cua FaultManager
// (machine_control.h:706-745) + thu tu goi faults_.set() thuc te trong
// MachineController::update() de kiem chung finding F-05 (tran bang loi).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const int MAX_FAULTS = 32;   // machine_control.h:708

struct Desc { bool latching; };
static Desc descOf(const std::string& c){
  // Chi 2 thuoc tinh can cho mo hinh nay: latching cua ma dao = true,
  // latching cua StorageRetryTrend = false.
  if (c.rfind("Turn",0)==0 && c!="TurnMechanicalCheckRequired") return {true};
  return {false};
}

struct State { std::string code; bool condition=false, active=false, acked=false; };
static State states[MAX_FAULTS];
static int count_ = 0;
static std::vector<std::string> log;

static State& slot(const std::string& code){
  for (int i=0;i<count_;++i) if (states[i].code==code) return states[i];
  if (count_ < MAX_FAULTS) { states[count_].code = code; return states[count_++]; }
  states[MAX_FAULTS-1].code = code;              // nhanh tran (fail-safe theo comment)
  return states[MAX_FAULTS-1];
}

static void clearState(State& s){ log.push_back("CLEARED:"+s.code); s.active=false; s.acked=false; }

static void setFault(const std::string& code, bool condition){
  State& s = slot(code);
  if (condition){
    s.condition = true;
    if (!s.active){ s.active = true; s.acked=false; log.push_back("RAISED:"+code); }
    return;
  }
  s.condition = false;
  if (!s.active) return;
  if (descOf(code).latching && !s.acked) return;
  clearState(s);
}

static bool activeCode(const std::string& code){
  for (int i=0;i<count_;++i) if (states[i].code==code) return states[i].active;
  return false;
}

// Thu tu THUC TE cac ma duoc faults_.set() moi chu ky (doc tu update(),
// processResume(), updateAlarms(), syncOutputFaults(), serviceHealthMonitor()).
static const char* PER_CYCLE[] = {
  "RtcFailure","ResumeConfirmationPending","ResumeRtcWaitTooLong",
  "SensorLost","SensorInvalid","SensorSuspect","LowTemperature","HighTemperature",
  "EmergencyTemperature","HumidityLow","HumidityHigh","TemperatureRateExceeded",
  "TemperatureUnstable","HeaterNotHeating","TurnMechanicalCheckRequired","BatchOverdue",
  "HeaterSwitchOffDuringBatch","AutoModeOffDuringBatch","AutoTurningDisabledDuringBatch",
  "BatchLogUnavailable","ResumeRequiresAuto","StorageUnavailable","StorageDegraded",
  "AbnormalReset","BatchStateClearPending","SafetyJournalUnavailable",
  "OutputConflict","RelayRateExceeded"
};
// serviceHealthMonitor() - chay sau HEALTH_BASELINE_CAPTURE_DELAY_MS (60 s),
// lap lai moi HEALTH_CHECK_INTERVAL_MS (30 s). Ca 4 deu goi vo dieu kien.
static const char* HEALTH[] = {
  "HeapCritical","HeapLow","StorageRetryTrend"
};

static void normalCycle(bool withHealth){
  for (auto c : PER_CYCLE) setFault(c,false);
  if (withHealth) for (auto c : HEALTH) setFault(c,false);
}

int fails=0;
static void check(const char* n,bool ok){ printf("%-62s %s\n",n,ok?"PASS":"FAIL"); if(!ok)++fails; }

int main(){
  // Giai doan 1: 60 giay dau (chua co health monitor)
  for(int i=0;i<5;++i) normalCycle(false);
  check("Truoc health-monitor: da dung 28/32 slot", count_==28);

  // Giai doan 2: health monitor bat dau -> bang day
  normalCycle(true);
  check("Sau health-monitor dau tien: bang loi DAY (32/32)", count_==32);
  check("Slot cuoi cung (index 31) = StorageRetryTrend",
        states[MAX_FAULTS-1].code=="StorageRetryTrend");

  // Giai doan 3: xay ra loi dao THAT (latchTurnFault -> faults_.set(TurnTimeout,true))
  log.clear();
  setFault("TurnTimeout", true);
  check("E202 TurnTimeout duoc bao len (RAISED)",
        log.size()==1 && log[0]=="RAISED:TurnTimeout");
  check("E202 dang active ngay sau khi latch", activeCode("TurnTimeout"));
  check("E202 da GHI DE slot cua StorageRetryTrend",
        states[MAX_FAULTS-1].code=="TurnTimeout");

  // Giai doan 4: chu ky dieu khien binh thuong ke tiep KHONG co health gate
  log.clear();
  normalCycle(false);
  check("Giua 2 nhip health: E202 van con active", activeCode("TurnTimeout"));

  // Giai doan 5: nhip health-monitor ke tiep (toi da 30 s sau)
  log.clear();
  normalCycle(true);
  bool wiped = !activeCode("TurnTimeout");
  check("SAU 1 NHIP HEALTH (<=30s): E202 BI XOA khoi FaultManager", wiped);
  bool bogus=false; for(auto&e:log) if(e=="CLEARED:StorageRetryTrend") bogus=true;
  check("Su kien ghi vao nhat ky la 'CLEARED:StorageRetryTrend' (SAI ma)", bogus);
  check("Slot 31 quay ve StorageRetryTrend",
        states[MAX_FAULTS-1].code=="StorageRetryTrend");

  // Giai doan 6: lap lai voi TurnLimitConflict va TurnLimitStuck - cung ket qua
  for (const char* c : {"TurnLimitConflict","TurnLimitStuck","TurnCommandConflict"}) {
    setFault(c,true);
    bool raised = activeCode(c);
    normalCycle(true);
    check((std::string("Ma dao ")+c+" cung bi xoa sau 1 nhip health").c_str(),
          raised && !activeCode(c));
  }

  printf("\nTONG: %s (%d loi)\n", fails?"CO LOI":"TAT CA PASS", fails);
  return fails?1:0;
}
