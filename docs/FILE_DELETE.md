# Deleting a file from an AccessPort

RevLink can remove a file from the AccessPort's own storage. Only files
directly inside `maps/` and `datalog/`, and only when two separate gates are
both open.

**From 0.2.2 the published image contains this.**
`CONFIG_REVLINK_ALLOW_DEVICE_DELETES` still defaults to `n` for anyone building
their own, and the Nano product profile turns it on. Compiling it in is not
enabling it: consent is a runtime switch, locked until the owner sets it. It
is stored in NVS and stays set until it is turned off; it does not re-lock on
reboot.

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

## Removing the Sidecar's own copy

Since 0.2.3 the cached copy can be removed too, as a separate choice:

| Scope | What goes | What is left |
| --- | --- | --- |
| `device` (default) | The file on the AccessPort | The Sidecar's cached copy |
| `sidecar` | The Sidecar's cached copy | Whatever the AccessPort has |
| `both` | Both | Nothing |

The portal asks with one **Delete** button per row that opens a dialog naming
each place and what survives it. A chain of yes/no prompts was tried first and
was worse: it made the safe outcome depend on answering "cancel" to the right
question, and it needed two red buttons in every row, which wrapped onto three
lines on a phone and still left "Delete" and "Remove" to be told apart by
guessing.

Only the choices that destroy the last copy are styled as grave — `both`
always, and `sidecar` when the last completed listing did not find the file on
the device. Deleting from the AccessPort while the Sidecar keeps a copy is not
grave, and dressing it in the same red would train people to ignore the red.

`both` is the only operation in the product that can leave a file existing
nowhere, so the dialog says that in those words and offers to download it
first.

Order matters for `both`: the device delete goes first and the cached copy is
dropped only once the device has confirmed. If the cache went first and the
device delete then failed, the owner would have lost their only spare copy of
a file that is still sitting on the AccessPort.

Clearing the cache alone never speaks to a device, so it does not need one
attached — which is the point, since a full card is exactly the situation
where you cannot conveniently go and find a computer. For the same reason it
does not require the AccessPort delete consent: that switch exists to protect
somebody's device, and the microSD is the owner's own storage, which they can
already empty by taking the card out. Granting one permission on the strength
of another is the thing the write and delete gates are kept apart to avoid.

The cache is content-addressed, so two catalogued paths holding identical
bytes share one object on the card. The manifest is the reference count: the
object is unlinked only when the last entry naming that digest has gone, and
the catalogue is saved before the payload is unlinked. Losing power between
the two leaves an object nothing points at — wasted space the next write of
the same content reuses — rather than a catalogue row pointing at a file the
portal would then offer for download and fail to produce.

Notes and map tags are version-scoped by digest, and go with the last entry
that carried it. A datalog tagged with a map that has since been removed keeps
its tag and the portal renders it as *previously tagged map (not in this
cache)*, rather than dangling or silently losing the association.

## The cached copy stays by default

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

Removing the Sidecar's own copy, and removing a file from both places at once,
were accepted on 0.2.5 — including the case that leaves no copy anywhere.

Getting there took three releases, and the first two are worth recording
because each looked identical from the outside: the row simply did not change.

1. **0.2.2** offered no button at all for a file the last listing had not found
   on the device, which was the intended behaviour for an AccessPort delete and
   left nothing to remove the cached copy with.
2. **0.2.4** fixed a render race — rows decide whether to offer Delete from the
   status, but the files usually arrive first, so rows were built while
   deletion looked unavailable and were never rebuilt.
3. **0.2.5** fixed the actual refusal. Both cache operations guarded on
   `storage_device.selected`, which is cleared by `revlink_sd_release_device()`
   at the end of every sync. The portal keeps showing files because the
   published projection survives that, so for most of the time the portal is in
   use there are rows on screen and no manifest behind them. Every call was
   rejected, on exactly the files being displayed.

The common thread is that a refused request and a successful no-op look the
same in a list that does not change. The endpoint's error text now reaches the
portal's toast verbatim, so a refusal names itself.
