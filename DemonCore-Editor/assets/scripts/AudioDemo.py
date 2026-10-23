"""Audio showcase: loads a WAV clip and plays it through the AudioSystem.

The scene's ShowcaseAudioSource entity carries an AudioSourceComponent naming
the clip, but a clip only reaches the mixer when something loads it. This
script does that once, then hands playback to the engine's spatial audio path
(the AudioListenerComponent on the camera positions it in the world).

Attach to the same entity as the AudioSourceComponent.

WHY PlayEntity AND NOT Play
---------------------------

This used to call `audio.Play("showcase_tone")`, which is NON-spatial: the clip
plays at full volume from both ears and the listener is never consulted. That
made every spatial field on the scene's AudioSourceComponent -- Spatial,
PositionOffset, MinDistance, MaxDistance -- inert, and it contradicted this
docstring, which promised the spatial path.

PlayEntity resolves the source's world position from the entity and routes it
through the listener, so the tone pans as the player moves. That is the
showcase's purpose; the previous call made it a plain stereo beep.

The listener has to be the camera for that to work. FindListenerEntity gives
an explicit AudioListenerComponent priority over the primary camera, so the
scene's ShowcaseAudioListener marker -- a static entity 48 m from the source --
used to pin the ears to itself and ignore the player entirely. That component
is gone: with no explicit listener the ears follow the eyes, which is what the
docstring above always claimed.
"""

import Wasteland


class AudioDemo:
    def __init__(self):
        self._started = False

    def OnUpdateEntity(self, dt):
        if self._started:
            return

        scene = Wasteland.Scene.GetActiveScene()
        if scene is None:
            return

        audio = scene.GetAudio()
        if audio is None:
            return

        clip_path = "assets/audio/showcase_tone.wav"
        if not audio.LoadClip(clip_path):
            return

        # ClipName in the scene is the file stem, which is how the mixer
        # registers it.
        #
        # The source entity is looked up by tag rather than captured at
        # construction: a script's __init__ runs before the entity is fully
        # wired into the scene, and an Entity holds a raw pointer into its
        # Scene's registry, so one taken too early dangles.
        source = None
        for entity in scene.GetAllEntitiesWithTransform():
            if entity.HasTag() and entity.GetTag().Tag == "ShowcaseAudioSource":
                source = entity
                break

        if source is None:
            return

        # Spatial play: position comes from the entity, panning from the
        # listener. This is the call that makes the scene's MinDistance and
        # MaxDistance mean something.
        audio.PlayEntity(source)
        self._started = True
