#pragma once

#include "config.h"
#include "service_recovery.h"
#include "wifi_stable_state.h"
#include "wifi_power_policy.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_wifi.h>

// Wi-Fi duoc cach ly khoi task dieu khien. File nay chi duoc goi boi
// networkTask (tru mayapSetConnectivityMode/mayapGetNetworkStatus/
// mayapRequestWifiPortal/mayapCancelWifiPortal/mayapGetWifiPortalStatus,
// nhung tat ca deu dung atomic/co doc snapshot nen goi tu task nao cung an toan).
//
// Nguon goc cong 1 "Doi Wi-Fi": da gop toan bo logic tu sketch tham khao rieng
// MAYAP_WIFI_WEB_PHU_ONLY (AP MAYAP-XXXX + web cau hinh SSID/mat khau) thang
// vao day va chuyen tu kieu blocking (delay() trong vong doi ket noi) sang
// non-blocking de khong bao gio lam cham task dieu khien/HMI. Sketch tham
// khao rieng khong con ton tai trong repo; toan bo firmware gio chi gom
// 4 file .h (config/network_service/hmi/machine_control) + 1 file .ino.
namespace MayapNetworkInternal {

// Ban rieng cua file nay: machine_control.h co elapsedMs/timeReached trong
// namespace Mayap, hmi.h co ban global rieng, nhung network_service.h duoc
// include TRUOC ca hai trong .ino nen khong the dua vao chung - moi file
// tu chua ham nho nay de doc lap thu tu include.
inline uint32_t elapsedMs(uint32_t now, uint32_t then) {
  return static_cast<uint32_t>(now - then);
}
inline bool timeReached(uint32_t now, uint32_t target) {
  return static_cast<int32_t>(now - target) >= 0;
}

constexpr uint16_t DNS_PORT = 53;

static volatile uint8_t requestedMode =
    static_cast<uint8_t>(ConnectivityMode::Offline);
static volatile uint8_t publishedState =
    static_cast<uint8_t>(NetworkStateCode::Offline);
static volatile bool publishedConfigured = false;
static volatile bool publishedConnected = false;
static volatile int8_t publishedRssiDbm = -127;
static uint32_t publishedLocalIp = 0U;

// Raw connected is immediate I/O availability; association remains independent
// while RadioGate drains owners or memory pressure pauses Online services.
static volatile bool rawConnected = false;
static bool rawAssociated = false;
static volatile uint8_t rawState = static_cast<uint8_t>(NetworkStateCode::Offline);
static volatile int8_t rawRssiDbm = -127;
static MayapNetwork::StableWifiState stableWifi;

static bool radioActive = false;
static uint32_t connectionStartedAt = 0U;
// Mains-powered controller: only networkTask applies fixed awake STA policy.
static bool wifiPowerModeAppliedValid = false;
static MayapWifiPower::Mode wifiPowerModeApplied = MayapWifiPower::Mode::Performance;
// Backoff RIENG cua STA Wi-Fi, doc lap voi backoff cua MQTT (transaction_bridge.h)
// va Cloud Push (cloud_alert_link.h) - loi/reset o tang nao khong dung cham
// tang khac. Khong con dung 2 bien lastRetryAt/lastStartAttemptAt + hang so co
// dinh nhu truoc: moi that bai lien tiep se tu keo gian khoang cho ra thay vi
// dap WiFi.begin() moi 30s vinh vien khi mat mang keo dai.
static BackoffTimer staBackoff{};
static MayapRecovery::WifiRecovery deepPolicy;
enum class DeepPhase : uint8_t { Idle, Quiesce, OffWait, Isolated };
static DeepPhase deepPhase = DeepPhase::Idle;
static uint32_t deepPhaseAt = 0U;
static bool deepRequested = false;

// ------------------------- Thong tin dang nhap Wi-Fi ------------------------
// Doc/ghi tu networkTask. SSID/mat khau nap tu NVS (Preferences); neu chua
// tung luu, dung macro bien dich MAYAP_WIFI_SSID/PASSWORD lam gia tri mac dinh
// mot lan duy nhat de tuong thich firmware cu.
static Preferences wifiPrefs;
static char activeSsid[WIFI_PORTAL_SSID_MAX + 1U] = "";
static char activePassword[WIFI_PORTAL_PASSWORD_MAX + 1U] = "";
static bool credentialsLoaded = false;

inline bool credentialsConfigured() { return activeSsid[0] != '\0'; }

inline void loadCredentialsOnce() {
  if (credentialsLoaded) return;
  credentialsLoaded = true;
  wifiPrefs.begin("mayapwifi", false);
  String ssid = wifiPrefs.getString("ssid", "");
  String pass = wifiPrefs.getString("pass", "");
  if (ssid.isEmpty() && sizeof(NETWORK_WIFI_SSID) > 1U) {
    // Chua tung cau hinh qua HMI/portal: mo phong gia tri build-time mot lan,
    // luu lai de cac lan sau doc thang tu NVS.
    ssid = NETWORK_WIFI_SSID;
    pass = NETWORK_WIFI_PASSWORD;
    wifiPrefs.putString("ssid", ssid);
    wifiPrefs.putString("pass", pass);
  }
  snprintf(activeSsid, sizeof(activeSsid), "%s", ssid.c_str());
  snprintf(activePassword, sizeof(activePassword), "%s", pass.c_str());
}

inline bool saveCredentials(const char *ssid, const char *password) {
  if (!ssid || !ssid[0]) return false;
  if (strlen(ssid) > WIFI_PORTAL_SSID_MAX ||
      strlen(password ? password : "") > WIFI_PORTAL_PASSWORD_MAX) {
    return false;
  }
  wifiPrefs.putString("ssid", ssid);
  wifiPrefs.putString("pass", password ? password : "");
  snprintf(activeSsid, sizeof(activeSsid), "%s", ssid);
  snprintf(activePassword, sizeof(activePassword), "%s", password ? password : "");
  return true;
}

inline void publish(NetworkStateCode state, bool connected,
                    int8_t rssiDbm = -127) {
  const bool associated = WiFi.isConnected();
  connected = connected && associated;
  if (!connected && state == NetworkStateCode::Connected)
    state = NetworkStateCode::Connecting;
  const bool oldRaw = __atomic_load_n(&rawConnected, __ATOMIC_ACQUIRE);
  __atomic_store_n(&rawConnected, connected, __ATOMIC_RELEASE);
  __atomic_store_n(&rawState, static_cast<uint8_t>(state), __ATOMIC_RELEASE);
  __atomic_store_n(&rawRssiDbm, connected ? rssiDbm : -127, __ATOMIC_RELEASE);
  if (oldRaw != connected || rawAssociated != associated)
    mayapSerialPrintf(false, "[WIFI-RAW] associated=%u available=%u state=%u\n",
        associated, connected, static_cast<unsigned>(state));
  rawAssociated = associated;

  // Explicit OFFLINE/unconfigured is immediate. Involuntary loss is debounced.
  const bool forced = state == NetworkStateCode::Offline ||
                      state == NetworkStateCode::NotConfigured;
  if (forced) stableWifi.reset();
  const bool stable = forced ? false : stableWifi.update(millis(), associated);
  const bool oldStable = __atomic_load_n(&publishedConnected, __ATOMIC_ACQUIRE);
  if (stable != oldStable)
    mayapSerialPrintf(false, "[WIFI-STABLE] connected=%u raw=%u\n", stable, connected);
  // Keep the last useful RSSI/IP during the short down grace, never for I/O.
  if (!stable || associated) {
    __atomic_store_n(&publishedLocalIp,
        stable ? static_cast<uint32_t>(WiFi.localIP()) : 0U, __ATOMIC_RELEASE);
    const int8_t stableRssi = !stable ? -127 :
        (connected ? rssiDbm : static_cast<int8_t>(WiFi.RSSI()));
    __atomic_store_n(&publishedRssiDbm, stableRssi, __ATOMIC_RELEASE);
  }
  __atomic_store_n(&publishedConfigured, credentialsConfigured(), __ATOMIC_RELEASE);
  __atomic_store_n(&publishedConnected, stable, __ATOMIC_RELEASE);
  __atomic_store_n(&publishedState, static_cast<uint8_t>(
      stable ? NetworkStateCode::Connected :
          (state == NetworkStateCode::Connected ? NetworkStateCode::Connecting : state)), __ATOMIC_RELEASE);
}

// Called by networkTask even when deep recovery/isolation skips station service.
// A pending offline transition must still expire while the owners are draining.
inline void tickStableWifi() {
  // publish samples the driver afresh; owner drain is not a Wi-Fi disconnect.
  publish(static_cast<NetworkStateCode>(__atomic_load_n(&rawState, __ATOMIC_ACQUIRE)),
      __atomic_load_n(&rawConnected, __ATOMIC_ACQUIRE),
      __atomic_load_n(&rawRssiDbm, __ATOMIC_ACQUIRE));
}

inline void stopRadio() {
  // Called only at boot or after the runtime quiesce handshake.
  WiFi.setAutoReconnect(false);
  (void)WiFi.disconnect(false, false);
  (void)WiFi.mode(WIFI_OFF);
  radioActive = false;
  connectionStartedAt = 0U;
  wifiPowerModeAppliedValid = false;
}

inline bool startStation(uint32_t now) {
  // Never tear STA down here. Runtime reconnects arrive after a quiesced
  // reconnect transaction; boot starts with the radio already stopped.
  if (!WiFi.setHostname(NETWORK_WIFI_HOSTNAME)) {
    stopRadio();
    return false;
  }
  if (!WiFi.mode(WIFI_STA)) {
    stopRadio();
    return false;
  }
  (void)WiFi.setAutoReconnect(false);
  const char *password = activePassword[0] == '\0' ? nullptr : activePassword;
  (void)WiFi.begin(activeSsid, password);
  radioActive = true;
  connectionStartedAt = now;
  wifiPowerModeAppliedValid = false;
  publish(NetworkStateCode::Connecting, false);
  return true;
}

// PERFORMANCE while the Web is in use and for 15 min after, MIN_MODEM after that (wifi_power_policy.h). Applied only
// here (networkTask), re-applied after every association/recovery, retried on driver failure without log flooding.
inline void applyWifiPowerMode() {
  if (!WiFi.isConnected()) { wifiPowerModeAppliedValid = false; return; }
  const uint32_t now = millis();
  const MayapWifiPower::Mode want = MayapWifiPower::desired(now);
  if (wifiPowerModeAppliedValid && want == wifiPowerModeApplied) return;
  if (esp_wifi_set_ps(want == MayapWifiPower::Mode::Save ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE) != ESP_OK) return;
  wifiPowerModeAppliedValid = true;
  wifiPowerModeApplied = want;
  mayapSerialPrintf(false, "[WIFI-RECOVERY] power=%s\n",
      want == MayapWifiPower::Mode::Save ? "SAVE (Web idle 15 min)" : "PERFORMANCE");
}

// ------------------------------ Cong 1 doi Wi-Fi -----------------------------
// Portal chi duoc yeu cau/huy tu HMI qua cac ham public ben duoi; toan bo xu ly
// thuc te nam trong networkTask (mayapNetworkUpdate) de khong dung chung stack
// TCP/DNS voi bat ky task nao khac.
static volatile uint8_t portalRequestFlag = 0U;   // 1 = HMI vua bam "Doi Wi-Fi"
static volatile uint8_t portalCancelFlag = 0U;    // 1 = HMI bam Thoat/Huy
static volatile uint8_t publishedPortalState =
    static_cast<uint8_t>(WifiPortalState::Idle);
static char publishedPortalApName[20] = "";
static char publishedPortalPassword[16] = "";
static portMUX_TYPE portalNameMux = portMUX_INITIALIZER_UNLOCKED;

enum class PortalPhase : uint8_t { Idle, Quiescing, Starting, ApActive, Testing, Success, Failed };
static PortalPhase portalPhase = PortalPhase::Idle;
static uint32_t portalOpenedAt = 0U;
static uint32_t portalTestStartedAt = 0U;
static uint32_t portalResultUntil_ = 0U;
static char portalApName[20] = "";
static char portalApPassword[16] = "";
static char pendingSsid[WIFI_PORTAL_SSID_MAX + 1U] = "";
static char pendingPassword[WIFI_PORTAL_PASSWORD_MAX + 1U] = "";
static bool pendingCredentialsReady = false;

// Doi Wi-Fi dung chung radio voi MQTT/Cloud/OTA. Trước khi ha STA phai doi
// otaTask dong socket HTTPS xong; neu khong se co race teardown interface
// trong luc task khac van dang dung lwIP/TLS.
static volatile uint8_t portalOtaQuiescedFlag = 0U;
static uint32_t portalQuiesceStartedAt_ = 0U;
constexpr uint32_t WIFI_PORTAL_QUIESCE_TIMEOUT_MS = 10000UL;

// Breadcrumb nam trong RTC RAM de giu qua panic reset. reset_reason=4 chi cho
// biet ESP_RST_PANIC; marker nay cho biet panic xay ra o buoc radio nao.
constexpr uint32_t PORTAL_CRASH_MAGIC = 0x4D505750UL;  // "MPWP"
RTC_NOINIT_ATTR static uint32_t portalCrashMagic_;
RTC_NOINIT_ATTR static uint32_t portalCrashStage_;

inline void portalCrashMark(uint32_t stage) {
  portalCrashMagic_ = PORTAL_CRASH_MAGIC;
  portalCrashStage_ = stage;
}
inline void portalCrashClear() {
  portalCrashMagic_ = 0U;
  portalCrashStage_ = 0U;
}
inline const char *portalCrashStageText(uint32_t stage) {
  switch (stage) {
    case 10: return "QUIESCING";
    case 20: return "DISCONNECT_STA";
    case 30: return "MODE_AP_STA";
    case 40: return "SOFTAP_START";
    case 50: return "SERVER_START";
    case 60: return "AP_ACTIVE";
    case 70: return "TEST_NEW_STA";
    case 80: return "PORTAL_STOP";
    default: return "UNKNOWN";
  }
}

// Trang thai rieng cho pha "Starting": AP tren ESP32+STA da bat ke ca khi
// STA dang ket noi that (WiFi.softAP() vua bi tu choi vua bi cham) la
// nguyen nhan pho bien nhat khien AP "chap chon"/gan nhu khong phat song.
// Thu lai vai lan thay vi tin softAP() luon thanh cong ngay lan dau.
constexpr uint32_t WIFI_PORTAL_AP_RETRY_MS = 400UL;
constexpr uint32_t WIFI_PORTAL_AP_START_TIMEOUT_MS = 6000UL;
static uint32_t portalApNextAttemptAt_ = 0U;
static uint32_t portalApStartingSince_ = 0U;
static bool portalServersStarted_ = false;

static WebServer portalServer(80);
static DNSServer portalDns;

inline void publishPortalState(WifiPortalState state, const char *apName) {
  __atomic_store_n(&publishedPortalState, static_cast<uint8_t>(state),
                   __ATOMIC_RELEASE);
  portENTER_CRITICAL(&portalNameMux);
  snprintf(publishedPortalApName, sizeof(publishedPortalApName), "%s",
           apName ? apName : "");
  snprintf(publishedPortalPassword, sizeof(publishedPortalPassword), "%s",
           (apName && apName[0]) ? portalApPassword : "");
  portEXIT_CRITICAL(&portalNameMux);
}

inline String htmlEscape(const String &s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); ++i) {
    switch (s[i]) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default: out += s[i]; break;
    }
  }
  return out;
}

