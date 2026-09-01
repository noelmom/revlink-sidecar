# RevLink firmware architecture

The consumer firmware uses a layered, ports-and-adapters architecture. Board
SDKs and peripherals are implementation details at the edge; product rules
remain ordinary C that can run in host tests.

## Dependency direction

```text
esp32p4/main                         composition root
        |
        v
revlink_application   P4 hardware adapters
        | \ \          |-- accessport_usb (ESP-IDF USB Host)
        |  \ \         |-- P4 SD storage
        |   \ \        |-- P4 USB link control
        |    \ \       |-- P4 soft-power task
        |     \ \      |-- P4 SH1106 status display
        |      \ \     `-- P4 development UART
        v      v v
revlink_core  revlink_sync  accessport_protocol  revlink_power
                      \                 /
                       revlink_status  revlink_cli
```

Dependencies point inward. A core or application component must never include
ESP-IDF, FreeRTOS, USB, SDMMC, Wi-Fi, or board headers. An adapter may depend
on an inward contract and its platform SDK. Board `main/` code only selects
implementations, supplies configuration, and connects event callbacks.

## Current components

| Component | Responsibility | Platform dependency |
| --- | --- | --- |
| `accessport_protocol` | Capture-verified framing, parsing, checksums, and upload target validation | None |
| `accessport_catalog` | Exact-match supported-part catalog and family classification, without transport or feature side effects | None |
| `revlink_core` | Device lifecycle state machine and centralized operation authorization | None |
| `revlink_sync` | Incremental manifests/history plus the runtime sync coordinator and progress model | None |
| `revlink_power` | Debounce, long-press timing, and one-shot shutdown intent | None |
| `revlink_network` | Client-first/fallback-hotspot policy, sticky reconnect, time budgets, and transfer lock | None |
| `revlink_credentials` | Bounded Wi-Fi identity/password validation and explicit zeroization | None |
| `revlink_identity` | Persistent random Sidecar ID, MAC-derived local naming, and bounded collision suffixes | None |
| `revlink_captive_dns` | Bounded DNS question parser and local IPv4 response encoder | None |
| `revlink_time` | Trusted/untrusted wall-clock classification and portable timestamp conversion | None |
| `revlink_update` | Bounded post-boot health decision, blocker reporting, and terminal ready/timeout/safety rejection | None |
| `revlink_usb_control` | Transport-neutral logical/electrical USB-link capabilities and safe control contract | None |
| `revlink_application` | Product-level initialization, protocol self-test, device/sync orchestration, and auto-sync policy | None |
| `revlink_control` | Transport-neutral local control commands and immutable status responses | None |
| `revlink_status` | Deterministic user-facing display state, progress, and copy derived from device/sync snapshots | None |
| `revlink_cli` | Fixed development command grammar and response formatting over the control contract | None |
| `accessport_usb` | ESP-IDF USB enumeration and normalized lifecycle events | ESP-IDF |
| `esp32p4/main/revlink_sd_storage` | P4 board SDMMC mount and accepted card layout | ESP-IDF/P4 |
| `esp32p4/main/revlink_usb_link` | Board-specific logical or physical AccessPort-link isolation | ESP-IDF/board |
| `esp32p4/main/revlink_soft_power` | GPIO polling and safe shutdown sequencing | ESP-IDF/P4 |
| `esp32p4/main/revlink_status_oled` | Optional SH1106 SPI framebuffer and status task | ESP-IDF/P4 |
| `esp32p4/main/revlink_dev_console` | Optional nonblocking programming-UART adapter for `revlink_cli` | ESP-IDF/P4 |
| `esp32p4/main/revlink_wifi_radio` | Bounded onboard ESP32-C6 acceptance adapter and identity-free radio snapshot | ESP-IDF/P4/ESP-Hosted |
| `esp32p4/main/revlink_network_runtime` | P4/C6 composition adapter for coordinator actions, RAM credentials, and transfer events | ESP-IDF/P4/ESP-Hosted |
| `esp32p4/main/revlink_onboarding` | Local HTTP setup, captive DNS/probes, mDNS, and delayed station transition | ESP-IDF/P4 |
| `esp32p4/main/revlink_portal` | Embedded product portal plus bounded status, inventory, cache streaming, notes, sync, cancel, and auto-sync HTTP adapters | ESP-IDF/P4 |
| board `main` | Hardware acceptance and dependency composition | Target board |

ESP32-P4 is the only active consumer-firmware target. Platform-neutral
components live in `firmware/components/`, and their host tests live in
`firmware/test/`. The former ESP32-S3 feasibility target is archived evidence
of the USB full-speed limitation; it is not built, maintained, or considered
when changing P4 contracts.

## Embedded product portal

The ESP32-P4 serves the product UI directly from firmware. It does not proxy
the laboratory application and does not require another computer after the
firmware is flashed. The current portal includes Dashboard, Datalogs, Maps,
Images, Device, Vehicle Health Beta, and Settings views plus a compact
five-item mobile navigation model.

The browser receives immutable cache projections and a deliberately small set
of bounded control/write adapters:

```text
GET  /api/portal/status
GET  /api/portal/devices
GET  /api/portal/files
GET  /api/portal/file?digest=<64-hex-sha256>
POST /api/portal/device/select
POST /api/portal/notes
POST /api/portal/log-map
POST /api/portal/sync
POST /api/portal/sync/cancel
POST /api/portal/auto-sync
POST /api/portal/writes
POST /api/portal/maps/stage
POST /api/portal/maps/apply
GET/POST /api/portal/startup/profiles
GET  /api/portal/startup/profile?id=<bounded-profile-id>
POST /api/portal/startup/apply
```

Map and startup-screen write routes remain inert until the compiled
administrator capability, persistent owner consent, one-device topology,
identity pin, exact target validator, and operation-specific confirmation all
agree. Deletion is not exposed.

`status.attachedAccessPort` is the live, read-only identity of the single
physically attached unit. `status.accessPort` is the independently selected
cached dataset retained for compatibility with the inventory projection. The
portal exposes this distinction as a global **Viewing data from** control:
switching it changes only the offline microSD projection and never redirects a
USB transaction, changes the OLED's connected-device identity, or writes to an
AccessPort.

`GET /api/portal/devices` scans only valid device namespaces whose stored
identity re-derives to the directory key. `POST /api/portal/device/select`
accepts one bounded key, requires the portal request header, and refuses to
switch while USB or synchronization work is active. The selector is responsive
and remains available on every product view, so mobile users do not need to
return to Dashboard to inspect another AccessPort's logs or maps.

The file route never accepts a path or filename. It resolves an exact digest
from the current portal snapshot, derives the object path from the selected
device namespace and known collection, then verifies the object is a regular
file with the manifest size before streaming it through a 4 KiB buffer.
Immutable objects remain valid if a later sync updates the current manifest.
Gzip-compressed datalogs retain their stored bytes and are served with
`Content-Encoding: gzip`, allowing the browser to parse the CSV without a
second decompression implementation in firmware.

The Datalogs and Maps libraries use Initial sync as their primary chronology.
Filename wrap and reuse therefore do not corrupt newest/oldest ordering.
Downloads are direct files rather than ZIP archives, and the selection bar
caps one user action at five files to keep mobile behavior predictable.
Notes remain version-scoped by SHA-256 on microSD.

The embedded log viewer parses quoted CSV locally and never sends vehicle data
off-device. It provides:

- Single and Split views;
- real numeric values and units rather than percentage normalization;
- shared-value and per-channel Y-axis modes;
- graph headroom above and below observed extrema;
- searchable channels and reusable browser-local profiles;
- Air/Fuel, Boost, Knock/Roughness, Timing, and Thermal split presets;
- pointer inspection with exact values;
- direct CSV download; and
- native OS sharing of up to five individual CSV attachments with a prepared
  subject/message and a download-plus-mailto compatibility fallback; and
- print-to-PDF using the browser's native print path.

Vehicle Health, guarded map/startup-screen writes, and native mail handoff are
local portal capabilities. Full in-browser email composition, Sidecar-hosted
delivery, accounts, share links, cloud sync, and AI analysis remain separate
product capabilities. Their absence must not cause the embedded portal to
imply that a remote service is available.

Account entitlements and billing policy do not belong in firmware. If an
optional hosted layer is ever added, a registered Sidecar may consume only a
signed, expiring capability snapshot with a bounded offline grace period. It
must never receive billing-provider identifiers, and local synchronization,
owned-data access, and export must keep working when no network is reachable.

## Stable contracts

The USB adapter reports `revlink_device_event_t`; it does not expose ESP-IDF
descriptor structures to the application. The core converts those events into
the deterministic lifecycle:

```text
stopped -> waiting -> inspecting -> available
                                     |       ^
                                     v       |
                                session-active
