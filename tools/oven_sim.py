#!/usr/bin/env python3
"""Oven simulation for the CURRENT firmware (Windows / Linux / macOS).  SIMULATION ONLY - not a physical qualification.

The controller in the simulation is the real firmware heating code (PID, Adaptive V1, Smart Thermal, burst scheduler, output arbiter),
compiled for the PC by g++.  Only the oven is a model: first-order heat balance with transport delay and heater lag.

  python tools/oven_sim.py --config tools/oven_sim/oven.example.json --out oven-report

Needs: Python 3.8+ and g++ (Windows: MSYS2 UCRT64 `pacman -S mingw-w64-ucrt-x86_64-gcc`).  No other packages.
Outputs in --out: report.html (open in a browser), report.md, results.csv, trace_*.csv
"""
import argparse, csv, hashlib, html, itertools, json, math, os, shutil, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / 'tools/oven_sim/oven_sim.cpp'
REFERENCE = ROOT / 'tools/oven_sim/reference.json'
EXE = '.exe' if os.name == 'nt' else ''
CAP = 600000.0          # J/degC: only the ratios matter to the controller (duty based); fixed so the model stays well scaled
BANK_W = 16000.0        # the simulator's nominal heater bank (eff scales it)
MODES = [('legacy', 0, 'PID cu (V4)'), ('adaptive', 2, 'Adaptive V1'), ('smart', 3, 'Smart Thermal')]
# Acceptance used by the firmware test-suite (sim::Result::pass).
CRITERIA = 'khong High 38.2 / Emergency 39.0; vot lo <= 0.30; MAE <= 0.10; P95 <= 0.15; dao dong <= 0.25 (30 phut cuoi); on dinh trong han'
SAFE_KH_C_PER_S = 0.08   # measured in simulation: no High/Emergency up to 0.12 C/s, danger starts ~0.15


def find_gpp():
    g = shutil.which('g++')
    if g: return g
    for c in (r'C:\msys64\ucrt64\bin\g++.exe', r'C:\msys64\mingw64\bin\g++.exe', r'C:\msys64\clang64\bin\g++.exe'):
        if Path(c).exists(): return c
    sys.exit('Khong tim thay g++. Windows: cai MSYS2 (https://www.msys2.org), mo "MSYS2 UCRT64", chay: pacman -S mingw-w64-ucrt-x86_64-gcc  '
             'roi them C:\\msys64\\ucrt64\\bin vao PATH.')


def source_key():
    h = hashlib.sha256()
    for pat in ('MAYAP_INDUSTRIAL_v1_0_0/*.h', 'tests/thermal-*.h', 'tests/actual-*.inc', 'tools/oven_sim/oven_sim.cpp', 'tools/test_thermal_control.py'):
        for f in sorted(ROOT.glob(pat)): h.update(f.name.encode()); h.update(f.read_bytes())
    return h.hexdigest()


def build(work):
    work.mkdir(parents=True, exist_ok=True)
    binary = work / ('oven_sim' + EXE); keyf = work / 'build.key'
    key = source_key()
    if binary.exists() and keyf.exists() and keyf.read_text() == key: return binary
    inc = work / 'include'
    print('Dang tao include tu ma firmware...', flush=True)
    subprocess.run([sys.executable, str(ROOT / 'tools/test_thermal_control.py'), '--emit-includes', str(inc)], check=True, stdout=subprocess.DEVNULL)
    print('Dang bien dich bo mo phong (lan dau ~15-60 giay)...', flush=True)
    subprocess.run([find_gpp(), '-std=c++11', '-O2', '-I', str(inc), str(DRIVER), '-o', str(binary)], check=True)
    keyf.write_text(key)
    return binary


def run_one(binary, args, trace=None):
    cmd = [str(binary)] + [str(a) for a in args] + (['--trace', str(trace)] if trace else [])
    out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout.strip().splitlines()[-1].split(',')
    k = ['class', 'mode', 'pass', 'high', 'emergency', 'overshoot', 'mae', 'p95', 'ripple', 'settling_s', 'rise_s', 'energy_j', 'confidence']
    r = dict(zip(k, out))
    for n in k[2:]: r[n] = float(r[n])
    return r


