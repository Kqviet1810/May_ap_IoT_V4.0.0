#!/usr/bin/env python3
"""Acceptance check of the thermal controller on the two-node digital twin (simulation only).

It builds tools/thermal-research/twin_grid.cpp against the firmware code, runs it over a subset of the 72 twin ovens and
compares each oven with tools/thermal-research/acceptance.json. The criteria describe the symptoms seen on the oven, in
absolute numbers (not "no worse than the previous version").  Without --enforce it only reports (exit 0).
"""
import argparse, csv, json, os, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GRID = ROOT / 'tools' / 'thermal-research' / 'twin_grid.cpp'
CRIT = ROOT / 'tools' / 'thermal-research' / 'acceptance.json'


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--stride', type=int, default=4, help='every Nth oven of the 72 (1 = all)')
    ap.add_argument('--enforce', action='store_true', help='exit 1 when any criterion fails')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--csv', type=Path, help='write the raw rows here')
    ap.add_argument('--reuse', type=Path, help='evaluate this raw CSV instead of simulating')
    args = ap.parse_args()
    crit = json.loads(CRIT.read_text())['criteria']
    rows = []
    if args.reuse:
        rows = [r for r in csv.DictReader(args.reuse.open())]
    else:
        work = Path(tempfile.mkdtemp(prefix='mayap-accept-'))
        inc = work / 'inc'
        run([sys.executable, str(ROOT / 'tools' / 'test_thermal_control.py'), '--emit-includes', str(inc)], stdout=subprocess.DEVNULL)
        exe = work / 'twin_grid'
        run(['g++', '-std=c++17', '-O2', '-I', str(inc), '-I', str(ROOT / 'tests'), str(GRID), '-o', str(exe)])
        starts = sorted({c['start'] for c in crit})
        for start in starts:
            scn = ''.join(sorted({c['scn'][1] for c in crit if c['start'] == start}))
            procs = []
            for i in range(args.jobs):
                env = dict(os.environ, MODE='predictive', STRIDE=str(args.stride), SCN=scn, T1START=str(start))
                procs.append(subprocess.Popen([str(exe), str(i), str(args.jobs)], stdout=subprocess.PIPE, text=True, env=env))
            for p in procs:
                out, _ = p.communicate()
                if p.returncode: sys.exit('twin_grid failed')
                for r in csv.DictReader(l for l in out.splitlines() if ',' in l):
                    if r['scn'] != 'scn':
                        r['start'] = str(start); rows.append(r)
        if args.csv:
            with args.csv.open('w', newline='') as f:
                w = csv.DictWriter(f, fieldnames=list(rows[0].keys())); w.writeheader(); w.writerows(rows)
    failed = 0
    print('%-14s %5s  %-9s %s' % ('criterion', 'ovens', 'pass', 'worst (limit)'))
    for c in crit:
        sel = [r for r in rows if r['scn'] == c['scn'] and r.get('start', '28') == str(c['start'])]
        if not sel:
            print('%-14s  no rows' % c['id']); failed += 1; continue
        bad = 0; worst = None
        for r in sel:
            v = float(r[c['metric']])
            if 'max_vs_fastest' in c:
                lim = c['max_vs_fastest']['factor'] * float(r['fastest_s']) + c['max_vs_fastest']['add']
            else:
                lim = c.get('max', c.get('min'))
            ok = (v <= lim) if 'min' not in c else (v >= lim)
            if c['metric'] == 'reach_s' and v < 0: ok = False        # never reached
            if not ok: bad += 1
            if worst is None or ((v > worst[0]) if 'min' not in c else (v < worst[0])): worst = (v, lim)
        failed += bad > 0
        print('%-14s %5d  %3d/%-5d %g (%g)' % (c['id'], len(sel), len(sel) - bad, len(sel), worst[0], worst[1]))
    print('ACCEPTANCE: %s (%d of %d criteria failed) -- simulation on the digital twin, not a real oven' % ('PASS' if not failed else 'FAIL', failed, len(crit)))
    if failed and args.enforce: sys.exit(1)


if __name__ == '__main__':
    main()
