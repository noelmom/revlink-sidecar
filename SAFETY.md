# Safety model

This project talks to a device that holds the maps your car runs on. The
constraints below are not stylistic — they are the reason this is safe to
plug into a vehicle. Please do not relax them casually in a pull request.

## The one-sentence version

Reading is safe and enabled. Writing works and has been done against real
hardware, but it is off by default, requires two independent unlocks, is
restricted to two exact destinations, and refuses to proceed the moment
anything is ambiguous. What is genuinely new is RevLink starting a write
without being asked; see automatic application below.

## Read vs. write

| Operation | Status | Notes |
| --- | --- | --- |
| List directories | Enabled | Read-only |
| Download files | Enabled | Verified by size and SHA-256, published atomically |
| Upload a map file | **Gated** | Compile flag + persistent owner consent. Copies a `.ptm` onto the AccessPort's storage; it does not install anything onto an ECU |
| Auto-apply a staged map | **Gated** | The above, plus a separate preference and a matching pinned device — see [STAGED_MAPS.md](docs/STAGED_MAPS.md) |
| Replace the startup screen | **Gated** | Fixed destination only |
| Delete a file from the AccessPort | **Gated** | `maps/` and `datalog/` only, one level deep. Its own compile flag and its own consent, neither implied by the write gates |
| Remove the Sidecar's cached copy | Confirmed per action | The owner's own microSD. No device involved, so no AccessPort consent — but it is the one action that can leave a file nowhere, so the portal says so and offers the download first |
| ECU flashing / live tuning | **Out of scope permanently** | Not a goal of this project |

## What a "write" is here

A write copies a file onto the AccessPort's own storage. It does not put a
calibration into a vehicle. Installing a map onto the ECU is a separate,
deliberate action the owner takes on the AccessPort itself, and nothing in
this project can perform it or trigger it.

That is why the published image ships with writes compiled in: the failure
this project can plausibly cause is a damaged file or a confused `maps/`
directory on the AccessPort, not a damaged engine.

It stays gated anyway, and the reason is not doubt about the code. The write
path has been accepted on hardware and is in regular use; the gate is there
because writing to someone's device is their decision to make, not a default
to be assumed. Consent starts locked on every boot for that reason alone.

## Deletion is gated separately

Deletion has its own compile flag (`CONFIG_REVLINK_ALLOW_DEVICE_DELETES`,
default `n`) and its own runtime consent. Neither is implied by the write
gates.

That separation is the point. Agreeing to copy maps onto a device is not
agreeing to let this remove files from it, and a shared flag would convert one
decision into the other. Deletion also has no undo: the AccessPort keeps no
recycle bin, and the Sidecar holds nothing back beyond whatever it had already
synchronised.

Only a file directly inside `maps/` or `datalog/` can be removed — not
`images/`, not the directories themselves, nothing nested, no traversal. Every
delete pins the device by part number and serial, requires the file to be
present first, and re-lists the directory afterwards to confirm it is gone. A
delete is never transmitted for a file the device does not list. Each one is
appended to an audit log on the card.

From 0.2.2 the published image contains it. Compiling it in is not enabling
it: delete consent is a separate runtime switch that starts locked on every
boot until the owner sets it, and is never implied by agreeing to map
transfers. The reason to ship it is the same reason writes ship — without a
computer, a full AccessPort cannot be cleared, and that is one of the two
problems this project exists to solve. What deletion can cost you is a file
you meant to keep; the Sidecar's own synced copy of it is kept either way.

## The two write gates

A map write requires **both**:

1. `CONFIG_REVLINK_ALLOW_DEVICE_WRITES` compiled into the image. Without it,
   the write service returns `ESP_ERR_NOT_SUPPORTED` and no write code path
   exists at runtime.
2. **Persistent owner consent** enabled in portal Settings. New and
   factory-reset Sidecars default to locked.

Neither gate implies the other. Compiling writes in does not enable them.

## Destination allow-list

The generic protocol serializer can express more than the product permits.
That is deliberate — it lets the serializer be checked against every captured
request. The *product policy* is narrower and authorizes exactly:

- `maps/<name>.ptm` — one directory level, no traversal, no subdirectories
- `images/startup_screen.fb` — the exact startup framebuffer, fixed size

Everything else is refused, including captured CSV upload paths.

## Fail closed, never guess

- **Two attached AccessPorts produce a first-class conflict state**, not an
  arbitrary selection. See
  [`docs/SINGLE_ACCESSPORT_SAFETY.md`](docs/SINGLE_ACCESSPORT_SAFETY.md).
- **Stale USB topology events are rejected** by a transport-owned monotonic
  revision, so a late detach cannot invalidate a newer attach.
- **Device operations are serialized.** No concurrent transactions.
- **Checksum mismatches are hard failures.** Never a warning.
- **Writes are never retried automatically.** A failed write stops and
  requires a deliberate human action.
- **Downloads publish atomically** — temp file, flush, checksum, rename — so
  power loss cannot turn a partial download into a valid cache entry.

## Network posture

The portal has **no transport encryption**. It is designed for a trusted local
network: your own Wi-Fi, or the Sidecar's own fallback hotspot.

Do not port-forward it. Do not expose it to the public internet. If you want
remote access, put it behind a VPN or a private overlay network.

## What has actually been proven on hardware

Claims here are limited to what has been verified on a real device.

**Accepted:**

- USB enumeration of the AccessPort at high speed with correct endpoints
- Root, `datalog/`, and `maps/` listings, repeatedly
- Incremental download with exact size and SHA-256 match against a known
  baseline, published atomically to microSD
