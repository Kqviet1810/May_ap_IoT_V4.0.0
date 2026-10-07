#pragma once
// Minimal RFC 6455 WebSocket client layer for the firmware's own MQTT-over-WSS transport
// (mqtt_transport.h). Pure C++: no Arduino/FreeRTOS types, no heap, no task, no library. It is
// host-tested (tests/mqtt-ws.cpp) against the RFC vectors and against Node's own ws framing.
//
// Scope is exactly what a device needs to speak to the Cloudflare broker Durable Object:
//   * opening handshake: GET with Upgrade, Sec-WebSocket-Protocol: mqtt, verified Accept;
//   * client -> server: masked BINARY frames (and CLOSE / PONG), built IN PLACE in the caller's
//     buffer (the MQTT packet is written 8 bytes into it, the header is filled in just before);
//   * server -> client: unmasked frames, incremental and bounded; data payload is handed out as
//     slices of the caller's receive chunk (zero copy), control frames (<= 125 B) are collected.
// No extensions (no compression), no text frames, no 64-bit lengths beyond 2^31.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace MayapMqttWs {

// ---- SHA-1 and Base64 (only to verify Sec-WebSocket-Accept) ---------------------------------
namespace detail {
struct Sha1 {
  uint32_t h[5];
  uint8_t block[64];
  uint32_t used;
  uint64_t total;
  static uint32_t rol(uint32_t v, unsigned n) { return (v << n) | (v >> (32U - n)); }
  void init() { h[0] = 0x67452301U; h[1] = 0xEFCDAB89U; h[2] = 0x98BADCFEU; h[3] = 0x10325476U; h[4] = 0xC3D2E1F0U; used = 0U; total = 0U; }
  void compress(const uint8_t *p) {
    uint32_t w[80];
    for (unsigned i = 0; i < 16U; ++i)
      w[i] = (static_cast<uint32_t>(p[4U * i]) << 24U) | (static_cast<uint32_t>(p[4U * i + 1U]) << 16U) |
             (static_cast<uint32_t>(p[4U * i + 2U]) << 8U) | p[4U * i + 3U];
    for (unsigned i = 16U; i < 80U; ++i) w[i] = rol(w[i - 3U] ^ w[i - 8U] ^ w[i - 14U] ^ w[i - 16U], 1U);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (unsigned i = 0; i < 80U; ++i) {
      uint32_t f, k;
      if (i < 20U) { f = (b & c) | (~b & d); k = 0x5A827999U; }
      else if (i < 40U) { f = b ^ c ^ d; k = 0x6ED9EBA1U; }
      else if (i < 60U) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCU; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6U; }
      const uint32_t t = rol(a, 5U) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30U); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  void update(const uint8_t *data, size_t length) {
    total += length;
    while (length) {
      size_t take = 64U - used;
      if (take > length) take = length;
      memcpy(block + used, data, take);
      used += static_cast<uint32_t>(take); data += take; length -= take;
      if (used == 64U) { compress(block); used = 0U; }
    }
  }
  void finish(uint8_t out[20]) {
    const uint64_t bits = total * 8U;
    const uint8_t one = 0x80U, zero = 0U;
    update(&one, 1U);
    while (used != 56U) update(&zero, 1U);
    uint8_t length[8];
    for (unsigned i = 0; i < 8U; ++i) length[i] = static_cast<uint8_t>(bits >> (56U - 8U * i));
    update(length, 8U);
    for (unsigned i = 0; i < 5U; ++i) {
      out[4U * i] = static_cast<uint8_t>(h[i] >> 24U); out[4U * i + 1U] = static_cast<uint8_t>(h[i] >> 16U);
      out[4U * i + 2U] = static_cast<uint8_t>(h[i] >> 8U); out[4U * i + 3U] = static_cast<uint8_t>(h[i]);
    }
  }
};

// Base64 of `length` bytes into out (NUL terminated). out must hold 4*ceil(length/3)+1 bytes.
inline size_t base64(const uint8_t *in, size_t length, char *out) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0U;
  for (size_t i = 0; i < length; i += 3U) {
    const uint32_t v = (static_cast<uint32_t>(in[i]) << 16U) | (i + 1U < length ? static_cast<uint32_t>(in[i + 1U]) << 8U : 0U) |
                       (i + 2U < length ? in[i + 2U] : 0U);
    out[o++] = T[(v >> 18U) & 63U]; out[o++] = T[(v >> 12U) & 63U];
    out[o++] = i + 1U < length ? T[(v >> 6U) & 63U] : '=';
    out[o++] = i + 2U < length ? T[v & 63U] : '=';
  }
  out[o] = '\0';
  return o;
}
inline char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
inline bool equalsIgnoreCase(const char *a, size_t aLength, const char *b) {
  const size_t bLength = strlen(b);
  if (aLength != bLength) return false;
  for (size_t i = 0; i < aLength; ++i) if (lower(a[i]) != lower(b[i])) return false;
  return true;
}
}  // namespace detail

