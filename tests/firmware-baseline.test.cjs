const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const read = (name) => fs.readFileSync(path.resolve(__dirname, '..', name), 'utf8');
const ota = read('MAYAP_INDUSTRIAL_v1_0_0/ota_web_update.h');
const hmi = read('MAYAP_INDUSTRIAL_v1_0_0/hmi.h');

test('Cloud and OTA transient TLS users share nonblocking admission with memory budget', () => {
  for (const name of ['cloud_alert_link.h', 'ota_web_update.h'])
    assert.match(read(`MAYAP_INDUSTRIAL_v1_0_0/${name}`), /MayapTlsOperation tlsOperation/);
  const gate = read('MAYAP_INDUSTRIAL_v1_0_0/network_io_guard.h');
  assert.match(gate, /__atomic_compare_exchange_n/);
  assert.match(gate, /49152U : 73728U/);
  assert.match(gate, /ESP.getMaxAllocHeap\(\) < 24576U/);
  assert.match(gate, /class MayapNetworkBatchOperation/);
  assert.match(gate, /~MayapTlsOperation/);
  // Busy admission must not consume an explicit OTA request/check.
  assert.match(ota, /if \(!tlsOperation\) return;[^\n]*\n\s*__atomic_store_n\(&applyRequestFlag/);
});

test('OTA keeps physical confirmation, maintenance interlock and strict byte/signature boundaries', () => {
  const app = read('app.js');
  assert.match(app, /sendCommand\('firmware_check_now'\)/);
  assert.doesNotMatch(app, /sendCommand\('firmware_apply'\)/);
  assert.match(ota, /if \(!mayapFirmwareMaintenanceReady\(\)\)/);
  assert.match(ota, /std::min<size_t>\(static_cast<size_t>\(remaining\)/);
  assert.match(ota, /mbedtls_pk_verify/);
  assert.ok(ota.indexOf('mbedtls_pk_verify') < ota.indexOf('Update.end(false)'));
  assert.match(ota, /Update.abort\(\)/);
  assert.match(hmi, /queueCommand\(HmiCommandType::FirmwareWebCheckNow\)/);
  assert.match(hmi, /!ack.ok && command.type == HmiCommandType::FirmwareWebApply/);
});

test('Wi-Fi guide uses HMI, and ArduinoOTA has one empty tracked password source', () => {
  const html = read('index.html');
  assert.match(html, /Cài đặt chung → Hệ thống → Đổi Wi‑Fi/);
  assert.doesNotMatch(html, /Giữ nút BOOT/);
  const publicBuild = read('MAYAP_INDUSTRIAL_v1_0_0/build_public.h');
  assert.match(publicBuild, /^#define MAYAP_OTA_PASSWORD ""$/m);
  assert.equal(fs.existsSync(path.resolve(__dirname, '../MAYAP_INDUSTRIAL_v1_0_0/build_secrets.h')), false);
  assert.doesNotMatch(publicBuild, /MAYAP_MQTT/);
});

test('transport policy: standard MQTT clients only, no HiveMQ/PubSubClient/DeviceHub/custom WebSocket, no tracked credentials',()=>{
 const crypto=require('node:crypto');
 const mqttJs=fs.readFileSync(path.resolve(__dirname,'../vendor/mqtt.min.js'));
 assert.equal(crypto.createHash('sha256').update(mqttJs).digest('hex'),'13f43563b76f99bc60d278fd3f5d7056038d016fcb067310ff14591e73f3b9bb');
 assert.match(read('vendor/README.md'),/MQTT\.js\*\* \*\*5\.13\.2\*\*|MQTT\.js \*\*5\.13\.2\*\*/);
 const firmware=read('MAYAP_INDUSTRIAL_v1_0_0/mqtt_transport.h');
 assert.match(firmware,/esp_mqtt_client_init/);
 assert.match(firmware,/wss:\/\//);
 assert.doesNotMatch(firmware,/PubSubClient|WebSocketsClient|WebSocketsServer|setInsecure|esp_websocket_client/);
 assert.doesNotMatch(read('MAYAP_INDUSTRIAL_v1_0_0/transaction_bridge.h'),/PubSubClient|MqttTransport|MQTT_BROKER/);
 assert.doesNotMatch(read('.github/workflows/build-firmware.yml'),/PubSubClient|MAYAP_MQTT_/);
 assert.match(read('index.html'),/vendor\/mqtt\.min\.js/);
 assert.match(read('index.html'),/mqtt_transport\.js/);
 for (const file of ['app.js','mqtt_transport.js','config.js','config.production.example.js','cloudflare/src/account-worker.js',
   'MAYAP_INDUSTRIAL_v1_0_0/mqtt_transport.h','MAYAP_INDUSTRIAL_v1_0_0/build_public.h','MAYAP_INDUSTRIAL_v1_0_0/config.h'])
   assert.doesNotMatch(read(file),/hivemq/i,file);
 // The per-device broker password comes from the Worker (NVS), never from the source/build.
 assert.doesNotMatch(firmware,/FIXTURE_PASSWORD|build_local/);
 assert.match(firmware,/mayapMqttKey\(\)/);
 assert.match(firmware,/#define MAYAP_BROKER_HOST "[a-z0-9.-]+"/);
 assert.match(read('MAYAP_INDUSTRIAL_v1_0_0/cloud_alert_link.h'),/mqtt_password/);
 assert.doesNotMatch(read('.github/workflows/build-firmware.yml'),/build_local|FIXTURE/);
});

test('supervisor control trip persists and prints its evidence before any reset (no longer lost to TWDT)', () => {
 const ino=read('MAYAP_INDUSTRIAL_v1_0_0/MAYAP_INDUSTRIAL_v1_0_0.ino');
 const trip=ino.slice(ino.indexOf('if (!controlHealthy || deadlineTrip)'),ino.indexOf('if (hmiBeat != 0U && hmiHealthy'));
 for (const field of ['reasonText','heartbeatAgeMs','cycleUs','slowCycles','stageText','heapFree','heapMin','heapLargest'])
  assert.match(trip,new RegExp(field),field);
 // Persisted to RTC before the heap walk and before any Serial work.
 assert.ok(trip.indexOf('mayapBootPlanRestart')<trip.indexOf('heap_caps_get_largest_free_block'));
 assert.ok(trip.indexOf('mayapBootPlanRestart')<trip.indexOf('mayapSerialPrintf(true'));
 assert.match(trip,/mayapSerialPrintf\(true,\s*"\[SUPERVISOR\] TRIP/);
 assert.match(trip,/mayapSerialDrainFor\(150U\)/);
 assert.match(trip,/mayapRestart\(tripReason, detail\)/);
 assert.match(ino,/mayapSerialPrintf\(true,\s*"\[BOOT-DIAG\]/);
 // Watchdog policy untouched.
 assert.match(read('MAYAP_INDUSTRIAL_v1_0_0/config.h'),/CONTROL_WDT_TIMEOUT_MS = 5000UL/);
 assert.match(read('MAYAP_INDUSTRIAL_v1_0_0/config.h'),/CONTROL_HEARTBEAT_TIMEOUT_MS = 500UL/);
 assert.match(read('MAYAP_INDUSTRIAL_v1_0_0/config.h'),/CONTROL_CYCLE_TRIP_US = 400000UL/);
 assert.match(read('MAYAP_INDUSTRIAL_v1_0_0/serial_diagnostics.h'),/\[BOOT-DIAG\]/);
});

test('control trip separates "not scheduled" from "stuck in Machine.update", and esp-mqtt is kept off core 1', () => {
 const ino=read('MAYAP_INDUSTRIAL_v1_0_0/MAYAP_INDUSTRIAL_v1_0_0.ino');
 const control=ino.slice(ino.indexOf('void controlTask('),ino.indexOf('void hmiTask('));
 assert.ok(control.indexOf('controlInUpdate, 1U')<control.indexOf('Machine.update(now)'));
 assert.ok(control.indexOf('Machine.update(now)')<control.indexOf('controlInUpdate, 0U'));
 assert.match(control,/controlPhase, 3U[\s\S]*vTaskDelayUntil/);
 const trip=ino.slice(ino.indexOf('if (!controlHealthy || deadlineTrip)'),ino.indexOf('if (hmiBeat != 0U && hmiHealthy'));
 assert.ok(trip.indexOf('eTaskGetState(controlTaskHandle)')<trip.indexOf('vTaskSuspend(controlTaskHandle)'));
 for (const field of ['currentCycleAge','inUpdate','phase','ctrlState']) assert.match(trip,new RegExp(field),field);
 const mqtt=read('MAYAP_INDUSTRIAL_v1_0_0/mqtt_transport.h');
 assert.match(mqtt,/vTaskCoreAffinitySet\(espMqttTask, 1U << 0\)/);
 assert.match(mqtt,/xPortGetCoreID\(\)/);
 assert.match(mqtt,/MQTT_EVENT_BEFORE_CONNECT/);
 // Watchdog/heartbeat policy untouched.
 assert.match(read('MAYAP_INDUSTRIAL_v1_0_0/config.h'),/CONTROL_HEARTBEAT_TIMEOUT_MS = 500UL/);
 assert.match(read('MAYAP_INDUSTRIAL_v1_0_0/config.h'),/CONTROL_WDT_TIMEOUT_MS = 5000UL/);
});

test('bisect switches are compile-time only, default off, and never touch watchdog/heartbeat/safety', () => {
 const mqtt=read('MAYAP_INDUSTRIAL_v1_0_0/mqtt_transport.h');
 assert.match(mqtt,/#define MAYAP_BISECT_MQTT_NO_START 0/);
 assert.match(mqtt,/#define MAYAP_BISECT_MQTT_DELAY_MS 0/);
 assert.match(mqtt,/#define MAYAP_BISECT_MQTT_UNPINNED 0/);
 assert.ok(mqtt.indexOf('MAYAP_BISECT_MQTT_NO_START')<mqtt.indexOf('esp_mqtt_client_start(client)'));
 const wf=read('.github/workflows/build-firmware.yml');
 assert.match(wf,/MAYAP_BISECT_MQTT_NO_START=1/);
 assert.match(wf,/MAYAP_BISECT_MQTT_DELAY_MS=60000/);
 assert.match(wf,/firmware-bisect\${{ inputs.bisect }}-/);
 assert.doesNotMatch(wf,/WDT|HEARTBEAT/);
});
