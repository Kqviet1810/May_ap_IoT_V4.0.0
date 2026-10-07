#pragma once

#include "config.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <string.h>

// ============================================================================
// MAYAP DEVICE IDENTITY - FRESH INSTALL
//
// Khong con legacy firmware/device secret migration.
// Moi ESP32 lan dau chay se tu sinh device_key 256-bit, luu trong NVS va dung
// lai o cac lan boot/Upload firmware sau. Upload .ino binh thuong KHONG xoa NVS.
// ============================================================================

enum class MayapProvisioningState : uint8_t {
  Syncing = 0,
  CloudOffline,
  TlsError,
  ServerDenied,
  KeyMismatch,
  CloudError,
};

namespace MayapDeviceIdentityInternal {
static char activeKey[65] = "";
static char commandKey[65] = "";
static char mqttKey[65] = "";
static char mqttHost[64] = "";
static uint16_t mqttPort = 0U;
static char webPin[9] = "";
static bool webPinConfigured = false;
static volatile uint8_t provisioningState =
    static_cast<uint8_t>(MayapProvisioningState::Syncing);

inline void randomHex(char *out, size_t bytes) {
  static const char HEX_DIGITS[] = "0123456789abcdef";
  uint8_t data[32];
  if (!out) return;
  if (bytes > sizeof(data)) bytes = sizeof(data);
  esp_fill_random(data, bytes);
  for (size_t i = 0; i < bytes; ++i) {
    out[i * 2U] = HEX_DIGITS[data[i] >> 4U];
    out[i * 2U + 1U] = HEX_DIGITS[data[i] & 0x0FU];
  }
  out[bytes * 2U] = '\0';
}
}

inline void mayapSetProvisioningState(MayapProvisioningState state) {
  __atomic_store_n(&MayapDeviceIdentityInternal::provisioningState,
                   static_cast<uint8_t>(state), __ATOMIC_RELEASE);
}

inline MayapProvisioningState mayapProvisioningState() {
  return static_cast<MayapProvisioningState>(__atomic_load_n(
      &MayapDeviceIdentityInternal::provisioningState, __ATOMIC_ACQUIRE));
}

inline const char *mayapProvisioningStateText() {
  switch (mayapProvisioningState()) {
    case MayapProvisioningState::CloudOffline: return "CLOUD OFF";
    case MayapProvisioningState::TlsError: return "TLS ERROR";
    case MayapProvisioningState::ServerDenied: return "SERVER 403";
    case MayapProvisioningState::KeyMismatch: return "KEY ERROR";
    case MayapProvisioningState::CloudError: return "CLOUD ERROR";
    default: return "DANG DONG BO";
  }
}

inline void mayapDeviceIdentityBegin() {
  using namespace MayapDeviceIdentityInternal;
  activeKey[0] = '\0';
  commandKey[0] = '\0';
  mqttKey[0] = '\0';
  mqttHost[0] = '\0';
  mqttPort = 0U;
  webPin[0] = '\0';
  webPinConfigured = false;
  mayapSetProvisioningState(MayapProvisioningState::Syncing);

  Preferences prefs;
  if (!prefs.begin("mayap-id", false)) return;

  const String storedPin = prefs.getString("web-pin", "");
  if (storedPin.length() >= 4U && storedPin.length() < sizeof(webPin)) {
    strlcpy(webPin, storedPin.c_str(), sizeof(webPin));
    webPinConfigured = true;
  } else {
    webPinConfigured = prefs.getBool("web-pin-set", false);
  }

  const String storedCommandKey = prefs.getString("command-key", "");
  if (storedCommandKey.length() == 64U) {
    strlcpy(commandKey, storedCommandKey.c_str(), sizeof(commandKey));
  }

  const String storedMqttKey = prefs.getString("mqtt-key", "");
  if (storedMqttKey.length() == 64U) {
    strlcpy(mqttKey, storedMqttKey.c_str(), sizeof(mqttKey));
  }

  const String storedMqttHost = prefs.getString("mqtt-host", "");
  if (storedMqttHost.length() > 0U && storedMqttHost.length() < sizeof(mqttHost)) {
    strlcpy(mqttHost, storedMqttHost.c_str(), sizeof(mqttHost));
    mqttPort = prefs.getUShort("mqtt-port", 0U);
  }

  const String storedKey = prefs.getString("device-key", "");
  if (storedKey.length() >= 32U && storedKey.length() < sizeof(activeKey)) {
    strlcpy(activeKey, storedKey.c_str(), sizeof(activeKey));
  } else {
    // Fresh install: khoa rieng cho tung may, khong hard-code va khong dung
    // chung giua cac thiet bi.
    randomHex(activeKey, 32U);
    if (prefs.putString("device-key", activeKey) == 0U) {
      // Khong cho phep tiep tuc voi mot khoa chi ton tai trong RAM: neu reboot
      // se sinh khoa khac va cloud mat dong bo. De rong de fail closed.
      activeKey[0] = '\0';
    }
  }

  // Don dep marker migration cu neu NVS tung chay ban thu nghiem hardening.
  prefs.remove("legacy-done");
  prefs.end();
}

inline const char *mayapDeviceSecret() {
  return MayapDeviceIdentityInternal::activeKey;
}