```

Detach returns an inspected or active device to `waiting`. Platform failures
enter `faulted`. Invalid transitions are rejected rather than guessed.

Before beta, the lifecycle must add a first-class `conflict` state for two or
more simultaneously live, accepted AccessPorts:

```text
waiting -> inspecting -> available
              |             |
              `-> conflict <-'
```

`conflict` blocks every device operation in both the application authorization
gate and USB adapter. The consumer UI never chooses between devices. The
current P4 adapter enumerates multiple addresses but its request loop still
selects the first accepted candidate, so this is an open safety gate rather
than an accepted capability. Transaction pinning, polite-re-enumeration
handling, recovery semantics, and the test matrix are defined in
[`SINGLE_ACCESSPORT_SAFETY.md`](SINGLE_ACCESSPORT_SAFETY.md).

`revlink_safety_policy_authorize()` is the mandatory operation gate.
Discovery and reads are allowed. Map writes, startup-screen writes, and device
deletes are denied by default. A transport implementation must not bypass this
application-layer decision even if it can physically submit a USB transfer.

## Channel separation

Do not create one oversized "AccessPort manager." Keep these channels
independent behind their own ports:

- file transactions on high-speed `0x03` OUT / `0x82` IN;
- live telemetry research on `0x02` OUT / `0x81` IN;
- local microSD repositories and manifests;
- Wi-Fi/BLE provisioning and connectivity;
- HTTP/API presentation;
- OTA and recovery;
- display/status output.

