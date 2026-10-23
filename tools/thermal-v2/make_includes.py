#!/usr/bin/env python3
"""Generate the production-derived include files used by the V2 digital-twin rig (tools/thermal-v2/rig.h).
Production text is COMPILED, not re-implemented: OutputArbiter, sensor filter, sensor acceptance, High/Emergency alarm,
E115 accounting, ConditionTimer, the config struct and updateHeatingAndOutputs() itself (with the controller slot substituted).
usage: make_includes.py <out_dir>"""
import re, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
FW = ROOT / 'MAYAP_INDUSTRIAL_v1_0_0'
out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
machine = (FW / 'machine_control.h').read_text(encoding='utf-8')
config = (FW / 'config.h').read_text(encoding='utf-8')

def body_end(text, start):
    opening = text.index('{', start); depth = 0
    for i in range(opening, len(text)):
        if text[i] == '{': depth += 1
        if text[i] == '}':
            depth -= 1
            if depth == 0: return i + 1
    raise ValueError('unclosed body')
def between(text, first, last, include_last=False):
    a = text.index(first); b = text.index(last, a)
    return text[a:b + (len(last) if include_last else 0)]
def method(signature):
    s = machine.index(signature); return machine[s:body_end(machine, s)]
def const(name, src=config):
    m = re.search(r'constexpr [^;\n]*\b' + name + r'\s*=[^;]*;', src)
    if not m: raise ValueError('missing constant ' + name)
    return m[0]

# ---- OutputArbiter ----
s = machine.index('struct OutputRequest {'); e = body_end(machine, machine.index('class OutputArbiter {', s)) + 1
src = machine[s:e]
names = sorted(set(re.findall(r'\b(?:PIN_OUT_\w+|OUTPUT_ACTIVE_HIGH|OUTPUT_EVENT_QUEUE_SIZE|MAX_RELAY_TRANSITIONS_PER_HOUR|HEAT_MASTER_\w+|TURN_DIRECTION_DEADTIME_MS|RELAY_\w+_MS)\b', src)))
(out / 'actual-output.inc').write_text('\n'.join(const(n) for n in names) + '\n' + src)

# ---- sensor filter ----
s = machine.index('  void updateFilter('); e = machine.index('  void completeCycleSuccess(', s)
iir = '\n'.join(re.search(r'constexpr [^;]*\b' + n + r'\s*=[^;]*;', machine)[0] for n in ['IIR_NUMERATOR', 'IIR_DENOMINATOR'])
(out / 'actual-filter.inc').write_text('namespace SHT485Config {\n' + iir + '\n}\n'
    + 'class ActualSensorFilter { public:\n float value() const { return filteredTemp_; }\n' + machine[s:e]
    + ' private: float tempWindow_[3]{}, humWindow_[3]{}; uint8_t windowIndex_=0,windowCount_=0;\n'
    + 'float filteredTemp_=0, filteredHum_=0; bool filterInitialized_=false;\n};\n')

# ---- sensor path / protection slices ----
(out / 'actual-sensor-accept.inc').write_text('{\n' + between(machine, '      bool acceptSample = frameValid;', '    // Fail-safe freshness is owned by SHT485Industrial'))
(out / 'actual-sensor-usable.inc').write_text(
    between(machine, '    const bool transportValid = sensor_.dataValid()',
            'sensorUsable_ = transportValid && !sensorSuspect_ &&\n                    isfinite(temperature_) &&\n                    goodSensorStreak_ >= SENSOR_RECOVERY_GOOD_SAMPLES;', True).replace('sensor_.dataValid()', 'pathDataValid_'))
(out / 'actual-safety-alarm.inc').write_text(between(machine, '    const float safetyTemp = isfinite(rawTemperature_)', '    if (emergencyActive_) highTemperatureActive_ = true;', True))
(out / 'actual-alarm-edges.inc').write_text(between(machine, '    if ((!wasHigh && highTemperatureActive_) ||', '      sirenMutedUntil_ = 0;\n    }', True))
# the sensor-usable edge: rig.h re-states these 3 statements (the rest of the block only logs); fail loudly if production changes them
edge = between(machine, '    if (wasUsable != sensorUsable_) {', '      if (!sensorUsable_) postCoolUntil_ = now + POST_COOL_MS;', True)
assert 'heatRestartNotBefore_ = now + HEAT_RESTART_LOCKOUT_MS;' in edge and 'if (!sensorUsable_) postCoolUntil_ = now + POST_COOL_MS;' in edge
(out / 'actual-vent-temp.inc').write_text(between(machine, '    if (!ventTemperatureActive_ && validSafety &&', '    if (!validSafety) ventTemperatureActive_ = false;', True))
(out / 'actual-frozen.inc').write_text(between(machine, '    const uint32_t frozenEvidenceOnMs = std::max<uint32_t>', '    faults_.set(FaultCode::SensorFrozen, sensorFrozenActive'))
cls = machine.index('class ConditionTimer {')
(out / 'actual-condition-timer.inc').write_text(machine[cls:body_end(machine, cls) + 1])
sc = ['SENSOR_MAX_DOWN_STEP_C', 'SENSOR_PLAUSIBILITY_MATCH_C', 'SENSOR_PLAUSIBILITY_CONFIRM_SAMPLES', 'SENSOR_FROZEN_EPSILON_C', 'SENSOR_FROZEN_TIMEOUT_MS',
      'SENSOR_RECOVERY_GOOD_SAMPLES', 'HIGH_TEMP_CONFIRM_MS', 'HIGH_TEMP_CLEAR_HYSTERESIS_C', 'HIGH_TEMP_CLEAR_CONFIRM_MS', 'EMERGENCY_CLEAR_HYSTERESIS_C', 'EMERGENCY_CLEAR_CONFIRM_MS']
