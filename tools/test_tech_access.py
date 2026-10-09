#!/usr/bin/env python3
"""Host tests for the technical-access feature (technical PIN, Web permission, HMI-approved Web requests, old records).

* tests/tech-access.cpp       - the pure headers tech_access.h / advanced_history.h (SHA/HMAC vectors, PIN, lock-out, sessions, 3-slot ring)
* tests/tech-controller.cpp   - the REAL MachineController glue sliced out of machine_control.h (handleTechCommand, submitTechRequest,
                                executeTechPending, applyAdvancedSnapshot, restoreAdvanced, serviceTech...) running on fakes for the
                                EEPROM/config save. The functions are extracted textually, never re-implemented.
Simulation / host evidence only: it proves the logic, not the I2C EEPROM timing or the LCD on the real board.
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / 'MAYAP_INDUSTRIAL_v1_0_0'


def between(text, start, end):
    a = text.index(start)
    b = text.index(end, a)
    return text[a:b]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cxx', default='g++')
    args = parser.parse_args()
    machine = (FW / 'machine_control.h').read_text(encoding='utf-8')
    config = (FW / 'config.h').read_text(encoding='utf-8')
    with tempfile.TemporaryDirectory(prefix='mayap-tech-') as tmp:
        out = Path(tmp)
        (out / 'actual-tech-config.inc').write_text(between(config, 'enum class TechResult', 'struct MachineRuntime {'), encoding='utf-8')
        (out / 'actual-hmi-command-type.inc').write_text(between(config, 'enum class HmiCommandType : uint8_t {', '// F-09 (audit'), encoding='utf-8')
        (out / 'actual-tech-members.inc').write_text(between(machine, '  // A Web technical request waits here', '  uint32_t techRetryAt_ = 0;') + '  uint32_t techRetryAt_ = 0;\n', encoding='utf-8')
        (out / 'actual-tech-funcs.inc').write_text(between(machine, '  // Keep the advanced values that are about to be replaced', '  // Read-only PID monitor.'), encoding='utf-8')
        for name in ('tech-access', 'tech-controller'):
            exe = out / name
            cmd = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(out), '-I', str(FW),
                   str(ROOT / 'tests' / (name + '.cpp')), '-o', str(exe)]
            if args.sanitize:
                cmd += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            subprocess.run(cmd, check=True)
            subprocess.run([str(exe)], check=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
