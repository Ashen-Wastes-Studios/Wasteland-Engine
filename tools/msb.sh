#!/usr/bin/env bash
# Serialise MSBuild runs for this repo, and reset the PDB server between them.
#
# Overlapping builds are the cause of every "C1041: cannot open program
# database", "MSB6006: CL.exe exited with code 1", "MSB6003: ... could not be
# run", and "C1083: cannot open compiler generated file" seen here. Two
# distinct mechanisms, both handled below:
#
#   1. Two builds at once. An orphaned cl.exe from a killed or timed-out build
#      keeps the .pch/.obj handles and the next build dies on a file it cannot
#      open. /FS in Directory.Build.props serialises PDB writes *within* one
#      build; it does nothing about two builds racing for the same objects.
#
#   2. A wedged mspdbsrv. Symptom is error code '23' (0x000006BA =
#      RPC_S_SERVER_UNAVAILABLE). The server is a shared singleton that can
#      survive a client crash in a bad state, so later builds fail instantly
#      even with nothing else running. Restarting it fixes it outright.
#
# Usage: msb.sh <project> [extra MSBuild args...]
set -u
# No `set -e`: a non-zero MSBuild result must still reach the caller with the
# log intact, and the log is what says what happened.
MSB="/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe"
PROJ="$1"; shift

# 1. Wait out any straggler COMPILER from a previous run.
#
# Only cl.exe and link.exe count. MSBuild.exe is deliberately excluded: MSBuild
# keeps idle `nodeReuse` worker nodes alive between builds by design, so
# waiting for MSBuild.exe to disappear waits forever -- this loop ran its full
# 120 x 5s = 10 minutes and then the build was killed mid-LTCG with no error,
# which is a genuinely confusing failure. Those idle nodes hold nothing the
# next build needs.
for _ in $(seq 1 120); do
  n=$(tasklist 2>/dev/null | grep -ciE '^cl\.exe|^link\.exe' || true)
  [ "$n" -eq 0 ] && break
  sleep 5
done

# 2. Reset the PDB server. Harmless when it is healthy.
powershell -NoProfile -Command \
  "Stop-Process -Name mspdbsrv -Force -ErrorAction SilentlyContinue" >/dev/null 2>&1 || true
sleep 1

"$MSB" "$PROJ" -p:Configuration=Release -p:Platform=x64 -t:Build \
       -v:m -m:1 -nologo "$@"
rc=$?
echo "MSBUILD_RC=$rc"
exit $rc