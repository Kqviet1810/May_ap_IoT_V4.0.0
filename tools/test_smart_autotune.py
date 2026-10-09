#!/usr/bin/env python3
"""Smart AutoTune V1 simulation gate (SOFTWARE / SIMULATION ONLY - physical commissioning is still required).

Builds against the production-derived harness includes (same extraction as tools/test_thermal_control.py):
  1. tests/thermal-smart-autotune-unit.cpp   estimator, SIMC bounds, baseline gates, bumpless hand-over, accept + atomic save + profile
                                              seed, immediate aborts at every phase, power loss, rejection rollback
  2. tests/thermal-smart-autotune.cpp        closed-loop matrix, SMART engine:  36-case mini matrix, then the 540-case full matrix
  3. the same file with -DLEGACY_ENGINE      the original relay-only AutoTune on the SAME plants (cold start; warm start for context)

Gates (a regression ceiling, not a claim of physical accuracy):
  * High = 0 and Emergency = 0 during every tune and in every post-tune run;
  * ACCEPTED_BAD = 0 on the 36-case mini matrix AND on the 540-case full matrix (the independent 3 h post-tune run is the oracle; the firmware never sees it);
  * the full matrix accepts at least the recorded minimum (a "never accept anything" firmware must not pass);
  * every rejection restores the old PID and profile (checked inside the harness; a violation aborts the run).
"""
import argparse, csv, collections, json, os, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)

