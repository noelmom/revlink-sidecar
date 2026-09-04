# Web flasher

A static page that flashes a RevLink Sidecar release over Web Serial. No
build step — plain HTML, CSS, and ES modules. Serve the directory over HTTPS
(or `localhost`) and it works.

```bash
python3 -m http.server 8777 --directory web/flash
```

It is self-contained and origin-relative, so the same directory can be
published on GitHub Pages, dropped onto revlinkgarage.com, or embedded in an
iframe.

## Layout

```
index.html                      markup and copy
style.css                       styling, light and dark
flash.js                        Web Serial + esptool-js driver
gate.js                         board-compatibility gate (no browser deps)
gate.test.mjs                   tests for the gate and the release manifest
vendor/esptool-js-0.6.1.js      vendored flashing library (Apache-2.0)
firmware/<release>/             binaries + manifest.json + SHA256SUMS
```

## Why the library is vendored

`esptool-js` is committed here rather than loaded from a CDN. This page writes
to people's hardware; a CDN that can substitute the script can substitute what
gets written. Vendoring also means the page keeps working offline and when a
CDN is blocked.

Upgrading it is deliberate: replace the file, bump the import in `flash.js`,
record the new SHA-256 below, and re-test against a real board.

```
esptool-js 0.6.1
sha256  ef7d5a237d3f273ecf546bcee65dddad90bd82cf02f22a980d1537e0cd79a152
```

## The board gate

The ESP32-P4-NANO and the ESP32-P4-WIFI6-DEV-KIT both report as `ESP32-P4`
but use pre-v3 and v3+ silicon, which ESP-IDF treats as mutually incompatible
firmware targets. `gate.js` reads the silicon revision and **refuses** a
mismatch before writing anything.

It fails closed. An unreadable revision, an unexpected chip family, or a
missing manifest field all refuse; only a positive match enables the button.
`gate.test.mjs` covers each case, and runs in `scripts/ci-local.sh`.

## Integrity

Every part is fetched, size-checked, and SHA-256 verified against
`manifest.json` before a byte reaches the board. A truncated download or a
tampered mirror fails before flashing, not halfway through it.

`gate.test.mjs` additionally verifies that the published binaries match their
recorded digests, that flash regions do not overlap, and that the image agrees
with what the manifest claims about device writes and deletes — by looking for
the corresponding log strings and NVS keys in the binary rather than trusting
the manifest's own flags, in either direction. A manifest that *under*-claims
fails too: people decide what to install from it.

It also asserts the shipping posture those flags imply — that a write-capable
image still declares consent locked at startup, and that a delete-capable one
carries its own separate consent key rather than riding on the write flag.

## Releases are immutable

```
releases.json                    {"current": "v0.2.6-nano"}
firmware/v0.2.6-nano/            binaries + manifest.json + SHA256SUMS
firmware/v0.2.5-nano/            the previous one, still reachable
```

A published release directory is never rewritten. A version string has to name
one set of bytes: if two different binaries both call themselves 0.2.0, a bug
report against 0.2.0 cannot be resolved, and someone comparing behaviour
between two boards has nothing to compare. Version 0.2.0 was overwritten
repeatedly while this repository was being built and was removed rather than
left as a false reference.

Publishing therefore *adds* a directory and moves the pointer in
`releases.json`. The page and the tests both read that pointer, so nothing has
to be edited in two places, and older releases stay reachable for anyone who
needs to go back to a build that worked.

`gate.test.mjs` enforces the parts of this that can be checked: the manifest
version must match the directory it lives in, and every release named in
`releases.json` must exist with digests that verify.

## Cutting a new release

