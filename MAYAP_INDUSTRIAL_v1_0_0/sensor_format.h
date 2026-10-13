#pragma once

// Paired Modbus register profile. Register order remains RH, temperature.
enum class SensorProfile : uint8_t {
  Auto = 0, X10RhX10 = 1, X100RhX10 = 2, Sht30Native = 3
};

class SensorFormatDecoder {
 public:
  explicit SensorFormatDecoder(SensorProfile configured = SensorProfile::Auto)
      : configured_(configured) {}

  static float decodeTemperature(uint16_t raw, SensorProfile profile) {
    // X10/X100 registers use signed two's complement; native SHT30 is unsigned.
    const int32_t signedRaw = raw < 0x8000U ? raw : static_cast<int32_t>(raw) - 65536;
    switch (profile) {
      case SensorProfile::X10RhX10: return signedRaw * 0.1f;
      case SensorProfile::X100RhX10: return signedRaw * 0.01f;
      case SensorProfile::Sht30Native: return -45.0f + 175.0f * raw / 65535.0f;
      default: return NAN;
    }
  }

  static float decodeHumidity(uint16_t raw, SensorProfile profile) {
    if (profile == SensorProfile::Sht30Native) return 100.0f * raw / 65535.0f;
    if (profile == SensorProfile::X10RhX10 || profile == SensorProfile::X100RhX10)
      return raw * 0.1f;
    return NAN;
  }

  // HMI sensor-resolution choice: 0 = auto-detect (unchanged), 1 = 0.1 C (X10), 2 = 0.01 C (X100). A change forgets the locked format, so the
  // sensor must be verified again with the new choice before it can be used.
  void setRuntimeMode(uint8_t mode) {
    const SensorProfile want = mode == 1U ? SensorProfile::X10RhX10 : mode == 2U ? SensorProfile::X100RhX10 : SensorProfile::Auto;
    if (configured_ != SensorProfile::Auto || want == runtime_) return;
    runtime_ = want;
    profile_ = SensorProfile::Auto; candidateMask_ = 0U; consecutive_ = 0U; valid_ = false; hasLast_ = false;
  }
  bool accept(uint16_t rawTemperature, uint16_t rawHumidity) {
    valid_ = false;
    // A compile-time profile keeps its old behaviour. A chosen resolution drops the guessing (one candidate) and needs two matching samples;
    // it keeps the 10..60 C start envelope, so a wrong choice (x10 error) is rejected instead of read as 3.75 C or 250 C.
    const SensorProfile chosen = configured_ != SensorProfile::Auto ? configured_ : runtime_;
    const bool runtimeChoice = configured_ == SensorProfile::Auto && runtime_ != SensorProfile::Auto;
    if (!locked()) {
      uint8_t candidates = 0U;
      for (uint8_t i = 1U; i <= 3U; ++i) {
        const auto profile = static_cast<SensorProfile>(i);
        if (chosen != SensorProfile::Auto && profile != chosen) continue;
        const float t = decodeTemperature(rawTemperature, profile);
        const float rh = decodeHumidity(rawHumidity, profile);
        // Raw-only autodetection is ambiguous outside this commissioning envelope.
        // Explicit verified module profiles can start anywhere in the old -40..60 C range.
        const float minimum = configured_ == SensorProfile::Auto ? 10.0f : -40.0f;   // a runtime choice has configured_ == Auto: 10 C
        if (t >= minimum && t <= 60.0f && rh >= 0.0f && rh <= 100.0f)
          candidates |= static_cast<uint8_t>(1U << i);
      }
      if (!candidates || (candidates & (candidates - 1U))) { rejectSample(); return false; }
      if (candidates != candidateMask_) { candidateMask_ = candidates; consecutive_ = 0U; }
      if (++consecutive_ < (runtimeChoice ? 2U : 6U)) return false;
      for (uint8_t i = 1U; i <= 3U; ++i)
        if (candidates == (1U << i)) profile_ = static_cast<SensorProfile>(i);
    }
    const float t = decodeTemperature(rawTemperature, profile_);
    const float rh = decodeHumidity(rawHumidity, profile_);
    if (!isfinite(t) || t < -40.0f || t > 60.0f ||
        !isfinite(rh) || rh < 0.0f || rh > 100.0f ||
        (hasLast_ && lastTemperature_ - t > 20.0f)) {
      rejectSample();
      return false; // Never change scale, even through loss/recovery of UART.
    }
    // Never suppress a plausible upward raw step: EmergencyHigh must see it
    // immediately, independently of median/IIR lag or PID plausibility gates.
    temperature_ = lastTemperature_ = t;
    humidity_ = rh;
    hasLast_ = valid_ = true;
    return true;
  }

  void rejectSample() { valid_ = false; consecutive_ = 0U; candidateMask_ = 0U; }
  bool locked() const { return profile_ != SensorProfile::Auto; }
  bool valid() const { return locked() && valid_; }
  SensorProfile profile() const { return profile_; }
  float temperature() const { return valid() ? temperature_ : NAN; }
  float humidity() const { return valid() ? humidity_ : NAN; }

 private:
  const SensorProfile configured_;
  SensorProfile runtime_ = SensorProfile::Auto;   // HMI choice (only used when no compile-time profile is set)
  SensorProfile profile_ = SensorProfile::Auto;
  uint8_t candidateMask_ = 0U;
  uint8_t consecutive_ = 0U;
  bool valid_ = false;
  bool hasLast_ = false;
  float lastTemperature_ = 0.0f;
  float temperature_ = NAN;
  float humidity_ = NAN;
};
