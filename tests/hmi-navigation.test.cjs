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
  // Thermal Control V2.0.0: the old controller's rows (15-17 Kp/Ki/Kd, 26-27 tune, 51 balance, 54 Smart Thermal, 55 Predictive) stay in the
  // SETTINGS table (indexes and stored config keep their meaning) but no group lists them any more.
  const hidden = [15, 16, 17, 26, 27, 51, 54, 55];
  assert.equal(indexes.length, 29);
  assert.equal(new Set(indexes).size, 29);
  assert.deepEqual([...indexes].sort((a, b) => a - b), [
    ...Array.from({ length: 33 }, (_, i) => i).filter((i) => ![18, 33, 34, 35, 36, 37, 38, 39, 40, 41, ...hidden].includes(i)), 52, 53,
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

test('Thermal Control V2.0.0: the Auto Tune screens, confirmations and runtime hooks are gone from the HMI', () => {
  for (const gone of ['openAutoTuneEntry', 'openAutoTuneConfirm', 'drawAutoTune', 'autoTuneStateText', 'AutoTuneState', 'ThermalProfile', 'drawThermalProfile']) {
    assert.ok(!hmi.includes(gone), `${gone} must be gone`);
  }
  assert.doesNotMatch(hmi, /ConfirmAction::AutoTune|View::AutoTune/);
  // the command ids stay (protocol/queue numbering) but nothing in the HMI can queue them
  assert.doesNotMatch(hmi, /queueCommand\(HmiCommandType::AutoTune/);
  assert.match(config, /AutoTuneCancel,/);
});


// ---- NANG CAO (technical access) ----
test('PID, thermal protection and calibration live only under NANG CAO; operating thresholds stay in NHIET DO', () => {
  const idx = hmi.match(/const uint8_t GROUP_SETTING_INDEXES\[\] = \{([\s\S]*?)\};/)[1]
    .replace(/\/\/[^\n]*/g, '').match(/\d+/g).map(Number);
  const groups = [...hmi.matchAll(/\{"([^"\n]+)",\s*(\d+),\s*(\d+)\}/g)]
    .map((m) => ({ name: m[1], first: Number(m[2]), count: Number(m[3]) }));
  const of = (name) => { const g = groups.find((x) => x.name === name); return idx.slice(g.first, g.first + g.count); };
  assert.deepEqual(of('NHIET DO'), [4, 5, 6, 8]);                 // low / high / emergency / out-of-batch: unchanged
  assert.deepEqual(of('GIA NHIET'), [19]);                       // only the power ceiling is left of the old PID group
  assert.deepEqual(of('BAO VE NHIET'), [20, 21, 22, 23, 24, 25]);
  assert.deepEqual(of('HIEU CHUAN'), [7, 52, 32, 53]);      // + Bo tao am (hardware) and DPG cam bien, both behind the technical PIN
  assert.deepEqual(of('HE THONG'), [14, 29]);
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

test('PID Monitor is a read-only view of what the firmware measured and commanded', () => {
  const info = bodyOf('void infoItem(');
  for (const key of ['temperature', 'targetTemp', 'm.error', 'requestPct', 'actualAvgPct', 'cycleLastUs', 'cycleMaxUs']) {
    assert.ok(info.includes(key), `${key} is shown`);
  }
  for (const gone of ['pidCorrection', 'holdFf', 'ventFf', 'limitLo', 'limitHi', 'thermalConfidence', 'launchGain', 'predState', 'currentConfig.kp']) {
    assert.ok(!info.includes(gone), `${gone} belonged to the old controller`);
  }
  assert.match(hmi, /constexpr uint8_t MON_ITEM_COUNT = 6U;/);
  assert.doesNotMatch(bodyOf('void drawInfoPage('), /queueCommand|startConfigSave/);
  const input = bodyOf('void handleInput()');
  const monCase = input.slice(input.indexOf('case View::PidMonitor:'), input.indexOf('case View::AdvHistory:'));
  assert.doesNotMatch(monCase, /queueCommand|startConfigSave/);
});

test('HE THONG: Thong tin ket noi gathers Doi wifi / Ma QR / Dat lai PIN / Thiet bi ket noi; Thoi gian ngu is a system item', () => {
  const slots = bodyOf('GroupExtra groupExtraSlot(');
  assert.match(slots, /group == 3 && slot == 0\) return GroupExtra::ConnectionInfo/);
  assert.match(slots, /group == 3 && slot == 1\) return GroupExtra::SleepTime/);
  assert.match(slots, /group == 3 && slot == 2\) return GroupExtra::FirmwareWebUpdate/);
  for (const moved of ['WifiChange', 'QrCode', 'CloudPinReset']) assert.doesNotMatch(slots, new RegExp(`return GroupExtra::${moved}`));
  // the sub-menu: Chi tiet is the old "Thong tin ket noi" view; cloud-backed items only while Online
  const items = bodyOf('ConnItem connMenuItemAt(');
  const order = ['Detail', 'WifiChange', 'QrCode', 'PinReset', 'Members', 'Exit'].map((n) => items.indexOf(`ConnItem::${n}`));
  assert.ok(order.every((v, i) => v >= 0 && (i === 0 || v > order[i - 1])), 'order Chi tiet, Doi wifi, QR, PIN, Thiet bi, Thoat');
  assert.match(items, /if \(online\) items\[n\+\+\] = ConnItem::WifiChange/);
  assert.match(items, /if \(online\) \{ items\[n\+\+\] = ConnItem::PinReset; items\[n\+\+\] = ConnItem::Members; \}/);
  const input = bodyOf('void handleInput()');
  assert.match(input, /case ConnItem::Detail: view = View::ConnectionInfo/);
  assert.match(input, /case ConnItem::Members: openMemberList\(\)/);
  // every sub screen returns to the sub-menu, the sub-menu returns to HE THONG
  const back = bodyOf('void goBack()');
  for (const v of ['WifiChange', 'ConnectionInfo', 'QrCode', 'MemberList']) assert.match(back, new RegExp(`case View::${v}:[\\s\\S]*?view = View::ConnMenu`));
  assert.match(back, /case View::ConnMenu:\s*view = View::SettingList/);
  assert.match(bodyOf('void openCloudPinResetConfirm('), /confirmReturnView = View::ConnMenu/);
});

test('Thiet bi ket noi: list from Cloud, owner cannot be removed, removal needs CO/KHONG confirmation', () => {
  const input = bodyOf('void handleInput()');
  assert.match(input, /memberView\.entry\[memberIndex\]\.owner\) showToast\("KHONG XOA DUOC CHU MAY"/);
  assert.match(input, /else openMemberRemoveConfirm\(memberIndex\)/);
  assert.match(bodyOf('void openMemberRemoveConfirm('), /confirmAction = ConfirmAction::MemberRemove/);
  assert.match(hmi, /action == ConfirmAction::MemberRemove\) \{\s*mayapRequestMemberRemove\(memberRemovePending\)/);
  assert.match(bodyOf('void openMemberList('), /CHI DUNG DUOC KHI ONLINE/);
  const cloud = fs.readFileSync(path.join(root, 'MAYAP_INDUSTRIAL_v1_0_0', 'cloud_alert_link.h'), 'utf8');
  assert.match(cloud, /!memberSnap\.entry\[removeAt\]\.owner/);                       // firmware refuses an owner row even if asked
  assert.match(cloud, /"\/api\/device\/members\/remove"/);
  assert.match(cloud, /servicePinReset\(\);\s*serviceMembers\(\);/);                    // same Cloud owner/admission path as reset-PIN, no new task
});

test('Man hinh ngu: header (time left, date right, rule; NO border)  + only a large centred temperature with a small degree-C; 0 minutes switches the sleep screen off; 2 min default; managed in HE THONG', () => {
  assert.match(hmi, /SLEEP_MINUTES_DEFAULT = 2U/);
  const draw = bodyOf('void drawSleep()');
  assert.doesNotMatch(draw, /drawRFrame|drawFrame/);                                       // no border around the screen
  assert.match(draw, /lcd\.drawStr\(2, 8, currentRuntime\.timeText\)/);                // time, left of the header row
  assert.match(draw, /currentRuntime\.dateText/);                                       // date, right of the header row
  assert.match(draw, /lcd\.drawHLine\(0, 10, 128\)/);                                   // rule under the header
  assert.match(draw, /currentRuntime\.temperature/);
  assert.match(draw, /u8g2_font_logisoso38_tn/);                                        // the biggest numeric font that fits the band below the header
  assert.match(draw, /\(128 - \(w \+ UNIT_W\)\) \/ 2/);                               // horizontally centred
  assert.match(draw, /drawCircle\([^)]*\)[\s\S]*"C"/);                                  // small degree sign + C
  assert.doesNotMatch(draw, /humidity|drawVLine/);                                       // temperature only: no humidity, no divider
  assert.doesNotMatch(draw, /targetTemp|currentDay|machineState/);                       // nothing else on the sleep screen
  // 0 = the sleep screen is OFF: the screen never sleeps, and the editor says so
  assert.match(hmi, /const bool canSleep = sleepMinutes > 0U && view == View::Home/);
  assert.match(bodyOf('void drawSleepEdit()'), /"Man ngu \(0 = TAT\)"/);
  assert.match(bodyOf('void drawSleepEdit()'), /sleepEditValue == 0U\) snprintf\(value, sizeof\(value\), "TAT"\)/);
  // the sleep-time editor uses the SAME layout as the parameter editor (drawEditSetting): header, label y=23, big value y=48, hint y=62
  const sleepEditDraw = bodyOf('void drawSleepEdit()');
  const param = bodyOf('void drawEditSetting()');
  assert.match(sleepEditDraw, /drawHeader\("SUA THONG SO", false\)/);
  assert.match(param, /drawHeader\("SUA THONG SO", false\)/);
  assert.match(sleepEditDraw, /drawCenteredFit\(23, "Man ngu \(0 = TAT\)"/);
  assert.match(sleepEditDraw, /drawCenteredFit\(48, value, u8g2_font_helvB14_tf, u8g2_font_helvB12_tf, u8g2_font_6x12_tf\)/);
  assert.match(param, /drawCenteredFit\(48, value, u8g2_font_helvB14_tf,\s*u8g2_font_helvB12_tf,\s*u8g2_font_6x12_tf\)/);
  assert.match(sleepEditDraw, /lcd\.drawStr\(11, 62, "NHAN: LUU   GIU: HUY"\)/);
  assert.match(param, /lcd\.drawStr\(11, 62, "NHAN: LUU   GIU: HUY"\)/);
  const update = bodyOf('void hmiUpdate(');
  assert.match(update, /now - lastInteractionAt >= static_cast<uint32_t>\(sleepMinutes\) \* 60000UL/);
  assert.match(update, /view == View::Home && !splashActive && !confirmationActive\(\)/);
  assert.match(update, /currentRuntime\.activeFaultCount == 0 && currentRuntime\.alarmMask == 0/);   // a fault or alarm always wakes the screen
  // the first input only wakes the screen; it never triggers the action underneath
  const input = bodyOf('void handleInput()');
  assert.match(input, /if \(sleepActive && \(rotary\.step \|\| rotary\.button != ButtonEvent::None\)\) \{\s*sleepActive = false;[\s\S]*?return;/);
  assert.match(bodyOf('void loadSleepMinutes('), /"mayap_ui"[\s\S]*sleep_min/);
  const edit = input.slice(input.indexOf('case View::SleepTime:'));
  assert.match(edit, /constrain\(static_cast<int>\(sleepEditValue\) \+ rotary\.step, 0, SLEEP_MINUTES_MAX\)/);
  assert.match(edit, /saveSleepMinutes\(sleepEditValue\)/);
});

test('Bo tao am and DPG cam bien live under NANG CAO > HIEU CHUAN; the sensor resolution is a three-way choice', () => {
  assert.match(hmi, /ITEM_BOOL_OPTIONS\("Bo tao am", humidifierInstalled/);
  assert.doesNotMatch(hmi, /"Co bo tao am"/);
  assert.match(hmi, /OPT_SENSOR_RES\[\] = \{"TU DONG", "0\.1 C", "0\.01 C"\}/);
  assert.match(hmi, /ITEM_U8_OPTIONS\("DPG cam bien", sensorResolutionMode, OPT_SENSOR_RES, 3\)/);
  assert.match(hmi, /sensorResolutionMode > 2U\) cfg\.sensorResolutionMode = 0U/);
  const controller = fs.readFileSync(path.join(root, 'MAYAP_INDUSTRIAL_v1_0_0/machine_control.h'), 'utf8');
  assert.match(controller, /sensor_\.setResolutionMode\(config_\.sensorResolutionMode\);\s*sensor_\.update\(\);/);
});

test('Thermal Control V2.0.0: Smart Thermal and Nhiet du doan switches are hidden from the menu and nothing in the controller reads them', () => {
  const controller = fs.readFileSync(path.join(root, 'MAYAP_INDUSTRIAL_v1_0_0/machine_control.h'), 'utf8');
  // the stored fields and their packed bits stay (config schema 13 compatibility)
  assert.match(config, /bool smartThermalEnabled = false;/);
  assert.match(config, /bool predictiveThermalEnabled = true;/);
  assert.match(controller, /c\.smartThermalEnabled \? 8U : 0U/);
  assert.match(controller, /\(c\.predictiveThermalEnabled \? 0U : 16U\)/);
  assert.match(controller, /c\.predictiveThermalEnabled = \(p\.adaptiveThermalBalanceEnabled & 16U\) == 0U;/);
  // but no controller code consumes them
  for (const gone of ['smartThermalApplied_', 'predictiveApplied_', 'thermalPredictive_', 'thermalV1_', 'adaptiveThermal_', 'startupHeat_', 'autotune_', 'pid_.', 'pidPower_']) {
    assert.ok(!controller.includes(gone), `${gone} must be gone from machine_control.h`);
  }
  const bridge = fs.readFileSync(path.join(root, 'MAYAP_INDUSTRIAL_v1_0_0/transaction_bridge.h'), 'utf8');
  assert.doesNotMatch(bridge, /smartThermalEnabled|predictiveThermalEnabled/);
});

test('Che do 2 tai khoan: only in HMI Nang cao, needs Online, the machine asks the Worker with its own key, back returns to the Advanced list', () => {
  const cloud = fs.readFileSync(path.join(root, 'MAYAP_INDUSTRIAL_v1_0_0', 'cloud_alert_link.h'), 'utf8');
  // Advanced list: the item sits after "Quyen Web" and before "Doi ma ky thuat"; the screen belongs to the Advanced session (PIN keep-alive, lock on leave)
  assert.match(hmi, /enum AdvItem : uint8_t \{[^}]*ADV_WEB, ADV_GUEST, ADV_PINCHG, ADV_EXIT \}/);
  assert.match(hmi, /constexpr uint8_t ADV_ITEM_COUNT = 9U;/);
  assert.match(bodyOf('bool viewInAdvanced('), /case View::GuestMode:/);
  assert.match(bodyOf('void advSelect()'), /case ADV_GUEST: openGuestMode\(\); break;/);
  assert.match(bodyOf('void goBack()'), /case View::GuestMode: view = View::AdvMenu; advIndex = ADV_GUEST;/);
  assert.match(bodyOf('void openGuestMode('), /CHI DUNG DUOC KHI ONLINE/);
  // not reachable from the normal menus (no GroupExtra / ConnItem entry)
  assert.doesNotMatch(bodyOf('const char *groupExtraLabelFor('), /GuestMode|2 tai khoan/);
  assert.doesNotMatch(bodyOf('const char *connItemLabel('), /GuestMode|2 tai khoan/);
  // the knob toggles, a changed choice is sent (1 = on, 2 = off), an unchanged one just leaves; the answer arrives through the snapshot
  const input = bodyOf('void handleInput()');
  assert.match(input, /case View::GuestMode: \{[\s\S]*?mayapRequestGuestMode\(guestChoice \? 1U : 2U\)/);
  assert.match(bodyOf('void drawGuestMode('), /TK THU 2 BI XOA KHOI MAY/);                               // the consequence of "off" is on the screen
  // firmware side: device key + action to the Worker, same admission path as reset-PIN / members, no new task
  assert.match(cloud, /"\/api\/device\/guest-mode"/);
  assert.match(cloud, /doc\["device_key"\] = mayapDeviceSecret\(\);\s*doc\["action"\]/);
  assert.match(cloud, /servicePinReset\(\);\s*serviceMembers\(\);\s*serviceGuestMode\(\);/);
  assert.match(config, /void mayapRequestGuestMode\(uint8_t action\);/);
});