inline String buildWifiOptions() {
  String options;
  const int count = WiFi.scanComplete() >= 0 ? WiFi.scanComplete()
                                             : WiFi.scanNetworks(false, true);
  if (count <= 0) return F("<option value=''>Khong tim thay Wi-Fi</option>");
  for (int i = 0; i < count; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;
    options += F("<option value=\"");
    options += htmlEscape(ssid);
    options += F("\">");
    options += htmlEscape(ssid);
    options += F("  (");
    options += String(WiFi.RSSI(i));
    options += F(" dBm)</option>");
  }
  return options;
}

// Dinh danh thiet bi theo dung dinh dang "MAP-XXXXXXXXXXXX" ma web dung
// (xem transaction_bridge.h::ensureIdentity). File nay duoc include TRUOC
// transaction_bridge.h trong .ino nen tu tinh rieng, khong dung chung bien -
// cung ly do elapsedMs/timeReached o dau file nay phai co ban rieng.
inline String mayapDeviceIdText() {
  const uint64_t mac = ESP.getEfuseMac();
  char buf[20];
  snprintf(buf, sizeof(buf), "MAP-%02X%02X%02X%02X%02X%02X",
           static_cast<uint8_t>(mac >> 0), static_cast<uint8_t>(mac >> 8),
           static_cast<uint8_t>(mac >> 16), static_cast<uint8_t>(mac >> 24),
           static_cast<uint8_t>(mac >> 32), static_cast<uint8_t>(mac >> 40));
  return String(buf);
}

