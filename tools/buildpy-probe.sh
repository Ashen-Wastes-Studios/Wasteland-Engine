#!/usr/bin/env bash
# Build the Python module to a scratch output directory.
#
# Why this exists: tools/buildpy.sh links into bin/Release-windows-x86_64/
# DemonCore-Editor/Wasteland.pyd, and that path is frequently held open by a
# long-lived process -- the hermes execute_code kernel imports Wasteland.pyd at
# session start and holds it for the whole session. The symptom is the worst
# possible one for a build:
#
#   LINK : fatal error LNK1104: cannot open file '...Wasteland.pyd'
#   MSBUILD_RC=1
#
# with **zero** `error C` lines. Every translation unit compiled perfectly; only
# the final write failed. Because the log's error list is empty, this reads as
# a broken build and sends you hunting in the source for a defect that is not
# there. The objects are already on disk and are fine.
#
# Do not kill the holding process to "fix" it. Linking somewhere else is faster,
# does not disturb the running session, and gives a module you can actually
# test against.
#
# THE TRAP, which cost a real cycle: passing /p:OutDir to
# WastelandPython.vcxproj redirects the parent .pyd, but the ProjectReference to
# Wasteland.vcxproj writes its .lib by ITS OWN OutDir. So the parent links
# against whatever stale Wasteland.lib happens to be sitting there. The build
# reports RC=0, every .obj is fresh, and the module silently contains the OLD
# code. Observed exactly that: NodeLibrary.obj demonstrably contained the new
# symbol while the loaded module did not have it. Hence the staleness check at
# the bottom, which is the only thing that catches this.
#
# Usage: tools/buildpy-probe.sh [config]
# Prints the path of the freshly linked module on the last line.
set -u
cd /c/dev/Wasteland || exit 2

CONFIG="${1:-Release}"
OUT="C:\\dev\\Wasteland\\.buildlogs\\linkprobe\\"
LOG=/c/dev/Wasteland/.buildlogs/linkprobe.log
mkdir -p /c/dev/Wasteland/.buildlogs

bash tools/msb.sh WastelandPython.vcxproj "/p:Configuration=$CONFIG" "/p:OutDir=$OUT" > "$LOG" 2>&1
RC=$?

echo "MSBUILD_RC=$RC"
grep -aE "error [CL][0-9]+" "$LOG" | sed 's/^[^(]*//' | sort -u | head -12 | cut -c1-170

if [ $RC -ne 0 ] && ! grep -aq "LNK1104" "$LOG"; then
    echo "--- real failure; last errors ---"
    grep -aoE "error [A-Z]+[0-9]+[^\"]{0,140}" "$LOG" | sort -u | head -10
fi

PYD=".buildlogs/linkprobe/Wasteland.pyd"
if [ -f "$PYD" ]; then
    echo "MODULE: C:/dev/Wasteland/$PYD ($(stat -c%s "$PYD") bytes, $(date -r "$PYD" '+%H:%M:%S'))"
fi

# DO NOT mistake a green probe build for a shipped one. This script links
# ELSEWHERE, so bin/Release-windows-x86_64/DemonCore-Editor/Wasteland.pyd keeps
# whatever it had last while every green suite runs against the copy here. That
# divergence is silent: observed with the canonical module passing 25/28 navmesh
# checks against this probe's 28/28, an hour and two fixes apart, with nothing
# erroring. When the work is ready to ship, run tools/buildpy-canonical.sh to
# refresh the real output and re-verify against it.
#
# A .pyd older than an object means the link used a stale Wasteland.lib, so the
# module silently contains the previous code while every build looks perfect.
STALE=$(find ./bin-int/$CONFIG-windows-x86_64 -name '*.obj' -newer "$PYD" 2>/dev/null | head -4)
if [ -n "$STALE" ]; then
    echo "WARNING: objects newer than the .pyd -- the link used a stale lib:"
    echo "$STALE" | sed 's/^/  /'
fi
exit $RC