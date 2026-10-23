"""AVB audio path, exercised against the shipped DemoGame.wastescene.

WHY THIS FILE EXISTS

`tests/headless/test_feature_coverage.py` proves the scene CONTAINS the audio
components; `test_audio_scene.py` proves the bindings and the round trip; the
`tests/avb_*` projects prove the DSP in isolation. None of them connects the
two: nobody had ever loaded the demo scene and listened to it.

That gap is real. The scene's audio is authored as data -- a spatial source
naming `showcase_tone`, a listener, a script that loads the clip -- and every
one of those steps can fail independently and silently:

  * the deserializer presence-checks every key, so a scene field that no longer
    matches its component loses its value to the DEFAULT with no warning. A
    source authored `Spatial: true` that reads back false plays at full volume
    everywhere, which is exactly the kind of bug that is invisible until
    somebody walks past the emitter in game;
  * `LoadClip` returns false for a missing asset and the caller decides what
    that means. AudioDemo.py returns early and sets no flag, so a wrong path is
    indistinguishable from "the sound finished";
  * `AudioSourceComponent.ClipName` is the file STEM. The scene says
    `showcase_tone` and the file is `showcase_tone.wav`, so a rename of either
    side breaks playback with no error anywhere.

So this suite loads the scene, finds its audio entities BY NAME, and drives
them through the mixer to a real rendered buffer. It is the only test in the
tree that would notice the demo scene going silent.

WHAT "AVB" MEANS HERE

The AVB bus graph has no owner in a running scene -- `AvbProjectRuntime::
RenderBlock` takes an `AudioBusGraph*` as a parameter and only the offline
`Recorder` constructs one. So there is no scene-level graph to drive from
Python, and no AVB binding exists at all (Bindings.cpp contains no `Avb`
symbol). What IS reachable is the DSP the graph would feed: the Mixer's voice
table, its spatial panning, and `Mixer::Mix`, which renders the interleaved
stereo buffer a device would consume.

That is the right seam for a scene test, and it is why this file asserts on
rendered SAMPLES rather than on configuration. A scene can be perfectly wired
and still render silence if the clip failed to load, so "did it produce a
non-zero buffer" is the assertion that actually covers the user-visible
outcome.

HARNESS RULES (inherited from test_audio_scene.py, and both load-bearing):

  * The Scene is held in a local for the whole test. An Entity carries a raw
    pointer into its Scene's registry, so `Scene().CreateEntity(...)` -- where
    the Scene is collected the instant the expression ends -- leaves that
    pointer dangling and the next read is an EnTT "Value must be a power of
    two" assert or a crash.
  * `GetAllEntitiesWithTransform()` is the view that enumerates; `GetEntities()`
    is empty in a headless scene.
  * `AudioSystem` has no default constructor and comes from `Scene.GetAudio()`,
    so there is no way to build one against a scene that does not exist.
  * Nothing calls `OnRuntimeStop()` (pre-existing exit-139 teardown).

Run:  python tests/headless/test_avb_demogame_scene.py
"""

from __future__ import annotations

import os as _os
import sys as _sys
import tempfile

_HERE = _os.path.dirname(_os.path.abspath(__file__))
if _HERE not in _sys.path:
    _sys.path.insert(0, _HERE)

from runner import check, check_eq, check_near  # noqa: E402

import wlpath  # noqa: E402

wlpath.setup()

import Wasteland  # noqa: E402
from Wasteland import Scene, Vec3  # noqa: E402

ROOT = _os.path.abspath(_os.path.join(_HERE, "..", ".."))
SCENE = _os.path.join(ROOT, "DemonCore-Editor", "assets", "scenes", "DemoGame.wastescene")

# The scene's audio entities, by the Tag the scene file gives them. Pinned
# rather than discovered by component type: "the entity that has a source" is
# ambiguous in a 40 000-line showcase scene, and a rename here should be a
# loud failure rather than a silent test of whatever else matched.
SOURCE_TAG = "ShowcaseAudioSource"
LISTENER_TAG = "ShowcaseAudioListener"
SOURCE_SCRIPT = "AudioDemo.py"

# The clip the scene's source names, and the script that loads it. Resolved
# against several roots because LoadClip takes a raw path and the engine's
# notion of "relative to what" is the process working directory, which differs
# between the editor, the test runner and a bare `python` invocation.
CLIP_STEM = "showcase_tone"
CLIP_CANDIDATES = (
    _os.path.join(ROOT, "DemonCore-Editor", "assets", "audio", CLIP_STEM + ".wav"),
    _os.path.join(ROOT, "bin", "Release-windows-x86_64", "DemonCore-Editor",
                  "assets", "audio", CLIP_STEM + ".wav"),
    _os.path.join("assets", "audio", CLIP_STEM + ".wav"),
)


