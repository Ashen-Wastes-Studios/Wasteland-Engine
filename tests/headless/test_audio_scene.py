"""The scene-facing audio layer, and the bindings that make it reachable.

Both components were registered as py::class_ with every field readable, but no
Entity accessor was bound for either -- so a script could inspect a source it had
somehow obtained and could never attach one. Scene.PlayEntitySound and
PlayAutoStartSources were therefore unreachable from Python: the entire
scene-facing audio layer, complete and tested at the DSP level, with no way in.

The gap was invisible for two independent reasons, and both are worth recording:

  * `hasattr(e, "AddOrReplaceAudioSourceComponent")` is False, which reads as
    "this engine does not have entity audio components" rather than "nobody
    wrote the BIND_GAMEPLAY_COMP line". The classes existing made the omission
    look deliberate.
  * Binding them did not immediately work. Entity::AddComponent routes through
    Scene::OnComponentAdded<T>, a template member, and there was no explicit
    specialization for either audio type -- so the first build with the binding
    failed to LINK with two unresolved externals. Nothing instantiates that
    template for a component type until something adds it, so the missing
    definition stayed invisible until the first script could attach a source,
    which is the entire point of binding them.

And AudioSystem itself -- LoadClip, Play, PlayAt, PlayEntity, StopClip,
SetClipVolume, PlayAutoStartSources, Update, GetListenerEntity,
ResolveSourcePosition -- plus all seven Scene wrappers had no test at all. This
suite covers the bindings and the reachable-path behaviour. It does NOT claim
the DSP is covered: test_audio_clip.py and test_audio_mixer.py do that, and
neither touches this layer.

HARNESS RULE: every Scene here is held in a local for the whole test. An Entity
carries a pointer into its Scene's registry, so letting the Scene die while an
Entity survives -- `Scene().CreateEntity(...)` does exactly that, because the
Scene is unreferenced the moment the expression ends -- makes the next access
read freed storage. EnTT's response is a "Value must be a power of two"
assertion, or an outright interpreter crash, and neither points at the real
cause. This suite hit that while being written: the failure looked like a
binding defect and was entirely the test's own setup.
"""
import atexit
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runner import check, check_eq, check_near

import wlpath

wlpath.setup()

import Wasteland
from Wasteland import Scene, Vec3

# Temp directories are registered and removed at exit, so a failing
# assertion leaves its .wastescene behind for inspection instead of
# leaking it one test at a time into %%TEMP%%.
_TMP_DIRS = []


def _tmpdir():
    path = tempfile.mkdtemp(prefix="wl_audio_scene_")
    _TMP_DIRS.append(path)
    return path


atexit.register(lambda: [shutil.rmtree(d, ignore_errors=True) for d in _TMP_DIRS])


def test_the_audio_components_are_attachable_from_python():
    """The regression this suite exists for.

    A py::class_ registration is not an Entity accessor. The class existing only
    means it can be constructed and its fields read; attaching it to an entity is
    a separate binding, and it was missing.
    """
    # The scene MUST stay alive for the whole test. An Entity holds a raw
    # registry pointer, so `Scene().CreateEntity(...)` -- where the Scene is
    # garbage collected the instant the expression ends -- leaves the Entity
    # pointing into freed storage. EnTT then reads a pool whose size is no
    # longer a power of two and asserts, or simply crashes the interpreter.
    # Every other suite here returns the scene and its entities together for
    # exactly this reason.
    sc = Scene()
    e = sc.CreateEntity("probe")
    for comp in ("AudioSourceComponent", "AudioListenerComponent"):
        for prefix in ("AddOrReplace", "Add", "Get", "TryGet", "Has", "Remove"):
            check(hasattr(e, prefix + comp),
                  "Entity must expose %s%s -- without it the audio layer is "
                  "unreachable from a script" % (prefix, comp))


