'use strict';
// "Chi tiet" in the Web fault popup deep-links into the operating manual (docs/...html#E<code>): every fault code the firmware can
// raise must have a title + description in the Web AND a row anchor in the manual, so the link can never point at nothing.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const root = path.resolve(__dirname, '..');
const read = (p) => fs.readFileSync(path.join(root, p), 'utf8');
const MANUAL = 'docs/MAYAP_Huong_dan_van_hanh_A5_v1.3_E503.html';

function firmwareCodes() {
  const src = read('MAYAP_INDUSTRIAL_v1_0_0/machine_control.h');
  const start = src.indexOf('enum class FaultCode : uint16_t {');
  const body = src.slice(start, src.indexOf('};', start));
  return [...new Set([...body.matchAll(/=\s*(\d{3})\b/g)].map((m) => Number(m[1])))].sort((a, b) => a - b);
}
function objectKeys(app, name) {
  const start = app.indexOf(`const ${name} = {`);
  assert.ok(start > 0, `${name} exists`);
  const body = app.slice(start, app.indexOf('\n  };', start));
  return [...body.matchAll(/(?:^|[\s,{])(\d{3}):/g)].map((m) => Number(m[1])).sort((a, b) => a - b);
}

test('every firmware fault code has a Web title, a Web description and exactly one manual row anchor', () => {
  const codes = firmwareCodes();
  assert.equal(codes.length, 41);
  const app = read('app.js');
  assert.deepEqual(objectKeys(app, 'FAULT_TITLES'), codes);
  assert.deepEqual(objectKeys(app, 'FAULT_DESCRIPTIONS'), codes);
  const html = read(MANUAL);
  for (const code of codes) {
    const hits = html.match(new RegExp(`<tr id="E${code}">\\s*<td><strong>E${code}</strong></td>`, 'g')) || [];
    assert.equal(hits.length, 1, `manual row anchor for E${code}`);
  }
  assert.equal((html.match(/<tr id="E\d{3}"/g) || []).length, codes.length);   // no stray anchors
  assert.match(html, /<h1 id="danh-sach-ma-loi">/);                                  // fallback target for an unknown code
  assert.match(html, /tr:target td\{[^}]*background/);                               // the targeted row is highlighted
  assert.match(html, /tr\[id\]\{scroll-margin-top/);                                 // and not hidden under the sticky toolbar
});

test('popup link: new tab, only known codes get a row anchor, popup stays open while it is touched', () => {
  const app = read('app.js');
  assert.match(app, /detail\.className = 'faultPopupLink'/);
  assert.match(app, /detail\.target = '_blank';\s*detail\.rel = 'noopener';/);
  assert.match(app, /detail\.href = faultManualHref\(code\)/);
  assert.match(app, /\$\('faultPopup'\)\.addEventListener\('pointerdown'/);
  assert.match(app, /\$\('faultPopup'\)\.addEventListener\('click', \(event\) => event\.stopPropagation\(\)\)/);
  // run the real href builder
  const titles = app.slice(app.indexOf('const FAULT_TITLES = {'), app.indexOf('\n  };', app.indexOf('const FAULT_TITLES = {')) + 5);
  const fn = app.slice(app.indexOf('const FAULT_MANUAL_URL'), app.indexOf('  let faultPopupTimer'));
  const ctx = {};
  vm.runInNewContext(`${titles}\n${fn}\nthis.href = faultManualHref;`, ctx);
  assert.equal(ctx.href(115), './docs/MAYAP_Huong_dan_van_hanh_A5_v1.3_E503.html#E115');
  assert.equal(ctx.href(503), './docs/MAYAP_Huong_dan_van_hanh_A5_v1.3_E503.html#E503');
  assert.equal(ctx.href(999), './docs/MAYAP_Huong_dan_van_hanh_A5_v1.3_E503.html#danh-sach-ma-loi');
  assert.equal(ctx.href('constructor'), './docs/MAYAP_Huong_dan_van_hanh_A5_v1.3_E503.html#danh-sach-ma-loi');   // no prototype keys
});

test('the manual page is published and cached as a document of its own, never as the app shell', () => {
  assert.match(read('tools/build_web_assets.py'), /"docs\/MAYAP_Huong_dan_van_hanh_A5_v1\.3_E503\.html"/);
  const sw = read('sw.js');
  assert.match(sw, /OPTIONAL_SHELL = \[[^\]]*'\.\/docs\/MAYAP_Huong_dan_van_hanh_A5_v1\.3_E503\.html'/);
  assert.ok(sw.indexOf('docsPageResponse(event)') > 0 && sw.indexOf('docsPageResponse(event)') < sw.indexOf('event.respondWith(navigationResponse(event))'),
    'the docs page is answered before the generic navigation handler');
  assert.match(sw, /async function docsPageResponse[\s\S]*cache\.put\(event\.request,/);              // cached under its own URL
});

test('Thanh nhiet tile shows the heater power relay state', () => {
  const app = read('app.js');
  assert.match(app, /updateOutput\('outputHeater', bool\(runtime\.heaterOn\), 'RELAY BẬT', 'RELAY TẮT'\)/);
  assert.match(read('index.html'), /id="outputHeater">RELAY TẮT</);
});

test('login page introduces the smart thermal feature', () => {
  const html = read('index.html');
  assert.match(html, /<section class="smartIntro"[^>]*>[\s\S]*Nhiệt thông minh[\s\S]*<\/section>/);
  assert.ok(html.indexOf('class="smartIntro"') < html.indexOf('class="dashboardDemo"'));
  assert.match(read('landing.css'), /\.smartIntro\{/);
});
