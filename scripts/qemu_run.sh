#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Catfish Feeder — headless QEMU smoke test for the display unit.
#
# Builds firmware/display-unit (env:display-unit-qemu), uses the resulting
# factory image as an emulated 16 MB flash and boots it in Espressif's QEMU
# (esp32s3 machine). The firmware runs the full application headless (LVGL in
# a RAM framebuffer; no panel/touch/RTC/SD/nRF24/WiFi) and scripts a UI-event
# smoke sequence. PASS requires ALL of:
#   - "[QEMU] SMOKE PASS" appears in the serial log
#   - no "[QEMU] FAIL" lines
#   - no crash markers ("Guru Meditation", panic, assert, abort, ...)
#   - exactly one boot (no reset loop)
#
# Usage: scripts/qemu_run.sh [serial-log-file]
# Env overrides: QEMU_BIN, QEMU_RUN_SECONDS (default 75), SKIP_BUILD=1
# Docs: docs/tooling.md ("QEMU emulator")
# ---------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJ="$ROOT/firmware/display-unit"
BUILD="$PROJ/.pio/build/display-unit-qemu"
LOG="${1:-$BUILD/qemu-serial.log}"
RUN_SECONDS="${QEMU_RUN_SECONDS:-75}"
FLASH_SIZE=$((16 * 1024 * 1024))

# --- locate qemu-system-xtensa (Espressif fork with the esp32s3 machine) ----
QEMU="${QEMU_BIN:-}"
if [ -z "$QEMU" ]; then
  if command -v qemu-system-xtensa >/dev/null 2>&1; then
    QEMU="$(command -v qemu-system-xtensa)"
  elif [ -x "$ROOT/../tools/qemu/bin/qemu-system-xtensa" ]; then
    QEMU="$ROOT/../tools/qemu/bin/qemu-system-xtensa"
  fi
fi
if [ -z "$QEMU" ]; then
  echo "ERROR: qemu-system-xtensa (Espressif fork) not found." >&2
  echo "Install it from https://github.com/espressif/qemu/releases, e.g.:" >&2
  echo "  curl -L <esp_qemu-xtensa-linux tarball> | tar -xJ -C tools/   (or set QEMU_BIN)" >&2
  exit 2
fi

# Espressif's QEMU needs libslirp, SDL2 and friends. IMPORTANT: never export
# LD_LIBRARY_PATH globally in this script — prepending a system lib dir
# shadowed the setup-python interpreter's bundled libpython on GitHub runners
# and broke `pio` (ModuleNotFoundError: platformio). The override below is
# scoped: it only affects the ldd preflight and the QEMU invocation itself.
LDP_QEMU="$LD_LIBRARY_PATH"
if ldd "$QEMU" 2>/dev/null | grep -q "libslirp.*not found"; then
  for d in "$ROOT/../tools/deps/usr/lib/x86_64-linux-gnu" "/usr/lib/x86_64-linux-gnu" "/usr/lib64"; do
    if ls "$d"/libslirp.so.0* >/dev/null 2>&1; then
      LDP_QEMU="$d${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
      break
    fi
  done
fi

# --- preflight: every shared library must resolve BEFORE the long build -----
MISSING="$(env ${LDP_QEMU:+LD_LIBRARY_PATH="$LDP_QEMU"} ldd "$QEMU" 2>/dev/null | awk '/not found/{print $1}' | sort -u || true)"
if [ -n "$MISSING" ]; then
  echo "ERROR: $QEMU has unresolved shared libraries:" >&2
  echo "$MISSING" >&2
  echo "On Debian/Ubuntu: sudo apt-get install libslirp0 libsdl2-2.0-0 libpixman-1-0" >&2
  exit 2
fi

# --- build (unless SKIP_BUILD=1) --------------------------------------------
if [ "${SKIP_BUILD:-0}" != "1" ]; then
  pio run -e display-unit-qemu -d "$PROJ"
fi

FACTORY="$BUILD/firmware.factory.bin"
if [ ! -f "$FACTORY" ]; then
  # pioarduino emits the merged image; if absent fall back to manual merge
  echo "ERROR: $FACTORY not found (build failed?)" >&2
  exit 2
fi

# --- pad the factory image to the board's 16 MB flash (sparse copy) ---------
IMG="$BUILD/qemu-flash-16MB.img"
cp -f "$FACTORY" "$IMG"
truncate -s "$FLASH_SIZE" "$IMG"

# --- boot headless, capture serial ------------------------------------------
echo "[qemu-run] booting $IMG with $QEMU (up to ${RUN_SECONDS}s) ..."
rm -f "$LOG"
set +e
timeout --signal=INT --kill-after=5 "$RUN_SECONDS" \
  env ${LDP_QEMU:+LD_LIBRARY_PATH="$LDP_QEMU"} "$QEMU" -machine esp32s3 \
          -drive file="$IMG",if=mtd,format=raw \
          -display none -monitor none \
          -serial file:"$LOG"
RC=$?
set -e
# 124 = time limit reached — the normal end of a healthy run
if [ "$RC" -ne 0 ] && [ "$RC" -ne 124 ]; then
  echo "[qemu-run] QEMU exited with code $RC (see $LOG)" >&2
  tail -40 "$LOG" || true
  exit 1
fi

# --- verdict -----------------------------------------------------------------
FAIL=0
if ! grep -q "\[QEMU\] SMOKE PASS" "$LOG"; then
  echo "[qemu-run] FAIL: SMOKE PASS marker missing"
  FAIL=1
fi
if grep -q "\[QEMU\] FAIL" "$LOG"; then
  echo "[qemu-run] FAIL: [QEMU] FAIL lines present"
  FAIL=1
fi
if grep -qE "Guru Meditation|assert failed|abort\(\)|LoadProhibited|StoreProhibited|Cache disabled but cached memory" "$LOG"; then
  echo "[qemu-run] FAIL: crash markers in serial log"
  FAIL=1
fi
BOOTS=$(grep -cE "rst:0x[0-9a-fA-F]+" "$LOG" || true)
if [ "${BOOTS:-0}" -gt 1 ]; then
  echo "[qemu-run] FAIL: $BOOTS resets detected (boot loop?)"
  FAIL=1
fi
if grep -qi "panic" "$LOG"; then
  echo "[qemu-run] FAIL: panic in serial log"
  FAIL=1
fi

if [ "$FAIL" -eq 0 ]; then
  echo "[qemu-run] PASS (serial log: $LOG)"
  exit 0
fi
echo "---- last 60 serial lines ----"
tail -60 "$LOG" || true
exit 1
