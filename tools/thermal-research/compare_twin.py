import csv,statistics as st,sys
f=float
def load(p): return {(r['scn'],r['Cf'],r['hA'],r['fan'],r['eggEnd'],r['dead']):r for r in csv.DictReader(open(p))}
sets=[('fw1.1.14','twin_20k.csv')]+[(a,b) for a,b in [x.split('=') for x in sys.argv[1:]]]
data=[(n,load(p)) for n,p in sets]
def q(x,p): x=sorted(x); return x[min(len(x)-1,int(p*(len(x)-1)+0.5))]
def tri(v): return "%5.2f/%5.2f/%5.2f"%(st.median(v),q(v,.9),max(v))
for scn in ['T1_batch1','T3_kick','T4_door3','T5_embryoRamp','T6_amb+5']:
    keys=[k for k in data[0][1] if k[0]==scn and all(k in d for _,d in data)]
    print("\n%s  n=%d (same ovens)"%(scn,len(keys)))
    for n,d in data:
        R=[d[k] for k in keys]
        over=[f(r['over']) for r in R]; under=[-f(r['under']) for r in R]
        t=[(f(r['reach_s']) if scn=='T1_batch1' else 0)+f(r['recov_s']) for r in R]
        ib=[f(r['inband_pct']) for r in R]
        print("  %-14s over %s | under %s | %s_s %6.0f/%6.0f/%6.0f | inband med %5.1f min %5.1f | High %d"%(n,tri(over),tri(under),"stable" if scn=='T1_batch1' else "recov",st.median(t),q(t,.9),max(t),st.median(ib),min(ib),sum(int(r['high']) for r in R)))
