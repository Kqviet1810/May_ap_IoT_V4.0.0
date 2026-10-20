// Host test of the answer of POST /api/firmware/check (MAYAP_INDUSTRIAL_v1_0_0/firmware_check_parse.h).
// Field case: the Worker answer carried the GitHub release notes (1117 bytes for v1.1.10) and the device, which read it into a 1024-byte
// buffer, silently dropped it, so the HMI never found an update that the Web showed. Every answer shape the Worker or a proxy can send is
// fed through the same parser the firmware uses.
#include "../MAYAP_INDUSTRIAL_v1_0_0/firmware_check_parse.h"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace MayapFirmwareCheckParse;
static unsigned checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); std::abort(); } } while (0)

static std::string answer(const std::string &version, const std::string &notes, bool available = true, const std::string &sha = std::string(64, 'a'),
                          const std::string &sig = std::string(96, 'B'), unsigned long size = 1415728UL) {
  std::string j = "{\"success\":true,\"update_available\":" + std::string(available ? "true" : "false");
  if (available) {
    j += ",\"version\":\"" + version + "\",\"sha256\":\"" + sha + "\",\"signature\":\"" + sig + "\",\"size\":" + std::to_string(size);
    if (!notes.empty()) j += ",\"notes\":\"" + notes + "\"";
  }
  return j + "}";
}

int main() {
  Info info;
  // the exact shape that failed in the field, with and without notes, and with notes far longer than the old 1024-byte buffer
  const std::string notes1110(800, 'n');
  std::string old = answer("1.1.10", notes1110);
  CHECK(old.size() > 1023U && old.size() < MaxBodyBytes);
  CHECK(parse(old.c_str(), "1.1.9", info) == Result::UpdateAvailable);
  CHECK(std::string(info.version) == "1.1.10" && info.size == 1415728UL && std::string(info.sha256).size() == 64U && std::string(info.signature).size() == 96U);
  CHECK(parse(answer("1.1.11", "").c_str(), "1.1.10", info) == Result::UpdateAvailable);
  // Worker notes can be up to 2000 characters, JSON-escaped (\n, \", á): still parsed, extra fields are ignored
  std::string escaped; for (int i = 0; i < 300; ++i) escaped += "line \\\"x\\\" \\u00e1\\n";
  CHECK(parse(answer("2.0.0", escaped).c_str(), "1.1.11", info) == Result::UpdateAvailable);
  // up to date / not newer
  CHECK(parse(answer("", "", false).c_str(), "1.1.11", info) == Result::UpToDate);
  CHECK(parse(answer("1.1.11", "").c_str(), "1.1.11", info) == Result::NotNewer);
  CHECK(parse(answer("1.1.9", "").c_str(), "1.1.11", info) == Result::NotNewer);
  // version compare (numeric, not textual)
  CHECK(versionNewer("1.1.10", "1.1.9") && !versionNewer("1.1.9", "1.1.10") && versionNewer("3.10.0", "3.9.0") && !versionNewer("1.1.11", "1.1.11"));
  CHECK(versionNewer("2.0.0", "1.99.99") && !versionNewer("garbage", "1.0.0") && !versionNewer(nullptr, "1.0.0"));
  // broken answers: each one is rejected, none can become an update
  CHECK(parse("", "1.1.9", info) == Result::BadJson);
  CHECK(parse("<html>502 Bad Gateway</html>", "1.1.9", info) == Result::BadJson);
  CHECK(parse("{\"success\":true,\"update_available\":tru", "1.1.9", info) == Result::BadJson);   // cut in the middle
  CHECK(parse("{\"update_available\":true}", "1.1.9", info) == Result::BadJson);                  // no success flag
  CHECK(parse("{\"success\":false,\"error\":\"device_key sai\"}", "1.1.9", info) == Result::ServerError);
  CHECK(parse("{\"success\":true,\"update_available\":\"yes\"}", "1.1.9", info) == Result::BadJson);
  CHECK(parse(nullptr, "1.1.9", info) == Result::BadJson);
  // metadata that must never be accepted
  CHECK(parse(answer("1.1.10", "", true, std::string(63, 'a')).c_str(), "1.1.9", info) == Result::BadMetadata);         // short hash
  CHECK(parse(answer("1.1.10", "", true, std::string(64, 'g')).c_str(), "1.1.9", info) == Result::BadMetadata);         // not hex
  CHECK(parse(answer("1.1.10", "", true, std::string(64, 'a'), std::string(39, 'B')).c_str(), "1.1.9", info) == Result::BadMetadata);
  CHECK(parse(answer("1.1.10", "", true, std::string(64, 'a'), std::string(128, 'B')).c_str(), "1.1.9", info) == Result::BadMetadata);
  CHECK(parse(answer("1.1", "").c_str(), "1.1.9", info) == Result::BadMetadata);
  CHECK(parse(answer("v1.1.10", "").c_str(), "1.1.9", info) == Result::BadMetadata);
  CHECK(parse(answer("12345.1.1", "").c_str(), "1.1.9", info) == Result::BadMetadata);
  CHECK(parse(answer("1.1.10", "", true, std::string(64, 'a'), std::string(96, 'B'), 0UL).c_str(), "1.1.9", info) == Result::BadMetadata);
  // the answer the Worker sends today (no notes) is far below the device ceiling, with the longest legal values
  const std::string worst = answer("9999.9999.9999", "", true, std::string(64, 'f'), std::string(127, 'Z'), 99999999UL);
  CHECK(worst.size() < 700U && worst.size() < MaxBodyBytes / 3U);
  std::printf("firmware check answer parser host tests PASS (%u checks)\n", checks);
  return 0;
}
