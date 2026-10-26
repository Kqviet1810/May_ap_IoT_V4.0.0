#!/usr/bin/env python3
"""Thermal V3 Benchmark across Plant Set A (12 industrial incubator plants)."""
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

class PlantSim:
    def __init__(self, p):
        self.p = p
        self.Pmax = p["Pmax"]
        self.Cf = p["Cf"]
        self.Ce = p.get("Ce", 1900000)
        self.hA = p["hA"]
        self.UA = p["UA"]
        self.td = p["td"]
        self.tl = p["tl"]
        self.Tamb = p.get("Tamb", 28.0)
        self.ventW = p.get("ventW", 2500.0)
        self.tailFrac = p.get("tailFrac", 0.0)
        self.tailTau = p.get("tailTau", 120.0)
        self.sensorLag = p.get("sensorLag", 0.0)

        self.stepMs = 100
        self.dt = 0.1
        self.dlySteps = max(1, int(round(self.td / self.dt)))
        self.pipe = [0.0] * self.dlySteps
        self.pipeIdx = 0
        self.heater = 0.0
        self.heater2 = 0.0
        self.vent = 0.0
        self.Ta = self.Tamb
        self.Te = self.Tamb
        self.sensPhys = self.Tamb
        self.embryoW = 0.0

    def step(self, powerPct, ventFan=False, doorOpen=False):
        dt = self.dt
        pwr = (powerPct / 100.0) * self.Pmax

        delayed = self.pipe[self.pipeIdx]
        self.pipe[self.pipeIdx] = pwr
        self.pipeIdx = (self.pipeIdx + 1) % len(self.pipe)

        self.heater += (delayed - self.heater) * dt / max(0.1, self.tl)
        if self.tailFrac > 0:
            self.heater2 += (delayed - self.heater2) * dt / max(0.1, self.tailTau)
            heaterEff = (1.0 - self.tailFrac) * self.heater + self.tailFrac * self.heater2
        else:
            heaterEff = self.heater

        ventTarget = 1.0 if ventFan else 0.0
        self.vent += (ventTarget - self.vent) * dt / 8.0
        ventLoss = (self.ventW / 12.5) * self.vent * (self.Ta - self.Tamb)
        doorLoss = 1500.0 * (self.Ta - self.Tamb) if doorOpen else 0.0

        q_egg = self.hA * (self.Ta - self.Te)
        loss_wall = self.UA * (self.Ta - self.Tamb)

        dTa = heaterEff - loss_wall - q_egg - ventLoss - doorLoss
        dTe = q_egg + self.embryoW

        self.Ta += (dTa / self.Cf) * dt
        self.Te += (dTe / self.Ce) * dt

        if self.sensorLag > 0:
            self.sensPhys += (self.Ta - self.sensPhys) * dt / self.sensorLag
        else:
            self.sensPhys = self.Ta

        return self.Ta, self.sensPhys


