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

test('production dependencies contain no broker library, fleet credential or MQTT browser bundle',()=>{
 assert.equal(fs.existsSync(path.resolve(__dirname,'../vendor/mqtt.min.js')),false);
 assert.equal(fs.existsSync(path.resolve(__dirname,'../MAYAP_INDUSTRIAL_v1_0_0/mqtt_transport.h')),false);
 assert.doesNotMatch(read('MAYAP_INDUSTRIAL_v1_0_0/transaction_bridge.h'),/PubSubClient|MqttTransport|MQTT_BROKER/);
 assert.doesNotMatch(read('.github/workflows/build-firmware.yml'),/PubSubClient|MAYAP_MQTT_/);
 assert.doesNotMatch(read('index.html'),/mqtt.min.js/);
});
