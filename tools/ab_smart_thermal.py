#!/usr/bin/env python3
"""A/B/C comparison for the Smart Thermal program. SIMULATION ONLY.

  A  legacy V4 PID            (mode BASELINE_V4, adaptive OFF)
  B  Adaptive Thermal V1      (mode ADAPTIVE_V1,  the frozen reference of audit/smart-thermal/baseline)
  C  Smart Thermal            (mode SMART_THERMAL, Adaptive V1 + the Smart startup/learning changes under test)

Same case generator, plant, set point, ambient, duration, sensor model and oracle for all three (the oracle is the unchanged
`sim::Result::pass`). B and A are re-run from the CURRENT source and must reproduce the frozen baseline CSVs byte for byte
(reproducibility check); C is run per case id. Outputs (all under --report-dir):
  ab-788.csv ab-2160.csv   one row per case id: A/B/C verdicts and metrics + heuristic fail reason of C
  ab-vent.csv ab-hwchange.csv ab-faults.csv, summary.md
Fail reasons are a HEURISTIC read of the CSV metrics (not a proof of cause); UNKNOWN_ROOT_CAUSE is used when nothing matches.
"""
import argparse, collections, csv, hashlib, os, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'audit/smart-thermal/baseline'

def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)

def sharded(binary, suite, jobs, out):
    files = [out / f'{suite}.{i}.csv' for i in range(jobs)]
    with ThreadPoolExecutor(jobs) as pool:
        list(pool.map(lambda i: run([str(binary), suite, str(i), str(jobs), str(files[i])]), range(jobs)))
    merged = out / f'{suite}.csv'
    with merged.open('w') as o:
        for f in files: o.write(f.read_text())
    return merged

def rows(path): return list(csv.DictReader(Path(path).open()))

def reason(r):
    """Primary fail reason of one row (heuristic, from the row's own metrics)."""
    if r['target'] == 'PASS': return ''
    if int(r['high']) or int(r['emergency']): return 'UNSAFE_TRUE_TEMP'
    if r['class'] == 'HEAT_LIMITED': return 'PHYSICAL_POWER_LIMIT'
    if r['class'] == 'COOLING_REQUIRED': return 'PHYSICAL_COOLING_LIMIT'
    if float(r['overshoot']) > 0.3: return 'STARTUP_OVERSHOOT'
    if int(float(r['settling'])) < 0 and float(r['mae']) <= 0.1: return 'SETTLING_TIMEOUT'
    if float(r['mae']) > 0.1: return 'LEARNER_LOW_CONFIDENCE' if int(r['confidence']) < 60 else 'STEADY_MAE'
    if float(r['p95']) > 0.15: return 'P95_FAILURE'
    if float(r['ripple']) > 0.25: return 'RIPPLE_FAILURE'
    if int(float(r['settling'])) < 0: return 'SETTLING_TIMEOUT'
    return 'UNKNOWN_ROOT_CAUSE'

