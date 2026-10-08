'use strict';
const CACHE = 'mayap-web-v1.1.9';

// The canonical installed-app entry is the Service Worker scope root. Android
// may keep an older /index.html start URL for a while after an app update, so
// both forms are accepted, but only the canonical root is required for install.
const REQUIRED_SHELL = [
  './', './styles.css', './landing.css', './account.js?v=1.1.9', './config.js?v=1.1.9',
  './app.js?v=1.1.9', './protocol_v2.js?v=1.1.9', './push.js?v=1.1.9',
  './manifest.webmanifest', './vendor/jsQR.min.js', './vendor/mqtt.min.js', './mqtt_transport.js?v=1.1.9',
  './notes.js?v=1.1.9', './notes.css', './icons/icon-192.png', './icons/icon-512.png'
];
const OPTIONAL_SHELL = [
  './index.html', './docs/MAYAP_Huong_dan_van_hanh_A5_v1.3_E503.pdf', './icons/badge-72.png'
];

self.addEventListener('install', (event) => {
  event.waitUntil((async () => {
    const cache = await caches.open(CACHE);
    // Do not activate a new worker unless the complete runtime shell exists.
    // This prevents a first successful network visit from installing a worker
    // that cannot reopen the Android PWA on the next launch.
    await cache.addAll(REQUIRED_SHELL);
    await Promise.all(OPTIONAL_SHELL.map((url) => cache.add(url).catch(() => {})));
    await self.skipWaiting();
  })());
});

self.addEventListener('activate', (event) => {
  event.waitUntil(caches.keys().then((keys) => Promise.all(
    keys.filter((key) => key !== CACHE).map((key) => caches.delete(key))
  )));
  self.clients.claim();
});

const CANONICAL_SHELL_URL = new URL('./', self.location.href).href;
const LEGACY_SHELL_URL = new URL('./index.html', self.location.href).href;