def test_a_source_round_trips_through_the_ecs():
    """Attaching must store real values, not accept and discard."""
    sc = Scene()
    e = sc.CreateEntity("speaker")
    src = Wasteland.AudioSourceComponent()
    src.ClipName = "probe_tone"
    src.Volume = 0.5
    src.Loop = True
    src.Pitch = 1.25
    e.AddOrReplaceAudioSourceComponent(src)

    check(e.HasAudioSourceComponent(), "the source must be found after adding")
    got = e.GetAudioSourceComponent()
    check_eq(got.ClipName, "probe_tone", "ClipName must round-trip")
    check_eq(got.Volume, 0.5, "Volume must round-trip")
    check_eq(got.Loop, True, "Loop must round-trip")
    check_eq(got.Pitch, 1.25, "Pitch must round-trip")

    # AddOrReplace must replace, not append or ignore.
    src2 = Wasteland.AudioSourceComponent()
    src2.ClipName = "replaced"
    e.AddOrReplaceAudioSourceComponent(src2)
    check_eq(e.GetAudioSourceComponent().ClipName, "replaced",
             "AddOrReplace must replace the existing value")

    e.RemoveAudioSourceComponent()
    check(not e.HasAudioSourceComponent(), "Remove must clear it")


def test_a_listener_is_attachable_and_separable():
    sc = Scene()
    e = sc.CreateEntity("ear")
    e.AddOrReplaceAudioListenerComponent(Wasteland.AudioListenerComponent())
    check(e.HasAudioListenerComponent(), "the listener must be found after adding")
    e.RemoveAudioListenerComponent()
    check(not e.HasAudioListenerComponent(), "Remove must clear it")


def test_the_scene_audio_wrappers_are_all_present():
    """Seven bound wrappers. Any one missing makes part of the layer dead."""
    sc = Scene()
    for name in ("PlaySound", "PlaySoundAt", "PlayEntitySound", "StopSound",
                 "LoadAudioClip", "UpdateAudio", "GetAudio"):
        check(hasattr(sc, name), "Scene must expose %s" % name)


def test_an_unresolvable_clip_reports_minus_one_rather_than_raising():
    """A missing clip must be survivable.

    Nothing should take the game down mid-firefight because an asset was not
    imported. The engine logs and counts; the script gets -1.
    """
    sc = Scene()
    e = sc.CreateEntity("speaker")
    src = Wasteland.AudioSourceComponent()
    src.ClipName = "definitely_not_a_real_clip"
    e.AddOrReplaceAudioSourceComponent(src)

    check_eq(sc.PlayEntitySound(e), -1,
             "an unregistered clip must return -1, not raise")
    # And an entity with no source at all.
    check_eq(sc.PlayEntitySound(sc.CreateEntity("silent")), -1,
             "an entity with no AudioSourceComponent must return -1")


def test_loading_a_nonexistent_clip_reports_false():
    sc = Scene()
    check_eq(sc.LoadAudioClip("no_such_file.wav"), False,
             "LoadAudioClip must report failure rather than raise")


def test_runtime_lifecycle_autostarts_sources_and_advances_the_mixer():
    """Runtime hooks start authored sources and advance audio with the scene."""
    sc = Scene()
    audio = sc.GetAudio()
    clip = Wasteland.Audio.AudioClip.FromSamples([0.0] * 480, 48000)
    check(clip.IsValid(), "the generated runtime probe clip must be valid")
    audio.GetMixer().RegisterClip("runtime_probe", clip)
    check(audio.GetMixer().HasClip("runtime_probe"),
          "the probe clip must register before runtime starts")

    speaker = sc.CreateEntity("runtime speaker")
    source = Wasteland.AudioSourceComponent()
    source.ClipName = "runtime_probe"
    source.PlayOnStart = True
    speaker.AddOrReplaceAudioSourceComponent(source)

    sc.OnRuntimeStart()
    check_eq(audio.GetMixer().GetActiveVoiceCount(), 1,
             "OnRuntimeStart must play every registered PlayOnStart source")

    sc.OnUpdateRuntime(0.02)
    check_eq(audio.GetMixer().GetActiveVoiceCount(), 0,
             "OnUpdateRuntime must advance and retire the 10 ms probe voice")