This prevents an experimental live-data decoder, a new storage backend, or a
different radio from destabilizing proven file synchronization.

### Supported-device classification

The exact supported-part catalog is isolated in `accessport_catalog`. It is
assembled from vendor-published product identifiers and contains 64 part
numbers across 12 protocol families; see
[`ACCESSPORT_CATALOG.md`](ACCESSPORT_CATALOG.md). USB eligibility, authoritative
device identity, reviewed part support, and feature capability remain separate
decisions:

1. `accessport_usb` validates the USB descriptors and reads the device's
   authoritative identity.
2. `accessport_catalog` matches the returned part number exactly.
3. The identity response remains authoritative for the installed vehicle;
   RevLink never guesses a vehicle from the catalog's broad compatibility
   list.
4. Read-only file work proceeds only for an exact reviewed part.
5. An unknown part remains visible for troubleshooting but fails closed before
   namespace selection, listing, or downloading.

The portal publishes the catalog revision, family, and read-only file-sync
capability for both the attached device and selected cached dataset. Catalog
membership does not grant live telemetry, map decoding, firmware work, upload,
or deletion. See [`SUPPORTED_ACCESSPORTS.md`](SUPPORTED_ACCESSPORTS.md).

## Firmware slots and recovery

The 16 MiB P4 flash is divided into an immutable 4 MiB factory recovery image,
two 4 MiB OTA slots, redundant OTA selection data, NVS/PHY state, and a bounded
crash partition. ESP-IDF rollback support is compiled into the bootloader, but
no network update route exists yet. A future update service must write only the
inactive OTA slot and mark a pending image valid only after the bounded product
health gate passes. The partition contract and security gates are documented
in [`FIRMWARE_UPDATE.md`](FIRMWARE_UPDATE.md).

## Peripheral ownership and build profiles

The Waveshare development board exposes one P4 SD/MMC host to both its
onboard ESP32-C6 link and its microSD wiring. Resource ownership is therefore
selected at build time:

```text
default:
    SD/MMC -> microSD (4-bit)
    C6 radio -> disabled

wifi-scan:
    SD/MMC -> ESP32-C6 (4-bit SDIO)
    SPI3   -> microSD
```

The display remains on SPI2 and AccessPort traffic remains on the independent
high-speed USB host in both profiles. The storage port above FAT and the sync
coordinator do not know whether the card uses SDMMC or SPI; switching the
board adapter cannot change namespaces, manifests, immutable-object rules, or
transaction limits.

Managed-component presence is not authority to start hardware. The default
configuration explicitly disables `ESP_WIFI_REMOTE` and `ESP_HOSTED`, and the
radio source compiles its ESP-IDF Wi-Fi path only when the bounded profile is
selected. Both radio-off and radio-on images must build in CI. This prevents a
dependency update from silently claiming the SD/MMC controller.

The radio adapter supports an explicitly requested anonymous station scan, one
station connection, and one RAM-only WPA2 fallback SoftAP. It owns the ESP-IDF
station/AP netifs, hosted-radio mode changes, association events, AP client
count, and generated device SSID. It starts no HTTP server and persists no
credential itself. The independent `revlink_wifi_store` adapter owns the one
preferred station record; the network runtime saves it only after association
succeeds and loads it before the coordinator starts.

The platform-neutral `revlink_network` coordinator owns the product policy
above that adapter. It selects actions for saved-network scanning, one opaque
preferred-network connection, bounded same-network reconnect, and fallback
hotspot startup. While client-ready it schedules bounded local-link probes,
filters transient failures, and requests radio recovery after three
consecutive failures. It never handles an SSID, password, netif, ICMP packet,
or BLE packet.
Automatic background migration is intentionally absent, so a successful
lower-priority connection remains sticky until it is lost. HTTP, BLE, and
physical recovery controls will emit coordinator events rather than switching
the radio directly.