function offlineDocument() {
  return new Response(
    '<!doctype html><html lang="vi"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">' +
    '<title>MAYAP</title><body style="font-family:system-ui;padding:32px"><h1>MAYAP</h1>' +
    '<p>Chưa có kết nối mạng và bản ứng dụng cục bộ chưa sẵn sàng. Hãy kết nối mạng rồi mở lại.</p></body></html>',
    { status: 503, headers: { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' } }
  );
}

async function cachedNavigationShell(cache) {
  if (!cache) return undefined;
  return (await cache.match(CANONICAL_SHELL_URL)) ||
         (await cache.match(LEGACY_SHELL_URL));
}

// Navigation has its own fallback. It never returns Response.error(), because
// Chromium renders that as ERR_FAILED when an installed Android PWA is reopened.
// Query/deep-link URLs deliberately fall back to the canonical cached app shell.
function navigationResponse(event) {
  const cacheReady = caches.open(CACHE).catch(() => null);
  const cachedReady = cacheReady.then((cache) => cachedNavigationShell(cache)).catch(() => undefined);
  const refresh = fetch(event.request).then(async (response) => {
    if (!response.ok) return (await cachedReady) || response;
    const cache = await cacheReady;
    if (cache) {
      try { await cache.put(CANONICAL_SHELL_URL, response.clone()); } catch (_) {}
    }
    return response;
  }).catch(async () => (await cachedReady) || offlineDocument());

  event.waitUntil(refresh.then(() => {}));
  return cachedReady.then(async (cached) => {
    if (!cached) return refresh;
    let timer;
    try {
      return await Promise.race([
        refresh,
        new Promise((resolve) => { timer = setTimeout(() => resolve(cached), 1000); }),
      ]);
    } finally {
      clearTimeout(timer);
    }
  });
}

// A warm static shell must not wait indefinitely for a weak/mobile connection.
// Prefer fresh code, then fall back to THIS release's cache after 250 ms.
function networkFirstCore(event) {
  const cacheReady = caches.open(CACHE).catch(() => null);
  const cachedReady = cacheReady.then((cache) => cache?.match(event.request)).catch(() => undefined);
  const refresh = fetch(event.request).then(async (response) => {
    if (!response.ok) return (await cachedReady) || response;
    try { await (await cacheReady)?.put(event.request, response.clone()); } catch (_) {}
    return response;
  }).catch(async () => (await cachedReady) || Response.error());
  event.waitUntil(refresh.then(() => {}));
  return cachedReady.then(async (cached) => {
    if (!cached) return refresh;
    let timer;
    try {
      return await Promise.race([
        refresh,
        new Promise((resolve) => { timer = setTimeout(() => resolve(cached), 250); }),
      ]);
    } finally {
      clearTimeout(timer);
    }
  });
}

self.addEventListener('fetch', (event) => {
  if (event.request.method !== 'GET') return;
  const url = new URL(event.request.url);
  if (url.origin !== self.location.origin ||
      url.pathname.startsWith('/api/') ||
      url.pathname.startsWith('/auth/')) return;

  const accept = event.request.headers?.get?.('accept') || '';
  const isNavigation = event.request.mode === 'navigate' ||
      event.request.destination === 'document' || accept.includes('text/html');
  if (isNavigation) {
    event.respondWith(navigationResponse(event));
    return;
  }

  // Pinned bundle reuses the current release cache.
  if (/\/vendor\/(?:jsQR|mqtt)\.min\.js$/.test(url.pathname)) {
    event.respondWith(caches.open(CACHE).then(async (cache) => {
      const cached = await cache.match(event.request);
      if (cached) return cached;
      const response = await fetch(event.request);
      if (response.ok) await cache.put(event.request, response.clone());
      return response;
    }));
    return;
  }

  const isCoreAsset = /\.(?:js|css)$/.test(url.pathname);
  if (isCoreAsset) {
    event.respondWith(networkFirstCore(event));
    return;
  }

  event.respondWith(caches.match(event.request).then((cached) => cached || fetch(event.request).then((response) => {
    const copy = response.clone();
    caches.open(CACHE).then((cache) => cache.put(event.request, copy));
    return response;
  })));
});

// ------------------------------- Web Push -----------------------------------
// Nhan push tu Cloudflare Worker (thay Telegram) va hien notification that su
// cua he dieu hanh - hoat dong ke ca khi khong co tab nao cua website dang mo.
self.addEventListener('push', (event) => {
  let data = {};
  try { data = event.data ? event.data.json() : {}; } catch (_) {
    data = { title: 'MAYAP', body: event.data ? event.data.text() : '' };
  }

  const title = data.title || 'MAYAP';
  const options = {
    body: data.body || '',
    icon: data.icon || './icons/icon-192.png',
    badge: data.badge || './icons/badge-72.png',
    data: data.data || {},
    tag: data.data?.alarmType ? `mayap-${data.data.deviceId || ''}-${data.data.alarmType}-${data.data.state || 'active'}` : undefined,
    // Canh bao ACTIVE thay the ban cu cung loai (khong xep chong nhieu thong
    // bao "van con loi X" giong nhau); tin RESOLVED luon la thong bao rieng
    // (khong ghi de) de nguoi dung con thay ro da tung co canh bao.
    renotify: data.data?.state === 'active',
  };

  const shown=self.registration.showNotification(title, options);
  if(data.data?.eventId) shown.then(()=>console.info(`[push] displayed event=${data.data.eventId} age_ms=${Date.now()-Number(data.data.ts || Date.now())}`)).catch(()=>{});
  event.waitUntil(shown);
});

self.addEventListener('notificationclick', (event) => {
  event.notification.close();
  const targetUrl = new URL(event.notification.data?.url || './', self.location.href).href;
  event.waitUntil(
    self.clients.matchAll({ type: 'window', includeUncontrolled: true }).then((clients) => {
      for (const client of clients) {
        if (client.url === targetUrl && 'focus' in client) return client.focus();
      }
      if (self.clients.openWindow) return self.clients.openWindow(targetUrl);
      return undefined;
    })
  );
});

self.addEventListener('pushsubscriptionchange', (event) => {
  // Trinh duyet tu xoay subscription (het han/thu hoi khoa) - trang web se tu
  // phat hien va dang ky lai o lan mo tiep theo qua MayapPush.getState() trong
  // push.js, khong can xu ly gi them o day.
});
