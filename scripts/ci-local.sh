#!/usr/bin/env bash
#
# RevLink Sidecar local CI.
#
# Runs everything that can be checked without an AccessPort attached. No
# network, no device I/O, nothing is flashed.
#
#   ./scripts/ci-local.sh            host tests + portal tests + geometry
#   ./scripts/ci-local.sh --full     the above, plus a full ESP32-P4 build
#
# The host tests need only a C compiler. ESP-IDF is required for --full and is
# used opportunistically for its bundled CMake/Ninja when it is present.

set -Eeuo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_MODE="quick"

case "${1:-}" in
  "") ;;
  --full) RUN_MODE="full" ;;
  --quick) RUN_MODE="quick" ;;
  *) echo "Usage: $0 [--quick|--full]" >&2; exit 64 ;;
esac

START_SECONDS="$SECONDS"
FAILED_PHASE=""

finish() {
  local status=$?
  local elapsed=$((SECONDS - START_SECONDS))
  if [[ $status -eq 0 ]]; then
    printf '\n\033[32mLOCAL CI PASSED\033[0m (%s mode, %ss)\n' \
      "$RUN_MODE" "$elapsed"
  else
    printf '\n\033[31mLOCAL CI FAILED\033[0m (%s mode, %ss, exit %s)%s\n' \
      "$RUN_MODE" "$elapsed" "$status" \
      "${FAILED_PHASE:+ during: $FAILED_PHASE}"
  fi
  exit "$status"
}
trap finish EXIT

phase() {
  FAILED_PHASE="$1"
  printf '\n\033[1m==> %s\033[0m\n' "$1"
}

have() { command -v "$1" >/dev/null 2>&1; }

cd "$REPO_ROOT"

# ---------------------------------------------------------------------------
# Toolchain discovery
# ---------------------------------------------------------------------------

# ESP-IDF ships CMake and Ninja. Source it when present so contributors who
# already have it do not need a second copy; it is build-only and never
# touches attached hardware.
IDF_EXPORT="${REVLINK_IDF_EXPORT:-$HOME/.espressif/frameworks/esp-idf-v6.0.2/export.sh}"
IDF_AVAILABLE=0
if [[ -f "$IDF_EXPORT" ]]; then
  # ESP-IDF's export.sh reads unbound variables and returns non-zero in some
  # paths, so relax errexit/nounset across it and restore them immediately.
  set +eu
  # shellcheck disable=SC1090
  source "$IDF_EXPORT" >/dev/null 2>&1 || true
  set -eu
  have idf.py && IDF_AVAILABLE=1
fi