def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--report-dir', type=Path, default=Path(tempfile.gettempdir()) / 'mayap-ab-smart')
    ap.add_argument('--jobs', type=int, default=min(4, os.cpu_count() or 1))
    ap.add_argument('--skip-reproduce', action='store_true', help='do not re-run A/B (use the frozen baseline CSVs)')
    args = ap.parse_args()
    out = args.report_dir; out.mkdir(parents=True, exist_ok=True)
    inc = out / 'include'
    run([sys.executable, str(ROOT / 'tools/test_thermal_control.py'), '--emit-includes', str(inc)], stdout=subprocess.DEVNULL)
    binary = out / 'thermal-adaptive-v1'
    run(['g++', '-std=c++11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(inc), str(ROOT / 'tests/thermal-adaptive-v1.cpp'), '-o', str(binary)])
    md, problems = ['# Smart Thermal A/B/C (simulation only)', ''], []

    # --- reproducibility of A and B -----------------------------------------------------------------
    frozen = {'legacy788': 'adaptive-788.csv', 'matrix': 'adaptive-2160.csv'}
    if not args.skip_reproduce:
        for suite, name in frozen.items():
            got = sharded(binary, suite, args.jobs, out)
            same = sha(got) == sha(BASE / name)
            if not same:
                # shard order differs from the frozen merge only by concatenation order; compare as sets of rows
                same = sorted(got.read_text().splitlines()) == sorted((BASE / name).read_text().splitlines())
            md.append(f'* reproducibility {suite}: A and B rows identical to frozen baseline `{name}`: **{same}**')
            if not same: problems.append(f'{suite}: A/B no longer reproduce the frozen baseline')
        md.append('')

    # --- matrices ------------------------------------------------------------------------------------
    for suite, name, title in (('legacy788', 'adaptive-788.csv', '788'), ('matrix', 'adaptive-2160.csv', '2160')):
        smart = rows(sharded(binary, suite + '_smart', args.jobs, out))
        base = rows(BASE / name)
        A = {r['label']: r for r in base if r['mode'] == 'BASELINE_V4'}
        B = {r['label']: r for r in base if r['mode'] == 'ADAPTIVE_V1'}
        C = {r['label']: r for r in smart}
        assert set(B) == set(C) == set(A), 'case ids differ between modes'
        P = lambda d: sum(r['target'] == 'PASS' for r in d.values())
        hi = lambda d: sum(int(r['high']) > 0 for r in d.values()); em = lambda d: sum(int(r['emergency']) > 0 for r in d.values())
        rc = lambda d: sum(r['target'] == 'PASS' and r['class'] == 'REACHABLE' for r in d.values())
        nreach = sum(r['class'] == 'REACHABLE' for r in B.values())
        reg = sorted(k for k in C if B[k]['target'] == 'PASS' and C[k]['target'] == 'FAIL')
        fix = sorted(k for k in C if B[k]['target'] == 'FAIL' and C[k]['target'] == 'PASS')
        md += [f'## {title}-case matrix', '', '| | PASS | reachable PASS | High | Emergency |', '|---|---|---|---|---|',
               f'| A legacy PID | {P(A)}/{len(A)} | {rc(A)}/{nreach} | {hi(A)} | {em(A)} |',
               f'| B Adaptive V1 | {P(B)}/{len(B)} | {rc(B)}/{nreach} | {hi(B)} | {em(B)} |',
               f'| C Smart Thermal | {P(C)}/{len(C)} | {rc(C)}/{nreach} | {hi(C)} | {em(C)} |', '',
               f'B PASS -> C FAIL: {len(reg)}   B FAIL -> C PASS: {len(fix)}   A PASS -> C FAIL: '
               f"{sum(1 for k in C if A[k]['target'] == 'PASS' and C[k]['target'] == 'FAIL')}", '']
        cls = collections.defaultdict(lambda: [0, 0, 0, 0])
        for k, r in C.items():
            c = cls[r['class']]; c[0] += 1; c[1] += A[k]['target'] == 'PASS'; c[2] += B[k]['target'] == 'PASS'; c[3] += r['target'] == 'PASS'
        md += ['| class | cases | A | B | C |', '|---|---|---|---|---|'] + [f'| {k} | {v[0]} | {v[1]} | {v[2]} | {v[3]} |' for k, v in sorted(cls.items())]
        why = collections.Counter(reason(r) for r in C.values() if r['target'] == 'FAIL')
        md += ['', 'C fail reasons (heuristic): ' + ', '.join(f'{k}={v}' for k, v in sorted(why.items())), '']
        with (out / f'ab-{title}.csv').open('w', newline='') as f:
            cols = ['label', 'class', 'sp', 'eff', 'capacity', 'loss', 'dead', 'lag', 'ambient', 'resolution']
            w = csv.writer(f, lineterminator='\n')
            w.writerow(cols + [f'{m}_{c}' for m in 'ABC' for c in ('target', 'overshoot', 'mae', 'p95', 'ripple', 'settling', 'high', 'emergency')] + ['C_reason'])
            for k in C:
                w.writerow([B[k][c] for c in cols] + [d[k][c] for d in (A, B, C) for c in ('target', 'overshoot', 'mae', 'p95', 'ripple', 'settling', 'high', 'emergency')] + [reason(C[k])])
        if hi(C) > hi(B) or em(C) > em(B): problems.append(f'{title}: Smart has MORE High/Emergency than Adaptive V1')
        if title == '2160' and (hi(C) > 33 or em(C) > 17): problems.append('2160: Smart exceeds the frozen High/Emergency counts')

    # --- vent / hwchange / faults ---------------------------------------------------------------------
    for suite, name in (('vent', 'adaptive-vent63x3.csv'), ('hwchange', 'adaptive-hwchange.csv'), ('faults', 'adaptive-faults30.csv')):
        smart = rows(sharded(binary, suite + '_smart', args.jobs, out))
        base = rows(BASE / name)
        agg = collections.defaultdict(list)
        for r in base + smart: agg[r['mode']].append(r)
        md += [f'## {suite}', '', '| mode | rows | High | Emergency |' + (' vent dev max mean | integral wind-up mean | tail MAE mean |' if suite == 'vent' else ' tail MAE mean |'),
               '|---|---|---|---|' + ('---|---|---|' if suite == 'vent' else '---|')]
        for m in ('BASELINE_V4', 'V1_NO_VENT_COORD', 'ADAPTIVE_V1', 'SMART_THERMAL'):
            rs = agg.get(m)
            if not rs: continue
            mean = lambda k: sum(float(r[k]) for r in rs) / len(rs)
            hh = sum(int(r['high']) > 0 for r in rs); ee = sum(int(r['emergency']) > 0 for r in rs)
            extra = f" {mean('vent_dev_max'):.3f} | {mean('integral_windup_max'):.1f} | {mean('mae_tail'):.3f} |" if suite == 'vent' else f" {mean('mae_tail'):.3f} |"
            md.append(f'| {m} | {len(rs)} | {hh} | {ee} |' + extra)
            if m == 'SMART_THERMAL' and (hh or ee): problems.append(f'{suite}: Smart High/Emergency')
        md.append('')
    (out / 'summary.md').write_text('\n'.join(md) + '\n')
    print('\n'.join(md))
    if problems:
        print('\nA/B PROBLEMS:\n  ' + '\n  '.join(problems)); sys.exit(1)
    print('\nA/B/C: no safety regression (simulation only)')

if __name__ == '__main__':
    main()
