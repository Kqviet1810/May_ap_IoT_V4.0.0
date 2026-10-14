// Same-origin Cloudflare Static Assets and account/API configuration.
(() => {
  'use strict';
  try {
    localStorage.removeItem('mayap.web.v10.mqtt.private');
    localStorage.removeItem('mayap.web.v10.devices');
  } catch (_) {}
  window.MAYAP_WEB_CONFIG = Object.freeze({
    cloudApiBase: location.origin,
    staleAfterMs: 20000, offlineAfterMs: 30000, slowNoteAfterMs: 6000,
    commandTimeoutMs: 20000, configTimeoutMs: 30000,
    keepaliveSeconds: 30, reconnectPeriodMs: 3000, connectTimeoutMs: 10000,
    sessionTtlMs: 45000, sessionRefreshMs: 3000,
  });
})();
