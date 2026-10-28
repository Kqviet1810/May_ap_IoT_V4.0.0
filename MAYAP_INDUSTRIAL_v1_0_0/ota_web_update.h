#pragma once

#include "config.h"
#include "boot_diagnostic.h"
#include "service_recovery.h"
#include "network_io_guard.h"
#include "bounded_http.h"
#include "firmware_update_guard.h"
#include "firmware_check_policy.h"
#include "firmware_check_parse.h"
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
#include <esp_partition.h>

void mayapOtaRollbackBegin();

// Cap nhat firmware TU XA qua Cloudflare Worker (la cach cap nhat qua mang DUY NHAT: ArduinoOTA da
// go bo, nap truc tiep chi bang cap USB). Dung kenh HTTPS da co san toi CLOUD_API_HOST (cung host
// voi cloud_alert_link.h) nen hoat dong duoc TU XA, khong can cung mang - phu hop quan ly nhieu
// may lap dat o nhieu noi.
//
// Luong hoat dong (an toan la uu tien hang dau, day la thiet bi dieu khien
// nhiet dang ap trung that):
//  1. Dinh ky (moi FIRMWARE_CHECK_INTERVAL_MS + lech ngau nhien, lan dau sau
//     FIRMWARE_FIRST_CHECK_DELAY_MS) khi ONLINE, hoi Worker "co ban moi hon
//     khong" - CHI hoi, KHONG tu tai ve.
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
// Chay tren otaTask (xem .ino), tach khoi networkTask vi nap la HTTPS blocking (toi 120 s) va 1 task
// chi lam 1 viec 1 luc nen tu nhien loai tru lan nhau.
namespace MayapFirmwareWebInternal {

static uint32_t lastCheckAt = 0U;
static volatile uint8_t applyRequestFlag = 0U;
static volatile uint8_t checkNowRequestFlag = 0U;
// Ket qua lan kiem tra gan nhat de HMI bao dung thuc te (truoc day chi co toast "dang kiem tra"
// va khong bao gio biet ket qua): 0=chua, 1=da yeu cau/cho mang, 2=dang chay, 3=da la ban moi nhat, 4=loi (lastError), 5=co ban moi.
static volatile uint8_t checkPhase = 0U;
static volatile uint8_t applyPhase = 0U;  // 0=khong lam gi, 1=dang tai, 2=loi, 3=thanh cong (truoc khi restart)
// Phan tram tai ve (0..100), CHI co y nghia khi applyPhase==1 - HMI doc gia
// tri nay moi khung hinh de ve thanh % tren man hinh "Dang cap nhat..."
// (xem drawFirmwareProgress() trong hmi.h). Doc/ghi don gian bang uint8_t la
// atomic tren ESP32 (khong can portENTER_CRITICAL) nen khong dung chung mux
// voi cac truong "pending" khac de khong lam cham vong lap tai file.
static volatile uint8_t downloadPercent = 0U;
// Automatic update ("Cap nhat tu dong", HMI Nang cao > Dieu khien Web, default ON): when a newer firmware is waiting and the machine is idle
// (no batch, no test, heater switch off: the same condition the HMI path needs) the update starts BY ITSELF after AUTO_UPDATE_DELAY_MS of
// uninterrupted idleness, without anyone confirming on the HMI.  Anything that makes the machine busy again resets the count.
static volatile uint8_t autoUpdateAllowed = 1U;
static volatile uint8_t autoCountdownS = 255U;
static uint32_t autoReadySince = 0U;
static char autoTriedVersion[16] = "";
static uint32_t autoTriedAt = 0U;
static uint32_t applyFailedAt = 0U;
constexpr uint32_t AUTO_UPDATE_DELAY_MS = 60000UL;

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
  uint8_t checkPhase = 0U;
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
  status.checkPhase = __atomic_load_n(&checkPhase, __ATOMIC_ACQUIRE);
  status.downloadPercent = __atomic_load_n(&downloadPercent, __ATOMIC_ACQUIRE);
  return status;
}

