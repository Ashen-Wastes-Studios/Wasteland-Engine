"""Shared, repository-relative setup for the promoted Visual Blocks suites."""

from __future__ import annotations

import os
from pathlib import Path
import sys

# tests/visualblocks/_bootstrap.py -> repository root; never depend on CWD.
REPO_ROOT = Path(__file__).resolve().parents[2]
ASSET_ROOT = REPO_ROOT / "DemonCore-Editor" / "assets"
TREES = (ASSET_ROOT / "trees").resolve().as_posix()
SCRIPTS = (ASSET_ROOT / "scripts").resolve().as_posix()
GRAPHS = (ASSET_ROOT / "graphs").resolve().as_posix()


def configure() -> str:
    """Add the selected engine module and assets to sys.path, returning its dir.

    The caller-provided WASTELAND_MODULE_DIR is authoritative. Without it,
    select the newest in-repository module artifact. Scratch-only paths are
    deliberately not searched, so these canonical suites cannot depend on the
    cache that they replace.
    """
    if sys.version_info[:3] != (3, 14, 5):
        raise RuntimeError(
            "Visual Blocks regression suites require CPython 3.14.5; got %s"
            % sys.version.split()[0]
        )

    override = os.environ.get("WASTELAND_MODULE_DIR")
    if override:
        candidates = [Path(override)]
    else:
        candidates = [
            REPO_ROOT / ".buildlogs" / "linkprobe",
            REPO_ROOT / "bin" / "Release-windows-x86_64" / "DemonCore-Editor",
        ]

    candidates = [
        path for path in candidates
        if (path / "Wasteland.pyd").is_file()
    ]
    if not candidates:
        raise RuntimeError(
            "No Wasteland.pyd found. Set WASTELAND_MODULE_DIR to the module "
            "artifact under test."
        )
    module_dir = max(
        candidates,
        key=lambda path: (path / "Wasteland.pyd").stat().st_mtime,
    ).resolve().as_posix()

    # Fail setup, not a later asset check, if discovery has gone stale.
    if not (ASSET_ROOT / "trees").is_dir() or not list((ASSET_ROOT / "trees").glob("*.wlbtree")):
        raise RuntimeError("No shipped .wlbtree assets discovered under %s" % TREES)
    if not (ASSET_ROOT / "graphs").is_dir() or not list((ASSET_ROOT / "graphs").glob("*.wlblocks")):
        raise RuntimeError("No shipped .wlblocks assets discovered under %s" % GRAPHS)
    if not (ASSET_ROOT / "scripts" / "wl_behavior.py").is_file():
        raise RuntimeError("Shared behavior runtime not found under %s" % SCRIPTS)

    for path in (SCRIPTS, module_dir):
        if path not in sys.path:
            sys.path.insert(0, path)
    return module_dir
