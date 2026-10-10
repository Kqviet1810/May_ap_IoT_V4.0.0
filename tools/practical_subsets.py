#!/usr/bin/env python3
"""PRACTICAL scoring and "normal" subsets for the Smart Thermal A/B/C data. SIMULATION ONLY. Reads CSVs only: it never runs the
controller and never touches the oracle (`sim::Result::pass`, i.e. the STANDARD column of every table).

The definitions of PRACTICAL / normal subset / extended normal belong to an earlier external report that is not in this workspace.
They are RECONSTRUCTED here from the reference counts that report is known to give for the frozen Adaptive V1 baseline
(594/788, 1464/2160, 45/47, 187/213, 86/96) by exhaustive search over natural threshold / factor-range grids, and accepted ONLY if
the search finds the counts exactly. `--verify` re-runs that check against the frozen baseline CSVs and fails if any count moves.
A match on several independent exact counts is strong evidence, not proof: every table below is labelled RECONSTRUCTED.

  PRACTICAL   overshoot <= 0.30, MAE <= 0.20, P95 <= 0.30, no High, no Emergency (no ripple limit, no settling requirement)
  NORMAL      set point 37.5, class REACHABLE, efficiency >= 1.0, capacity 600 kJ/K .. 1.6 MJ/K, dead time 0..30 s, ambient 20..28 C
              (loss, lag, resolution unrestricted)                          -> 47 cases of the 788 set, 213 of the 2160 set
  EXTENDED    holdout set only: efficiency 0.85..1.75, dead time 8..45 s, resolution 0.1 (everything else unrestricted) -> 96 of 360
"""
import argparse, csv
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'audit/smart-thermal/baseline'
FINAL = ROOT / 'audit/smart-thermal/final'
REFERENCE = {  # frozen Adaptive V1 counts of the external report (what the reconstruction must reproduce)
    'practical788': 594, 'practical2160': 1464, 'normal788': (47, 45), 'normal2160': (213, 187), 'extended360': (96, 86)}

def f(r, k): return float(r[k])

def practical(r, p=''):
    return (f(r, p + 'overshoot') <= 0.30 and f(r, p + 'mae') <= 0.20 and f(r, p + 'p95') <= 0.30 and
            int(r[p + 'high']) == 0 and int(r[p + 'emergency']) == 0)

def normal(r):
    return (f(r, 'sp') == 37.5 and r['class'] == 'REACHABLE' and f(r, 'eff') >= 1.0 and 600000 <= f(r, 'capacity') <= 1600000 and
            0 <= f(r, 'dead') <= 30 and 20 <= f(r, 'ambient') <= 28)

def extended(r):
    return 0.85 <= f(r, 'eff') <= 1.75 and 8 <= f(r, 'dead') <= 45 and abs(f(r, 'resolution') - 0.1) < 1e-9

def frozen(name):
    return [r for r in csv.DictReader((BASE / name).open()) if r['mode'] == 'ADAPTIVE_V1']

def verify():
    s, m, h = frozen('adaptive-788.csv'), frozen('adaptive-2160.csv'), frozen('adaptive-holdout360.csv')
    got = {'practical788': sum(practical(r) for r in s), 'practical2160': sum(practical(r) for r in m)}
    for key, rows, sel in (('normal788', s, normal), ('normal2160', m, normal), ('extended360', h, extended)):
        sub = [r for r in rows if sel(r)]
        got[key] = (len(sub), sum(r['target'] == 'PASS' for r in sub))
    bad = {k: (got[k], REFERENCE[k]) for k in REFERENCE if got[k] != REFERENCE[k]}
    print('reconstruction check (got vs reference):', {k: got[k] for k in REFERENCE})
    if bad: raise SystemExit(f'RECONSTRUCTION DOES NOT REPRODUCE THE REFERENCE COUNTS: {bad}')
    print('reconstruction reproduces all 7 reference counts exactly')

def table(rows, label, sel=None):
    rows = [r for r in rows if sel is None or sel(r)]
    out = [f'| {label} | cases | STANDARD A/B/C | PRACTICAL A/B/C | High A/B/C | Emergency A/B/C |', '|---|---|---|---|---|---|']
    def cnt(fn): return '/'.join(str(sum(fn(r, p) for r in rows)) for p in 'ABC')
    out.append(f"| | {len(rows)} | {cnt(lambda r, p: r[p + '_target'] == 'PASS')} | {cnt(lambda r, p: practical(r, p + '_'))} | "
               f"{cnt(lambda r, p: int(r[p + '_high']) > 0)} | {cnt(lambda r, p: int(r[p + '_emergency']) > 0)} |")
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--verify', action='store_true', help='only check that the reconstruction reproduces the reference counts')
    ap.add_argument('--final-dir', type=Path, default=FINAL)
    ap.add_argument('--prefix', default='ab-')
    a = ap.parse_args()
    verify()
    if a.verify: return
    ld = lambda n: list(csv.DictReader((a.final_dir / f'{a.prefix}{n}.csv').open()))
    s, m, h = ld('788'), ld('2160'), ld('holdout-360')
    md = ['# PRACTICAL and normal subsets (RECONSTRUCTED definitions, simulation only)', '',
          'A = legacy PID, B = Adaptive V1 (frozen), C = Smart Thermal (flag ON). STANDARD is the unchanged oracle.', '']
    for title, rows, sel in (('788 all', s, None), ('2160 all', m, None), ('holdout 360 all', h, None),
                             ('788 NORMAL', s, normal), ('2160 NORMAL', m, normal), ('holdout EXTENDED', h, extended)):
        md += table(rows, title, sel) + ['']
    print('\n'.join(md))

if __name__ == '__main__':
    main()