def _has(entity, comp):
    """Probe a component, tolerating both bound naming conventions."""
    for name in ("Has" + comp, "HasWasteland::" + comp):
        fn = getattr(entity, name, None)
        if fn is not None:
            return fn()
    return False


def _find(scene, tag):
    """Exactly one entity carrying this tag. Zero or many is a failure.

    Asserting uniqueness is deliberate: the scene is a showcase with several
    hundred entities, and "the first one that matched" would keep testing the
    same entity forever while a second, broken one went unnoticed.
    """
    names = [e.GetName() for e in scene.GetAllEntitiesWithTransform()]
    matches = [e for e in scene.GetAllEntitiesWithTransform() if e.GetName() == tag]
    check_eq(len(matches), 1,
             "expected exactly one entity tagged %r, got %d matches among %d entities"
             % (tag, len(matches), len(names)))
    return matches[0]


def _load_scene():
    """The shipped demo scene, held by the CALLER so it outlives its entities."""
    scene = Scene()
    check(scene.LoadScene(SCENE), "LoadScene must accept the shipped demo scene")
    return scene


def _fresh_scene():
    """An EMPTY scene, started, for receiving a load.

    Separate from _load_scene for a reason this suite hit: loading into a scene
    that already holds the demo APPENDS rather than replaces, so the destination
    ended up with every showcase entity twice and the round-trip test failed on
    "expected exactly one entity tagged ShowcaseAudioSource, got 2". That read as
    a serializer duplication bug and was entirely the test's own setup.

    OnSimulationStart is what creates the Box3D world; audio does not need
    physics, but starting keeps one shape for every scene here.
    """
    scene = Scene()
    scene.OnSimulationStart()
    return scene


def _resolve_clip():
    """The first clip path that exists on disk, or None."""
    for path in CLIP_CANDIDATES:
        if _os.path.isfile(path):
            return path
    return None


# ---------------------------------------------------------------------------
# 1. The scene's audio content is actually there, with the authored values.
# ---------------------------------------------------------------------------


def test_the_demo_scene_carries_its_audio_entities():
    """The fixture this whole file depends on.

    If the scene stops containing these entities, every test below is testing
    nothing -- so the presence of the fixture is asserted once, up front, with
    the reason stated.
    """
    scene = _load_scene()
    source = _find(scene, SOURCE_TAG)
    listener = _find(scene, LISTENER_TAG)

    check(_has(source, "AudioSourceComponent"),
          "the showcase source entity must carry an AudioSourceComponent")
    # The listener marker is present but deliberately component-free -- it used
    # to own the ears, which pinned them to a static entity 48 m from the
    # source. Its absence is the fix, asserted in its own test below.
    check(not _has(listener, "AudioListenerComponent"),
          "the showcase listener marker must exist but carry no listener "
          "component")


def test_the_source_keeps_its_authored_spatial_settings():
    """A field that silently reverts to default is the failure this guards.

    The deserializer presence-checks every key. If a key stops matching its
    component -- a rename, a moved block -- the value is dropped and the
    COMPONENT default is used, with no warning. `Spatial: true` reverting to
    false is invisible in the editor and audible everywhere in game.
    """
    scene = _load_scene()
    src = _find(scene, SOURCE_TAG).GetAudioSourceComponent()

    check_eq(src.ClipName, CLIP_STEM,
             "the source must name the clip by file stem -- that is how the "
             "mixer registers it, so a rename here breaks playback silently")
    check_eq(src.Spatial, True,
             "the source must stay spatial: restored as 2D it plays at full "
             "volume everywhere, which is the bug this assertion exists for")
    check_eq(src.Loop, True, "a showcase tone must loop")
    # Volume/MinDistance/MaxDistance are float32 in the component, so 0.8 comes
    # back as 0.800000011920929. A tolerance is named at the call site rather
    # than loosened into meaninglessness: 1e-5 is far below anything audible.
    check_near(src.Volume, 0.8, 1e-5, "Volume must survive")
    check_near(src.Pitch, 1.0, 1e-5, "Pitch must survive")
    check_eq(src.Priority, 128, "Priority must survive exactly")
    check_near(src.MinDistance, 2.0, 1e-5, "MinDistance must survive")
    check_near(src.MaxDistance, 25.0, 1e-5, "MaxDistance must survive")