def plant_args(cfg, kh_min, tau_min, dead_s, amb, res):
    """Oven numbers -> simulator parameters.  Kh = 16000*eff/C (C/s at 100 %), tau = C/loss."""
    kh_s = kh_min / 60.0
    return ['--cap', CAP, '--eff', kh_s * CAP / BANK_W, '--loss', CAP / (tau_min * 60.0), '--dead', dead_s, '--lag', cfg['lag_s'],
            '--amb', amb, '--res', res, '--noise', cfg['noise_c'], '--sp', cfg['setpoint_c'], '--hours', cfg['hours']]


def hold_duty(cfg, kh_min, tau_min, amb):
    return (cfg['setpoint_c'] - amb) / (tau_min * 60.0 * kh_min / 60.0)  # dT / (tau * Kh)


def read_trace(path):
    rows = list(csv.DictReader(open(path)))
    return [(float(r['t']) / 60.0, float(r['temp']), float(r['fed']), float(r['sp']), float(r['duty'])) for r in rows]


def event_metrics(tr, at_min, sp, drop):
    after = [x for x in tr if x[0] >= at_min]
    if not after: return {}
    dip = min(x[1] for x in after) - sp
    low = min(after, key=lambda x: x[1])
    rec = next((x[0] - at_min for x in after if x[0] > low[0] and x[1] >= sp - 0.15), None)
    return {'dip': dip, 'recover_min': rec, 'peak_after': max(x[1] for x in after) - sp}


def svg_chart(series, w=720, h=230, ylab='C', ymin=None, ymax=None, hlines=()):
    pad = 42
    xs = [p[0] for s in series for p in s['pts']]; ys = [p[1] for s in series for p in s['pts']]
    x0, x1 = min(xs), max(xs); y0 = ymin if ymin is not None else min(ys); y1 = ymax if ymax is not None else max(ys)
    if y1 - y0 < 1e-9: y1 = y0 + 1
    X = lambda x: pad + (x - x0) / (x1 - x0 or 1) * (w - pad - 10); Y = lambda y: h - 24 - (y - y0) / (y1 - y0) * (h - 24 - 12)
    o = ['<svg viewBox="0 0 %d %d" width="100%%" role="img" style="background:#fff;border:1px solid #ccd;border-radius:6px">' % (w, h)]
    for i in range(5):
        v = y0 + (y1 - y0) * i / 4
        o.append('<line x1="%d" x2="%d" y1="%.1f" y2="%.1f" stroke="#e3e6ee"/><text x="2" y="%.1f" font-size="10" fill="#555">%.2f</text>' % (pad, w - 10, Y(v), Y(v), Y(v) + 3, v))
    for i in range(5):
        v = x0 + (x1 - x0) * i / 4
        o.append('<text x="%.1f" y="%d" font-size="10" fill="#555" text-anchor="middle">%.0f ph</text>' % (X(v), h - 8, v))
    for y, col, label in hlines:
        if y0 <= y <= y1: o.append('<line x1="%d" x2="%d" y1="%.1f" y2="%.1f" stroke="%s" stroke-dasharray="4 3"/><text x="%d" y="%.1f" font-size="10" fill="%s">%s</text>' % (pad, w - 10, Y(y), Y(y), col, w - 120, Y(y) - 3, col, label))
    for s in series:
        d = ' '.join('%s%.1f,%.1f' % ('M' if i == 0 else 'L', X(p[0]), Y(p[1])) for i, p in enumerate(s['pts']))
        o.append('<path d="%s" fill="none" stroke="%s" stroke-width="1.6"/>' % (d, s['color']))
    lx = pad + 6
    for s in series:
        o.append('<rect x="%d" y="4" width="10" height="3" fill="%s"/><text x="%d" y="9" font-size="10" fill="#333">%s</text>' % (lx, s['color'], lx + 14, html.escape(s['name']))); lx += 20 + 6 * len(s['name'])
    o.append('</svg>')
    return ''.join(o)


def selfcheck(binary):
    if not REFERENCE.exists(): return 'chua co reference.json'
    ref = json.loads(REFERENCE.read_text()); bad = []
    for case in ref['cases']:
        r = run_one(binary, case['args'])
        for k, tol in (('mae', 0.002), ('p95', 0.003), ('overshoot', 0.003), ('ripple', 0.003)):
            if abs(r[k] - case['expect'][k]) > tol: bad.append('%s %s: %.4f != %.4f' % (case['name'], k, r[k], case['expect'][k]))
        if r['pass'] != case['expect']['pass'] or r['high'] != case['expect']['high']: bad.append('%s pass/high khac' % case['name'])
    return 'OK' if not bad else 'LECH: ' + '; '.join(bad)


