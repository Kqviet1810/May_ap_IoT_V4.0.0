// Host test for MAYAP_INDUSTRIAL_v1_0_0/mqtt_wire.h.
//   mqtt-wire encode   -> prints the device-side packets as hex lines (decoded by the broker's JS codec)
//   mqtt-wire decode   -> reads "hex" lines from stdin (packets built by the JS codec), prints what the
//                         firmware parser sees; every line is fed in random-ish chunk sizes.
#include "../MAYAP_INDUSTRIAL_v1_0_0/mqtt_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

using namespace MayapMqttWire;

static void hex(const char *label, const uint8_t *data, size_t length) {
  printf("%s ", label);
  for (size_t i = 0; i < length; ++i) printf("%02x", data[i]);
  printf("\n");
}
static int fail(const char *what) { fprintf(stderr, "FAIL: %s\n", what); return 1; }

static int encode() {
  uint8_t out[4200];
  static const uint8_t will[] = "{\"online\":false}";
  ConnectArgs connect{"esp-MAP-AABBCCDDEEFF", "MAP-AABBCCDDEEFF", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                      "mayap/v1/MAP-AABBCCDDEEFF/presence", will, sizeof(will) - 1U, 30U};
  size_t n = encodeConnect(out, sizeof(out), connect);
  if (!n) return fail("connect");
  hex("connect", out, n);
  const Subscription subs[] = {{"mayap/v1/MAP-AABBCCDDEEFF/command", 1}, {"mayap/v1/MAP-AABBCCDDEEFF/config/set", 1},
                               {"mayap/v1/MAP-AABBCCDDEEFF/history/request", 1}, {"mayap/v1/MAP-AABBCCDDEEFF/session", 0}};
  n = encodeSubscribe(out, sizeof(out), 1U, subs, 4U);
  if (!n) return fail("subscribe");
  hex("subscribe", out, n);
  static const uint8_t body[] = "{\"online\":true}";
  n = encodePublish(out, sizeof(out), "mayap/v1/MAP-AABBCCDDEEFF/presence", body, sizeof(body) - 1U, 1U, true, 7U);
  if (!n) return fail("publish qos1 retain");
  hex("publish-q1-retain", out, n);
  n = encodePublish(out, sizeof(out), "mayap/v1/MAP-AABBCCDDEEFF/snapshot", body, sizeof(body) - 1U, 0U, false, 0U);
  if (!n) return fail("publish qos0");
  hex("publish-q0", out, n);
  // 2000-byte payload: two-byte Remaining Length.
  static uint8_t big[2000]; memset(big, 'x', sizeof(big));
  n = encodePublish(out, sizeof(out), "mayap/v1/MAP-AABBCCDDEEFF/ack", big, sizeof(big), 1U, false, 65535U);
  if (!n) return fail("publish big");
  hex("publish-big", out, n);
  n = encodePuback(out, sizeof(out), 513U); hex("puback", out, n);
  n = encodePingreq(out, sizeof(out)); hex("pingreq", out, n);
  n = encodeDisconnect(out, sizeof(out)); hex("disconnect", out, n);
  // Bounds: too small a buffer must fail, never write past it.
  uint8_t tiny[8];
  if (encodeConnect(tiny, sizeof(tiny), connect) != 0U) return fail("connect overflow accepted");
  if (encodePublish(tiny, sizeof(tiny), "mayap/v1/MAP-AABBCCDDEEFF/ack", big, sizeof(big), 1U, false, 1U) != 0U) return fail("publish overflow accepted");
  if (encodePublish(out, sizeof(out), "t", body, 1U, 2U, false, 1U) != 0U) return fail("qos2 accepted");
  if (encodePublish(out, sizeof(out), "t", body, 1U, 1U, false, 0U) != 0U) return fail("qos1 without id accepted");
  return 0;
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static int decode() {
  char line[16384];
  uint8_t buffer[2560];
  unsigned seed = 12345U;
  while (fgets(line, sizeof(line), stdin)) {
    size_t hexLength = strlen(line);
    while (hexLength && (line[hexLength - 1U] == '\n' || line[hexLength - 1U] == '\r')) --hexLength;
    if (!hexLength) continue;
    static uint8_t raw[8192];
    size_t rawLength = 0U;
    for (size_t i = 0; i + 1U < hexLength; i += 2U) raw[rawLength++] = static_cast<uint8_t>(hexval(line[i]) * 16 + hexval(line[i + 1U]));
    StreamParser parser(buffer, sizeof(buffer));
    size_t pos = 0U;
    unsigned packets = 0U;
    while (pos < rawLength) {
      seed = seed * 1103515245U + 12345U;
      size_t chunk = 1U + (seed >> 16U) % 97U;           // 1..97 bytes: packets split anywhere
      if (chunk > rawLength - pos) chunk = rawLength - pos;
      size_t used = 0U;
      while (used < chunk) {
        used += parser.feed(raw + pos + used, chunk - used);
        if (parser.fatal()) { printf("fatal\n"); goto next; }
        if (parser.ready()) {
          ++packets;
          if (parser.truncated()) { printf("truncated type=%u\n", parser.type()); continue; }
          if (parser.type() == PUBLISH) {
            PublishView view;
            if (!parsePublish(parser.flags(), parser.body(), parser.bodyLength(), view)) { printf("bad-publish\n"); continue; }
            printf("publish qos=%u retain=%u id=%u topic=%.*s payload=%.*s len=%zu\n", view.qos, view.retain ? 1U : 0U, view.packetId,
                   static_cast<int>(view.topicLength), reinterpret_cast<const char *>(view.topic),
                   static_cast<int>(view.payloadLength > 40U ? 40U : view.payloadLength), reinterpret_cast<const char *>(view.payload), view.payloadLength);
          } else {
            printf("type=%u flags=%u len=%zu body=", parser.type(), parser.flags(), parser.bodyLength());
            for (size_t i = 0; i < parser.bodyLength(); ++i) printf("%02x", parser.body()[i]);
            printf("\n");
          }
        }
      }
      pos += chunk;
    }
    printf("packets=%u\n", packets);
  next:;
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc >= 2 && !strcmp(argv[1], "encode")) return encode();
  if (argc >= 2 && !strcmp(argv[1], "decode")) return decode();
  return fail("usage");
}
