#!/usr/bin/env bash
# The authoritative test interpreter, and the suite runners.
#
# Use the NATIVE path form (C:/...), not the MSYS form (/c/...). The MSYS form
# fails intermittently on this host -- the directory metadata resolves but the
# executable is reported "No such file or directory" for seconds at a time,
# while the native form works every time. That produced a long stretch of
# confusing "interpreter vanished" moments that were purely a path-translation
# problem, not a missing interpreter.
#
# Hermes's own `python` is 3.14.7 and is ABI-INCOMPATIBLE with the module, which
# links against 3.14.5. Using it produces an apparent native import crash
# (0xC0000005) that is really just a version mismatch.
set -u
cd /c/dev/Wasteland || exit 2

# Resolve the authoritative interpreter, do not hardcode it.
#
# The original location, AppData/Local/Python/pythoncore-3.14-64, stopped
# existing on 2026-10-04, and reinstalling to it fails with "Access is denied"
# because something still holds the directory. A hardcoded path that a third
# party can delete turns every run into a mystery "interpreter not found" --
# and `where /R` still REPORTS the missing file from a stale cache, so it looks
# present right up until you try to run it. Verify by RUNNING, never by stat-ing.
#
# The module links against CPython 3.14.5. Hermes' own 3.14.7 is ABI-incompatible
# and produces an apparent native crash (0xC0000005) that is really a version
# mismatch.
resolve_python() {
  local c
  for c in \
    "C:/Users/rtoue/AppData/Local/Python/pythoncore-3.14-64/python.exe" \
    "C:/dev/Wasteland/.buildlogs/py314/cpython-3.14.5-windows-x86_64-none/python.exe" \
    "C:/Users/rtoue/.local/bin/python3.14.exe"
  do
    if "$c" -c "import sys; sys.exit(0 if sys.version_info[:3]==(3,14,5) else 1)" \
         >/dev/null 2>&1; then
      printf '%s' "$c"; return 0
    fi
  done
  c="$(uv python find 3.14.5 2>/dev/null)"
  if [ -n "$c" ] && "$c" -c "import sys; sys.exit(0 if sys.version_info[:3]==(3,14,5) else 1)" \
       >/dev/null 2>&1; then
    printf '%s' "$c"; return 0
  fi
  return 1
}

if ! PY314="$(resolve_python)"; then
  echo "FATAL: no working CPython 3.14.5 interpreter." >&2
  echo "  The module is linked against 3.14.5; 3.14.7 is ABI-incompatible." >&2
  echo "  Restore it with:" >&2
  echo "    uv python install --install-dir C:/dev/Wasteland/.buildlogs/py314 3.14.5" >&2
  exit 2
fi
export WASTELAND_PYTHON="$PY314"

