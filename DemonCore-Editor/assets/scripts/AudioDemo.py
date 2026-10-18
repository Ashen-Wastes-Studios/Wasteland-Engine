"""Audio showcase: loads a WAV clip and plays it through the AudioSystem.

The scene's ShowcaseAudioSource entity carries an AudioSourceComponent naming
the clip, but a clip only reaches the mixer when something loads it. This
script does that once, then hands playback to the engine's spatial audio path
(the AudioListenerComponent on the camera positions it in the world).

Attach to the same entity as the AudioSourceComponent.
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
        audio.Play("showcase_tone")
        self._started = True
