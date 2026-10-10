#!/usr/bin/env python3
"""Smart AutoTune rejection study (Smart Thermal phase 5). SIMULATION ONLY, read-only: no firmware or oracle is changed.

For every plant that Smart AutoTune REJECTED in the frozen 540-case matrix, run the same plant (SP 37.5, 3 h cold start, same sensor
resolution/lag/ambient) through the production control path with the DEFAULT gains and the learner only, i.e. what the oven does after
a safe rejection, for Adaptive V1 and for Smart Thermal. The question per rejection reason:
  * is the plant physically able to hold the target (class REACHABLE / SAFETY_LIMITED / HEAT_LIMITED ...)?
  * after the rejection, is the controller still safe (High/Emergency of the plant) and good (the unchanged oracle)?
A rejection is only a FALSE rejection candidate when the plant is REACHABLE, passive control FAILS the oracle, and the tune was
refused. Everything else is a correct refusal or a refusal that cost nothing.
"""
import argparse, collections, csv, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--binary', type=Path, required=True, help='thermal-adaptive-v1 binary built by tools/ab_smart_thermal.py')
    ap.add_argument('--matrix', type=Path, default=ROOT / 'audit/smart-thermal/baseline/smart-autotune-full540.csv')
    ap.add_argument('--out', type=Path, default=Path(tempfile.gettempdir()) / 'autotune-reject-study.csv')
    ap.add_argument('--jobs', type=int, default=4)
    args = ap.parse_args()
    rows = list(csv.DictReader(args.matrix.open()))
    rej = [r for r in rows if r['verdict'] == 'REJECTED']
    def one(r, mode):
        cmd = [str(args.binary), 'one', '--eff', r['eff'], '--cap', r['capacity'], '--loss', r['loss'], '--dead', r['dead'], '--lag', r['lag'],
               '--amb', r['ambient'], '--res', r['resolution'], '--sp', '37.5', '--mode', str(mode), '--hours', '3', '--csv', '1']
        out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.splitlines()
        row = next(csv.DictReader(out))
        return row
    jobs = [(r, m) for r in rej for m in (2, 3)]
    with ThreadPoolExecutor(args.jobs) as pool:
        res = list(pool.map(lambda j: one(*j), jobs))
    by = collections.defaultdict(dict)
    for (r, m), x in zip(jobs, res): by[r['label']][m] = x
    out_rows = []
    for r in rej:
        a, s = by[r['label']][2], by[r['label']][3]
        out_rows.append({'label': r['label'], 'reason': r['reason'], 'val_why': r['val_why'], 'plant_class': a['class'], 'eff': r['eff'], 'capacity': r['capacity'],
                         'dead': r['dead'], 'lag': r['lag'], 'resolution': r['resolution'],
                         'adaptive_pass': a['target'], 'adaptive_high': a['high'], 'adaptive_emerg': a['emergency'], 'adaptive_mae': a['mae'],
                         'smart_pass': s['target'], 'smart_high': s['high'], 'smart_emerg': s['emergency'], 'smart_mae': s['mae']})
    with args.out.open('w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=list(out_rows[0].keys()), lineterminator='\n'); w.writeheader(); w.writerows(out_rows)
    print(f'{len(rej)} rejected plants of {len(rows)}')
    print('| reason | n | plant class | passive Adaptive PASS | passive Smart PASS | Adaptive High/Emerg | Smart High/Emerg | false-rejection candidates (REACHABLE and Smart FAIL) |')
    print('|---|---|---|---|---|---|---|---|')
    for reason in sorted({o['reason'] for o in out_rows}):
        g = [o for o in out_rows if o['reason'] == reason]
        cls = collections.Counter(o['plant_class'] for o in g)
        cand = [o for o in g if o['plant_class'] == 'REACHABLE' and o['smart_pass'] == 'FAIL']
        print(f"| {reason} | {len(g)} | " + ', '.join(f'{k} {v}' for k, v in sorted(cls.items())) +
              f" | {sum(o['adaptive_pass'] == 'PASS' for o in g)} | {sum(o['smart_pass'] == 'PASS' for o in g)} | "
              f"{sum(int(o['adaptive_high']) > 0 for o in g)}/{sum(int(o['adaptive_emerg']) > 0 for o in g)} | "
              f"{sum(int(o['smart_high']) > 0 for o in g)}/{sum(int(o['smart_emerg']) > 0 for o in g)} | {len(cand)} |")
    cand = [o for o in out_rows if o['plant_class'] == 'REACHABLE' and o['smart_pass'] == 'FAIL']
    print(f'\nFalse-rejection candidates (REACHABLE, refused, passive Smart control FAILS the oracle): {len(cand)}')
    print('by reason:', dict(collections.Counter(o['reason'] + ('/' + o['val_why'] if o['val_why'] != '-' else '') for o in cand)))

if __name__ == '__main__':
    main()
