#pragma once
// Sensor-path integrity harness (Smart Thermal phase 1).
//
// The legacy matrices derive High/Emergency (and the controller's inhibit) from the PLANT TRUE temperature.
// This harness derives them exactly as the firmware does: from the temperature the SENSOR REPORTS.
// Production text is compiled, not re-implemented (see tools/test_thermal_control.py --emit-includes):
//   * sensor acceptance (plausibility filter, suspect latch, frozen reference)   -> actual-sensor-accept.inc
//   * sensorUsable_ / safetySampleValid_                                       -> actual-sensor-usable.inc
//   * High (1 s confirm) / Emergency alarm on max(raw, filtered)               -> actual-safety-alarm.inc
//   * E115 HeaterNotHeating accounting                                         -> actual-heater-evidence.inc
//   * E104 SensorFrozen                                                         -> actual-frozen.inc
// Only the plant, the probe lag, the fault injection and the fault -> inhibit mapping are host code.
#include "actual-sensor-constants.inc"
#include "actual-condition-timer.inc"

namespace sim {

// Fault kinds the sensor can present while the protocol frame stays "valid" (except Disconnect).
// SsrStuckOn / SsrContactorStuck are ACTUATOR faults (the probe stays healthy): the SSR conducts although commanded OFF
// (the master contactor still opens), or both conduct. The firmware has no feedback input for either.
enum class SensorFault : uint8_t { None, Disconnect, StuckLow, StuckHigh, Frozen, Spike, Drift, SsrStuckOn, SsrContactorStuck };
inline const char *sensorFaultName(SensorFault f) {
  static const char *n[] = {"none", "disconnect", "stuck_low", "stuck_high", "frozen_value", "spike", "drift_low", "ssr_stuck_on", "ssr_contactor_stuck"};
  return n[static_cast<uint8_t>(f)];
}

struct SensorPath {
  bool enabled = false;
  double lagS = 0;                    // first-order probe/air-pocket response time constant
  SensorFault fault = SensorFault::None;
  double faultAtS = 5400;
  double faultDurationS = 1e9;        // Disconnect / Spike: how long it lasts
  double faultValue = 0;              // StuckLow/High: reported value; Spike: added degC; Drift: degC per hour (reads LOW)
  float tempOffset = 0;               // user calibration offset (negative must never lower the safety temperature)
};

struct PathHarness : TuneHarness {
  ConditionTimer highTripTimer_{}, highClearTimer_{}, emergencyClearTimer_{};
  bool safetySampleValid_ = false, latestFrameValid_ = false, pathDataValid_ = false;
  bool sensorSuspect_ = false;
  uint8_t sensorPlausibilityStreak_ = 0, goodSensorStreak_ = 0;
  float lastSuspectCandidate_ = NAN, lastAcceptedTemperature_ = NAN, sensorFrozenRefTemp_ = NAN;
  uint32_t sensorFrozenSince_ = 0;
  bool heaterStuckTracking_ = false, heaterNotHeatingActive_ = false, sensorFrozenLatched_ = false;
  uint32_t heaterStuckSinceAt_ = 0, heaterStuckAccumOnMs_ = 0;
  float heaterStuckStartTemp_ = NAN;

  // processSensor() acceptance block for one valid frame.
  void pathAccept(uint32_t now, float candidateTemp, bool frameValid) {
#include "actual-sensor-accept.inc"
  }
  void pathUsable() {
#include "actual-sensor-usable.inc"
  }
  // updateAlarms(): High / Emergency from the reported temperature.
  void pathAlarms(uint32_t now) {
#include "actual-safety-alarm.inc"
    (void)wasHigh; (void)wasEmergency;
  }
  // updateFaults(): E115 and E104 accounting (needs the actual SSR state of the previous output update).
  void pathEvidence(uint32_t now) {
#include "actual-heater-evidence.inc"
#include "actual-frozen.inc"
    sensorFrozenLatched_ = sensorFrozenActive;
  }
  // Fault table rows with inhibit-SSR + drop-master (see FAULT_TABLE): SensorLost/Invalid/Suspect, SensorFrozen,
  // HeaterNotHeating, HighTemperature, EmergencyTemperature.
  void pathApplyFaults(bool externalCut) {
    const bool stop = !sensorUsable_ || sensorFrozenLatched_ || heaterNotHeatingActive_ ||
                      highTemperatureActive_ || emergencyActive_ || externalCut;
    faults_.inhibit = stop;
    faults_.drop = stop;
    faults_.cooling = highTemperatureActive_ || emergencyActive_;   // sensor-lost cooling is production code (sensorFaultNeedsFan)
  }
};

}  // namespace sim
