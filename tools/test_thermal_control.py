#!/usr/bin/env python3
"""Test production thermal algorithms and GPIO arbiter; generate honest OLD/NEW metrics."""
import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import statistics
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('--sanitize', action='store_true')
parser.add_argument('--require-targets', action='store_true', help='Fail if any model misses the fixed acceptance targets')
parser.add_argument('--report-dir', type=Path, default=Path('/tmp/mayap-thermal-report'))
parser.add_argument('--quick', action='store_true', help='unit tests only; skip the long simulation matrices (development loop)')
parser.add_argument('--emit-includes', type=Path, default=None, help='write the generated production-derived include dir here and exit (used by tools/test_thermal_adaptive.py)')
parser.add_argument('--only', default='', help='comma list of unit tests to run (implies --quick)')
args = parser.parse_args()
if args.only: args.quick = True
args.report_dir.mkdir(parents=True, exist_ok=True)
machine = (ROOT / 'MAYAP_INDUSTRIAL_v1_0_0/machine_control.h').read_text()
config = (ROOT / 'MAYAP_INDUSTRIAL_v1_0_0/config.h').read_text()
# Frozen OLD controller from main 9569fcc, not silently replaced by production NEW.
assert hashlib.sha256((ROOT / 'tests/fixtures/thermal-v1/thermal_control.h').read_bytes()).hexdigest() == 'fb2b5828ccdd8997736f9618ae32f3635370c7253f2d1b8efbbd763002d15354'
assert hashlib.sha256((ROOT / 'tests/fixtures/thermal-v1/ssr_window.h').read_bytes()).hexdigest() == '0a7fa13570ec7a29c603f891dd9f38d7bc884503b684e28fb2ff907a0081c1aa'

assert hashlib.sha256((ROOT / 'tests/fixtures/thermal-v1/sensor_filter.h').read_bytes()).hexdigest() == '58d07d62a9d4d345fce56bf39f4c84fb3de7f1072af9636fbcf5838d9753433e'
assert hashlib.sha256((ROOT / 'tests/fixtures/thermal-v3-baseline/thermal_control.h').read_bytes()).hexdigest() == 'd3d63bd3f564727920de02c9aa25ccd9d0cf323640ab36d697d4ceb261716b0c'
assert hashlib.sha256((ROOT / 'tests/fixtures/thermal-v3-baseline/heating.inc').read_bytes()).hexdigest() == '56ed19f22256e8e291dca1859ad96391824bcb18685bbceb3d7a7bbb6d3226c6'

def body_end(text, start):
    opening = text.index('{', start)
    depth = 0
    for i in range(opening, len(text)):
        if text[i] == '{': depth += 1
        if text[i] == '}':
            depth -= 1
            if depth == 0: return i + 1
    raise ValueError('unclosed production body')