`revlink_network_runtime` is the P4 acceptance composition adapter between
those two layers. It converts coordinator actions into radio calls, hashes a
station identity into an opaque policy ID, ticks bounded timeouts, and reports
radio loss upward. It probes only the DHCP gateway, pauses probes during file
transfers, restarts the hosted radio on sustained failure, and applies a
one-reboot guard if both station and hotspot recovery are exhausted. It owns
only acceptance-time RAM credentials.
`revlink_credentials` centralizes their bounds and zeroization, while
`revlink_onboarding` translates bounded local HTTP requests into the same
runtime operation. Its captive DNS parser remains platform-neutral and does
not own sockets. The future encrypted credential repository remains a
separate port because selecting flash encryption or HMAC/eFuse provisioning
is an irreversible product-security decision. Sync terminal and start events
drive the coordinator's transfer lock so user-requested network mode changes
cannot interrupt an active file operation. The composition root asserts that
lock as soon as sync state becomes `queued`, keeps it asserted through
`running` and `cancelling`, and clears it only on a terminal state. The P4
runtime setter is idempotent so progress callbacks cannot generate redundant
policy events. Live acceptance rejected a mode transition submitted
immediately after queueing while allowing the read-only sync and acknowledged
AccessPort close to finish normally.

## Local control boundary

HTTP, BLE, a future display, and test tooling must not call USB or storage
adapters directly. They translate their input into the same platform-neutral
`revlink_control_request_t` command and receive a
`revlink_control_response_t` containing the current device, sync, policy, and
capability snapshot.

The accepted commands are status, saved auto-sync policy, manual sync, and
cooperative cancellation. Unknown commands fail closed. Mutating commands
also return the resulting snapshot so a UI does not need to guess whether its
request took effect. The ESP32-P4 composition root maps these commands onto
the runtime application; a future HTTP/JSON adapter owns authentication,
serialization, request-size limits, and task serialization.

The optional development UART is the first concrete input adapter for this
boundary. `revlink_cli` accepts only `status`, `sync`, `cancel`, `auto on`,
`auto off`, and `help`; it cannot accept a path, protocol opcode, write, or
delete request. The P4 adapter incrementally reads printable characters from
the programming UART and never blocks the USB or display tasks waiting for a
line. This interface is bench tooling, not a product authentication boundary.

Output-only status surfaces subscribe to immutable lifecycle and sync
snapshots. `revlink_status` converts those snapshots into a small semantic
view—headline, detail, footer, severity, and progress—without knowing which
screen renders it. A monochrome OLED, future color display, LEDs, and an HTTP
status stream can therefore share product behavior without sharing a display
driver. Active sync/cancellation and reconnect safety messages have the
highest priority. After those states, current physical device absence or USB
fault takes priority over a prior terminal sync result so the display cannot
claim a completed backup indefinitely after the AccessPort is removed.

## Idle vehicle behavior and USB isolation

Controlled ESP32-P4 tests on 2026-07-26 proved that USB VBUS, high-speed
enumeration, and a complete descriptor scan do not by themselves put the
AccessPort into PC mode; Gauges remain available. ESP-IDF can then disable the
logical root port and report a clean host disconnect. This descriptor-only
result repeated after a clean P4 reboot.

Follow-up tests on 2026-07-26/27 proved that the session state preceding the
disconnect request is significant. A cold combined lifecycle test that never
established identity left the AccessPort on `connected to your computer`
after interface release and logical root-port shutdown. An isolated test that
sent only the capture-verified mini subtype `0x05` also produced no
subtype `0x35` acknowledgment, software re-enumeration, or return to Gauges.

A complete read-only session passed software-only exit acceptance on
2026-07-27. The P4 completed the identity handshake, listed and reconciled 33
datalogs against the per-device microSD manifest, sent subtype `0x05`, received
the expected subtype `0x35` acknowledgment, and released interface `0` with
`ESP_OK`. The AccessPort performed a software detach/re-enumeration and visibly
returned to Gauges while D+/D- and VBUS remained connected. The P4 classified
the new attachment as a polite software re-enumeration and did not start a
second transaction. The same scheduler subsequently accepted a combined
33-datalog/one-map inventory without changing this lifecycle.

The same lifecycle then passed five consecutive automated cycles. Every cycle
reconciled the same 33-file namespace, produced an acknowledged `0x05`/`0x35`
close, released interface `0` with `ESP_OK`, observed removal, and accepted a
fresh high-speed address before scheduling the next cycle. No cable action,
manual D+/D- switch, logical root-port shutdown, bus reset, or VBUS removal
was used.

Therefore the normal exit path is a polite subtype `0x05` close after a
successfully initialized session, followed by interface release. Logical
root-port shutdown and an unacknowledged or context-free subtype `0x05` remain
insufficient.

Do not automatically issue `SET_CONFIGURATION(0)` or a host-side bus reset
after the acknowledged close. Research proposed those as workarounds for a
missed soft disconnect, but the live P4 trace disproved that premise: ESP-IDF
reported the removal and re-enumeration on every accepted cycle. Preserve the
device-originated removal as lifecycle evidence instead.

A stale read-only PC-mode session was subsequently recovered by rebooting and
reinitializing the P4 host, completing the full bounded handshake, sending
subtype `0x05`, receiving subtype `0x35`, and releasing the interface. The
AccessPort returned to Gauges without opening D+/D- or removing VBUS.