inline String buildConnectionInfo() {
  String info;
  info += F("<div class=info><span>ID thiet bi</span><b>");
  info += mayapDeviceIdText();
  info += F("</b></div>");
  info += F("<div class=info><span>Wi-Fi hien tai</span><b>");
  if (WiFi.status() == WL_CONNECTED) {
    info += htmlEscape(WiFi.SSID());
    info += F(" (");
    info += String(WiFi.RSSI());
    info += F(" dBm)");
  } else if (credentialsConfigured()) {
    info += htmlEscape(String(activeSsid));
    info += F(" (chua ket noi)");
  } else {
    info += F("Chua cau hinh");
  }
  info += F("</b></div>");
  return info;
}

inline void handlePortalRoot() {
  const String connectionInfo = buildConnectionInfo();
  const String options = buildWifiOptions();
  String html;
  html.reserve(5120);  // CSS dai hon ban cu (dong bo mau thuong hieu) - du cho + 1KB
  // Mang dung MAU/PHONG CACH giong het trang web chinh (index.html/styles.css)
  // de nguoi dung cam thay "cung 1 san pham" thay vi 1 trang ky thuat roi rac
  // - du day la trang RIENG, tu ESP32 host qua AP offline (khong the tai
  // Google Fonts/CSS ngoai, phai tu chua toan bo nhu truoc).
  html += F(
    "<!doctype html><html lang=vi><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>MAYAP - Doi Wi-Fi</title><style>"
    "*{box-sizing:border-box}"
    "body{margin:0;font-family:ui-sans-serif,-apple-system,'Segoe UI',Roboto,"
    "Arial,sans-serif;background:linear-gradient(180deg,#f2f7f5,#eaf2f0);"
    "color:#0c2f2a;min-height:100vh;padding:28px 16px}"
    ".card{max-width:420px;margin:0 auto;background:#fff;"
    "border:1px solid #cededb;border-radius:20px;"
    "box-shadow:0 12px 32px rgba(10,55,48,.08);padding:24px}"
    ".brandRow{display:flex;align-items:center;gap:12px;margin-bottom:6px}"
    ".brandMark{width:38px;height:38px;border-radius:12px;background:#49cfbc;"
    "color:#073b34;font-weight:900;font-size:18px;display:flex;"
    "align-items:center;justify-content:center;flex:none}"
    "h1{font-size:19px;margin:0;letter-spacing:-.3px}"
    "label{display:block;font-size:12px;font-weight:700;margin:16px 0 6px;"
    "color:#496963;text-transform:uppercase;letter-spacing:.3px}"
    "select,input{width:100%;height:48px;border-radius:14px;border:1px "
    "solid #c5d8d4;background:#fff;color:#0c2f2a;padding:0 14px;"
    "font-size:15px;box-sizing:border-box}"
    "select:focus,input:focus{outline:0;border-color:#42aa9c;"
    "box-shadow:0 0 0 3px rgba(66,170,156,.15)}"
    ".showPass{display:flex;align-items:center;gap:8px;margin:10px 2px 0;"
    "text-transform:none;letter-spacing:0;font-size:13px;color:#496963;cursor:pointer}"
    ".showPass input{width:18px;height:18px;margin:0;padding:0;flex:none;box-shadow:none}"
    "button,a.reload{width:100%;height:48px;margin-top:18px;border:0;"
    "border-radius:14px;background:#0d8275;color:#fff;font-weight:800;"
    "font-size:15px;display:flex;align-items:center;justify-content:center;"
    "text-decoration:none;box-sizing:border-box;cursor:pointer}"
    "a.reload{background:#eef5f3;color:#0c2f2a;margin-top:10px}"
    "p{color:#718783;font-size:12px;line-height:1.6;margin:16px 0 0}"
    ".info{display:flex;justify-content:space-between;align-items:center;"
    "font-size:13px;padding:9px 0;border-bottom:1px solid #e2ece9}"
    ".info span{color:#718783}.info b{color:#0c2f2a;font-weight:700;"
    "text-align:right;word-break:break-word}"
    "</style></head><body>"
    "<div class=card><div class=brandRow><div class=brandMark>M</div>"
    "<h1>Doi mang Wi-Fi cho may ap</h1></div>");
  html += connectionInfo;
  html += F(
    "<form method=POST action=/save>"
    "<label>Mang Wi-Fi</label><select name=ssid required>");
  html += options;
  html += F(
    "</select><label>Mat khau</label>"
    "<input id=wifiPassword name=password type=password maxlength=64 autocomplete=off>"
    "<label class=showPass><input id=showPassword type=checkbox "
    "onchange=\"document.getElementById('wifiPassword').type=this.checked?'text':'password'\">"
    "<span>Hien mat khau</span></label>"
    "<button type=submit>Luu &amp; ket noi</button></form>"
    "<a class=reload href=/rescan>&#8635; Tim lai Wi-Fi</a>"
    "<p><b>Sau khi luu:</b> may se tu thu ket noi mang moi, kiem tra man "
    "hinh may ap de biet ket qua. Bay gio ban co the chuyen dien thoai tro "
    "lai mang Wi-Fi thuong (thoat khoi mang MAYAP-XXXX) va mo lai trang web "
    "chinh nhu cu. Thong bao canh bao cua may nay duoc bat rieng qua trang "
    "web chinh, khong can cau hinh gi them o day.</p></div>");
  html += F("</body></html>");
  portalServer.send(200, "text/html; charset=utf-8", html);
}

