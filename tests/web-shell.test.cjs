const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

function worker(fetch, stored = {}) {
  const events = new Map(), timers = new Map(), values = new Map(Object.entries(stored));
  const key = request => new URL(typeof request === 'string' ? request : request.url, 'https://web.test/sw.js').href;
  const cache = {
    match: async request => values.has(key(request)) ? new Response(values.get(key(request))) : undefined,
    put: async (request, response) => values.set(key(request), await response.text()),
    add: async request => {
      const response = await fetch(key(request));
      if (!response.ok) throw new Error('cache add failed');
      await cache.put(request, response.clone());
    },
    addAll: async requests => { for (const request of requests) await cache.add(request); },
  };
  const opened = [];
  const ctx = { self:{
      location:{origin:'https://web.test',href:'https://web.test/sw.js'},
      addEventListener:(name,fn)=>events.set(name,fn),
      skipWaiting:async()=>{},
      clients:{claim:()=>{}},
    },
    caches:{open:async name=> { opened.push(name); return cache; },match:async request=>cache.match(request),keys:async()=>[],delete:async()=>true},
    fetch, URL, Response,
    setTimeout:(fn,ms)=> { const id=timers.size+1; timers.set(id,{fn,ms}); return id; },
    clearTimeout:id=>timers.delete(id) };
  vm.runInNewContext(fs.readFileSync(require.resolve('../sw.js'),'utf8'),ctx);
  return { values, timers, opened,
    request(url, init = {}) {
      let response; const lifetime=[];
      events.get('fetch')({request:new Request(url, init), respondWith:p=> {response=p;}, waitUntil:p=>lifetime.push(p)});
      return {get response(){return response;}, lifetime};
    }
  };
}

test('slow static fetch falls back promptly and still refreshes the versioned shell', async () => {
  let complete;
  const w = worker(()=>new Promise(resolve=> {complete=resolve;}), {'https://web.test/app.js':'cached'});
  const request = w.request('https://web.test/app.js');
  await new Promise(setImmediate);
  assert.equal(w.timers.get(1).ms,250);
  w.timers.get(1).fn();
  assert.equal(await (await request.response).text(),'cached');
  complete(new Response('fresh'));
  await Promise.all(request.lifetime);
  assert.equal(w.values.get('https://web.test/app.js'),'fresh');
  const source = fs.readFileSync(require.resolve('../sw.js'),'utf8');
  const expectedCache = source.match(/const CACHE = '([^']+)'/)?.[1];
  assert.ok(expectedCache);
  assert.ok(w.opened.every(name=>name===expectedCache));
});

test('normal network remains fresh-first and cleans the fallback timer', async () => {
  const w = worker(async()=>new Response('fresh'), {'https://web.test/styles.css':'old'});
  const request = w.request('https://web.test/styles.css');
  assert.equal(await (await request.response).text(),'fresh');
  assert.equal(w.timers.size,0);
});

test('offline startup includes cached public config hardening; error pages do not replace code', async () => {
  const source = fs.readFileSync(require.resolve('../sw.js'),'utf8');
  const version=JSON.parse(fs.readFileSync(require.resolve('../release-manifest.json'),'utf8')).web;
  assert.ok(source.includes(`'./config.js?v=${version}'`));
  const w = worker(async()=> {throw new Error('offline');}, {'https://web.test/config.js':'public security wrapper'});
  assert.equal(await (await w.request('https://web.test/config.js').response).text(),'public security wrapper');
  const failed = worker(async()=>new Response('server error',{status:503}), {'https://web.test/app.js':'good code'});
  assert.equal(await (await failed.request('https://web.test/app.js').response).text(),'good code');
  assert.equal(failed.values.get('https://web.test/app.js'),'good code');
});

test('installed Android PWA uses the canonical scope root and stable manifest id', () => {
  const manifest=JSON.parse(fs.readFileSync(require.resolve('../manifest.webmanifest'),'utf8'));
  assert.equal(manifest.id,'./');
  assert.equal(manifest.start_url,'./');
  assert.equal(manifest.scope,'./');
  assert.notEqual(manifest.start_url,'./index.html');
});

test('installed PWA navigation reopens from canonical cached shell while offline', async () => {
  const w=worker(async()=>{throw new Error('offline');},{'https://web.test/':'cached app shell'});
  const request=w.request('https://web.test/index.html',{headers:{accept:'text/html'}});
  const response=await request.response;
  assert.equal(response.status,200);
  assert.equal(await response.text(),'cached app shell');
});

