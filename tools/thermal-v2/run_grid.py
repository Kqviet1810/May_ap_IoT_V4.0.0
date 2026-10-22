#!/usr/bin/env python3
"""Thermal V2 digital-twin grid runner + grader (G1).  SIMULATION ONLY: a pass here is NOT proof for the real oven (lesson L6).

  run_grid.py selftest                         build the rig and run its self-tests (analytic closed forms, burst accuracy, protection, E115)
  run_grid.py unit                             host unit tests of thermal_v2.h alone
  run_grid.py plants A|B|C                      print the plant CSV generated from plants.json
  run_grid.py protect [--ctl v2,fastest] [--sets A,D]   G4: injected faults (S10a..h), pass = no heating command while blocked + no hazard for the faults the layers can see
  run_grid.py grid --ctl NAME [--sets A,B,C] [--scn S1,S2,...] [--out DIR]
                                                run the sets, grade against tools/thermal-research/acceptance.json

Held-out discipline (ROADMAP section 5 / CONSTANTS section 6), enforced here and not by promise:
  * sets B and C refuse to run when tracked files are modified (dirty tree) or the controller source is not committed;
  * every B/C run prints the commit hash and the sha256 of thermal_v2.h and is appended to tools/thermal-v2/bc_runs.log;
    the log is the count of how many times B/C were looked at; after a structural change the B seed in plants.json must be changed.
"""
import argparse, csv, hashlib, io, json, math, os, subprocess, sys, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
TV2 = ROOT / 'tools' / 'thermal-v2'
ACC = ROOT / 'tools' / 'thermal-research' / 'acceptance.json'
CTL_SRC = ROOT / 'MAYAP_INDUSTRIAL_v1_0_0' / 'thermal_v2.h'
BC_LOG = TV2 / 'bc_runs.log'
COLS = ['id', 'Pmax', 'Cf', 'Ce', 'hA', 'UA', 'td', 'tl', 'Tamb', 'thirdC', 'thirdG', 'sensorLag', 'gain', 'fanCoupling', 'sagPerDuty', 'sagStepAt', 'sagStep', 'offset']

def sh(cmd, **kw): return subprocess.run(cmd, check=True, **kw)

def plant_row(p, **over):
    d = dict(Ce=1900000, Tamb=28, thirdC=0, thirdG=0, sensorLag=0, gain=1, fanCoupling=0, sagPerDuty=0, sagStepAt=-1, sagStep=0, offset=0)
    d.update(p); d.update(over)
    return [d['id'], d['Pmax'], d['Cf'], d['Ce'], d['hA'], d['UA'], d['td'], d['tl'], d['Tamb'], d['thirdC'], d['thirdG'], d['sensorLag'], d['gain'],
            d['fanCoupling'], d['sagPerDuty'], d['sagStepAt'], d['sagStep'], d['offset']]

def xorshift32(seed):
    x = seed & 0xFFFFFFFF
    while True:
        x ^= (x << 13) & 0xFFFFFFFF; x ^= x >> 17; x ^= (x << 5) & 0xFFFFFFFF
        yield x / 4294967296.0

def gen_plants(which, spec):
    A = {p['id']: p for p in spec['A']}
    common = spec['common']
    rows = []
    if which == 'A':
        for p in spec['A']: rows.append(plant_row(p, Ce=common['Ce'], Tamb=common['Tamb']))
    elif which in ('B', 'D'):
        b = spec[which]; r = xorshift32(b['seed']); rg = b['ranges']; n = 0
        while len(rows) < b['count']:
            vals = {k: rg[k][0] + next(r) * (rg[k][1] - rg[k][0]) for k in ['Pmax', 'Cf', 'Ce', 'hA', 'UA', 'td', 'tl']}
            if vals['UA'] * 9.5 / vals['Pmax'] > 0.9: continue          # rejection rule in plants.json
            n += 1
            rows.append(plant_row(dict(id=which + '%02d' % n, **{k: round(v, 3) for k, v in vals.items()}), Tamb=common['Tamb']))
    elif which == 'C':
        mods = {m['id']: m for m in spec['C']['modifiers']}
        over = {'C1': dict(thirdC=1.5e6, thirdG=400), 'C2': dict(sensorLag=20), 'C3': dict(sensorLag=60), 'C4': dict(gain=0.7), 'C5': dict(gain=1.3),
                'C6': dict(fanCoupling=1), 'C7': dict(sagPerDuty=0.12, sagStepAt=3 * 3600, sagStep=0.05), 'C8': dict(offset=-0.5)}
        for base in spec['C']['baseOvens']:
            for m in mods:
                rows.append(plant_row(dict(A[base], id=base + m), Ce=common['Ce'], Tamb=common['Tamb'], **over[m]))
    return rows

