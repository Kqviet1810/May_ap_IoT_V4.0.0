// Host test of mqtt_ws.h: RFC 6455 vectors, key/accept generation (cross-checked by Node's crypto in
// mqtt-ws.test.cjs), in-place client framing for every length class, and the incremental server parser.
#include "../MAYAP_INDUSTRIAL_v1_0_0/mqtt_ws.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace MayapMqttWs;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); return 1; } } while (0)

int main() {
  // RFC 6455 section 1.3
  char accept[ACCEPT_BASE64 + 1];
  expectedAccept("dGhlIHNhbXBsZSBub25jZQ==", accept);
  CHECK(std::string(accept) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  // Keys: 16 bytes -> 24 base64 chars; printed for the Node cross-check.
  uint32_t seed = 12345U;
  for (int i = 0; i < 64; ++i) {
    uint8_t nonce[16];
    for (auto &b : nonce) { seed = seed * 1664525U + 1013904223U; b = static_cast<uint8_t>(seed >> 24U); }
    char key[KEY_BASE64 + 1U];
    makeKey(nonce, key);
    CHECK(strlen(key) == KEY_BASE64);
    expectedAccept(key, accept);
    printf("ACCEPT %s %s\n", key, accept);
  }

  // Upgrade request construction and bounds.
  char req[256];
  const size_t n = buildUpgradeRequest(req, sizeof(req), "h.example", "/mqtt/MAP-1", "KEY");
  CHECK(n > 0 && n == strlen(req));
  CHECK(strncmp(req, "GET /mqtt/MAP-1 HTTP/1.1\r\n", 26) == 0 && strstr(req, "\r\nHost: h.example\r\n") && strstr(req, "Sec-WebSocket-Key: KEY\r\n\r\n"));
  char small[40];
  CHECK(buildUpgradeRequest(small, sizeof(small), "h.example", "/mqtt/MAP-1", "KEY") == 0);

  // Response validation.
  const std::string good = "HTTP/1.1 101 Switching Protocols\r\nupgrade: WebSocket\r\nSEC-WEBSOCKET-ACCEPT: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nSec-WebSocket-Protocol: mqtt\r\n\r\n";
  CHECK(checkUpgradeResponse(good.data(), good.size(), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == Handshake::Ok);
  CHECK(checkUpgradeResponse(good.data(), good.size(), "s3pPLMBiTxaQ9kYGzzhZRbK+xOp=") == Handshake::BadAccept);
  const std::string noUpgrade = "HTTP/1.1 101 x\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
  CHECK(checkUpgradeResponse(noUpgrade.data(), noUpgrade.size(), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == Handshake::MissingUpgrade);
  const std::string redirect = "HTTP/1.1 301 Moved\r\nLocation: x\r\n\r\n";
  CHECK(checkUpgradeResponse(redirect.data(), redirect.size(), "x") == Handshake::BadStatus);
  CHECK(checkUpgradeResponse("HTTP", 4U, "x") == Handshake::BadStatus);
  const std::string ext = good.substr(0, good.size() - 2) + "Sec-WebSocket-Extensions: permessage-deflate\r\n\r\n";
  CHECK(checkUpgradeResponse(ext.data(), ext.size(), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == Handshake::ExtensionRefused);
  const std::string badLine = "HTTP/1.1 101 x\r\nnocolon\r\n\r\n";
  CHECK(checkUpgradeResponse(badLine.data(), badLine.size(), "x") == Handshake::Malformed);

  // Client frames, built in place, for every length class; unmask and compare.
  const size_t lengths[] = {0, 1, 125, 126, 127, 2120, 65535};
  for (size_t length : lengths) {
    std::vector<uint8_t> buf(MAX_HEADER + length + 4U, 0xEE);
    for (size_t i = 0; i < length; ++i) buf[MAX_HEADER + i] = static_cast<uint8_t>(i * 7U + 3U);
    size_t total = 0;
    uint8_t *frame = wrapClientFrame(buf.data(), MAX_HEADER, length, BINARY, 0xA1B2C3D4U, &total);
    CHECK(frame && frame >= buf.data() && frame + total == buf.data() + MAX_HEADER + length);
    CHECK(frame[0] == 0x82 && (frame[1] & 0x80));
    size_t pos = 2, len = frame[1] & 0x7F;
    if (len == 126) { len = (static_cast<size_t>(frame[2]) << 8) | frame[3]; pos = 4; }
    CHECK(len == length);
    const uint8_t mask[4] = {0xA1, 0xB2, 0xC3, 0xD4};
    CHECK(memcmp(frame + pos, mask, 4) == 0);
    for (size_t i = 0; i < length; ++i) CHECK((frame[pos + 4 + i] ^ mask[i & 3]) == static_cast<uint8_t>(i * 7U + 3U));
    CHECK(buf[MAX_HEADER + length] == 0xEE);                  // nothing written past the payload
  }
  size_t total;
  uint8_t tiny[16];
  CHECK(wrapClientFrame(tiny, 4, 1, BINARY, 1, &total) == nullptr && wrapClientFrame(tiny, 8, 70000, BINARY, 1, &total) == nullptr);

  // Parser: byte-at-a-time and in one piece give identical event streams.
  std::vector<uint8_t> stream;
  auto add = [&](uint8_t b0, std::vector<uint8_t> lenBytes, const std::string &payload) {
    stream.push_back(b0); stream.insert(stream.end(), lenBytes.begin(), lenBytes.end()); stream.insert(stream.end(), payload.begin(), payload.end());
  };
  add(0x02, {5}, "hello");                                    // binary, !FIN
  add(0x89, {2}, "pp");                                       // PING in the middle of a fragmented message
  add(0x80, {0}, "");                                         // empty final continuation
  add(0x82, {126, 0x01, 0x00}, std::string(256, 'z'));
  add(0x8A, {0}, "");                                         // PONG
  for (int mode = 0; mode < 2; ++mode) {
    FrameParser parser;
    std::string data; unsigned controls = 0;
    size_t used = 0;
    while (used < stream.size()) {
      const size_t step = mode == 0 ? stream.size() - used : 1U;
      FrameParser::Event ev;
      const size_t got = parser.feed(stream.data() + used, step, ev);
      used += got;
      CHECK(!parser.fatal());
      if (ev == FrameParser::Event::Payload) data.append(reinterpret_cast<const char *>(parser.payload()), parser.payloadLength());
      if (ev == FrameParser::Event::Control) ++controls;
      if (got == 0U && ev == FrameParser::Event::None) break;
    }
    CHECK(data == "hello" + std::string(256, 'z') && controls == 2U);
  }

  // unconsume(): a slice partly handed back is delivered again from the caller's carry.
  {
    FrameParser parser;
    std::vector<uint8_t> frame = {0x82, 10, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    FrameParser::Event ev;
    size_t used = parser.feed(frame.data(), frame.size(), ev);
    CHECK(ev == FrameParser::Event::Payload && used == frame.size() && parser.payloadLength() == 10U);
    parser.unconsume(6U);                                     // caller only took 4 payload bytes
    used = parser.feed(frame.data() + 6, 6, ev);              // replays the last 6
    CHECK(ev == FrameParser::Event::Payload && parser.payloadLength() == 6U && used == 6U && parser.payload()[0] == 4U);
    CHECK(!parser.fatal());
    parser.feed(frame.data(), 2U, ev);                        // the next frame header is accepted afterwards
    CHECK(!parser.fatal());
  }
  printf("mqtt ws host tests PASS\n");
  return 0;
}