(out / 'actual-sensor-constants.inc').write_text('\n'.join(const(n) for n in sc))
(out / 'actual-heater-evidence.inc').write_text(between(machine, '    const bool responseDemand = batchRunning_', '    const bool sensorGrace'))

# ---- heater permits: the verbatim block from updateHeatingAndOutputs is part of actual-heating.inc ----
initializer = re.search(r'HeaterBurstScheduler heaterBurst_\{[^;]+;', machine)[0]
assert initializer == 'HeaterBurstScheduler heaterBurst_{HEATER_GROUP_COUNT, HEATER_BURST_QUANTUM_MS};'
(out / 'actual-burst-member.inc').write_text(initializer)

# ---- constants used by the production route ----
cn = ['CIRC_FAN_BATCH_START_STAGGER_MS', 'POST_COOL_MS', 'VENT_SCHEDULE_MAX_RUNS', 'FAN_PRESTART_MS', 'MANUAL_FAN_CAN_DISABLE_HEATING', 'HUMIDIFIER_HYSTERESIS_RH',
      'HEATER_BURST_QUANTUM_MS', 'WARMUP_VENT_HOLD_ERR_C', 'WARMUP_VENT_RELEASE_ERR_C', 'WARMUP_VENT_HOLD_MAX_MS', 'HEAT_RESTART_LOCKOUT_MS', 'CONTROL_TASK_PERIOD_MS', 'SENSOR_STARTUP_GRACE_MS']

# ---- config struct + helpers ----
cfg_start = config.index('struct MachineConfig {'); cfg_struct = config[cfg_start:body_end(config, cfg_start) + 1]
sanitize = method('inline void sanitizeMachineConfig(')
cfg_names = sorted(set(re.findall(r'\b[A-Z][A-Z0-9_]+\b', cfg_struct + sanitize)))
cc = []
for n in cfg_names + ['HEATER_BURST_QUANTUM_MS']:
    f = re.search(r'constexpr [^;\n]*\b' + n + r'\s*=[^;]*;', config)
    if f and f[0] not in cc: cc.append(f[0])
enums = '\n'.join(re.search(r'enum class ' + n + r'[^;]+;', config)[0] for n in ['ControlMode', 'TurnDirection', 'ConnectivityMode'])
cfg_defined = {re.search(r'\b([A-Z][A-Z0-9_]+)\s*=', c)[1] for c in cc}
(out / 'actual-heating-constants.inc').write_text('\n'.join(const(n) for n in cn if n not in cfg_defined))
(out / 'actual-config.inc').write_text('\n'.join(cc) + '\n' + enums + '\n' + cfg_struct + '\n' + sanitize + '\n' + method('inline uint8_t ventProfileDutyPercent(') + '\n')

# ---- the production heating route with the controller slot substituted ----
start = machine.index('  void updateHeatingAndOutputs(')
route = machine[start:body_end(machine, start)]
slot = ('    const bool v2Shadow = normalMasterPermit && heaterPidConditions && !faults_.ssrInhibited() && ventTemperatureActive_ && actuatorReady;   // blocked ONLY by the vent line\n'
        '    float commandedPower = thermalV2Command(now, normalSsrPermit && actuatorReady, v2Shadow);\n')
assert route.count(slot) == 1, 'controller slot line not found exactly once'
route = route.replace(slot, '    const bool v2Shadow = normalMasterPermit && heaterPidConditions && !faults_.ssrInhibited() && ventTemperatureActive_ && actuatorReady;\n    float commandedPower = rigSlot(now, normalSsrPermit, actuatorReady, fanStable, req, v2Shadow);   // <- the ONLY substitution (the slot)\n')
(out / 'actual-heating.inc').write_text(route)
# the production adapter glue the rig reuses for the V2 controller: textual, so a change in machine_control.h changes what the digital twin runs
(out / 'actual-v2-builder.inc').write_text(method('  ThermalV2SlotIn buildThermalV2SlotIn('))
print('includes written to', out)