```bash
# 1. Bump firmware/esp32p4/version.txt. Every published build gets its own
#    version; commits between publishes do not need one.
# 2. Build the profile you intend to publish.
source "$HOME/.espressif/frameworks/esp-idf-v6.0.2/export.sh"
B="$PWD/build-release/nano"
D='sdkconfig.defaults;sdkconfig.oled.defaults;sdkconfig.wifi-scan.defaults;sdkconfig.wifi-join.defaults;sdkconfig.network-runtime.defaults;sdkconfig.onboarding.defaults;sdkconfig.nano.defaults'
idf.py -C firmware/esp32p4 -B "$B" \
  -D "SDKCONFIG=$B/sdkconfig" -D "SDKCONFIG_DEFAULTS=$D" \
  set-target esp32p4 build

# 3. Copy the four artefacts into a NEW web/flash/firmware/<version>-<profile>/
#    as bootloader.bin, partition-table.bin, ota-data-initial.bin and
#    revlink-sidecar.bin. Never into an existing one.
# 4. Write its manifest.json and SHA256SUMS.
# 5. Add it to releases.json and set "current" to it.
# 6. Run ./scripts/ci-local.sh — the manifest, version and digest assertions
#    must pass.
# 7. Flash a real board before announcing it.
# 8. Re-check the docs against what you just flashed. See below.
# 9. Open a PR and MERGE IT WITH A MERGE COMMIT, not a squash. See below.
# 10. Tag the build commit, then cut a GitHub Release pointing at the release
#     directory on main.
```

### Re-check the docs against what you just built

Every sentence in the docs saying the firmware cannot do something is a claim
with an expiry date. Three have been wrong in public: the landing page said
writing maps was unproven on hardware while it was in daily use, `SAFETY.md`
said consent re-locks every boot when it persists in NVS, and
`WIFI_BRINGUP.md` listed a shipped network stack as future work.

This finds the sentences worth re-reading. It returns a short list, not a wall:

```bash
grep -rniE "not yet|has not been|have not been|is not implemented|\
remains unimplemented|not proven|future work|remaining work|\
not in the published|does not yet" \
  --include="*.md" --include="*.html" docs/ web/site/ *.md
```

Confirm each hit is still true of the build in the release directory, and fix
what is not in the same PR.

Check them against the tree or the binary, **never against another doc**. The
landing page's false claim was inherited from `SAFETY.md`, so a single stale
sentence propagated outward into the most public surface the project has.

Both directions matter, but they are not equally bad. Understating what
shipped is embarrassing. Overstating a safety property — or failing to notice
one has regressed — is not, and that is the direction this check exists for.

### Merge the release PR, do not squash it

`main` is protected, so a release goes through a pull request. Merge it with a
**merge commit**.

The binary embeds the commit it was built from — `CMakeLists.txt` resolves it
at configure time into `REVLINK_GIT_COMMIT`, it is what the portal footer
shows, and it is what `manifest.json` records as the provenance for those
bytes. A squash mints a new hash and discards the one that was built, so the
manifest ends up naming a commit that never reaches `main`. The release
directory then cannot be traced back to anything, which is the entire property
it exists to provide.

After merging, confirm it survived:

```bash
git merge-base --is-ancestor <build-commit> main && echo ok
```

### Tag the build commit, not the publish commit

```bash
git tag -a v0.2.6 <build-commit> -m "RevLink Sidecar 0.2.6 (v0.2.6-nano)"
git push origin v0.2.6
```

The tag names the commit whose source reproduces the published bytes. It
deliberately does **not** contain the release directory, which is added by the
commit after it — so a GitHub Release body must link the directory on `main`:

```
https://github.com/noelmom/revlink-sidecar/tree/main/web/flash/firmware/v0.2.6-nano
```

not at the tag, where it 404s. Release directories are immutable, so a link to
`main` is stable forever.

It is tempting to fix that by moving the tag onto the publish commit instead.
Do not. For 0.2.2 the embedded `portal/index.html` was corrected between the
build and the publish commit, so the publish commit does not rebuild the bytes
that were shipped. Tagging it would trade a broken link for a false provenance
claim, which is worse.

Offsets for the current partition table:

| Part | Offset |
| --- | --- |
| `bootloader.bin` | `0x2000` |
| `partition-table.bin` | `0x8000` |
| `ota-data-initial.bin` | `0xF000` |
| `revlink-sidecar.bin` | `0x20000` |

## Browser support

Web Serial is Chromium-only — Chrome, Edge, Opera, Arc, Brave. Safari and
Firefox do not implement it, and the page says so plainly instead of failing
mysteriously.

## Driver support

The Nano's programming console runs through an onboard WCH USB-serial bridge
(`1a86:55d3`). No driver is needed: confirmed on a clean Windows 11 machine and
on macOS, where the board enumerates on its own and flashes without anything
being installed first.
