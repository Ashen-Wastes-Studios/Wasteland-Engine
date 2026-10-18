#!/usr/bin/env bash
# Build DemonCore-Editor (Release/x64) and keep the log in the repo.
#
# Same rationale as buildpy.sh: the agent scratch dir prunes MSBuild logs
# before they can be read, which turns a failure into a bare RC with no
# diagnostic. Also waits out any straggling compiler first -- an orphaned
# cl.exe holding intermediates makes LTCG die with MSB6006 ("CL.exe exited
# with code N") and no error line at all.
set -u
cd /c/dev/Wasteland || exit 2
LOG=.buildlogs/editor.log
mkdir -p .buildlogs

MSB="/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe"

# Wait on cl.exe/link.exe only. MSBuild keeps idle nodeReuse workers alive
# between builds by design, so waiting on MSBuild.exe never terminates.
for _ in $(seq 1 120); do
  n=$(tasklist 2>/dev/null | grep -ciE '^cl\.exe|^link\.exe' || true)
  [ "$n" -eq 0 ] && break
  sleep 5
done

powershell -NoProfile -Command "Stop-Process -Name mspdbsrv -Force -ErrorAction SilentlyContinue" >/dev/null 2>&1 || true

"$MSB" DemonCore-Editor/DemonCore-Editor.vcxproj -p:Configuration=Release \
       -p:Platform=x64 -t:Build -v:m -m:1 -nologo > "$LOG" 2>&1
rc=$?
echo "EDITOR_RC=$rc"
grep -aE "error [CL][0-9]+|MSB6006|fatal" "$LOG" | sed 's/^[^(]*//' | sort -u | head -8 | cut -c1-170
echo "--- log: $LOG ($(wc -c < "$LOG") bytes) ---"
exit $rc
