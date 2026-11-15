"""Audio graphs: a .wlavb attached to an entity and triggered by gameplay.

Phase E of the parity plan. The plan's verification is explicit and is the spine
of this file:

    "a .wlavb that plays a tone on a trigger; attach, dispatch, assert the
     mixer's active voice count rises and falls. Read the count, not 'did not
     throw'."

So every claim here is read from `Mixer.GetActiveVoiceCount` (through
`AudioSystem.GetGraphVoiceCount`, which counts the same thing but only for
graph voices). A test that asserted "TriggerGraph returned True" would pass on a
graph that compiled, attached and then produced no sound at all.

What the pieces are, and why they are split the way they are:

  * `Mixer::ProceduralVoice` -- a voice whose samples come from a callback. The
    ONLY way a graph reaches the mixer. Without it a graph would have to bypass
    the voice cap, the priority tiers and the steal rules, so a busy firefight
    could not silence it and the cap would be decorative.
  * `AudioGraphComponent` -- pure DATA: an asset path, an event->pin map, and a
    duration. The runtime lives in AudioSystem because a compiled graph is
    neither copyable nor serializable and the chunk store copies components.
  * `AudioSystem::m_Graphs` -- the runtime, keyed by entity id.

Screenshot verification: none. The editor cannot start on this machine (DX12
device removed, 0x887a0006), so no graph has been seen in the editor's audio
panel. Every claim here is from the headless mixer.
"""
import atexit
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runner import check, check_eq

import wlpath

wlpath.setup()

import Wasteland
from Wasteland import Scene

_TMP_DIRS = []


def _cleanup():
    for path in _TMP_DIRS:
        shutil.rmtree(path, ignore_errors=True)


atexit.register(_cleanup)


def _tempdir():
    path = tempfile.mkdtemp(prefix="wasteland_audio_graph_")
    _TMP_DIRS.append(path)
    return path


# ---------------------------------------------------------------------------
# fixtures
# ---------------------------------------------------------------------------

# A graph that makes a tone. The chain Sine -> Gain -> Output runs as soon as
# the voice starts, so "the voice is audible" is a property of the graph and not
# of any trigger routing.
#
# `n_start` (TriggerOnce) has an `In` pin of type Trigger, which is what
# QueueTrigger requires -- it rejects a pin that does not exist or is not a
# Trigger pin, returning false. So the trigger map below names a REAL pin.
TONE_GRAPH = """\
schema: 1
name: "TestTone"
output: Mono
inputs: []
variables: []
nodes:
  - id: "n_start"
    type: "TriggerOnce"
    title: "Start"
    literals: []
  - id: "n_env"
    type: "ADEnvelope"
    title: "Envelope"
    literals:
      - pin: "Attack"
        value: 0.01
      - pin: "Decay"
        value: 0.25
  - id: "n_osc"
    type: "Sine"
    title: "Oscillator"
    literals:
      - pin: "Frequency"
        value: 220.0
      - pin: "Amplitude"
        value: 0.5
  - id: "n_gain"
    type: "Gain"
    title: "Level"
    literals:
      - pin: "Gain"
        value: 0.8
  - id: "n_out"
    type: "Output"
    title: "Master out"
    literals: []
links:
  - from:
      node: "n_start"
      pin: "Out"
    to:
      node: "n_env"
      pin: "Trigger"
    feedback: false
  - from:
      node: "n_osc"
      pin: "Out"
    to:
      node: "n_gain"
      pin: "In"
    feedback: false
  - from:
      node: "n_gain"
      pin: "Out"
    to:
      node: "n_out"
      pin: "In"
    feedback: false
sends: []
"""

# A graph that cannot compile: an Output node with no input. The compiler
# rejects a required, unwired audio input.
BROKEN_GRAPH = """\
schema: 1
name: "Broken"
output: Mono
inputs: []
variables: []
nodes:
  - id: "n_osc"
    type: "Sine"
    title: "Oscillator"
    literals: []
  - id: "n_out"
    type: "Output"
    title: "Master out"
    literals: []
links: []
sends: []
"""


