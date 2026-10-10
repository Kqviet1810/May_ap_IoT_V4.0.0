#!/usr/bin/env python3
"""Sensor-integrity study of the heating route (simulation only; see tests/thermal-sensor-integrity.cpp).

Builds the harness from the production-derived includes (the REAL High/Emergency trip block, ConditionTimer and the E115 energy block),
injects sensor faults the firmware cannot tell from a healthy reading, and writes a CSV of what the PLANT did versus what the firmware
knew. It does not claim any case is safe. It enforces a RATCHET: the measured software-only gaps may shrink, never grow.
"""
import argparse, csv, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Baseline measured at 8d51432 (main). A later phase that closes a gap lowers these numbers in the same commit.
RATCHET = {'low_reading_scenarios': 72, 'true_over_emergency': 55, 'never_cut_and_over_emergency': 24}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--report-dir', type=Path, default=Path(tempfile.gettempdir()) / 'mayap-thermal-integrity')
    ap.add_argument('--update-ratchet', action='store_true', help='print the measured numbers and exit 0 (use when a gap was closed on purpose)')
    args = ap.parse_args()
    out = args.report_dir; out.mkdir(parents=True, exist_ok=True)
    inc = out / 'include'
    subprocess.run([sys.executable, str(ROOT / 'tools/test_thermal_control.py'), '--emit-includes', str(inc)], check=True, stdout=subprocess.DEVNULL)
    exe = out / 'thermal-sensor-integrity'
    subprocess.run(['g++', '-std=c++11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(inc), str(ROOT / 'tests/thermal-sensor-integrity.cpp'), '-o', str(exe)], check=True)
    csv_path = out / 'sensor-integrity.csv'
    subprocess.run([str(exe), str(csv_path)], check=True)
    rows = list(csv.DictReader(csv_path.open()))
    low = [r for r in rows if r['fault'] not in ('NONE', 'STUCK_HIGH_45C')]
    over = [r for r in low if float(r['true_peak_c']) >= 39.0]
    never = [r for r in over if r['firmware_first_cut'] == 'NONE']
    measured = {'low_reading_scenarios': len(low), 'true_over_emergency': len(over), 'never_cut_and_over_emergency': len(never)}
    problems = []
    high = [r for r in rows if r['fault'] == 'STUCK_HIGH_45C']
    if any(r['firmware_first_cut'] != 'EMERGENCY_TRIP' or float(r['true_peak_c']) > 38.0 for r in high):
        problems.append('a sensor stuck HIGH must trip Emergency immediately and the plant must stay below High')
    if args.update_ratchet:
        print(measured); return
    for k, v in measured.items():
        if k == 'low_reading_scenarios':
            if v != RATCHET[k]: problems.append(f'scenario count changed: {v} != {RATCHET[k]}')
        elif v > RATCHET[k]: problems.append(f'{k}: {v} > ratchet {RATCHET[k]} (a software-only safety gap GREW)')
    print('sensor-integrity (software only):', measured, '| ratchet:', RATCHET)
    if problems:
        print('FAIL\n  ' + '\n  '.join(problems)); sys.exit(1)
    print('sensor-integrity ratchet OK (simulation only: gaps documented in doc/thermal/PHASE1_SENSOR_INTEGRITY.md, not closed)')

if __name__ == '__main__':
    main()
