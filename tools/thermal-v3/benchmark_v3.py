#!/usr/bin/env python3
"""Thermal V3 closed-loop benchmark: builds bench_v3.cpp against the REAL thermal_v3.h and runs it on plants.json (sets A and OWNER).

SIMULATION ONLY - a pass is not proof for a real oven.  Exit code 1 when any case is outside the acceptance limits printed by the program."""
import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def plant_rows(spec):
    common = spec["common"]
    rows = []
    for p in spec["A"]:
        rows.append([p["id"], p["Pmax"], p["Cf"], p.get("Ce", common["Ce"]), p["hA"], p["UA"], p["td"], p["tl"], p.get("Tamb", common["Tamb"]), p.get("sensorLag", 0)])
    for p in spec.get("OWNER", []):
        rows.append([p["id"], p["Pmax"], p["Cf"], p["Ce"], p["hA"], p["UA"], p["td"], p["tl"], p["Tamb"], p.get("sensorLag", 0)])
    return rows


def main():
    spec = json.loads((HERE / "plants.json").read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory() as tmp:
        csv_path = Path(tmp) / "plants.csv"
        with csv_path.open("w", newline="") as fh:
            w = csv.writer(fh, lineterminator="\n")
            for r in plant_rows(spec):
                w.writerow(r)
        exe = Path(tmp) / "bench_v3"
        subprocess.run(["g++", "-std=c++11", "-O2", "-Wall", "-Wextra", "-Werror", str(HERE / "bench_v3.cpp"), "-o", str(exe)], check=True)
        return subprocess.run([str(exe), str(csv_path)]).returncode


if __name__ == "__main__":
    sys.exit(main())
