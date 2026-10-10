# Command manifest (run from repo root, HEAD 8d51432, g++ 13.3, 4 jobs)

    python3 tools/test_thermal_adaptive.py --full --report-dir <dir>/adaptive --jobs 4   # 1m40s
    python3 tools/test_smart_autotune.py --report-dir <dir>/autotune --jobs 4            # 0m29s
    python3 tools/test_thermal_control.py --report-dir <dir>/control                     # 2m54s

Copied here: adaptive `legacy788.csv`→`adaptive-788.csv`, `matrix.csv`→`adaptive-2160.csv`, `vent.csv`→`adaptive-vent63x3.csv`,
`hwchange.csv`, `faults.csv`; Smart AutoTune `full.csv`→`smart-autotune-full540.csv`, `mini.csv`→`smart-autotune-mini36.csv`;
console logs `adaptive.log`, `smart-autotune.log`, `thermal-control.log` (host temp paths replaced by `<report-dir>`).
These files are the immutable reference: later phases must add new files, never overwrite these.
