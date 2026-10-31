#!/usr/bin/env python3
"""Summary of two runs of fleet-bench.cpp: A (the baseline) against B (the change), per situation and over the fleet.

    python3 tools/thermal-research/fleet-summary.py old.csv new.csv

A situation passes when the oven is back within +-0.1 degC in time (situations 0-5: 40/40/40/60/90/40 min from the event; the fans, the one-step
overshoot and the reboot must also keep the reading above 36.5 degC). The two long situations (6: the room warms up, 7: the day profile of the last week)
pass when the reading never falls under 37.2 degC, never goes over 37.8 degC and stands at/over SP+0.1 for less than 30 minutes in all.
"""
import csv
import statistics as st
import sys

NAMES = ["climb + hold", "scheduled fan", "one step over", "door 3 min", "door + cold tray", "reboot", "room warms up", "day profile 70 %"]
RECLIM = [40, 40, 40, 60, 90, 40]


def load(path):
    d = {}
    for row in csv.reader(open(path)):
        if len(row) < 17:
            continue
        k, sc = int(row[0]), int(row[1])
        d[(k, sc)] = dict(reach=float(row[2]) / 60, peak=float(row[3]), mn=float(row[4]), mx=float(row[5]), rec=float(row[6]), iae=float(row[7]),
                          rms=float(row[8]), tover=float(row[11]), tover10=float(row[12]), fanmin=float(row[14]), starts=float(row[15]))
    return d


def passed(r, sc):
    if sc < 6:
        ok = r["rec"] <= RECLIM[sc]
        if sc in (1, 2, 5):
            ok = ok and r["mn"] >= 36.5
        return ok
    return r["mn"] >= 37.2 and r["mx"] <= 37.8 and r["tover"] < 30


def q(a, p):
    a = sorted(a)
    return a[int(p * (len(a) - 1))]


def main():
    A, B = load(sys.argv[1]), load(sys.argv[2])
    ks = sorted({k for k, _ in A} & {k for k, _ in B})
    scs = sorted({s for _, s in A} & {s for _, s in B})
    n = len(ks)
    print(f"{n} ovens")
    print(f"{'situation':18s} {'pass A':>7s} {'pass B':>7s} | {'IAE med A':>9s} {'IAE med B':>9s} | {'min A':>6s} {'min B':>6s} | {'max A':>6s} {'max B':>6s} | {'>=SP+0.1 med min A/B':>22s} | {'starts med/p90/max A':>22s} | {'B':>14s}")
    tot = {"A": 0.0, "B": 0.0}
    for sc in scs:
        pa = 100 * sum(1 for k in ks if passed(A[(k, sc)], sc)) / n
        pb = 100 * sum(1 for k in ks if passed(B[(k, sc)], sc)) / n
        tot["A"] += pa
        tot["B"] += pb
        ia, ib = st.median(A[(k, sc)]["iae"] for k in ks), st.median(B[(k, sc)]["iae"] for k in ks)
        mna, mnb = q([A[(k, sc)]["mn"] for k in ks], 0.1), q([B[(k, sc)]["mn"] for k in ks], 0.1)
        mxa, mxb = q([A[(k, sc)]["mx"] for k in ks], 0.9), q([B[(k, sc)]["mx"] for k in ks], 0.9)
        ta, tb = st.median(A[(k, sc)]["tover"] for k in ks), st.median(B[(k, sc)]["tover"] for k in ks)
        sa = [A[(k, sc)]["starts"] for k in ks]
        sb = [B[(k, sc)]["starts"] for k in ks]
        print(f"{NAMES[sc]:18s} {pa:6.0f}% {pb:6.0f}% | {ia:9.1f} {ib:9.1f} | {mna:6.2f} {mnb:6.2f} | {mxa:6.2f} {mxb:6.2f} | {ta:10.1f} {tb:10.1f} | "
              f"{q(sa, .5):4.0f}/{q(sa, .9):3.0f}/{max(sa):3.0f}           | {q(sb, .5):3.0f}/{q(sb, .9):3.0f}/{max(sb):3.0f}")
    print(f"mean pass: A {tot['A'] / len(scs):.1f} %   B {tot['B'] / len(scs):.1f} %")
    # per oven: B no worse than A on the integral of the error, summed over the situations
    w = sum(1 for k in ks if sum(B[(k, s)]["iae"] for s in scs) <= sum(A[(k, s)]["iae"] for s in scs) * 1.02)
    print(f"ovens where B is not worse than A on the summed IAE (2 % tolerance): {w}/{n}")
    c0 = [A[(k, 0)]["reach"] for k in ks]
    c1 = [B[(k, 0)]["reach"] for k in ks]
    pk0 = [A[(k, 0)]["peak"] - 37.5 for k in ks]
    pk1 = [B[(k, 0)]["peak"] - 37.5 for k in ks]
    print(f"first climb to 37.4: A {q(c0, .5):.1f}/{q(c0, .9):.1f} min  B {q(c1, .5):.1f}/{q(c1, .9):.1f} min; first peak over SP: A {q(pk0, .5):+.2f}/{q(pk0, .9):+.2f}  B {q(pk1, .5):+.2f}/{q(pk1, .9):+.2f}")


if __name__ == "__main__":
    main()