The same recovery was then accepted under deterministic fault injection
without rebooting. Firmware deliberately omitted the first close after a
completed 33-file read-only reconciliation. The application queued one
dedicated recovery (never a second data sync), deferred it until the original
USB work item released the device, attempted the normal initialized identity
probe, and then issued the bounded close. Although the probe timed out against
the stale session, `0x05` received `0x35`; interface release succeeded, and
the AccessPort removed and re-enumerated at a fresh high-speed address.
Recovery success is therefore `acknowledged close + clean release`, while the
original sync retains its independent `data phase complete` evidence.

The retry budget is one per explicit or attachment-triggered sync. It is
eligible only when the original read-only data phase completed but its close
was not acknowledged. Data-phase failures, cancellation, write paths, and a
failed recovery do not recurse.

Software recovery is consequently the required primary path. A
GPIO-controlled reverse-blocking VBUS switch and high-speed data-path switch
with a true disabled state remain optional final-product defense in depth,
pending interrupted-transfer, timeout, repeated-cycle, crash, and write-fault
testing. Do not add unconditional VBUS discharge: an OBD-powered AccessPort
can hold the downstream node at 5 V and RevLink must not sink that external
supply.

### Safe shutdown boundary

The demo firmware reuses GPIO35/`BOOT` after boot as an active-low physical
control. The platform-neutral `revlink_power` component owns debounce and
reports short-release or long-hold intent. The P4 adapter maps a double press
to a context-aware network shortcut: the bounded, volatile Wi-Fi QR in local
hotspot mode, or the bounded `.local` browser address in preferred-network
mode. A single press dismisses either overlay. The URL overlay is unavailable
during active transfer work and is dismissed if sync begins or the preferred
network drops. A two-second hold maps to soft power. The P4 adapter owns the
shutdown side effects:

```text
block new syncs
    -> disable in-memory auto-sync
    -> cooperatively cancel active work
    -> wait for a terminal sync state
    -> isolate the board USB link as far as hardware permits
    -> abort incomplete storage work and unmount microSD
    -> deep sleep
```

If cancellation does not become quiescent within 30 seconds or the card
cannot unmount, firmware remains awake rather than risking an interrupted
transaction. The current Waveshare link adapter advertises no physical data
or VBUS isolation and performs logical root-port shutdown only. A custom-board
adapter may implement physical isolation without changing the power-button,
sync, or storage components.

The read-only file pipeline itself is now proven end to end. On 2026-07-26,
the ESP32-P4 issued one bounded `datalog` listing request, parsed 33 entries,
selected `datalog35.csv`, and streamed one download through the incremental
archive decoder into the atomic microSD cache sink. The sink published only
after exact-size and SHA-256 validation:

```text
46568 bytes
bff4f73f8aef276d354c8277bdc6547433f6f15a164ba41ebe2df1ff736e2688
```

This matches the Linux acceptance baseline byte for byte. An identical
existing cache entry was deduplicated. No retry, upload, delete, live-data,
reset, or disconnect transaction was sent. This validates the replaceable
protocol-decoder, USB-source, and storage-sink boundaries, but it does not
remove the separate requirement for drive-idle electrical isolation.

The next read-only gate promoted that path to bounded incremental sync. A
platform-neutral manifest component now owns file identity and serialization;
the ESP32-P4 SDMMC adapter owns durable publication and cache verification;
and the USB adapter owns one-session scheduling limits. The manifest records
device path, raw device timestamp, size, SHA-256, local cache name, and the
trusted UTC time when that exact version was first synchronized. A candidate
is skipped only after both metadata and the actual cached file verify.
Manifest v2 preserves that first-sync value across reconnects. Parsing remains
backward compatible with manifest v1; migrated entries use zero rather than
inventing a date.

The `revlink_time` component owns the replaceable UTC anchor. The P4
composition may initialize it from network time, a plausible retained RTC, or
the local browser clock when no stronger source exists. HTTP does not write
manifest fields directly: it submits a bounded time observation through the
time-service adapter. When UTC first becomes available, the storage adapter
repairs only zero-valued Initial sync fields for the last unambiguous device
namespace. Nonzero values remain immutable. A supported rechargeable RTC
battery is still required to retain accurate time through a complete
unattended power loss.

Editable user annotations are deliberately stored outside both authoritative
sync documents. `revlink_sync_annotations` keys each note by the cached
content's SHA-256 inside that AccessPort's namespace. A note therefore follows
identical bytes across a rename, does not leak to replacement content when a
native filename rotates, and cannot make a valid sync manifest unreadable.
Notes support multiline UTF-8 text up to 767 bytes. Their update time is zero
while wall-clock time is untrusted and UTC only after the time service has a
trusted source. The platform adapter must publish changes using the same
temporary-file, `fsync`, backup, and atomic-rename pattern as manifests.