// Goi tu HMI (machine_control.h) khi nguoi van hanh da XAC NHAN dong y cap
// nhat - chi dat co, viec tai ve/flash thuc su lam ben trong otaTask.
inline void mayapRequestFirmwareWebApply() {
  using namespace MayapFirmwareWebInternal;
  setError("");
  __atomic_store_n(&applyPhase, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&downloadPercent, 0U, __ATOMIC_RELEASE);
  applyFailedAt = 0U;
  __atomic_store_n(&applyRequestFlag, 1U, __ATOMIC_RELEASE);
}

inline void mayapSetFirmwareAutoUpdate(bool allowed) {
  __atomic_store_n(&MayapFirmwareWebInternal::autoUpdateAllowed, allowed ? 1U : 0U, __ATOMIC_RELEASE);
}
// Seconds left before the automatic update starts; 255 = nothing is counting down.
inline uint8_t mayapFirmwareAutoUpdateCountdownS() {
  return __atomic_load_n(&MayapFirmwareWebInternal::autoCountdownS, __ATOMIC_ACQUIRE);
}

// Goi tu lenh "firmware_check_now" nguoi dung bam tren web dashboard (qua
// MQTT, xem transaction_bridge.h::mapCommandAction) - CHI yeu cau kiem tra ngay
// (bo qua nhip FIRMWARE_CHECK_INTERVAL_MS dinh ky), KHONG tu tai ve/flash gi
// ca. Neu co ban moi se hien tren HMI nhu binh thuong, van can xac nhan tai
// may - khong bypass buoc do.
inline void mayapRequestFirmwareWebCheckNow() {
  __atomic_store_n(&MayapFirmwareWebInternal::checkPhase, 1U, __ATOMIC_RELEASE);
  __atomic_store_n(&MayapFirmwareWebInternal::checkNowRequestFlag, 1U, __ATOMIC_RELEASE);
}

// Phan tich/so sanh nam trong firmware_check_parse.h (kiem thu duoc tren may tinh). Giu ten cu cho phan con lai.
inline bool mayapFirmwareVersionNewer(const char *a, const char *b) { return MayapFirmwareCheckParse::versionNewer(a, b); }

inline bool mayapFirmwareMetadataValid(const char *version, const char *hash,
                                      const char *signature, uint32_t size) {
  if (!MayapFirmwareCheckParse::metadataSyntaxValid(version, hash, signature, size)) return false;
  const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
  return target && size <= target->size;
}