class ThermalV3Controller:
    def __init__(self):
        self.reset()

    def reset(self):
        self.pv = float("nan")
        self.sp = 37.5
        self.capPct = 100.0

        self.kAir = 0.00025
        self.tauTransit = 15.0
        self.pHold = 18.0

        self.pipeLen = 20
        self.pipe = [self.pHold] * 320
        self.pipeIdx = 0
        self.secTimer = 0.0

        self.kp = 30.0
        self.ki = 0.020
        self.integral = 0.0
        self.iClamp = 15.0

        self.pvHist = []
        self.filteredSlope = 0.0
        self.peakClimbSlope = 0.0

        self.clockS = 0.0
        self.heaterOnTime = None
        self.pvAtOn = None
        self.identified = False

        self.doorDetected = False
        self.doorTimer = 0.0
        self.stuckTimer = 0.0

    def step(self, pv, sp, permit=True, ventFan=False, dt=0.1):
        self.clockS += dt
        if not permit or math.isnan(pv):
            return 0.0

        self.pv = pv
        self.sp = sp

        if self.heaterOnTime is None:
            self.heaterOnTime = self.clockS
            self.pvAtOn = self.pv

        # Slope estimation (sliding window 20s)
        self.pvHist.append(pv)
        if len(self.pvHist) > 200:
            self.pvHist.pop(0)
        if len(self.pvHist) >= 30:
            rawSlope = (self.pvHist[-1] - self.pvHist[0]) / ((len(self.pvHist) - 1) * dt)
            self.filteredSlope += min(1.0, dt / 3.0) * (rawSlope - self.filteredSlope)

        # Transit time measurement
        if not self.identified:
            if (pv - self.pvAtOn) >= 0.15:
                measuredDelay = self.clockS - self.heaterOnTime
                self.tauTransit = max(5.0, min(180.0, measuredDelay * 1.05))
                self.pipeLen = max(5, min(319, int(self.tauTransit * 1.35)))
                self.identified = True

        # Track peak climb slope while heating at 100%
        if pv < sp - 1.0 and self.filteredSlope > self.peakClimbSlope:
            self.peakClimbSlope = self.filteredSlope
            estK = self.peakClimbSlope / max(10.0, 100.0 - self.pHold)
            self.kAir = max(0.0001, min(0.0007, estK))

        # DOOR OPEN DETECTION
        if self.filteredSlope < -0.012 and (self.sp - self.pv) > 0.4:
            self.doorDetected = True
            self.doorTimer = 0.0

        if self.doorDetected:
            self.doorTimer += dt
            if self.filteredSlope > -0.003 and self.doorTimer > 15.0:
                self.doorDetected = False

        # Update energy-in-flight pipe once per second
        self.secTimer += dt
        if self.secTimer >= 1.0:
            self.secTimer -= 1.0

        excessEnergy = sum(self.pipe[:self.pipeLen]) - self.pipeLen * self.pHold
        pvPredicted = max(self.pv - 0.20, self.pv + self.kAir * excessEnergy)

        errPred = self.sp - pvPredicted
        errTrue = self.sp - self.pv

        # Asymmetric P on predicted error
        if errPred >= 0:
            pTerm = self.kp * errPred
        else:
            pTerm = (self.kp * 3.0) * errPred

        # Conditional I on true error (within +-0.75 C)
        if not self.doorDetected and abs(errTrue) <= 0.75:
            self.integral += self.ki * errTrue * dt
            self.integral = max(-self.iClamp, min(self.iClamp, self.integral))

            # Slow adaptation of pHold
            shift = self.integral * (dt / 300.0)
            self.pHold = max(5.0, min(45.0, self.pHold + shift))
            self.integral -= shift
        elif not self.doorDetected:
            self.integral *= max(0.0, 1.0 - 0.05 * dt)

        # Anti-stuck: if PV stays below SP - 0.08 C for > 120s with flat slope
        if not self.doorDetected and errTrue >= 0.08 and abs(self.filteredSlope) < 0.0015:
            self.stuckTimer += dt
            if self.stuckTimer > 120.0:
                self.pHold = min(45.0, self.pHold + 0.02 * dt)
        else:
            self.stuckTimer = 0.0

        cmd = self.pHold + pTerm + self.integral

        if self.doorDetected:
            cmd = min(cmd, self.pHold + 6.0)

        cmd = max(0.0, min(self.capPct, cmd))

        if self.secTimer < dt:
            self.pipe[self.pipeIdx] = cmd
            self.pipeIdx = (self.pipeIdx + 1) % self.pipeLen

        return cmd


def run_benchmark():
    with open(ROOT / "tools/thermal-v3/plants.json") as f:
        data = json.load(f)

    plants = data["A"]
    results = []

    for p in plants:
        pid = p["id"]
        sim = PlantSim(p)
        sim.Ta = 30.0; sim.Te = 30.0; sim.sensPhys = 30.0

        ctl = ThermalV3Controller()
        maxTa = -100.0
        reachTime = None

        totalSteps = int(7200 / 0.1)
        for step in range(totalSteps):
            t = step * 0.1
            measPv = round(sim.sensPhys * 10.0) / 10.0
            doorOpen = (3600.0 <= t < 3720.0)
            ventFan = (5400.0 <= t < 5580.0)

            cmd = ctl.step(measPv, 37.5, permit=True, ventFan=ventFan, dt=0.1)
            ta, ts = sim.step(cmd, ventFan=ventFan, doorOpen=doorOpen)

            if t > 300.0 and ta > maxTa:
                maxTa = ta
            if reachTime is None and ta >= 37.2:
                reachTime = t

        overshoot = max(0.0, maxTa - 37.5)
        results.append({
            "id": pid,
            "Pmax": p["Pmax"],
            "Cf": p["Cf"],
            "td": p["td"],
            "tl": p["tl"],
            "reach_s": reachTime,
            "max_ta": round(maxTa, 3),
            "overshoot": round(overshoot, 3),
            "end_ta": round(sim.Ta, 3),
            "kAir": round(ctl.kAir * 1e4, 2),
            "tau_transit": round(ctl.tauTransit, 1),
            "pHold": round(ctl.pHold, 1)
        })

    print(f"{'Plant':<6} | {'Pmax':<6} | {'Cf (kJ)':<8} | {'Delay':<6} | {'Reach (m)':<10} | {'Max Ta':<8} | {'Overshoot':<10} | {'End Ta':<8} | {'kAir*1e4':<8} | {'Hold %':<6}")
    print("-" * 100)
    for r in results:
        reachM = f"{r['reach_s']/60:.1f}m" if r['reach_s'] else "N/A"
        print(f"{r['id']:<6} | {r['Pmax']:<6} | {r['Cf']/1000:<8.0f} | {r['td']:<6} | {reachM:<10} | {r['max_ta']:<8.2f} | {r['overshoot']:<10.2f} | {r['end_ta']:<8.2f} | {r['kAir']:<8.2f} | {r['pHold']:<6.1f}")

if __name__ == "__main__":
    run_benchmark()
