#!/usr/bin/env python3
"""Smart AutoTune on a family of virtual ovens (SIMULATION ONLY - not a physical qualification).

The REAL firmware AutoTune (thermal_smart_autotune.h, the production heating route, scheduler and arbiter) runs on the PC against an oven
model that you describe with the numbers you can measure on the real oven. For every combination of the lists you pass it prints whether
the tune is accepted, how long it takes, how close the identified gain / delay are to the model's truth, the PID it would store and what an
independent 3 h run with those gains does (the "oracle" the firmware never sees).

  python3 tools/oven_sim/autotune_family.py --kh 0.0133,0.02 --dead 150 --lag 50 --hold 0.019,0.03 --res 0.1

Units (all measurable on the real oven, see doc/OVEN_SIMULATION.md):
  --kh    degC per second at 100 % heater duty (initial slope of a long full-power run, divided by 60 for degC/min)
  --dead  seconds from "heater ON" to the first reaction of the sensor      --lag  extra first-order lag of the heater/air path (seconds)
  --hold  duty fraction that keeps the oven at the setpoint (0.019 = 1.9 %)  --cap  thermal capacity of the chamber, J/K (only sets the cooling time constant)
  --amb / --sp / --res   room temperature, setpoint, probe resolution (all lists allowed)
The nominal heater bank is the 16 kW of the machine; the model's effectiveness is derived from --kh so the plant gain is exactly --kh.
"""
import argparse, csv, itertools, os, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BANK_W = 16000.0


def floats(text):
    return [float(x) for x in text.split(',') if x.strip()]


def build(build_dir, force):
    build_dir.mkdir(parents=True, exist_ok=True)
    inc = build_dir / 'include'
    binary = build_dir / 'thermal-smart-autotune'
    sources = list((ROOT / 'MAYAP_INDUSTRIAL_v1_0_0').glob('*.h')) + list((ROOT / 'tests').glob('thermal-*.h')) + [ROOT / 'tests/thermal-smart-autotune.cpp']
    if not force and binary.exists() and binary.stat().st_mtime >= max(p.stat().st_mtime for p in sources):
        return binary
    subprocess.run([sys.executable, str(ROOT / 'tools/test_thermal_control.py'), '--emit-includes', str(inc)], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['g++', '-std=c++11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(inc), str(ROOT / 'tests/thermal-smart-autotune.cpp'), '-o', str(binary)], check=True)
    return binary


def one(args):
    binary, kh, dead, lag, hold, cap, amb, sp, res = args
    eff = kh * cap / BANK_W                         # plant gain  Kh = BANK_W * eff / cap   [degC/s at 100 % duty]
    loss = hold * BANK_W * eff / max(0.5, sp - amb)  # W/K so that the equilibrium duty at SP is exactly `hold`
    handle, tmp = tempfile.mkstemp(suffix='.csv'); os.close(handle)
    try:
        spec = 'one:%.6f,%.0f,%.6f,%.1f,%.2f,%.3f,%.1f,%.2f' % (eff, cap, loss, dead, amb, res, lag, sp)
        env = dict(os.environ)
        proc = subprocess.run([str(binary), spec, tmp], capture_output=True, text=True, env=env)
        rows = list(csv.DictReader(open(tmp)))
        row = rows[0] if rows else None
    finally:
        try: os.unlink(tmp)
        except OSError: pass
    return (kh, dead, lag, hold, cap, amb, sp, res), row, proc.returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--kh', default='0.0133', help='degC/s at 100 %% duty, comma list')
    ap.add_argument('--dead', default='150', help='dead time s, comma list')
    ap.add_argument('--lag', default='50', help='extra lag s, comma list')
    ap.add_argument('--hold', default='0.019', help='duty fraction that holds SP, comma list')
    ap.add_argument('--cap', default='600000', help='thermal capacity J/K, comma list')
    ap.add_argument('--amb', default='28', help='room temperature degC, comma list')
    ap.add_argument('--sp', default='37.5', help='setpoint degC, comma list')
    ap.add_argument('--res', default='0.1', help='probe resolution degC, comma list')
    ap.add_argument('--jobs', type=int, default=min(4, os.cpu_count() or 1))
    ap.add_argument('--build-dir', type=Path, default=Path(tempfile.gettempdir()) / 'mayap-autotune-family')
    ap.add_argument('--rebuild', action='store_true')
    ap.add_argument('--csv', type=Path, help='also write the selected columns to this CSV file')
    a = ap.parse_args()
    binary = build(a.build_dir, a.rebuild)
    grid = list(itertools.product(floats(a.kh), floats(a.dead), floats(a.lag), floats(a.hold), floats(a.cap), floats(a.amb), floats(a.sp), floats(a.res)))
    with ThreadPoolExecutor(a.jobs) as pool:
        results = list(pool.map(one, [(binary,) + g for g in grid]))
    cols = ['kh', 'dead', 'lag', 'hold', 'cap', 'amb', 'sp', 'res', 'class', 'reason', 'last_phase', 'tune_min', 'gain_est', 'gain_true', 'delay_est', 'delay_true',
            'hold_est', 'hold_true', 'coast100', 'confidence', 'kp', 'ki', 'val_why', 'verdict', 'post_over', 'post_mae', 'post_ripple']
    out_rows = []
    for key, row, rc in results:
        kh, dead, lag, hold, cap, amb, sp, res = key
        if row is None:
            out_rows.append([kh, dead, lag, hold, cap, amb, sp, res, 'ERROR(rc=%d)' % rc] + [''] * (len(cols) - 9)); continue
        out_rows.append([kh, dead, lag, hold, cap, amb, sp, res, row['class'], row['reason'], row['last_phase'], '%.0f' % (float(row['tune_s']) / 60.0),
                         row['gain'], row['gain_true'], row['delay'], row['delay_true'], row['hold'], row['hold_true'], row['coast100'], row['confidence'],
                         row['kp'], row['ki'], row['val_why'], row['verdict'],
                         row['post_over'], row['post_mae'], row['post_ripple']])
    widths = [max(len(str(r[i])) for r in out_rows + [cols]) for i in range(len(cols))]
    print('  '.join(c.ljust(w) for c, w in zip(cols, widths)))
    for r in out_rows:
        print('  '.join(str(v).ljust(w) for v, w in zip(r, widths)))
    accepted = sum(1 for r in out_rows if r[8] == 'ACCEPTED')
    bad = sum(1 for r in out_rows if r[cols.index('verdict')] == 'ACCEPTED_BAD')
    print('\n%d virtual ovens: %d accepted, %d ACCEPTED_BAD (accepted but the independent 3 h run misses a target). SIMULATION ONLY.' % (len(out_rows), accepted, bad))
    if a.csv:
        with a.csv.open('w', newline='') as f:
            w = csv.writer(f); w.writerow(cols); w.writerows(out_rows)


if __name__ == '__main__':
    main()
