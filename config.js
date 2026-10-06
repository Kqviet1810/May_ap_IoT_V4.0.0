// Same-origin Cloudflare Static Assets and account/API configuration.
(() => {
  'use strict';
  try {
    localStorage.removeItem('mayap.web.v10.mqtt.private');
    localStorage.removeItem('mayap.web.v10.devices');
  } catch (_) {}
  window.MAYAP_WEB_CONFIG = Object.freeze({
    cloudApiBase: location.origin,
    staleAfterMs: 8000, offlineAfterMs: 30000,
    commandTimeoutMs: 10000, configTimeoutMs: 15000,
  });
})();