test('uncached offline navigation returns a real HTML response instead of ERR_FAILED', async () => {
  const w=worker(async()=>{throw new Error('offline');});
  const response=await w.request('https://web.test/',{headers:{accept:'text/html'}}).response;
  assert.equal(response.status,503);
  assert.match(await response.text(),/MAYAP/);
  assert.notEqual(response.type,'error');
});

test('fresh navigation refreshes the canonical cached entry', async () => {
  const w=worker(async()=>new Response('fresh app shell',{status:200,headers:{'Content-Type':'text/html'}}));
  const request=w.request('https://web.test/?device=MAP-1234567890AB',{headers:{accept:'text/html'}});
  assert.equal(await (await request.response).text(),'fresh app shell');
  await Promise.all(request.lifetime);
  assert.equal(w.values.get('https://web.test/'),'fresh app shell');
});

test('Cloud auth requests never enter shell cache and pinned QR scanner reuses the current release', async () => {
  let calls=0;
  const w = worker(async()=> {calls++; return new Response('network');}, {'https://web.test/vendor/jsQR.min.js':'pinned'});
  assert.equal(w.request('https://worker.test/api/device/realtime-session').response,undefined);
  assert.equal(w.request('https://web.test/api/account/session').response,undefined);
  assert.equal(w.request('https://web.test/auth/google/callback?code=secret').response,undefined);
  assert.equal(w.request('https://worker.test/config.js').response,undefined);
  assert.equal(await (await w.request('https://web.test/vendor/jsQR.min.js').response).text(),'pinned');
  assert.equal(calls,0);
});

test('UI/core scripts share one release-qualified asset set and cache',()=>{
 const html=fs.readFileSync(require.resolve('../index.html'),'utf8'),sw=fs.readFileSync(require.resolve('../sw.js'),'utf8');
 const version=JSON.parse(fs.readFileSync(require.resolve('../release-manifest.json'),'utf8')).web;
 assert.ok(sw.includes(`mayap-web-v${version}`));
 for(const name of ['app','notes','protocol_v2']){
  assert.ok(html.includes(`./${name}.js?v=${version}`));assert.ok(sw.includes(`./${name}.js?v=${version}`));
 }
});

test('phones: no hamburger - the bottom tab bar is always visible, full width, flush with the bottom edge; tall pages never scroll, short ones scroll', () => {
  const fs2 = require('node:fs');
  const html = fs2.readFileSync('index.html', 'utf8'), css = fs2.readFileSync('styles.css', 'utf8').replace(/\r\n/g, '\n');
  const app = fs2.readFileSync('app.js', 'utf8'), notes = fs2.readFileSync('notes.js', 'utf8');
  assert.doesNotMatch(html, /navToggle/);                                                 // the 3-line button is gone
  assert.doesNotMatch(css, /navToggle/);
  assert.doesNotMatch(app, /navToggle|setNavOpen/);
  assert.match(html, /<nav class="nav" id="mainNav">/);
  const bar = css.slice(css.indexOf('@media(max-width:800px),(max-height:500px){\n  :root{--navH:'));
  const barBlock = bar.slice(0, bar.indexOf('\n}\n'));
  assert.match(barBlock, /\.sidebar\{position:fixed;inset:auto 0 0 0;[^}]*bottom:0;/);      // anchored to the very bottom edge
  assert.match(barBlock, /\.nav\{[^}]*width:100%;[^}]*padding:6px 8px var\(--safeBottom\);[^}]*border-radius:18px 18px 0 0/);   // full width, safe area INSIDE the bar
  assert.doesNotMatch(barBlock, /\.nav\{display:none/);
  // device / batch pages: the content area scrolls only when the content really does not fit
  assert.match(css, /body\[data-page="device"\] \.main,body\[data-page="batch"\] \.main\{height:100dvh;min-height:0;overflow-y:auto;overscroll-behavior-y:auto/);
  assert.match(css, /@media\(max-height:840px\)\{/);                                        // compaction ladder reaches XS-class phones (812 px)
  assert.match(notes, /bar && getComputedStyle\(bar\)\.position === 'fixed'/);                // the notes bubble never sits on the bar
  assert.match(app, /querySelectorAll\('\.nav button'\)\.forEach\(\(button\) => button\.addEventListener\('click', \(\) => showPage/);
  // notes bubble: no dodging of page content, no repositioning on scroll - only a drag moves it
  assert.doesNotMatch(notes, /obstacles|candidates|scrollFrame/);
  assert.match(notes, /bubble\.style\.top = `\$\{targetY\}px`/);
  assert.doesNotMatch(notes, /document\.addEventListener\('scroll'/);
});
