#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <cstddef>
static uint32_t clockMs = 0U;
uint32_t millis() { return clockMs; }
uint32_t micros() { return clockMs * 1000U; }
uint32_t elapsedMs(uint32_t now, uint32_t then) { return now - then; }
bool timeReached(uint32_t now, uint32_t when) { return static_cast<int32_t>(now - when) >= 0; }
void mayapSerialPrintf(bool, const char *, ...) {}
constexpr int LOW=0, HIGH=1, INPUT_PULLUP=2, OUTPUT_OPEN_DRAIN=3, OUTPUT=4;
constexpr uint8_t PIN_I2C_SDA=1, PIN_I2C_SCL=2, PIN_RS485_RX=3, PIN_RS485_TX=4, PIN_RS485_DE_RE=5, SHT_UART_PORT=1;
constexpr uint8_t LCD_I2C_ADDRESS=0x3F, RTC_I2C_ADDRESS=0x68, EEPROM_I2C_ADDRESS=0x50;
constexpr uint32_t I2C_CLOCK_HZ=100000, I2C_TIMEOUT_MS=25, SERIAL_8N1=1;
static bool sdaStuck=false, sclStuck=false, lockAvailable=true, locked=false;
static int levels[8]={HIGH,HIGH,HIGH,HIGH,HIGH,LOW,HIGH,HIGH};
static unsigned pulses=0, releaseAfter=0, delaysUs=0;
void pinMode(int, int) {}
int digitalRead(int pin) {
  if (pin == PIN_I2C_SDA && sdaStuck) return LOW;
  if (pin == PIN_I2C_SCL && sclStuck) return LOW;
  return levels[pin];
}
void digitalWrite(int pin, int value) {
  if (pin == PIN_I2C_SCL && value == HIGH && levels[pin] == LOW) {
    ++pulses;
    if (releaseAfter && pulses >= releaseAfter) sdaStuck=false;
  }
  levels[pin]=value;
}
void delayMicroseconds(unsigned duration) { delaysUs += duration; }
bool mayapI2cLock(uint32_t timeout) { assert(timeout == 0); if (!lockAvailable) return false; assert(!locked); return locked=true; }
void mayapI2cUnlock() { assert(locked); locked=false; }
struct FakeWire {
  unsigned ends=0, begins=0, probes=0;
  bool present=true;
  void end() { assert(locked); ++ends; }
  bool begin(int sda, int scl, uint32_t hz) { assert(locked && sda==PIN_I2C_SDA && scl==PIN_I2C_SCL && hz==I2C_CLOCK_HZ); ++begins; return true; }
  void setTimeOut(uint32_t ms) { assert(ms==I2C_TIMEOUT_MS); }
  void beginTransmission(uint8_t) { assert(locked); ++probes; }
  uint8_t endTransmission(bool stop) { assert(stop); return present ? 0 : 2; }
} Wire;
#include "actual-i2c.inc"
static unsigned uartBegins=0, uartEnds=0, uartWrites=0;
static int replyMode=0;
static uint32_t replyNotBefore=0;   // sensor firmware warm-up: no reply before this time (boot simulation)
static uint16_t replyTemperature=375, replyHumidity=600;
static std::deque<uint8_t> uartRx;
static uint16_t crc16(const uint8_t *data, size_t length) {
  uint16_t crc=0xFFFF;
  for (size_t i=0; i<length; ++i) {
    crc ^= data[i];
    for (unsigned bit=0; bit<8; ++bit) crc = crc & 1 ? (crc>>1)^0xA001 : crc>>1;
  }
  return crc;
}
class HardwareSerial {
 public:
  explicit HardwareSerial(uint8_t port) { assert(port==SHT_UART_PORT); }
  void begin(uint32_t baud, uint32_t format, int rx, int tx) {
    assert(baud==9600 && format==SERIAL_8N1 && rx==PIN_RS485_RX && tx==PIN_RS485_TX);
    assert(levels[PIN_RS485_DE_RE]==LOW); ++uartBegins;
  }
  void end() { assert(levels[PIN_RS485_DE_RE]==LOW); ++uartEnds; uartRx.clear(); }
  int available() { return static_cast<int>(uartRx.size()); }
  int read() { if (uartRx.empty()) return -1; auto b=uartRx.front(); uartRx.pop_front(); return b; }
  int availableForWrite() { return 128; }
  size_t write(const uint8_t *data, size_t length) {
    assert(levels[PIN_RS485_DE_RE]==HIGH && length==8 && crc16(data,6)==static_cast<uint16_t>(data[6]|data[7]<<8));
    ++uartWrites;
    if (replyMode && clockMs >= replyNotBefore) {
      uint8_t frame[]={1,3,4,0x02,0x58,0x01,0x77,0,0}; // 60% RH, 37.5 C
      frame[3]=replyHumidity>>8; frame[4]=replyHumidity & 255;
      frame[5]=replyTemperature>>8; frame[6]=replyTemperature & 255;
      const uint16_t crc=crc16(frame,7);
      frame[7]=static_cast<uint8_t>(crc); frame[8]=static_cast<uint8_t>(crc>>8);
      if (replyMode==2) frame[8]^=1;
      for (auto byte : frame) uartRx.push_back(byte);
    }
    return length;
  }
};
using std::isfinite;
#define MAYAP_SENSOR_PROFILE 0
#include "../MAYAP_INDUSTRIAL_v1_0_0/sensor_format.h"
#include "actual-uart.inc"
#include "../MAYAP_INDUSTRIAL_v1_0_0/boot_policy.h"
#include <cstdlib>
#include <algorithm>
static void run(SHT485Industrial &sensor, uint32_t duration) {
  for (uint32_t i=0; i<duration; ++i) { ++clockMs; sensor.update(); }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// COLD-BOOT SIMULATION (host, virtual time). Real code: SHT485Industrial (sliced from machine_control.h), MayapBoot::Sequencer /
// Stability / HomeGate (boot_policy.h). Modelled, NOT the firmware: the control task start instants and the controller's "sensor
// usable" rule (three consecutive good samples after the format lock, config.h SENSOR_RECOVERY_GOOD_SAMPLES) and the network times.
// Proves the policy and the sensor cadence, not the ESP32 timing: hardware boot times stay NOT TESTED until measured on the board.
struct BootResult {
  uint32_t lockAt=0, usableAt=0, readyAt=0, homeAt=0, diagAt=0, localSettleAt=0, wifiTaskAt=0, wifiUpAt=0, mqttUpAt=0;
  bool homeBeforeUsable=false, diagShown=false, ready=false;
  MayapBoot::BlockReason diagReason=MayapBoot::BlockReason::None;
};
static BootResult simulateBoot(uint32_t seed, uint32_t sensorWarmupMs, int mode, uint32_t sensorStartsAt, bool pressHomeAtDiag, uint32_t horizonMs) {
  std::srand(seed);
  BootResult r;
  clockMs=0; replyMode=mode; replyNotBefore=sensorStartsAt+sensorWarmupMs; uartRx.clear();
  const uint32_t t0=clockMs;
  const uint32_t controlStart=600U+static_cast<uint32_t>(std::rand()%120);      // ControlSafety stage + task creation jitter
  const uint32_t wifiAssoc=1500U+static_cast<uint32_t>(std::rand()%4500);       // modelled association time after the radio task starts
  const uint32_t ntpMs=800U+static_cast<uint32_t>(std::rand()%2500), tlsMs=2200U+static_cast<uint32_t>(std::rand()%900);
  SHT485Industrial sensor;
  MayapBoot::Sequencer seq; MayapBoot::Stability tasks; MayapBoot::HomeGate gate;
  seq.begin(0U, 0U); gate.begin(0U);
  bool sensorBegun=false; uint32_t goodStreak=0, usableSince=0; bool usable=false;
  bool released=false, readyShown=false; uint32_t readyAt=0; bool homeRequested=false;
  for (clockMs=1; clockMs<=horizonMs; ++clockMs) {
    const uint32_t now=clockMs;
    // staged startup (150 ms dwell for the local stages, exactly the .ino rule)
    if (seq.stage()<=MayapBoot::Stage::ControlSafety) { if (seq.age(now)>=150U) seq.advance(now); }
    if (!sensorBegun && seq.stage()>=MayapBoot::Stage::SensorMachine) { sensor.begin(); sensorBegun=true; }
    const bool controlRunning = now>=controlStart && seq.stage()>=MayapBoot::Stage::ControlSafety;
    if (controlRunning) {
      sensor.update();
      if (sensor.takeNewData()) {
        if (sensor.dataValid()) { if (goodStreak<255U) ++goodStreak; } else goodStreak=0U;
      }
      const bool transportValid = sensor.dataValid();
      if (!transportValid) goodStreak=0U;
      const bool nowUsable = transportValid && goodStreak>=3U;
      if (nowUsable && !usable) { usableSince=now; if (!r.usableAt) r.usableAt=now-t0; }
      usable=nowUsable;
      if (sensor.formatLocked() && !r.lockAt) r.lockAt=now-t0;
    }
    tasks.update(now, controlRunning);
    // network release + service stages (real Sequencer)
    switch (seq.stage()) {
      case MayapBoot::Stage::LocalSettle: if (seq.releaseNetwork(now,tasks)) { r.localSettleAt=now-t0; seq.advance(now); r.wifiTaskAt=now-t0; } break;
      case MayapBoot::Stage::Wifi: if (seq.wifiDone(now,true,false)) seq.advance(now); break;
      case MayapBoot::Stage::Mqtt: if (seq.mqttDone(now,true,false)) seq.advance(now); break;
      case MayapBoot::Stage::Cloud: case MayapBoot::Stage::Ota: if (seq.age(now)>=seq.serviceGap()) seq.advance(now); break;
      default: break;
    }
    if (r.wifiTaskAt && !r.wifiUpAt && now-t0>=r.wifiTaskAt+wifiAssoc) r.wifiUpAt=now-t0;
    if (r.wifiUpAt && !r.mqttUpAt && now-t0>=r.wifiUpAt+3000U/*STA_STABLE_MS*/+ntpMs+tlsMs) r.mqttUpAt=now-t0;
    MayapBoot::LocalInputs in;
    in.tasksHealthy=tasks.held(now,1U); in.displayHealthy=true; in.storageBlocked=false; in.tripLatched=false;
    in.sensorUsable=usable; in.temperatureFinite=usable; in.sensorBootReason=sensor.bootReason();
    const auto phase=gate.update(now,in);
    if (phase==MayapBoot::HomeGate::Phase::Diagnostic) {
      if (!r.diagShown) { r.diagShown=true; r.diagAt=now-t0; r.diagReason=gate.reason(); }
      if (pressHomeAtDiag && now-t0>=r.diagAt+500U) homeRequested=true;
      if (homeRequested && !readyShown) { readyShown=true; readyAt=now-MayapBoot::READY_DISPLAY_MS; }
    }
    if (!readyShown && phase==MayapBoot::HomeGate::Phase::Ready) { readyShown=true; readyAt=now; r.ready=true; r.readyAt=now-t0; }
    if (readyShown && !released && now-readyAt>=MayapBoot::READY_DISPLAY_MS) {
      released=true; r.homeAt=now-t0;
      // The invariant: Home is never shown for a sensor that was not usable (unless the operator overrode the diagnostic screen).
      if (!homeRequested && !(usable && now-usableSince+1U>=MayapBoot::READY_HOLD_MS)) r.homeBeforeUsable=true;
    }
  }
  return r;
}
static void bootSimulation() {
  // 1) 30 cold boots, healthy sensor, random start-up jitter (control task start, sensor firmware warm-up 0..1.5 s, network times).
  uint32_t sumHome=0, maxHome=0, maxLock=0, maxUsable=0, maxMqtt=0, maxWifi=0, minHome=0xFFFFFFFFU;
  for (uint32_t i=0;i<30;++i) {
    const BootResult r=simulateBoot(1000U+i, static_cast<uint32_t>((i*53)%1500), 1, 0, false, 40000);
    assert(r.ready && r.homeAt>0 && !r.homeBeforeUsable && !r.diagShown);
    assert(r.homeAt<=30000U);   // requirement; the margin is large (see the printed maxima)
    assert(r.lockAt>0 && r.usableAt>r.lockAt-1U && r.homeAt>=r.usableAt);
    assert(r.wifiTaskAt>0 && r.wifiUpAt>r.wifiTaskAt && r.mqttUpAt>r.wifiUpAt);
    sumHome+=r.homeAt; maxHome=std::max(maxHome,r.homeAt); minHome=std::min(minHome,r.homeAt);
    maxLock=std::max(maxLock,r.lockAt); maxUsable=std::max(maxUsable,r.usableAt); maxMqtt=std::max(maxMqtt,r.mqttUpAt); maxWifi=std::max(maxWifi,r.wifiUpAt);
  }
  std::printf("Boot simulation (30 cold boots, healthy sensor): sensor lock <=%lu ms, usable <=%lu ms, Home %lu..%lu ms (mean %lu), Wi-Fi up <=%lu ms, MQTT up <=%lu ms (modelled network)\n",
    static_cast<unsigned long>(maxLock),static_cast<unsigned long>(maxUsable),static_cast<unsigned long>(minHome),static_cast<unsigned long>(maxHome),
    static_cast<unsigned long>(sumHome/30U),static_cast<unsigned long>(maxWifi),static_cast<unsigned long>(maxMqtt));
  std::fflush(stdout);
  // 2) slow sensor (firmware warm-up 12 s / 20 s): Home only after it is usable, still inside 30 s.
  for (uint32_t warm : {12000U, 20000U}) {
    const BootResult r=simulateBoot(7, warm, 1, 0, false, 40000);
    assert(r.ready && !r.diagShown && !r.homeBeforeUsable && r.usableAt>=warm && r.homeAt<=30000U);
  }
  // 3) sensor later than the deadline (40 s): diagnostic at 30 s with "no reply" (E101), Home only after it recovers, never a false READY.
  { const BootResult r=simulateBoot(8, 40000, 1, 0, false, 60000);
    assert(r.diagShown && r.diagAt>=30000U && r.diagAt<=30100U && r.diagReason==MayapBoot::BlockReason::SensorNoResponse);
    assert(MayapBoot::blockCode(r.diagReason)==101U);
    assert(r.ready && r.readyAt>=40000U && !r.homeBeforeUsable); }
  // 4) sensor absent: diagnostic at 30 s, never ready; the operator may continue to Home, which is NOT reported as ready.
  { const BootResult r=simulateBoot(9, 0, 0, 0, false, 90000);
    assert(r.diagShown && r.diagReason==MayapBoot::BlockReason::SensorNoResponse && !r.ready && r.homeAt==0);
    const BootResult o=simulateBoot(9, 0, 0, 0, true, 90000);
    assert(o.diagShown && !o.ready && o.homeAt>=30000U && o.homeAt<=31500U); }
  // 5) frames with CRC errors only: the sensor answers but never locks -> format diagnosis (E102), not "no reply".
  { const BootResult r=simulateBoot(10, 0, 2, 0, false, 40000);
    assert(r.diagShown && !r.ready && (r.diagReason==MayapBoot::BlockReason::SensorNoResponse || r.diagReason==MayapBoot::BlockReason::SensorFormat)); }
  // 6) every local safety condition also gates Home (pure policy, one fault at a time).
  { using namespace MayapBoot;
    LocalInputs ok; ok.tasksHealthy=ok.displayHealthy=ok.sensorUsable=ok.temperatureFinite=true; ok.sensorBootReason=0;
    assert(blockReason(ok)==BlockReason::None);
    LocalInputs x=ok; x.storageBlocked=true; assert(blockReason(x)==BlockReason::Storage && blockCode(blockReason(x))==301U);
    x=ok; x.tripLatched=true; assert(blockReason(x)==BlockReason::SafetyTrip);
    x=ok; x.tasksHealthy=false; assert(blockReason(x)==BlockReason::Tasks);
    x=ok; x.displayHealthy=false; assert(blockReason(x)==BlockReason::Display);
    x=ok; x.sensorUsable=false; x.sensorBootReason=2U; assert(blockReason(x)==BlockReason::SensorFormat && blockCode(blockReason(x))==102U);
    x=ok; x.sensorUsable=false; x.sensorBootReason=3U; assert(blockReason(x)==BlockReason::SensorUnstable);
    x=ok; x.temperatureFinite=false; assert(blockReason(x)!=BlockReason::None);
    // Ready is held 1.5 s: a flickering sensor never opens the gate; a later sensor loss does not close an open one (alarm path).
    HomeGate g; g.begin(0U); LocalInputs bad=ok; bad.sensorUsable=false; bad.sensorBootReason=3U;
    for (uint32_t t=0;t<6000U;t+=100U) { assert(g.update(t, ((t/700U)%2U)?ok:bad)!=HomeGate::Phase::Ready || false); }
    g.begin(0U); assert(g.update(100U,ok)==HomeGate::Phase::Waiting); assert(g.update(100U+READY_HOLD_MS,ok)==HomeGate::Phase::Ready);
    assert(g.update(200000U,bad)==HomeGate::Phase::Ready);
    // deadline + automatic recovery from the diagnostic phase
    HomeGate d; d.begin(1000U); assert(d.update(30999U,bad)==HomeGate::Phase::Waiting); assert(d.update(31000U,bad)==HomeGate::Phase::Diagnostic);
    assert(d.update(40000U,ok)==HomeGate::Phase::Diagnostic); assert(d.update(40000U+READY_HOLD_MS,ok)==HomeGate::Phase::Ready); }
}
int main() {
  clockMs=1000;
  lockAvailable=false; sdaStuck=true;
  mayapI2cSupervisorUpdate(clockMs);
  assert(Wire.ends==0);
  lockAvailable=true; sdaStuck=false; clockMs=2000;
  for (unsigned i=0; i<3; ++i) mayapI2cReport(LCD_I2C_ADDRESS,false);
  mayapI2cSupervisorUpdate(clockMs);
  assert(Wire.ends==0); // One missing device is not a shared bus hang.
  for (unsigned i=0; i<3; ++i) mayapI2cReport(RTC_I2C_ADDRESS,false);
  clockMs=3000; mayapI2cSupervisorUpdate(clockMs);
  assert(Wire.ends==1 && Wire.begins==1 && Wire.probes==3 && !locked);
  sdaStuck=true; releaseAfter=3; pulses=0;
  clockMs=4000; mayapI2cSupervisorUpdate(clockMs); assert(Wire.ends==1);
  clockMs=33000; mayapI2cSupervisorUpdate(clockMs);
  assert(Wire.ends==2 && pulses==3 && !sdaStuck && mayapI2cRecoveryEpoch()==2);
  sdaStuck=true; releaseAfter=0; pulses=0; delaysUs=0;
  clockMs=63000; mayapI2cSupervisorUpdate(clockMs);
  assert(Wire.ends==3 && pulses==9 && delaysUs<=105 && !locked);
  sclStuck=true; pulses=0; clockMs=93000; mayapI2cSupervisorUpdate(clockMs);
  assert(Wire.ends==4 && pulses<=1 && !locked);
  sdaStuck=sclStuck=false;
  clockMs=0;
  SHT485Industrial sensor;
  sensor.begin();
  for (unsigned i=0; i<25000 && !uartEnds; ++i) run(sensor,1);
  assert(uartEnds==1 && uartBegins==2 && !sensor.online() && !sensor.dataValid());
  const uint32_t recoveredAt=clockMs;
  run(sensor,29999); assert(uartEnds==1); // UART reinit cooldown.
  while (uartEnds<3 && clockMs<130000) run(sensor,1);
  assert(uartEnds==3);
  const unsigned beforeWrites=uartWrites;
  run(sensor,29999); assert(uartWrites==beforeWrites); // Isolated but still scheduled.
  replyMode=2; run(sensor,3000);
  assert(sensor.crcErrors()>0 && !sensor.dataValid());
  replyMode=1; run(sensor,15000);
  assert(sensor.online() && sensor.dataValid() && sensor.goodFrames()>=3);
  assert(std::fabs(sensor.temperatureC()-37.5f)<0.01f);
  replyMode=0; run(sensor,7000);
  assert(!sensor.online() && !sensor.dataValid());
  assert(clockMs-recoveredAt>30000 && levels[PIN_RS485_DE_RE]==LOW);
  // An absent sensor is polled fast only inside the 15 s cold-start window, then at the normal 2 s cadence (bus load stays bounded),
  // and the boot diagnosis says "no reply", not "unstable".
  replyMode=0;
  { SHT485Industrial absent; absent.begin(); const unsigned w0=uartWrites; run(absent,15000);
    const unsigned fast=uartWrites-w0; assert(fast>=8 && fast<=30 && absent.bootReason()==1);
    const unsigned w1=uartWrites; run(absent,10000); assert(uartWrites-w1<=10); }   // 2 attempts per cycle at the normal 2 s cadence
  replyMode=1; replyTemperature=3751;
  SHT485Industrial precise;
  precise.begin();
  run(precise,2500);
  assert(precise.online() && !precise.dataValid() && precise.bootReason()==2); // boot heater gate before the sixth consistent sample
  run(precise,2500);
  // Same six-sample verification, 600 ms cold-start cadence: locked in ~4 s instead of ~13 s.
  assert(precise.dataValid() && precise.formatLocked() && precise.bootReason()==0 && precise.goodFrames()<=9);
  assert(precise.sensorProfile()==SensorProfile::X100RhX10);
  assert(std::fabs(precise.rawTemperatureC()-37.51f)<0.00001f);
  assert(std::fabs(precise.temperatureC()-37.51f)<0.00001f);
  replyTemperature=3752; run(precise,6000);
  assert(precise.temperatureC()>37.51f && precise.temperatureC()<37.52f);
  replyTemperature=375; run(precise,2500);
  assert(!precise.dataValid() && std::isnan(precise.rawTemperatureC()));
  assert(precise.sensorProfile()==SensorProfile::X100RhX10);
  replyTemperature=3751; run(precise,4000);
  assert(precise.dataValid());
  const uint32_t goodBeforeCrc=precise.goodFrames();
  replyMode=2; run(precise,2200);
  assert(precise.dataValid() && precise.formatLocked()); // CRC rejects the packet, retaining bounded last-good data.
  assert(precise.goodFrames()==goodBeforeCrc);
  run(precise,5000);
  assert(!precise.dataValid() && precise.formatLocked()); // persistent errors still expire data and cut heat.
  replyMode=1;run(precise,6000);assert(precise.dataValid());
  replyMode=1; replyTemperature=30902; replyHumidity=39321;
  SHT485Industrial native;
  native.begin(); run(native,13000);
  assert(native.dataValid() && native.sensorProfile()==SensorProfile::Sht30Native);
  assert(std::fabs(native.rawTemperatureC()-(-45.0f+175.0f*30902/65535.0f))<0.00001f);
  assert(std::fabs(native.humidityRH()-60.0f)<0.001f);
  replyTemperature=3751; replyHumidity=600;
  SHT485Industrial hot;
  hot.begin();run(hot,13000);assert(hot.dataValid());
  replyTemperature=6000;run(hot,2500);
  assert(hot.dataValid() && hot.rawTemperatureC()>59.99f); // Raw EmergencyHigh path remains immediate.
  bootSimulation();
  std::puts("Actual I2C/UART: mutex, correlated errors, cooldown, <=9 clocks, stuck lines, CRC, reinit, isolate, reconnect and stale samples PASS");
}