def main():
    if hasattr(sys.stdout, 'reconfigure'): sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--config', type=Path, default=ROOT / 'tools/oven_sim/oven.example.json')
    ap.add_argument('--out', type=Path, default=Path('oven-report'))
    ap.add_argument('--work', type=Path, default=ROOT / 'tools/oven_sim/.build')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 2)
    ap.add_argument('--strict', action='store_true', help='exit 2 if the self-check against the reference results fails (used by CI)')
    ap.add_argument('--make-reference', action='store_true', help=argparse.SUPPRESS)
    a = ap.parse_args()
    cfg = json.loads(a.config.read_text(encoding='utf-8'))
    t0 = time.time(); binary = build(a.work)
    if a.make_reference:
        cases = []
        for name, args in (('adaptive-6h', ['--cap', 600000, '--eff', 0.3125, '--loss', 111.0, '--dead', 15, '--lag', 8, '--amb', 25, '--sp', 37.5, '--hours', 6, '--mode', 2]),
                           ('legacy-6h', ['--cap', 600000, '--eff', 0.3125, '--loss', 111.0, '--dead', 15, '--lag', 8, '--amb', 25, '--sp', 37.5, '--hours', 6, '--mode', 0]),
                           ('smart-6h', ['--cap', 600000, '--eff', 0.3125, '--loss', 111.0, '--dead', 15, '--lag', 8, '--amb', 25, '--sp', 37.5, '--hours', 6, '--mode', 3])):
            r = run_one(binary, args); cases.append({'name': name, 'args': args, 'expect': {k: r[k] for k in ('pass', 'high', 'mae', 'p95', 'overshoot', 'ripple')}})
        REFERENCE.write_text(json.dumps({'note': 'Linux g++ reference; tolerance in tools/oven_sim.py selfcheck()', 'cases': cases}, indent=1)); print('reference ghi'); return
    chk = selfcheck(binary); print('Tu kiem tra so voi ket qua chuan:', chk, flush=True)
    a.out.mkdir(parents=True, exist_ok=True)
    u = cfg.get('uncertainty', {})
    kh0, tau0, dead0, res0 = cfg['kh_c_per_min'], cfg['cool_tau_min'], cfg['dead_s'], cfg['resolution_c']
    khs = [kh0 * (1 - u.get('kh_pct', 0) / 100), kh0, kh0 * (1 + u.get('kh_pct', 0) / 100)]
    taus = [tau0 * (1 - u.get('tau_pct', 0) / 100), tau0, tau0 * (1 + u.get('tau_pct', 0) / 100)]
    deads = [max(1.0, dead0 * (1 - u.get('dead_pct', 0) / 100)), dead0, dead0 * (1 + u.get('dead_pct', 0) / 100)]
    ambs = u.get('ambient_c', [cfg['ambient_c']])
    grid = sorted(set(itertools.product(khs, taus, deads, ambs)))
    jobs = [(m, g) for m in MODES for g in grid]
    print('Chay %d ca luoi + kich ban tren %d luong...' % (len(jobs), a.jobs), flush=True)

    def work(j):
        (name, mid, _), (kh, tau, dead, amb) = j
        r = run_one(binary, plant_args(cfg, kh, tau, dead, amb, res0) + ['--mode', mid]); r.update(kh=kh, tau=tau, dead=dead, amb=amb, ctl=name)
        return r
    with ThreadPoolExecutor(a.jobs) as ex: res = list(ex.map(work, jobs))
    with open(a.out / 'results.csv', 'w', newline='') as f:
        w = csv.writer(f); keys = ['ctl', 'kh', 'tau', 'dead', 'amb', 'class', 'pass', 'high', 'emergency', 'overshoot', 'mae', 'p95', 'ripple', 'settling_s']
        w.writerow(keys); [w.writerow([r[k] for k in keys]) for r in res]

    # scenarios on the nominal oven, with traces
    nom = plant_args(cfg, kh0, tau0, dead0, cfg['ambient_c'], res0)
    drop, at = cfg.get('door_drop_c', 2.0), cfg.get('door_at_hour', 4)
    scen = [('khoi_dong', 'Khoi dong tu nhiet phong', []),
            ('mo_cua', 'Mo cua (nhiet tut %.1f C luc gio %s)' % (drop, at), ['--event', 'door', '--eventAt', at, '--eventValue', drop]),
            ('doi_setpoint', 'Doi setpoint +1.0 C luc gio %s' % at, ['--event', 'setpoint_step', '--eventAt', at, '--spBefore', cfg['setpoint_c'] - 1.0])]
    sres = {}
    for sid, _, extra in scen:
        for name, mid, _ in MODES:
            tp = a.out / ('trace_%s_%s.csv' % (sid, name))
            r = run_one(binary, nom + ['--mode', mid] + extra, trace=tp); tr = read_trace(tp)
            if sid != 'khoi_dong': r['ev'] = event_metrics(tr, at * 60, cfg['setpoint_c'], drop)
            sres[(sid, name)] = (r, tr)

    # ------------------------------------------------------------------ report
    md = ['# Bao cao mo phong lo (MO PHONG - chua phai kiem dinh vat ly)', '',
          '* Bo dieu khien: ma firmware that chay tren PC. Lo: mo hinh bac 1 + tre. Tieu chi dat: ' + CRITERIA,
          '* Lo: Kh=%.3f C/phut (%.4f C/s) | tau nguoi=%g phut | tre=%g s + %g s | nhiet phong=%g C | setpoint=%g C | cam bien %g C | %g gio/ca' % (kh0, kh0 / 60, tau0, dead0, cfg['lag_s'], cfg['ambient_c'], cfg['setpoint_c'], res0, cfg['hours']),
          '* Tu kiem tra so voi ket qua chuan (Linux): %s' % chk, '']
    warn = []
    hd = hold_duty(cfg, kh0, tau0, cfg['ambient_c'])
    md.append('* Cong suat can de giu %g C o danh nghia: ~%.0f%% (suy ra tu Kh, tau)' % (cfg['setpoint_c'], hd * 100))
    if kh0 / 60 >= SAFE_KH_C_PER_S: warn.append('Kh %.3f C/s >= %.2f: vung gan nguong nguy hiem (High/Emergency co the xay ra).' % (kh0 / 60, SAFE_KH_C_PER_S))
    if hd > 0.8: warn.append('Can >80%% cong suat de giu setpoint: lo khong du cong suat (HEAT_LIMITED).')
    if hd < 0.02: warn.append('Cong suat giu qua nho (<2%%): kiem tra lai so do tau/Kh.')
    for x in warn: md.append('* **CANH BAO:** ' + x)
    md += ['', '## Tong ket %d ca luoi (Kh, tau, tre, nhiet phong thay doi trong khoang bat dinh)' % len(grid), '',
           '| Bo dieu khien | Dat | Dat (chi ca lo du cong suat) | High | Emergency | Vot lo max | MAE max | P95 max | Dao dong max |', '|---|---|---|---|---|---|---|---|---|']
    summ = {}
    for name, _, label in MODES:
        rs = [r for r in res if r['ctl'] == name]; reach = [r for r in rs if r['class'] == 'REACHABLE']
        summ[name] = (rs, reach)
        md.append('| %s | %d/%d | %d/%d | %d | %d | %.3f | %.3f | %.3f | %.3f |' % (label, sum(r['pass'] for r in rs), len(rs), sum(r['pass'] for r in reach), len(reach),
                  sum(1 for r in rs if r['high']), sum(1 for r in rs if r['emergency']), max(r['overshoot'] for r in rs), max(r['mae'] for r in rs), max(r['p95'] for r in rs), max(r['ripple'] for r in rs)))
    md += ['', '## Kich ban tren lo danh nghia', '', '| Kich ban | Bo dieu khien | Dat | Vot lo | MAE | P95 | Dao dong | Tut nhiet | Hoi phuc (phut) |', '|---|---|---|---|---|---|---|---|---|']
    for sid, title, _ in scen:
        for name, _, label in MODES:
            r, _t = sres[(sid, name)]; ev = r.get('ev', {})
            md.append('| %s | %s | %s | %.3f | %.3f | %.3f | %.3f | %s | %s |' % (title, label, 'DAT' if r['pass'] else 'KHONG', r['overshoot'], r['mae'], r['p95'], r['ripple'],
                      '%.2f' % ev['dip'] if 'dip' in ev else '-', ('%.0f' % ev['recover_min'] if ev.get('recover_min') is not None else ('khong' if ev else '-'))))
    bad = [r for r in res if not r['pass']]
    if bad:
        md += ['', '## Ca khong dat (toi da 12; ca REACHABLE truoc, HEAT_LIMITED = lo khong du cong suat)', '', '| Bo DK | Kh | tau | tre | phong | lop | High | Vot lo | MAE |', '|---|---|---|---|---|---|---|---|---|']
        for r in sorted(bad, key=lambda r: (r['class'] != 'REACHABLE', -r['high'], -r['mae']))[:12]:
            md.append('| %s | %.3f | %.0f | %.0f | %.0f | %s | %d | %.3f | %.3f |' % (r['ctl'], r['kh'], r['tau'], r['dead'], r['amb'], r['class'], r['high'], r['overshoot'], r['mae']))
    md += ['', 'Thoi gian chay: %.1f giay.' % (time.time() - t0)]
    (a.out / 'report.md').write_text('\n'.join(md), encoding='utf-8')

    colors = {'legacy': '#b06000', 'adaptive': '#0a6bd1', 'smart': '#1a9a4a'}
    page = ['<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Bao cao mo phong lo</title>',
            '<style>body{font:15px system-ui,sans-serif;max-width:900px;margin:16px auto;padding:0 16px;color:#1a2233}table{border-collapse:collapse;width:100%;font-size:13px;margin:8px 0}td,th{border:1px solid #ccd;padding:4px 6px;text-align:left}th{background:#eef1f7}h2{margin-top:28px}.w{background:#fff3cd;border:1px solid #e0b100;padding:8px;border-radius:6px}</style>']
    body = html.escape('\n'.join(md)).split('\n'); inlines = []
    page.append('<h1>Bao cao mo phong lo</h1><p><b>MO PHONG</b> - chua phai kiem dinh vat ly. Ma dieu khien la firmware that; chi co lo la mo hinh.</p>')
    for x in warn: page.append('<p class="w"><b>CANH BAO:</b> %s</p>' % html.escape(x))
    # simple md->html for tables/lists
    in_t = False
    for line in md:
        if line.startswith('|'):
            cells = [c.strip() for c in line.strip('|').split('|')]
            if set(''.join(cells)) <= set('-'): continue
            tag = 'th' if not in_t else 'td'
            if not in_t: page.append('<table>')
            in_t = True; page.append('<tr>' + ''.join('<%s>%s</%s>' % (tag, html.escape(c), tag) for c in cells) + '</tr>')
            continue
        if in_t: page.append('</table>'); in_t = False
        if line.startswith('# '): continue
        if line.startswith('## '): page.append('<h2>%s</h2>' % html.escape(line[3:]))
        elif line.startswith('* '): page.append('<p>%s</p>' % html.escape(line[2:]).replace('**', ''))
        elif line: page.append('<p>%s</p>' % html.escape(line))
    if in_t: page.append('</table>')
    sp = cfg['setpoint_c']
    for sid, title, _ in scen:
        page.append('<h2>Do thi: %s</h2>' % html.escape(title))
        for name, _, label in MODES:
            r, tr = sres[(sid, name)]
            page.append('<h3>%s - %s</h3>' % (html.escape(label), 'DAT' if r['pass'] else 'KHONG DAT'))
            page.append(svg_chart([{'name': 'nhiet that', 'color': colors[name], 'pts': [(x[0], x[1]) for x in tr]}, {'name': 'cam bien', 'color': '#999', 'pts': [(x[0], x[2]) for x in tr]},
                                   {'name': 'setpoint', 'color': '#d33', 'pts': [(x[0], x[3]) for x in tr]}], ymin=sp - 1.5 if sid != 'khoi_dong' else cfg['ambient_c'], ymax=sp + 1.0 if sid != 'khoi_dong' else sp + 1.0,
                                  hlines=[(38.2, '#d33', 'High 38.2')]))
            page.append(svg_chart([{'name': 'cong suat heater (trung binh 1 phut)', 'color': '#555', 'pts': [(x[0], x[4] * 100) for x in tr]}], h=110, ymin=0, ymax=100))
    (a.out / 'report.html').write_text('\n'.join(page), encoding='utf-8')
    print('\n'.join(md)); print('\nMo file:', (a.out / 'report.html').resolve())


if __name__ == '__main__': main()
