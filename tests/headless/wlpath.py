"""Headless test bootstrap for the Wasteland engine.

Two things this must solve before any test can run:

1. The interpreter at ``pythoncore-3.14-64`` ships a ``python314._pth`` file,
   which puts CPython into *isolated* mode: ``PYTHONPATH`` is ignored and only
   the paths listed in the ``_pth`` are searched. So the engine module has to be
   injected with ``sys.path.insert`` rather than an environment variable. This is
   why every suite starts with ``import wlpath`` (or calls ``wlpath.setup()``)
   instead of relying on the environment.

2. The engine module lives in the *build output* directory, whose name depends
   on configuration. Resolve it by globbing so the same suite runs against both
   Release and Debug without editing.

Import this module before importing ``Wasteland``.
"""

from __future__ import annotations

import glob
import os
import sys

# tests/headless/ -> repository root
REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def _candidate_module_dirs() -> list[str]:
    """Every directory that might hold a built ``Wasteland.pyd``.

    ``WASTELAND_MODULE_DIR`` wins outright. It exists because the normal output
    path is often LOCKED for the whole session: the hermes execute_code kernel
    imports ``Wasteland.pyd`` at start-up and holds it open, so ``tools/buildpy.sh``
    fails at the very last step with ``LNK1104`` and **no** ``error C`` lines --
    every translation unit compiled fine and only the final write was refused.

    Rebuilding into a scratch directory (``tools/buildpy-probe.sh``) sidesteps
    it, and this variable is how a test suite then loads that fresh module
    instead of the stale locked one. Preferring the override keeps
    "the artifact I just built" and "the artifact under test" the same file.
    """
    override = os.environ.get("WASTELAND_MODULE_DIR")
    if override and os.path.isdir(override):
        return [override]

    patterns = [
        os.path.join(REPO_ROOT, "bin", "*", "DemonCore-Editor"),
        os.path.join(REPO_ROOT, "bin", "*", "Wasteland"),
        # Probe output from tools/buildpy-probe.sh, used when the normal
        # directory has no module at all.
        os.path.join(REPO_ROOT, ".buildlogs", "linkprobe"),
    ]
    found: list[str] = []
    for pattern in patterns:
        found.extend(p for p in glob.glob(pattern) if os.path.isdir(p))
    return sorted(found)


def module_dirs() -> list[str]:
    return _candidate_module_dirs()


def setup(prefer: str | None = None) -> str:
    """Put a built engine module on ``sys.path`` and return the one chosen.

    ``prefer`` filters candidates by a case-insensitive substring, e.g.
    ``prefer="Release"``. Release is preferred by default because the project
    verifies against it: a Debug build has different floating-point behaviour
    and its assertions on tight tolerances are not meaningful.
    """
    # Headless tests must never open the user's physical audio endpoint. The
    # real-time device path has its own native smoke test; this suite exercises
    # deterministic mixer advancement instead.
    os.environ["WASTELAND_DISABLE_AUDIO_OUTPUT"] = "1"

    candidates = _candidate_module_dirs()
    if prefer:
        matching = [d for d in candidates if prefer.lower() in d.lower()]
        if matching:
            candidates = matching
    else:
        # Default preference order, most preferred first.
        for tag in ("Release", "RelWithDebInfo", "Debug"):
            matching = [d for d in candidates if tag.lower() in d.lower()]
            if matching:
                candidates = matching
                break

    if not candidates:
        raise RuntimeError(
            "No built Wasteland module found. Expected something like\n"
            "  bin/<config>-windows-x86_64/DemonCore-Editor/Wasteland.pyd\n"
            "Build the WastelandPython project first."
        )

    for directory in candidates:
        if directory not in sys.path:
            sys.path.insert(0, directory)

    import Wasteland  # noqa: F401  (import is the assertion)

    return candidates[0]


def load():
    """Import and return the engine module, setting the path up if needed."""
    setup()
    import Wasteland

    return Wasteland


# Allow `python wlpath.py` as a smoke check.
if __name__ == "__main__":
    chosen = setup()
    import Wasteland

    print("repo root :", REPO_ROOT)
    print("module dir:", chosen)
    print("python    :", sys.version.split()[0])
    print("Wasteland : ok")
    print("Audio     :", sorted(n for n in dir(Wasteland.Audio) if not n.startswith("_")))