# --------------------------------------------------------------------------
# 9. .wastescene serialization of the audio pair.
#
# SceneSerializer had NO writer and NO reader for AudioSourceComponent or
# AudioListenerComponent, while InventoryComponent and
# ItemDefinitionComponent were both handled. Clips load by path at runtime, so
# a saved scene carried NO sound set: reopening a level lost every audio source
# on it, and nothing errored or warned -- the components were fully bound and
# fully working, only persistence was missing.
#
# The gap is invisible to any test that constructs a source in the same
# process it reads it back in, because the live component is the source of
# truth there. Every test below therefore goes through a real file and a
# FRESH scene, which is the only way to exercise the reader rather than the
# writer.
#
# HARNESS RULES (the same ones the rest of this file obeys):
#   * Every Scene is held in a local for the whole test. An Entity holds a raw
#     registry pointer, so `Scene().CreateEntity(...)` -- where the Scene is
#     collected the instant the expression ends -- leaves that pointer
#     dangling and the next read is an EnTT "Value must be a power of two"
#     assert or a crash. Each builder below RETURNS the scene so it stays
#     referenced.
#   * GetEntities() is empty in a headless scene; GetAllEntitiesWithTransform
#     is the view that enumerates.
#   * Components passed to AddOrReplace* are SNAPSHOT COPIES, so every
#     assertion re-reads off the entity via Get.
#   * Nothing calls OnRuntimeStop() (pre-existing exit-139 teardown).


def _scene() -> Scene:
    """A scene held in a LOCAL by the caller, and started.

    OnSimulationStart is what creates the Box3D world. Audio does not need
    physics, but starting means one builder serves every test here and the
    assertions are against a scene in the state the game uses.
    """
    sc = Scene()
    sc.OnSimulationStart()
    return sc


def _speaker(sc: Scene, name: str, clip: str) -> "Wasteland.Entity":
    """An entity with a fully-populated AudioSourceComponent.

    Every field is set to something NON-default. A source left at defaults
    would round-trip through a reader that drops half the fields and still
    pass, because the dropped values equal the ones written -- which is why
    the test that matters is the one with distinctive values.
    """
    e = sc.CreateEntity(name)
    src = Wasteland.AudioSourceComponent()
    src.ClipName = clip
    src.Volume = 0.375
    src.Pitch = 1.75
    src.Loop = True
    src.Priority = 7
    src.Spatial = True
    src.PositionOffset = Vec3(0.25, -0.5, 1.125)
    src.MinDistance = 2.5
    src.MaxDistance = 42.0
    # Left False deliberately -- see
    # test_play_on_start_is_not_written_and_not_restored, which pins the
    # decision rather than letting it ride on this fixture.
    src.PlayOnStart = False
    e.AddOrReplaceAudioSourceComponent(src)
    return e


def _find_by_name(sc: Scene, name: str) -> "Wasteland.Entity":
    entities = sc.GetAllEntitiesWithTransform()
    matches = [x for x in entities if x.GetName() == name]
    check_eq(len(matches), 1,
             "expected exactly one entity named %r, got %r"
             % (name, [x.GetName() for x in entities]))
    return matches[0]


