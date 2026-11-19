-- Backend after firmware 3.1.2: one created_at index replaces the expires_at one (expires_at is always created_at + 1 hour, so the retention and expiry queries
-- both use it; the number of indexes on alarm_events does not grow, an extra index would be an extra row written per event), and the incident
-- columns of alarm_state: a RESOLVED push waits resolve_due (an off-delay, ISA-18.2 style) and a re-activation inside that time cancels it
-- (held_seq = the event whose deliveries are held) instead of sending a second ACTIVE and RESOLVED pair.
CREATE INDEX IF NOT EXISTS alarm_events_created ON alarm_events(created_at);
DROP INDEX IF EXISTS alarm_events_expiry;
ALTER TABLE alarm_state ADD COLUMN resolve_due INTEGER NOT NULL DEFAULT 0;
ALTER TABLE alarm_state ADD COLUMN held_seq INTEGER NOT NULL DEFAULT 0;
