# Transaction V2 application contract

## Scope

Transaction V2 defines the transport-independent command, configuration,
history, and event semantics shared by the Web application and ESP32. Phase 1
does not attach a network publisher. A later transport may carry these messages
without changing the state machine described here.

PID, heater control, thermal safety, turning, alarms, HMI, RTC, EEPROM, ATtiny,
batch recovery, and service recovery remain owned by their existing modules.
The transaction bridge only exchanges bounded mailboxes with MachineController
and the HMI.

## Identity and validation

- `requestId` identifies one application transaction and stays stable across a
  retry of the same signed envelope.
- `bootId` fences results from an earlier device boot.
- Sequence and expiry checks reject stale, reordered, or expired writes.
- HMAC covers the application channel, body, grant identity, result fields,
  and request identity. A transport acknowledgement never counts as an
  application result.
- Replay windows and the terminal result cache prevent a duplicate request from
  executing twice. A duplicate completed request receives its cached result.

## Command and configuration handoff

The bridge validates and queues a bounded request. MachineController consumes
that request through the existing mailbox and reports completion through
`mayapRealtimeConfirmCommand` or `mayapRealtimeConfirmConfigSave`. Configuration
changes still pass through sanitize, invariant validation, EEPROM save, and
readback before they can be reported as applied.

The application result phases are:

- `received`: the request was authenticated and accepted into the bounded
  application queue. It is not success.
- `applied`: MachineController completed the operation successfully. This is a
  terminal success.
- `rejected`: validation or MachineController rejected the operation with a
  stable reason. This is terminal.
- `uncertain`: the completion deadline expired without a trustworthy terminal
  result. UI state may be refreshed for reconciliation, but must not infer
  success from a later snapshot.

Configuration patches preserve revision ordering. A newer signed rejection
cannot be hidden by a configuration snapshot. Terminal ACKs are signed and bind
the operation, request identity, result, reason, revision, and completion data.

## History and event data

History requests use a bounded request mailbox and chunked responses within the
shared packet limits. A terminal history result is valid only after all expected
chunks are present. The event-log mailbox copies bounded HMI snapshots without
performing network I/O in a control-task critical section.

## Timing and bounds

`realtime_publish_policy.h` owns only generic bootstrap and snapshot cadence.
Command/config completion deadlines and packet limits remain explicit constants
covered by host tests. The bridge uses short critical sections for struct copies
and never holds its mutex across I/O.

## Required regression coverage

CI covers HMAC validation, request/operation mismatch, replay, duplicate
terminal results, reboot fences, late terminal ACK after `uncertain`, missing or
out-of-order history chunks, configuration revision reconciliation, packet
budgets, mailbox handoff, and publish cadence. Host tests do not replace
physical ESP32, EEPROM fault, or live-network validation.