def plants_csv(which, spec):
    buf = io.StringIO(); w = csv.writer(buf, lineterminator='\n'); w.writerow(COLS)
    for r in gen_plants(which, spec): w.writerow(r)
    return buf.getvalue()

def build(work):
    inc = work / 'inc'
    sh([sys.executable, str(TV2 / 'make_includes.py'), str(inc)], stdout=subprocess.DEVNULL)
    exe = work / 'v2grid'
    sh(['g++', '-std=c++14', '-O2', '-Wall', '-Wno-unused-variable', '-I', str(inc), str(TV2 / 'v2grid.cpp'), '-o', str(exe)])
    return exe

def git(*a): return subprocess.run(['git', '-C', str(ROOT)] + list(a), capture_output=True, text=True, check=True).stdout.strip()

def held_out_guard(sets, ctl, spec):
    if not (set(sets) & {'B', 'C'}): return
    dirty = [l for l in git('status', '--porcelain', '--untracked-files=all', '--', 'tools/thermal-v2', 'tools/thermal-research', 'MAYAP_INDUSTRIAL_v1_0_0').splitlines() if l.strip() and 'bc_runs.log' not in l]
    if CTL_SRC.exists() and not git('ls-files', str(CTL_SRC)): sys.exit('REFUSED: thermal_v2.h is not tracked by git')
    if dirty: sys.exit('REFUSED: sets B/C only run on a clean tree (commit first). Dirty: ' + '; '.join(dirty[:6]))
    sha = hashlib.sha256(CTL_SRC.read_bytes()).hexdigest() if CTL_SRC.exists() else 'none'
    head = git('rev-parse', 'HEAD')
    prev = BC_LOG.read_text().count('\n') if BC_LOG.exists() else 0
    print(f'HELD-OUT RUN #{prev + 1}: commit {head[:12]} thermal_v2.h sha256 {sha[:16]} seedB {spec["B"]["seed"]} controller {ctl}')
    with BC_LOG.open('a') as f: f.write(f'{head}\t{sha}\t{spec["B"]["seed"]}\t{ctl}\t{",".join(sorted(sets))}\n')

def run(exe, csvtext, mode, ctl, scn, work, tag):
    p = work / f'{tag}.csv'; p.write_text(csvtext)
    cmd = [str(exe), 'ref', str(p), scn] if mode == 'ref' else [str(exe), 'run', str(p), ctl, scn]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    return list(csv.DictReader(io.StringIO(out)))