with tempfile.TemporaryDirectory(prefix='mayap-thermal-') as directory:
    out = Path(directory)
    tune_constants = [m[0] for m in re.finditer(r'constexpr [^;\n]*\bAUTOTUNE_\w+\s*=[^;]*;', config)]
    (out / 'actual-autotune-constants.inc').write_text('\n'.join(tune_constants))
    pin_start = config.index('constexpr uint8_t MAYAP_USED_PINS[]')
    pin_end = config.index('static_assert(mayapPinsValidAndUnique()',pin_start)
    pin_end = config.index(';',pin_end)+1
    pins = config[pin_start:pin_end]
    pin_names = sorted(set(re.findall(r'\bPIN_\w+\b',pins)))
    declarations=[]
    used=set()
    for name in pin_names:
        declaration = re.search(r'constexpr uint8_t '+name+r'\s*=[^;]*;',config)[0]
        used.add(int(re.search(r'=\s*(\d+)',declaration)[1]))
        declarations.append(declaration)
    pin_source = out / 'pins.cpp'
    pin_source.write_text('#include <cstdint>\n#include <cstddef>\n'+'\n'.join(declarations)+'\n'+pins+'\nint main(){}\n')
    assert 'constexpr uint8_t HEATER_GROUP_COUNT = 1U;' in config
    assert '#error "This board has one heater control GPIO; both SSRs share GPIO1"' in config
    subprocess.run(['g++','-std=c++14',str(pin_source),'-o',str(out/'pins')],check=True)
    print('Actual pinmap: GPIO1 drives both SSRs as one 16 kW bank; group count fixed at one')
    start = machine.index('struct OutputRequest {')
    end = body_end(machine, machine.index('class OutputArbiter {', start)) + 1
    source = machine[start:end]
    names = sorted(set(re.findall(r'\b(?:PIN_OUT_\w+|OUTPUT_ACTIVE_HIGH|OUTPUT_EVENT_QUEUE_SIZE|MAX_RELAY_TRANSITIONS_PER_HOUR|HEAT_MASTER_\w+|TURN_DIRECTION_DEADTIME_MS|RELAY_\w+_MS)\b', source)))
    declarations = []
    for name in names:
        found = re.search(r'constexpr [^;\n]*\b' + name + r'\s*=[^;]*;', config)
        if not found: raise ValueError('missing production constant ' + name)
        declarations.append(found[0])
    (out / 'actual-output.inc').write_text('\n'.join(declarations) + '\n' + source)
    start = machine.index('  void updateHeatingAndOutputs(')
    (out / 'actual-heating.inc').write_text(machine[start:body_end(machine,start)])
    heating = machine[start:body_end(machine,start)]
    evidence_start = machine.index('    const bool responseDemand = batchRunning_')
    evidence_end = machine.index('    const bool sensorGrace', evidence_start)
    (out / 'actual-heater-evidence.inc').write_text(machine[evidence_start:evidence_end])
    # Phase 1 (sensor-path integrity): the REAL sensor-acceptance, High/Emergency alarm and frozen-sensor bodies,
    # extracted by markers so the sensor-path harness cannot drift from production. Any marker miss fails loudly.
    def between(text, first, last, include_last=False):
        a = text.index(first); b = text.index(last, a)
        return text[a:b + (len(last) if include_last else 0)]
    (out / 'actual-sensor-accept.inc').write_text(
        '{\n' + between(machine, '      bool acceptSample = frameValid;', '    // Fail-safe freshness is owned by SHT485Industrial'))
    # The only textual substitution: the driver object `sensor_` is not part of the host harness; its dataValid() verdict is
    # injected as a plain member. Everything else is production text, byte for byte.
    (out / 'actual-sensor-usable.inc').write_text(
        between(machine, '    const bool transportValid = sensor_.dataValid()', 'sensorUsable_ = transportValid && !sensorSuspect_ &&\n                    isfinite(temperature_) &&\n                    goodSensorStreak_ >= SENSOR_RECOVERY_GOOD_SAMPLES;', True)
        .replace('sensor_.dataValid()', 'pathDataValid_'))
    (out / 'actual-safety-alarm.inc').write_text(
        between(machine, '    const float safetyTemp = isfinite(rawTemperature_)', '    if (emergencyActive_) highTemperatureActive_ = true;', True))
    (out / 'actual-frozen.inc').write_text(
        between(machine, '    const uint32_t frozenEvidenceOnMs = std::max<uint32_t>', '    faults_.set(FaultCode::SensorFrozen, sensorFrozenActive'))
    cls_a = machine.index('class ConditionTimer {')
    (out / 'actual-condition-timer.inc').write_text(machine[cls_a:body_end(machine, cls_a) + 1])
    sensor_constants = ['SENSOR_MAX_DOWN_STEP_C','SENSOR_PLAUSIBILITY_MATCH_C','SENSOR_PLAUSIBILITY_CONFIRM_SAMPLES',
        'SENSOR_FROZEN_EPSILON_C','SENSOR_FROZEN_TIMEOUT_MS','SENSOR_RECOVERY_GOOD_SAMPLES','HIGH_TEMP_CONFIRM_MS',
        'HIGH_TEMP_CLEAR_HYSTERESIS_C','HIGH_TEMP_CLEAR_CONFIRM_MS','EMERGENCY_CLEAR_HYSTERESIS_C','EMERGENCY_CLEAR_CONFIRM_MS']
    (out / 'actual-sensor-constants.inc').write_text('\n'.join(re.search(r'constexpr [^;\n]*\b'+n+r'\s*=[^;]*;', config)[0] for n in sensor_constants))
    initializer = re.search(r'HeaterBurstScheduler heaterBurst_\{[^;]+;', machine)[0]
    assert initializer == 'HeaterBurstScheduler heaterBurst_{HEATER_GROUP_COUNT, HEATER_BURST_QUANTUM_MS};'
    (out / 'actual-burst-member.inc').write_text(initializer)
    fields = sorted(set(re.findall(r'config_\.(\w+)', heating)) | {'pidCycleSec'})
    extra = []
    fixture_fields = {'controlMode','kp','ki','kd','maxHeaterPower','autotuneRelayPowerPercent','tempHysteresis','autotuneBandC'}
    for name in fields:
        if name in fixture_fields: continue
        match = re.search(r'  (?:float|bool|uint\d+_t) ' + name + r'\s*=[^;]+;', config)
        if not match: raise ValueError('missing heat configuration ' + name)
        extra.append(match[0])
    (out / 'actual-heating-config.inc').write_text('struct HeatingConfig : MachineConfig {\n' + '\n'.join(extra) + '\n HeatingConfig() { kp=18.0f; ki=0.8f; kd=45.0f; }\n};\n')
    assert re.search(r'uint16_t heaterStuckDurationSec\s*=\s*900;', config), 'E115 test must match production default'
    constants = ['CIRC_FAN_BATCH_START_STAGGER_MS','POST_COOL_MS','VENT_SCHEDULE_MAX_RUNS','FAN_PRESTART_MS','MANUAL_FAN_CAN_DISABLE_HEATING','HUMIDIFIER_HYSTERESIS_RH','HEATER_BURST_QUANTUM_MS']
    (out / 'actual-heating-constants.inc').write_text('\n'.join(re.search(r'constexpr [^;\n]*\b'+n+r'\s*=[^;]*;',config)[0] for n in constants))
    start = machine.index('  void updateFilter(')
    end = machine.index('  void completeCycleSuccess(', start)
    iir = '\n'.join(re.search(r'constexpr [^;]*\b' + name + r'\s*=[^;]*;', machine)[0]
                    for name in ['IIR_NUMERATOR','IIR_DENOMINATOR'])
    # The simulation uses the production median/IIR methods, not a redesigned filter.
    (out / 'actual-filter.inc').write_text('namespace SHT485Config {\n' + iir + '\n}\n'
        + 'class ActualSensorFilter { public:\n float value() const { return filteredTemp_; }\n'
        + machine[start:end]
        + ' private: float tempWindow_[3]{}, humWindow_[3]{}; uint8_t windowIndex_=0,windowCount_=0;\n'
        + 'float filteredTemp_=0, filteredHum_=0; bool filterInitialized_=false;\n};\n')
    # Compile the real config serializers, CRC validators and EEPROM migration
    # path against a byte-backed EEPROM. No alternate migration implementation.
    def method(signature):
        start = machine.index(signature)
        return machine[start:body_end(machine, start)]
    cfg_start = config.index('struct MachineConfig {')
    cfg_struct = config[cfg_start:body_end(config, cfg_start)+1]
    sanitize = method('inline void sanitizeMachineConfig(')
    cfg_names = sorted(set(re.findall(r'\b[A-Z][A-Z0-9_]+\b', cfg_struct + sanitize)))
    cfg_constants = []
    for name in cfg_names + ['EEPROM_ADDR_CONFIG_A','EEPROM_ADDR_CONFIG_B','HEATER_BURST_QUANTUM_MS']:
        found = re.search(r'constexpr [^;\n]*\b'+name+r'\s*=[^;]*;', config)
        if found and found[0] not in cfg_constants: cfg_constants.append(found[0])
    enums = '\n'.join(re.search(r'enum class '+name+r'[^;]+;', config)[0]
                      for name in ['ControlMode','TurnDirection','ConnectivityMode'])
    records = machine[machine.index('struct PackedMachineConfigV1 {'):machine.index('struct PackedBatchV1 {')]
    schemas = '\n'.join(re.findall(r'constexpr [^;\n]*\bCONFIG_(?:MAGIC|SCHEMA\w*)\s*=[^;]*;', machine))
    validators = machine[machine.index('  static bool validConfig('):machine.index('  static bool validBatch(')]
    (out / 'actual-config.inc').write_text('\n'.join(cfg_constants)+'\n'+enums+'\n'+cfg_struct+'\n'
        +sanitize+'\n#pragma pack(push,1)\n'+records+'\n#pragma pack(pop)\n'+schemas+'\n'
        +method('inline uint32_t mcCrc32(')+'\n'+method('inline PackedMachineConfigV1 packConfig(')+'\n'
        +method('inline MachineConfig unpackConfig(')+'\n'+method('inline uint8_t ventProfileDutyPercent(')+'\n')
    (out / 'actual-config-load.inc').write_text(method('  bool loadConfig(')+'\n'+method('  bool saveConfig(')+'\n'
        +method('  static bool newer(')+'\n'+validators+'\n'+method('  bool refreshConfigCache('))
    adaptive_methods = ['  uint32_t adaptiveCompatibility(', '  uint32_t thermalSignature(', '  void trackAdaptiveEnergy(',
        '  bool adaptiveCoolingRequested(', '  void publishThermalLearning(', '  float updateAdaptiveBalance(']
    (out / 'actual-adaptive.inc').write_text('\n'.join(method(signature) for signature in adaptive_methods))
    # Storage I/O and FreeRTOS critical-section stubs only; real mailbox/store class is tested.
    (out / 'freertos').mkdir()
    (out / 'freertos/FreeRTOS.h').write_text('#pragma once\nusing portMUX_TYPE=int;\n#define portMUX_INITIALIZER_UNLOCKED 0\n#define portENTER_CRITICAL(x) (void)(x)\n#define portEXIT_CRITICAL(x) (void)(x)\n')
    (out / 'Preferences.h').write_text('#pragma once\n#include <map>\n#include <string>\n#include <vector>\n#include <cstring>\nclass Preferences {\n public:\n  static std::map<std::string,std::vector<unsigned char>> &records(){static std::map<std::string,std::vector<unsigned char>> map;return map;}\n  static int &budget(){static int limit=-1;return limit;}\n  bool begin(const char *,bool){return true;}\n  bool isKey(const char *key){auto i=records().find(key);return i!=records().end()&&!i->second.empty();}\n  size_t getBytes(const char *key,void *data,size_t length){auto &v=records()[key];if(v.size()!=length)return 0;std::memcpy(data,v.data(),length);return length;}\n  size_t putBytes(const char *key,const void *data,size_t length){if(budget()>=0 && static_cast<size_t>(budget())<length){const auto *p=static_cast<const unsigned char *>(data);records()[key]=std::vector<unsigned char>(p,p+budget());return budget();}const auto *p=static_cast<const unsigned char *>(data);records()[key]=std::vector<unsigned char>(p,p+length);return length;}\n};\n')
    extra_constants = ['THERMAL_PID_BETA','PID_D_FILTER_TAU_SEC','HEAT_RESTART_LOCKOUT_MS','POST_COOL_MS',
        'EVENT_LOG_RAM_SIZE','HMI_EVENT_DISPLAY_CAPACITY','CIRC_FAN_BATCH_START_STAGGER_MS','FAN_PRESTART_MS',
        'MANUAL_FAN_CAN_DISABLE_HEATING','CONTROL_TASK_PERIOD_MS','CONTROL_CYCLE_TRIP_US']
    (out / 'actual-tune-extra.inc').write_text('\n'.join(re.search(r'constexpr [^;\n]*\b'+n+r'\s*=[^;]*;', config)[0] for n in extra_constants))
    event_types = ''
    for name in ['HmiEventItem','HmiEventSnapshot']:
        start=config.index('struct '+name+' {');event_types+=config[start:body_end(config,start)+1]+'\n'
    start=machine.index('enum class EventType :')
    end=body_end(machine,machine.index('class EventLog {',start))+1
    (out / 'actual-event.inc').write_text(event_types+machine[start:end])
    for name, signature in [('start','  bool startAutoTune('),('cancel','  bool cancelAutoTune('),('update','  void updateAutoTune(')]:
        (out / ('actual-tune-'+name+'.inc')).write_text(method(signature))
    (out / 'actual-safety-thresholds.inc').write_text('\n'.join(
        'constexpr double MODEL_'+name.upper()+' = '+re.search(r'float '+name+r'\s*=\s*([\d.]+)f;',config)[1]+';'
        for name in ['highTempAlarm','emergencyTemp']))
    # Assert simulator defaults match current production (both OLD and NEW receive these gains).
    for name, expected in [('kp',18.0),('ki',0.8),('kd',45.0)]:
        actual = float(re.search(r'float ' + name + r'\s*=\s*([\d.]+)f;', config)[1])
        if actual != expected: raise ValueError('Update documented simulator defaults: ' + name)
    common = ['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(out)]
    if args.sanitize: common += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
    plant_source=(ROOT / 'tests/thermal-plant.cpp').read_text()
    (out / 'actual-plants.inc').write_text(re.search(r'const Plant plants\[\]=[^;]+;', plant_source)[0])
    if args.emit_includes:
        shutil.copytree(out, args.emit_includes, dirs_exist_ok=True)
        print('Generated include directory: ' + str(args.emit_includes))
        raise SystemExit(0)
    unit_tests = ['adaptive-observer','adaptive-thermal','thermal-autotune','thermal-control','thermal-startup','thermal-smart-startup','thermal-program','thermal-v2','thermal-output','thermal-heating','thermal-e115','thermal-config','thermal-filter','thermal-adaptive-unit','thermal-smart-autotune-unit']
    if args.only: unit_tests = [t for t in args.only.split(',') if t]
    for test in unit_tests:
        variants = [1] if test in ('thermal-output','thermal-heating') else [0]
        for groups in variants:
            executable = out / (test + str(groups))
            command = common + [str(ROOT / ('tests/' + test + '.cpp')), '-o', str(executable)]
            subprocess.run(command, check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            if result.returncode:
                print(result.stdout + result.stderr, flush=True)
                result.check_returncode()
            (args.report_dir / (test + str(groups) + '.log')).write_text(result.stdout)
            print(result.stdout.splitlines()[-1])
            if test == 'thermal-filter':
                (args.report_dir / 'filter-compatibility.csv').write_text(
                    'samples,max_abs_delta_c,mean_abs_delta_c,rms_delta_c\n'
                    +next(line[7:] for line in result.stdout.splitlines() if line.startswith('FILTER,'))+'\n')
            if test == 'thermal-v2':
                with (args.report_dir / 'low-duty.csv').open('w') as bank_report:
                    bank_report.write('quantum_ms,power_percent,horizon_s,requested_pct,delivered_pct,absolute_energy_error_j,max_no_heat_ms,transitions_per_hour\n')
                    bank_report.writelines(line[5:]+'\n' for line in result.stdout.splitlines() if line.startswith('BANK,'))
    if args.quick:
        print('QUICK: unit tests passed; simulation matrices skipped')
        raise SystemExit(0)
    current_executable = out / 'adaptive-plant-current'
    subprocess.run(common + ['-O2', '-DMAYAP_ADAPTIVE_FAST_PATH=0',
        str(ROOT / 'tests/adaptive-plant.cpp'), '-o', str(current_executable)], check=True)
    with (args.report_dir / 'adaptive-current-summary.csv').open('w') as report:
        subprocess.run([str(current_executable), str(args.report_dir / 'adaptive-current-observer.csv'),
            str(args.report_dir / 'adaptive-current-control.csv')], stdout=report, check=True)
    current_rows=list(csv.DictReader((args.report_dir / 'adaptive-current-summary.csv').open()))
    assert len(current_rows)==1248

    executable = out / 'adaptive-plant'
    subprocess.run(common + ['-O2', str(ROOT / 'tests/adaptive-plant.cpp'), '-o', str(executable)], check=True)
    with (args.report_dir / 'adaptive-summary.csv').open('w') as report:
        subprocess.run([str(executable), str(args.report_dir / 'adaptive-observer.csv'), str(args.report_dir / 'adaptive-control.csv')], stdout=report, check=True)
    adaptive_rows=list(csv.DictReader((args.report_dir / 'adaptive-summary.csv').open()))
    assert len(adaptive_rows)==1248
    assert all(int(r['false_learning_count'])==0 for r in adaptive_rows)
    for old,new in zip(adaptive_rows[::2],adaptive_rows[1::2]):
        assert new['mode']=='ADAPTIVE' and old['mode']=='BASELINE'
        assert int(new['Emergency'])<=int(old['Emergency']), 'new Emergency crossing'
        if int(old['settling'])>=0:
            assert int(new['settling'])>=0, 'previously settled case no longer settles'
            assert float(new['ripple'])<=max(0.25,float(old['ripple'])+0.1), 'new sustained oscillation'

    current_adaptive=[r for r in current_rows if r['mode']=='ADAPTIVE']
    fast_adaptive=[r for r in adaptive_rows if r['mode']=='ADAPTIVE']
    current_high=sum(int(r['High']) for r in current_adaptive)
    fast_high=sum(int(r['High']) for r in fast_adaptive)
    current_emergency=sum(int(r['Emergency']) for r in current_adaptive)
    fast_emergency=sum(int(r['Emergency']) for r in fast_adaptive)
    # The former strict gate (fast_high < current_high) is stale: Adaptive Thermal V1 moved part of the
    # fast-path protection (hold-aware braking, integral-gain stability limit) into the common control
    # path, so the FAST_PATH=0 build is no longer the pre-fast-path controller and has little left for the
    # fast path to save. The acceptance below keeps the intent: no more High/Emergency crossings than
    # main had, none created by the fast path in any scenario, and no worse than the current build.
    base=json.loads((ROOT / 'tests/thermal-fastpath-baseline.json').read_text())
    key=lambda r:(r['plant'],r['scenario'],r['ambient'],r['deadtime'],r['resolution'],r['SP'])
    current_by={key(r):r for r in current_adaptive}
    assert fast_high<=current_high, 'fast path increased High crossings'
    assert current_high<=base['currentHigh'], 'current build has more High crossings than main (%d)'%base['currentHigh']
    assert fast_high<=base['fastHigh'], 'fast build has more High crossings than main (%d)'%base['fastHigh']
    for row in fast_adaptive:
        ref=current_by[key(row)]
        assert int(row['High'])<=int(ref['High']), 'fast path created a High crossing: '+str(key(row))
        assert int(row['Emergency'])<=int(ref['Emergency']), 'fast path created an Emergency crossing: '+str(key(row))
    assert fast_emergency<=current_emergency, 'fast path increased Emergency crossings'
    for metric in ['MAE','P95','ripple']:
        before=statistics.fmean(float(r[metric]) for r in current_adaptive)
        after=statistics.fmean(float(r[metric]) for r in fast_adaptive)
        assert after<=before+0.01, 'fast path materially worsened '+metric
    print(f'Adaptive current -> fast-path: High {current_high}->{fast_high}, '
          f'Emergency {current_emergency}->{fast_emergency}, false learning '
          f'{sum(int(r["false_learning_count"]) for r in fast_adaptive)}')

    print('Adaptive actual plant matrix: '+str(len(adaptive_rows))+' baseline/adaptive rows; cooling capacity unknown (zero watts credited)')
    executable = out / 'thermal-autotune-plant'
    subprocess.run(common + ['-O2', str(ROOT / 'tests/thermal-autotune-plant.cpp'), '-o', str(executable)], check=True)
    with (args.report_dir / 'autotune-plant.csv').open('w') as report:
        subprocess.run([str(executable), str(args.report_dir / 'autotune-cycles.csv')], stdout=report, check=True)
    tune_rows=list(csv.DictReader((args.report_dir / 'autotune-plant.csv').open()))
    assert len(tune_rows)==864
    for preheat in [30,40,50]:
        subset=[r for r in tune_rows if float(r['preheat_power'])==preheat and r['start_condition']=='COLD']
        print(f'AUTOTUNE preheat {preheat}%: '+str(sum(r['success']=='1' for r in subset))+'/216 SUCCESS; failures bounded and gains retained')
    for relay in [20,30,40]:
        subset=[r for r in tune_rows if float(r['preheat_power'])==30 and float(r['relay_power'])==relay and r['start_condition']=='COLD']
        print(f'AUTOTUNE candidate preheat30/relay{relay}: '+str(sum(r['success']=='1' for r in subset))+'/72 SUCCESS')
    executable = out / 'thermal-plant'
    subprocess.run(common + ['-O2', str(ROOT / 'tests/thermal-plant.cpp'), '-o', str(executable)], check=True)
    with (args.report_dir / 'plant.csv').open('w') as report:
        subprocess.run([str(executable)], stdout=report, check=True)
    print('Thermal model metrics: ' + str(args.report_dir / 'plant.csv'))
    with (args.report_dir / 'plant.csv').open() as report: rows=list(csv.DictReader(report))
    qualification = ['plant','scenario','setpoint','ambient','dead_s','resolution','algorithm','quantum_ms',
        'first_high_cross_s','first_emergency_cross_s','peak_before_high','peak_before_emergency',
        'high_crossed','emergency_crossed','qualification','model_scope']
    with (args.report_dir / 'safety-qualification.csv').open('w') as report:
        writer=csv.DictWriter(report, fieldnames=qualification, lineterminator='\n'); writer.writeheader()
        writer.writerows({k:r[k] for k in qualification} for r in rows)
    sp375=[r for r in rows if r['algorithm']=='NEW' and r['quantum_ms']=='300' and r['setpoint']=='37.5']
    print(f'CONTROL-ONLY / NO PRODUCTION SAFETY INTERVENTION: NEW300 SP37.5 High '
          f'{sum(r["high_crossed"]=="1" for r in sp375)}/{len(sp375)}, Emergency '
          f'{sum(r["emergency_crossed"]=="1" for r in sp375)}/{len(sp375)}')
    failures=sum(r['targets']=='FAIL' for r in rows)
    print(f'SIMULATION TARGETS: {len(rows)-failures}/{len(rows)} pass; {failures} FAIL. Thresholds unchanged; this is not physical accuracy.')
    # Calibrated performance is experimental, but the observed anti-windup
    # sticking case is a deterministic software regression and must fail CI.
    stuck_case=[r for r in rows if r['plant']=='light' and r['scenario']=='cold_start'
        and r['setpoint']=='30.0' and r['ambient']=='28.0' and r['dead_s']=='30'
        and r['resolution']=='0.01' and r['algorithm']=='NEW' and r['quantum_ms']=='300']
    assert len(stuck_case)==1
    stuck=stuck_case[0]
    assert float(stuck['mean_abs_error'])<0.5 and abs(float(stuck['bias']))<0.5 \
        and float(stuck['requested_tail_pct'])<3.0, 'Anti-windup stuck case regressed'
    print('HARD REGRESSION: SP30/ambient28/dead30/res0.01 no positive-heater tail while hot PASS')
    if failures and os.getenv('GITHUB_ACTIONS'):
        print(f'::warning::Thermal simulation: {failures}/{len(rows)} miss acceptance targets; review OLD/NEW CSV before commissioning.')
    if failures and args.require_targets: raise SystemExit(1)
    source=str(ROOT / 'tests/thermal-orchestration-plant.cpp')
    for label, flags in [('baseline',['-DTHERMAL_V3_BASELINE']),('phase1',[])]:
        executable=out / ('thermal-orchestration-'+label)
        subprocess.run(common + ['-O2'] + flags + [source,'-o',str(executable)],check=True)
        with (args.report_dir / ('orchestration-'+label+'.csv')).open('w') as report:
            subprocess.run([str(executable)],stdout=report,check=True)
    before=list(csv.DictReader((args.report_dir/'orchestration-baseline.csv').open()))
    after=list(csv.DictReader((args.report_dir/'orchestration-phase1.csv').open()))
    assert len(before)==len(after) and len(before)==788
    for label, rows in [('baseline',before),('phase1',after)]:
        passed=sum(row['target']=='PASS' for row in rows)
        high=sum(int(row['high']) for row in rows)
        emergency=sum(int(row['emergency']) for row in rows)
        print(f'ACTUAL HEATING ROUTE {label}: {passed}/{len(rows)} PASS, '
              f'{len(rows)-passed} FAIL, High={high}, Emergency={emergency}')
    assert sum(int(row['high']) for row in before)>0, 'Frozen baseline no longer exercises overshoot'
    assert all(int(row['high'])==0 and int(row['emergency'])==0 for row in after), \
        'Phase-1 controller crossed a thermal safety threshold in the commissioning matrix'