The FAT adapter uses `current`, `.tmp`, and `.bak` manifest files because FAT
rename cannot atomically replace an existing destination. Data publication
still precedes manifest advancement, so an interruption can leave a safe
unindexed file but cannot create a manifest entry for incomplete data. On the
physical board, an interrupted run resumed from one valid entry, deduplicated
the already completed next file, and advanced to five entries. A reset then
loaded those five entries, verified and skipped all five, and downloaded the
next four. The two accepted bounded sessions reported `4 downloaded / 1
skipped` and `4 downloaded / 5 skipped`, respectively, and both released the
interface cleanly with writes disabled.

### Multi-device identity and immutable file history

Persistent data is never keyed by the USB descriptor serial. The connected
AccessPort exposes the placeholder
`0123456789.0123456789.0123456789`; the file session therefore sends the four
capture-verified read-only identity mini requests and parses the third
`0x1601` response before storage can be selected. A sync fails closed if the
true serial cannot be obtained or if a namespace's stored identity disagrees.

The storage key is the first 24 lowercase hexadecimal characters of
`SHA-256("revlink-device-v1:" + true_serial)`. Every AccessPort owns a
completely separate directory:

```text
/revlink/devices/<device-key>/
  identity.txt
  annotations.tsv
  current/inventory.manifest
  history/inventory.versions
  objects/datalogs/<full-sha256>.csv[.gz]
  objects/maps/<full-sha256>.ptm
  tmp/
```

The `current` manifest answers “what bytes are currently observed at this
remote path?” while the append-only version journal answers “what bytes have
ever been observed at this path?” Object names are content-addressed by the
full SHA-256, so AccessPort filename rotation cannot overwrite an older log
or map. The remote path—including its `datalog/` or `maps/` collection—is the
authoritative manifest key. Equal filenames in different collections cannot
collide.

### Diagnostic map context

A diagnostic case may optionally reference the map used while its evidence
logs were recorded. The reference is an immutable snapshot containing the
device-scoped map path, full content SHA-256, byte size, initial-sync time, and
available device timestamp metadata. A filename alone is never sufficient:
AccessPort files can be replaced under the same name, and a later replacement
must not silently change an existing case.

The future tuning-summary engine attaches its own versioned result to this
snapshot. It may describe a change only when both the selected map and an
explicit baseline are available through a parser with verified provenance.
Opaque PTM bytes, a filename such as `stage2`, or tuner-supplied timestamps
must not be converted into guessed table or calibration changes. Until those
requirements are met, the UI shows the verified map identity without claiming
a tuning summary.

The commit order is object, version history, then current manifest. A power
loss may leave a safe orphan object or an already-recorded history entry, but
cannot make incomplete bytes current. Both journals use the FAT-safe
`current`/`.tmp`/`.bak` publication pattern and recover an interrupted
publish.

The portal's offline projection is also independent of the Sidecar network
identity. `/revlink/last-device` is a small versioned, FAT-safe pointer to the
last AccessPort namespace selected by an authoritative identity handshake.
At boot, the adapter verifies that namespace's `identity.txt`, re-derives its
key from the stored true serial, then loads the current manifest and notes for
read-only portal display. It immediately releases session selection, so a
later USB operation cannot inherit writable state from the cached projection.

For cards created before this pointer existed, the migration selects a device
only when exactly one valid namespace is present. Multiple namespaces without
a pointer are intentionally ambiguous: the portal waits for a real sync
instead of merging data or guessing which device is current. Changing the
Sidecar hostname, SSID, collision suffix, or internal `r1_...` ID therefore
cannot orphan or re-key AccessPort data.

When one eligible AccessPort is attached, the runtime automatically queues an
identity-only work item after enumeration unless a full auto-sync is already
queued or running. It executes the same four capture-verified read-only
identity mini requests and acknowledged `0x05`/`0x35` close as a full sync,
but does not list or download files. This establishes
`attachedAccessPort` immediately and creates/selects the correct empty
namespace for a previously unseen unit. A polite software re-enumeration
preserves that live identity; a genuine physical detach clears it. The user
may subsequently select a different cached namespace for browsing without
changing the attached identity.

The unified ledger names supersede the original datalog-specific filenames.
On upgrade, the storage adapter atomically adopts the legacy primary manifest
and history files only when their unified destinations do not exist. Live
acceptance after reset loaded all 34 pre-upgrade entries, SHA-verified and
skipped all 33 datalogs plus one map, transferred zero bytes, and left writes
disabled.

This architecture was accepted live on the ESP32-P4 on 2026-07-26. The board
read true serial `SUB0406661`, selected device key
`d2313ecdf618801b8d5b1270`, SHA-verified four existing immutable objects, and
downloaded four more. After reset it loaded eight current/history entries,
verified and skipped all eight, and downloaded the next four. The sessions
reported `4 downloaded / 4 skipped` and `4 downloaded / 8 skipped`; both
released interface 0 with `ESP_OK`, and all write/delete capabilities remained
disabled. The board was then restored to its normal descriptor-only firmware.

