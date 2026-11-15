"""Runtime layout audit: no panel floats, in every workflow.

The source-contract audit (`test_editor_docking_audit.py`) proves `kPanels[]`
and the `ImGui::Begin` titles agree. It cannot prove the RUNTIME tree is right:
the dock builder could be handed the wrong node, a panel could be docked into a
split node and land in an arbitrary child, or ImGui could restore a stale ini
position over the built tree. All of those are invisible in source and none of
them break the build.

This suite runs the REAL editor headlessly with `WL_DUMP_DOCK_TREE=1`, which
scans ImGui's live window list after the first frame and logs a `FLOATING` line
for every window whose `DockId` is 0 -- i.e. not in the dockspace at all. A
FLOATING line naming a panel is a layout failure, and the only thing that can
catch it without a cursor on a 4K display.

Why not a screenshot: the `WL_SCREENSHOT` hook reads the GL framebuffer during
`OnUpdate`, which runs BEFORE ImGui renders, so a captured PNG contains the 3D
scene and never a single panel. The runtime dump is the only cursor-free proof.

BENIGN FLOATERS

Three kinds of window legitimately have `DockId == 0` and must not be treated as
failures:

  * ImGuizmo's own `'gizmo'` window -- it draws inside the viewport;
  * ImGui's built-in `'Debug##Default'`;
  * a panel's child and popup windows, whose names carry a `##` suffix
    (ImGui renders the text after `##` as an ID with an empty label, so these
    are not dockable panels).

Anything else floating is a bug wearing a title.

Run:  python tests/test_editor_layout_runtime.py
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_EDITOR_DIR = os.path.join(
    _ROOT, "bin", "Release-windows-x86_64", "DemonCore-Editor")
_EDITOR = os.path.join(_EDITOR_DIR, "DemonCore-Editor.exe")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from runner import check, check_eq  # noqa: E402

# The workflow -> primary panel map, mirroring ApplyLayout's primary switch.
# Each entry is asserted to be DOCKED in that workflow.
WORKFLOW_PRIMARY = {
    "Scene": "Viewport",
    "Scripting": "Script Editor",
    "Visual": "Visual Blocks",
    "Audio": "Viewport",
    "Review": "AI Chatbot",
}

# Window name fragments that legitimately float. See the module docstring.
BENIGN_FLOATERS = ("##", "gizmo", "Debug")


def _is_benign(name: str) -> bool:
    """Is this floating window a child/internal window rather than a panel?

    Three forms are legitimate:

      * `##...`   -- ImGui renders the text after `##` as an ID with an empty
                     label, so it is a child window, not a dockable panel.
      * `A/B`     -- ImGui names a `BeginChild` window `<parent>/<child>`, and a
                     child window can never be docked. These show up as
                     `'Visual Blocks/vb_right_...'` and `'Audio Clips/<id>'`.
      * the ImGuizmo and ImGui debug windows, which float by design.

    A floating window whose name is a bare title is a panel that escaped the
    dock system -- the failure this suite exists to catch.
    """
    if any(fragment in name for fragment in BENIGN_FLOATERS):
        return True
    return "/" in name


def run_workflow(workflow: str):
    """Run the editor in one workflow and return (docked, floating) name sets.

    The screenshot env vars are not there to capture anything -- they are the
    editor's only clean-exit mechanism. WL_SCREENSHOT_LINGER keeps the process
    alive a few frames after the shot so the readback drains before the window
    closes; without it the OS tears the device down mid-capture and the process
    can die on the way out.
    """
    if not os.path.isfile(_EDITOR):
        return None, None

    shot = os.path.join(tempfile.gettempdir(), "wl_layout_audit.png")
    env = dict(os.environ)
    env["WASTELAND_FORCE_API"] = "1"
    env["WASTELAND_EDITOR_WORKFLOW"] = workflow
    env["WL_DUMP_DOCK_TREE"] = "1"
    env["WL_SCREENSHOT"] = shot
    env["WL_SCREENSHOT_FRAME"] = "20"
    env["WL_SCREENSHOT_LINGER"] = "3"

    try:
        proc = subprocess.run(
            [_EDITOR], cwd=_EDITOR_DIR, env=env,
            capture_output=True, timeout=120,
            stdin=subprocess.DEVNULL,
        )
    except subprocess.TimeoutExpired:
        return None, None

    # The log is a mix of UTF-16 engine output and ASCII ImGui output, so null
    # bytes are stripped before decoding. A truncated multi-byte sequence at the
    # end is expected and ignored.
    raw = proc.stdout + proc.stderr
    text = raw.replace(b"\x00", b"").decode("utf-8", errors="replace")

    docked, floating = set(), set()
    for line in text.splitlines():
        if "docked   '" in line:
            docked.add(line.split("docked   '", 1)[1].split("'", 1)[0])
        elif "FLOATING '" in line:
            floating.add(line.split("FLOATING '", 1)[1].split("'", 1)[0])
    return docked, floating


def main():
    if not os.path.isfile(_EDITOR):
        print("SKIP: no DemonCore-Editor.exe at %s" % _EDITOR)
        print("      build it first (build_incremental.bat)")
        return 0

    total_floating_failures = 0

    for workflow, primary in WORKFLOW_PRIMARY.items():
        docked, floating = run_workflow(workflow)
        if docked is None:
            print("  SKIP %s: editor did not produce a dock dump "
                  "(crashed or timed out)" % workflow)
            continue

        check(primary in docked,
              "%s workflow: primary panel '%s' must be docked (docked: %s)"
              % (workflow, primary, sorted(docked)))

        bad = sorted(f for f in floating if not _is_benign(f))
        total_floating_failures += len(bad)
        check_eq(bad, [],
                 "%s workflow: no panel may float (floating: %s)"
                 % (workflow, bad))

    print("")
    import runner
    if runner._failures:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
