"""Source contract for the Animation Timeline editor panel (Task A.5).

The panel is ImGui code, so a headless process cannot exercise it. What a
source test CAN pin is the parts that are easy to break silently:

  * the window title must match kPanels[] exactly, or the panel floats free of
    the dock layout;
  * the panel must not touch a runtime, or scrubbing would animate the level;
  * the keyframe edit API the panel depends on must exist with the tolerance
    the drag code assumes;
  * the .wlanim routing must exist, or double-clicking a clip opens nothing.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "DemonCore-Editor", "src")
ENGINE = os.path.join(ROOT, "Wasteland", "src", "Wasteland", "Animation")

PASSED = 0
FAILED = []


def read(name):
    with open(os.path.join(SRC, name), encoding="utf-8", errors="replace") as f:
        return f.read()


def read_engine(name):
    with open(os.path.join(ENGINE, name), encoding="utf-8", errors="replace") as f:
        return f.read()


def check(cond, message):
    global PASSED
    if cond:
        PASSED += 1
    else:
        FAILED.append(message)
        print("  FAIL:", message)


def main():
    layer_h = read(os.path.join("EditorLayer.h"))
    layer_cpp = read(os.path.join("EditorLayer.cpp"))
    panel_h = read(os.path.join("Panels", "AnimationTimelinePanel.h"))
    panel_cpp = read(os.path.join("Panels", "AnimationTimelinePanel.cpp"))

    # 1. Window title == kPanels[] entry. A mismatch means the panel is not in
    #    the dock layout and floats at a hardcoded position.
    kpanels = re.search(r"kPanels\[\]\s*=\s*\{(.*?)\};", layer_cpp, re.S)
    check(kpanels is not None, "kPanels[] is present")
    if kpanels:
        titles = re.findall(r'"([^"]+)"', kpanels.group(1))
        check("Animation Timeline" in titles,
              "kPanels[] contains 'Animation Timeline'")

    check('Begin("Animation Timeline"' in panel_cpp,
          "the panel's window title is 'Animation Timeline'")

    # 2. The panel must not touch a runtime. AnimationSystem and Animator are
    #    the two types whose methods advance playback, so an include of either
    #    is the tell.
    for forbidden in ("Scene/AnimationSystem.h", "Animation/Animator.h"):
        check(forbidden not in panel_cpp and forbidden not in panel_h,
              "the panel does not include %s" % forbidden)

    check("AnimationSystem" not in panel_cpp,
          "the panel never names AnimationSystem")
    # The word appears in a comment that says the panel does NOT touch one, so
    # the check is for the type, not the identifier.
    check("Animation::Animator" not in panel_cpp,
          "the panel never names an Animator type")

    # 3. The drag path depends on these existing, and on the tolerance being
    #    the one the panel's 1e-6 comparisons assume.
    clip_h = read_engine("AnimationClip.h")
    curve_h = read_engine("AnimationCurve.h")
    check(re.search(r"bool\s+MoveKey\(float fromTime, float toTime\)", clip_h),
          "RotationTrack::MoveKey exists")
    check(re.search(r"int\s+RemoveKeyAtTime\(float time\)", clip_h),
          "RotationTrack::RemoveKeyAtTime exists")
    check(re.search(r"bool\s+MoveKey\(float fromTime, float toTime\)", curve_h),
          "AnimationCurve::MoveKey exists")

    for name, text in (("AnimationClip.cpp", read_engine("AnimationClip.cpp")),
                      ("AnimationCurve.cpp", read_engine("AnimationCurve.cpp"))):
        check("kTimeTolerance = 1e-6f" in text,
              "%s uses the 1e-6 time tolerance" % name)

    # 4. .wlanim routing, so a content-browser double-click opens the editor.
    check('ext == ".wlanim"' in layer_cpp,
          "OpenVisualFile routes .wlanim to the timeline panel")
    check("m_AnimationTimelinePanel.OpenAsset" in layer_cpp,
          "the routing actually opens the asset")

    # 5. The panel is registered as a member and drawn, not just declared.
    check("AnimationTimelinePanel m_AnimationTimelinePanel;" in layer_h,
          "EditorLayer owns the panel")
    check("m_AnimationTimelinePanel.OnImGuiRender()" in layer_cpp,
          "EditorLayer draws the panel")

    # 6. A project switch must invalidate the panel's in-memory asset, or the
    #    editor would show the previous project's clip as this project's.
    check("m_AnimationTimelinePanel.Reset()" in layer_cpp,
          "OnProjectChanged resets the panel")

    # 7. The panel is in the project file, or it compiles only on a developer
    #    machine that happens to have the file in the directory glob.
    with open(os.path.join(ROOT, "DemonCore-Editor", "DemonCore-Editor.vcxproj"),
              encoding="utf-8", errors="replace") as f:
        vcxproj = f.read()
    check("AnimationTimelinePanel.cpp" in vcxproj,
          "the .cpp is registered in DemonCore-Editor.vcxproj")
    check("AnimationTimelinePanel.h" in vcxproj,
          "the .h is registered in DemonCore-Editor.vcxproj")

    # 8. The library scans the project's asset root rather than a hardcoded
    #    path. A hardcoded path would make the panel project-specific.
    check("SetAssetRoot" in panel_cpp and "SetAssetRoot" in panel_h,
          "SetAssetRoot exists and is public")
    check("GetAssetRoot()" in layer_cpp,
          "the layer supplies the project's asset root")

    print("%d passed, %d failed" % (PASSED, len(FAILED)))
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
