#pragma once

#include "config.h"
#include "boot_diagnostic.h"
#include "service_recovery.h"
#include "network_io_guard.h"
#include "bounded_http.h"
#include "firmware_update_guard.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <mbedtls/sha256.h>
#include <mbedtls/pk.h>
#include <mbedtls/base64.h>
#include <algorithm>
#include <esp_ota_ops.h>

// Cap nhat firmware TU XA qua Cloudflare Worker - KHAC HAN ota_update.h (do
// la nap qua Arduino IDE, bat buoc CUNG mang LAN, dung ArduinoOTA). File nay
// dung kenh HTTPS da co san toi CLOUD_API_HOST (cung host voi cloud_alert_
// link.h) nen hoat dong duoc TU XA, khong can cung mang - phu hop quan ly
// nhieu may lap dat o nhieu noi.
//
// Luong hoat dong (an toan la uu tien hang dau, day la thiet bi dieu khien
// nhiet dang ap trung that):
//  1. Dinh ky (moi FIRMWARE_CHECK_INTERVAL_MS) khi ONLINE, hoi Worker "co ban
//     moi hon khong" - CHI hoi, KHONG tu tai ve.
//  2. Neu co, hien dong "Cap nhat firmware" trong muc KET NOI tren HMI.
//     Nguoi van hanh phai tu bam va XAC NHAN - khong bao gio tu dong tai ve/
//     flash khi chua duoc dong y, tranh gian doan mot me dang ap ma khong ai
//     hay biet.
//  3. Khi da xac nhan, tai file qua HTTPS, VUA tai VUA tinh SHA-256 cua du
//     lieu nhan duoc, ghi vao Update tung khoi.
//  4. Sau khi tai xong, SO SANH SHA-256 tinh duoc voi gia tri Worker tra ve
//     luc kiem tra (buoc 1). Khop moi goi Update.end(true) (chinh thuc chon
//     firmware moi cho lan khoi dong ke tiep) va khoi dong lai. KHONG khop -
//     hoac mang dut giua chung - Update.abort(), GIU NGUYEN firmware dang
//     chay, bao loi ro rang, KHONG khoi dong lai vao firmware loi/thieu.
//
// Chay tren otaTask (xem .ino) - CUNG task voi ota_update.h (nap qua Arduino
// IDE), vi ca hai deu la "dang ghi flash" nen tu nhien loai tru lan nhau (1
// task chi lam 1 viec 1 luc), va ca hai deu can tach khoi networkTask vi ly
// do da neu trong ota_update.h (networkTask co the blocking toi 8s/lan boi
// MQTT/Cloud Push).
namespace MayapFirmwareWebInternal {

static uint32_t lastCheckAt = 0U;
static volatile uint8_t applyRequestFlag = 0U;
static volatile uint8_t checkNowRequestFlag = 0U;
static volatile uint8_t applyPhase = 0U;  // 0=khong lam gi, 1=dang tai, 2=loi, 3=thanh cong (truoc khi restart)
// Phan tram tai ve (0..100), CHI co y nghia khi applyPhase==1 - HMI doc gia
// tri nay moi khung hinh de ve thanh % tren man hinh "Dang cap nhat..."
// (xem drawFirmwareProgress() trong hmi.h). Doc/ghi don gian bang uint8_t la
// atomic tren ESP32 (khong can portENTER_CRITICAL) nen khong dung chung mux
// voi cac truong "pending" khac de khong lam cham vong lap tai file.
static volatile uint8_t downloadPercent = 0U;

static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
static bool pendingAvailable = false;
static char pendingVersion[16] = "";
static char pendingSha256[65] = "";
static char pendingSignature[128] = "";
static uint32_t pendingSize = 0U;
static char lastErrorText[48] = "";

inline void publishPending(bool available, const char *version, const char *sha256, const char *signature, uint32_t size) {
  portENTER_CRITICAL(&stateMux);
  pendingAvailable = available;
  snprintf(pendingVersion, sizeof(pendingVersion), "%s", version ? version : "");
  snprintf(pendingSha256, sizeof(pendingSha256), "%s", sha256 ? sha256 : "");
  snprintf(pendingSignature, sizeof(pendingSignature), "%s", signature ? signature : "");
  pendingSize = size;
  portEXIT_CRITICAL(&stateMux);
}

inline void setError(const char *text) {
  portENTER_CRITICAL(&stateMux);
  snprintf(lastErrorText, sizeof(lastErrorText), "%s", text ? text : "");
  portEXIT_CRITICAL(&stateMux);
}

// Ban rieng cua file nay - xem ghi chu tuong tu tai network_service.h/
// cloud_alert_link.h ve ly do khong dung chung ham giua cac file de doc lap
// thu tu include. Giong het beginCloudRequest() trong cloud_alert_link.h.
inline bool beginRequest(HTTPClient &http, WiFiClientSecure &client, const char *path) {
  if (!TLS_ROOT_CA[0]) {
    setError("TLS chua co CA goc tin cay");
    return false;
  }
  client.setCACert(TLS_ROOT_CA);
  client.setConnectionTimeout(CLOUD_HTTP_CONNECT_TIMEOUT_MS);
  client.setHandshakeTimeout(8);
  http.setConnectTimeout(CLOUD_HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
  char url[192];
  snprintf(url, sizeof(url), "https://%s%s", CLOUD_API_HOST, path);
  return http.begin(client, url);
}

}  // namespace MayapFirmwareWebInternal

struct FirmwareWebStatus {
  bool available = false;
  char version[16] = "";
  char sha256[65] = "";
  char signature[128] = "";
  uint32_t size = 0U;
  uint8_t applyPhase = 0U;
  uint8_t downloadPercent = 0U;
  char lastError[48] = "";
};

inline FirmwareWebStatus mayapFirmwareWebStatus() {
  using namespace MayapFirmwareWebInternal;
  FirmwareWebStatus status;
  portENTER_CRITICAL(&stateMux);
  status.available = pendingAvailable;
  snprintf(status.version, sizeof(status.version), "%s", pendingVersion);
  snprintf(status.sha256, sizeof(status.sha256), "%s", pendingSha256);
  snprintf(status.signature, sizeof(status.signature), "%s", pendingSignature);
  status.size = pendingSize;
  snprintf(status.lastError, sizeof(status.lastError), "%s", lastErrorText);
  portEXIT_CRITICAL(&stateMux);
  status.applyPhase = __atomic_load_n(&applyPhase, __ATOMIC_ACQUIRE);
  status.downloadPercent = __atomic_load_n(&downloadPercent, __ATOMIC_ACQUIRE);
  return status;
}

// Goi tu HMI (machine_control.h) khi nguoi van hanh da XAC NHAN dong y cap
// nhat - chi dat co, viec tai ve/flash thuc su lam ben trong otaTask.
inline void mayapRequestFirmwareWebApply() {
  __atomic_store_n(&MayapFirmwareWebInternal::applyRequestFlag, 1U, __ATOMIC_RELEASE);
}

// Goi tu lenh "firmware_check_now" nguoi dung bam tren web dashboard (qua
// MQTT, xem transaction_bridge.h::mapCommandAction) - CHI yeu cau kiem tra ngay
// (bo qua nhip FIRMWARE_CHECK_INTERVAL_MS dinh ky), KHONG tu tai ve/flash gi
// ca. Neu co ban moi se hien tren HMI nhu binh thuong, van can xac nhan tai
// may - khong bypass buoc do.
inline void mayapRequestFirmwareWebCheckNow() {
  __atomic_store_n(&MayapFirmwareWebInternal::checkNowRequestFlag, 1U, __ATOMIC_RELEASE);
}

// So sanh 2 chuoi phien ban dang "X.Y.Z" (giong MAYAP_FIRMWARE_VERSION) -
// tra ve dung neu 'a' MOI HON 'b'. Khong dung so sanh chuoi truc tiep vi
// "3.10.0" phai MOI HON "3.9.0" (chuoi thi "3.10.0" < "3.9.0").
inline bool mayapFirmwareVersionNewer(const char *a, const char *b) {
  int pa[3] = {0, 0, 0};
  int pb[3] = {0, 0, 0};
  sscanf(a, "%d.%d.%d", &pa[0], &pa[1], &pa[2]);
  sscanf(b, "%d.%d.%d", &pb[0], &pb[1], &pb[2]);
  for (uint8_t i = 0; i < 3U; ++i) {
    if (pa[i] != pb[i]) return pa[i] > pb[i];
  }
  return false;
}

inline bool mayapFirmwareMetadataValid(const char *version, const char *hash,
                                      const char *signature, uint32_t size) {
  if (!version || !hash || !signature || strlen(version) >= 16U ||
      strlen(hash) != 64U || strlen(signature) < 40U || strlen(signature) >= 128U)
    return false;
  uint8_t dots = 0U, digits = 0U;
  for (const char *p = version; *p; ++p) {
    if (*p >= '0' && *p <= '9') { if (++digits > 4U) return false; }
    else if (*p == '.' && digits && dots < 2U) { ++dots; digits = 0U; }
    else return false;
  }
  if (dots != 2U || !digits) return false;
  for (uint8_t i = 0U; i < 64U; ++i) {
    const char c = hash[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) return false;
  }
  const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
  return target && size > 0U && size <= target->size;
}

// Chi HOI Worker "co ban moi khong" - KHONG tai ve. An toan goi thuong
// xuyen (chi 1 request JSON nho).
inline bool mayapFirmwareWebCheck() {
  using namespace MayapFirmwareWebInternal;
  JsonDocument doc;
  doc["device_id"] = mayapDeviceIdText();
  doc["device_key"] = mayapDeviceSecret();
  doc["current_version"] = MAYAP_FIRMWARE_VERSION;
  String body;
  serializeJson(doc, body);

  WiFiClientSecure client;
  HTTPClient http;
  if (!beginRequest(http, client, "/api/firmware/check")) {
    mayapSerialPrintf(false, "[FWWEB] check -> http.begin() THAT BAI\n");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST(body);
  bool available = false;
  if (code == 200) {
    char resp[1024];
    if (!mayapReadBoundedHttpBody(http, resp, sizeof(resp))) {
      http.end(); client.stop();
      setError("Firmware metadata qua lon/khong day du");
      return false;
    }
    JsonDocument respDoc;
    if (deserializeJson(respDoc, resp) == DeserializationError::Ok) {
      available = respDoc["update_available"] | false;
      if (available) {
        const char *version = respDoc["version"] | "";
        const char *sha256 = respDoc["sha256"] | "";
        const char *signature = respDoc["signature"] | "";
        const uint32_t size = respDoc["size"] | 0U;
        if (mayapFirmwareMetadataValid(version, sha256, signature, size) &&
            mayapFirmwareVersionNewer(version, MAYAP_FIRMWARE_VERSION)) {
          publishPending(true, version, sha256, signature, size);
          mayapSerialPrintf(false, "[FWWEB] Co ban moi: v%s (%lu bytes)\n",
                            version, static_cast<unsigned long>(size));
        } else {
          available = false;
        }
      }
    }
  } else {
    mayapSerialPrintf(false, "[FWWEB] check -> HTTP %d\n", code);
  }
  http.end();
  if (!available) publishPending(false, "", "", "", 0U);
  return available;
}

// Tai ve + flash THUC SU - CHI goi sau khi nguoi van hanh da xac nhan tren
// HMI (xem executeConfirmation() trong hmi.h). Blocking (chay tren otaTask
// rieng, khong anh huong controlTask/hmiTask) toi khi xong hoac loi.
inline void mayapFirmwareWebApplyNow() {
  using namespace MayapFirmwareWebInternal;
  if (!mayapFirmwareMaintenanceReady()) {
    setError("DUNG ME/TEST VA TAT CONG TAC NHIET TRUOC");
    __atomic_store_n(&applyPhase, 2U, __ATOMIC_RELEASE);
    return;
  }
  MayapFirmwareMaintenance maintenance;
  const FirmwareWebStatus status = mayapFirmwareWebStatus();
  if (!status.available || status.size == 0U) {
    setError("KHONG CO BAN CAP NHAT DANG CHO");
    __atomic_store_n(&applyPhase, 2U, __ATOMIC_RELEASE);
    return;
  }

  __atomic_store_n(&applyPhase, 1U, __ATOMIC_RELEASE);
  __atomic_store_n(&downloadPercent, 0U, __ATOMIC_RELEASE);
  mayapSerialPrintf(false, "[FWWEB] Bat dau tai firmware v%s (%lu bytes)...\n",
                    status.version, static_cast<unsigned long>(status.size));

  WiFiClientSecure client;
  HTTPClient http;
  char path[64];
  snprintf(path, sizeof(path), "/api/firmware/download/%s", status.version);
  if (!beginRequest(http, client, path)) {
    setError("KHONG MO DUOC KET NOI TAI VE");
    __atomic_store_n(&applyPhase, 2U, __ATOMIC_RELEASE);
    return;
  }
  http.addHeader("X-Device-Id", mayapDeviceIdText());
  http.addHeader("X-Device-Key", mayapDeviceSecret());
  const int code = http.GET();
  if (code != 200) {
    mayapSerialPrintf(false, "[FWWEB] Tai firmware THAT BAI, ma HTTP=%d\n", code);
    setError("TAI FIRMWARE THAT BAI");
    http.end();
    __atomic_store_n(&applyPhase, 2U, __ATOMIC_RELEASE);
    return;
  }

  const int len = http.getSize();
  if (len <= 0 || static_cast<uint32_t>(len) != status.size) {
    setError("KICH THUOC FILE KHONG KHOP");
    http.end();
    __atomic_store_n(&applyPhase, 2U, __ATOMIC_RELEASE);
    return;
  }
  if (!Update.begin(static_cast<size_t>(len))) {
    setError("KHONG DU BO NHO OTA");
    http.end();
    __atomic_store_n(&applyPhase, 2U, __ATOMIC_RELEASE);
    return;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);  // 0 = thuat toan SHA-256 (khong phai SHA-224)

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  int remaining = len;
  bool ioError = false;
  const uint32_t startedAt = millis();
  // 2 phut du rong rai cho vai MB qua Wi-Fi noi bo/4G - tranh treo vinh vien
  // neu ket noi "treo" giua chung (khong dut hood nhung khong con du lieu den).
  const uint32_t maxDurationMs = 120000UL;
  while (remaining > 0) {
    if (millis() - startedAt > maxDurationMs) {
      ioError = true;
      break;
    }
    const size_t avail = stream->available();
    if (avail == 0U) {
      if (!http.connected()) { ioError = true; break; }
      delay(2);
      continue;
    }
    const size_t toRead = std::min<size_t>(static_cast<size_t>(remaining),
        std::min<size_t>(avail, sizeof(buf)));
    const int readBytes = stream->readBytes(buf, toRead);
    if (readBytes <= 0) { ioError = true; break; }
    const size_t written = Update.write(buf, static_cast<size_t>(readBytes));
    if (written != static_cast<size_t>(readBytes)) { ioError = true; break; }
    mayapServiceBeat(MayapRecovery::Service::Ota);
    mbedtls_sha256_update(&sha, buf, static_cast<size_t>(readBytes));
    remaining -= readBytes;
    __atomic_store_n(&downloadPercent,
        static_cast<uint8_t>((static_cast<uint32_t>(len - remaining) * 100U) / static_cast<uint32_t>(len)),
        __ATOMIC_RELEASE);
  }
  http.end();

  if (ioError || remaining != 0) {
    mbedtls_sha256_free(&sha);
    Update.abort();
    setError("MAT KET NOI GIUA LUC TAI - DA HUY, GIU FIRMWARE CU");
    mayapSerialPrintf(false, "[FWWEB] Tai firmware bi ngat giua chung, HUY OTA, GIU NGUYEN firmware dang chay\n");
    __atomic_store_n(&applyPhase, 2U, __ATOMIC_RELEASE);
    return;
  }

  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  char digestHex[65];
  for (uint8_t i = 0; i < 32U; ++i) {
    snprintf(digestHex + i * 2U, 3U, "%02x", digest[i]);
  }

  if (strcasecmp(digestHex, status.sha256) != 0) {
    Update.abort();
    mayapSerialPrintf(false,
        "[FWWEB] SAI CHECKSUM (nhan=%s, mong doi=%s) - HUY, GIU NGUYEN firmware dang chay\n",
        digestHex, status.sha256);
    setError("SAI CHECKSUM - DA HUY, GIU FIRMWARE CU");
    __atomic_store_n(&applyPhase, 2U, __ATOMIC_RELEASE);
    return;
  }

  if (!OTA_SIGNING_PUBLIC_KEY[0]) {
    Update.abort();
    setError("THIEU KHOA CONG KHAI OTA");
    __atomic_store_n(&applyPhase, 2U, __ATOMIC_RELEASE);
    return;
  }
  uint8_t signatureDer[96];
  size_t signatureLen = 0U;
  if (mbedtls_base64_decode(signatureDer, sizeof(signatureDer), &signatureLen,
        reinterpret_cast<const unsigned char *>(status.signature),
        strlen(status.signature)) != 0) {
    Update.abort();
    setError("CHU KY OTA KHONG HOP LE");
    __atomic_store_n(&applyPhase, 2U, __ATOMIC_RELEASE);
    return;
  }
  mbedtls_pk_context publicKey;
  mbedtls_pk_init(&publicKey);
  const int parseResult = mbedtls_pk_parse_public_key(
      &publicKey,
      reinterpret_cast<const unsigned char *>(OTA_SIGNING_PUBLIC_KEY),
      strlen(OTA_SIGNING_PUBLIC_KEY) + 1U);
  const int verifyResult = parseResult == 0
      ? mbedtls_pk_verify(&publicKey, MBEDTLS_MD_SHA256, digest, sizeof(digest),
                          signatureDer, signatureLen)
      : parseResult;
  mbedtls_pk_free(&publicKey);
  if (verifyResult != 0) {
    Update.abort();
    setError("SAI CHU KY SO - DA HUY");
    mayapSerialPrintf(false, "[FWWEB] Chu ky firmware KHONG HOP LE (%d) - HUY OTA\n",
                      verifyResult);
    __atomic_store_n(&applyPhase, 2U, __ATOMIC_RELEASE);
    return;
  }

  if (!Update.end(false)) {
    mayapSerialPrintf(false, "[FWWEB] Update.end() THAT BAI: %s\n", Update.errorString());
    setError("GHI FLASH THAT BAI");
    __atomic_store_n(&applyPhase, 2U, __ATOMIC_RELEASE);
    return;
  }

  mayapSerialPrintf(false, "[FWWEB] Checksum khop, ghi flash thanh cong - KHOI DONG LAI\n");
  __atomic_store_n(&applyPhase, 3U, __ATOMIC_RELEASE);
  publishPending(false, "", "", "", 0U);
  // Danh dau day la khoi dong lai CO CHU DICH (xem config.h) - khong de
  // PowerManager tinh nham lan nap firmware thanh cong nay vao bo dem "reset
  // bat thuong", tranh bao gia "ABNORMAL RESET"/mat dien sau khi nap ban moi.
  mayapMarkIntentionalRestart();
  delay(300);
  mayapRestart(MayapBoot::RestartReason::InternetOta, "verified Internet OTA complete");
}

// Goi moi chu ky tu otaTask (xem .ino). Uu tien xu ly yeu cau ap dung dang
// cho; neu khong co, xu ly yeu cau "kiem tra ngay" tu web; neu khong co
// nua, dinh ky tu kiem tra ban moi khi dang ONLINE.
inline void mayapFirmwareWebUpdate(uint32_t now) {
  using namespace MayapFirmwareWebInternal;
  if (__atomic_load_n(&applyRequestFlag, __ATOMIC_ACQUIRE)) {
    MayapTlsOperation tlsOperation;
    if (!tlsOperation) return; // keep the accepted request pending, never lose it
    __atomic_store_n(&applyRequestFlag, 0U, __ATOMIC_RELEASE);
    mayapFirmwareWebApplyNow();
    return;
  }

  const NetworkStatus netStatus = mayapGetRawNetworkStatus();
  if (netStatus.requestedMode != ConnectivityMode::Online || !netStatus.connected) return;

  const bool checkNow = __atomic_load_n(&checkNowRequestFlag, __ATOMIC_ACQUIRE) != 0U;
  if (!checkNow && lastCheckAt != 0U && (now - lastCheckAt) < FIRMWARE_CHECK_INTERVAL_MS) return;
  MayapTlsOperation tlsOperation(MayapTlsKind::Ota);
  if (!tlsOperation) return; // retry next loop, not six hours later
  if (checkNow) __atomic_store_n(&checkNowRequestFlag, 0U, __ATOMIC_RELEASE);
  lastCheckAt = now;
  mayapFirmwareWebCheck();
}
