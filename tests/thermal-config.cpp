// Real EEPROM load/migration/CRC/sanitize paths; only the I/O is byte-backed.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>
using std::isfinite;
int constrain(int v,int lo,int hi){return std::max(lo,std::min(hi,v));}
float clampFloat(float v,float lo,float hi){return std::max(lo,std::min(hi,v));}
#include "actual-config.inc"
#include "../MAYAP_INDUSTRIAL_v1_0_0/heater_burst_scheduler.h"
constexpr uint8_t HEATER_GROUP_COUNT=1;
struct ConfigStore {
  uint8_t bytes[1024];
  bool ready_=true,configCacheValid_=false,configCurrentIsA_=false;
  uint32_t configSequence_=0;
  PackedMachineConfigV1 configPayload_{};
  ConfigStore(){std::memset(bytes,0xff,sizeof(bytes));}
  template<class T> bool readRecord(uint16_t addr,T &r) const {
    assert(addr+sizeof(r)<=sizeof(bytes));std::memcpy(&r,bytes+addr,sizeof(r));return true;
  }
  template<class T> bool writeRecord(uint16_t addr,const T &r) {
    assert(addr+sizeof(r)<=sizeof(bytes));std::memcpy(bytes+addr,&r,sizeof(r));return true;
  }
#include "actual-config-load.inc"
};
static std::vector<bool> waveform(const MachineConfig &loaded) {
  // No legacy field may affect timing. This is the exact production initializer.
#include "actual-burst-member.inc"
  std::vector<bool> result;
  for(uint32_t t=0;t<60000;t+=5) result.push_back(heaterBurst_.update(t,30,true).groupA);
  assert(loaded.pidCycleSec>=1 && loaded.pidCycleSec<=60);
  return result;
}
template<class Record> void verifyLegacy(uint16_t schema,uint16_t cycle) {
  MachineConfig input;input.pidCycleSec=cycle;
  const PackedMachineConfigV1 packed=packConfig(input);
  Record r{};r.magic=CONFIG_MAGIC;r.schema=schema;r.size=sizeof(r);r.sequence=9;
  std::memcpy(&r.payload,&packed,sizeof(r.payload));
  r.crc=mcCrc32(reinterpret_cast<const uint8_t *>(&r),offsetof(Record,crc));
  ConfigStore store;std::memcpy(store.bytes+EEPROM_ADDR_CONFIG_A,&r,sizeof(r));
  MachineConfig loaded;assert(store.loadConfig(loaded));
  assert(loaded.pidCycleSec==cycle && loaded.targetTemp==input.targetTemp);
  assert(packConfig(loaded).pidCycleSec==cycle);
  assert(!loaded.adaptiveThermalBalanceEnabled);
  MachineConfig reference;assert(waveform(loaded)==waveform(reference));
  // Corrupt CRC must not admit a configuration.
  store.bytes[offsetof(Record,crc)]^=1;assert(!store.loadConfig(loaded));
}
int main(){
  static_assert(CONFIG_SCHEMA==13,"Append-only adaptive opt-in schema");
  static_assert(HEATER_BURST_QUANTUM_MS==300,"Commissioning candidate stays 300 ms");
  // Exercise actual append-only save/readback and reboot selection, both ways.
  ConfigStore flags;
  for(bool enabled : {true, false, true}) {
    MachineConfig input, readback;
    input.adaptiveThermalBalanceEnabled=enabled;
    assert(flags.saveConfig(input,readback));
    assert(readback.adaptiveThermalBalanceEnabled==enabled);
    ConfigStore reboot;
    std::memcpy(reboot.bytes,flags.bytes,sizeof(flags.bytes));
    assert(reboot.loadConfig(readback));
    assert(readback.adaptiveThermalBalanceEnabled==enabled);
  }
  // Sensor resolution choice shares the schema-13 adaptive byte without disturbing the adaptive flag.
  for(uint8_t mode:{0U,1U,2U}) for(bool enabled:{false,true}) {
    ConfigStore store;MachineConfig input,readback;
    input.sensorResolutionMode=mode;input.adaptiveThermalBalanceEnabled=enabled;
    assert(store.saveConfig(input,readback));
    assert(readback.sensorResolutionMode==mode && readback.adaptiveThermalBalanceEnabled==enabled);
  }
  // Smart Thermal switch (bit 3) is independent of the adaptive flag and the resolution choice.
  for(bool smart:{false,true}) for(bool adaptive:{false,true}) for(uint8_t mode:{0U,2U}) {
    ConfigStore store;MachineConfig input,readback;
    input.smartThermalEnabled=smart;input.adaptiveThermalBalanceEnabled=adaptive;input.sensorResolutionMode=mode;
    assert(store.saveConfig(input,readback));
    assert(readback.smartThermalEnabled==smart && readback.adaptiveThermalBalanceEnabled==adaptive && readback.sensorResolutionMode==mode);
  }
  { // a pre-existing record (byte 0/1) means "automatic"
    const MachineConfig d;assert(d.sensorResolutionMode==0U);
    PackedMachineConfigV1 p=packConfig(d);p.adaptiveThermalBalanceEnabled=1U;
    const MachineConfig u=unpackConfig(p);assert(u.adaptiveThermalBalanceEnabled&&u.sensorResolutionMode==0U);
    assert(!u.smartThermalEnabled);                                                          // old records: Smart Thermal OFF
    p.adaptiveThermalBalanceEnabled=7U;assert(unpackConfig(p).sensorResolutionMode==0U);   // invalid mode 3 -> automatic
  }
  for(uint16_t cycle:{1U,10U,60U}) {
    verifyLegacy<ConfigRecordV1>(13,cycle);
    verifyLegacy<ConfigRecordLegacyV12>(12,cycle);
    verifyLegacy<ConfigRecordLegacyV3>(3,cycle);
    verifyLegacy<ConfigRecordLegacyV4>(4,cycle);
    verifyLegacy<ConfigRecordLegacyV5>(5,cycle);
    verifyLegacy<ConfigRecordLegacyV6>(6,cycle);
    verifyLegacy<ConfigRecordLegacyV7>(7,cycle);
    verifyLegacy<ConfigRecordLegacyV8>(8,cycle);
    verifyLegacy<ConfigRecordLegacyV9>(9,cycle);
    verifyLegacy<ConfigRecordLegacyV10>(10,cycle);
    verifyLegacy<ConfigRecordLegacyV11>(11,cycle);
  }
  // Stale pre-fix config: SV changed but all thermal thresholds remained the
  // exact 37.5 C defaults. Sanitization must re-anchor the entire envelope.
  MachineConfig low; low.targetTemp=30; sanitizeMachineConfig(low);
  assert(std::fabs(low.lowTempAlarm-29.0f)<0.001f);
  assert(std::fabs(low.highTempAlarm-30.7f)<0.001f);
  assert(std::fabs(low.emergencyTemp-31.5f)<0.001f);
  assert(std::fabs(low.ventOnTemp-30.5f)<0.001f);
  assert(std::fabs(low.ventOffTemp-30.1f)<0.001f);

  // A customized envelope must never be silently rewritten by that repair.
  MachineConfig custom;
  custom.targetTemp=30; custom.lowTempAlarm=28.5f; custom.highTempAlarm=32.0f;
  custom.emergencyTemp=33.0f; custom.ventOnTemp=31.0f; custom.ventOffTemp=30.5f;
  sanitizeMachineConfig(custom);
  assert(std::fabs(custom.lowTempAlarm-28.5f)<0.001f);
  assert(std::fabs(custom.highTempAlarm-32.0f)<0.001f);
  assert(std::fabs(custom.emergencyTemp-33.0f)<0.001f);
  assert(std::fabs(custom.ventOnTemp-31.0f)<0.001f);
  assert(std::fabs(custom.ventOffTemp-30.5f)<0.001f);

  MachineConfig invalid; invalid.targetTemp=30; invalid.highTempAlarm=29;
  invalid.emergencyTemp=29; sanitizeMachineConfig(invalid);
  assert(invalid.highTempAlarm>=invalid.targetTemp+HIGH_ALARM_GAP_C);
  assert(invalid.emergencyTemp>=invalid.highTempAlarm+EMERGENCY_ABOVE_HIGH_C);
  std::puts("Actual EEPROM schemas 3..13: legacy cycle 1/10/60 loads, CRC rejected, constant timing; stale default thermal envelope re-anchors to SP while custom thresholds are preserved PASS");
}