def _open_project(graph_text, name="TestTone.wlavb"):
    """Author a project containing one graph asset and OPEN it.

    Project.Open, not AttachAssetRoot: AvbProjectRuntime::Load refuses to run
    without an open Wasteland Project, and AttachAssetRoot alone leaves the
    singleton unopened. That failure reads as "the graph did not compile" with
    the real reason one frame up the stack, so the test opens it properly.
    """
    root = _tempdir()
    graphs = os.path.join(root, "assets", "graphs")
    os.makedirs(graphs, exist_ok=True)
    with open(os.path.join(graphs, name), "w", encoding="utf-8") as handle:
        handle.write(graph_text)
    # Project.Open refuses a directory without a descriptor, and the AVB runtime
    # refuses to run without an OPEN project. Both are load-bearing: the
    # descriptor is the project's identity, and the open state is what
    # AvbProjectRuntime checks before it resolves an asset.
    with open(os.path.join(root, "project.wasteland"), "w", encoding="utf-8") as handle:
        handle.write("Version: 1\nName: AudioGraphTests\n")
    project = Wasteland.Project.Get()
    project.Close()
    project.Open(root)
    return root


def _graph_entity(scene, graph_path, triggers=None, duration=0.0):
    """Create an entity carrying an AudioGraphComponent.

    The Scene is held by the caller: an Entity carries a pointer into its
    Scene's registry, and `Scene().CreateEntity(...)` in a one-liner leaves the
    Scene unreferenced at the end of the expression.
    """
    entity = scene.CreateEntity("graph speaker")
    component = Wasteland.AudioGraphComponent()
    component.GraphPath = graph_path
    component.DurationSeconds = duration
    if triggers:
        for event, node, pin in triggers:
            component.Triggers.append(
                Wasteland.AudioGraphTrigger(event, node, pin))
    entity.AddOrReplaceAudioGraphComponent(component)
    return entity


def _voice_count(scene):
    """Active GRAPH voices, read from the mixer rather than from the map.

    AudioSystem.GetGraphVoiceCount deliberately counts through the mixer: an
    instance whose voice was stolen or stopped is still "attached" and must not
    be counted. That distinction is the whole point of the number.
    """
    return scene.GetAudio().GetGraphVoiceCount()


def _mix(scene, frames=480):
    """Drive the mixer by one block.

    A procedural voice is advanced by Mix, NOT by Update: Update advances clip
    positions and fades, while the graph's generator (and therefore its
    duration clock) runs inside the mix. A test that only called Update would
    see a voice that never retires.
    """
    scene.GetAudio().GetMixer().Mix(frames)


# ---------------------------------------------------------------------------
# the plan's verification
# ---------------------------------------------------------------------------

def test_a_triggered_graph_raises_the_voice_count():
    """The plan's own check: attach, dispatch, and read the count."""
    _open_project(TONE_GRAPH)
    scene = Scene()
    try:
        entity = _graph_entity(scene, "assets/graphs/TestTone.wlavb",
                               triggers=[("OnStep", "n_start", "In")],
                               duration=1.0)
        check_eq(_voice_count(scene), 0,
                 "no graph voice before any trigger")

        check(scene.GetAudio().TriggerGraph(entity, "OnStep"),
              "the trigger reached the graph")
        check_eq(_voice_count(scene), 1,
                 "the mixer voice count ROSE after the trigger")
    finally:
        del scene


def test_the_voice_falls_when_the_duration_elapses():
    """A graph is a continuous source, so without a limit a one-shot footstep
    graph would hold a voice forever and the cap would fill with stale graphs.

    This is the "falls" half of the plan's verification, and it is the half that
    would silently rot without a test: the voice rising is easy to see, the
    voice never retiring is only visible as a cap that is permanently full.
    """
    _open_project(TONE_GRAPH)
    scene = Scene()
    try:
        entity = _graph_entity(scene, "assets/graphs/TestTone.wlavb",
                               triggers=[("OnStep", "n_start", "In")],
                               duration=0.05)
        scene.GetAudio().TriggerGraph(entity, "OnStep")
        check_eq(_voice_count(scene), 1, "the voice started")

        # 0.05 s at 48 kHz is 2400 frames: five 480-frame blocks.
        for _ in range(6):
            _mix(scene)
        check_eq(_voice_count(scene), 0,
                 "the voice FELL once its duration elapsed")
    finally:
        del scene