constexpr size_t KEY_BASE64 = 24U;        // base64 of the 16-byte nonce
constexpr size_t ACCEPT_BASE64 = 28U;     // base64 of the 20-byte SHA-1
constexpr size_t MAX_HEADER = 8U;         // client frame header incl. mask for payloads <= 65535 B
constexpr size_t MAX_CONTROL = 125U;

// Sec-WebSocket-Key from 16 caller-supplied random bytes (NUL terminated, 25 bytes).
inline void makeKey(const uint8_t nonce[16], char key[KEY_BASE64 + 1U]) { detail::base64(nonce, 16U, key); }

// Expected Sec-WebSocket-Accept for a key (RFC 6455 section 1.3).
inline void expectedAccept(const char *key, char accept[ACCEPT_BASE64 + 1U]) {
  static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  detail::Sha1 sha;
  sha.init();
  sha.update(reinterpret_cast<const uint8_t *>(key), strlen(key));
  sha.update(reinterpret_cast<const uint8_t *>(GUID), sizeof(GUID) - 1U);
  uint8_t digest[20];
  sha.finish(digest);
  detail::base64(digest, 20U, accept);
}

// Opening request. Returns its length, or 0 when it does not fit.
inline size_t buildUpgradeRequest(char *out, size_t cap, const char *host, const char *path, const char *key) {
  static const char HEAD[] = " HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: mqtt\r\n";
  size_t pos = 0U;
  auto put = [&](const char *text) {
    const size_t length = strlen(text);
    if (pos + length >= cap) return false;
    memcpy(out + pos, text, length); pos += length; out[pos] = '\0';
    return true;
  };
  const bool ok = put("GET ") && put(path) && put(HEAD) && put("Host: ") && put(host) && put("\r\nSec-WebSocket-Key: ") &&
                  put(key) && put("\r\n\r\n");
  return ok ? pos : 0U;
}

enum class Handshake : uint8_t { Ok, BadStatus, MissingUpgrade, BadAccept, BadProtocol, ExtensionRefused, Malformed };

