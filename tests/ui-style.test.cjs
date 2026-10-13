'use strict';
// "Hien thi" setting: Co ban (old look) vs Hien dai (new look, default). The modern CSS must be scoped so Co ban stays the old UI.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const read = (p) => fs.readFileSync(path.join(__dirname, '..', p), 'utf8');

test('settings expose Co ban / Hien dai and the choice is persisted', () => {
  const html = read('index.html'), app = read('app.js');
  assert.match(html, /<html[^>]*data-ui="modern"/);
  assert.match(html, /data-ui-choice="basic"[\s\S]*Cơ bản[\s\S]*data-ui-choice="modern"[\s\S]*Hiện đại/);
  assert.match(app, /UI_STYLE_STORAGE = 'mayap\.ui'/);
  assert.match(app, /localStorage\.getItem\(UI_STYLE_STORAGE\) === 'basic' \? 'basic' : 'modern'/);
  assert.match(app, /applyUiStyle\(getUiStylePreference\(\)\)/);
});

test('every modern-look rule is scoped to html[data-ui="modern"]', () => {
  const css = read('styles.css');
  const block = css.slice(css.indexOf('Web 1.1.16: kieu hien thi'));
  const selectors = [...block.matchAll(/(?:^|\})\s*([^{}@]+)\{/g)].map((m) => m[1].trim()).filter((s) => !s.startsWith('/*') && !s.startsWith('.uiStyle') && !/^\.(settingsTools,|searchHidden|srOnly)/.test(s));
  assert.ok(selectors.length > 10);
  for (const group of selectors) for (const sel of group.split(',')) assert.match(sel.trim(), /^html\[data-ui="modern"\]/, sel);
});

test('settings tools (search + group jump) exist and only show in the modern look', () => {
  const html = read('index.html'), css = read('styles.css'), app = read('app.js');
  assert.match(html, /id="settingsSearch"/);
  for (const g of ['connection', 'alerts', 'batch', 'ventilation', 'temperature', 'advanced', 'support']) {
    assert.match(html, new RegExp('data-jump="settings-' + g + '"'));
    assert.match(html, new RegExp('id="settings-' + g + '"'));
  }
  assert.match(css, /\.settingsTools,\.settingsEmpty\{display:none\}/);
  assert.match(css, /html\[data-ui="modern"\] \.settingsTools\{display:grid/);
  assert.match(app, /function bindSettingsTools\(\)/);
  assert.doesNotMatch(html, /uiStyleHint/);
});