def test_a_graph_without_a_duration_never_retires_itself():
    """DurationSeconds == 0 means "until stopped", which is what an ambient
    graph wants. The two behaviours must not be confused: a default component
    must not be a one-shot."""
    _open_project(TONE_GRAPH)
    scene = Scene()
    try:
        entity = _graph_entity(scene, "assets/graphs/TestTone.wlavb",
                               triggers=[("OnStep", "n_start", "In")])
        scene.GetAudio().TriggerGraph(entity, "OnStep")
        for _ in range(20):
            _mix(scene)
        check_eq(_voice_count(scene), 1,
                 "a graph with no duration stays audible until stopped")
    finally:
        del scene


def test_an_unmatched_event_is_a_quiet_no_op():
    """The same convention ScriptEventDispatcher uses for a missing Python
    method: a game can fire one event at every entity without checking which
    ones listen."""
    _open_project(TONE_GRAPH)
    scene = Scene()
    try:
        entity = _graph_entity(scene, "assets/graphs/TestTone.wlavb",
                               triggers=[("OnStep", "n_start", "In")])
        check(not scene.GetAudio().TriggerGraph(entity, "OnJump"),
              "an event with no matching trigger entry does nothing")
        check_eq(_voice_count(scene), 0,
                 "an unmatched event started no voice")

        # And it must not disturb a graph that IS listening for something else.
        scene.GetAudio().TriggerGraph(entity, "OnStep")
        check_eq(_voice_count(scene), 1, "the matching event still works")
    finally:
        del scene


def test_a_failed_graph_reports_its_reason_and_starts_no_voice():
    """A graph that cannot compile is an authoring error, not a crash: the
    reason is kept on the instance so the editor can show it."""
    _open_project(BROKEN_GRAPH, "Broken.wlavb")
    scene = Scene()
    try:
        entity = _graph_entity(scene, "assets/graphs/Broken.wlavb")
        check(entity.TryGetAudioGraphComponent().CompileError != "",
              "the compile failure is reported on the component")
        check_eq(_voice_count(scene), 0,
                 "a graph that did not compile starts no voice")

        # A trigger on a broken graph is a quiet no-op, not an exception.
        check(not scene.GetAudio().TriggerGraph(entity, "OnStep"),
              "a trigger cannot start a graph that did not compile")
    finally:
        del scene


def test_detach_stops_the_voice():
    """Detach is the explicit stop. The per-frame prune is the safety net for a
    component removed without one, and there is no OnComponentRemoved hook in
    Scene, so the prune is the only thing that notices."""
    _open_project(TONE_GRAPH)
    scene = Scene()
    try:
        entity = _graph_entity(scene, "assets/graphs/TestTone.wlavb",
                               triggers=[("OnStep", "n_start", "In")])
        scene.GetAudio().TriggerGraph(entity, "OnStep")
        check_eq(_voice_count(scene), 1, "the voice started")

        scene.GetAudio().DetachGraph(entity)
        check_eq(_voice_count(scene), 0, "detaching stopped the voice")
        check(not scene.GetAudio().IsGraphAttached(entity),
              "the instance is gone")
    finally:
        del scene


def test_a_removed_component_is_pruned():
    """There is no OnComponentRemoved in Scene, so UpdateGraphs is the only
    place a component removed without DetachGraph can be noticed. Without it a
    runtime leaks and holds a voice slot forever."""
    _open_project(TONE_GRAPH)
    scene = Scene()
    try:
        entity = _graph_entity(scene, "assets/graphs/TestTone.wlavb",
                               triggers=[("OnStep", "n_start", "In")])
        scene.GetAudio().TriggerGraph(entity, "OnStep")
        check_eq(_voice_count(scene), 1, "the voice started")

        entity.RemoveAudioGraphComponent()
        check_eq(_voice_count(scene), 1,
                 "the voice survives until the next prune tick")
        scene.OnUpdateRuntime(1.0 / 60.0)
        check_eq(_voice_count(scene), 0,
                 "the prune tick retired the orphaned voice")
    finally:
        del scene


