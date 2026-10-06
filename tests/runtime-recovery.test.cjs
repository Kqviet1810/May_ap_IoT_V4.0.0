const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const read = (name) => fs.readFileSync(path.resolve(__dirname, '..', name), 'utf8').replace(/\r\n/g, '\n');
const dir = 'MAYAP_INDUSTRIAL_v1_0_0/';
function body(source, signature) {
  const start = source.indexOf(signature);
  assert.notEqual(start, -1, signature);
  const open = signature.endsWith('{') ? start + signature.length - 1 : source.indexOf('{', start);
  let depth = 0;
  for (let i = open; i < source.length; ++i) {
    if (source[i] === '{') ++depth;
    if (source[i] === '}' && --depth === 0) return source.slice(open + 1, i);
  }
  throw new Error(`Unclosed ${signature}`);
}
test('runtime recovery preserves Adaptive Boot, local safety and schemas', () => {
  const manifest = JSON.parse(read('tests/runtime-preservation.json'));
  for (const entry of manifest.entries) {
    // Web transport and transaction functions are covered by focused V2 tests.
    if (entry.file === 'app.js') continue;
    let source = read(entry.file);
    if (entry.signature) source = body(source, entry.signature);
    // Only explicitly reviewed user-facing copy can differ inside protected Web functions.
    for (const [current, baseline] of entry.copyReplacements || []) source = source.replace(current, baseline);
    // The only additions allowed inside these protected functions are health instrumentation.
    if (entry.filter === 'webBeat') source = source.replace(/^\s*mayapServiceBeat\(MayapRecovery::Service::Ota\);\n/gm, '');
    if (entry.filter === 'supervisor') {
      source = source.replace(/    MayapRecovery::Service failedService[\s\S]*?(?=    const esp_err_t result = esp_task_wdt_reset\(\);)/, '');
    }
    if (entry.filter === 'cloudFaultEvents') source = source.replace(/^#ifdef MAYAP_CLOUD_FAULT_EVENTS\n[\s\S]*?^#endif\n/gm, '');
    if (entry.filter === 'config') source = source.replace(/^void mayapI2cReport\(uint8_t address, bool ok\);\n|^uint32_t mayapI2cRecoveryEpoch\(\);\n/gm, '');
    assert.equal(crypto.createHash('sha256').update(source).digest('hex'), entry.sha256, entry.file + ' ' + (entry.signature || ''));
  }
});
test('all service tasks admit and beat themselves, including isolation paths', () => {
  const ino = read(dir + 'MAYAP_INDUSTRIAL_v1_0_0.ino');
  for (const [task, service] of [['networkTask','Network'], ['mqttTask','Mqtt'], ['cloudTask','Cloud'], ['otaTask','Ota']]) {
    const source = body(ino, `void ${task}(`);
    assert.match(source, new RegExp(`mayapServiceAdmit\\(MayapRecovery::Service::${service}\\)`));
    assert.match(source, new RegExp(`mayapServiceBeat\\(MayapRecovery::Service::${service}\\)`));
    assert.match(source, /mayapServiceRecoveryComplete/);
    if (service !== 'Mqtt') assert.match(source, /mayapServiceIsolated/);
    else assert.match(source, /mayapSetRealtimeOnline\(false\)/);
    for (const prefix of source.split(/\bcontinue;/).slice(0,-1)) assert.match(prefix, /mayapServiceBeat/);
  }
});
test('online service failure degrades without controller restart authority', () => {
  const source = body(read(dir + 'MAYAP_INDUSTRIAL_v1_0_0.ino'), 'void supervisorTask(');
  const start = source.indexOf('MayapRecovery::Service failedService');
  const end = source.indexOf('const esp_err_t result = esp_task_wdt_reset();', start);
  assert.notEqual(start, -1);
  assert.notEqual(end, -1);
  const runtime = source.slice(start, end);
  assert.match(runtime, /mayapServiceSupervisorUpdate\(now\);/);
  assert.doesNotMatch(runtime, /mayapLatchSystemTrip|vTaskSuspend\(controlTaskHandle\)|mayapSafeOutputsEarly|mayapRestart/);
  assert.match(read(dir + 'runtime_recovery_policy.h'), /Action : uint8_t \{ None, Reinit, Isolate, Degraded \}/);
  assert.doesNotMatch(read(dir + 'runtime_recovery_policy.h'), /Action::Restart/);
  assert.match(read(dir + 'service_recovery.h'), /LOCAL CONTROL CONTINUES, NO RESTART/);
  for (const file of ['i2c_supervisor.h', 'service_recovery.h', 'runtime_recovery_policy.h', 'network_service.h'])
    assert.doesNotMatch(read(dir + file), /\bmayapRestart\(/);
});
test('shared I2C recovery has one bus reset owner and never clears physical faults', () => {
  const hmi = read(dir + 'hmi.h');
  assert.doesNotMatch(hmi, /recoverI2cBusUnlocked|Wire\.end\(|Wire\.begin\(/);
  const bus = body(read(dir + 'i2c_supervisor.h'), 'inline void mayapI2cSupervisorUpdate(');
  assert.ok(bus.indexOf('mayapI2cLock(0U)') < bus.indexOf('Wire.end()'));
  assert.match(bus, /pulse < 9U/);
  assert.match(bus, /mayapI2cUnlock/);
  assert.doesNotMatch(bus, /clearRecovered|EEPROM\.write|mayapRestart/);
});

test('Online task creation, stalls and heap pressure never request local restart',()=>{
 const ino=read(dir+'MAYAP_INDUSTRIAL_v1_0_0.ino'),machine=read(dir+'machine_control.h');
 assert.doesNotMatch(ino,/healthRestartRequested|RestartReason::HealthMonitor/);
 for(const task of ['networkTask','mqttTask','cloudTask','otaTask'])
  assert.doesNotMatch(body(ino,'void '+task+'('),/mayapRestart|fatalRestart|subscribeCurrentTaskToWdt|vTaskSuspend/);
 assert.doesNotMatch(body(machine,'  void serviceHealthHeap('),/healthRestartRequested|mayapRestart/);
 assert.match(ino,/bootSequence.age\(now\) >= MayapBoot::WIFI_WAIT_MS/);
 assert.match(ino,/bootSequence.age\(now\) >= MayapBoot::MQTT_WAIT_MS/);
 assert.match(ino,/wdtConfig.idle_core_mask = 0U/);
 const control=body(ino,'void controlTask(');
 assert.doesNotMatch(control,/mayapOnlineOwnersDrained|mayapOnlineIoEnter|WiFi\.|HTTPClient|esp_tls|WebSocket/);
 assert.match(ino,/controlTask, "mayap_ctrl", sizeof\(controlTaskStack\), nullptr, 5,[\s\S]*?controlTaskStack, &controlTaskTcb, 1/);
 assert.match(ino,/hmiTask, "mayap_hmi", sizeof\(hmiTaskStack\), nullptr, 2,\s*hmiTaskStack, &hmiTaskTcb, 1/);
 for (const task of ['network','mqtt','cloud','ota'])
  assert.match(ino,new RegExp(task+'Task, "mayap_'+task+'",[\\s\\S]*?'+task+'TaskStack, &'+task+'TaskTcb, 0'));
});
test('radio mutations require real owner closure and Online startup cannot bypass admission',()=>{
 const network=read(dir+'network_service.h'),ino=read(dir+'MAYAP_INDUSTRIAL_v1_0_0.ino');
 assert.match(network,/mayapOnlineOwnersDrained\(\)/);
 assert.doesNotMatch(network,/setAutoReconnect\(true\)/);
 assert.doesNotMatch(read(dir+'transaction_bridge.h'),/WiFi\.\w+\s*\(|esp_wifi_(?:get|set)_ps\(/);
 assert.doesNotMatch(body(ino,'void mqttTask('),/WiFi\./);
 const startup=body(ino,'void networkTask(').split('mayapNetworkBegin();')[0];
 assert.match(startup,/mayapRadioQuiesceBegin\(\);\s*while \(!mayapOnlineOwnersDrained\(\)\)/);
 for(const service of ['Mqtt','Cloud','Ota']){
  if(service!=='Mqtt') assert.match(ino,new RegExp('mayapOnlineIoEnter\\(MayapRecovery::Service::'+service+'\\)'));
  if(service!=='Mqtt') {
   assert.match(ino,new RegExp('mayapOnlineIoLeave\\(MayapRecovery::Service::'+service+'\\)'));
   assert.match(ino,new RegExp('mayapOnlineOwnerQuiet\\(MayapRecovery::Service::'+service+'\\)'));
  }
 }
});
