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
    await page.locator('#notesPanel button[aria-label="Đóng ghi chú"]').click();   // close the notes panel again
    assert.equal(await page.locator('#notesPanel').isVisible(), false);
    // No hamburger any more: the bottom tab bar is always there, full width and flush with the bottom edge of the screen.
    assert.equal(await page.locator('#navToggle').count(), 0);
    const geometry = () => page.evaluate(() => {
      const nav = document.querySelector('#mainNav').getBoundingClientRect(), main = document.querySelector('.main');
      const save = [...document.querySelectorAll('button')].find(b => /Lưu thông số/.test(b.textContent) && b.offsetParent);
      return { vh: innerHeight, vw: innerWidth, navTop: nav.top, navBottom: nav.bottom, navLeft: nav.left, navRight: nav.right,
        scrollable: main.scrollHeight > main.clientHeight + 1, saveBottom: save ? save.getBoundingClientRect().bottom : null };
    });
    // Tall phone (390x844): the bar is flush at the bottom, the page fits and does not scroll.
    let g = await geometry();
    assert.equal(await page.locator('#mainNav').isVisible(), true);
    assert.equal(Math.round(g.navBottom), g.vh);
    assert.equal(Math.round(g.navLeft), 0);
    assert.equal(Math.round(g.navRight), g.vw);
    assert.equal(g.scrollable, false);
    // Short phone (390x560): the content cannot fit even fully compacted, so the page scrolls and the save button can be reached above the bar.
    await page.setViewportSize({ width: 390, height: 560 });
    assert.equal(await page.locator('#mainNav').isVisible(), true);
    g = await geometry();
    assert.equal(Math.round(g.navBottom), g.vh);
    assert.equal(g.scrollable, true);
    await page.locator('.main').evaluate(el => { el.scrollTop = el.scrollHeight; });
    await page.waitForTimeout(150);
    g = await geometry();
    assert.ok(g.saveBottom !== null && g.saveBottom <= g.navTop, 'save button is reachable above the tab bar');
    await page.locator('.nav button[data-page="batch"]').click();
    assert.equal(await page.evaluate(() => document.body.dataset.page), 'batch');
    await page.locator('.nav button[data-page="settings"]').click();
    assert.equal(await page.evaluate(() => document.body.dataset.page), 'settings');
    // Back to a tall screen: still the same bar, no scrolling needed on the device page.
    await page.locator('.nav button[data-page="device"]').click();
    await page.setViewportSize({ width: 390, height: 844 });
    g = await geometry();
    assert.equal(Math.round(g.navBottom), g.vh);
    assert.equal(g.scrollable, false);
    await page.setViewportSize({ width: 390, height: 560 });
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