// Validates the server's complete response header (everything up to and including the blank line).
inline Handshake checkUpgradeResponse(const char *response, size_t length, const char *expectedAcceptValue) {
  using namespace detail;
  if (length < 12U || memcmp(response, "HTTP/1.1 101", 12U) != 0) return Handshake::BadStatus;
  bool upgrade = false, accept = false, protocolOk = true;
  size_t pos = 0U;
  while (pos < length && response[pos] != '\n') ++pos;       // skip the status line
  ++pos;
  while (pos < length) {
    size_t end = pos;
    while (end < length && response[end] != '\n') ++end;
    size_t lineEnd = end;
    if (lineEnd > pos && response[lineEnd - 1U] == '\r') --lineEnd;
    if (lineEnd == pos) break;                                // blank line: end of the header
    size_t colon = pos;
    while (colon < lineEnd && response[colon] != ':') ++colon;
    if (colon == lineEnd) return Handshake::Malformed;
    size_t valueStart = colon + 1U;
    while (valueStart < lineEnd && (response[valueStart] == ' ' || response[valueStart] == '\t')) ++valueStart;
    const char *name = response + pos; const size_t nameLength = colon - pos;
    const char *value = response + valueStart; const size_t valueLength = lineEnd - valueStart;
    if (equalsIgnoreCase(name, nameLength, "upgrade")) upgrade = equalsIgnoreCase(value, valueLength, "websocket");
    else if (equalsIgnoreCase(name, nameLength, "sec-websocket-accept")) {
      accept = valueLength == strlen(expectedAcceptValue) && memcmp(value, expectedAcceptValue, valueLength) == 0;
    } else if (equalsIgnoreCase(name, nameLength, "sec-websocket-protocol")) {
      protocolOk = equalsIgnoreCase(value, valueLength, "mqtt");
    } else if (equalsIgnoreCase(name, nameLength, "sec-websocket-extensions")) return Handshake::ExtensionRefused;
    pos = end + 1U;
  }
  if (!upgrade) return Handshake::MissingUpgrade;
  if (!accept) return Handshake::BadAccept;
  if (!protocolOk) return Handshake::BadProtocol;
  return Handshake::Ok;
}

// ---- client frames, built in place ----------------------------------------------------------
enum Opcode : uint8_t { CONTINUATION = 0x0, BINARY = 0x2, CLOSE = 0x8, PING = 0x9, PONG = 0xA };

// `buffer[headroom .. headroom+length)` holds the payload (headroom >= MAX_HEADER). Writes the
// header immediately before it, masks the payload in place and returns the start of the frame
// (>= buffer); *total is its length. Returns nullptr when the arguments are out of range.
inline uint8_t *wrapClientFrame(uint8_t *buffer, size_t headroom, size_t length, uint8_t opcode, uint32_t maskKey, size_t *total) {
  if (!buffer || !total || headroom < MAX_HEADER || length > 65535U) return nullptr;
  const size_t header = length < 126U ? 6U : 8U;
  uint8_t *p = buffer + headroom - header;
  size_t i = 0U;
  p[i++] = static_cast<uint8_t>(0x80U | (opcode & 0x0FU));          // FIN, no RSV
  if (length < 126U) p[i++] = static_cast<uint8_t>(0x80U | length);  // MASK bit
  else { p[i++] = 0x80U | 126U; p[i++] = static_cast<uint8_t>(length >> 8U); p[i++] = static_cast<uint8_t>(length); }
  const uint8_t mask[4] = {static_cast<uint8_t>(maskKey >> 24U), static_cast<uint8_t>(maskKey >> 16U),
                           static_cast<uint8_t>(maskKey >> 8U), static_cast<uint8_t>(maskKey)};
  for (unsigned m = 0; m < 4U; ++m) p[i++] = mask[m];
  uint8_t *payload = buffer + headroom;
  for (size_t n = 0; n < length; ++n) payload[n] = static_cast<uint8_t>(payload[n] ^ mask[n & 3U]);
  *total = header + length;
  return p;
}

// ---- server frames, incremental --------------------------------------------------------------
class FrameParser {
 public:
  enum class Event : uint8_t { None, Payload, Control, Fatal };
  FrameParser() { reset(); }
  void reset() { state_ = State::Byte0; inMessage_ = false; fatal_ = false; remaining_ = 0U; lengthBytes_ = 0U; control_ = false; controlLength_ = 0U; }
  bool fatal() const { return fatal_; }

