# D6 — incubation programme and egg cooling: architecture, persistence, gating

Status: **IMPLEMENTED + TESTED on the host; NOT wired into the firmware, defaults OFF; wiring PENDING approval** (EEPROM address allocation, HMI/Web schema, event codes).
| Piece | File | State |
|---|---|---|
| Set-point programme (validated stages, slew-limited) | `thermal_program.h` | tested (440 658 checks incl. 400 000-step cooling fuzz) |
| **Confirmation gate** | same | `configure()` only stores; the programme acts on a batch only after `activateForBatch(batchId)` for THAT batch; re-configuring, a different batch id, `deactivate()`, or a reboot without the persisted confirmation leaves the configured set point untouched |
| Egg cooling policy + state machine (default OFF) | `egg_cooling.h` | tested; heater-inhibit/low-temp-suppress requests only |
| A/B persistence (CRC32, wrap-safe sequence, read-back verified, wear-limited) | `program_store.h` | tested (248 checks): torn write at **every byte** of the record keeps the previous record whole; both slots bad / unknown version ⇒ defaults (OFF); newer schema never guessed; unchanged content is not written; minimum interval between writes |
| Records | `ProgramRecord` (44 B, includes the batch id the operator confirmed), `PolicyRecord` (26 B), `CoolingRecord` (24 B) | packed, versioned |
Rules honoured: no species/day schedule is shipped; Egg Cooling is OFF by default and its policy has absolute floors (≥ 30.0 °C, ≤ 90 min); a programme never changes a
running batch's set point without a stored configuration **and** a confirmation for that batch; nothing starts a cooling window for the first time after a reboot
(default CANCEL, opt-in resume needs a valid record + valid RTC + the same slot); High/Emergency/sensor/E115 are never delayed or suppressed (cooling can only remove heat;
low-temperature suppression is limited to the Cooling phase and the approved window). First production trial: both stay OFF.
Wear (28-day batch, 6 cooling cycles a day, 2 writes per cycle): 336 record writes per batch, spread over two slots.
PENDING: allocate the three record kinds (2 slots each) in the real EEPROM map; HMI "confirm programme for this batch" screen; Web read-only view; event codes; migration note
(none needed today: new records only); hardware measurement of egg temperature before any cooling setting is chosen.
