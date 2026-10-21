-- "Che do 2 tai khoan": only the machine (HMI, Nang cao) switches it. While it is on, ONE more account may claim the machine with Device ID + PIN;
-- switching it off removes that account. join_until bounds the time the second slot stays open when nobody has joined.
CREATE TABLE device_guest_mode (
  device_id TEXT PRIMARY KEY REFERENCES devices(device_id),
  enabled INTEGER NOT NULL DEFAULT 0,
  join_until INTEGER NOT NULL DEFAULT 0,
  updated_at INTEGER NOT NULL
);