def test_an_audio_source_survives_a_save_and_reload_exactly():
    """Every authored field, compared EXACTLY after a real round trip.

    Exact rather than near: these are serialized values with no physics or
    integration involved, so a tolerance would hide a genuine precision or
    truncation bug behind a green.
    """
    root = _tmpdir()
    src_sc = _scene()
    _speaker(src_sc, "speaker", "ambience_wind")

    path = os.path.join(root, "audio.wastescene")
    check(src_sc.SaveScene(path), "SaveScene must report success")

    dst = _scene()
    check(dst.LoadScene(path), "LoadScene must report success")

    back = _find_by_name(dst, "speaker")
    check(back.HasAudioSourceComponent(),
          "the reloaded entity must still carry a source -- the whole point")

    got = back.GetAudioSourceComponent()
    check_eq(got.ClipName, "ambience_wind",
             "ClipName must survive: it is the only thing identifying the sound")
    check_eq(got.Volume, 0.375, "Volume must survive exactly")
    check_eq(got.Pitch, 1.75, "Pitch must survive exactly")
    check_eq(got.Loop, True, "Loop must survive")
    check_eq(got.Priority, 7, "Priority must survive -- it is read at Play time, "
                              "so a lost value silently changes which voice wins")
    check_eq(got.Spatial, True, "Spatial must survive: a 3D source restored as 2D "
                                "plays at full volume everywhere")
    check_eq(got.MinDistance, 2.5, "MinDistance must survive exactly")
    check_eq(got.MaxDistance, 42.0, "MaxDistance must survive exactly")

    off = got.PositionOffset
    check_near(off.x, 0.25, 1e-6, "PositionOffset.x must survive exactly")
    check_near(off.y, -0.5, 1e-6, "PositionOffset.y must survive exactly")
    check_near(off.z, 1.125, 1e-6, "PositionOffset.z must survive exactly")


def test_an_audio_listener_survives_a_save_and_reload():
    """The listener's Enabled flag, set to the non-default False.

    False is the interesting value: a reader that ignored the key entirely
    would leave the component default True and pass a "does it load at all"
    check. Asserting the DISABLED state is what proves the flag was read.
    """
    root = _tmpdir()
    src_sc = _scene()
    ear = src_sc.CreateEntity("ear")
    listener = Wasteland.AudioListenerComponent()
    listener.Enabled = False
    ear.AddOrReplaceAudioListenerComponent(listener)

    path = os.path.join(root, "listener.wastescene")
    check(src_sc.SaveScene(path), "SaveScene must report success")

    dst = _scene()
    check(dst.LoadScene(path), "LoadScene must report success")

    back = _find_by_name(dst, "ear")
    check(back.HasAudioListenerComponent(),
          "the listener component must be restored, not just its flag")
    check_eq(back.GetAudioListenerComponent().Enabled, False,
             "a listener authored as DISABLED must come back disabled -- a "
             "reader that skipped the key would leave it True")


def test_both_audio_components_on_one_entity_both_survive():
    """Source and listener are independent nodes on the same entity.

    Cheap to assert and it catches a reader that consumes the node handle or
    mis-nests the second block inside the first, which a one-component test
    cannot see.
    """
    root = _tmpdir()
    src_sc = _scene()
    e = _speaker(src_sc, "both", "door_creak")
    listener = Wasteland.AudioListenerComponent()
    listener.Enabled = True
    e.AddOrReplaceAudioListenerComponent(listener)

    path = os.path.join(root, "both.wastescene")
    check(src_sc.SaveScene(path), "SaveScene must report success")

    dst = _scene()
    check(dst.LoadScene(path), "LoadScene must report success")

    back = _find_by_name(dst, "both")
    check(back.HasAudioSourceComponent(), "the source must survive alongside it")
    check(back.HasAudioListenerComponent(), "and so must the listener")
    check_eq(back.GetAudioSourceComponent().ClipName, "door_creak",
             "with its clip intact")
    check_eq(back.GetAudioListenerComponent().Enabled, True,
             "and its own flag intact")


def test_the_saved_file_actually_contains_the_audio_nodes():
    """A guard against the tests above passing for the wrong reason.

    If SaveScene silently wrote nothing, LoadScene would succeed on an empty
    scene and _find_by_name would fail loudly -- but this pins the actual
    YAML keys, which is the contract the reader depends on and the thing a
    future refactor of the writer would break.
    """
    root = _tmpdir()
    src_sc = _scene()
    _speaker(src_sc, "speaker", "rain_loop")
    ear = src_sc.CreateEntity("ear")
    ear.AddOrReplaceAudioListenerComponent(Wasteland.AudioListenerComponent())

    path = os.path.join(root, "keys.wastescene")
    check(src_sc.SaveScene(path), "SaveScene must report success")
    with open(path, "r", encoding="utf-8", newline="") as fh:
        text = fh.read()

    check("AudioSourceComponent" in text, "the file must name the source node")
    check("AudioListenerComponent" in text, "and the listener node")
    for key in ("ClipName", "Volume", "Pitch", "Loop", "Priority", "Spatial",
                "PositionOffset", "MinDistance", "MaxDistance"):
        check(key in text, "the file must contain the %s key" % key)


