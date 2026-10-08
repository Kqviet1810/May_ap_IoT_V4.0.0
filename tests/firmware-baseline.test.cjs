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

test('Wi-Fi guide uses HMI, and ArduinoOTA (LAN upload) stays removed: USB flashing + signed web OTA only', () => {
  const html = read('index.html');
  assert.match(html, /Cài đặt chung → Hệ thống → Đổi Wi‑Fi/);
  assert.doesNotMatch(html, /Giữ nút BOOT/);
  const publicBuild = read('MAYAP_INDUSTRIAL_v1_0_0/build_public.h');
  assert.doesNotMatch(publicBuild, /OTA_PASSWORD/);
  for (const gone of ['ota_update.h', 'arduino_ota_window.h'])
    assert.equal(fs.existsSync(path.resolve(__dirname, '../MAYAP_INDUSTRIAL_v1_0_0/' + gone)), false, gone);
  assert.doesNotMatch(read('MAYAP_INDUSTRIAL_v1_0_0/MAYAP_INDUSTRIAL_v1_0_0.ino'), /ArduinoOTA|mayapOtaBegin|mayapOtaUpdate/);
  assert.equal(fs.existsSync(path.resolve(__dirname, '../MAYAP_INDUSTRIAL_v1_0_0/build_secrets.h')), false);
  assert.doesNotMatch(publicBuild, /MAYAP_MQTT/);
});

test('transport policy: firmware speaks MQTT/WSS to the Cloudflare broker from mayap_mqtt only (no esp-mqtt, no WebSocket library); Web stays MQTT.js/WSS; no tracked credentials',()=>{
 const crypto=require('node:crypto');
 const mqttJs=fs.readFileSync(path.resolve(__dirname,'../vendor/mqtt.min.js'));
 assert.equal(crypto.createHash('sha256').update(mqttJs).digest('hex'),'13f43563b76f99bc60d278fd3f5d7056038d016fcb067310ff14591e73f3b9bb');
 assert.match(read('vendor/README.md'),/MQTT\.js\*\* \*\*5\.13\.2\*\*|MQTT\.js \*\*5\.13\.2\*\*/);
 const firmware=read('MAYAP_INDUSTRIAL_v1_0_0/mqtt_transport.h');
 // ESP32: TLS (WiFiClientSecure) + our own bounded WebSocket framing + our own MQTT 3.1.1, all polled by the
 // single owner task. No esp-mqtt (hidden task), no esp_websocket_client (hidden task), no library WebSocket.
 assert.match(firmware,/WiFiClientSecure/);
 assert.match(firmware,/setCACert\(TLS_ROOT_CA\)/);
 assert.match(firmware,/#include "mqtt_wire\.h"/);
 assert.match(firmware,/#include "mqtt_ws\.h"/);
 assert.match(firmware,/"\/mqtt\/%s"/);                                  // same path the Web uses: /mqtt/<deviceId>
 assert.doesNotMatch(firmware,/mqtt_client\.h|esp_mqtt|esp_websocket_client|WebSocketsClient|WebSocketsServer|PubSubClient|setInsecure|xTaskCreate|vTaskCoreAffinitySet|MAYAP_BISECT/);
 for (const f of ['mqtt_wire.h','mqtt_ws.h'])
   assert.doesNotMatch(read('MAYAP_INDUSTRIAL_v1_0_0/'+f).replace(/\/\/[^\n]*/g,''),/#include <Arduino|FreeRTOS|malloc\(|\bnew\b|\bString\b/,f);
 for (const gone of ['MAYAP_INDUSTRIAL_v1_0_0/mqtt_core_pin.cpp','MAYAP_INDUSTRIAL_v1_0_0/build_local.h','platform.local.txt','broker'])
   assert.equal(fs.existsSync(path.resolve(__dirname,'..',gone)),false,gone);   // no extra server, no linker/Kconfig hack
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
 assert.doesNotMatch(read('.github/workflows/build-firmware.yml'),/build_local|FIXTURE|--wrap|trace-symbol/);
});

test('MQTT owner: one static pinned task on core 0, control/supervisor/HMI stay on core 1, no other task creation', () => {
 const ino=read('MAYAP_INDUSTRIAL_v1_0_0/MAYAP_INDUSTRIAL_v1_0_0.ino');
 assert.match(ino,/mqttTask, "mayap_mqtt", sizeof\(mqttTaskStack\), nullptr, 2,\s*mqttTaskStack, &mqttTaskTcb, 0\)/);
 for (const name of ['mayap_ctrl','mayap_supervisor','mayap_hmi'])
   assert.match(ino,new RegExp(`"${name}"[\\s\\S]{0,120}?, 1\\);`),name);
 assert.equal((ino.match(/xTaskCreate(?!StaticPinnedToCore)\w*\(/g)||[]).length,0);
 for (const f of fs.readdirSync(path.resolve(__dirname,'../MAYAP_INDUSTRIAL_v1_0_0')).filter((n)=>/\.(h|cpp)$/.test(n)))
   assert.doesNotMatch(read('MAYAP_INDUSTRIAL_v1_0_0/'+f),/xTaskCreate\w*\(/,f);
 // Watchdog/heartbeat policy untouched.
 assert.match(read('MAYAP_INDUSTRIAL_v1_0_0/config.h'),/CONTROL_HEARTBEAT_TIMEOUT_MS = 500UL/);
 assert.match(read('MAYAP_INDUSTRIAL_v1_0_0/config.h'),/CONTROL_WDT_TIMEOUT_MS = 5000UL/);
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
