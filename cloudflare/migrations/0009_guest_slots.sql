-- "Che do tai khoan" (firmware 3.1.2): the machine (HMI, Nang cao) lets 0, 1 or 2 more accounts claim it (1, 2 or 3 accounts in all).
-- slots = how many MORE accounts may join while the mode is on; the old firmware never sends it and keeps the one slot it always had.
ALTER TABLE device_guest_mode ADD COLUMN slots INTEGER NOT NULL DEFAULT 1;
