# Map write acceptance

Map upload is a core RevLink workflow:

1. send datalogs to a tuner;
2. receive a `.ptm` map;
3. upload it to the connected AccessPort;
4. prove that the device stored the exact bytes; and
5. refresh the map inventory so the new map is immediately visible.

An upload acknowledgement alone is not success. RevLink reports success only
after downloading the destination and matching its byte count and SHA-256 to
the local file.

## Product write scope

RevLink intentionally permits only two file-write categories:

- tuner map files: one `.ptm` file directly inside `maps/`; and
- the fixed startup framebuffer: `images/startup_screen.fb`, exactly 153,600
  bytes.

CSV uploads seen in the capture corpus prove that the generic `0x1622`
transaction is understood, but RevLink has no product workflow for writing
datalog CSV files back to the AccessPort. CSV and every other destination are
rejected before transport. Startup-screen validation and conversion are
covered in [`FIRMWARE_ARCHITECTURE.md`](FIRMWARE_ARCHITECTURE.md).

## Current proof

The Windows proof established the complete protocol flow:

- send the `0x1622` upload request;
- require a valid class-`0x01`, subtype-`0x07` ready acknowledgement;
- send one checksum-protected upload chunk as sequential 512-byte USB writes;
- require a valid class-`0x01`, subtype-`0x23` completion acknowledgement;
- list `maps/`;
- download the uploaded destination; and
- compare SHA-256.

The established test artifact is:

```text
source: Stage0 v400.ptm test copy
destination: maps/ZZ_TEST ROUNDTRIP v400.ptm
size: 41469 bytes
sha256: fa08e4ed1569c71fb4faed87c2bf23bee82453d81933602a24277ff37d9b2eaa
```

That round trip was proven byte-identical on Windows. On 2026-07-29, the
ESP32-P4 donor-pinned acceptance image uploaded the same artifact to
`AP3-VLK-002` / `VLK0207629`. A fresh directory listing reported the new
destination at 41,469 bytes and a read-only download produced the expected
SHA-256:

```text
fa08e4ed1569c71fb4faed87c2bf23bee82453d81933602a24277ff37d9b2eaa
```

Since then the path has left acceptance and entered ordinary use. The
maintainer has transferred several different maps to an AccessPort in service,
each one read back and matched by SHA-256 before it was reported as done, and
the written files persist across power cycles and are usable from the
AccessPort afterwards. Unattended transfers — a map staged with no device
attached, then written automatically after the next clean sync — have also
been accepted on hardware.

What remains is narrower than it once was: this document's scripted ceremony
has not been captured as a single transcript, and Linux transport acceptance is
still a separate gate.

The platform-neutral C protocol component rebuilds all five captured `0x1622`
requests byte-for-byte and produces upload chunks that match the Python
protocol oracle. Some captured requests are CSV transfers retained solely as
protocol evidence; the product upload-target policy rejects them. The normal
P4 production profile now includes the guarded map-transfer path. Its
device-scoped owner consent defaults off after provisioning or recovery and
persists until changed in Settings. The isolated
`sdkconfig.map-write.defaults` overlay adds a donor part-number pin for focused
acceptance work.

## Required safety gates

Every map upload must satisfy all of these:

1. The build or administrator configuration permits device writes.
2. The owner has explicitly enabled device writes after seeing the warning.
   On P4 this is a persistent device-scoped preference that defaults off after
   provisioning or reset. In the Linux reference it remains account-scoped;
   a disposable demo login receives a new default-off permission for every
   temporary session.
3. The destination is directly inside `maps/`, has no traversal components,
   and ends in `.ptm`.
4. The file is non-empty and fits the capture-verified 24-bit chunk length.
5. The connected device and serial remain unchanged throughout the operation.
6. The request and chunk checksums validate locally before transmission.
7. The device sends the expected ready acknowledgement before any file bytes.
8. The device sends the expected completion acknowledgement afterward.
9. A fresh download matches the original size and SHA-256.

Do not retry automatically after a partial chunk write. Mark the connection as
requiring recovery, ask the user to reconnect the AccessPort, and re-list
storage before another write.

## Linux acceptance procedure

Keep `REVLINK_ENABLE_WRITES=false` until the user explicitly authorizes this
controlled test.

1. Connect the established development AccessPort.
2. Record its serial number and export the existing `maps/` listing.
3. Confirm `maps/ZZ_TEST ROUNDTRIP v400.ptm` is absent. If it is present, stop;
   do not overwrite it.
4. Confirm the local test file is exactly 41,469 bytes and has the SHA-256
   above.
5. Enable the administrator write capability, restart RevLink, accept the UI
   warning, and enable writes in Settings.
6. Upload only the established test file to the established destination.
7. Require ready `0x07`, completion `0x23`, and RevLink's automatic read-back
   verification.
8. Re-list `maps/` and confirm the destination and size.
9. Disconnect and reconnect, then download the destination again and verify
   the same SHA-256.
10. Disable device writes immediately after acceptance.

Deletion is not required for the tuner-to-AccessPort workflow and must remain a
separate destructive acceptance test.

## ESP32-P4 acceptance procedure

Do not begin this phase until the ESP32-P4 passes USB enumeration, repeated
directory listing, disconnect recovery, and a byte-perfect read-only datalog
download.

Repeat the Linux procedure with these additional requirements:

- `CONFIG_REVLINK_ALLOW_DEVICE_WRITES=y` is an administrator capability, not
  user consent;
- runtime owner consent is stored separately in NVS, defaults to disabled
  after provisioning or recovery, and remains independently revocable in
  Settings;
- stream the upload from microSD in 512-byte USB transfers rather than requiring
  the entire map in internal RAM;
- calculate and validate the chunk checksum before the first USB transfer;
- enforce task-level deadlines for both acknowledgements; and
- record a local audit entry containing device identity, destination, size,
  source SHA-256, read-back SHA-256, and outcome, without storing map contents
  in logs.

The ESP32-P4 write milestone passes only when the established map survives a
true power cycle and downloads byte-identically afterward.

## ESP32-P4 implementation status

The accepted map-transfer path now implements:

- an exact build-time part-number pin;
- persistent owner consent that defaults off after provisioning or reset;
- one-level `maps/*.ptm` destination validation;
- a fresh listing and case-insensitive no-overwrite check;
- topology, USB handle, true serial, and part-number pinning for the complete
  transaction;
- streamed 512-byte upload chunks with locally validated JAMCRC;
- required ready `0x07` and completion `0x23` acknowledgements;
- an immediate read-only download and SHA-256 comparison;
- an append-only redacted audit record; and
- a sticky recovery latch after indeterminate or partial failures, cleared
  only by a physical detach rather than polite software re-enumeration.

The normal product profile compiles this guarded path without a donor
part-number pin. Exact connected-device identity and USB topology are still
pinned for every operation. The donor overlay remains available for focused
acceptance work.

On 2026-07-29 the production profile's consent lifecycle was verified without
performing an AccessPort write: the default state was locked, enabled consent
survived a hard reset, disabled consent survived a second hard reset, and the
device was left locked.

During the first live pass the AccessPort accepted the file, but the local
cache rejected history publication because its path validator did not permit
spaces in map names. This produced an inaccurate `ESP_ERR_INVALID_CRC` result
after the upload had completed. Manifest and history validators now accept
safe spaces and parentheses, host tests cover those names, and a subsequent
read-only session recovered the exact uploaded bytes and digest. Automatic
write retry was never performed.
