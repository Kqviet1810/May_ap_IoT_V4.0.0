const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const dir = 'MAYAP_INDUSTRIAL_v1_0_0/';
const read = name => fs.readFileSync(path.join(__dirname, '..', name), 'utf8');

test('startup permission follows a non-splash LCD transfer and successful presence probe', () => {
  const source = read(dir + 'hmi.h');
  const render = source.slice(source.indexOf('void render(uint32_t now)'), source.indexOf('// 7. API HMI'));
  assert.ok(render.indexOf('lcd.sendBuffer()') < render.indexOf('mayapBootAcknowledgeHomeFrame()'));
  assert.match(render, /if \(!splashActive\) mayapBootAcknowledgeHomeFrame/);
  assert.match(render, /if \(!startupFrameOk\)[\s\S]*?return;/);
  assert.match(render, /startupFrameOk = splashActive \|\| mayapBootOperationsReady\(\) \|\|\s*probeLcdUnlocked\(\)/);
  const machine = read(dir + 'machine_control.h');
  for (const action of ['startBatch', 'enterTestMode'])
    assert.match(machine, new RegExp(`bool ${action}\\([^)]*\\) \\{\\s*if \\(!mayapBootOperationsReady\\(\\)\\)`));
  assert.match(machine, /mayapApplyStartupOutputPolicy\(request, mayapBootOperationsReady\(\)/);
  assert.match(machine, /const bool operationsReady = mayapBootOperationsReady\(\);[\s\S]*?if \(operationsReady\) \{[\s\S]*?processResume\(now\);[\s\S]*?updateTurning\(now\);/);
});
test('healthy static LCD menus are never periodically blanked/reinitialized or redrawn twice', () => {
  for (const file of ['hmi.h', 'config.h'])
    assert.doesNotMatch(read(dir + file), /LCD_FULL_REINIT|HMI_IDLE_SELFHEAL|HMI_VERIFY_REDRAW|uiVerifyFrames|lastLcdFullReinit/);
  assert.match(read(dir + 'hmi.h'), /if \(!ok\) \{\s*lcdReady = false;/);
  const assets = read(dir + 'boot_assets.h');
  assert.match(assets, /BOOT_LOGO_HEIGHT = 32U/);
  assert.doesNotMatch(assets, /bootStatusBits/);
});
test('Tiny restores proven GPIO/timer TX, direct IRAM capture and validates measured command width', () => {
  const source = read(dir + 'attiny_bus.h');
  assert.doesNotMatch(source, /rmt_transmit|rmt_new_tx_channel|attachInterrupt\(|noInterrupts\(/);
  assert.match(source, /esp_timer_start_once\(txReleaseTimer_/);
  assert.match(source, /GPIO_MODE_INPUT_OUTPUT_OD/);
  assert.match(source, /!commandPulseValid\(\)/);
  assert.match(source, /gpio_isr_handler_add\([^;]*busIsr, nullptr/);
  assert.match(source, /busIsr\(void \*\)[\s\S]*?esp_timer_get_time\(\)/);
  const encoder = read(dir + 'hmi.h');
  const isr = encoder.slice(encoder.indexOf('void IRAM_ATTR rotaryEncoderIsr'), encoder.indexOf('void beginRotary()'));
  assert.match(isr, /gpio_ll_get_level\(&GPIO/);
  assert.doesNotMatch(isr, /gpio_get_level|digitalRead|micros\(|Serial/);
  assert.doesNotMatch(encoder, /attachInterrupt\(/); // Arduino wrapper also lives in flash in core 3.3.11.
});
