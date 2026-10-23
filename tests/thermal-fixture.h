#pragma once
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
using std::isfinite;

enum class ControlMode : uint8_t { OnOff, Pid };
struct MachineConfig {
  ControlMode controlMode = ControlMode::Pid;
  float targetTemp = 37.5f;
  float kp = 20, ki = 0.04f, kd = 60;
  uint8_t maxHeaterPower = 100, autotuneRelayPowerPercent = 30;
  float tempHysteresis = 0.2f, autotuneBandC = 0.2f;
};
constexpr uint32_t SSR_MIN_ON_MS = 300, SSR_MIN_OFF_MS = 300;
constexpr float THERMAL_PID_BETA = 1.0f;
constexpr float PI = 3.14159265358979323846f;
constexpr float PID_D_FILTER_TAU_SEC = 5.0f;
uint32_t elapsedMs(uint32_t now, uint32_t then) { return now - then; }
float clampFloat(float x, float lo, float hi) { return std::max(lo, std::min(hi, x)); }
void sanitizeMachineConfig(MachineConfig &) {}