// Chi HOI Worker "co ban moi khong" - KHONG tai ve. An toan goi thuong
// xuyen (chi 1 request JSON nho). MOI ket cuc deu de lai mot ket qua doc duoc (checkPhase + lastError + dong [FWWEB]):
// khong con truong hop "khong thay gi ca".
inline bool mayapFirmwareWebCheck() {
  using namespace MayapFirmwareWebInternal;
  namespace P = MayapFirmwareCheckParse;
  __atomic_store_n(&checkPhase, 2U, __ATOMIC_RELEASE);
  setError("");
  auto fail = [](const char *screen, const char *log) {
    setError(screen);
    mayapSerialPrintf(false, "[FWWEB] check THAT BAI: %s\n", log);
    publishPending(false, "", "", "", 0U);
    __atomic_store_n(&checkPhase, 4U, __ATOMIC_RELEASE);
    return false;
  };
  JsonDocument doc;
  doc["device_id"] = mayapDeviceIdText();
  doc["device_key"] = mayapDeviceSecret();
  doc["current_version"] = MAYAP_FIRMWARE_VERSION;
  String body;
  serializeJson(doc, body);

  WiFiClientSecure client;
  HTTPClient http;
  if (!beginRequest(http, client, "/api/firmware/check")) return fail("KHONG MO DUOC KET NOI", "http.begin()");
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST(body);
  if (code != 200) {
    http.end();
    char text[32], log[64];
    if (code == 401) snprintf(text, sizeof(text), "SAI KHOA THIET BI 401");
    else if (code == 404) snprintf(text, sizeof(text), "MAY CHUA DANG KY 404");
    else if (code > 0) snprintf(text, sizeof(text), "MAY CHU TRA LOI %d", code);
    else snprintf(text, sizeof(text), "KHONG KET NOI MAY CHU");
    snprintf(log, sizeof(log), "HTTP %d (%s)", code, code > 0 ? "may chu tu choi" : HTTPClient::errorToString(code).c_str());
    return fail(text, log);
  }
  const int announced = http.getSize();
  char *resp = static_cast<char *>(malloc(P::MaxBodyBytes));
  if (!resp) { http.end(); return fail("HET RAM DE DOC", "malloc"); }
  const bool bodyOk = mayapReadBoundedHttpBody(http, resp, P::MaxBodyBytes);
  http.end();
  if (!bodyOk) {
    free(resp);
    char log[64];
    snprintf(log, sizeof(log), "tra loi %d byte (toi da %u) hoac bi cat", announced, static_cast<unsigned>(P::MaxBodyBytes - 1U));
    return fail(announced >= static_cast<int>(P::MaxBodyBytes) ? "TRA LOI QUA DAI" : "DOC TRA LOI BI CAT", log);
  }
  P::Info info;
  const P::Result result = P::parse(resp, MAYAP_FIRMWARE_VERSION, info);
  free(resp);
  switch (result) {
    case P::Result::UpdateAvailable:
      if (!mayapFirmwareMetadataValid(info.version, info.sha256, info.signature, info.size))
        return fail("BAN MOI VUOT PHAN VUNG", "kich thuoc lon hon phan vung OTA");
      publishPending(true, info.version, info.sha256, info.signature, info.size);
      mayapSerialPrintf(false, "[FWWEB] Co ban moi: v%s (%lu bytes)\n", info.version, static_cast<unsigned long>(info.size));
      __atomic_store_n(&checkPhase, 5U, __ATOMIC_RELEASE);
      return true;
    case P::Result::UpToDate:
      mayapSerialPrintf(false, "[FWWEB] check OK: dang chay v%s la ban moi nhat\n", MAYAP_FIRMWARE_VERSION);
      publishPending(false, "", "", "", 0U);
      __atomic_store_n(&checkPhase, 3U, __ATOMIC_RELEASE);
      return false;
    case P::Result::NotNewer:
      mayapSerialPrintf(false, "[FWWEB] check OK: may chu bao ban khong moi hon v%s\n", MAYAP_FIRMWARE_VERSION);
      publishPending(false, "", "", "", 0U);
      __atomic_store_n(&checkPhase, 3U, __ATOMIC_RELEASE);
      return false;
    case P::Result::BadMetadata: return fail("THONG TIN BAN MOI SAI", "version/sha256/chu ky/kich thuoc sai dinh dang");
    case P::Result::ServerError: return fail("MAY CHU BAO LOI", "success=false");
    case P::Result::BadJson: default: break;
  }
  return fail("TRA LOI KHONG DOC DUOC", "JSON hong");
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
    applyFailedAt = millis() | 1U;
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
    applyFailedAt = millis() | 1U;
    return;
  }

  const int len = http.getSize();
  if (len <= 0 || static_cast<uint32_t>(len) != status.size) {
    setError("KICH THUOC FILE KHONG KHOP");
    http.end();
    __atomic_store_n(&applyPhase, 2U, __ATOMIC_RELEASE);
    applyFailedAt = millis() | 1U;
    return;
  }
  if (!Update.begin(static_cast<size_t>(len))) {
    setError("KHONG DU BO NHO OTA");
    http.end();
    __atomic_store_n(&applyPhase, 2U, __ATOMIC_RELEASE);
    applyFailedAt = millis() | 1U;
    return;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);  // 0 = thuat toan SHA-256 (khong phai SHA-224)

  WiFiClient *stream = http.getStreamPtr();
  uint8_t *heapBuf = static_cast<uint8_t *>(malloc(4096));
  const size_t bufCap = heapBuf ? 4096U : 1024U;
  uint8_t stackFallback[1024];
  uint8_t *buf = heapBuf ? heapBuf : stackFallback;
  int remaining = len;
  bool ioError = false;
  uint8_t disconnectRetries = 0;
  constexpr uint8_t MAX_RETRIES = 5;
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
      if (!http.connected()) {
        if (++disconnectRetries <= MAX_RETRIES) {
          mayapSerialPrintf(false, "[FWWEB] Socket ngat giua chung (con %d bytes), cho ket noi lai... (%u/%u)\n",
                            remaining, disconnectRetries, MAX_RETRIES);
          delay(1000);
          continue;
        }
        ioError = true;
        break;
      }
      delay(2);
      continue;
    }
    disconnectRetries = 0;
    const size_t toRead = std::min<size_t>(static_cast<size_t>(remaining),
        std::min<size_t>(avail, bufCap));
    const int readBytes = stream->readBytes(buf, toRead);
    if (readBytes <= 0) {
      if (++disconnectRetries <= MAX_RETRIES) {
        delay(500);
        continue;
      }
      ioError = true;
      break;
    }
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
  if (heapBuf) free(heapBuf);

  if (ioError || remaining != 0) {
    mbedtls_sha256_free(&sha);
    Update.abort();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(running);
    if (target) {
      (void)esp_partition_erase_range(target, 0, 4096);
    }
    mayapOtaRollbackBegin();
    setError("MAT KET NOI GIUA LUC TAI - DA HUY, GIU FIRMWARE CU");
    mayapSerialPrintf(false, "[FWWEB] Tai firmware bi ngat giua chung, HUY OTA, GIU NGUYEN firmware dang chay\n");
    __atomic_store_n(&applyPhase, 2U, __ATOMIC_RELEASE);
    applyFailedAt = millis() | 1U;
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
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(running);
    if (target) {
      (void)esp_partition_erase_range(target, 0, 4096);
    }
    mayapOtaRollbackBegin();
    mayapSerialPrintf(false,
        "[FWWEB] SAI CHECKSUM (nhan=%s, mong doi=%s) - HUY, GIU NGUYEN firmware dang chay\n",
        digestHex, status.sha256);
    setError("SAI CHECKSUM - DA HUY, GIU FIRMWARE CU");
    __atomic_store_n(&applyPhase, 2U, __ATOMIC_RELEASE);
    applyFailedAt = millis() | 1U;
    return;
  }

  if (!OTA_SIGNING_PUBLIC_KEY[0]) {
    Update.abort();
    setError("THIEU KHOA CONG KHAI OTA");
    __atomic_store_n(&applyPhase, 2U, __ATOMIC_RELEASE);
    applyFailedAt = millis() | 1U;
    return;
  }
  uint8_t signatureDer[96];
  size_t signatureLen = 0U;
  if (mbedtls_base64_decode(signatureDer, sizeof(signatureDer), &signatureLen,
        reinterpret_cast<const unsigned char *>(status.signature),
        strlen(status.signature)) != 0) {
    Update.abort();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(running);
    if (target) {
      (void)esp_partition_erase_range(target, 0, 4096);
    }
    mayapOtaRollbackBegin();
    setError("CHU KY OTA KHONG HOP LE");
    __atomic_store_n(&applyPhase, 2U, __ATOMIC_RELEASE);
    applyFailedAt = millis() | 1U;
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
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(running);
    if (target) {
      (void)esp_partition_erase_range(target, 0, 4096);
    }
    mayapOtaRollbackBegin();
    setError("SAI CHU KY SO - DA HUY");
    mayapSerialPrintf(false, "[FWWEB] Chu ky firmware KHONG HOP LE (%d) - HUY OTA\n",
                      verifyResult);
    __atomic_store_n(&applyPhase, 2U, __ATOMIC_RELEASE);
    applyFailedAt = millis() | 1U;
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
  // HTTPS never coexists with the MQTT socket: the admission below makes the realtime owner close first, and
  // refusals are retried every few seconds instead of every 30 ms.
  static uint32_t admissionRetryAt = 0U;
  if (admissionRetryAt != 0U && static_cast<int32_t>(now - admissionRetryAt) < 0) return;
  if (__atomic_load_n(&applyRequestFlag, __ATOMIC_ACQUIRE)) {
    MayapTlsOperation tlsOperation(MayapTlsKind::Ota);
    if (!tlsOperation) { admissionRetryAt = now + 3000U; return; } // keep the accepted request pending, never lose it
    __atomic_store_n(&applyRequestFlag, 0U, __ATOMIC_RELEASE);
    mayapFirmwareWebApplyNow();
    mayapReleaseCloudTlsYield();   // a failed update returns here: realtime may reconnect
    return;
  }

  const NetworkStatus netStatus = mayapGetRawNetworkStatus();
  const bool online = netStatus.requestedMode == ConnectivityMode::Online && netStatus.connected;
  if (!online) {
    // A manual check must not wait in silence: say why nothing happens (once per request).
    static bool told = false;
    if (__atomic_load_n(&checkNowRequestFlag, __ATOMIC_ACQUIRE) != 0U) {
      if (!told) { setError("CHUA ONLINE/MAT MANG"); told = true; }
    } else told = false;
    return;
  }

  {   // automatic update: the idle machine, a newer firmware waiting, the permission on: count 60 s, then ask for the apply (once per version and boot)
    const FirmwareWebStatus waiting = mayapFirmwareWebStatus();
    const bool idle = mayapFirmwareMaintenanceReady() && !mayapFirmwareMaintenanceActive();
    const bool allowed = __atomic_load_n(&autoUpdateAllowed, __ATOMIC_ACQUIRE) != 0U;
    const bool already = waiting.version[0] != '\0' && strcmp(autoTriedVersion, waiting.version) == 0;
    if (allowed && idle && waiting.available && waiting.applyPhase == 0U && !already) {
      if (autoReadySince == 0U) { autoReadySince = now | 1U; mayapSerialPrintf(false, "[FWWEB] cap nhat tu dong v%s: bat dau dem 60 s\n", waiting.version); }
      const uint32_t waited = now - autoReadySince;
      if (waited >= AUTO_UPDATE_DELAY_MS) {
        snprintf(autoTriedVersion, sizeof(autoTriedVersion), "%s", waiting.version);
        autoTriedAt = now | 1U;
        autoReadySince = 0U;
        __atomic_store_n(&autoCountdownS, 255U, __ATOMIC_RELEASE);
        __atomic_store_n(&applyRequestFlag, 1U, __ATOMIC_RELEASE);
        mayapSerialPrintf(false, "[FWWEB] cap nhat tu dong v%s: bat dau tai\n", waiting.version);
        return;
      }
      __atomic_store_n(&autoCountdownS, static_cast<uint8_t>((AUTO_UPDATE_DELAY_MS - waited + 999U) / 1000U), __ATOMIC_RELEASE);
    } else {
      autoReadySince = 0U;
      __atomic_store_n(&autoCountdownS, 255U, __ATOMIC_RELEASE);
    }
  }

  // Tu dong giai phong khoa loi sau 3 phut (180s) de cho phep thu lai khong can reboot
  if (__atomic_load_n(&applyPhase, __ATOMIC_ACQUIRE) == 2U &&
      applyFailedAt != 0U && static_cast<uint32_t>(now - applyFailedAt) >= 180000UL) {
    __atomic_store_n(&applyPhase, 0U, __ATOMIC_RELEASE);
    setError("");
    applyFailedAt = 0U;
  }
  if (autoTriedAt != 0U && static_cast<uint32_t>(now - autoTriedAt) >= 900000UL) {
    autoTriedVersion[0] = '\0';
    autoTriedAt = 0U;
  }

  const bool checkNow = __atomic_load_n(&checkNowRequestFlag, __ATOMIC_ACQUIRE) != 0U;
  static const uint32_t jitterMs = esp_random() % (FIRMWARE_CHECK_JITTER_MS + 1U);   // per boot
  if (!MayapFirmwareCheck::due(now, lastCheckAt, checkNow, FIRMWARE_FIRST_CHECK_DELAY_MS,
                               FIRMWARE_CHECK_INTERVAL_MS, jitterMs)) return;
  MayapTlsOperation tlsOperation(MayapTlsKind::Ota);
  if (!tlsOperation) {   // retry in seconds, not a whole interval later
    admissionRetryAt = now + 3000U;
    if (checkNow) setError("MANG/RAM DANG BAN, THU LAI");
    return;
  }
  if (checkNow) __atomic_store_n(&checkNowRequestFlag, 0U, __ATOMIC_RELEASE);
  lastCheckAt = now;
  mayapFirmwareWebCheck();
  mayapReleaseCloudTlsYield();     // the short HTTPS session is over: realtime may reconnect
}