- Byte-exact protocol reconstruction of the full captured request corpus
- Wi-Fi client rejoin, fallback hotspot, and captive portal across cold boots
- Two-second BOOT-button safe shutdown with an idle attached AccessPort
- A complete logical backup export downloaded end to end, 128 files, with the
  device staying up throughout
- A map staged with no AccessPort attached, surviving a power cycle: restored
  at boot with its recorded size and SHA-256 re-verified, and still pinned to
  the part number it was saved for
- A staged map applied automatically, with nobody pressing anything: written
  after a clean sync to the AccessPort it was pinned to, and verified by
  read-back (`MAP WRITE VERIFIED ... ready/completion/readback=passed`)
- Deleting a file from an AccessPort: refused while consent was locked, refused
  for every path outside the allowlist, then removed and confirmed absent by
  re-listing (`FILE DELETED ... confirmed_absent=yes`). A second attempt at the
  same path was refused without transmitting anything
- Removing the Sidecar's own cached copy, and removing a file from both places
  in one action — the case that leaves no copy anywhere. Accepted on 0.2.5,
  after two releases in which the button was drawn, the request was sent, and
  the firmware refused it with no visible symptom beyond the file staying
  where it was
- Writing maps to an AccessPort, repeatedly and in ordinary use — not a single
  lab pass. The maintainer has transferred several different maps to a device
  in service, each read back and matched by SHA-256 before being reported as
  done, and the files survive power cycles and are usable from the AccessPort
  afterwards. The first live pass reported a misleading `ESP_ERR_INVALID_CRC`
  because the local cache's path validator rejected spaces in map names; the
  transfer itself had already completed. Validators now accept safe spaces and
  parentheses, with host tests covering those names.
  See docs/MAP_WRITE_ACCEPTANCE.md

**Not yet accepted on hardware:**

- The scripted round-trip in docs/MAP_WRITE_ACCEPTANCE.md as one recorded
  ceremony. Every step in it has been done — writing, read-back verification,
  listing, deletion — but on separate occasions rather than as one transcript
- Startup-screen replacement against a device
- Powered-hub behavior with two AccessPorts attached
- Cooperative cancellation and shutdown *during* an active sync

Do not describe the unaccepted items as working, in documentation or in a
release. If you accept one on real hardware, move it up and say what you
verified.

## Reporting a security or safety issue

Please open an issue for ordinary bugs. For anything that could damage a
device or a vehicle, or that could cause a write to reach the wrong target,
please report it privately through GitHub's security advisory flow on this
repository rather than in a public issue.

## Logical backup export takes minutes

`GET /api/portal/backup` reads and checksums every cached file, then streams
them. On a card holding a few hundred megabytes of datalogs that is several
minutes — 128 files took about two and a half on the reference Nano.

The embedded HTTP server handles one request at a time, so nothing else is
served while that runs. Leave the tab open. The portal stops polling for the
duration and does not treat a timeout during a known-busy period as a lost
connection; before that it declared the Sidecar offline about fifteen seconds
in, while the backup was streaming perfectly well underneath.

Verified on hardware: a complete export downloads successfully, and the
device stays up through it. Heap holds flat at 33 MiB and the HTTP task keeps
about 6 KiB of stack spare for the whole run.

## Fixed in 0.2.2: restore path's 4 KiB stack buffers

`validate_pending()`, `skip_bytes()` and `revlink_backup_restore_merge()` each
held a `BACKUP_BUFFER_BYTES` array on the stack, on the same HTTP server task
whose export equivalents had to be moved to the heap. The worst case was
8 KiB, not 4: `restore_merge()` calls `skip_bytes()` from inside its loop with
its own buffer still live, against a 12 KiB stack that had itself been raised
from 6 KiB after a measured 6116-byte peak overflowed it.

Restore still has not been exercised on hardware, so this was never observed
to fail — it was the same arithmetic that had already overflowed once
elsewhere. The buffers are now on the heap, and `skip_bytes()` borrows its
caller's rather than allocating a second one.

The trap, recorded because it is easy to walk into again: those loops sized
their reads with `sizeof(buffer)`, which silently becomes the size of a
pointer the moment the array becomes a `malloc` — a four-byte read loop that
passes every test. They name the constant now, and the file has no
`sizeof(buffer)` left in it.

## The only action that can leave no copy

Every destructive operation here normally leaves a copy somewhere. Delete from
the AccessPort and the Sidecar still has it. Remove it from the Sidecar and the
AccessPort still has it. Removing both is the single exception, and the Sidecar
has no recycle bin any more than the AccessPort does.

So it is asked as its own question, after the first one, and never bundled into
a prompt about deleting from the device. The confirmation says the file will
not exist anywhere afterwards and offers to download it first. Declining
anywhere in the chain keeps the cached copy, because that is the outcome you
can still recover from.

Removing the cache alone deliberately does *not* require the AccessPort delete
consent. That switch exists to protect somebody's device; the microSD is the
owner's own storage, which they can already empty by removing the card.
Granting one permission on the strength of another is the thing the write and
delete gates are kept separate to avoid, and it would apply just as much here.

## Presence is three-valued, and absence needs evidence

The Sidecar keeps its own copy of everything it syncs, so a file can exist
here and not on the AccessPort. Since 0.2.2 each cached file records whether
the last *completed* listing found it on the device, and the portal says
"Sidecar only" for one that is gone.

The state is deliberately three-valued. A sync that was cancelled, failed, or
was cut short proves nothing about what is on the device, and a manifest
written by an earlier build carries no evidence at all; both read as unknown
rather than as absence. The API reports `null` for that case, and `null` is
not `false`. Telling an owner their files had been removed because a sync was
interrupted would be a worse failure than saying nothing.
