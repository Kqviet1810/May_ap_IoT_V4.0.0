#!/usr/bin/env python3
"""Host tests of the production code that stays after Thermal Control V2.0.0 removed the old controller:
GPIO arbiter, heater burst scheduler, E115 heater-evidence watchdog, config serializers, sensor filter, set-point programme.
There is NO controller test here: V2 brings its own."""
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
parser.add_argument('--report-dir', type=Path, default=Path('/tmp/mayap-heater-path-report'))
parser.add_argument('--only', default='', help='comma list of unit tests to run')
args = parser.parse_args()
args.report_dir.mkdir(parents=True, exist_ok=True)
machine = (ROOT / 'MAYAP_INDUSTRIAL_v1_0_0/machine_control.h').read_text()
config = (ROOT / 'MAYAP_INDUSTRIAL_v1_0_0/config.h').read_text()
# Frozen reference of the sensor filter (v1), kept to prove the production filter stays compatible with it.
assert hashlib.sha256((ROOT / 'tests/fixtures/thermal-v1/sensor_filter.h').read_bytes()).hexdigest() == '58d07d62a9d4d345fce56bf39f4c84fb3de7f1072af9636fbcf5838d9753433e'

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
    evidence_start = machine.index('    const bool responseDemand = batchRunning_')
    evidence_end = machine.index('    const bool sensorGrace', evidence_start)
    (out / 'actual-heater-evidence.inc').write_text(machine[evidence_start:evidence_end])
    initializer = re.search(r'HeaterBurstScheduler heaterBurst_\{[^;]+;', machine)[0]
    assert initializer == 'HeaterBurstScheduler heaterBurst_{HEATER_GROUP_COUNT, HEATER_BURST_QUANTUM_MS};'
    (out / 'actual-burst-member.inc').write_text(initializer)
    assert re.search(r'uint16_t heaterStuckDurationSec\s*=\s*900;', config), 'E115 test must match production default'
    constants = ['CIRC_FAN_BATCH_START_STAGGER_MS','POST_COOL_MS','VENT_SCHEDULE_MAX_RUNS','FAN_PRESTART_MS','MANUAL_FAN_CAN_DISABLE_HEATING','HUMIDIFIER_HYSTERESIS_RH','HEATER_BURST_QUANTUM_MS','WARMUP_VENT_HOLD_ERR_C','WARMUP_VENT_RELEASE_ERR_C','WARMUP_VENT_HOLD_MAX_MS']
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
    # Storage I/O and FreeRTOS critical-section stubs only; real mailbox/store class is tested.
    (out / 'freertos').mkdir()
    (out / 'freertos/FreeRTOS.h').write_text('#pragma once\nusing portMUX_TYPE=int;\n#define portMUX_INITIALIZER_UNLOCKED 0\n#define portENTER_CRITICAL(x) (void)(x)\n#define portEXIT_CRITICAL(x) (void)(x)\n')
    (out / 'Preferences.h').write_text('#pragma once\n#include <map>\n#include <string>\n#include <vector>\n#include <cstring>\nclass Preferences {\n public:\n  static std::map<std::string,std::vector<unsigned char>> &records(){static std::map<std::string,std::vector<unsigned char>> map;return map;}\n  static int &budget(){static int limit=-1;return limit;}\n  bool begin(const char *,bool){return true;}\n  bool isKey(const char *key){auto i=records().find(key);return i!=records().end()&&!i->second.empty();}\n  size_t getBytes(const char *key,void *data,size_t length){auto &v=records()[key];if(v.size()!=length)return 0;std::memcpy(data,v.data(),length);return length;}\n  size_t putBytes(const char *key,const void *data,size_t length){if(budget()>=0 && static_cast<size_t>(budget())<length){const auto *p=static_cast<const unsigned char *>(data);records()[key]=std::vector<unsigned char>(p,p+budget());return budget();}const auto *p=static_cast<const unsigned char *>(data);records()[key]=std::vector<unsigned char>(p,p+length);return length;}\n};\n')
    event_types = ''
    for name in ['HmiEventItem','HmiEventSnapshot']:
        start=config.index('struct '+name+' {');event_types+=config[start:body_end(config,start)+1]+'\n'
    start=machine.index('enum class EventType :')
    end=body_end(machine,machine.index('class EventLog {',start))+1
    (out / 'actual-event.inc').write_text(event_types+machine[start:end])
    common = ['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(out)]
    if args.sanitize: common += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
    unit_tests = ['thermal-program','heat-latch','program-store','output-budget','thermal-output','thermal-e115','thermal-config','thermal-filter']
    if args.only: unit_tests = [t for t in args.only.split(',') if t]
    # Every test is an independent executable: compile and run them on all cores, print in the original order.
    from concurrent.futures import ThreadPoolExecutor
    pool = ThreadPoolExecutor(max_workers=max(2, os.cpu_count() or 2))
    def unit_job(test, groups):
        executable = out / (test + str(groups))
        subprocess.run(common + [str(ROOT / ('tests/' + test + '.cpp')), '-o', str(executable)], check=True)
        return subprocess.run([str(executable)], capture_output=True, text=True)
    unit_futures = []
    for test in unit_tests:
        variants = [1] if test == 'thermal-output' else [0]
        for groups in variants:
            unit_futures.append((test, groups, pool.submit(unit_job, test, groups)))
    for test, groups, future in unit_futures:
        result = future.result()
        if result.returncode:
            print(result.stdout + result.stderr, flush=True)
            result.check_returncode()
        (args.report_dir / (test + str(groups) + '.log')).write_text(result.stdout)
        print(result.stdout.splitlines()[-1])
        if test == 'thermal-filter':
            (args.report_dir / 'filter-compatibility.csv').write_text(
                'samples,max_abs_delta_c,mean_abs_delta_c,rms_delta_c\n'
                +next(line[7:] for line in result.stdout.splitlines() if line.startswith('FILTER,'))+'\n')
    print('HEATER PATH: %d host tests passed' % len(unit_tests))
