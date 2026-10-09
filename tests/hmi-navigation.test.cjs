const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const hmi = fs.readFileSync(path.join(root, 'MAYAP_INDUSTRIAL_v1_0_0', 'hmi.h'), 'utf8');
const config = fs.readFileSync(path.join(root, 'MAYAP_INDUSTRIAL_v1_0_0', 'config.h'), 'utf8');

function bodyOf(signature) {
  const start = hmi.indexOf(signature);
  assert.notEqual(start, -1, `${signature} is missing`);
  const open = hmi.indexOf('{', start);
  let depth = 0;
  for (let i = open; i < hmi.length; i++) {
    if (hmi[i] === '{') depth++;
    if (hmi[i] === '}' && --depth === 0) return hmi.slice(open + 1, i);
  }
  throw new Error(`Unclosed ${signature}`);
}

test('one handled hold cannot trigger a second screen action', () => {
  const reset = bodyOf('void resetRotaryPending(');
  const guard = bodyOf('void armInputGuard(');
  const input = bodyOf('void handleInput()');
  assert.match(reset, /rearmSwallowedLongPress\s*&&\s*rotary\.button == ButtonEvent::LongPress/);
  assert.match(guard, /resetRotaryPending\(\);/);
  assert.match(input, /if \(!timeReached\(now, inputGuardUntil\)\)\s*\{\s*resetRotaryPending\(true\);/);
});

test('home hold never falls through to batch start or menu', () => {
  const home = bodyOf('void activateHomeContext(bool longPress)');
  assert.match(home, /if \(longPress\)\s*\{[\s\S]*?return;\s*\}/);
  assert.match(home, /eventLogFaultsOnly = true/);
  assert.match(home, /openAlarmView\(View::Home\)/);
  assert.match(home, /openBatchConfirm\(View::Home\)/);
});

test('button timing has release debouncing and release-time hold fallback', () => {
  const button = bodyOf('void updateRotary(uint32_t now)');
  assert.match(config, /BUTTON_LONG_PRESS_MS = 700UL/);
  assert.match(config, /BUTTON_RELEASE_DEBOUNCE_MS = 70UL/);
  assert.match(button, /raw == HIGH \? BUTTON_RELEASE_DEBOUNCE_MS : BUTTON_DEBOUNCE_MS/);
  assert.match(button, /now - rotary\.pressedAt >= BUTTON_LONG_PRESS_MS \?\s*ButtonEvent::LongPress : ButtonEvent::ShortPress/);
});

test('all settings remain reachable exactly once in shorter groups', () => {
  const indexes = hmi.match(/const uint8_t GROUP_SETTING_INDEXES\[\] = \{([\s\S]*?)\};/)[1]
    .replace(/\/\/[^\n]*/g, '').match(/\d+/g).map(Number);
  const groups = [...hmi.matchAll(/\{"([^"\n]+)",\s*(\d+),\s*(\d+)\}/g)]
    .map((match) => ({ name: match[1], first: Number(match[2]), count: Number(match[3]) }));
  assert.equal(indexes.length, 34);
  assert.equal(new Set(indexes).size, 34);
  assert.deepEqual([...indexes].sort((a, b) => a - b), [
    ...Array.from({ length: 33 }, (_, i) => i).filter((i) => ![18, 33, 34, 35, 36, 37, 38, 39, 40, 41].includes(i)), 51, 52,
  ]);
  assert.equal(groups.length, 9);
  assert.equal(groups.at(-1).name, 'HIEU CHUAN');
  assert.equal(groups.at(-2).name, 'TAO AM');
  let next = 0;
  for (const group of groups) {
    assert.equal(group.first, next);
    assert.ok(group.count <= 8, `${group.name} is too long`);
    next += group.count;
  }
  assert.equal(next, indexes.length);
  const vent = bodyOf('void drawVentilationMenu()');
  const advanced = bodyOf('void drawVentilationAdvanced()');
  assert.match(vent, /ventAutoEnabled/);
  assert.match(advanced, /SETTINGS\[44U \+ index\]/);
});


test('repeated acknowledged alarms re-open the Alarm screen when the buzzer re-arms', () => {
  // Source-level count is intentional here: buzzerUpdate contains braces in
  // comments/strings, so the lightweight body extractor is not suitable.
  assert.ok((hmi.match(/alarmPresentedMask\s*&=\s*~bit/g) || []).length >= 2);
});

test('running Auto Tune short press offers an explicit cancel confirmation', () => {
  const open = bodyOf('void openAutoTuneConfirm()');
  const execute = bodyOf('void executeConfirmation(bool accepted)');
  assert.match(open, /AutoTuneState::Running[\s\S]*?ConfirmAction::AutoTuneCancel/);
  assert.match(execute, /ConfirmAction::AutoTuneCancel[\s\S]*?HmiCommandType::AutoTuneCancel/);
  assert.match(config, /BatchOverdueContinue,\s*[\s\S]*?AutoTuneCancel/);
});


// ---- NANG CAO (technical access) ----
test('PID, thermal protection and calibration live only under NANG CAO; operating thresholds stay in NHIET DO', () => {
  const idx = hmi.match(/const uint8_t GROUP_SETTING_INDEXES\[\] = \{([\s\S]*?)\};/)[1]
    .replace(/\/\/[^\n]*/g, '').match(/\d+/g).map(Number);
  const groups = [...hmi.matchAll(/\{"([^"\n]+)",\s*(\d+),\s*(\d+)\}/g)]
    .map((m) => ({ name: m[1], first: Number(m[2]), count: Number(m[3]) }));
  const of = (name) => { const g = groups.find((x) => x.name === name); return idx.slice(g.first, g.first + g.count); };
  assert.deepEqual(of('NHIET DO'), [4, 5, 6, 8]);                 // low / high / emergency / out-of-batch: unchanged
  assert.deepEqual(of('PID/GIA NHIET'), [15, 16, 17, 19, 51]);
  assert.deepEqual(of('BAO VE NHIET'), [20, 21, 22, 23, 24, 25, 26, 27]);
  assert.deepEqual(of('HIEU CHUAN'), [7, 52]);
  assert.match(hmi, /bool advGroup\(uint8_t group\) \{ return group == 4U \|\| group == 5U \|\| group == 8U; \}/);
  assert.match(hmi, /CHUNG_GROUP_IDS\[\] = \{1U, 2U, 3U, 6U, 7U\}/);
  assert.match(bodyOf('const char *chungItemLabel('), /"NANG CAO"/);
  // an advanced group cannot be opened without an unlocked advanced session
  assert.match(bodyOf('void openGroup('), /advGroup\(group\) && !advActive\) return/);
});

test('technical PIN: four rotary digits, short press confirms, hold goes back, nothing is kept or logged', () => {
  const input = bodyOf('void handleInput()');
  const pinCase = input.slice(input.indexOf('case View::AdvPin:'), input.indexOf('case View::AdvMenu:'));
  assert.match(pinCase, /% 10/);
  assert.match(pinCase, /pinDigits\[pinPos\+\+\] = pinCur/);
  assert.match(pinCase, /pinPos >= ADV_PIN_DIGITS\) advPinSubmit\(\)/);
  assert.match(hmi, /constexpr uint8_t ADV_PIN_DIGITS = 4U/);
  const back = bodyOf('void advPinBack() {');
  assert.match(back, /--pinPos/);
  const submit = bodyOf('void advPinSubmit()');
  assert.match(submit, /TechPinVerify/);
  assert.match(submit, /TechPinSet/);
  assert.match(submit, /pinDigits\[i\] = 0U;/);                       // digits wiped after queueing
  for (const fn of ['void advPinSubmit() {', 'void onTechAck(', 'void advOpenPin(PinPurpose purpose) {']) {
    assert.doesNotMatch(bodyOf(fn), /Serial|printf\("%[^"]*pin|LOG|log\(/i, `${fn} must not log the PIN`);
  }
  assert.doesNotMatch(hmi, /pinDigits[^;]*"[0-9]{4}"/);                // no hard-coded PIN
  const draw = bodyOf('void drawAdvPin()');
  assert.match(draw, /drawDisc/);                                       // entered digits are masked
});

test('advanced edits stay in RAM and are saved once, only when changed, on exit; failure keeps the edits', () => {
  const commit = bodyOf('void commitSetting()');
  const advBranch = commit.slice(commit.indexOf('if (advActive && advGroup(selectedGroup))'), commit.indexOf('MachineConfig candidate = currentConfig;'));
  assert.doesNotMatch(advBranch, /startConfigSave|queueCommand/);
  assert.match(advBranch, /advEditedMask \|=/);
  const exit = bodyOf('void advBeginExit() {');
  assert.match(exit, /if \(!changed\) \{ advEnd\(true\); return; \}/);
  assert.equal((exit.match(/startConfigSave\(/g) || []).length, 1);
  assert.match(exit, /advSaving = true/);
  const ack = bodyOf('void processConfigAck(const ConfigAckInbox &ack) {');
  assert.match(ack, /if \(advTxn\) advEnd\(true\)/);
  assert.match(ack, /advSaving = false;[\s\S]*LOI LUU - GIU THAY DOI/);
  // the editor reads/limits against the draft, not the live config
  assert.match(bodyOf('void openSettingIndex('), /editConfigView\(\)/);
});

test('Web technical request: HMI decides, hold = decline, answer goes to the firmware (no local apply)', () => {
  const input = bodyOf('void handleInput()');
  assert.match(input, /view == View::TechRequest\) \{[^}]*techDecide\(false\)/);
  const decide = bodyOf('void techDecide(');
  assert.match(decide, /HmiCommandType::TechDecision/);
  assert.doesNotMatch(decide, /currentConfig\s*=|startConfigSave/);
  const svc = bodyOf('void serviceAdvanced(');
  assert.match(svc, /pendingKind && !techReqShown && techRequestMayOpen\(\)/);
  assert.match(svc, /advLockWanted/);
  assert.match(svc, /ADV_KEEPALIVE_MS/);
  // never pops over an alarm / test / firmware update
  assert.match(bodyOf('bool techRequestMayOpen()'), /case View::Alarm:[\s\S]*case View::FirmwareProgress/);
});

test('old records are HMI-only and restore asks CO/KHONG', () => {
  const input = bodyOf('void handleInput()');
  assert.match(input.slice(input.indexOf('case View::AdvHistoryView:')), /openAdvRestoreConfirm\(\)/);
  assert.match(hmi, /ConfirmAction::AdvRestore[\s\S]*HmiCommandType::AdvancedRestore/);
  assert.match(bodyOf('void drawAdvHistoryView()'), /KHOI PHUC/);
});

test('PID Monitor and thermal profile are read-only views fed by the running controller', () => {
  const info = bodyOf('void infoItem(');
  for (const key of ['pidCorrection', 'holdFf', 'ventFf', 'limitLo', 'limitHi', 'requestPct', 'actualAvgPct', 'thermalConfidence',
    'thermalGain', 'thermalDelaySec', 'thermalCoastC', 'thermalHoldPct', 'thermalVentGain', 'currentConfig.kp', 'currentConfig.ki', 'currentConfig.kd']) {
    assert.ok(info.includes(key), `${key} is shown`);
  }
  assert.doesNotMatch(bodyOf('void drawInfoPage('), /queueCommand|startConfigSave/);
  const input = bodyOf('void handleInput()');
  const monCase = input.slice(input.indexOf('case View::PidMonitor:'), input.indexOf('case View::AdvHistory:'));
  assert.doesNotMatch(monCase, /queueCommand|startConfigSave/);
});