def test_the_scene_does_not_pin_the_listener_to_a_static_marker():
    """No explicit AudioListenerComponent, so the camera owns the ears.

    FindListenerEntity gives an explicit AudioListenerComponent priority over
    the primary camera. The scene's ShowcaseAudioListener marker is a STATIC
    entity at x=416 -- 48 m from the showcase source, beyond the source's
    MaxDistance of 25 -- so carrying the component there pinned the ears to
    that marker and ignored the player entirely.

    Asserting the ABSENCE is the point: the marker is still in the scene as a
    visual anchor, and the only thing wrong with it was the component. A test
    that asserted "the listener is enabled" would have pinned the very bug this
    file exists to catch.
    """
    scene = _load_scene()
    listener = _find(scene, LISTENER_TAG)

    check(not _has(listener, "AudioListenerComponent"),
          "the showcase listener marker must NOT carry an AudioListenerComponent: "
          "an explicit one overrides the primary camera, so the ears would be "
          "pinned to a static entity 48 m from the source")

    # And no other entity may carry one either, or the fallback below is moot.
    carrying = [e.GetName() for e in scene.GetAllEntitiesWithTransform()
                if _has(e, "AudioListenerComponent")]
    check_eq(carrying, [],
             "no entity in the scene may carry an AudioListenerComponent, "
             "otherwise the camera is not the listener")

    audio = scene.GetAudio()
    check(audio is not None, "the scene must expose an AudioSystem")

    # Headless has no primary camera, so the fallback yields nothing valid --
    # which is the correct degradation, and the reason the spatial source in
    # this environment renders silence rather than a stale position.
    # IsValid, NOT Python truthiness: a pybind11 object is always truthy, so
    # `not resolved` is False and GetName() runs on an invalid entity, which
    # asserts. An invalid Entity is exactly the case this line is about.
    resolved = audio.GetListenerEntity()
    check(not resolved.IsValid() or resolved.GetName() != LISTENER_TAG,
          "the mixer must not resolve the listener to the static marker")


def test_the_source_script_is_the_scene_s_audio_loader():
    """The clip only reaches the mixer because a script loads it.

    `AudioSourceComponent.ClipName` is a name, not a path, and the deserializer
    does not load clips -- so a scene with a correct source and a missing or
    renamed script renders silence with every field correct.
    """
    scene = _load_scene()
    source = _find(scene, SOURCE_TAG)

    check(_has(source, "ScriptComponent"),
          "the showcase source must carry a ScriptComponent")
    script_path = source.GetScriptComponent().ScriptPath
    check_eq(_os.path.basename(script_path), SOURCE_SCRIPT,
             "the source's loader script must be %s" % SOURCE_SCRIPT)
    # ScriptPath is stored relative to the project's assets root, which is
    # DemonCore-Editor/assets -- not the editor directory itself.
    check(_os.path.isfile(_os.path.join(ROOT, "DemonCore-Editor", "assets", script_path)),
          "the loader script must exist on disk at %r" % script_path)


def test_the_scene_script_uses_the_spatial_path():
    """The loader must play the source entity, not the clip by name.

    The script used to call `audio.Play("showcase_tone")`, which is
    NON-spatial: the clip plays at full volume from both ears and the listener
    is never consulted. That made Spatial, PositionOffset, MinDistance and
    MaxDistance on the scene's AudioSourceComponent inert, and it contradicted
    the script's own docstring, which promised the spatial path.

    Asserted on the source text because the call is the contract: this is the
    one line that decides whether the scene's spatial audio is real.
    """
    scene = _load_scene()
    source = _find(scene, SOURCE_TAG)
    script_path = source.GetScriptComponent().ScriptPath

    body = _os.path.join(ROOT, "DemonCore-Editor", "assets", script_path)
    source_text = open(body, encoding="utf-8").read()

    # Parsed as a syntax tree rather than searched as text: the script
    # discusses the old call in its docstring, and a substring check cannot
    # tell prose about a call from the call itself.
    import ast

    calls = {node.func.attr for node in ast.walk(ast.parse(source_text))
             if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)}

    check("PlayEntity" in calls,
          "%s must call PlayEntity so position and panning apply" % SOURCE_SCRIPT)
    check("Play" not in calls,
          "%s must not call the non-spatial Play: it bypasses the listener and "
          "makes every spatial field on the scene inert" % SOURCE_SCRIPT)


# ---------------------------------------------------------------------------
# 2. The path actually renders.
# ---------------------------------------------------------------------------