inline void handlePortalRescan() {
  // Xoa ket qua quet cu va quet lai dong bo (nguoi dung vua bam "Tim lai Wi-Fi"
  // nen cho doi vai giay la hop ly); buildWifiOptions() cua trang / se dung
  // lai ket qua nay ngay, khong quet lan thu hai.
  WiFi.scanDelete();
  WiFi.scanNetworks(false, true);
  portalServer.sendHeader("Location", "/", true);
  portalServer.send(302, "text/plain", "");
}

inline void handlePortalSave() {
  if (!portalServer.hasArg("ssid") || portalServer.arg("ssid").isEmpty()) {
    portalServer.send(400, "text/plain; charset=utf-8", "Thieu SSID");
    return;
  }
  const String ssid = portalServer.arg("ssid");
  const String pass = portalServer.arg("password");
  if (ssid.length() > WIFI_PORTAL_SSID_MAX ||
      pass.length() > WIFI_PORTAL_PASSWORD_MAX) {
    portalServer.send(400, "text/plain; charset=utf-8", "SSID/mat khau qua dai");
    return;
  }
  snprintf(pendingSsid, sizeof(pendingSsid), "%s", ssid.c_str());
  snprintf(pendingPassword, sizeof(pendingPassword), "%s", pass.c_str());
  pendingCredentialsReady = true;
  portalServer.send(200, "text/html; charset=utf-8",
      "<!doctype html><html lang=vi><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>MAYAP - Da luu Wi-Fi</title><style>"
      "body{margin:0;font-family:ui-sans-serif,-apple-system,'Segoe UI',"
      "Roboto,Arial,sans-serif;background:linear-gradient(180deg,#f2f7f5,"
      "#eaf2f0);color:#0c2f2a;min-height:100vh;display:flex;"
      "align-items:center;justify-content:center;padding:24px}"
      ".card{max-width:380px;background:#fff;border:1px solid #cededb;"
      "border-radius:20px;box-shadow:0 12px 32px rgba(10,55,48,.08);"
      "padding:26px;text-align:center}"
      ".ok{width:52px;height:52px;border-radius:50%;background:#dff3ef;"
      "color:#0d8275;font-size:26px;display:flex;align-items:center;"
      "justify-content:center;margin:0 auto 14px}"
      "h1{font-size:18px;margin:0 0 8px}p{color:#718783;font-size:13px;"
      "line-height:1.6;margin:0}"
      "</style></head><body><div class=card><div class=ok>&#10003;</div>"
      "<h1>Da luu Wi-Fi moi</h1>"
      "<p>May ap dang thu ket noi mang vua nhap, vui long xem man hinh "
      "thiet bi de biet ket qua. Ban co the dong trang nay va chuyen dien "
      "thoai tro lai mang Wi-Fi thuong.</p></div></body></html>");
}

inline void handlePortalNotFound() {
  // Captive portal: moi URL la khong xac dinh deu quay ve trang cau hinh.
  portalServer.sendHeader("Location", "http://192.168.4.1/", true);
  portalServer.send(302, "text/plain", "");
}

inline void portalStop() {
  portalCrashMark(80U);
  portalServer.stop();
  portalDns.stop();
  // Quiescing nghia la AP CHUA duoc bat va otaTask co the chua ACK.
  // Huy dung o pha nay tuyet doi khong duoc cham radio; chi khi da qua
  // quiesce moi co quyen tat AP/doi mode.
  if (portalPhase != PortalPhase::Idle &&
      portalPhase != PortalPhase::Quiescing) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
  }
  // Preserve a successfully joined STA. Do not release exclusivity and then
  // tear the same interface down again while realtime starts TLS.
  radioActive = WiFi.getMode() != WIFI_OFF;
  connectionStartedAt = millis();
  wifiPowerModeAppliedValid = false;
  if (WiFi.isConnected()) {
    connectionStartedAt = 0U;
    deepPolicy.success(millis());
    staBackoff.onSuccess();
  } else {
    staBackoff.reset(millis());
  }
  portalPhase = PortalPhase::Idle;
  pendingCredentialsReady = false;
  __atomic_store_n(&portalOtaQuiescedFlag, 0U, __ATOMIC_RELEASE);
  publishPortalState(WifiPortalState::Idle, "");
  portalCrashClear();
  mayapRadioQuiesceEnd();
}

// Bat AP that su. Tach rieng khoi portalBeginStarting() de goi lai duoc
// nhieu lan (retry) ma khong lam lai buoc doi mode/dat ten AP.
inline bool bringUpSoftAp() {
  portalCrashMark(40U);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  const bool ok = WiFi.softAP(portalApName, portalApPassword);
  mayapSerialPrintf(false, "[PORTAL] softAP(%s) -> %s\n", portalApName,
                    ok ? "OK" : "FAIL");
  return ok;
}

inline void portalBeginStarting(uint32_t now) {
  const uint64_t chip = ESP.getEfuseMac();
  snprintf(portalApName, sizeof(portalApName), "MAYAP-%04X",
           static_cast<unsigned>((chip >> 32U) & 0xFFFFU));
  // Mat khau moi moi lan mo cong, hien truc tiep tren HMI. Khong con AP mo.
  snprintf(portalApPassword, sizeof(portalApPassword), "MP%06lX",
           static_cast<unsigned long>(esp_random() & 0xFFFFFFUL));

  // Tat auto reconnect va NGAT STA, nhung KHONG tat radio. Ban cu dung
  // disconnect(true, false): tham so true goi STA.end()/ha interface, roi ngay
  // sau lai bat AP+STA. Ket hop voi otaTask chay song song tao race lwIP/TLS.
  WiFi.setAutoReconnect(false);
  wifiPowerModeAppliedValid = false;
  portalCrashMark(20U);
  (void)WiFi.disconnect(false, false);
  portalCrashMark(30U);
  if (!WiFi.mode(WIFI_AP_STA)) {
    mayapSerialPrintf(false, "[PORTAL] WIFI_AP_STA that bai, giu STA cu\n");
    portalPhase = PortalPhase::Idle;
    __atomic_store_n(&portalRequestFlag, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&portalOtaQuiescedFlag, 0U, __ATOMIC_RELEASE);
    publishPortalState(WifiPortalState::Failed, "");
    portalCrashClear();
    mayapRadioQuiesceEnd();
    return;
  }

  portalApStartingSince_ = now;
  portalApNextAttemptAt_ = now;
  portalServersStarted_ = false;
  pendingCredentialsReady = false;
  portalOpenedAt = now;
  portalPhase = PortalPhase::Starting;
  publishPortalState(WifiPortalState::Starting, portalApName);
}

