#pragma once
// Pure (host-testable) parts of the Internet firmware check: the answer of POST /api/firmware/check and the version compare.
// ota_web_update.h owns the HTTPS session; everything that decides "is there an update and is its metadata sane" lives here so a host test can
// feed it every answer shape the Worker (or a proxy) can produce.
#include <ArduinoJson.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace MayapFirmwareCheckParse {

// The answer is read into a heap buffer of this size (it used to be a 1024-byte stack buffer that silently rejected the 1117-byte answer that
// carried the GitHub release notes). The Worker's own answer is < 700 bytes; this is the ceiling the device accepts.
constexpr size_t MaxBodyBytes = 2560U;

enum class Result : uint8_t { UpToDate, UpdateAvailable, BadJson, ServerError, BadMetadata, NotNewer };

struct Info {
  char version[16] = "";
  char sha256[65] = "";
  char signature[128] = "";
  uint32_t size = 0U;
};

// "3.10.0" is newer than "3.9.0" (a string compare would say the opposite).
inline bool versionNewer(const char *a, const char *b) {
  int pa[3] = {0, 0, 0};
  int pb[3] = {0, 0, 0};
  sscanf(a ? a : "", "%d.%d.%d", &pa[0], &pa[1], &pa[2]);
  sscanf(b ? b : "", "%d.%d.%d", &pb[0], &pb[1], &pb[2]);
  for (uint8_t i = 0; i < 3U; ++i) {
    if (pa[i] != pb[i]) return pa[i] > pb[i];
  }
  return false;
}

// Syntax only (the partition-size check needs the flash and stays in ota_web_update.h).
inline bool metadataSyntaxValid(const char *version, const char *hash, const char *signature, uint32_t size) {
  if (!version || !hash || !signature || strlen(version) >= 16U || strlen(hash) != 64U || strlen(signature) < 40U ||
      strlen(signature) >= 128U || size == 0U)
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
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

// Reads only the five fields it needs (an ArduinoJson filter), so extra fields the server may add (notes, ...) never matter, however long.
inline Result parse(const char *json, const char *currentVersion, Info &out) {
  if (!json) return Result::BadJson;
  JsonDocument filter;
  filter["success"] = true;
  filter["update_available"] = true;
  filter["version"] = true;
  filter["sha256"] = true;
  filter["signature"] = true;
  filter["size"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, json, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return Result::BadJson;
  if (!doc["success"].is<bool>()) return Result::BadJson;
  if (!doc["success"].as<bool>()) return Result::ServerError;
  if (!doc["update_available"].is<bool>()) return Result::BadJson;
  if (!doc["update_available"].as<bool>()) return Result::UpToDate;
  const char *version = doc["version"] | "";
  const char *sha256 = doc["sha256"] | "";
  const char *signature = doc["signature"] | "";
  const uint32_t size = doc["size"] | 0U;
  if (!metadataSyntaxValid(version, sha256, signature, size)) return Result::BadMetadata;
  if (!versionNewer(version, currentVersion)) return Result::NotNewer;
  snprintf(out.version, sizeof(out.version), "%s", version);
  snprintf(out.sha256, sizeof(out.sha256), "%s", sha256);
  snprintf(out.signature, sizeof(out.signature), "%s", signature);
  out.size = size;
  return Result::UpdateAvailable;
}

}  // namespace MayapFirmwareCheckParse
