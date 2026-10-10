#!/usr/bin/env python3
"""Sensor-path integrity suite (Smart Thermal phase 1). SIMULATION ONLY.

Builds tests/thermal-sensor-path.cpp against the production-derived includes and runs every case with the alarm,
E115 and E104 logic fed from the temperature the SENSOR REPORTS (probe lag, quantisation, injected faults), not from
the plant truth. Reports three things separately (plant true / firmware saw / command) and classifies each case.

Hard gates (they can only be loosened by a reviewed change to tests/thermal-sensor-path-baseline.json):
  * unsafeCommandTicks == 0 in every case: the heater is never ON while the firmware's own inhibit/drop is asserted;
  * detectable faults (disconnect, stuck_high, spike) never produce a plant-true High;
  * ratchet: UNDETECTED_TRUE_VIOLATION, TRUE_VIOLATION_STOPPED_LATE and FALSE_TRIP_NO_FAULT never exceed the recorded
    counts (the recorded counts document KNOWN LIMITS of a single-sensor design, they are not acceptance of them);
  * with --mode smart the same suite runs for the Smart Thermal mode and must not be worse than the Adaptive V1 baseline.
"""
import argparse, collections, csv, json, os, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DETECTABLE = ('disconnect', 'stuck_high', 'spike')

def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--report-dir', type=Path, default=Path(tempfile.gettempdir()) / 'mayap-sensor-path')
    ap.add_argument('--jobs', type=int, default=min(4, os.cpu_count() or 1))
    ap.add_argument('--sanitize', action='store_true')
    ap.add_argument('--mode', choices=('adaptive', 'smart'), default='adaptive')
    ap.add_argument('--write-baseline', action='store_true', help='record the current counts (reviewed change only)')
    args = ap.parse_args()
    out = args.report_dir; out.mkdir(parents=True, exist_ok=True)
    inc = out / 'include'
    run([sys.executable, str(ROOT / 'tools/test_thermal_control.py'), '--emit-includes', str(inc)], stdout=subprocess.DEVNULL)
    binary = out / 'thermal-sensor-path'
    cmd = ['g++', '-std=c++11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(inc)]
    if args.sanitize: cmd += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
    run(cmd + [str(ROOT / 'tests/thermal-sensor-path.cpp'), '-o', str(binary)])
    files = [out / f'sensor-path.{args.mode}.{i}.csv' for i in range(args.jobs)]
    with ThreadPoolExecutor(args.jobs) as pool:
        list(pool.map(lambda i: run([str(binary), 'run', str(i), str(args.jobs), str(files[i]), args.mode]), range(args.jobs)))
    rows = []
    for f in files: rows += list(csv.DictReader(f.open()))
    rows.sort(key=lambda r: r['label'])
    merged = out / f'sensor-path.{args.mode}.csv'
    with merged.open('w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()), lineterminator='\n'); w.writeheader(); w.writerows(rows)

    verdicts = collections.Counter(r['verdict'] for r in rows)
    by_fault = collections.defaultdict(collections.Counter)
    for r in rows: by_fault[r['fault']][r['verdict']] += 1
    md = [f'# Sensor-path integrity suite ({args.mode}) -- simulation only', '',
          f'{len(rows)} cases. Verdicts: ' + ', '.join(f'{k}={v}' for k, v in sorted(verdicts.items())), '',
          '| fault | cases | verdicts | worst plant-true peak °C | longest plant-true ≥High s | worst heater-ON s while true ≥High |', '|---|---|---|---|---|---|']
    for fault in sorted(by_fault):
        fr = [r for r in rows if r['fault'] == fault]
        md.append(f"| {fault} | {len(fr)} | " + ', '.join(f'{k}={v}' for k, v in sorted(by_fault[fault].items())) +
                  f" | {max(float(r['truePeak']) for r in fr):.2f} | {max(float(r['trueHighS']) for r in fr):.0f} | {max(float(r['onWhileTrueHighS']) for r in fr):.0f} |")
    (out / f'summary.{args.mode}.md').write_text('\n'.join(md) + '\n')
    print('\n'.join(md))

    problems = []
    if sum(int(r['unsafeCommandTicks']) for r in rows): problems.append('heater ON while the firmware inhibit/drop was asserted (UNSAFE_COMMAND)')
    for r in rows:
        if r['fault'] in DETECTABLE and r['verdict'] not in ('OK', 'SAFE_UNDER_FAULT'):
            problems.append(f"{r['label']}: detectable fault produced {r['verdict']} (peak {r['truePeak']})")
    counts = {k: verdicts.get(k, 0) for k in ('UNDETECTED_TRUE_VIOLATION', 'TRUE_VIOLATION_STOPPED_LATE_BY_E115_E104',
                                              'TRUE_VIOLATION_DETECTED_LATE', 'FALSE_TRIP_NO_FAULT', 'ACTUATOR_UNSTOPPABLE_BY_FIRMWARE', 'UNSAFE_COMMAND')}
    base_path = ROOT / 'tests/thermal-sensor-path-baseline.json'
    if args.write_baseline:
        base_path.write_text(json.dumps({'cases': len(rows), 'mode': args.mode, 'ceilings': counts}, indent=2) + '\n')
        print(f'baseline written: {counts}')
    else:
        base = json.loads(base_path.read_text())
        if len(rows) != base['cases']: problems.append(f"case count {len(rows)} != recorded {base['cases']}")
        for k, v in counts.items():
            if v > base['ceilings'][k]: problems.append(f"{k}: {v} > recorded ceiling {base['ceilings'][k]}")
    if problems:
        print('\nGATE FAILURES:\n  ' + '\n  '.join(problems)); sys.exit(1)
    print('\nsensor-path gates: OK (simulation only; UNDETECTED_TRUE_VIOLATION counts are documented single-sensor limits, '
          'they require the independent hardware over-temperature protection)')

if __name__ == '__main__':
    main()