inline void serviceStarting(uint32_t now) {
  if (!timeReached(now, portalApNextAttemptAt_)) return;
  portalApNextAttemptAt_ = now + WIFI_PORTAL_AP_RETRY_MS;

  if (!bringUpSoftAp()) {
    if (elapsedMs(now, portalApStartingSince_) >= WIFI_PORTAL_AP_START_TIMEOUT_MS) {
      mayapSerialPrintf(false,
          "[PORTAL] khong bat duoc AP sau %lums, huy mo cong\n",
          static_cast<unsigned long>(WIFI_PORTAL_AP_START_TIMEOUT_MS));
      WiFi.mode(WIFI_STA);
      portalPhase = PortalPhase::Idle;
      __atomic_store_n(&portalRequestFlag, 0U, __ATOMIC_RELEASE);
      publishPortalState(WifiPortalState::Idle, "");
      mayapRadioQuiesceEnd();
    }
    return;
  }

  if (!portalServersStarted_) {
    portalCrashMark(50U);
    portalDns.start(DNS_PORT, "*", IPAddress(192, 168, 4, 1));
    portalServer.on("/", HTTP_GET, handlePortalRoot);
    portalServer.on("/save", HTTP_POST, handlePortalSave);
    portalServer.on("/rescan", HTTP_GET, handlePortalRescan);
    portalServer.onNotFound(handlePortalNotFound);
    portalServer.begin();
    portalServersStarted_ = true;
  }
  portalPhase = PortalPhase::ApActive;
  portalCrashMark(60U);
  publishPortalState(WifiPortalState::ApActive, portalApName);
}

inline void servicePortal(uint32_t now) {
  const bool requested = __atomic_load_n(&portalRequestFlag, __ATOMIC_ACQUIRE) != 0U;
  const bool cancel = __atomic_load_n(&portalCancelFlag, __ATOMIC_ACQUIRE) != 0U;
  if (cancel) {
    __atomic_store_n(&portalCancelFlag, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&portalRequestFlag, 0U, __ATOMIC_RELEASE);
    if (portalPhase != PortalPhase::Idle) portalStop();
    return;
  }

  if (portalPhase == PortalPhase::Idle) {
    if (!requested) return;
    // Pha 1: cong bo STA offline cho cac client cua networkTask tu dong dong
    // MQTT/socket; otaTask thay portalRequestFlag va dong HTTPS.
    mayapRadioQuiesceBegin();
    portalPhase = PortalPhase::Quiescing;
    portalQuiesceStartedAt_ = now;
    portalCrashMark(10U);
    publish(NetworkStateCode::Connecting, false);
    publishPortalState(WifiPortalState::Starting, "");
    return;
  }

  if (portalPhase == PortalPhase::Quiescing) {
    publish(NetworkStateCode::Connecting, false);
    const bool otaQuiesced = __atomic_load_n(&portalOtaQuiescedFlag, __ATOMIC_ACQUIRE) != 0U;
    // Cho it nhat 1 network tick de mayapRealtimeUpdate() dong MQTT sau khi
    // publishedConnected=false, ke ca khi otaTask da ack rat nhanh.
    if (otaQuiesced && mayapOnlineOwnersDrained() && elapsedMs(now, portalQuiesceStartedAt_) >= NETWORK_TASK_PERIOD_MS) {
      portalBeginStarting(now);
      return;
    }
    if (elapsedMs(now, portalQuiesceStartedAt_) >= WIFI_PORTAL_QUIESCE_TIMEOUT_MS) {
      mayapSerialPrintf(false, "[PORTAL] huy doi Wi-Fi: I/O mang chua quiesce sau %lums\n",
          static_cast<unsigned long>(WIFI_PORTAL_QUIESCE_TIMEOUT_MS));
      __atomic_store_n(&portalRequestFlag, 0U, __ATOMIC_RELEASE);
      __atomic_store_n(&portalOtaQuiescedFlag, 0U, __ATOMIC_RELEASE);
      portalPhase = PortalPhase::Idle;
      publishPortalState(WifiPortalState::Failed, "");
      portalCrashClear();
      mayapRadioQuiesceEnd();
    }
    return;
  }

  // Da het han mo cong: dong lai va quay ve ket noi binh thuong.
  if (elapsedMs(now, portalOpenedAt) >= WIFI_PORTAL_MAX_OPEN_MS &&
      portalPhase != PortalPhase::Testing) {
    portalStop();
    __atomic_store_n(&portalRequestFlag, 0U, __ATOMIC_RELEASE);
    return;
  }

  if (portalPhase == PortalPhase::Starting) {
    serviceStarting(now);
    return;
  }

  portalDns.processNextRequest();
  portalServer.handleClient();

  if (portalPhase == PortalPhase::ApActive) {
    if (pendingCredentialsReady) {
      pendingCredentialsReady = false;
      (void)saveCredentials(pendingSsid, pendingPassword);
      portalCrashMark(70U);
      WiFi.begin(activeSsid, activePassword[0] ? activePassword : nullptr);
      portalTestStartedAt = now;
      portalPhase = PortalPhase::Testing;
      publishPortalState(WifiPortalState::Testing, portalApName);
    }
    return;
  }

  if (portalPhase == PortalPhase::Testing) {
    if (WiFi.status() == WL_CONNECTED) {
      portalPhase = PortalPhase::Success;
      portalResultUntil_ = now + 8000UL;
      publishPortalState(WifiPortalState::Success, portalApName);
      // Da co mang moi hoat dong: dong AP ngay, khong can nguoi dung thao tac them.
      portalServer.stop();
      portalDns.stop();
      WiFi.softAPdisconnect(true);
      return;
    }
    if (elapsedMs(now, portalTestStartedAt) >= WIFI_PORTAL_TEST_TIMEOUT_MS) {
      portalPhase = PortalPhase::Failed;
      portalResultUntil_ = now + 8000UL;
      publishPortalState(WifiPortalState::Failed, portalApName);
    }
    return;
  }

  if (portalPhase == PortalPhase::Success || portalPhase == PortalPhase::Failed) {
    if (portalPhase == PortalPhase::Failed) {
      // Cho phep nguoi dung mo trang lai thu SSID/mat khau khac trong cung phien AP.
      portalPhase = PortalPhase::ApActive;
      publishPortalState(WifiPortalState::ApActive, portalApName);
      return;
    }
    if (timeReached(now, portalResultUntil_)) {
      portalStop();
      __atomic_store_n(&portalRequestFlag, 0U, __ATOMIC_RELEASE);
    }
  }
}

