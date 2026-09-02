# Deleting a file from an AccessPort

RevLink can remove a file from the AccessPort's own storage. Only files
directly inside `maps/` and `datalog/`, and only when two separate gates are
both open.

**From 0.2.2 the published image contains this.**
`CONFIG_REVLINK_ALLOW_DEVICE_DELETES` still defaults to `n` for anyone building
their own, and the Nano product profile turns it on. Compiling it in is not
enabling it: consent is a runtime switch that starts locked on every boot.

## Why it is gated apart from writes

Agreeing to copy maps onto a device is not agreeing to let something remove
files from it. A shared consent flag would convert one decision into the
other, silently, so deletion has its own compile flag and its own runtime
consent and neither is implied by the write gates.

Deletion also has no undo. The AccessPort keeps no recycle bin, and the
Sidecar holds nothing back beyond whatever it had already synchronised.

## What can be deleted

| Path | Result |
| --- | --- |
| `maps/<file>` | permitted |
| `datalog/<file>` | permitted |
| `images/startup_screen.fb` | refused — writable, never deletable |
| `maps/` or `datalog/` | refused — that names a directory |
| `maps/sub/<file>` | refused — one level only |
| `maps/../images/<file>` | refused — traversal |
| `mapsx/<file>` | refused — a directory that merely starts with an allowed name |

`revlink_ap_validate_delete_target()` enforces this, and
`firmware/test/delete_target_host_test.c` covers every row above. Widening the
allowlist should be loud.

## What happens on the wire

1. Claim the interface and re-read identity from the device. Both part number
   and serial must equal what the owner authorised — the dataset open in a
   browser is not evidence about what is plugged in now.
2. List the directory. **The file must be present.** A delete aimed at
   something already absent means the caller's view is stale, and acting on a
   stale view is how the wrong file disappears.
3. Send `0x1625` and require the class-0x01 mini acknowledgement carrying the
   ASCII payload `"15"`.
4. List again and confirm the entry is gone. An outcome that cannot be seen in
   a listing is not reported as success.
5. Release, with the same polite disconnect a write uses.

A delete that fails after the request went out takes the same recovery lock a
failed write does, because the device is then in a state this firmware cannot
describe.

Every outcome is appended to
`/sdcard/revlink/system/acceptance/file-delete-audit.log` before it is
reported anywhere else. A delete leaves no other trace.

## The cached copy stays

The Sidecar keeps its own copy of everything it has synchronised, and a delete
does not touch that. The file is gone from the AccessPort; the row remains in
the portal, because the Sidecar still has it. That is deliberate — local
copies surviving the device is the point of the product — but it means a row
can outlive the file it came from, and a second delete of the same path is
refused without transmitting anything.

Since 0.2.2 the row says so. Each cached file records whether the last
completed listing found it on the device, and one that is gone is badged
**Sidecar only** and stops offering a Delete button. The flag is three-valued:
a sync that was cancelled or cut short, and a cache written by an earlier
build, both read as *unknown* rather than as absence — the portal will not
claim a file has been removed on the strength of a listing it never finished.

## Accepted on hardware

On an ESP32-P4-NANO with an AP3-SUB-004 attached: refused while consent was
locked, refused for every path outside the allowlist, then

```
Deleting 'maps/UI Flow Test.ptm' from AccessPort part=AP3-SUB-004
FILE DELETED: part=AP3-SUB-004 path=maps/UI Flow Test.ptm confirmed_absent=yes
```

and a second attempt at the same path refused with `ESP_ERR_NOT_FOUND` and
`sent=no`.