if [[ "$RUN_MODE" == "full" ]]; then
  if [[ "$IDF_AVAILABLE" -eq 0 ]]; then
    echo "--full needs ESP-IDF v6.0.2." >&2
    echo "Expected export script at: $IDF_EXPORT" >&2
    echo "Override with REVLINK_IDF_EXPORT=/path/to/export.sh" >&2
    exit 2
  fi
  # ESP-IDF does not bundle CMake or Ninja on macOS; idf.py needs both on the
  # PATH. Say so here rather than failing several layers down inside idf.py.
  missing=()
  have cmake || missing+=("cmake")
  have ninja || missing+=("ninja")
  if [[ ${#missing[@]} -gt 0 ]]; then
    echo "--full needs ${missing[*]} on the PATH; idf.py cannot run without it." >&2
    echo "Install with:  brew install ${missing[*]}" >&2
    echo "Host tests still run without either: ./scripts/ci-local.sh" >&2
    exit 2
  fi
fi

have cc || { echo "A C compiler (cc) is required." >&2; exit 2; }

printf 'RevLink Sidecar local CI\n'
printf 'Mode:    %s\n' "$RUN_MODE"
printf 'Commit:  %s\n' "$(git rev-parse --short HEAD 2>/dev/null || echo 'not a git repo')"
printf 'CMake:   %s\n' "$(have cmake && cmake --version | head -1 || echo 'absent (using direct compiler fallback)')"
printf 'ESP-IDF: %s\n' "$([[ $IDF_AVAILABLE -eq 1 ]] && idf.py --version 2>/dev/null || echo 'absent')"

# ---------------------------------------------------------------------------

if git rev-parse --git-dir >/dev/null 2>&1; then
  phase "Whitespace and patch integrity"
  git diff --check
fi

phase "Firmware host tests"
if have cmake && have ctest; then
  HOST_BUILD="$REPO_ROOT/build-local-ci/host"
  GENERATOR=()
  have ninja && GENERATOR=(-G Ninja)
  cmake -S firmware/test -B "$HOST_BUILD" "${GENERATOR[@]}" >/dev/null
  cmake --build "$HOST_BUILD" --parallel
  ctest --test-dir "$HOST_BUILD" --output-on-failure
else
  # No CMake: compile each host test directly. Mirrors the CMake flags so a
  # contributor without CMake gets the same coverage and the same errors.
  echo "CMake not found; compiling host tests directly."
  OUT="$REPO_ROOT/build-local-ci/host-cc"
  mkdir -p "$OUT"
  INC=()
  for d in firmware/components/*/include; do INC+=("-I$d"); done
  SRC=()
  while IFS= read -r f; do SRC+=("$f"); done < <(
    find firmware/components -name '*.c' ! -path '*accessport_usb*'
  )
  CFLAGS=(-std=c11 -O1 -Wall -Wextra -Wpedantic -Werror
          -D_POSIX_C_SOURCE=200809L)

  failures=0
  # protocol_host_cli links only the protocol component and self-tests.
  if cc "${CFLAGS[@]}" -o "$OUT/protocol_host_cli" \
      firmware/test/protocol_host_cli.c \
      firmware/components/accessport_protocol/revlink_accessport_protocol.c \
      -Ifirmware/components/accessport_protocol/include; then
    if "$OUT/protocol_host_cli" self-test; then
      printf '  \033[32mPASS\033[0m protocol_unit\n'
    else
      printf '  \033[31mFAIL\033[0m protocol_unit\n'; failures=$((failures + 1))
    fi
  else
    printf '  \033[31mFAIL\033[0m protocol_unit (build)\n'
    failures=$((failures + 1))
  fi

  for t in firmware/test/*_host_test.c; do
    n="$(basename "$t" .c)"
    if ! cc "${CFLAGS[@]}" -o "$OUT/$n" "$t" "${SRC[@]}" "${INC[@]}"; then
      printf '  \033[31mFAIL\033[0m %s (build)\n' "$n"
      failures=$((failures + 1))
      continue
    fi
    if "$OUT/$n" >/dev/null; then
      printf '  \033[32mPASS\033[0m %s\n' "$n"
    else
      printf '  \033[31mFAIL\033[0m %s\n' "$n"
      "$OUT/$n" || true
      failures=$((failures + 1))
    fi
  done
  [[ $failures -eq 0 ]] || exit 1
fi

phase "Portal tests"
if have node; then
  for f in firmware/test/portal_*.mjs; do
    printf '  %s\n' "$(basename "$f")"
    node "$f"
  done
else
  echo "  node not found; skipping portal tests." >&2
fi

phase "Web flasher tests"
if have node; then
  node --test web/flash/gate.test.mjs
else
  echo "  node not found; skipping flasher tests." >&2
fi

phase "Enclosure geometry baseline"
if have python3; then
  python3 hardware/nano-enclosure/tools/stl_inspect.py \
    hardware/nano-enclosure/enclosure-print/print-package/*.stl
else
  echo "  python3 not found; skipping geometry report." >&2
fi

if [[ "$RUN_MODE" == "full" ]]; then
  phase "ESP32-P4 Nano build"
  NANO_BUILD="$REPO_ROOT/build-local-ci/nano"
  NANO_DEFAULTS="sdkconfig.defaults;sdkconfig.oled.defaults;sdkconfig.wifi-scan.defaults;sdkconfig.wifi-join.defaults;sdkconfig.network-runtime.defaults;sdkconfig.onboarding.defaults;sdkconfig.nano.defaults"
  idf.py -C firmware/esp32p4 \
    -B "$NANO_BUILD" \
    -D "SDKCONFIG=$NANO_BUILD/sdkconfig" \
    -D "SDKCONFIG_DEFAULTS=$NANO_DEFAULTS" \
    set-target esp32p4 build

  phase "ESP32-P4 build with device writes disabled"
  NOWRITE_BUILD="$REPO_ROOT/build-local-ci/nowrites"
  # Keep the fragment outside the build directory: set-target runs fullclean,
  # which would delete a fragment written inside it before CMake reads it.
  NOWRITE_FRAGMENT="$REPO_ROOT/build-local-ci/nowrites.defaults"
  mkdir -p "$REPO_ROOT/build-local-ci"
  printf 'CONFIG_REVLINK_ALLOW_DEVICE_WRITES=n\n' > "$NOWRITE_FRAGMENT"
  idf.py -C firmware/esp32p4 \
    -B "$NOWRITE_BUILD" \
    -D "SDKCONFIG=$NOWRITE_BUILD/sdkconfig" \
    -D "SDKCONFIG_DEFAULTS=$NANO_DEFAULTS;$NOWRITE_FRAGMENT" \
    set-target esp32p4 build
else
  phase "ESP32-P4 builds skipped by --quick"
fi
