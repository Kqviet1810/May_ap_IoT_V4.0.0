#!/usr/bin/env python3
"""Adaptive Thermal V1 simulation gate: CURRENT V4 vs ADAPTIVE V1 on identical plants.

Builds tests/thermal-adaptive-v1.cpp against the production-derived harness includes, runs the
788-case legacy matrix, the ventilation set, the hardware-change set and the sensor/disturbance
fault set, writes CSV + a markdown summary, and enforces regression gates.

This is SIMULATION evidence only ("simulation qualified"): it never replaces physical validation.
`--full` additionally runs the 2160-case factorial matrix (several minutes).
"""
import argparse, csv, collections, os, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)

def shards(binary, name, jobs, out_dir):
    files = [out_dir / f'{name}.{i}.csv' for i in range(jobs)]
    with ThreadPoolExecutor(jobs) as pool:
        list(pool.map(lambda i: run([str(binary), name, str(i), str(jobs), str(files[i])]), range(jobs)))
    merged = out_dir / f'{name}.csv'
    with merged.open('w') as out:
        for f in files:
            out.write(f.read_text())
    return list(csv.DictReader(merged.open()))

def stats(rows, mode):
    rs = [r for r in rows if r['mode'] == mode]
    reach = [r for r in rs if r['class'] == 'REACHABLE']
    f = lambda k, s: [float(r[k]) for r in s]
    return {
        'cases': len(rs), 'pass': sum(r['target'] == 'PASS' for r in rs),
        'high': sum(int(r['high']) > 0 for r in rs), 'emergency': sum(int(r['emergency']) > 0 for r in rs),
        'reachable': len(reach), 'reachable_pass': sum(r['target'] == 'PASS' for r in reach),
        'worst': {k: max(f(k, reach)) for k in ('overshoot', 'mae', 'p95', 'ripple')},
        'mean': {k: sum(f(k, reach)) / len(reach) for k in ('overshoot', 'mae', 'p95', 'ripple')},
        'classes': dict(collections.Counter((r['class'], r['target']) for r in rs)),
    }

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--report-dir', type=Path, default=Path(tempfile.gettempdir()) / 'mayap-thermal-adaptive')
    ap.add_argument('--full', action='store_true', help='also run the 2160-case factorial matrix')
    ap.add_argument('--jobs', type=int, default=min(4, os.cpu_count() or 1))
    args = ap.parse_args()
    out = args.report_dir; out.mkdir(parents=True, exist_ok=True)
    inc = out / 'include'
    run([sys.executable, str(ROOT / 'tools/test_thermal_control.py'), '--emit-includes', str(inc)], stdout=subprocess.DEVNULL)
    binary = out / 'thermal-adaptive-v1'
    run(['g++', '-std=c++11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(inc),
         str(ROOT / 'tests/thermal-adaptive-v1.cpp'), '-o', str(binary)])
    problems, md = [], ['# Adaptive Thermal V1: simulation summary (simulation only)\n']

    # ---- 788-case matrix -------------------------------------------------------------------
    rows = shards(binary, 'legacy788', args.jobs, out)
    base, v1 = stats(rows, 'BASELINE_V4'), stats(rows, 'ADAPTIVE_V1')
    md += ['## 788-case matrix', '', '| | PASS | High | Emergency | reachable PASS | worst overshoot | mean MAE | worst MAE | worst P95 | worst ripple |', '|---|---|---|---|---|---|---|---|---|---|']
    for n, s in (('V4 baseline', base), ('Adaptive V1', v1)):
        md.append(f"| {n} | {s['pass']}/{s['cases']} | {s['high']} | {s['emergency']} | {s['reachable_pass']}/{s['reachable']} | "
                  f"{s['worst']['overshoot']:.3f} | {s['mean']['mae']:.3f} | {s['worst']['mae']:.3f} | {s['worst']['p95']:.3f} | {s['worst']['ripple']:.3f} |")
    if v1['cases'] != 788 or base['cases'] != 788: problems.append('788 matrix incomplete')
    if v1['high'] or v1['emergency']: problems.append(f"788: V1 High={v1['high']} Emergency={v1['emergency']} (must be 0)")
    if v1['pass'] < 480 or v1['pass'] < 2 * base['pass']: problems.append(f"788: V1 PASS {v1['pass']} below gate (>=480 and >=2x baseline {base['pass']})")
    if v1['reachable_pass'] < 360: problems.append(f"788: V1 reachable PASS {v1['reachable_pass']} < 360")
    by = collections.defaultdict(dict)
    for r in rows: by[r['label']][r['mode']] = r
    worse = [k for k, v in by.items() if v['BASELINE_V4']['target'] == 'PASS' and v['ADAPTIVE_V1']['target'] == 'FAIL']
    md.append(f'\nCases where the baseline passes and V1 fails: {len(worse)}')
    if len(worse) > 12: problems.append(f'788: {len(worse)} regressions versus baseline (> 12)')

    # ---- ventilation ----------------------------------------------------------------------
    vrows = shards(binary, 'vent', args.jobs, out)
    agg = collections.defaultdict(list)
    for r in vrows: agg[r['mode']].append(r)
    mean = lambda rs, k: sum(float(r[k]) for r in rs) / len(rs)
    md += ['\n## Ventilation (63 scenarios x 3 modes)', '', '| mode | High | Emergency | dev max mean | dev max worst | post-vent overshoot worst | recovery worst s | heater-on mean s | integral wind-up mean | tail MAE mean |', '|---|---|---|---|---|---|---|---|---|---|']
    for m in ('BASELINE_V4', 'V1_NO_VENT_COORD', 'ADAPTIVE_V1'):
        rs = agg[m]
        md.append(f"| {m} | {sum(int(r['high'])>0 for r in rs)} | {sum(int(r['emergency'])>0 for r in rs)} | {mean(rs,'vent_dev_max'):.3f} | "
                  f"{max(float(r['vent_dev_max']) for r in rs):.3f} | {max(float(r['post_vent_overshoot_max']) for r in rs):.3f} | "
                  f"{max(float(r['recovery_s_max']) for r in rs):.0f} | {mean(rs,'heater_on_s_mean'):.0f} | {mean(rs,'integral_windup_max'):.1f} | {mean(rs,'mae_tail'):.3f} |")
    a = agg['ADAPTIVE_V1']; b = agg['BASELINE_V4']
    if any(int(r['high']) or int(r['emergency']) for r in a): problems.append('vent: V1 High/Emergency')
    if mean(a, 'integral_windup_max') > 0.5 * mean(b, 'integral_windup_max'): problems.append('vent: integral wind-up not halved')
    if mean(a, 'mae_tail') > mean(b, 'mae_tail'): problems.append('vent: tail MAE worse than baseline')
    if mean(a, 'vent_dev_max') > mean(b, 'vent_dev_max'): problems.append('vent: mean deviation worse than baseline')

    # ---- hardware change + faults ---------------------------------------------------------
    hrows = shards(binary, 'hwchange', 1, out)
    md += ['\n## Hardware change (heater 100->150 / 100->60 %, vent 1->2)', '', '| plant | change | V1 tail MAE | V1 overshoot | detected at s | mismatch events | High | Emergency |', '|---|---|---|---|---|---|---|---|']
    for r in hrows:
        if r['mode'] != 'ADAPTIVE_V1': continue
        md.append(f"| {r['plant']} | {r['change']} | {float(r['mae_tail']):.3f} | {float(r['overshoot']):.3f} | {r['t_mismatch_s']} | {r['mismatch_final']} | {r['high']} | {r['emergency']} |")
        if int(r['high']) or int(r['emergency']): problems.append(f"hwchange {r['plant']}/{r['change']}: High/Emergency")
    frows = shards(binary, 'faults', 1, out)
    nf = sum(1 for r in frows if r['mode'] == 'ADAPTIVE_V1')
    bad = [r['case'] for r in frows if r['mode'] == 'ADAPTIVE_V1' and (int(r['high']) or int(r['emergency']))]
    md += [f'\n## Sensor / disturbance faults\n\n{nf} scenarios, V1 High/Emergency cases: {len(bad)}']
    if bad: problems.append(f'faults: High/Emergency in {bad}')

    if args.full:
        mrows = shards(binary, 'matrix', args.jobs, out)
        mb, mv = stats(mrows, 'BASELINE_V4'), stats(mrows, 'ADAPTIVE_V1')
        md += ['\n## Factorial matrix (2160 cases)', '', f"baseline PASS {mb['pass']}/{mb['cases']} (High {mb['high']}, Emergency {mb['emergency']}); "
               f"V1 PASS {mv['pass']}/{mv['cases']} (High {mv['high']}, Emergency {mv['emergency']}); reachable PASS {mv['reachable_pass']}/{mv['reachable']}"]
        if mv['high'] > mb['high'] or mv['emergency'] > mb['emergency']: problems.append('matrix: V1 has more High/Emergency than baseline')
        if mv['pass'] < 2 * mb['pass']: problems.append('matrix: V1 PASS < 2x baseline')

    (out / 'summary.md').write_text('\n'.join(md) + '\n')
    print('\n'.join(md))
    if problems:
        print('\nGATE FAILURES:\n  ' + '\n  '.join(problems)); sys.exit(1)
    print('\nthermal adaptive simulation gates: OK (simulation qualified; physical validation still required)')

if __name__ == '__main__':
    main()