inline bool mayapStoreCommandKey(const char *key) {
  using namespace MayapDeviceIdentityInternal;
  if (!key || strlen(key) != 64U) return false;
  for (size_t i = 0; i < 64U; ++i) {
    const char c = key[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
(c >= 'A' && c <= 'F'))) return false;
  }
  if (strcmp(commandKey, key) == 0) return true;  // register runs every boot: no needless flash write
  Preferences prefs;
  if (!prefs.begin("mayap-id", false)) return false;
  // NVS commits stall flash-cached code on BOTH cores; log the cost so a control
  // heartbeat trip can be correlated with provisioning (see [SUPERVISOR] TRIP).
  const int64_t writeStartUs = esp_timer_get_time();
  const bool ok = prefs.putString("command-key", key) > 0U;
  prefs.end();
  mayapSerialPrintf(false, "[NVS] command-key write %lluus\n",
                    static_cast<unsigned long long>(esp_timer_get_time() - writeStartUs));
  if (ok) strlcpy(commandKey, key, sizeof(commandKey));
  return ok;
}

// Per-device MQTT broker password issued by the account Worker at register.
inline bool mayapStoreMqttKey(const char *key) {
  using namespace MayapDeviceIdentityInternal;
  if (!key || strlen(key) != 64U) return false;
  for (size_t i = 0; i < 64U; ++i) {
    const char c = key[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  if (strcmp(mqttKey, key) == 0) return true;
  Preferences prefs;
  if (!prefs.begin("mayap-id", false)) return false;
  // NVS commits stall flash-cached code on BOTH cores; log the cost so a control
  // heartbeat trip can be correlated with provisioning (see [SUPERVISOR] TRIP).
  const int64_t writeStartUs = esp_timer_get_time();
  const bool ok = prefs.putString("mqtt-key", key) > 0U;
  prefs.end();
  mayapSerialPrintf(false, "[NVS] mqtt-key write %lluus\n",
                    static_cast<unsigned long long>(esp_timer_get_time() - writeStartUs));
  if (ok) strlcpy(mqttKey, key, sizeof(mqttKey));
  return ok;
}

inline const char *mayapMqttKey() {
  return MayapDeviceIdentityInternal::mqttKey;
}

// Broker endpoint issued by the account Worker together with the credential. Host names
// only (letters, digits, '.', '-'): it is later used as the TLS server name.
inline bool mayapStoreMqttEndpoint(const char *host, uint16_t port) {
  using namespace MayapDeviceIdentityInternal;
  if (!host || port == 0U) return false;
  const size_t length = strlen(host);
  if (length == 0U || length >= sizeof(mqttHost)) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = host[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-')) return false;
  }
  if (strcmp(mqttHost, host) == 0 && mqttPort == port) return true;  // register runs every boot
  Preferences prefs;
  if (!prefs.begin("mayap-id", false)) return false;
  const bool ok = prefs.putString("mqtt-host", host) > 0U && prefs.putUShort("mqtt-port", port) > 0U;
  prefs.end();
  if (ok) { strlcpy(mqttHost, host, sizeof(mqttHost)); mqttPort = port; }
  return ok;
}

inline const char *mayapMqttHost() { return MayapDeviceIdentityInternal::mqttHost; }
inline uint16_t mayapMqttPort() { return MayapDeviceIdentityInternal::mqttPort; }

inline const char *mayapCommandKey() {
  return MayapDeviceIdentityInternal::commandKey;
}

// Giu API de cloud_alert_link hien tai van bien dich; fresh-install khong bao
// gio dung legacy key nen rotateLegacyDeviceKey() se bo qua ngay.
inline bool mayapDeviceUsingLegacySecret() {
  return false;
}

inline void mayapGenerateDeviceSecret(char out[65]) {
  MayapDeviceIdentityInternal::randomHex(out, 32U);
}

inline bool mayapCommitDeviceSecret(const char *key) {
  using namespace MayapDeviceIdentityInternal;
  if (!key || strlen(key) < 32U || strlen(key) >= sizeof(activeKey)) return false;
  Preferences prefs;
  if (!prefs.begin("mayap-id", false)) return false;
  const bool ok = prefs.putString("device-key", key) > 0U;
  prefs.end();
  if (ok) strlcpy(activeKey, key, sizeof(activeKey));
  return ok;
}

inline void mayapMarkWebPinConfigured() {
  using namespace MayapDeviceIdentityInternal;
  Preferences prefs;
  if (!prefs.begin("mayap-id", false)) return;
  const bool ok = prefs.putBool("web-pin-set", true);
  prefs.end();
  if (ok) webPinConfigured = true;
}

inline void mayapStoreWebPin(const char *pin) {
  using namespace MayapDeviceIdentityInternal;
  if (!pin || strlen(pin) < 4U || strlen(pin) >= sizeof(webPin)) return;
  Preferences prefs;
  if (!prefs.begin("mayap-id", false)) return;
  const bool ok = prefs.putString("web-pin", pin) > 0U;
  if (ok) prefs.putBool("web-pin-set", true);
  prefs.end();
  if (ok) {
    strlcpy(webPin, pin, sizeof(webPin));
    webPinConfigured = true;
  }
}

inline const char *mayapWebPinText() {
  using namespace MayapDeviceIdentityInternal;
  if (webPin[0]) return webPin;
  return webPinConfigured ? "DUNG PIN CU" : mayapProvisioningStateText();
}
