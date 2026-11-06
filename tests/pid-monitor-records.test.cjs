'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { webcrypto } = require('node:crypto');
const protocol = require('../protocol_v2.js');

function loadBrowserEnv(overrides = {}, initialStorage = {}) {
  const source = fs.readFileSync(require.resolve('../app.js'), 'utf8').replace(/  init\(\);\s*\}\)\(\);\s*$/, `
    Object.assign(window.hooks, {
      state, pidMon, pmStorage, pmRecorder, pmReplay, pmCharts,
      pmUpdateRecordCount, pmValues, PM_ITEMS, pmIngestLog, pmCsv,
      drawPidMonitorCharts, drawPmChart
    });
  })();`);

  let now = 0, wall = Date.now(), timerId = 0;
  class BrowserDate extends Date { static now() { return wall; } }
  const timers = new Map(), elements = new Map(), events = new Map();
  const storage = new Map(Object.entries(initialStorage));
  const window = {
    hooks: {},
    addEventListener(name, fn) { events.set(name, fn); },
    MayapProtocolV2: protocol,
    MAYAP_WEB_CONFIG: { cloudApiBase: 'https://test.invalid', ...overrides }
  };

  const document = {
    hidden: false,
    body: { dataset: { page: 'settings' } },
    addEventListener(name, fn) { events.set(name, fn); },
    getElementById: (id) => elements.get(id) || null,
    querySelectorAll: (sel) => []
  };

  const context = {
    window, document, Date: BrowserDate, crypto: webcrypto, URL, URLSearchParams, TextEncoder, AbortController,
    localStorage: {
      getItem: (key) => storage.get(key) || null,
      setItem: (key, value) => storage.set(key, String(value)),
      removeItem: (key) => storage.delete(key)
    },
    console,
    performance: { now: () => now },
    setTimeout(fn, delay) { const id = ++timerId; timers.set(id, { fn, delay, at: wall + delay }); return id; },
    clearTimeout: (id) => timers.delete(id),
    setInterval(fn, delay) { const id = ++timerId; timers.set(id, { fn, delay, at: wall + delay, interval: true }); return id; },
    clearInterval: (id) => timers.delete(id),
    requestAnimationFrame: (fn) => setTimeout(fn, 16)
  };

  vm.runInNewContext(source, context);
  return { ...window.hooks, elements, storage, timers, elapse(ms) { wall += ms; now += ms; } };
}

test('UI elements for 24h recording, replay, zoom toolbar and history modal exist in index.html', () => {
  const html = fs.readFileSync(require.resolve('../index.html'), 'utf8');
  for (const id of [
    'pmRecordBtn', 'pmRecordBtnText', 'pmRecDot',
    'pmReplayBtn', 'pmHistoryBtn', 'pmRecordCount',
    'pmReplayBanner', 'pmReplayTitle', 'pmReplayTimer', 'pmReplayExitBtn', 'pmReplayScrubber',
    'pmReplayPlayBtn', 'pmReplayCurrentTime', 'pmReplayTotalTime',
    'pmHistoryDialog', 'pmHistoryTitle', 'pmHistoryList', 'pmHistoryClose', 'pmHistoryClearAll',
    'pmTempChart', 'pmPowerChart', 'pmDownload'
  ]) {
    assert.match(html, new RegExp(`id="${id}"`), `Missing required element id="${id}"`);
  }

  // Zoom toolbars for both charts exist
  assert.match(html, /data-chart="temp"/);
  assert.match(html, /data-chart="power"/);
  for (const action of ['in', 'out', '1h', '6h', '24h', 'reset']) {
    assert.match(html, new RegExp(`data-action="${action}"`));
  }
});

test('styles.css contains styling for chart toolbar, replay banner, recording dot and history list', () => {
  const css = fs.readFileSync(require.resolve('../styles.css'), 'utf8');
  for (const sel of [
    '.pmChartContainer', '.pmChartHeader', '.pmChartToolbar',
    '.pmRecordBtn', '.isRecording', '.pmRecDot',
    '.pmReplayBanner', '.pmReplayHead', '.pmReplayBadge', '.pmTimerText',
    '.pmReplayScrubberWrap', '.pmSlider', '.pmReplayControls',
    '.pmHistoryList', '.pmHistoryCard'
  ]) {
    assert.ok(css.includes(sel), `Missing CSS selector ${sel}`);
  }
});

