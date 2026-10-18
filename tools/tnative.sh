#!/usr/bin/env bash
# Run the C++ test binaries that have no runner, so they stop being orphaned.
#
# `SeedingSmokeTest.exe` was built in both configs and referenced by nothing --
# no script, no solution hook, no CI -- and the plan flagged it as "has never
# actually run". It turns out it works (RESULT: PASS against the editor assets).
#
# It was never run because it needs an assets directory on the command line and
# silently SKIPS -- exit 0 -- when it cannot find one. That is the trap: a skip
# is indistinguishable from a pass, so handing it a wrong path looks green.
#
# Hence the NATIVE path below. MSYS does not translate paths for a native
# Windows exe unless told not to, and "/c/dev/..." reaches the exe as a literal
# string that std::filesystem cannot resolve, so the test SKIPs and reports
# success. Always pass "C:/..." with MSYS_NO_PATHCONV=1.
#
# Exits non-zero if any binary fails OR skips, so a skip can never be mistaken
# for a pass by whatever runs this next.
set -u

cd /c/dev/Wasteland || exit 2

BINDIR="tests/bin-int/tests/Release-windows-x86_64"
ASSETS_NATIVE="C:/dev/Wasteland/DemonCore-Editor/assets"
FAILED=0

run() {
    local label="$1"; shift
    local exe="$1"; shift
    if [ ! -x "$exe" ]; then
        echo "  SKIP  $label -- binary not built: $exe"
        FAILED=1
        return
    fi
    local out
    out=$(MSYS_NO_PATHCONV=1 "$exe" "$@" 2>&1)
    local rc=$?
    echo "$out" | grep -aE "RESULT:|FAIL|MISSING|EMPTY" | sed "s/^/  $label: /"
    if echo "$out" | grep -qa "RESULT: SKIP"; then
        echo "  $label: SKIPPED -- a skip is not a pass" >&2
        FAILED=1
    elif [ $rc -ne 0 ]; then
        echo "  $label: exit $rc" >&2
        FAILED=1
    fi
}

echo "== C++ test binaries =="
run "SeedingSmokeTest" "$BINDIR/SeedingSmokeTest.exe" "$ASSETS_NATIVE"

if [ $FAILED -ne 0 ]; then
    echo
    echo "native C++ tests: FAILED"
    exit 1
fi
echo
echo "native C++ tests: PASS"