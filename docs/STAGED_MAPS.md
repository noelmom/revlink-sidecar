# Staged maps

A map can be uploaded to the Sidecar while no AccessPort is attached, and
written to the device during a later attach.

This is the normal way a tuner works. The map arrives by email in the pits or
in the driveway; the car is not necessarily there, and the AccessPort is not
necessarily plugged in. Requiring the device to be present at the moment a
file arrives is an artificial constraint.

## What happens

1. **Upload.** `POST /api/portal/maps/stage` accepts a `.ptm` file with no
   device attached. It is streamed to `/sdcard/revlink/system/uploads/
   map.stage`, hashed with SHA-256 as it is written, and committed only after
   `fflush`, `fsync`, and an atomic rename.
2. **Pin.** The payload is pinned to the AccessPort whose dataset is selected
   in the portal. Staging happens with nothing attached, so the selected
   dataset is the only available expression of "which car is this for".
3. **Persist.** A metadata record is written beside the payload recording the
   digest, size, destination, and the pinned part number and serial. Both
   files are removed together; neither is ever left without the other.
4. **Restore.** On boot, the record is decoded, the destination is
   re-validated against the *current* write policy, and the payload is
   re-hashed and compared. Anything that fails is discarded, not carried
   forward.
5. **Apply.** After the next attach completes a clean read-only sync, the
   write is evaluated. If every gate passes, the staged map is written.

## The gates

A staged map is written automatically only when **all** of these hold:

| Gate | Meaning |
| --- | --- |
| `writes_compiled` | `CONFIG_REVLINK_ALLOW_DEVICE_WRITES` is in the image |
| `consent_enabled` | The owner enabled writes in Settings |
| `auto_apply_enabled` | The owner enabled auto-apply separately |
| `staged` | A payload is present |
| pinned | The record names a target part number and serial |
| `device_identified` | The read-only identity handshake completed |
| target match | Part number **and** serial both equal the pinned values |
| `sync_completed_clean` | The sync finished with an acknowledged close |
| `sync_pending == 0` | No continuation batch is outstanding |
| no transfer running | Device transactions stay serialized |
| no recovery required | The transport is not in a recovery state |
| not already attempted | One automatic attempt per physical attach |

These are evaluated by `revlink_staged_map_evaluate_apply()` in
[`firmware/components/revlink_staged_map/`](../firmware/components/revlink_staged_map/),
a platform-neutral component with no I/O, so the whole decision is covered by
host tests that need no hardware.

## Why the pin matters

A map is built for one car. The failure this design exists to prevent is:
stage a map for car A, attach car B's AccessPort, and have the Sidecar write
A's map to B.

So the target is recorded at stage time, persisted with the payload, and
re-checked immediately before the write. Serial is the identity that must
match; part number is checked as well, so a reused or malformed serial cannot
by itself authorize a write. A record with a blank target is refused at decode
— it is never interpreted as "applies to any device", even if its checksum is
valid.

An unpinned payload — staged when no dataset was selected — still works for a
manual apply with the device in front of you. It simply does not persist
across a restart and is never applied automatically.

## Failure behavior

- A failed automatic write is **not retried**. The attempt is latched per
  physical attach; recovery is a deliberate owner action.
- A software re-enumeration after a polite session close does not count as a
  new attach, so one physical attach gets exactly one automatic attempt.
- Metadata that cannot be written causes the payload to be discarded rather
  than left unattributable.
- Power loss during staging leaves at most a `map.tmp`, which is removed at
  startup and is never resumable.

## Enabling it

`CONFIG_REVLINK_MAP_AUTO_APPLY_DEFAULT` sets only the factory default for the
owner preference, and defaults to `n`. Enabling it does nothing unless write
consent is also enabled.

```
POST /api/portal/maps/auto-apply
enabled=true
```

The preference persists in NVS. Portal status reports `autoApply`, `pinned`,
and `targetPartNumber` under `mapUpload`.

> [!IMPORTANT]
> Automatic map writing has not been accepted against real hardware. The
> controlled round-trip in [`MAP_WRITE_ACCEPTANCE.md`](MAP_WRITE_ACCEPTANCE.md)
> must be completed first. Until then this path is implemented and
> host-tested, not proven.
