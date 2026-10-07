#pragma once
// MQTT 3.1.1 wire format for the firmware's own client (mqtt_transport.h). Pure C++:
// no Arduino/FreeRTOS types, no heap, no hidden task. It is host-tested against the
// broker's own codec (tests/mqtt-native-wire.test.cjs + tests/mqtt-wire.cpp).
//
// Scope is exactly what doc/MQTT_CONTRACT.md needs from a device: CONNECT (clean
// session, LWT, username/password), SUBSCRIBE, PUBLISH QoS0/1, PUBACK, PINGREQ,
// DISCONNECT out; CONNACK, SUBACK, PUBLISH, PUBACK, PINGRESP in.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace MayapMqttWire {

enum PacketType : uint8_t {
  CONNECT = 1, CONNACK = 2, PUBLISH = 3, PUBACK = 4, SUBSCRIBE = 8, SUBACK = 9,
  PINGREQ = 12, PINGRESP = 13, DISCONNECT = 14,
};

// Encoders write into [out, out+cap) and return the packet length, or 0 when the
// packet does not fit / an argument is out of range (the caller never sends it).
namespace detail {
inline bool putVarLen(uint8_t *out, size_t cap, size_t &pos, size_t value) {
  if (value > 268435455U) return false;
  do {
    uint8_t digit = static_cast<uint8_t>(value % 128U);
    value /= 128U;
    if (value) digit |= 0x80U;
    if (pos >= cap) return false;
    out[pos++] = digit;
  } while (value);
  return true;
}
inline size_t varLenBytes(size_t value) { return value < 128U ? 1U : value < 16384U ? 2U : value < 2097152U ? 3U : 4U; }
inline bool putU16(uint8_t *out, size_t cap, size_t &pos, size_t value) {
  if (value > 65535U || pos + 2U > cap) return false;
  out[pos++] = static_cast<uint8_t>(value >> 8U);
  out[pos++] = static_cast<uint8_t>(value);
  return true;
}
inline bool putBytes(uint8_t *out, size_t cap, size_t &pos, const void *data, size_t length) {
  if (pos + length > cap) return false;
  if (length) memcpy(out + pos, data, length);
  pos += length;
  return true;
}
inline bool putString(uint8_t *out, size_t cap, size_t &pos, const char *text) {
  const size_t length = text ? strlen(text) : 0U;
  return putU16(out, cap, pos, length) && putBytes(out, cap, pos, text, length);
}
}  // namespace detail

struct ConnectArgs {
  const char *clientId;
  const char *username;     // required by this contract
  const char *password;     // required by this contract
  const char *willTopic;    // LWT, QoS1 retained (contract section 2)
  const uint8_t *willPayload;
  size_t willLength;
  uint16_t keepaliveSec;    // contract: 30..120
};

inline size_t encodeConnect(uint8_t *out, size_t cap, const ConnectArgs &a) {
  using namespace detail;
  if (!a.clientId || !a.username || !a.password || !a.willTopic || !a.willPayload) return 0U;
  const size_t remaining = 2U + 4U + 1U + 1U + 2U + 2U + strlen(a.clientId) + 2U + strlen(a.willTopic) +
                           2U + a.willLength + 2U + strlen(a.username) + 2U + strlen(a.password);
  size_t pos = 0U;
  if (cap < 1U) return 0U;
  out[pos++] = 0x10U;
  if (!putVarLen(out, cap, pos, remaining)) return 0U;
  // Protocol "MQTT" level 4; flags: user|pass|will retain|will QoS1|will|clean session.
  if (!putString(out, cap, pos, "MQTT") || pos >= cap) return 0U;
  out[pos++] = 4U;
  if (pos >= cap) return 0U;
  out[pos++] = 0x80U | 0x40U | 0x20U | 0x08U | 0x04U | 0x02U;
  if (!putU16(out, cap, pos, a.keepaliveSec) || !putString(out, cap, pos, a.clientId) ||
      !putString(out, cap, pos, a.willTopic) || !putU16(out, cap, pos, a.willLength) ||
      !putBytes(out, cap, pos, a.willPayload, a.willLength) || !putString(out, cap, pos, a.username) ||
      !putString(out, cap, pos, a.password)) return 0U;
  return pos;
}

struct Subscription { const char *filter; uint8_t qos; };

inline size_t encodeSubscribe(uint8_t *out, size_t cap, uint16_t packetId, const Subscription *subs, size_t count) {
  using namespace detail;
  if (!subs || count == 0U || packetId == 0U) return 0U;
  size_t remaining = 2U;
  for (size_t i = 0; i < count; ++i) {
    if (!subs[i].filter || subs[i].qos > 1U) return 0U;
    remaining += 2U + strlen(subs[i].filter) + 1U;
  }
  size_t pos = 0U;
  if (cap < 1U) return 0U;
  out[pos++] = 0x82U;  // SUBSCRIBE, mandatory flags 0b0010
  if (!putVarLen(out, cap, pos, remaining) || !putU16(out, cap, pos, packetId)) return 0U;
  for (size_t i = 0; i < count; ++i) {
    if (!putString(out, cap, pos, subs[i].filter) || pos >= cap) return 0U;
    out[pos++] = subs[i].qos;
  }
  return pos;
}

