#!/usr/bin/env bash
# tools/ci.sh -- J.8: the local embodiment of a CI job.
#
# The plan's J.8 wants "a CI job that builds Release + Debug and runs the headless
# suites". The headless suites and the canonical module build already exist; what
# a CI job adds is ONE entry point that builds, runs, and FAILS with an exit code
# a pipeline can gate on -- a green "every suite passed" is not a CI signal until
# a non-green build exits non-zero. That is the whole job here.
#
# Exit:
#   0  everything built and every counted suite passed
#   1  the module failed to build, or any suite failed / went uncounted
#
# Overrides:
#   PYTHON   the interpreter to run the suites under (defaults to the CPython the
#            canonical module is built for). Must be a Python 3.14 ABI.
#   CONFIG   "Release" (default) or "Debug".
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

CONFIG="${CONFIG:-Release}"
PYTHON="${PYTHON:-C:/Users/rtoue/AppData/Local/Python/pythoncore-3.14-64/python.exe}"

echo "== Wasteland CI ($CONFIG) =="
echo "root:   $ROOT"
echo "python: $PYTHON"
echo

echo "== build: canonical engine module ($CONFIG) =="
if ! bash tools/buildpy-canonical.sh > ci_build.log 2>&1; then
    echo "FAIL: the engine module failed to build."
    echo "--- build log tail ---"
    tail -30 ci_build.log
    exit 1
fi
tail -2 ci_build.log
echo

echo "== run: every headless suite =="
unset WASTELAND_MODULE_DIR
if ! "$PYTHON" -u tools/run_all_suites.py; then
    echo
    echo "FAIL: one or more suites failed or went uncounted (see above)."
    exit 1
fi

echo
echo "PASS: built and every counted suite passed."
exit 0