// --------------------------- Dong bo gio qua NTP (v3.8.0) --------------------
// Mailbox rieng, cung mo hinh voi realtimeMux/pendingConfigSave (transaction_bridge.h):
// GHI boi networkTask (serviceNtpSync(), khi Wi-Fi da ket noi va toi chu ky
// dong bo), DOC+XOA boi controlTask qua mayapTakePendingNtpTime() trong
// serviceI2cDeviceRecovery() (machine_control.h). Vung critical section chi
// copy vai byte, khong bao gio giu qua mot loi goi I/O.
static portMUX_TYPE ntpMux = portMUX_INITIALIZER_UNLOCKED;
struct PendingNtpTime {
  bool pending = false;
  uint16_t year = 0U;
  uint8_t month = 0U, day = 0U, hour = 0U, minute = 0U, second = 0U;
};
static PendingNtpTime pendingNtpTime;
static uint32_t lastNtpSyncAt = 0U;
static bool ntpConfigured = false;

inline void serviceNtpSync(uint32_t now) {
  if (!NTP_SYNC_ENABLED) return;
  // lastNtpSyncAt==0 dung nghia "chua tung dong bo" - dong bo NGAY lan dau co
  // Wi-Fi (khong doi du NTP_SYNC_INTERVAL_MS) de rut ngan thoi gian phuc hoi
  // sau khi mat dien/reboot dung luc RTC hong that; sau lan dau moi vao chu ky
  // dinh ky binh thuong (chi sua troi dat nho cua thach anh DS3231).
  if (lastNtpSyncAt != 0U &&
      elapsedMs(now, lastNtpSyncAt) < NTP_SYNC_INTERVAL_MS) {
    return;
  }
  lastNtpSyncAt = now;
  if (!ntpConfigured) {
    configTime(NTP_TIMEZONE_OFFSET_SEC, 0, NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY);
    ntpConfigured = true;
  }
  struct tm timeinfo{};
  // getLocalTime() block toi da NTP_REQUEST_TIMEOUT_MS - chap nhan duoc vi day
  // la networkTask (da chiu block boi HTTP(S)/MQTT o noi khac trong cung task),
  // KHONG phai controlTask dieu khien an toan (rieng, khong bao gio block).
  if (!getLocalTime(&timeinfo, NTP_REQUEST_TIMEOUT_MS)) {
    mayapSerialPrintf(false, "[NTP] Dong bo that bai (khong lien lac duoc may chu gio)\n");
    return;
  }
  const int year = timeinfo.tm_year + 1900;
  if (year < static_cast<int>(RTC_VALID_YEAR_MIN) ||
      year > static_cast<int>(RTC_VALID_YEAR_MAX)) {
    return;  // gia tri rac (SNTP chua sync xong that su ngay sau configTime) - bo qua, thu lai chu ky sau
  }
  portENTER_CRITICAL(&ntpMux);
  pendingNtpTime.pending = true;
  pendingNtpTime.year = static_cast<uint16_t>(year);
  pendingNtpTime.month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
  pendingNtpTime.day = static_cast<uint8_t>(timeinfo.tm_mday);
  pendingNtpTime.hour = static_cast<uint8_t>(timeinfo.tm_hour);
  pendingNtpTime.minute = static_cast<uint8_t>(timeinfo.tm_min);
  pendingNtpTime.second = static_cast<uint8_t>(timeinfo.tm_sec);
  portEXIT_CRITICAL(&ntpMux);
  mayapSerialPrintf(false, "[NTP] Dong bo OK %04d-%02d-%02d %02d:%02d:%02d\n",
      year, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour,
      timeinfo.tm_min, timeinfo.tm_sec);
}

}  // namespace MayapNetworkInternal

// Doc boi controlTask (MachineController::serviceI2cDeviceRecovery()) - lay va
// xoa 1 lan gio moi nhat tu NTP neu co (giong het pattern pendingConfigSave.used
// o transaction_bridge.h). Tra ve true + dien du 6 truong neu co du lieu moi.
inline bool mayapTakePendingNtpTime(uint16_t &year, uint8_t &month, uint8_t &day,
                                     uint8_t &hour, uint8_t &minute,
                                     uint8_t &second) {
  using namespace MayapNetworkInternal;
  bool has = false;
  portENTER_CRITICAL(&ntpMux);
  if (pendingNtpTime.pending) {
    has = true;
    year = pendingNtpTime.year;
    month = pendingNtpTime.month;
    day = pendingNtpTime.day;
    hour = pendingNtpTime.hour;
    minute = pendingNtpTime.minute;
    second = pendingNtpTime.second;
    pendingNtpTime.pending = false;
  }
  portEXIT_CRITICAL(&ntpMux);
  return has;
}

// Dinh danh thiet bi "MAP-XXXXXXXXXXXX" dung chung cho ca portal web va cac
// module khac (cloud_alert_link.h) - tranh moi noi tu tinh lai tu MAC rieng.
inline String mayapDeviceIdText() {
  return MayapNetworkInternal::mayapDeviceIdText();
}

inline void mayapNetworkBegin() {
  using namespace MayapNetworkInternal;
  if (esp_reset_reason() == ESP_RST_PANIC && portalCrashMagic_ == PORTAL_CRASH_MAGIC &&
      portalCrashStage_ != 0U) {
    mayapSerialPrintf(false, "[PORTAL-PANIC] stage=%lu (%s)\n",
        static_cast<unsigned long>(portalCrashStage_),
        portalCrashStageText(portalCrashStage_));
  }
  portalCrashClear();
  loadCredentialsOnce();
  // Staged boot loads the machine's requested mode before this task starts.
  // Preserve it; requestedMode is already initialized Offline at cold startup.
  stopRadio();
  publish(NetworkStateCode::Offline, false);
}

// Cac cau hinh KET NOI quan trong - in mot lan luc boot va bat cu khi nao go
// lenh CONFIG, de theo doi tu xa (mat khau Wi-Fi va device_key Cloud Push
// KHONG bao gio in ra, chi bao co cau hinh hay khong, tranh lo qua Serial).
inline void mayapPrintNetworkConfig() {
  using namespace MayapNetworkInternal;
  loadCredentialsOnce();
  // Goi ro namespace: ham nay cung ton tai o pham vi global (wrapper cong
  // khai ben tren) nen goi khong ro se mo ho gap "using namespace" o day.
  mayapSerialPrintf(false, "[CONFIG] id_thiet_bi=%s\n",
      MayapNetworkInternal::mayapDeviceIdText().c_str());
  mayapSerialPrintf(false, "[CONFIG] wifi_ssid=%s da_luu_mat_khau=%s\n",
      activeSsid[0] ? activeSsid : "(chua cau hinh)",
      activePassword[0] ? "CO" : "KHONG");
  mayapSerialPrintf(false, "[CONFIG] cloud_api_host=%s cloud_device_key=%s\n",
      CLOUD_API_HOST[0] ? CLOUD_API_HOST : "(chua cau hinh)",
      CLOUD_DEVICE_SECRET[0] ? "DA CAU HINH" : "CHUA CAU HINH (build flag rong)");
}