### Attachment-scoped session lifecycle

Sync eligibility belongs to an enumerated physical attachment, not to the
firmware boot. A newly accepted USB device record may run one bounded session.
When that session ends, the USB adapter releases interface `0` and tells the
storage adapter to abort any active writer, clear its in-memory manifests and
history, and forget the selected device namespace. A detach destroys the USB
record. A subsequent attach therefore re-reads authoritative identity and may
run another bounded session without rebooting.

This prevents three classes of cross-session error:

- a reconnect cannot be blocked by a boot-global “already synced” flag;
- a replacement AccessPort cannot inherit another device's manifest; and
- a failed or unplugged transfer cannot leave a writable temporary object
  selected for the next session.

The same-device reconnect path passed live on 2026-07-26. The first attachment
loaded 16 entries, downloaded four, and released at 20. After physical
unplug/replug, the unchanged firmware accepted the new USB address, re-read
serial `SUB0406661`, verified and skipped all 20 current objects, downloaded
four more, and released at 24. Both sessions closed with `ESP_OK`; writes and
deletes remained disabled. A host lifecycle test repeats the detach/attach
flow with a different identity to cover device replacement.

### Normal runtime sync coordinator

The accepted file pipeline is now part of the normal application flow rather
than a boot-global acceptance shortcut. `revlink_sync_coordinator` owns the
platform-neutral state machine:

```text
idle -> queued -> running -> completed
                  |   |
                  |   `-> failed
                  `-> cancelling -> cancelled
```

The application requests work through a transport port and receives immutable
progress events back from the USB adapter. Its snapshot contains the current
state, candidates, verified skips, downloads, downloaded bytes, pending files,
and the last platform error. A manual request is accepted only while the
device lifecycle is `available`. Auto-sync runs at most once for a newly
accepted physical attachment; returning from `session-active` to `available`
does not create a loop. A detach resets attachment eligibility.

Auto-sync policy is persisted by the ESP32-P4 composition root in NVS under
namespace `revlink`, key `auto_sync`. It defaults off. NVS stores only this
preference—device manifests and log history remain on microSD. The public
composition-root API is:

```c
revlink_runtime_set_auto_sync(bool enabled);
revlink_runtime_request_sync(void);
revlink_runtime_cancel_sync(void);
revlink_runtime_sync_policy(void);
revlink_runtime_sync_snapshot(void);
```

Cancellation is cooperative and lock-free: the USB owner task accepts the
cancel request through its control queue and publishes an atomic cancellation
flag. The descriptor worker checks it between identity transactions, archive
reads, listing reads, candidates, and download payload reads. It never tears
down a transfer buffer or USB handle from another task. Completion, failure,
cancel, and physical detach all abort any temporary writer, release interface
`0`, and release the selected device namespace.

The normal coordinator path passed live on the ESP32-P4 on 2026-07-26 using a
test-only auto-sync default. It selected true serial `SUB0406661`, loaded 24
current entries, verified and skipped all 24 immutable objects, downloaded
four new objects, and reported:

```text
candidates=33 downloaded=4 skipped=24 bytes=8008571 pending=5
```

The session released interface `0` with `ESP_OK`, returned the device to
`available`, and released namespace `d2313ecdf618801b8d5b1270`. The board was
then restored to the production build and boot-verified with runtime sync
enabled, auto-sync off, every legacy acceptance gate off, and device writes
disabled. The test-only overlay is
`firmware/esp32p4/sdkconfig.runtime-sync.defaults`.

The consumer hardware must therefore expose USB link control as a replaceable
platform adapter. The application-level behavior is:

```text
drive idle
    -> AccessPort powered by vehicle
    -> RevLink VBUS off and data path isolated
    -> on-device Gauges remain available

sync requested
    -> enable protected VBUS/data path
    -> enumerate and open a bounded file session
    -> perform the requested operation
    -> send the polite session-end transaction when applicable
    -> cancel work, release the interface, and stop the host port
    -> isolate data and remove RevLink-supplied VBUS
    -> return to drive idle
```

If a controllable USB power stage is retained, it must prevent backfeeding
when the AccessPort is already powered from its OBD cable. It needs
GPIO-controlled enable, current limiting, fault reporting, controlled
turn-off without an unconditional downstream discharge path, and
reverse-current protection. If a high-speed data switch is retained, it needs
a USB 2.0 high-speed-rated disabled/high-impedance state. These optional
controls are distinct from protocol Connect/Disconnect and from ESP-IDF's
logical root-port state.

The current Waveshare ESP32-P4 development board cannot provide a true
electrical detach by firmware alone. Its USB-A `VBUS_OUT` switch enable
is hardware-pulled on, and its high-speed data route is selected by the
physical OTG jumper rather than a GPIO. A controlled external power/data stage
is no longer required for current prototype acceptance. The ordered TS3USB221
module remains a useful optional test article; its results and the remaining
failure-injection matrix will determine whether physical data isolation
belongs on the production PCB.