# ST-3: name the module under test on every run, with its mtime.
# `t.sh` used to set no WASTELAND_MODULE_DIR, so wlpath globbed bin/* and
# preferred Release -- which is kernel-locked, so runs silently exercised a
# STALE build. That produced a false red on a CORRECT physics accumulator and
# nearly caused working code to be "fixed". Print it every time.
if [ -z "${WASTELAND_MODULE_DIR:-}" ]; then
  for d in .buildlogs/linkprobe \
           bin/Release-windows-x86_64/DemonCore-Editor; do
    if [ -f "$d/Wasteland.pyd" ]; then WASTELAND_MODULE_DIR="$d"; break; fi
  done
  # ABSOLUTE, and in NATIVE form (C:/...). The runners below `cd` into
  # tests/headless, so a relative override resolves against the wrong directory;
  # and an MSYS form (/c/...) is invisible to the Windows interpreter, so
  # wlpath's os.path.isdir() rejects it and silently falls back to globbing
  # bin/* -- i.e. to the kernel-locked stale module. That is precisely the
  # failure this banner exists to make visible, and it is why the override has
  # to be both absolute AND native before it is exported.
  case "$WASTELAND_MODULE_DIR" in
    [A-Za-z]:/*) : ;;                       # already native + absolute
    /*)          WASTELAND_MODULE_DIR="C:/${WASTELAND_MODULE_DIR#/}" ;;
    *)           WASTELAND_MODULE_DIR="$PWD/${WASTELAND_MODULE_DIR}" ;;
  esac
  WASTELAND_MODULE_DIR="$(cygpath -m "$WASTELAND_MODULE_DIR" 2>/dev/null \
                         || printf '%s' "$WASTELAND_MODULE_DIR")"
  export WASTELAND_MODULE_DIR
fi
if [ -f "${WASTELAND_MODULE_DIR:-/nonexistent}/Wasteland.pyd" ]; then
  _pyd_mtime="$(date -r "$WASTELAND_MODULE_DIR/Wasteland.pyd" '+%Y-%m-%d %H:%M:%S')"
  _newest_src="$(ls -t Wasteland/src/Wasteland/*/*.cpp 2>/dev/null | head -1)"
  _src_mtime="$(date -r "$_newest_src" '+%Y-%m-%d %H:%M:%S' 2>/dev/null)"
  echo "  python : $PY314"
  echo "  module : $WASTELAND_MODULE_DIR/Wasteland.pyd  ($_pyd_mtime)"
  if [ -n "$_src_mtime" ] && [[ "$_pyd_mtime" < "$_src_mtime" ]]; then
    echo "  WARNING: module is OLDER than $_newest_src ($_src_mtime)."
    echo "           A red suite may be a stale binary, not a code bug."
  fi
fi

case "${1:-all}" in
  headless)
    shift
    cd tests/headless || exit 2
    exec "$PY314" -u runner.py "$@"
    ;;
  scratch)
    shift
    cd /c/Users/rtoue/AppData/Local/hermes/cache/scratch || exit 2
    exec "$PY314" -u run_all_suites.py "$@"
    ;;
  repo)
    # Top-level tests/*.py suites that are not Visual Blocks. Each one prints
    # its own tally; the grep keeps the output to one line per suite so a
    # failure is visible in the summary rather than buried in a traceback.
    shift
    cd tests || exit 2
    if [ "$#" -gt 0 ]; then
      set -- "$@"
    else
      set -- test_no_identity_types.py test_enemy_migration.py test_shim_arity.py
    fi
    for f in "$@"; do
      [ -f "$f" ] || continue
      printf '  %-30s ' "$f"
      "$PY314" -u "$f" 2>&1 | tr -d '\000' |
        grep -aoE "checks? passed|[0-9]+/[0-9]+ checks passed|^OK|passed [0-9]+, failed [0-9]+" | tail -1
    done
    ;;
  visualblocks)
    cd tests || exit 2
    for f in test_visual_blocks.py test_visual_blocks_flow.py; do
      printf '  %-30s ' "$f"
      "$PY314" -u "$f" 2>&1 | tr -d '\000' |
        grep -aoE "passed [0-9]+, failed [0-9]+" | tail -1
    done
    printf '  %-30s ' test_visual_blocks_edit.py
    "$PY314" -u test_visual_blocks_edit.py 2>&1 | tr -d '\000' |
      grep -aoE "^(OK|FAILED.*)" | head -1
    ;;
  native)
    # C++ test binaries with no other runner (SeedingSmokeTest). Treats a SKIP
    # as a failure -- see the note in tnative.sh.
    exec bash "$(dirname "${BASH_SOURCE[0]}")/tnative.sh"
    ;;
  d1)
    # The age module's D1 guard. Pure source analysis, so it needs no module and
    # no build -- runnable while an unrelated build is broken.
    cd tests || exit 2
    exec "$PY314" -u test_no_identity_types.py
    ;;
  which)
    "$PY314" --version
    ;;
  *)
    echo "usage: t.sh [headless|scratch|visualblocks|native|d1|which] [suite names...]"
    exit 2
    ;;
esac