inline void mayapSetConnectivityMode(ConnectivityMode mode) {
  if (static_cast<uint8_t>(mode) >
      static_cast<uint8_t>(ConnectivityMode::Online)) {
    mode = ConnectivityMode::Offline;
  }
  __atomic_store_n(&MayapNetworkInternal::requestedMode,
                   static_cast<uint8_t>(mode), __ATOMIC_RELEASE);
  // Chuyen ve OFFLINE phai dong luon cong doi Wi-Fi neu dang mo, tranh AP
  // "mo cua" sau khi nguoi dung da chon rut mang.
  if (mode != ConnectivityMode::Online) {
    __atomic_store_n(&MayapNetworkInternal::portalCancelFlag, 1U,
                     __ATOMIC_RELEASE);
  }
}

inline NetworkStatus mayapGetNetworkStatus() {
  using namespace MayapNetworkInternal;
  NetworkStatus status{};
  uint8_t mode = __atomic_load_n(&requestedMode, __ATOMIC_ACQUIRE);
  if (mode > static_cast<uint8_t>(ConnectivityMode::Online)) {
    mode = static_cast<uint8_t>(ConnectivityMode::Offline);
  }
  uint8_t state = __atomic_load_n(&publishedState, __ATOMIC_ACQUIRE);
  if (state > static_cast<uint8_t>(NetworkStateCode::Connected)) {
    state = static_cast<uint8_t>(NetworkStateCode::Offline);
  }
  status.requestedMode = static_cast<ConnectivityMode>(mode);
  status.state = static_cast<NetworkStateCode>(state);
  status.credentialsConfigured = __atomic_load_n(
      &publishedConfigured, __ATOMIC_ACQUIRE);
  status.connected = __atomic_load_n(&publishedConnected, __ATOMIC_ACQUIRE);
  status.rssiDbm = __atomic_load_n(&publishedRssiDbm, __ATOMIC_ACQUIRE);
  return status;
}

// Transport admission must never use the presentation grace period.
inline NetworkStatus mayapGetRawNetworkStatus() {
  using namespace MayapNetworkInternal;
  NetworkStatus status = mayapGetNetworkStatus();
  status.connected = __atomic_load_n(&rawConnected, __ATOMIC_ACQUIRE);
  status.state = static_cast<NetworkStateCode>(__atomic_load_n(&rawState, __ATOMIC_ACQUIRE));
  status.rssiDbm = __atomic_load_n(&rawRssiDbm, __ATOMIC_ACQUIRE);
  return status;
}

inline bool mayapWifiPortalExclusiveRequested() {
  using namespace MayapNetworkInternal;
  return __atomic_load_n(&portalRequestFlag, __ATOMIC_ACQUIRE) != 0U;
}

inline IPAddress mayapNetworkLocalIp() {
  return IPAddress(__atomic_load_n(&MayapNetworkInternal::publishedLocalIp, __ATOMIC_ACQUIRE));
}

// Owner networkTask only. Other tasks cooperate through radioQuiesce; no
// socket is forcibly stopped from this task while another owner is in TLS I/O.
inline void mayapRequestWifiDeepRecovery() {
  if (MayapNetworkInternal::deepPhase == MayapNetworkInternal::DeepPhase::Idle) {
    MayapNetworkInternal::deepRequested = true;
  }
}
inline bool mayapNetworkDeepRecoveryUpdate(uint32_t now, bool externalIoBusy) {
  using namespace MayapNetworkInternal;
  const bool portal = mayapWifiPortalExclusiveRequested();
  const bool online = __atomic_load_n(&requestedMode, __ATOMIC_ACQUIRE) ==
      static_cast<uint8_t>(ConnectivityMode::Online);
  const bool configured = credentialsConfigured();
  const bool explicitStop = !online || !configured;

  if (deepPhase == DeepPhase::Idle) {
    if (portal) return false;
    if (explicitStop) {
      deepRequested = false;
      if (!radioActive) {
        __atomic_store_n(&MayapServiceInternal::radioQuiesce, 0U, __ATOMIC_RELEASE);
        if (!portal) mayapRadioQuiesceEnd();
        return false;
      }
      mayapRadioQuiesceBegin();
      deepPhase = DeepPhase::Quiesce;
      __atomic_store_n(&MayapServiceInternal::radioQuiesce, 1U, __ATOMIC_RELEASE);
      publish(online ? NetworkStateCode::NotConfigured : NetworkStateCode::Offline, false);
      mayapSerialPrintf(false, "[WIFI-RECOVERY] quiesce owners before explicit radio stop\n");
      return true;
    }
    if (!deepPolicy.cooldownReady(now)) {
      deepRequested = false;
      return false;
    }
    if (!deepRequested && !deepPolicy.wanted(now)) return false;
    deepRequested = false;
    mayapRadioQuiesceBegin();
    deepPhase = DeepPhase::Quiesce;
    __atomic_store_n(&MayapServiceInternal::radioQuiesce, 1U, __ATOMIC_RELEASE);
    publish(NetworkStateCode::Connecting, false);
    mayapSerialPrintf(false,
        "[WIFI-RECOVERY] quiesce owners before safe STA reconnect\n");
    return true;
  }

  if (deepPhase == DeepPhase::Quiesce) {
    if (portal) {
      deepPhase = DeepPhase::Idle;
      __atomic_store_n(&MayapServiceInternal::radioQuiesce, 0U, __ATOMIC_RELEASE);
      // Keep the admission gate closed: portal takes over the same transaction.
      return false;
    }
    if (externalIoBusy || !mayapRadioOtaQuiesced() || !mayapOnlineOwnersDrained()) return true;

    const bool stillOnline = __atomic_load_n(&requestedMode, __ATOMIC_ACQUIRE) ==
        static_cast<uint8_t>(ConnectivityMode::Online);
    const bool stillConfigured = credentialsConfigured();
    if (!stillOnline || !stillConfigured) {
      stopRadio();
      deepPhase = DeepPhase::Idle;
      __atomic_store_n(&MayapServiceInternal::radioQuiesce, 0U, __ATOMIC_RELEASE);
      if (!portal) mayapRadioQuiesceEnd();
      publish(stillOnline ? NetworkStateCode::NotConfigured : NetworkStateCode::Offline, false);
      mayapSerialPrintf(false, "[WIFI-RECOVERY] radio stopped after owner quiesce\n");
      return false;
    }

    // Ordinary loss/weak Wi-Fi must never WIFI_OFF/WIFI_STA-cycle the shared
    // driver. Drain owners first, then ask the existing STA to reconnect in place.
    (void)WiFi.setAutoReconnect(false);
    const bool reconnectAccepted = WiFi.reconnect();
    if (!reconnectAccepted) {
      const char *password = activePassword[0] == '\0' ? nullptr : activePassword;
      (void)WiFi.begin(activeSsid, password);
    }
    radioActive = true;
    connectionStartedAt = now;
    wifiPowerModeAppliedValid = false;
    deepPolicy.started(now);
    staBackoff.onFailure(now);
    deepPhaseAt = now;
    deepPhase = deepPolicy.isolate() ? DeepPhase::Isolated : DeepPhase::Idle;
    __atomic_store_n(&MayapServiceInternal::radioQuiesce, 0U, __ATOMIC_RELEASE);
    mayapRadioQuiesceEnd();
    publish(NetworkStateCode::Connecting, false);
    mayapSerialPrintf(false,
        "[WIFI-RECOVERY] safe reconnect accepted=%u isolate=%u (NO RADIO RESET)\n",
        reconnectAccepted, deepPhase == DeepPhase::Isolated);
    return deepPhase == DeepPhase::Isolated;
  }

  if (deepPhase == DeepPhase::Isolated) {
    if (!portal && online &&
        MayapRecovery::age(now, deepPhaseAt) < MayapRecovery::WIFI_ISOLATE_MS)
      return true;
    deepPhase = DeepPhase::Idle;
  }
  return false;
}

