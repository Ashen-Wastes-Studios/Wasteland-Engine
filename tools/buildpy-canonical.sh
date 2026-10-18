#!/usr/bin/env bash
# Refresh the CANONICAL Python module -- the one the editor and the suites load
# by default -- and prove it is the same binary the tests verified.
#
# ## Why this is not just buildpy-probe.sh with a different path
#
# The first version of this script passed /p:OutDir=<canonical dir> to
# WastelandPython.vcxproj. That redirects the parent .pyd, but the
# ProjectReference to Wasteland.vcxproj honours the same OutDir, so the engine's
# STATIC library is written to the same directory. The engine static lib is
# Wasteland.lib, and so is the module's import library -- link.exe sees the same
# name as both input and output and refuses:
#
#   LINK : fatal error LNK1149: output filename matches input filename
#          '...\DemonCore-Editor\Wasteland.lib'
#
# Worse, link.exe opens the output before it fails, so the working module is
# left TRUNCATED TO 0 BYTES while MSBuild still reports RC=0. The next import
# then dies with the maximally misleading:
#
#   ImportError: DLL load failed: %1 is not a valid Win32 application
#
# which reads as an ABI mismatch or a wrong interpreter. All three were wrong.
# The file was empty. (That is how this defect was caught: the script "succeeded"
# with RC=0 and destroyed a working module.)
#
# So: build to scratch, where the two libraries do not collide, and COPY the
# finished artifact into place. That reuses the mechanism buildpy-probe.sh
# already proves works, and separates "did it build" from "did it land".
#
# ## Verification is the point
#
# A successful copy proves nothing on its own, so this checks:
#   - the scratch module is non-empty and a real PE image (MZ)
#   - it is newer than every .obj, i.e. the link did not use a stale .lib
#   - the copy at the canonical path is byte-identical to it
#
# Usage: tools/buildpy-canonical.sh
set -u
cd /c/dev/Wasteland || exit 2

CANON=/c/dev/Wasteland/bin/Release-windows-x86_64/DemonCore-Editor/Wasteland.pyd
PROBE=/c/dev/Wasteland/.buildlogs/linkprobe/Wasteland.pyd

echo "=== 1. build to scratch (no library-name collision) ==="
bash tools/buildpy-probe.sh || {
    echo "build failed; canonical module left untouched"
    exit 1
}

echo
echo "=== 2. validate the scratch artifact ==="
if [ ! -f "$PROBE" ]; then
    echo "FAIL: no scratch module produced"; exit 1
fi
SIZE=$(stat -c%s "$PROBE")
if [ "$SIZE" -lt 100000 ]; then
    echo "FAIL: $PROBE is $SIZE bytes -- a real module is megabytes. Link aborted?"
    exit 1
fi
if [ "$(head -c2 "$PROBE")" != "MZ" ]; then
    echo "FAIL: $PROBE is not a PE image (no MZ header)"; exit 1
fi
echo "  size  $SIZE bytes"
echo "  magic MZ ok"

# Check only inputs that are actually linked into this module. Editor objects
# live below the shared bin-int root too, but are not module dependencies.
ENGINE_OBJ_DIR="./bin-int/Release-windows-x86_64/Wasteland"
ENGINE_LIB="./.buildlogs/linkprobe/Wasteland.lib"
BOX2D_OBJ_DIR="./Wasteland/vendor/Box2D/bin-int/Release-windows-x86_64/Box2D"
BOX2D_LIB="./.buildlogs/linkprobe/Box2D.lib"
BOX3D_OBJ_DIR="./Wasteland/vendor/Box3D/bin-int/Release-windows-x86_64/Box3D"
BOX3D_LIB="./.buildlogs/linkprobe/Box3D.lib"
BINDING_OBJ_DIR="./obj/Release"

for required in "$ENGINE_OBJ_DIR" "$ENGINE_LIB" "$BOX2D_OBJ_DIR" "$BOX2D_LIB" "$BOX3D_OBJ_DIR" "$BOX3D_LIB" "$BINDING_OBJ_DIR"; do
    if [ ! -e "$required" ]; then
        echo "FAIL: required module build input is missing: $required"; exit 1
    fi
done

STALE=""
NEWER=$(find "$ENGINE_OBJ_DIR" -type f -name '*.obj' -newer "$ENGINE_LIB" 2>/dev/null | head -4)
[ -z "$NEWER" ] || STALE="${STALE}${NEWER}"$'\n'
NEWER=$(find "$BOX2D_OBJ_DIR" -type f -name '*.obj' -newer "$BOX2D_LIB" 2>/dev/null | head -4)
[ -z "$NEWER" ] || STALE="${STALE}${NEWER}"$'\n'
NEWER=$(find "$BOX3D_OBJ_DIR" -type f -name '*.obj' -newer "$BOX3D_LIB" 2>/dev/null | head -4)
[ -z "$NEWER" ] || STALE="${STALE}${NEWER}"$'\n'
NEWER=$(find "$BINDING_OBJ_DIR" -type f -name '*.obj' -newer "$PROBE" 2>/dev/null | head -4)
[ -z "$NEWER" ] || STALE="${STALE}${NEWER}"$'\n'
for input in ./.buildlogs/linkprobe/*.lib; do
    [ -f "$input" ] || continue
    if [ "$input" -nt "$PROBE" ]; then STALE="${STALE}${input}"$'\n'; fi
done
if [ -n "$STALE" ]; then
    echo "FAIL: module-linked objects/libraries are newer than their consumers:"
    printf '%s' "$STALE" | sed '/^$/d' | sort -u | sed 's/^/  /'
    exit 1
fi
echo "  fresh against all objects"

echo
echo "=== 3. install into the canonical path ==="
if ! cp "$PROBE" "$CANON"; then
    echo "FAIL: could not write $CANON"
    echo "      Something holds it open. Do NOT kill a Hermes process to release"
    echo "      it -- wait for it to exit and re-run. WASTELAND_MODULE_DIR lets"
    echo "      the suites use the scratch module meanwhile."
    exit 1
fi

echo
echo "=== 4. verify what landed ==="
CSIZE=$(stat -c%s "$CANON")
if [ "$CSIZE" -ne "$SIZE" ]; then
    echo "FAIL: canonical is $CSIZE bytes, expected $SIZE"; exit 1
fi
if [ "$(head -c2 "$CANON")" != "MZ" ]; then
    echo "FAIL: canonical is not a PE image"; exit 1
fi
if ! cmp -s "$PROBE" "$CANON"; then
    echo "FAIL: canonical does not match the verified build"; exit 1
fi
echo "  canonical: $CSIZE bytes, $(date -r "$CANON" '+%H:%M:%S'), byte-identical to the verified build"
echo "OK"
