# Safety model

This project talks to a device that holds the maps your car runs on. The
constraints below are not stylistic — they are the reason this is safe to
plug into a vehicle. Please do not relax them casually in a pull request.

## The one-sentence version

Reading is safe and enabled. Writing is off by default, requires two
independent unlocks, is restricted to two exact destinations, and refuses to
proceed the moment anything is ambiguous.

## Read vs. write

| Operation | Status | Notes |
| --- | --- | --- |
| List directories | Enabled | Read-only |
| Download files | Enabled | Verified by size and SHA-256, published atomically |
| Upload a map file | **Gated** | Compile flag + persistent owner consent. Copies a `.ptm` onto the AccessPort's storage; it does not install anything onto an ECU |
| Auto-apply a staged map | **Gated** | The above, plus a separate preference and a matching pinned device — see [STAGED_MAPS.md](docs/STAGED_MAPS.md) |
| Replace the startup screen | **Gated** | Fixed destination only |
| Delete | **Not implemented as a product feature** | Destructive; not part of the tuner workflow |
| ECU flashing / live tuning | **Out of scope permanently** | Not a goal of this project |

## What a "write" is here

A write copies a file onto the AccessPort's own storage. It does not put a
calibration into a vehicle. Installing a map onto the ECU is a separate,
deliberate action the owner takes on the AccessPort itself, and nothing in
this project can perform it or trigger it.

That is why the published image ships with writes compiled in: the failure
this project can plausibly cause is a damaged file or a confused `maps/`
directory on the AccessPort, not a damaged engine. It is still gated, because
the write path has not completed its hardware round-trip.

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

**Not yet accepted on hardware:**

- Live map upload round-trip against a device
- Automatic application of a staged map on attach
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
served while that runs. The portal used to poll `/api/portal/status` every
2.5 seconds throughout and declare the Sidecar offline after three timeouts,
about fifteen seconds in, while the backup was streaming perfectly well
underneath. The portal now stops polling for the duration and does not treat
a timeout during a known-busy period as a lost connection.

Leave the tab open. The device stays healthy throughout: heap and stack are
flat across the whole run, and the only thing that ends it early is the
browser hanging up (`httpd_sock_err: error in recv : 104`).
