# Contributing

Thanks for looking. This project touches hardware people rely on in their
cars, so a few things matter more here than in an average repo.

## Before anything else

Read [SAFETY.md](SAFETY.md). The write gates, the destination allow-list, and
the fail-closed behavior on ambiguous device topology are load-bearing. A PR
that loosens one of them will be asked to justify it in detail, and most such
PRs will be declined.

## Ground rules

1. **Protocol logic stays in C components, never in the browser.** The portal
   renders and requests; it does not frame, checksum, or parse.
2. **New device-facing behavior needs a host test.** The `firmware/test/`
   suites run without hardware and without ESP-IDF — keep it that way.
3. **Never widen the write allow-list** (`maps/*.ptm` one level deep, and the
   exact `images/startup_screen.fb`) without a written rationale.
4. **Never auto-retry a write.** Failed writes stop and wait for a human.
5. **Don't claim hardware acceptance you didn't perform.** If you verified
   something on a real device, say which device, which build, and what you
   measured. If you didn't, say that instead.

## Running the tests

Everything runs on your laptop. No board required.

```bash
./scripts/ci-local.sh
```

This is the same set of checks a maintainer runs before merging. It needs
only a C compiler; CMake and Node are used when present and the script tells
you what it skipped. Please run it before opening a pull request.

To also build the firmware — both with device writes compiled in and with
them compiled out — add `--full`. That needs ESP-IDF v6.0.2 and CMake/Ninja
on your PATH:

```bash
./scripts/ci-local.sh --full
```

Both configurations must build. It is easy to add code that compiles only
when writes are enabled, and the `--full` run is what catches it.

## Firmware builds

ESP-IDF **v6.0.2**, pinned in `firmware/esp32p4/idf-version.txt`. The Nano and
the Dev Kit use different P4 silicon revisions and need separate build
directories — see [`firmware/esp32p4/README.md`](firmware/esp32p4/README.md).
Do not cross-flash them.

## Enclosure contributions

These are especially welcome; see the open items in
[`hardware/nano-enclosure/enclosure-print/README.md`](hardware/nano-enclosure/enclosure-print/README.md).

If you revise geometry, include before/after output from
[`hardware/nano-enclosure/tools/stl_inspect.py`](hardware/nano-enclosure/tools/stl_inspect.py)
so reviewers can confirm an opening was actually closed rather than the mesh
merely re-exported. Ship both STL and 3MF; the 3MF records millimeter units
explicitly.

## Style

Match the surrounding code. The C is C11, compiled with
`-Wall -Wextra -Werror -Wpedantic`. Keep components free of ESP-IDF headers
unless they genuinely are the platform adapter.

## Reporting problems

Ordinary bugs: open an issue. Anything that could damage a device or vehicle,
or cause a write to reach the wrong target: please use GitHub's private
security advisory flow instead of a public issue.