def test_the_component_round_trips_through_a_scene_file():
    """The trigger map is authored data, so it must survive a save/load. A
    by-value Python list would have made the append in _graph_entity vanish and
    this test would fail on an empty map."""
    _open_project(TONE_GRAPH)
    scene = Scene()
    try:
        entity = _graph_entity(scene, "assets/graphs/TestTone.wlavb",
                               triggers=[("OnStep", "n_start", "In"),
                                         ("OnLand", "n_start", "In")],
                               duration=0.75)
        component = entity.TryGetAudioGraphComponent()
        check_eq(len(component.Triggers), 2,
                 "both trigger entries are on the live component")

        path = os.path.join(_tempdir(), "graph_scene.wastescene")
        check(scene.SaveScene(path), "the scene saved")

        loaded = Scene()
        try:
            check(loaded.LoadScene(path), "the scene loaded")
            restored = None
            for candidate in loaded.GetAllEntitiesWithTag():
                if candidate.GetName() != "graph speaker":
                    continue
                restored = candidate
                break
            check(restored is not None, "the entity came back")
            graph = restored.TryGetAudioGraphComponent()
            check(graph is not None, "the component came back")
            check_eq(graph.GraphPath, "assets/graphs/TestTone.wlavb", "the path survived")
            check_eq(graph.DurationSeconds, 0.75, "the duration survived")
            check_eq(len(graph.Triggers), 2, "both triggers survived")
            events = sorted(t.Event for t in graph.Triggers)
            check_eq(events, ["OnLand", "OnStep"],
                      "the trigger map survived intact")
        finally:
            del loaded
    finally:
        del scene


def test_the_trigger_list_is_live_not_a_copy():
    """def_readwrite on a non-opaque vector hands Python a COPY, so an append
    would silently do nothing. This asserts the list is the live container."""
    component = Wasteland.AudioGraphComponent()
    component.Triggers.append(Wasteland.AudioGraphTrigger("OnStep", "n_start", "In"))
    check_eq(len(component.Triggers), 1,
             "an append reached the live vector")
    component.Triggers.append(Wasteland.AudioGraphTrigger("OnLand", "n_start", "In"))
    check_eq(len(component.Triggers), 2, "a second append reached it too")


# ---------------------------------------------------------------------------
# the mixer's procedural voice, in isolation
# ---------------------------------------------------------------------------

def _procedural_mixer(generator):
    mixer = Wasteland.Audio.Mixer()
    voice = Wasteland.Audio.ProceduralVoice()
    voice.Name = "test"
    voice.Generate = generator
    request = Wasteland.Audio.PlayRequest()
    request.Procedural = voice
    voice_id = mixer.PlayProcedural(request, voice)
    return mixer, voice_id


def test_a_procedural_voice_is_retired_by_its_generator():
    """Returning false from Generate is the ONLY way a procedural voice ends.
    AdvanceVoice deliberately keeps it alive, because Update has no idea how
    long a graph should run."""
    calls = []

    def generator(output, frames):
        calls.append(frames)
        for index in range(frames):
            output[index] = 0.5
        return len(calls) < 3

    mixer, voice_id = _procedural_mixer(generator)
    check(voice_id >= 0, "the procedural voice started")
    check_eq(mixer.GetActiveVoiceCount(), 1, "the voice is live")
    mixer.Mix(480)
    mixer.Mix(480)
    check_eq(mixer.GetActiveVoiceCount(), 1, "still live before the generator says stop")
    mixer.Mix(480)
    check_eq(mixer.GetActiveVoiceCount(), 0,
             "the generator's false retired the voice")
    check(not mixer.IsPlaying(voice_id),
          "the retired id is not reusable as a live voice")


def test_a_procedural_voice_writes_a_real_signal():
    """A graph that produces silence is indistinguishable from a broken graph
    by the voice count alone, so the samples are asserted too."""

    def generator(output, frames):
        for index in range(frames):
            output[index] = 0.25
        return False

    mixer, _ = _procedural_mixer(generator)
    buffer = mixer.Mix(64)
    check_eq(len(buffer), 128, "the mixer returns interleaved stereo")
    peak = max(abs(sample) for sample in buffer)
    check(peak > 0.0, f"the procedural voice produced a signal (peak {peak})")


