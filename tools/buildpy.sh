#!/usr/bin/env bash
# Build WastelandPython.vcxproj and keep the log in the repo's own tmp dir.
#
# The hermes scratch dir prunes aggressively -- a ~500KB MSBuild log written
# there is gone by the next tool call, so an error is only ever seen as a bare
# RC. This writes where it survives.
set -u
cd /c/dev/Wasteland || exit 2
LOG=/c/dev/Wasteland/.buildlogs/python.log
mkdir -p /c/dev/Wasteland/.buildlogs

bash tools/msb.sh WastelandPython.vcxproj > "$LOG" 2>&1
RC=$?
echo "MSBUILD_RC=$RC"

echo "--- errors (unique) ---"
grep -aE "error [CL][0-9]+" "$LOG" | sed 's/^[^(]*//' | sort -u | head -14 | cut -c1-170

echo "--- did the new source compile? ---"
grep -ac "BehaviorTreeSystem.cpp" "$LOG"

echo "--- log kept at: $LOG ($(wc -c < "$LOG") bytes) ---"
exit $RC