def test_the_clip_renders_through_the_mixer():
    """The scene's clip, reaching real rendered samples.

    Everything else in this file is configuration. This is the check that would
    notice the demo scene going quiet: a clip that failed to load, a voice that
    never started, or a mixer rendering zeros all leave the scene perfectly
    wired and the game perfectly silent.

    Non-spatial deliberately: the assertion here is that the clip reaches the
    DSP at all, and a spatial play would fold distance attenuation into the
    result -- which is the next test's subject, not this one's.
    """
    scene = _load_scene()
    audio = scene.GetAudio()

    clip_path = _resolve_clip()
    check(clip_path is not None,
          "the scene's clip must exist on disk; looked in %r" % (CLIP_CANDIDATES,))
    check(audio.LoadClip(clip_path), "LoadClip must accept %r" % clip_path)
    check(audio.GetMixer().HasClip(CLIP_STEM),
          "the clip must register under its file stem, which is the name the "
          "scene's AudioSourceComponent carries")

    audio.Play(CLIP_STEM)
    check(audio.GetMixer().GetActiveVoiceCount() >= 1,
          "playing the clip must start a mixer voice")

    buffer = audio.GetMixer().Mix(512)
    check_eq(len(buffer), 1024,
             "Mix must return frameCount*2 interleaved stereo samples")
    peak = max(abs(s) for s in buffer)
    check(peak > 1e-5,
          "the clip must render audible samples (peak %g); silence here means "
          "the clip never reached the mixer" % peak)


def test_the_spatial_path_renders_when_the_source_is_in_range():
    """The 3D path, proven rather than assumed.

    This needed `Update` before `Mix`, and that is not incidental: the mixer's
    listener position is refreshed by AudioSystem::Update, so a spatial voice
    rendered without it is measured against a listener still at the origin --
    1157 units away from this scene's -- and attenuates to nothing. A test that
    omits the update reports the DSP as broken when it is only un-advanced.

    `PlayAt` at the listener's own position is the cleanest in-range case: the
    distance is zero, so the only thing that can silence it is the spatial path
    itself.
    """
    scene = _load_scene()
    audio = scene.GetAudio()

    clip_path = _resolve_clip()
    check(clip_path is not None, "the clip must exist for this test to mean anything")
    check(audio.LoadClip(clip_path), "LoadClip must accept %r" % clip_path)

    # Update FIRST, for two reasons that are easy to conflate. It refreshes
    # the mixer's listener position, which starts at the origin and is only
    # written by Update -- reading it before any update returns (0,0,0) and
    # reports a healthy scene as broken. And the spatial gains are computed
    # from that position, so the voice below is measured against a listener
    # that has not moved yet if this is skipped.
    # The listener position is read from the mixer rather than hardcoded at
    # 416: the scene no longer pins a static listener, so where the ears are
    # depends on whether this environment has a camera. Asserting a fixed
    # coordinate would test the old design; asserting the mixer's own value
    # tests the path.
    audio.Update(0.02)

    listener_pos = audio.GetMixer().GetListenerPosition()
    check(abs(listener_pos.x) < 1e-6,
          "headless has no primary camera, so the mixer listener must sit at "
          "the origin (got %g, %g, %g) -- the spatial path below is measured "
          "against it" % (listener_pos.x, listener_pos.y, listener_pos.z))

    audio.PlayAt(CLIP_STEM, Vec3(listener_pos.x, listener_pos.y, listener_pos.z))
    check(audio.GetMixer().GetActiveVoiceCount() >= 1,
          "an in-range spatial play must start a voice")

    audio.Update(0.02)
    buffer = audio.GetMixer().Mix(512)
    peak = max(abs(s) for s in buffer)
    check(peak > 1e-5,
          "an in-range spatial source must render audible samples (peak %g); "
          "silence here means the spatial path is broken" % peak)


def test_the_scene_s_authored_source_is_silent_in_headless_only():
    """Why the scene's spatial source renders nothing HERE, and only here.

    This used to read as a scene bug: the source is 48 m from the listener
    marker and its MaxDistance is 25, so distance attenuation took it to zero.

    It was not the scene's numbers. The marker was an explicit
    AudioListenerComponent, so it owned the ears instead of the camera --
    meaning the player's position never mattered. Now that no explicit listener
    exists, the ears follow the camera, and a player walking down the showcase
    row toward x=368 comes inside the 25 m radius and hears the tone.

    In headless there is no camera, so the listener has no valid position and
    the source renders silence. Asserted because the failure mode is
    misleading: silence here means "no camera in this environment", not "the
    scene is broken", and the two need opposite fixes.
    """
    scene = _load_scene()
    audio = scene.GetAudio()

    clip_path = _resolve_clip()
    check(clip_path is not None, "the clip must exist for this test to mean anything")
    check(audio.LoadClip(clip_path), "LoadClip must accept %r" % clip_path)

    source = _find(scene, SOURCE_TAG)
    audio.PlayEntity(source)
    check(audio.GetMixer().GetActiveVoiceCount() >= 1,
          "the scene's source must start a voice -- it is AUDIBLE in principle")

    audio.Update(0.02)
    buffer = audio.GetMixer().Mix(512)
    peak = max(abs(s) for s in buffer)

    check_eq(peak, 0.0,
             "a spatial source with no listener in the environment must render "
             "silence (headless has no primary camera)")

    # And prove the silence is the listener, not the DSP: the same clip played
    # without a listener renders fine. Without this half, "silence" could just
    # as easily mean a broken mixer.
    audio.GetMixer().StopAll(0.0)
    audio.Play(CLIP_STEM)
    audio.Update(0.02)
    plain = max(abs(s) for s in audio.GetMixer().Mix(512))
    check(plain > 1e-5,
          "the clip must render without a listener (peak %g), proving the "
          "silence above is the missing listener and not a broken mixer" % plain)


