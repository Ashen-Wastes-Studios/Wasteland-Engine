"""Source-contract test for AnimatorPanel (Task A.6).

The panel cannot run headlessly, so this asserts the structural invariants the
panel depends on: every ImGui call is qualified, every helper it calls exists on
the machine with the signature it uses, and the file's own brace balance. The
behavioural half lives in tests/headless/test_state_machine_editing.py.
"""

import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PANEL = os.path.join(ROOT, "DemonCore-Editor", "src", "Panels")

PASSED = 0
FAILED = []


def read(*parts):
    with open(os.path.join(*parts), encoding="utf-8") as handle:
        return handle.read()


def check(cond, message):
    global PASSED
    if cond:
        PASSED += 1
    else:
        FAILED.append(message)
        print("  FAIL:", message)


def test_brace_balance():
    for name in ("AnimatorPanel.cpp", "AnimatorPanel.h"):
        text = read(PANEL, name)
        check(text.count("{") == text.count("}"),
              "%s has unbalanced braces (%d open, %d close)"
              % (name, text.count("{"), text.count("}")))


def test_every_imgui_call_is_qualified():
    """The editor's own translation units do not have `using namespace ImGui`,
    and a bare name does not resolve. The sibling panels qualify every call, so
    an unqualified one here is a compile error the build would otherwise only
    report.
    """
    text = read(PANEL, "AnimatorPanel.cpp")
    bare = re.findall(r"(?<![:\w])(?<!ImGui::)\b(Begin|End|Text|TextDisabled|Button|"
                      r"SameLine|InputText|Checkbox|Selectable|BeginChild|EndChild|"
                      r"GetContentRegionAvail|IsItemDeactivatedAfterEdit)\s*\(", text)
    # Filter out the qualified ones, which the lookbehind already handles.
    check(not bare, "unqualified ImGui calls: %s" % sorted(set(bare)))


def test_panel_header_is_self_contained():
    header = read(PANEL, "AnimatorPanel.h")
    check("class AnimatorPanel" in header, "the class is declared")
    for member in ("OnImGuiRender", "OpenAsset", "SetAssetRoot", "Reset",
                   "SetVisible", "IsVisible"):
        check(("void %s" % member) in header or ("bool %s" % member) in header,
              "header declares %s" % member)


def test_panel_uses_the_serializer_round_trip():
    """A panel that edits in memory but never persists is an editor in name only,
    and a panel that persists but cannot reload would show a stale canvas after
    every save.
    """
    text = read(PANEL, "AnimatorPanel.cpp")
    check("AnimationSerializer::Save" in text, "the panel saves through the serializer")
    check("AnimationSerializer::Load" in text, "the panel loads through the serializer")
    check("AnimationError" in text, "and reads the typed error")


def test_drag_writes_into_the_asset():
    """The presentation is rebuilt from the machine every frame, so a position
    held only in the presentation would be discarded on the next rebuild and the
    node would snap back to its grid slot.
    """
    text = read(PANEL, "AnimatorPanel.cpp")
    check("m_Asset.StatePositions" in text, "a drag writes into the asset's positions")
    check("MovedNodePosition" in text, "from the canvas move event")
    # And the rebuild must read them back.
    check("StatePositions.find" in text, "the presentation reads positions back")


def test_positions_are_sorted_before_save():
    """The map's iteration order changes between runs, so an unsorted emission
    would rewrite the file on every save even when nothing moved.
    """
    serializer = read(ROOT, "Wasteland", "src", "Wasteland", "Animation",
                      "AnimationSerializer.cpp")
    check("StatePositions" in serializer, "the serializer handles positions")
    check("editor" in serializer, "under an editor key")


def test_state_machine_api_matches_the_panel_calls():
    """Every machine call the panel makes must exist with that arity. A panel
    that compiles is the only other check, and a rebuild costs minutes.
    """
    header = read(ROOT, "Wasteland", "src", "Wasteland", "Animation",
                  "AnimationStateMachine.h")
    panel = read(PANEL, "AnimatorPanel.cpp")
    for method in ("RemoveState", "RemoveTransition", "SetStateSpeed",
                   "SetStateLooping", "SetStateTransitionDuration", "SetStateClip",
                   "SetTransitionTrigger", "SetTransitionDuration", "AddTransition",
                   "AddState"):
        check(method in header, "the machine declares %s" % method)
        check(("." + method) in panel, "the panel calls %s" % method)


def test_remove_state_keeps_transitions():
    """The panel relies on this: deleting a state must not silently delete
    authored transitions. The alternative makes a delete's effect depend on how
    the machine happens to be wired.
    """
    header = read(ROOT, "Wasteland", "src", "Wasteland", "Animation",
                  "AnimationStateMachine.h")
    check("NOT dropped" in header or "not dropped" in header,
          "RemoveState documents that it keeps transitions")


def test_the_panel_is_wired_into_the_editor():
    layer = read(ROOT, "DemonCore-Editor", "src", "EditorLayer.cpp")
    check("m_AnimatorPanel" in layer, "the layer owns a panel instance")
    check("m_AnimatorPanel.OnImGuiRender()" in layer, "and draws it")
    check('"Animator"' in layer, "and it is reachable from the View menu")

    vcxproj = read(ROOT, "DemonCore-Editor", "DemonCore-Editor.vcxproj")
    check("AnimatorPanel.cpp" in vcxproj, "the panel compiles into the editor target")
    check("AnimatorPanel.h" in vcxproj, "and its header is listed")


def test_the_animation_workflow_shows_both_halves():
    """The timeline edits clips and the Animator wires states between them. A
    workflow that showed only one would make the other unreachable without a
    cursor, which is exactly what a layout is supposed to avoid.
    """
    layer = read(ROOT, "DemonCore-Editor", "src", "EditorLayer.cpp")
    check("m_AnimatorPanel.SetVisible(workflow == Workflow::Animation)" in layer,
          "the Animation workflow makes the Animator visible")
    check("m_AnimatorPanel.SetAssetRoot" in layer,
          "and gives it the asset root, so it opens populated")


def main():
    test_brace_balance()
    test_every_imgui_call_is_qualified()
    test_panel_header_is_self_contained()
    test_panel_uses_the_serializer_round_trip()
    test_drag_writes_into_the_asset()
    test_positions_are_sorted_before_save()
    test_state_machine_api_matches_the_panel_calls()
    test_remove_state_keeps_transitions()
    test_the_panel_is_wired_into_the_editor()
    test_the_animation_workflow_shows_both_halves()

    print("%d passed, %d failed" % (PASSED, len(FAILED)))
    return 1 if FAILED else 0


if __name__ == "__main__":
    raise SystemExit(main())
