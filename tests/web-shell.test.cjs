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

test('small screens: tab bar is a hamburger-opened panel (no content hidden); the notes bubble stays put on every tab', () => {
  const fs2 = require('node:fs');
  const html = fs2.readFileSync('index.html', 'utf8'), css = fs2.readFileSync('styles.css', 'utf8');
  const app = fs2.readFileSync('app.js', 'utf8'), notes = fs2.readFileSync('notes.js', 'utf8');
  assert.match(html, /id="navToggle"[^>]*aria-controls="mainNav"|aria-controls="mainNav"[^>]*id="navToggle"/);
  assert.match(html, /<nav class="nav" id="mainNav">/);
  const mobile = css.slice(css.lastIndexOf('@media(max-width:800px),(max-height:500px){'));
  assert.match(mobile, /\.navToggle\{display:grid[^}]*position:fixed[^}]*left:/);          // fixed button on the left edge
  assert.match(mobile, /\.nav\{display:none;position:fixed/);                              // hidden until opened
  assert.match(mobile, /\.nav\.open\{display:grid\}/);
  assert.match(css, /\.navToggle\{display:none\}/);                                        // desktop keeps the sidebar
  assert.match(app, /navToggle\.addEventListener\('click'/);
  assert.match(app, /event\.key === 'Escape' && navPanel\.classList\.contains\('open'\)/);
  assert.match(app, /setNavOpen\(false\); showPage\(button\.dataset\.page\)/);              // choosing a tab closes the panel
  // notes bubble: no dodging of page content, no repositioning on scroll - only a drag moves it
  assert.doesNotMatch(notes, /obstacles|candidates|scrollFrame/);
  assert.match(notes, /bubble\.style\.top = `\$\{targetY\}px`/);
  assert.doesNotMatch(notes, /document\.addEventListener\('scroll'/);
});