inline size_t encodePublish(uint8_t *out, size_t cap, const char *topic, const uint8_t *payload, size_t length,
                            uint8_t qos, bool retain, uint16_t packetId) {
  using namespace detail;
  if (!topic || qos > 1U || (qos == 1U && packetId == 0U) || (length && !payload)) return 0U;
  const size_t topicLength = strlen(topic);
  if (topicLength == 0U) return 0U;
  const size_t remaining = 2U + topicLength + (qos ? 2U : 0U) + length;
  size_t pos = 0U;
  if (cap < 1U) return 0U;
  out[pos++] = static_cast<uint8_t>(0x30U | (qos << 1U) | (retain ? 1U : 0U));
  if (!putVarLen(out, cap, pos, remaining) || !putString(out, cap, pos, topic)) return 0U;
  if (qos && !putU16(out, cap, pos, packetId)) return 0U;
  return putBytes(out, cap, pos, payload, length) ? pos : 0U;
}

inline size_t encodePuback(uint8_t *out, size_t cap, uint16_t packetId) {
  if (cap < 4U) return 0U;
  out[0] = 0x40U; out[1] = 2U; out[2] = static_cast<uint8_t>(packetId >> 8U); out[3] = static_cast<uint8_t>(packetId);
  return 4U;
}
inline size_t encodePingreq(uint8_t *out, size_t cap) { if (cap < 2U) return 0U; out[0] = 0xC0U; out[1] = 0U; return 2U; }
inline size_t encodeDisconnect(uint8_t *out, size_t cap) { if (cap < 2U) return 0U; out[0] = 0xE0U; out[1] = 0U; return 2U; }

// Incremental parser for the broker -> device direction. Bounded: one packet body lives
// in the caller's buffer; a longer body is skipped byte by byte (never stored) and the
// packet is reported as `truncated` so the caller can drop it. A malformed Remaining
// Length (> 4 bytes) is fatal: the caller must drop the connection.
class StreamParser {
 public:
  StreamParser(uint8_t *buffer, size_t capacity) : buf_(buffer), cap_(capacity) { reset(); }
  void reset() { state_ = Header; remaining_ = 0U; multiplier_ = 1U; lengthBytes_ = 0U; got_ = 0U; ready_ = false; fatal_ = false; truncated_ = false; }
  bool fatal() const { return fatal_; }
  // Valid after feed() returned with ready()==true, until the next feed()/reset().
  bool ready() const { return ready_; }
  uint8_t type() const { return static_cast<uint8_t>(header_ >> 4U); }
  uint8_t flags() const { return static_cast<uint8_t>(header_ & 0x0FU); }
  const uint8_t *body() const { return buf_; }
  size_t bodyLength() const { return truncated_ ? 0U : static_cast<size_t>(remaining_); }
  bool truncated() const { return truncated_; }

  // Consumes at most one packet. Returns the number of bytes consumed from `data`.
  size_t feed(const uint8_t *data, size_t length) {
    if (ready_) { ready_ = false; state_ = Header; remaining_ = 0U; multiplier_ = 1U; lengthBytes_ = 0U; got_ = 0U; truncated_ = false; }
    size_t used = 0U;
    while (used < length && !ready_ && !fatal_) {
      const uint8_t byte = data[used];
      switch (state_) {
        case Header:
          header_ = byte; ++used; state_ = Length; remaining_ = 0U; multiplier_ = 1U; lengthBytes_ = 0U;
          break;
        case Length:
          ++used;
          remaining_ += static_cast<uint32_t>(byte & 0x7FU) * multiplier_;
          multiplier_ *= 128U;
          if (++lengthBytes_ > 4U || (lengthBytes_ == 4U && (byte & 0x80U))) { fatal_ = true; break; }
          if (!(byte & 0x80U)) {
            got_ = 0U;
            truncated_ = remaining_ > cap_;
            if (remaining_ == 0U) ready_ = true; else state_ = Body;
          }
          break;
        case Body: {
          size_t take = length - used;
          const size_t left = remaining_ - got_;
          if (take > left) take = left;
          if (!truncated_) memcpy(buf_ + got_, data + used, take);
          got_ += static_cast<uint32_t>(take);
          used += take;
          if (got_ == remaining_) ready_ = true;
          break;
        }
      }
    }
    return used;
  }

 private:
  enum State : uint8_t { Header, Length, Body };
  uint8_t *buf_;
  size_t cap_;
  State state_;
  uint8_t header_ = 0U, lengthBytes_ = 0U;
  uint32_t remaining_, multiplier_, got_;
  bool ready_, fatal_, truncated_;
};

// PUBLISH body -> topic/payload views (no copies). False when malformed.
struct PublishView {
  const uint8_t *topic; size_t topicLength;
  const uint8_t *payload; size_t payloadLength;
  uint8_t qos; bool retain; uint16_t packetId;
};
inline bool parsePublish(uint8_t flags, const uint8_t *body, size_t length, PublishView &out) {
  if (length < 2U) return false;
  const size_t topicLength = (static_cast<size_t>(body[0]) << 8U) | body[1];
  const uint8_t qos = (flags >> 1U) & 0x03U;
  if (qos > 1U || topicLength == 0U || 2U + topicLength + (qos ? 2U : 0U) > length) return false;
  size_t pos = 2U + topicLength;
  out.topic = body + 2U; out.topicLength = topicLength; out.qos = qos; out.retain = (flags & 1U) != 0U;
  out.packetId = 0U;
  if (qos) { out.packetId = static_cast<uint16_t>((body[pos] << 8U) | body[pos + 1U]); pos += 2U; }
  out.payload = body + pos; out.payloadLength = length - pos;
  return true;
}

}  // namespace MayapMqttWire
