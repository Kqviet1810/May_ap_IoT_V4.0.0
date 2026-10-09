// Browser smoke for the V4 clean baseline. No realtime client is installed.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { chromium } = require(process.env.MAYAP_PLAYWRIGHT || '../cloudflare/node_modules/playwright');

const root = path.resolve(__dirname, '..');
const out = path.resolve(process.argv[2] || path.join(root, 'work', 'web-qa'));
fs.mkdirSync(out, { recursive: true });

async function main() {
  const browser = await chromium.launch({ headless: true });
  try {
    const context = await browser.newContext({ viewport: { width: 390, height: 844 },
      isMobile: true, hasTouch: true, serviceWorkers: 'block' });
    await context.addInitScript(() => {
      sessionStorage.setItem('mayap.account.session.v1', 'aa'.repeat(32));
    });
    const requested = [];
    await context.route('**/*', async route => {
      const url = new URL(route.request().url());
      requested.push(url.pathname);
      if (!url.pathname.startsWith('/api/')) return route.continue();
      if (url.pathname === '/api/account/session') return route.fulfill({ status: 200,
        contentType: 'application/json', body: JSON.stringify({ success: true,
          user: { sub: 'qa-sub', name: 'QA account' }, expiresAt: Date.now() + 86400000,
          devices: [{ device_id: 'MAP-1234567890AB', device_name: 'Máy ấp thử', role: 'owner' }] }) });
      if (url.pathname.endsWith('/notes')) return route.fulfill({ status: 200,
        contentType: 'application/json', body: JSON.stringify({ success: true, notes: [] }) });
      return route.fulfill({ status: 200, contentType: 'application/json',
        body: JSON.stringify({ success: true }) });
    });
    const page = await context.newPage(), errors = [];
    page.on('pageerror', e => errors.push(e.message));
    await page.goto('http://127.0.0.1:8765', { waitUntil: 'networkidle' });
    await page.locator('#deviceSelector').waitFor();
    assert.equal(await page.locator('script[src*="realtime_transport"]').count(), 0);
    assert.equal(requested.some(p => p.includes('realtime-session')), false);
    assert.equal(await page.locator('#notesBubble').count(), 1);
    await page.locator('#notesBubble').click();
    assert.equal(await page.locator('#notesPanel').isVisible(), true);
    // Small screen: the tab list is hidden behind the hamburger button and never covers content until opened.
    await page.locator('#notesPanel button[aria-label="Đóng ghi chú"]').click();   // close the notes panel again
    assert.equal(await page.locator('#notesPanel').isVisible(), false);
    assert.equal(await page.locator('#navToggle').isVisible(), true);
    assert.equal(await page.locator('#mainNav').isVisible(), false);
    await page.locator('#navToggle').click();
    assert.equal(await page.locator('#mainNav').isVisible(), true);
    assert.equal(await page.locator('#navToggle').getAttribute('aria-expanded'), 'true');
    await page.locator('.nav button[data-page="batch"]').click();
    assert.equal(await page.evaluate(() => document.body.dataset.page), 'batch');
    assert.equal(await page.locator('#mainNav').isVisible(), false);  // choosing a tab closes the list
    await page.locator('#navToggle').click();
    await page.locator('.nav button[data-page="settings"]').click();
    assert.equal(await page.evaluate(() => document.body.dataset.page), 'settings');
    // The notes bubble stays exactly where it is while the page scrolls.
    const bubbleTop = async () => page.locator('#notesBubble').evaluate(el => Math.round(el.getBoundingClientRect().top));
    const before = await bubbleTop();
    await page.evaluate(() => window.scrollTo(0, document.body.scrollHeight));
    await page.waitForTimeout(200);
    assert.equal(await bubbleTop(), before);
    await page.evaluate(() => window.scrollTo(0, 0));
    assert.deepEqual(errors, []);
    await page.screenshot({ path: path.join(out, 'v4-clean-web.png') });
    fs.writeFileSync(path.join(out, 'web-browser-qa.json'), JSON.stringify({
      passed: true, pages: ['device', 'batch', 'settings'], oldRealtimeRequests: 0,
      browserErrors: errors }, null, 2));
    console.log('V4 clean Web browser smoke PASS');
    await context.close();
  } finally { await browser.close(); }
}

if (require.main === module) main().catch(e => { console.error(e); process.exitCode = 1; });
