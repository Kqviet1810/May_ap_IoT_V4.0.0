#pragma once
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static volatile bool gMayapSerialDebugEnabled = SERIAL_DEBUG_DEFAULT_ON;
inline bool mayapSerialDebugEnabled() {
  return __atomic_load_n(&gMayapSerialDebugEnabled, __ATOMIC_ACQUIRE);
}
#if MAYAP_DIAGNOSTIC_SERIAL
namespace MayapSerialInternal {
constexpr uint8_t CAPACITY = 8U;
struct Entry {
  char text[224];
  uint16_t length = 0U, offset = 0U;
  bool priority = false, force = false;
};
static Entry entries[CAPACITY];
static uint8_t count = 0U;
static bool draining = false;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t dropped = 0U, criticalDropped = 0U, truncated = 0U;
static uint32_t reportedDropped = 0U, reportedTruncated = 0U;
inline void removeEntry(uint8_t at) {
  for (uint8_t i = at; i + 1U < count; ++i) entries[i] = entries[i + 1U];
  --count;
}
}
#endif
inline void mayapSetSerialDebugEnabled(bool enabled) {
  __atomic_store_n(&gMayapSerialDebugEnabled, enabled, __ATOMIC_RELEASE);
#if MAYAP_DIAGNOSTIC_SERIAL
  if (!enabled) {
    using namespace MayapSerialInternal;
    portENTER_CRITICAL(&mux);
    // Finish an in-progress line before the forced EXIT response, not after.
    for (uint8_t i = 0U; i < count;) {
      if (!entries[i].force && !(i == 0U && (draining || entries[i].offset))) removeEntry(i);
      else ++i;
    }
    portEXIT_CRITICAL(&mux);
  }
#endif
}
inline void mayapSerialPrintf(bool force, const char *format, ...) {
#if MAYAP_DIAGNOSTIC_SERIAL
  if ((!force && !mayapSerialDebugEnabled()) || !format) return;
  using namespace MayapSerialInternal;
  Entry item;
  const int prefix = snprintf(item.text, sizeof(item.text), "[t=%lu] ", static_cast<unsigned long>(millis()));
  va_list args; va_start(args, format);
  const int length = vsnprintf(item.text + prefix, sizeof(item.text) - prefix, format, args);
  va_end(args);
  if (length <= 0) return;
  item.length = strlen(item.text);
  item.force = force;
  item.priority = force || strstr(format, "[FAULT]") || strstr(format, "[FATAL]") ||
      strstr(format, "[HEALTH]") || strstr(format, "[SUPERVISOR]") || strstr(format, "[BOOT-DIAG]");
  portENTER_CRITICAL(&mux);
  if (!force && !mayapSerialDebugEnabled()) { portEXIT_CRITICAL(&mux); return; }
  if (length >= static_cast<int>(sizeof(item.text) - prefix)) {
    ++truncated; item.text[item.length - 1U] = '\n';
  }
  if (count == CAPACITY) {
    uint8_t victim = CAPACITY;
    if (item.priority) for (uint8_t i = 0U; i < count; ++i) {
      if (!entries[i].priority && !entries[i].offset && !(i == 0U && draining)) { victim = i; break; }
    }
    if (victim == CAPACITY && force) for (uint8_t i = 0U; i < count; ++i) {
      if (entries[i].force && !entries[i].offset && !(i == 0U && draining)) { victim = i; break; }
    }
    ++dropped;
    if (victim == CAPACITY) {
      if (item.priority) ++criticalDropped;
      portEXIT_CRITICAL(&mux); return;
    }
    if (entries[victim].priority) ++criticalDropped;
    removeEntry(victim);
  }
  entries[count++] = item;
  portEXIT_CRITICAL(&mux);
#else
  (void)force; (void)format;
#endif
}
// Sole Serial writer, called by supervisorTask. No heap, waits or flush.
inline void mayapSerialDrain() {
#if MAYAP_DIAGNOSTIC_SERIAL
  using namespace MayapSerialInternal;
  for (uint8_t burst = 0U; burst < 3U; ++burst) {
    const int room = Serial.availableForWrite();
    if (room <= 0) break;
    uint8_t buffer[64]; size_t length = 0U;
    portENTER_CRITICAL(&mux);
    if (count) {
      length = entries[0].length - entries[0].offset;
      if (length > sizeof(buffer)) length = sizeof(buffer);
      if (length > static_cast<size_t>(room)) length = room;
      memcpy(buffer, entries[0].text + entries[0].offset, length);
      draining = true;
    }
    portEXIT_CRITICAL(&mux);
    if (!length) break;
    const size_t written = Serial.write(buffer, length);
    portENTER_CRITICAL(&mux);
    entries[0].offset += written;
    draining = false;
    if (entries[0].offset >= entries[0].length) removeEntry(0U);
    portEXIT_CRITICAL(&mux);
    if (!written) break;
  }
  uint32_t lost = 0U, important = 0U, shortened = 0U; bool report = false;
  portENTER_CRITICAL(&mux);
  if (!count && mayapSerialDebugEnabled() &&
      (dropped != reportedDropped || truncated != reportedTruncated)) {
    lost = dropped; important = criticalDropped; shortened = truncated;
    reportedDropped = dropped; reportedTruncated = truncated; report = true;
  }
  portEXIT_CRITICAL(&mux);
  if (report) mayapSerialPrintf(false, "[SERIAL] dropped=%lu critical=%lu truncated=%lu\n",
      static_cast<unsigned long>(lost), static_cast<unsigned long>(important), static_cast<unsigned long>(shortened));
#endif
}
// Supervisor trip only (still the sole Serial writer): empties the queue for at most
// maxMs so the real trip cause is printed before any reset. Bounded, never blocks on
// a missing USB host, and does not feed or reconfigure the TWDT.
inline void mayapSerialDrainFor(uint32_t maxMs) {
#if MAYAP_DIAGNOSTIC_SERIAL
  using namespace MayapSerialInternal;
  const uint32_t startedMs = millis();
  while (static_cast<uint32_t>(millis() - startedMs) < maxMs) {
    portENTER_CRITICAL(&mux);
    const bool pending = count != 0U;
    portEXIT_CRITICAL(&mux);
    if (!pending) break;
    mayapSerialDrain();
    vTaskDelay(1);
  }
#else
  (void)maxMs;
#endif
}