### Platform-neutral USB electrical control

`firmware/components/revlink_usb_control` owns the portable electrical-link
state machine. Host tests cover fail-safe initialization, RevLink-supplied
VBUS attach, externally powered data-only attach, data-before-power isolation,
invalid transitions, forced isolation after a driver error, and idempotent
disconnect.

Platform adapters own GPIO numbers, active polarities, delays, and future
fault/sense inputs. The portable component deliberately contains none of
those board details. GPIO39 through GPIO44 are already reserved by the
accepted four-bit microSD interface, so temporary USB-control pins remain
unassigned until the complete development-board pin and boot-strap audit is
finished.

## Vehicle power lifecycle

Use a switched vehicle USB source as the preferred ACC/RUN proxy. A protected
charger and power-path controller must keep RevLink running from its battery
after external USB disappears and expose a 3.3 V-safe power-good signal to an
ESP32-P4 low-power wake GPIO.

External-power loss starts a grace timer rather than cutting the system off.
After active work is complete, firmware flushes storage, stops USB and radio
services, and enters deep sleep. External-power return cancels the timer or
wakes the P4. See [`POWER_MANAGEMENT.md`](POWER_MANAGEMENT.md) for the
deterministic state model and acceptance checks.

If electrical isolation is retained as final-product defense in depth, test
these cases independently:

1. AccessPort on OBD power with RevLink idle: Gauges remain available.
2. RevLink initiates sync: the AccessPort enumerates at high speed.
3. Sync completes: the acknowledged software close returns Gauges before any
   optional hardware isolation is exercised.
4. RevLink reboots or crashes: hardware defaults to the isolated state.
5. OBD and RevLink power overlap: neither supply backfeeds the other.
6. Cable attach detection does not require leaving the AccessPort in PC mode.

## Concurrency and ownership rules

- One component owns each peripheral handle and its worker tasks.
- Callers exchange immutable events, bounded requests, or owned buffers.
- No browser or HTTP handler talks directly to USB.
- The local portal issues transport-neutral `revlink_control` requests and
  reads an immutable storage projection. The SD adapter publishes at most 128
  current-manifest entries, labels each projection as `datalog` or `map`, and
  preserves the last authoritative device namespace after the file session
  closes.
- Portal responses use bounded bodies and close their HTTP connection.
  The P4 composition reserves a 16-socket LWIP pool so the HTTP server,
  captive DNS, mDNS, and network coordinator cannot exhaust one another under
  normal dashboard polling. The 2026-07-27 live pass completed 80
  concurrent/repeated status requests without an accept failure.
- Device transactions are serialized by the USB adapter's bounded work and
  control queues; future transports must preserve this single-session rule.
- Every asynchronous sync request has bounded transfers, progress events, and
  a cooperative cancellation path.
- Disconnect cancels outstanding work before handles or buffers are released.
- Large files are streamed through bounded buffers; they are not loaded whole
  into RAM.
- Persistent writes use temporary files, flush, checksum verification, and
  atomic rename.
- A component reports errors upward; it does not restart unrelated systems.

## Adding or replacing a subsystem

1. Define or extend a platform-neutral port under `revlink_core` or the
   application layer.
2. Write host tests for behavior and failure transitions.
3. Implement the platform adapter in its own component.
4. Select the adapter only in the board composition root.
5. Add a hardware acceptance gate that is disabled by default.
6. Preserve the previous adapter until parity and recovery behavior pass.

For example, replacing microSD with eMMC should require a new storage adapter,
not changes to device sync, the HTTP API, or AccessPort protocol framing.

## Test gates

Host tests must pass before a board build:

```sh
source "$HOME/.espressif/frameworks/esp-idf-v6.0.2/export.sh"
cmake -S firmware/test -B firmware/test/build
cmake --build firmware/test/build --parallel
ctest --test-dir firmware/test/build --output-on-failure
```

These cover protocol vectors, all 97 captured request parity checks, lifecycle
transitions, strict root-list response parsing, incremental fragmented
download decoding, both capture-valid JAMCRC and device-observed zero
trailers, corrupt-trailer rejection, sink-failure propagation, and
default-deny write/delete policy. Then build the active full-product P4
profile:

```sh
idf.py -C firmware/esp32p4 \
  -B firmware/esp32p4/build-onboarding \
  -D "SDKCONFIG=$PWD/firmware/esp32p4/build-onboarding/sdkconfig" \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.oled.defaults;sdkconfig.wifi-scan.defaults;sdkconfig.wifi-join.defaults;sdkconfig.network-runtime.defaults;sdkconfig.onboarding.defaults' \
  build
```

Hardware acceptance remains staged. Enabling an interface, read, write,
delete, format, or recovery gate is a deliberate test decision; successful
compilation never grants that authority.