  // Consumes bytes up to the end of ONE event: a slice of data payload (Event::Payload, see
  // payload()/payloadLength(), pointing into `data`), a complete control frame (Event::Control),
  // or a protocol violation (Event::Fatal). Returns the number of bytes consumed; Event::None
  // means all of `data` was consumed without completing an event.
  size_t feed(const uint8_t *data, size_t length, Event &event) {
    event = Event::None;
    size_t used = 0U;
    while (used < length && !fatal_) {
      const uint8_t byte = data[used];
      switch (state_) {
        case State::Byte0: {
          ++used;
          opcode_ = byte & 0x0FU;
          fin_ = (byte & 0x80U) != 0U;
          if ((byte & 0x70U) != 0U) return fail(event, used);          // RSV bits: no extension negotiated
          control_ = (opcode_ & 0x08U) != 0U;
          if (control_) { if (!fin_ || opcode_ > PONG || opcode_ < CLOSE) return fail(event, used); }
          else if (opcode_ == CONTINUATION) { if (!inMessage_) return fail(event, used); }
          else if (opcode_ != BINARY || inMessage_) return fail(event, used);   // text/reserved/interleaved
          state_ = State::Byte1;
          break;
        }
        case State::Byte1:
          ++used;
          if (byte & 0x80U) return fail(event, used);                  // servers never mask
          if ((byte & 0x7FU) < 126U) { remaining_ = byte & 0x7FU; if (control_ && remaining_ > MAX_CONTROL) return fail(event, used); if (!startPayload(event)) { return used; } }
          else if (control_) return fail(event, used);                 // control frames are <= 125 B
          else { state_ = State::Extended; lengthBytes_ = (byte & 0x7FU) == 126U ? 2U : 8U; remaining_ = 0U; }
          break;
        case State::Extended:
          ++used;
          if (lengthBytes_ > 4U) {                                      // upper half of a 64-bit length must be zero
            if (byte != 0U) return fail(event, used);
            --lengthBytes_;
            break;
          }
          if (remaining_ > 0x7FFFFFU) return fail(event, used);         // would exceed 2^31 after this shift
          remaining_ = (remaining_ << 8U) | byte;
          if (--lengthBytes_ == 0U && !startPayload(event)) return used;
          break;
        case State::Payload: {
          if (control_) {
            ctrl_[controlLength_++] = byte; ++used; --remaining_;
            if (remaining_ == 0U) { state_ = State::Byte0; event = Event::Control; return used; }
          } else {
            size_t take = length - used;
            if (take > remaining_) take = remaining_;
            slice_ = data + used; sliceLength_ = take;
            used += take; remaining_ -= static_cast<uint32_t>(take);
            if (remaining_ == 0U) state_ = State::Byte0;
            event = Event::Payload;
            return used;
          }
          break;
        }
      }
    }
    return used;
  }

  // The data frame currently being read still owes `count` payload bytes: the caller handed some
  // back (it stopped consuming mid-slice) and will feed them again from its own carry.
  void unconsume(size_t count) {
    if (control_ || count == 0U) return;
    remaining_ += static_cast<uint32_t>(count);
    state_ = State::Payload;
  }

  const uint8_t *payload() const { return slice_; }
  size_t payloadLength() const { return sliceLength_; }
  uint8_t controlOpcode() const { return opcode_; }
  const uint8_t *control() const { return ctrl_; }
  size_t controlLength() const { return controlLength_; }

 private:
  enum class State : uint8_t { Byte0, Byte1, Extended, Payload };
  size_t fail(Event &event, size_t used) { fatal_ = true; event = Event::Fatal; return used; }
  // Header complete. Returns false when the caller must return now (event set).
  bool startPayload(Event &event) {
    controlLength_ = 0U;
    if (remaining_ == 0U) {
      state_ = State::Byte0;
      if (control_) { event = Event::Control; return false; }
      if (fin_) inMessage_ = false; else inMessage_ = true;
      return true;                                              // empty data frame: nothing to hand out
    }
    state_ = State::Payload;
    if (!control_) inMessage_ = !fin_;
    return true;
  }

  State state_;
  bool inMessage_, fatal_, fin_ = true, control_;
  uint8_t opcode_ = 0U, lengthBytes_;
  uint32_t remaining_;
  const uint8_t *slice_ = nullptr;
  size_t sliceLength_ = 0U;
  uint8_t ctrl_[MAX_CONTROL];
  size_t controlLength_;
};

}  // namespace MayapMqttWs