test('pmStorage enforces FIFO with maximum of 5 records', () => {
  const env = loadBrowserEnv();
  const { pmStorage } = env;

  // Save 5 records
  for (let i = 1; i <= 5; ++i) {
    const ok = pmStorage.save({
      id: `rec_${i}`,
      name: `Bản ghi ${i}`,
      deviceId: 'MAP-TEST',
      startedAt: 1000 * i,
      samples: [{ t: 1000 * i, pv: 37.5, sp: 37.5, req: 10, act: 10 }]
    });
    assert.equal(ok, true);
  }

  let list = pmStorage.list('MAP-TEST');
  assert.equal(list.length, 5);
  assert.equal(list[0].id, 'rec_1');
  assert.equal(list[4].id, 'rec_5');

  // Saving the 6th record drops the oldest (rec_1) and keeps exactly 5 records
  pmStorage.save({
    id: 'rec_6',
    name: 'Bản ghi 6',
    deviceId: 'MAP-TEST',
    startedAt: 6000,
    samples: [{ t: 6000, pv: 37.5, sp: 37.5, req: 12, act: 12 }]
  });

  list = pmStorage.list('MAP-TEST');
  assert.equal(list.length, 5);
  assert.equal(list[0].id, 'rec_2');
  assert.equal(list[4].id, 'rec_6');

  // Deleting a record decreases count
  pmStorage.delete('rec_2');
  list = pmStorage.list('MAP-TEST');
  assert.equal(list.length, 4);
});

test('pmRecorder starts, accumulates samples, and stops with valid record', () => {
  const env = loadBrowserEnv();
  const { pmRecorder, pmStorage } = env;

  pmRecorder.start({ id: 'MAP-DEVICE-01' });
  assert.equal(pmRecorder.active, true);
  assert.equal(pmRecorder.deviceId, 'MAP-DEVICE-01');

  // Ingest samples
  pmRecorder.ingest({ t: 10000, pv: 37.50, sp: 37.50, req: 20, act: 19, vent: 0 });
  pmRecorder.ingest({ t: 20000, pv: 37.52, sp: 37.50, req: 15, act: 16, vent: 1, ventWhy: 1 });
  assert.equal(pmRecorder.samples.length, 2);

  // Stop recording
  pmRecorder.stop();
  assert.equal(pmRecorder.active, false);

  const saved = pmStorage.list('MAP-DEVICE-01');
  assert.ok(saved.length >= 1);
  assert.equal(saved[saved.length - 1].sampleCount, 2);
});

test('pmReplay initializes 5-minute timeout and can be stopped', () => {
  const env = loadBrowserEnv();
  const { pmReplay } = env;

  const mockRecord = {
    id: 'rec_mock',
    name: 'Bản ghi test',
    deviceId: 'MAP-01',
    samples: [
      { t: 1000, pv: 37.50, sp: 37.50, req: 10, act: 10 },
      { t: 2000, pv: 37.52, sp: 37.50, req: 8, act: 9 }
    ]
  };

  pmReplay.start(mockRecord);
  assert.equal(pmReplay.active, true);
  assert.equal(pmReplay.timeoutSec, 300); // exactly 5 minutes (300 seconds)

  // Exit replay
  pmReplay.stop();
  assert.equal(pmReplay.active, false);
  assert.equal(pmReplay.record, null);
});

test('pmCharts maintains independent zoom states for temp and power charts', () => {
  const env = loadBrowserEnv();
  const { pmCharts } = env;

  assert.ok(pmCharts.temp);
  assert.ok(pmCharts.power);

  // Changing temp chart zoom does not alter power chart zoom
  pmCharts.temp.zLo = 0.2;
  pmCharts.temp.zHi = 0.8;
  assert.equal(pmCharts.temp.zLo, 0.2);
  assert.equal(pmCharts.power.zLo, 0);
  assert.equal(pmCharts.power.zHi, 1);
});
