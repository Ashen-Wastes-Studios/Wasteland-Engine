"""Every dockable panel must be docked, and nothing must float by accident.

The editor's layout system rebuilds the dock tree from a hardcoded `kPanels[]`
array plus a switch over workflows. A window that `DockBuilderDockWindow` never
sees reverts to a free-floating window on first use, and ImGui then restores it
to whatever position the widest session that ever saved the ini used. On a 4K
display a floater can end up off-screen with no way to get it back -- it is not
merely untidy, it is unreachable.

That is not hypothetical: "Clean-Room Reconstruction" is drawn at a hardcoded
(800, 800) and is NOT in `kPanels[]`. It floats today. The failure mode is
invisible to a build and invisible to a log, which is why it survived.

The property cannot be checked through the running editor from a test -- it
needs a screenshot and a cursor. It CAN be checked in the source, and the source
is where the omission happens, so that is where the guard belongs.

WHAT THIS ASSERTS

  1. every literal `ImGui::Begin("Title")` in the editor source appears in
     `kPanels[]`, unless it is on the intentional-floater allowlist;
  2. every `kPanels[]` entry actually appears as a string somewhere in the
     editor source -- a panel listed but never drawn is the other half of the
     same bug.

`##`-prefixed titles are excluded: ImGui treats the text after `##` as an ID with
an empty label, so those are child windows drawn into a parent, not dockable
panels. `ScriptEditorPanel` is exempt from the literal check because it builds
its title from a variable (`windowTitle = "Script Editor"` plus a filename); the
reverse check covers it.

The allowlist is the point. It is exactly one entry, and adding to it requires a
deliberate edit to this file -- which is the decision the audit exists to force.

Run:  python tests/test_editor_docking_audit.py
"""

from __future__ import annotations

import os
import re
import sys

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_EDITOR_SRC = os.path.join(_ROOT, "DemonCore-Editor", "src")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from runner import check, check_eq  # noqa: E402

# Windows drawn deliberately outside the dock system. Each entry needs a reason.
#
#   "DockSpace Demo"  -- ImGui's own demo window, opened from a menu item. It is
#                        a debugging aid, not a panel, and docking it would
#                        put demo widgets into the user's saved layout.
#
# Anything else here is a bug wearing an allowlist. Do not add to this list to
# make the test pass -- fix the panel instead.
INTENTIONAL_FLOATERS = {"DockSpace Demo"}

# Child-window prefixes: ImGui renders the text after "##" as the ID and shows
# an empty label, so these are drawn INSIDE a parent window and cannot be
# docked. They are not panels.
_CHILD_PREFIX = "##"


def _read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def _kpanels(source):
    """Extract the kPanels[] string literals, preserving order."""
    match = re.search(r"kPanels\[\]\s*=\s*\{(.*?)\};", source, re.DOTALL)
    if not match:
        return []
    return re.findall(r'"([^"]+)"', match.group(1))


def _begin_titles(source):
    """Every literal ImGui::Begin("Title") in a source blob."""
    return re.findall(r'ImGui::Begin\("([^"]+)"', source)


def _collect_sources():
    """All editor source files, as {relative path: text}."""
    found = {}
    for base, _dirs, files in os.walk(_EDITOR_SRC):
        for name in files:
            if name.endswith((".cpp", ".h")):
                full = os.path.join(base, name)
                rel = os.path.relpath(full, _EDITOR_SRC).replace("\\", "/")
                found[rel] = _read(full)
    return found


def main():
    if not os.path.isdir(_EDITOR_SRC):
        print("SKIP: no DemonCore-Editor/src at %s" % _EDITOR_SRC)
        return 0

    sources = _collect_sources()
    layer_path = "EditorLayer.cpp"
    if layer_path not in sources:
        print("FAIL: %s not found" % layer_path)
        return 1

    layer = sources[layer_path]
    kpanels = _kpanels(layer)

    check(len(kpanels) > 0,
          "kPanels[] must be parseable and non-empty (got %d entries)" % len(kpanels))
    check_eq(len(set(kpanels)), len(kpanels),
             "kPanels[] must not contain duplicates")

    # --- Direction 1: every drawn panel must be docked -----------------------
    #
    # Collect every literal Begin title across the whole editor source, then
    # subtract the child windows and the intentional floaters. What remains must
    # be a subset of kPanels.
    drawn = {}
    for rel, text in sources.items():
        for title in _begin_titles(text):
            drawn.setdefault(title, []).append(rel)

    child_titles = sorted(t for t in drawn if t.startswith(_CHILD_PREFIX))
    check_eq(len(child_titles), len([t for t in drawn if t.startswith(_CHILD_PREFIX)]),
             "child-window titles are identified by their '##' prefix")

    unexpected = {}
    for title, files in sorted(drawn.items()):
        if title.startswith(_CHILD_PREFIX):
            continue
        if title in INTENTIONAL_FLOATERS:
            continue
        if title not in kpanels:
            unexpected[title] = files

    check_eq(unexpected, {},
             "every drawn window must appear in kPanels[] (un-docked: %s)"
             % (", ".join("%s (%s)" % (t, ",".join(f)) for t, f in unexpected.items())
                or "none"))

    # Name the known offender explicitly when it appears, so the failure message
    # says what to do rather than just what broke.
    for title, files in sorted(unexpected.items()):
        print("  NOTE: %r is drawn in %s but never docked" % (title, ", ".join(files)))
        print("       Fix: add it to kPanels[] in %s and dock it in ApplyLayout,"
              % layer_path)
        print("       or add it to INTENTIONAL_FLOATERS here with a reason.")

    # --- Direction 2: every kPanels entry must be drawn somewhere -----------
    #
    # A panel listed in kPanels[] but never drawn is a layout that reserves space
    # for a window that does not exist -- the mirror image of the same bug, and
    # one that produces an empty dock region rather than a floater.
    #
    # "Drawn" means either a literal ImGui::Begin("Title") anywhere (Viewport,
    # Settings and Stats are drawn by EditorLayer itself, not by a panel class),
    # or the title appearing as a string in a panel source. The second case is
    # ScriptEditorPanel, which builds its title from a variable
    # (`windowTitle = "Script Editor"` plus a filename) so it has no literal
    # Begin to find.
    never_drawn = []
    for title in kpanels:
        if title in drawn:
            continue
        hits = [rel for rel, text in sources.items() if '"%s"' % title in text]
        if not hits:
            never_drawn.append(title)

    check_eq(never_drawn, [],
             "every kPanels[] entry must be drawn by a panel (never drawn: %s)"
             % (", ".join(never_drawn) or "none"))

    # --- The allowlist must stay tiny --------------------------------------
    check_eq(sorted(INTENTIONAL_FLOATERS), ["DockSpace Demo"],
             "the intentional-floater allowlist must stay at exactly one entry")

    print("")
    if _failures_report():
        return 1
    return 0


def _failures_report():
    # runner.check prints failures as they happen; this just reports the count.
    import runner
    return bool(runner._failures)


if __name__ == "__main__":
    sys.exit(main())
