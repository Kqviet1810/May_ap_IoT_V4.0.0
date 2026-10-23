> **Đã gỡ khỏi firmware 2.0.0 (Thermal Control V2.0.0).** Tài liệu này mô tả hệ nhiệt cũ (≤ 1.1.15) đã bị xoá khỏi nhánh `thermal-control-v2.0.0`; chỉ còn giá trị lịch sử. Mã nguồn cũ nằm ở `main` (e8b557e) và nhánh `claude/gracious-hopper-zjvufg`. Xem `doc/THERMAL_V2_REMOVAL_REPORT.md`.

# Thermal Phase 1: software commissioning evidence

This branch is **not physically qualified**. It retains the single GPIO1 16 kW
bank, the 300 ms pulse-density scheduler, existing sensor filter, fault limits,
OutputArbiter, and AutoTune. Do not merge or flash it for production solely on
the basis of these simulations.

The frozen `f7e4373` controller and this branch run through the same extracted
production `updateHeatingAndOutputs` route, actual `ThermalController`, sensor
filter, `HeaterBurstScheduler`, and `OutputArbiter`. The model has a first-order
chamber, heater lag, transport delay, quantization and bounded noise. High and
Emergency counts are temperature-threshold crossings in the model with fault
injection at the next control cycle; they are not confirmed field alarms.

The fixed 788-case matrix has 720 cold starts (SP 30/32/35/37.5 °C; ambient
10/20/25/28/35 °C; capacities 180/600/1600 kJ/°C; loss 120/180/300 W/°C;
dead time 0/5/15/30/60/120 s; effective heater 70/100/120% of the nominal
model; 3/8/30 s heater lag; resolution 0.1/0.01 °C; some 0.08 °C sensor bias)
plus 68 disturbances (doors −1/−2/−5 °C, ambient rise, loss up/down, light/heavy
load changes, vent, sensor loss/frozen/bad sample, safety cut, power recovery,
setpoint step, noise and sample jitter/drop). The matrix intentionally includes
ambient >= SP and high-loss plants; the heater cannot cool an overambient room.

| Extracted route | Full target pass | Full target fail | Cases crossing High | Cases crossing Emergency |
| --- | ---: | ---: | ---: | ---: |
| Frozen baseline | 40 / 788 | 748 | 296 | 180 |
| Phase 1 | 186 / 788 | 602 | 0 | 0 |

The full target requires overshoot <=0.3 °C, tail MAE <=0.1 °C, tail P95
<=0.15 °C, tail ripple <=0.25 °C and finite settling. Phase 1 does **not**
meet that performance target across the sweep. In cold starts with ambient
below SP, the median tail MAE is approximately 0.15/0.17/0.24/0.25 °C for
SP 30/32/35/37.5 °C respectively. A physically reachable heavy plant
(SP 37.5 °C, ambient 10 °C, capacity 1600 kJ/°C, loss 300 W/°C, dead time
120 s) still has about 8 °C tail MAE; the conservative coast estimate limits
heater duty too much. A fast plant plus a frozen sensor can exceed the 0.3 °C
overshoot target after recovery while remaining below High. These failures are
visible in `orchestration-baseline.csv` and `orchestration-phase1.csv` generated
by `tools/test_thermal_control.py`.

The model does not establish the actual chamber capacity, the locations and
uniformity of its sensors, true SSR power, fan/door effects, absolute sensor
accuracy, relay failure response, or safe tuning on a physical incubator.
Review model limits and instrument a bench run before considering a production
merge. Keep the existing High/Emergency, local safety and independent hardware
cut paths active during commissioning.