def test_a_muted_procedural_voice_still_advances():
    """Muting must not freeze a graph. Its phase, envelope and delay lines have
    to advance on the same clock they would at full volume, or unmuting it
    resumes from where it was muted instead of where the music is."""

    calls = []

    def generator(output, frames):
        calls.append(frames)
        for index in range(frames):
            output[index] = 1.0
        return len(calls) < 2

    mixer, voice_id = _procedural_mixer(generator)
    # Mute the VOICE, not the mixer. Mixer::Mix has a deliberate whole-mixer
    # early-out when the master volume is zero -- "muting costs nothing, since no
    # voice is even visited" -- so muting the mixer would freeze every graph's
    # internal state, not just this one. A muted VOICE is the case the generator
    # still has to run for.
    mixer.SetVoiceVolume(voice_id, 0.0)
    mixer.Mix(480)
    check_eq(mixer.GetActiveVoiceCount(), 1,
             "a muted procedural voice is still live")
    check_eq(len(calls), 1,
             "the muted voice still consumed a generator block")
    buffer = mixer.Mix(480)
    check(all(sample == 0.0 for sample in buffer),
          "a muted voice writes silence")


def test_a_procedural_voice_respects_the_voice_cap():
    """The reason ProceduralVoice exists: a graph must be subject to the same
    cap, priority and steal rules as every other sound. A privileged
    direct-to-output path would be a graph a firefight cannot silence."""

    def generator(output, frames):
        for index in range(frames):
            output[index] = 0.0
        return True

    mixer = Wasteland.Audio.Mixer(2)
    voice = Wasteland.Audio.ProceduralVoice()
    voice.Generate = generator
    request = Wasteland.Audio.PlayRequest()
    request.Procedural = voice

    first = mixer.PlayProcedural(request, voice)
    second = mixer.PlayProcedural(request, voice)
    check(first >= 0 and second >= 0, "both voices started under the cap")
    check_eq(mixer.GetActiveVoiceCount(), 2, "the cap is two")

    # At the cap a new voice STEALS the lowest-priority one, exactly as a clip
    # voice does. That is the behaviour under test: a graph is not privileged.
    # A drop would be the OTHER path -- it only happens when every live voice
    # outranks the newcomer, which is not the case here.
    third = mixer.PlayProcedural(request, voice)
    check(third >= 0, "the third voice took a slot rather than being refused")
    check_eq(mixer.GetActiveVoiceCount(), 2, "still two after the steal")
    check(mixer.GetStealedVoiceCount() > 0,
          "the steal is counted so a busy firefight is visible in a debugger")


def test_set_voice_position_moves_a_procedural_voice():
    """Without SetVoicePosition a moving source would keep its launch position
    and pan as if it had not moved."""

    def generator(output, frames):
        for index in range(frames):
            output[index] = 0.0
        return True

    mixer, voice_id = _procedural_mixer(generator)
    check(mixer.SetVoicePosition(voice_id, Wasteland.Vec3(10.0, 0.0, 0.0)),
          "the voice accepted a new position")
    check(not mixer.SetVoicePosition(-1, Wasteland.Vec3(0.0, 0.0, 0.0)),
          "a retired id is refused rather than silently matching")


# ---------------------------------------------------------------------------
# runner
# ---------------------------------------------------------------------------

def _main():
    tests = [(name, obj) for name, obj in sorted(globals().items())
             if name.startswith("test_") and callable(obj)]

    failed = 0
    for name, test in tests:
        try:
            test()
            print(f"  {name} ...")
        except AssertionError as error:
            failed += 1
            print(f"  {name} ... FAIL")
            print(f"      {error}")
        except Exception as error:
            failed += 1
            print(f"  {name} ... FAIL")
            print(f"      {type(error).__name__}: {error}")

    print(f"\n{len(tests) - failed}/{len(tests)} tests passed")
    print(f"{len(tests) - failed}/{len(tests)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(_main())
