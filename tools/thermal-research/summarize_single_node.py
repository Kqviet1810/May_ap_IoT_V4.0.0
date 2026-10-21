import csv,sys,statistics as st
rows=list(csv.DictReader(open(sys.argv[1])))
f=float
def q(x,p): x=sorted(x); return x[min(len(x)-1,int(p*(len(x)-1)+0.5))]
scn=[]
for r in rows:
    if r['scn'] not in scn: scn.append(r['scn'])
print("%-11s %3s | %-21s | %-21s | %-24s | %-16s | %s"%("scenario","n","over  med/p90/max","under med/p90/max","recov s  med/p90/max","inband%% med/min","High"))
for s in scn:
    rs=[r for r in rows if r['scn']==s]
    ov=[f(r['over']) for r in rs]; un=[-f(r['under']) for r in rs]; rc=[f(r['recov_s']) for r in rs]; ib=[f(r['inband_pct']) for r in rs]
    print("%-11s %3d | %5.2f %5.2f %5.2f | %5.2f %5.2f %5.2f | %6.0f %6.0f %6.0f | %5.1f %5.1f | %d"%(s,len(rs),st.median(ov),q(ov,.9),max(ov),st.median(un),q(un,.9),max(un),st.median(rc),q(rc,.9),max(rc),st.median(ib),min(ib),sum(int(r['high']) for r in rs)))
