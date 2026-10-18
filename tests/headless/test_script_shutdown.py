"""Ensure Python-owned engine script state is released before interpreter finalization."""

from __future__ import annotations

import os
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import wlpath

wlpath.setup()

from runner import check  # noqa: E402


def test_script_engine_child_exits_cleanly_without_runtime_stop():
    """Exercise a real ScriptEngine instance in a child and require exit code 0."""
    child_code = r'''
import os, sys, tempfile
sys.path.insert(0, sys.argv[1])
import wlpath
wlpath.setup()
from Wasteland import Scene, ScriptComponent

scene = Scene()
entity = scene.CreateEntity("script-shutdown-probe")
fd, path = tempfile.mkstemp(prefix="_wl_script_shutdown_", suffix=".py", dir=sys.argv[1])
with os.fdopen(fd, "w", encoding="utf-8") as stream:
    stream.write(
        "class ShutdownProbe:\n"
        "    def __init__(self, entity): self.ready = True\n"
        "    def OnUpdateEntity(self, ts): pass\n"
    )
component = ScriptComponent()
component.ScriptPath = path
component.ScriptName = "ShutdownProbe"
entity.AddOrReplaceScriptComponent(component)
scene.OnUpdateRuntime(0.016)
assert entity.GetScriptAttribute("ready") is True, "script was not attached"
os.remove(path)
# Deliberately do not call Scene.OnRuntimeStop() or ScriptEngine_Shutdown().
# Normal Python exit must clean the engine's pybind11 caches safely.
'''
    result = subprocess.run(
        [sys.executable, "-c", child_code, _HERE],
        cwd=_HERE,
        capture_output=True,
        text=True,
        errors="replace",
    )
    check(
        result.returncode == 0,
        "a headless child with a real ScriptEngine instance must exit 0 without "
        f"manual shutdown (exit {result.returncode})\n"
        f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
    )