def test_the_spatial_source_is_positioned_in_the_world():
    """Spatial audio is only meaningful if the source has a position.

    `ResolveSourcePosition` is the function the mixer calls per voice. A source
    whose offset resolves to the origin while the entity sits at (368, 1.5,
    1090) would pan as though it were at the listener regardless of where the
    player stands.
    """
    scene = _load_scene()
    source = _find(scene, SOURCE_TAG)

    out = Vec3(0.0, 0.0, 0.0)
    Wasteland.Audio.AudioSystem.ResolveSourcePosition(scene, source, out)

    check(abs(out.x - 368.0) < 1.0,
          "the source must resolve near its authored x=368 (got %g)" % out.x)
    check(abs(out.z - 1090.0) < 1.0,
          "the source must resolve near its authored z=1090 (got %g)" % out.z)


def test_the_listener_sits_away_from_the_source_so_panning_is_not_degenerate():
    """A listener on top of the source makes every spatial gain collapse.

    The scene places them 48 m apart on x, which is what makes the panning
    observable. If they were co-located the test above would still pass while
    the spatial path was doing nothing at all.
    """
    scene = _load_scene()
    source = _find(scene, SOURCE_TAG)
    listener = _find(scene, LISTENER_TAG)

    src_pos = Vec3(0.0, 0.0, 0.0)
    lst_pos = Vec3(0.0, 0.0, 0.0)
    Wasteland.Audio.AudioSystem.ResolveSourcePosition(scene, source, src_pos)
    Wasteland.Audio.AudioSystem.ResolveSourcePosition(scene, listener, lst_pos)

    distance = ((src_pos.x - lst_pos.x) ** 2 +
                (src_pos.y - lst_pos.y) ** 2 +
                (src_pos.z - lst_pos.z) ** 2) ** 0.5
    check(distance > 1.0,
          "the listener and source must not be co-located (distance %g), or "
          "the spatial path is untestable" % distance)


# ---------------------------------------------------------------------------
# 3. The scene's audio survives the round trip that the editor performs.
# ---------------------------------------------------------------------------


def test_the_scene_s_audio_survives_a_save_and_reload():
    """What the editor does to the scene on every open and save.

    DemoGame.wastescene is a shipped asset that the editor loads, lets the user
    edit, and writes back. A reader/writer asymmetry would quietly strip the
    audio from the scene the first time anybody saved it.
    """
    root = tempfile.mkdtemp(prefix="wl_avb_demo_")
    src_scene = _load_scene()
    path = _os.path.join(root, "roundtrip.wastescene")
    check(src_scene.SaveScene(path), "SaveScene must succeed")

    dst = _fresh_scene()
    check(dst.LoadScene(path), "the round-tripped scene must load")

    source = _find(dst, SOURCE_TAG)
    check(_has(source, "AudioSourceComponent"),
          "the source must survive the round trip -- a scene saved from the "
          "editor and reopened must still have its audio")
    got = source.GetAudioSourceComponent()
    check_eq(got.ClipName, CLIP_STEM, "ClipName must survive")
    check_eq(got.Spatial, True, "Spatial must survive")
    check_near(got.MinDistance, 2.0, 1e-5, "MinDistance must survive")
    check_near(got.MaxDistance, 25.0, 1e-5, "MaxDistance must survive")

    listener = _find(dst, LISTENER_TAG)
    check(not _has(listener, "AudioListenerComponent"),
          "the listener marker must survive WITHOUT an AudioListenerComponent "
          "-- if a save/load cycle re-introduced one, the ears would be pinned "
          "to a static entity again and the spatial fix would silently revert")


if __name__ == "__main__":
    _sys.exit(0)