inline void mayapSetWifiPortalOtaQuiesced(bool quiesced) {
  __atomic_store_n(&MayapNetworkInternal::portalOtaQuiescedFlag,
                   quiesced ? 1U : 0U, __ATOMIC_RELEASE);
}

inline bool mayapRequestWifiPortal() {
  using namespace MayapNetworkInternal;
  const uint8_t mode = __atomic_load_n(&requestedMode, __ATOMIC_ACQUIRE);
  if (mode != static_cast<uint8_t>(ConnectivityMode::Online)) return false;
  __atomic_store_n(&portalCancelFlag, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&portalOtaQuiescedFlag, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&portalRequestFlag, 1U, __ATOMIC_RELEASE);
  return true;
}

inline void mayapCancelWifiPortal() {
  __atomic_store_n(&MayapNetworkInternal::portalCancelFlag, 1U, __ATOMIC_RELEASE);
}

inline WifiPortalStatus mayapGetWifiPortalStatus() {
  using namespace MayapNetworkInternal;
  WifiPortalStatus status{};
  uint8_t state = __atomic_load_n(&publishedPortalState, __ATOMIC_ACQUIRE);
  if (state > static_cast<uint8_t>(WifiPortalState::Failed)) {
    state = static_cast<uint8_t>(WifiPortalState::Idle);
  }
  status.state = static_cast<WifiPortalState>(state);
  portENTER_CRITICAL(&portalNameMux);
  snprintf(status.apName, sizeof(status.apName), "%s", publishedPortalApName);
  snprintf(status.password, sizeof(status.password), "%s", publishedPortalPassword);
  portEXIT_CRITICAL(&portalNameMux);
  return status;
}

inline void mayapNetworkUpdate(uint32_t now) {
  using namespace MayapNetworkInternal;
  servicePortal(now);

  const uint8_t requested = __atomic_load_n(&requestedMode, __ATOMIC_ACQUIRE);
  const bool onlineRequested =
      requested == static_cast<uint8_t>(ConnectivityMode::Online);
  const bool portalOwnsRadio = portalPhase != PortalPhase::Idle;

  if (!onlineRequested) {
    // Explicit radio shutdown is performed only by the quiesced recovery path.
    // Nguoi dung chu dong rut OFFLINE - khi ho bat lai ONLINE (co the sau
    // vai gio/vai ngay), cho phep thu ket noi ngay lap tuc thay vi ke thua
    // buoc backoff cua lan mat mang KHONG lien quan truoc do.
    staBackoff.reset(now);
    if (!portalOwnsRadio) publish(NetworkStateCode::Offline, false);
    return;
  }

  if (!credentialsConfigured()) {
    // Do not tear the interface down here; quiesce owners first.
    staBackoff.reset(now);
    if (!portalOwnsRadio) publish(NetworkStateCode::NotConfigured, false);
    return;
  }

  // Trong luc cong dang mo (AP/Testing), STA thuong thuong duoc portal tu
  // dieu khien (WiFi.begin khi test SSID moi); khong de vong lap STA binh
  // thuong danh nhau voi no.
  if (portalOwnsRadio) {
    if (portalPhase == PortalPhase::Quiescing) {
      // Co y bao offline cho MQTT/Cloud trong khi radio that van con song de
      // cac socket co thoi gian dong sach TRUOC khi doi mode Wi-Fi.
      publish(NetworkStateCode::Connecting, false);
    } else if (WiFi.isConnected()) {
      int32_t rssi = WiFi.RSSI();
      if (rssi < -127) rssi = -127;
      if (rssi > 0) rssi = 0;
      publish(NetworkStateCode::Connected, true, static_cast<int8_t>(rssi));
    } else {
      publish(NetworkStateCode::Connecting, false);
    }
    // Portal owns the radio. Preserve whether the tested STA is already joined
    // so portalStop() can hand it back without a second teardown.
    radioActive = WiFi.isConnected();
    if (!radioActive && connectionStartedAt == 0U) connectionStartedAt = now;
    // Giu backoff o step 0 suot thoi gian cong dang mo (goi lai moi tick o
    // day khong sao - chi la 2 phep gan so). Khi cong dong va nhuong lai
    // quyen dieu khien STA, vong lap ben duoi luon bat dau tu do tre ngan
    // nhat, khong "an theo" so lan that bai cua mang CU truoc khi mo cong.
    staBackoff.reset(now);
    return;
  }

  if (!radioActive) {
    if (!staBackoff.ready(now)) {
      publish(NetworkStateCode::Connecting, false);
      return;
    }
    mayapRadioQuiesceBegin();
    if (!mayapOnlineOwnersDrained()) return;
    const bool started = startStation(now);
    mayapRadioQuiesceEnd();
    if (!started) {
      deepPolicy.failure(now);
      // setHostname()/mode() that bai (rat hiem - loi driver): lui backoff
      // truoc khi thu lai, khong dap lien tuc gay xoay vong CPU vo ich.
      staBackoff.onFailure(now);
      publish(NetworkStateCode::Connecting, false);
      return;
    }
    return;  // vua goi WiFi.begin(): danh cho no NETWORK_CONNECT_TIMEOUT_MS de ket noi
  }

  if (WiFi.isConnected()) {
    deepPolicy.success(now);
    staBackoff.onSuccess();  // dat lai retry counter dung yeu cau
    int32_t rssi = WiFi.RSSI();
    if (rssi < -127) rssi = -127;
    if (rssi > 0) rssi = 0;
    publish(NetworkStateCode::Connected, true,
            static_cast<int8_t>(rssi));
    applyWifiPowerMode();
    // Dong bo gio qua NTP (xem serviceNtpSync() o tren) - chi khi mang STA
    // that su on dinh (khong phai luc cong Wi-Fi dang test SSID moi).
    serviceNtpSync(now);
    return;
  }

  publish(NetworkStateCode::Connecting, false);
  deepPolicy.offline(now);
  if (elapsedMs(now, connectionStartedAt) < NETWORK_CONNECT_TIMEOUT_MS) {
    return;  // van con trong thoi gian cho hop ly cho lan thu hien tai
  }
  if (!staBackoff.ready(now)) return;  // dang trong thoi gian lui backoff

  staBackoff.onFailure(now);
  deepPolicy.failure(now);
  connectionStartedAt = now;
  // Do not touch STA here. Next networkTask cycle enters the shared quiesce
  // handshake, then reconnects in place after every TLS/socket owner is drained.
  mayapRequestWifiDeepRecovery();
}