def test_play_on_start_is_not_written_and_not_restored():
    """The documented decision, pinned in both directions.

    PlayOnStart is an ACTION REQUEST, not state. Round-tripping it would
    replay every autostart sound on every level load -- a door closing once
    per load, looping ambience restarting on every scene swap -- which is the
    classic reason a game "sounds wrong when you reopen the editor". So the
    writer omits the key and the reader leaves the flag at its default false.

    The counterweight, stated in the C++ comment at the serialization site: an
    autostart flag authored in the editor is LOST across a reload. That is a
    real cost, accepted deliberately because the runtime save path shares this
    file and a blind round trip would replay on every load. This test is what
    makes the choice visible -- if someone changes it, this goes red.
    """
    root = _tmpdir()
    src_sc = _scene()
    e = src_sc.CreateEntity("ambience")
    src = Wasteland.AudioSourceComponent()
    src.ClipName = "cave_drip"
    src.PlayOnStart = True
    e.AddOrReplaceAudioSourceComponent(src)

    path = os.path.join(root, "autostart.wastescene")
    check(src_sc.SaveScene(path), "SaveScene must report success")
    with open(path, "r", encoding="utf-8", newline="") as fh:
        text = fh.read()
    check("PlayOnStart" not in text,
          "PlayOnStart must NOT be written -- it is an action request, and "
          "writing it makes every reload replay the level's autostart sounds")

    dst = _scene()
    check(dst.LoadScene(path), "LoadScene must report success")
    back = _find_by_name(dst, "ambience")
    check_eq(back.GetAudioSourceComponent().PlayOnStart, False,
             "a reloaded source must be silent on arrival: PlayAutoStartSources "
             "fires from OnRuntimeStart, not from Deserialize, so a load is "
             "silent by construction")

    # MinPlayTime is the other excluded runtime field: it counts the tail of a
    # sound that is CURRENTLY playing, which a scene file does not describe.
    # It is not bound to Python either -- it is runtime state with no authoring
    # surface -- so it is asserted on the FILE rather than off the component.
    check("MinPlayTime" not in text,
          "MinPlayTime must not be written -- it is the remaining seconds of a "
          "sound that is CURRENTLY playing, and carrying it would make a scene "
          "loaded mid-tail refuse to retrigger")


def test_audio_serialization_is_byte_stable_across_a_second_save():
    """save -> load -> save must produce identical bytes.

    This is the check that catches an asymmetric pair. A field written but not
    read makes the second save DIFFER (the value is gone); a field read but
    not written loses data on the very first reload. Comparing the two FILES
    rather than two component reads is what makes it a real round-trip test.
    """
    root = _tmpdir()
    src_sc = _scene()
    _speaker(src_sc, "speaker", "generator_hum")
    src_sc.CreateEntity("ear").AddOrReplaceAudioListenerComponent(
        Wasteland.AudioListenerComponent())

    first = os.path.join(root, "first.wastescene")
    second = os.path.join(root, "second.wastescene")
    check(src_sc.SaveScene(first), "the first save must succeed")

    mid = _scene()
    check(mid.LoadScene(first), "the load must succeed")
    check(mid.SaveScene(second), "the second save must succeed")

    with open(first, "r", encoding="utf-8", newline="") as fh:
        a = fh.read()
    with open(second, "r", encoding="utf-8", newline="") as fh:
        b = fh.read()

    check("AudioSourceComponent" in a,
          "setup: the first file must contain the source, or this proves nothing")

    # Compared as a MULTISET of lines, not as raw text. Entity ORDER differs
    # between the two files even for components this change never touched:
    # SceneSerializer emits entities in registry-view order, and EnTT's view
    # order after a load is not the file's order. That is pre-existing and
    # outside this task, so asserting raw equality would be asserting a
    # property the serializer does not have -- and would have gone red for a
    # reason that has nothing to do with audio.
    #
    # A multiset comparison still catches every asymmetry this test exists for:
    # a field written but not read changes a VALUE on a line, and a field read
    # but not written removes a line entirely. Neither survives a reordering.
    check(sorted(a.splitlines()) == sorted(b.splitlines()),
          "save -> load -> save must preserve the file's content exactly; a "
          "difference means the audio reader loses or invents something")


