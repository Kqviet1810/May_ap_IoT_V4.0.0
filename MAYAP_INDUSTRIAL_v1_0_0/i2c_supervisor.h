#pragma once
#include "config.h"
#include <Wire.h>

namespace MayapI2cSupervisor {
static uint32_t failures[3] = {}, lastError[3] = {};
static uint32_t epoch = 0U;
static uint32_t lastCheckAt = 0U, lastRecoveryAt = 0U;
static bool recoveredBefore = false;
inline uint8_t index(uint8_t address) {
  return address == LCD_I2C_ADDRESS ? 0U : address == RTC_I2C_ADDRESS ? 1U : 2U;
}
}
inline void mayapI2cReport(uint8_t address, bool ok) {
  using namespace MayapI2cSupervisor;
  const uint8_t i = index(address);
  if (ok) __atomic_store_n(&failures[i], 0U, __ATOMIC_RELEASE);
  else {
    __atomic_store_n(&lastError[i], millis(), __ATOMIC_RELEASE);
    uint32_t n = __atomic_load_n(&failures[i], __ATOMIC_ACQUIRE);
    if (n < 255U) __atomic_store_n(&failures[i], n + 1U, __ATOMIC_RELEASE);
  }
}
inline void mayapI2cBusClearEarly() {
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, OUTPUT_OPEN_DRAIN);
  digitalWrite(PIN_I2C_SCL, HIGH);
  delayMicroseconds(5);
  for (uint8_t pulse = 0U; pulse < 9U && digitalRead(PIN_I2C_SDA) == LOW; ++pulse) {
    digitalWrite(PIN_I2C_SCL, LOW); delayMicroseconds(5);
    digitalWrite(PIN_I2C_SCL, HIGH); delayMicroseconds(5);
    if (digitalRead(PIN_I2C_SCL) == LOW) break;
  }
  pinMode(PIN_I2C_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(PIN_I2C_SDA, LOW); delayMicroseconds(5);
  digitalWrite(PIN_I2C_SCL, HIGH); delayMicroseconds(5);
  digitalWrite(PIN_I2C_SDA, HIGH); delayMicroseconds(5);
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, INPUT_PULLUP);
}
inline uint32_t mayapI2cRecoveryEpoch() {
  return __atomic_load_n(&MayapI2cSupervisor::epoch, __ATOMIC_ACQUIRE);
}
inline void mayapI2cSupervisorUpdate(uint32_t now) {
  using namespace MayapI2cSupervisor;
  if (static_cast<uint32_t>(now - lastCheckAt) < 1000U) return;
  lastCheckAt = now;
  if (recoveredBefore && static_cast<uint32_t>(now - lastRecoveryAt) < 30000U) return;
  if (!mayapI2cLock(0U)) return; // Never wait behind a storage transaction.
  uint8_t failedDevices = 0U;
  for (uint8_t i = 0U; i < 3U; ++i) {
    if (__atomic_load_n(&failures[i], __ATOMIC_ACQUIRE) >= 3U &&
        static_cast<uint32_t>(now - __atomic_load_n(&lastError[i], __ATOMIC_ACQUIRE)) < 10000U) ++failedDevices;
  }
  const bool linesStuck = digitalRead(PIN_I2C_SDA) == LOW || digitalRead(PIN_I2C_SCL) == LOW;
  if (!linesStuck && failedDevices < 2U) { mayapI2cUnlock(); return; }
  recoveredBefore = true;
  lastRecoveryAt = now;
  Wire.end();
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, OUTPUT_OPEN_DRAIN);
  digitalWrite(PIN_I2C_SCL, HIGH);
  for (uint8_t pulse = 0U; pulse < 9U && digitalRead(PIN_I2C_SDA) == LOW; ++pulse) {
    digitalWrite(PIN_I2C_SCL, LOW); delayMicroseconds(5);
    digitalWrite(PIN_I2C_SCL, HIGH); delayMicroseconds(5);
    // A device holding SCL low is a physical fault; do not loop indefinitely.
    if (digitalRead(PIN_I2C_SCL) == LOW) break;
  }
  pinMode(PIN_I2C_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(PIN_I2C_SDA, LOW); delayMicroseconds(5);
  digitalWrite(PIN_I2C_SCL, HIGH); delayMicroseconds(5);
  digitalWrite(PIN_I2C_SDA, HIGH); delayMicroseconds(5);
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, INPUT_PULLUP);
  const bool clear = digitalRead(PIN_I2C_SDA) == HIGH && digitalRead(PIN_I2C_SCL) == HIGH;
  const bool begun = Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_CLOCK_HZ);
  Wire.setTimeOut(I2C_TIMEOUT_MS);
  uint8_t present = 0U;
  const uint8_t addresses[] = {LCD_I2C_ADDRESS, RTC_I2C_ADDRESS, EEPROM_I2C_ADDRESS};
  if (clear && begun) {
    for (uint8_t address : addresses) {
      Wire.beginTransmission(address);
      const bool ok = Wire.endTransmission(true) == 0U;
      mayapI2cReport(address, ok);
      if (ok) ++present;
    }
  }
  __atomic_add_fetch(&epoch, 1U, __ATOMIC_ACQ_REL);
  mayapI2cUnlock();
  // Device owners perform their normal reinit/readback validation. No fault
  // is cleared here and a missing/shorted device never requests ESP restart.
  mayapSerialPrintf(false, "[I2C-RECOVERY] clear=%u begin=%u present=%u/3\n", clear, begun, present);
}
