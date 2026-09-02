# Operational logging, retention, and support export

RevLink operational evidence exists to diagnose Sidecar behavior without
turning customer files, credentials, or vehicle data into an unbounded log.
This policy applies to the ESP32-P4 product firmware.

## Data classes

Keep these classes separate:

| Class | Examples | Default retention |
| --- | --- | --- |
| Volatile runtime diagnostics | component, severity, error code, bounded state transition | current boot only |
| Durable operational events | boot reason, firmware version, sync result counts, rollback result | bounded rotating history |
| Customer-owned data | datalogs, maps, screenshots, startup images, notes | dataset policy; never an operational log |
| Secrets | Wi-Fi passwords, session credentials, tokens, private keys | never logged |

Manifests and immutable object history are authoritative customer-data
metadata. They are not a substitute for operational logs, and operational logs
must not duplicate their file contents.

## ESP32-P4 policy

Normal `ESP_LOG*` output is volatile development evidence. Production builds
must suppress debug/verbose output and must never print:

- Wi-Fi passwords, HTTP authorization values, tokens, or cryptographic keys;
- datalog/map/image contents;
- complete request or response payloads from the AccessPort protocol;
- notes or diagnostic symptom text;
- full hardware MAC addresses or the internal `r1_...` identifier in routine
  logs; or
- a customer's complete file inventory.

When durable operational events are implemented, store them in a separate
versioned, checksummed, rotating file under
`/sdcard/revlink/system/events/`. Use at most four 256 KiB segments and rotate
oldest first. Each record should contain only:

- schema version and monotonic sequence;
- UTC timestamp plus time-source/quality when trusted time exists;
- boot/session-local monotonic milliseconds;
- firmware version and reset reason;
- stable event code and severity;
- bounded numeric counters or public state names; and
- a redacted correlation ID that is random per support export.

Do not place durable logs in NVS or internal flash. This avoids flash wear and
keeps removable customer data under an explicit retention boundary. Logging
failure must not block local sync, browsing, safe close, or shutdown.

The reserved coredump partition remains disabled until the same redaction,
bounded retention, export, and explicit erase rules are implemented.

## Support bundle

A support bundle is customer-initiated and local-only by default. Creating one
must require an explicit UI action and show exactly what will be included.
The initial format should be a ZIP containing:

- a human-readable summary with firmware/build, reset reason, network mode,
  storage health, and USB endpoint contract;
- the bounded durable operational event segments;
- current configuration values with secrets omitted;
- manifest counts and integrity status, not customer filenames or file
  contents; and
- an inclusion manifest with SHA-256 for every exported file.

Exclude datalogs, maps, images, notes, credentials, MAC addresses, the internal
Sidecar ID, AccessPort serial numbers, IP addresses, and SSIDs by default.
Customer-owned files require a separate, explicit selection and consent step.
Never upload a bundle automatically. Future cloud support upload must disclose
destination and retention before consent and must use authenticated encrypted
transport.

## Erase and recovery

- Rotating operational events can be erased independently of customer data.
- Factory reset must state whether operational evidence is erased; it must
  never silently erase the microSD dataset.
- A corrupt event segment is quarantined or skipped. It must not trigger
  formatting, manifest repair, or deletion of customer files.
- Removing or replacing the microSD card simply makes durable operational
  history unavailable. The Sidecar continues with volatile logging and a clear
  storage-health state.

## Audit logs that ship today

Two exceptions to "operational logging is volatile" already exist. Every
device write and every device delete is appended to the card *before* the
outcome is reported anywhere else, because an irreversible action on somebody
else's hardware with no record is worse than a log file:

```text
/sdcard/revlink/system/acceptance/map-write-audit.log
/sdcard/revlink/system/acceptance/file-delete-audit.log
```

One line per operation: UTC timestamp, AccessPort part number and serial, the
path, the outcome, the platform error code, and whether recovery is required.

Three properties worth knowing before you rely on them:

- **They are not rotated.** They grow without bound. In normal use that is a
  few bytes per deliberate write or delete, but nothing trims them.
- **They are not in a logical backup.** The backup export walks
  `/sdcard/revlink/devices`; these live under `/sdcard/revlink/system`, so
  they do not travel with a card backup and are lost with the card.
- **They contain device identifiers** — part number and serial — which is the
  point, and which is also why they are the one thing on the card to redact
  before sharing a log with anyone.

## Acceptance before durable logging ships

Host and hardware tests must prove:

1. rotation never exceeds the configured byte/segment limits;
2. power loss during append preserves previous valid records;
3. malformed segments do not affect customer manifests;
4. redaction fixtures contain no known secret, identifier, filename, note, or
   payload sample;
5. export inclusion manifests match bundle bytes;
6. erase removes only operational evidence; and
7. a full or absent microSD does not prevent safe device close or shutdown.

Until these tests pass, keep production operational logging volatile and use
the existing manifests as the only persistent sync evidence.
