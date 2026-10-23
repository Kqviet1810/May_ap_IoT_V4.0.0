#!/usr/bin/env python3
"""G0 check of the V2 tuning rule on LINEAR plants (no controller code exists yet).
Plant  : G(s) = g e^{-td s} / ((tau1 s + 1)(tl s + 1))   [deg C per % power]
Control: u = Kc*(SP - PV_pred) + I (PI),  PV_pred = PV + (Ghat_red - Ghat_full) u   (delay+lag removed from the prediction)
Rules  : Kc = tau1/(g (tc + b*th)),  tI = min(tau1, 4 (tc + th)),  th = td + tl/2 (half rule),  tc = a*th.
Reports, for model/plant mismatch grid, closed-loop stability, overshoot of a +SP step, and frequency-domain sensitivity peak.
Not a proof about the real oven (L6): it only fixes the SHAPE of the rule and the mismatch it must survive."""
import numpy as np, itertools, sys, json

def sim(g,tau1,td,tl, ghat,t1h,tdh,tlh, Kc,tI, step=1.0, T=6000, dt=1.0, umax=1e9):
    n=int(T/dt); dly=int(round(td/dt)); dlyh=int(round(tdh/dt))
    ubuf=np.zeros(n+dly+5); ubufh=np.zeros(n+dlyh+5)
    x1=x2=0.0      # plant air node and lag node
    h1=h2=h3=0.0   # model: full path (h1 lag1,h2 lag2) and reduced path (h3)
    I=0.0; y=np.zeros(n); sp=step
    u=0.0
    for k in range(n):
        # plant: delayed input -> tau1 lag -> tl lag
        uin=ubuf[k]   # input delayed by dly samples (u stored at k+dly)
        x1+=dt*(g*uin-x1)/tau1
        x2+=dt*(x1-x2)/tl
        yk=x2; y[k]=yk
        # model predictions
        uinh=ubufh[k]
        h1+=dt*(ghat*uinh-h1)/t1h
        h2+=dt*(h1-h2)/tlh
        h3+=dt*(ghat*u-h3)/t1h
        ypred=yk+(h3-h2)
        e=sp-ypred
        v=Kc*e+I
        u=min(max(v,0.0),umax) if umax<1e8 else v
        I+=dt*Kc*e/tI + (dt/ (tI/10.0))*(u-v) if umax<1e8 else dt*Kc*e/tI
        ubuf[k+dly]=u; ubufh[k+dlyh]=u
        if not np.isfinite(yk) or abs(yk)>1e6: return None
    return y

def tune(g,tau1,td,tl,a,b):
    th=td+tl/2.0
    tc=a*th
    Kc=tau1/(g*(tc+b*th))
    tI=min(tau1,4*(tc+th))
    return Kc,tI

def freq_peak(g,tau1,td,tl, ghat,t1h,tdh,tlh,Kc,tI):
    w=np.logspace(-5,0,4000); s=1j*w
    G=g*np.exp(-td*s)/((tau1*s+1)*(tl*s+1))
    Gf=ghat*np.exp(-tdh*s)/((t1h*s+1)*(tlh*s+1)); Gr=ghat/(t1h*s+1)
    C=Kc*(1+1/(tI*s))
    L=C*(G+Gr-Gf)
    S=1/(1+L)
    return float(np.max(np.abs(S))), float(np.max(np.abs(L/(1+L))))

PLANTS={ # name: (Cf J/C, UA, hA)  -- Pmax 16 kW. Parameter RANGES from doc/THERMAL_REDESIGN_RESEARCH.md; not oven data.
 'P1':(0.5e6,250,1300),'P2':(0.3e6,350,1700),'P3':(1.0e6,200,1000),'slow':(1.2e6,31,1000)}
if __name__=='__main__':
    rows=[]
    for a,b in [(1.0,1.0),(1.5,1.0),(2.0,1.0),(1.0,0.0),(1.5,0.0),(2.0,0.5)]:
        worst=dict(unstable=0,over=0.0,ms=0.0,cases=0)
        for name,(Cf,UA,hA) in PLANTS.items():
            tau1=Cf/(UA+hA); g=16000/100/(UA+hA)
            for td in (60,100,150):
                for tl in (30,50):
                    for dth,dg,dt1 in itertools.product((0.7,1.0,1.4),(0.7,1.0,1.3),(0.7,1.0,1.4)):
                        # model (hat) is what the controller believes; plant has the mismatch
                        ghat,t1h,tdh,tlh=g,tau1,td,tl
                        gp,t1p,tdp,tlp=g*dg,tau1*dt1,td*dth,tl
                        Kc,tI=tune(ghat,t1h,tdh,tlh,a,b)
                        y=sim(gp,t1p,tdp,tlp,ghat,t1h,tdh,tlh,Kc,tI,T=9000)
                        worst['cases']+=1
                        if y is None or abs(y[-1]-1.0)>0.1 or np.max(np.abs(np.diff(y[-1500:])))>1e-3 and np.max(y[-1500:])-np.min(y[-1500:])>0.02:
                            worst['unstable']+=1; continue
                        worst['over']=max(worst['over'],float(np.max(y)-1.0))
                        worst['ms']=max(worst['ms'],freq_peak(gp,t1p,tdp,tlp,ghat,t1h,tdh,tlh,Kc,tI)[0])
        rows.append(dict(a=a,b=b,**worst)); print(rows[-1]); sys.stdout.flush()
    json.dump(rows,open('/tmp/g0_margins.json','w'))