def grade(rows, refs, acc):
    """returns {criterion: (passed, total, [failing case ids])}"""
    res = {}
    f = lambda r, k: float(r[k])
    sfx = ['']                      # C8 is a sensor calibration offset the controller cannot see: its rows are graded under '<id>.C8cal'
    def put(cid, ok, case):
        cid += sfx[0]
        p, t, bad = res.get(cid, (0, 0, []))
        res[cid] = (p + (1 if ok else 0), t + 1, bad + ([case] if not ok else []))
    for r in rows:
        sfx[0] = '.C8cal' if r['plant'].endswith('C8') else ''
        sid = r['scenario']; case = r['plant'] + '/' + sid; base = sid[:2] if sid[:2] in ('S5', 'S6') else sid
        if sid in ('S3p', 'S3w', 'S3q', 'S3r'): sfx[0] += '.' + {'S3p': 'persist', 'S3w': 'wrongprior', 'S3q': 'gainx1.43', 'S3r': 'gainx0.77'}[sid]; base = 'S3'      # restart with a stored path: graded as S3, reported apart
        ref = refs.get((r['plant'], 'S3' if sid in ('S3p', 'S3w', 'S3q', 'S3r') else sid))
        for c in acc['criteria']:
            if 'scn' not in c or (sid not in c['scn'] and base not in c['scn']): continue
            m = c['metric']; cid = c['id']
            if m == 'reach_s' and 'max_vs_fastest' in c:
                if ref is None or ref <= 0: put(cid + '.NOREF', False, case); continue
                lim = c['max_vs_fastest']['factor'] * ref + c['max_vs_fastest']['add_s']
                if sid in ('S1', 'S2'): lim += c['max_vs_fastest'].get('first_batch_extra_s', 0)
                put(cid, f(r, 'reach_s') >= 0 and f(r, 'reach_s') <= lim, case)
            elif m == 'recov_s' and 'max_vs_physical' in c:
                if ref is None or ref <= 0: put(cid + '.NOREF', False, case); continue
                lim = c['max_vs_physical']['factor'] * ref + c['max_vs_physical']['add_s']
                put(cid, f(r, 'recov_s') >= 0 and f(r, 'recov_s') <= lim, case)
            elif m == 'over_c':
                put(cid, f(r, 'over_c') <= c['max'], case)
                if 'settle_0p1_s_max' in c: put(cid + '.settle', 0 <= f(r, 'settle_0p1_s') <= c['settle_0p1_s_max'], case)
            elif m == 'under_c': put(cid, f(r, 'under_c') >= c['min'], case)
            elif m == 'high_samples': put(cid, int(r['high_samples']) <= c['max'], case)
            elif m == 'settle_0p3_s': put(cid, 0 <= f(r, 'settle_0p3_s') <= c['max'], case)
            elif m == 'settle_0p1_s': put(cid, 0 <= f(r, 'settle_0p1_s') <= c['max'], case)
            elif m == 'tail_in_0p1_pct_avg60': put(cid, f(r, 'tail01_avg60') >= c['min'], case)
            elif m == 'tail_in_0p3_pct': put(cid, f(r, 'tail03') >= c['min'], case)
            elif m == 'unsafe_command_ticks': put(cid, int(r['unsafe_ticks']) <= c['max'], case)
        put('GATE.emergency', int(r['emergency_samples']) == 0, case)
        put('GATE.high', int(r['high_samples']) == 0, case)
        put('GATE.e115', int(r['e115']) == 0, case)
        put('GATE.unsafe', int(r['unsafe_ticks']) == 0, case)
    return res

FAULTS = {'S10a': ('sensor frozen at the setpoint (valid frames, value never changes)', False), 'S10b': ('sensor drifts low by 3 C in 30 min', False),
          'S10c': ('sensor jumps low by 4 C', False), 'S10d': ('sensor jumps high by 4 C', True), 'S10e': ('sensor / path lost for 2 h', True),
          'S10f': ('SSR welded on (only the master contactor can stop it)', False), 'S10g': ('SSR never conducts', True),
          'S10h': ('sensor frozen LOW while climbing from 28 C', False)}