def sharded(binary, mode, jobs, out_dir, env=None):
    files = [out_dir / f'{mode}.{i}.csv' for i in range(jobs)]
    sums = [None] * jobs
    def one(i):
        r = subprocess.run([str(binary), mode, str(files[i]), str(i), str(jobs)], check=True, capture_output=True, text=True,
                           env={**os.environ, **(env or {})})
        sums[i] = r.stdout.strip().splitlines()[-1]
    with ThreadPoolExecutor(jobs) as pool:
        list(pool.map(one, range(jobs)))
    rows = []
    for f in files:
        rows += list(csv.DictReader(f.open()))
    total = collections.Counter()
    for s in sums:
        for k, v in (kv.split('=') for kv in s.split()[1:] if '=' in kv):
            if v.isdigit(): total[k] += int(v)
    merged = out_dir / f'{mode}.csv'
    with merged.open('w', newline='') as f:
        if rows:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()), lineterminator='\n'); w.writeheader(); w.writerows(rows)
    return rows, total

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--report-dir', type=Path, default=Path(tempfile.gettempdir()) / 'mayap-smart-autotune')
    ap.add_argument('--jobs', type=int, default=min(4, os.cpu_count() or 1))
    ap.add_argument('--sanitize', action='store_true')
    args = ap.parse_args()
    out = args.report_dir; out.mkdir(parents=True, exist_ok=True)
    inc = out / 'include'
    run([sys.executable, str(ROOT / 'tools/test_thermal_control.py'), '--emit-includes', str(inc)], stdout=subprocess.DEVNULL)
    base = json.loads((ROOT / 'tests/thermal-smart-autotune-baseline.json').read_text())
    common = ['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(inc)]
    if args.sanitize: common += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
    opt = ['-O2']

    unit = out / 'smart-unit'
    run(common + opt + [str(ROOT / 'tests/thermal-smart-autotune-unit.cpp'), '-o', str(unit)])
    r = subprocess.run([str(unit)], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr); r.check_returncode()
    print(r.stdout.strip().splitlines()[-1])
    for line in r.stdout.strip().splitlines()[:-1]: print(line)

    smart = out / 'smart-matrix'
    legacy = out / 'legacy-matrix'
    run(common + opt + [str(ROOT / 'tests/thermal-smart-autotune.cpp'), '-o', str(smart)])
    run(common + opt + ['-DLEGACY_ENGINE', str(ROOT / 'tests/thermal-smart-autotune.cpp'), '-o', str(legacy)])
    problems, md = [], ['# Smart AutoTune V1: simulation summary (SOFTWARE / SIMULATION ONLY)\n']

    def funnel(title, rows, t):
        by_class = collections.Counter(r['class'] for r in rows)
        reasons = collections.Counter(r['reason'] for r in rows if r['class'] != 'ACCEPTED')
        md.append(f'\n## {title}\n')
        md.append(f"cases={t['cases']} started={t['started']} model_valid={t['model_valid']} candidate={t['candidate']} validation_started={t['validation_started']} "
                  f"accepted={t['accepted']} rejected={t['rejected']} timeout={t['timeout']} safety_abort={t['safety_abort']} sensor_abort={t['sensor_abort']} "
                  f"**ACCEPTED_BAD={t['ACCEPTED_BAD']}** High/Emergency during tune={t['high_during_tune']}/{t['emergency_during_tune']} "
                  f"post-tune High/Emergency={t['post_high']}/{t['post_emergency']}\n")
        md.append('rejection reasons: ' + ', '.join(f'{k}={v}' for k, v in sorted(reasons.items())) + '\n')
        acc = [r for r in rows if r['class'] == 'ACCEPTED']
        if acc:
            f = lambda k: [float(r[k]) for r in acc]
            md.append('accepted: gain err %% mean|worst = %.1f|%.1f, delay err s mean|worst = %.1f|%.1f, hold err pp worst = %.1f, post-tune MAE worst %.3f, P95 worst %.3f, ripple worst %.3f, overshoot worst %.3f\n' % (
                sum(abs(x) for x in f('gain_err_pct')) / len(acc), max(abs(x) for x in f('gain_err_pct')),
                sum(abs(x) for x in f('delay_err_s')) / len(acc), max(abs(x) for x in f('delay_err_s')),
                max(abs(float(r['hold']) - float(r['hold_true'])) for r in acc), max(f('post_mae')), max(f('post_p95')), max(f('post_ripple')), max(f('post_over'))))
        return by_class

    # ---- mini (qualification) matrix ----------------------------------------------------------------------------------
    rows, t = sharded(smart, 'mini', 1, out)
    funnel('Mini matrix (36 cases: every level of efficiency / mass / dead time / loss / resolution / ambient)', rows, t)
    if t['cases'] != 36: problems.append(f"mini: {t['cases']} cases")
    if t['ACCEPTED_BAD'] != 0: problems.append(f"mini: ACCEPTED_BAD={t['ACCEPTED_BAD']} (must be 0)")
    if t['accepted'] < base['miniAcceptedMin']: problems.append(f"mini: accepted {t['accepted']} < {base['miniAcceptedMin']}")
    for k in ('high_during_tune', 'emergency_during_tune', 'post_high', 'post_emergency'):
        if t[k]: problems.append(f'mini: {k}={t[k]}')

    # ---- full matrix -------------------------------------------------------------------------------------------------------
    frows, ft = sharded(smart, 'full', args.jobs, out)
    funnel('Full matrix (540 cases: 6 efficiencies x 3 masses x 3 losses x 5 dead times x 2 ambients, lag 3/8/30, resolution 0.01/0.1)', frows, ft)
    if ft['cases'] != 540: problems.append(f"full: {ft['cases']} cases")
    for k in ('high_during_tune', 'emergency_during_tune', 'post_high', 'post_emergency'):
        if ft[k]: problems.append(f'full: {k}={ft[k]}')
    if ft['ACCEPTED_BAD'] != 0 or ft['ACCEPTED_BAD'] > base['fullAcceptedBadMax']: problems.append(f"full: ACCEPTED_BAD={ft['ACCEPTED_BAD']} (must be 0)")
    if ft['accepted'] < base['fullAcceptedMin']: problems.append(f"full: accepted {ft['accepted']} < {base['fullAcceptedMin']}")
    bad = [r for r in frows if r['verdict'] == 'ACCEPTED_BAD']
    md.append('ACCEPTED_BAD cases: ' + (', '.join(f"{r['label']}(eff {r['eff']}, mass {r['capacity']}, loss {r['loss']}, dead {r['dead']}, res {r['resolution']})" for r in bad) or 'none') + '\n')

    # ---- legacy comparison on the SAME plants -----------------------------------------------------------------------
    md.append('\n## Legacy relay-only AutoTune vs Smart AutoTune (same 36 mini plants)\n')
    md.append('| engine / start | accepted | rejected | timeout | safety abort | ACCEPTED_BAD | High during tune | Emergency during tune |')
    md.append('|---|---|---|---|---|---|---|---|')
    lrows, lt = sharded(legacy, 'mini', 1, out)
    md.append(f"| Smart, cold start | {t['accepted']} | {t['rejected']} | {t['timeout']} | {t['safety_abort']} | {t['ACCEPTED_BAD']} | {t['high_during_tune']} | {t['emergency_during_tune']} |")
    md.append(f"| Legacy, cold start | {lt['accepted']} | {lt['rejected']} | {lt['timeout']} | {lt['safety_abort']} | {lt['ACCEPTED_BAD']} | {lt['high_during_tune']} | {lt['emergency_during_tune']} |")
    wrows, wt = sharded(legacy, 'mini', 1, out, env={'START_TEMP': '37.0'})
    md.append(f"| Legacy, warm start 37.0 C | {wt['accepted']} | {wt['rejected']} | {wt['timeout']} | {wt['safety_abort']} | {wt['ACCEPTED_BAD']} | {wt['high_during_tune']} | {wt['emergency_during_tune']} |")
    md.append('\n(Smart refuses a warm chamber at start: it needs >= 2.5 C of headroom for the first excitation.)')
    if lt['high_during_tune'] < t['high_during_tune']: problems.append('legacy comparison: smart crossed High more often than legacy')
    (out / 'summary.md').write_text('\n'.join(md) + '\n')
    print('\n'.join(md))
    if problems:
        print('SMART AUTOTUNE GATE FAILED:\n  ' + '\n  '.join(problems)); raise SystemExit(1)
    print('Smart AutoTune V1 gates PASS (simulation only): mini ACCEPTED_BAD=0, High/Emergency=0, full ACCEPTED_BAD=%d (must be 0)' % ft['ACCEPTED_BAD'])

if __name__ == '__main__':
    main()