def test_a_hand_written_audio_node_missing_a_key_loads_with_defaults():
    """The presence-check contract, proven against a partial node.

    The reader checks EVERY key, unlike the ViewStateComponent block next to
    it which reads fields unconditionally and throws on a partial node. That
    difference matters: a .wastescene written before a field existed must load
    with the component default rather than fail to load at all, because clips
    have been loading by path since 14.4 and an old file must keep working.
    """
    root = _tmpdir()
    path = os.path.join(root, "partial.wastescene")
    os.makedirs(root, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(
            "Scene: PartialAudio\n"
            "Entities:\n"
            "  - Entity: 9000000000000000042\n"
            "    TagComponent:\n"
            "      Tag: speaker\n"
            "    TransformComponent:\n"
            "      Translation: [0, 0, 0]\n"
            "      Rotation: [0, 0, 0]\n"
            "      Scale: [1, 1, 1]\n"
            "    AudioSourceComponent:\n"
            "      ClipName: only_a_clip\n"
        )

    dst = _scene()
    check(dst.LoadScene(path), "a partial audio node must LOAD, not throw")

    back = _find_by_name(dst, "speaker")
    check(back.HasAudioSourceComponent(), "the component must be created")
    got = back.GetAudioSourceComponent()
    check_eq(got.ClipName, "only_a_clip", "the one present key must be read")
    # Everything absent takes the COMPONENT default, not zero-by-accident.
    check_eq(got.Volume, 1.0, "an absent Volume must take the component default of 1.0")
    check_eq(got.Pitch, 1.0, "an absent Pitch must take the component default of 1.0")
    check_eq(got.Loop, False, "an absent Loop must take the component default of False")
    check_eq(got.Spatial, False, "an absent Spatial must take the component default of False")
    check_eq(got.MinDistance, 1.0, "an absent MinDistance must take its default")
    check_eq(got.MaxDistance, 30.0, "an absent MaxDistance must take its default of 30.0")
    check_near(got.PositionOffset.x, 0.0, 1e-6,
               "an absent PositionOffset must be the zero vector, not garbage")


def test_an_entity_with_no_audio_components_saves_and_loads_unchanged():
    """The negative case: adding serialization must not invent components.

    A reader that added an AudioSourceComponent unconditionally would put an
    inert source on every entity in every scene -- an entity that plays
    nothing but still costs a mixer voice check and shows up in every
    "find all sources" query.
    """
    root = _tmpdir()
    src_sc = _scene()
    src_sc.CreateEntity("plain")

    path = os.path.join(root, "silent.wastescene")
    check(src_sc.SaveScene(path), "SaveScene must report success")
    with open(path, "r", encoding="utf-8", newline="") as fh:
        text = fh.read()
    check("AudioSourceComponent" not in text,
          "an entity with no source must not acquire one on save")
    check("AudioListenerComponent" not in text,
          "and likewise for the listener")

    dst = _scene()
    check(dst.LoadScene(path), "LoadScene must report success")
    back = _find_by_name(dst, "plain")
    check(not back.HasAudioSourceComponent(),
          "a silent entity must come back silent")
    check(not back.HasAudioListenerComponent(),
          "with no listener either")