def protect(exe, spec, work, ctls, sets):
    """G4.  Formal criteria: P1 no heating command while the production layers block (all faults, all plants);
    P2 for faults the single-sensor layers CAN see (jump high, path lost, SSR off): no High, no Emergency.  The rest is reported as measured containment, not pass/fail."""
    ok = True; scn = ','.join(FAULTS)
    for ctl in ctls:
        rows = []
        for s in sets: rows += run(exe, plants_csv(s, spec), 'run', ctl, scn, work, 'prot' + s)
        print(f'controller={ctl} sets={",".join(sets)} cases={len(rows)}  (SIMULATION ONLY)')
        unsafe = sum(int(r['unsafe_ticks']) for r in rows)
        print(f'  P1 unsafe heating commands        {unsafe:>6}   {"PASS" if unsafe == 0 else "FAIL"}'); ok &= unsafe == 0
        for sid, (what, visible) in FAULTS.items():
            rs = [r for r in rows if r['scenario'] == sid]
            hi = sum(int(r['high_samples']) > 0 for r in rs); em = sum(int(r['emergency_samples']) > 0 for r in rs); pk = max(float(r['peak_air']) for r in rs)
            e115 = sum(int(r['e115']) for r in rs)
            tag = ''
            if visible and ctl == 'v2':
                good = hi == 0 and em == 0; ok &= good; tag = 'P2 ' + ('PASS' if good else 'FAIL')
            elif visible: tag = 'info (a 100 % requester overheats a settled oven before the fault)'
            else: tag = 'measured (not detectable by the single-sensor layers)' if sid not in ('S10h',) else 'measured'
            print(f'  {sid} {what:<62} High {hi:>3}/{len(rs)} Emergency {em:>3}/{len(rs)} peak {pk:6.1f} C E115 {e115:>3}  {tag}')
    return ok

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('cmd', choices=['selftest', 'unit', 'plants', 'grid', 'protect']); ap.add_argument('arg', nargs='?')
    ap.add_argument('--ctl', default='fastest'); ap.add_argument('--sets', default='A'); ap.add_argument('--scn', default='S1,S2,S3,S4,S5a,S5b,S6a,S6b,S7,S8,S9')
    ap.add_argument('--out', default=None); a = ap.parse_args()
    spec = json.loads((TV2 / 'plants.json').read_text()); acc = json.loads(ACC.read_text())
    if a.cmd == 'plants': sys.stdout.write(plants_csv(a.arg or 'A', spec)); return
    if a.cmd == 'unit':
        with tempfile.TemporaryDirectory(prefix='v2unit-') as td:
            exe = Path(td) / 'unit_v2'
            sh(['g++', '-std=c++14', '-O2', '-Wall', str(TV2 / 'unit_v2.cpp'), '-o', str(exe)])
            sys.exit(subprocess.run([str(exe)]).returncode)
    with tempfile.TemporaryDirectory(prefix='v2grid-') as td:
        work = Path(td); exe = build(work)
        if a.cmd == 'selftest':
            sys.exit(subprocess.run([str(exe), 'selftest']).returncode)
        if a.cmd == 'protect':
            ctls = a.ctl.split(',') if a.ctl != 'fastest' else ['v2', 'fastest']
            sys.exit(0 if protect(exe, spec, work, ctls, a.sets.split(',')) else 1)
        sets = a.sets.split(','); held_out_guard(sets, a.ctl, spec)
        outdir = Path(a.out) if a.out else None
        if outdir: outdir.mkdir(parents=True, exist_ok=True)
        allrows = []; refs = {}
        for s in sets:
            text = plants_csv(s, spec)
            refscn = ','.join(x for x in a.scn.split(',') if x in ('S1', 'S2', 'S3', 'S4'))
            for r in run(exe, text, 'ref', 'ref', refscn, work, 'ref' + s):
                refs[(r['plant'], r['scenario'])] = float(r['reach_s'])
            rows = run(exe, text, 'run', a.ctl, a.scn, work, 'run' + s)
            allrows += rows
            if outdir:
                with (outdir / f'{s}_{a.ctl}.csv').open('w') as f: f.write(','.join(rows[0].keys()) + '\n'); [f.write(','.join(r.values()) + '\n') for r in rows]
        res = grade(allrows, refs, acc)
        print(f'controller={a.ctl} sets={a.sets} cases={len(allrows)}  (SIMULATION ONLY)')
        worst = 0
        for cid, (p, t, bad) in sorted(res.items()):
            print(f'  {cid:<22} {p:>4}/{t:<4} {"PASS" if p == t else "FAIL"}' + ('' if p == t else '   e.g. ' + ', '.join(bad[:4])))
            worst += (t - p)
        want = {c['id'] for c in acc['criteria'] if c['id'] != 'GATE'} if a.scn.count(',') >= 10 else set()
        missing = sorted(w for w in want if w not in res)
        for w in missing: print(f'  {w:<22} NOT EVALUATED')
        print('ALL CRITERIA PASS' if worst == 0 and not missing else f'{worst} case-criteria failed, {len(missing)} not evaluated')
        sys.exit(1 if worst else (3 if missing else 0))

if __name__ == '__main__': main()
