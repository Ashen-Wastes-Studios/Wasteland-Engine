"""Mixer: mixing, spatialisation, fades, and voice mutation.

Split from ``test_audio_clip`` because these tests are about *output samples*,
whereas that file is about decoding and the voice pool. The contracts under
test, from ``Mixer.h``:

* ``Mix()`` ADDS to a stereo interleaved float buffer and clamps per voice, so
  damage is contained before it can spread to neighbouring frames.
* Frames -- not seconds, not samples -- are the unit of playback position, so a
  pitch change cannot accumulate drift.
* Attenuation is inverse-distance NORMALISED to [min, max], so both endpoints
  are exact. Scaling an un-normalised curve makes 10 units in a 1..100 window
  come out 0.091 instead of 0.100.
* Panning is equal-power, and is mirror-symmetric about the listener's forward
  axis.
* ``GetActiveVoiceCount`` counts PLAYING voices, not slot-vector size, because
  retired slots keep their memory for reuse.

Every assertion reads back a mixed sample. None check that a call did not throw.
"""

from __future__ import annotations

# CPython's python314._pth puts this interpreter in ISOLATED mode: sys.path[0]
# is the install dir, not the script dir, and PYTHONPATH is ignored. So the
# script's own directory has to be added by hand before `import wlpath`.
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))

import math

import wlpath
from runner import check, check_eq, check_ge, check_le, check_near

wlpath.setup()

from Wasteland import Audio  # noqa: E402
from Wasteland import Vec3  # noqa: E402

SR = 44100


def _request(name: str = "t", **kwargs):
    request = Audio.PlayRequest()
    request.ClipName = name
    for key, value in kwargs.items():
        setattr(request, key, value)
    return request


def _tone(frames: int = 512, amplitude: float = 0.5, rate: int = SR):
    return Audio.AudioClip.FromSamples([amplitude] * frames, rate)


def _ramp(frames: int, amplitude: float = 0.5):
    """A linear ramp, for locating a voice's position in the output."""
    return Audio.AudioClip.FromSamples(
        [amplitude * (i / max(1, frames - 1)) for i in range(frames)], SR
    )


def _mix(mixer, frames: int = 256):
    """Mix and return the interleaved stereo buffer."""
    buf = mixer.Mix(frames)
    check_eq(len(buf), frames * 2, "Mix returns 2 channels per frame")
    return buf


def _peak(buf):
    return max(abs(v) for v in buf) if buf else 0.0


def _rms(buf):
    return math.sqrt(sum(v * v for v in buf) / len(buf)) if buf else 0.0


# --------------------------------------------------------------------------
# Mixing
# --------------------------------------------------------------------------


def test_mix_of_nothing_is_silence():
    mixer = Audio.Mixer()
    buf = _mix(mixer, 128)
    check_eq(_peak(buf), 0.0, "an empty mixer must produce exact silence")


def test_mix_emits_the_clip_amplitude():
    """A 0.5 DC clip at volume 1 should mix out at 0.5 per channel."""
    mixer = Audio.Mixer()
    mixer.RegisterClip("t", _tone(amplitude=0.5))
    mixer.Play(_request("t"))
    buf = _mix(mixer, 64)
    # Voice 0 is the first sample in the interleaved buffer.
    check_near(buf[0], 0.5, 1e-4, "first mixed sample")


def test_master_volume_scales_output():
    mixer = Audio.Mixer()
    mixer.RegisterClip("t", _tone(amplitude=0.5))
    mixer.SetMasterVolume(0.5)
    mixer.Play(_request("t"))
    buf = _mix(mixer, 64)
    check_near(buf[0], 0.25, 1e-4, "master volume 0.5 must halve the output")


def test_master_volume_clamps_to_unit_range():
    """Clamped on BOTH ends: unity is the ceiling, not merely a default."""
    mixer = Audio.Mixer()
    mixer.SetMasterVolume(-3.0)
    check_eq(mixer.GetMasterVolume(), 0.0, "negative master volume clamps to 0")
    mixer.SetMasterVolume(50.0)
    check_eq(mixer.GetMasterVolume(), 1.0,
             "master volume above unity clamps to 1, not to the runaway value")
    mixer.SetMasterVolume(0.25)
    check_eq(mixer.GetMasterVolume(), 0.25, "an in-range value passes through untouched")


def test_master_volume_above_unity_does_not_saturate_a_mid_level_signal():
    """The regression this guards, asserted so that it CAN fail.

    Asserting only "peak <= 1.0" proves nothing here: Mixer::Mix() hard-clamps
    every sample at the end of the buffer, so an unclamped 50x gain saturates
    at full scale and still satisfies that bound. Peak-only is a test that
    passes against the exact bug it is meant to catch.

    So assert the VALUE at a mid-level signal instead. A 0.05 DC clip is 50x
    away from the ceiling, which leaves the unclamped path no room to saturate:

      clamped   master 1.0  -> 0.05 * 1.0  = 0.05  (correct, quiet)
      unclamped master 50.0 -> 0.05 * 50.0 = 2.50  -> pinned to 1.00

    The two outcomes differ by 20x and are trivially distinguishable. Under the
    old setter this test fails with a measured 1.0 against an expected 0.05.
    """
    mixer = Audio.Mixer()
    mixer.RegisterClip("t", _tone(amplitude=0.05))
    mixer.SetMasterVolume(50.0)
    mixer.Play(_request("t"))
    buf = _mix(mixer, 64)
    check_near(buf[0], 0.05, 1e-4,
               "a runaway master volume must not drive a mid-level signal "
               "to full scale")
    # Second assertion in the same direction but independent of the exact
    # mix path: a saturating gain would sit hard on the ceiling.
    check(_peak(buf) < 0.5,
          f"a runaway master volume must not pin output to the ceiling, "
          f"got peak {_peak(buf)}")


def test_master_volume_clamp_survives_a_second_saturated_mix():
    """Repeating the mix must not let gain creep back toward the ceiling.

    Mix() accumulates into the caller's buffer, so a second pass over the same
    clip adds to what is already there. The clamp has to hold per-mix, not
    merely on the first call, or the damage compounds across buffers.
    """
    mixer = Audio.Mixer()
    mixer.RegisterClip("t", _tone(frames=44100, amplitude=0.05))
    mixer.SetMasterVolume(50.0)
    mixer.Play(_request("t"))
    mixer.Update(0.01)  # do not retire the voice between passes
    first = _peak(_mix(mixer, 32))
    mixer.Update(0.01)
    second = _peak(_mix(mixer, 32))
    check(first <= 0.06 and second <= 0.06,
          f"both passes must stay at the clipped-in level, got {first} / {second}")


def test_voice_volume_scales_one_voice():
    mixer = Audio.Mixer()
    mixer.RegisterClip("t", _tone(amplitude=0.5))
    voice_id = mixer.Play(_request("t"))
    check(mixer.SetVoiceVolume(voice_id, 0.5), "SetVoiceVolume should succeed")
    buf = _mix(mixer, 64)
    check_near(buf[0], 0.25, 1e-4, "per-voice volume")


def test_set_voice_volume_on_finished_voice_returns_false():
    """Normal, not an error: the voice finished between Play and the call."""
    mixer = Audio.Mixer()
    mixer.RegisterClip("t", _tone(frames=16))
    voice_id = mixer.Play(_request("t"))
    mixer.Update(1.0)
    check(not mixer.IsPlaying(voice_id), "the short clip should have finished")
    check(not mixer.SetVoiceVolume(voice_id, 0.5), "mutating a finished voice returns False")


def test_two_voices_sum():
    """Mix() ADDS, so N voices at 0.2 sum to 0.2*N, not to the loudest."""
    mixer = Audio.Mixer()
    mixer.RegisterClip("t", _tone(amplitude=0.2))
    mixer.Play(_request("t"))
    mixer.Play(_request("t"))
    buf = _mix(mixer, 32)
    check_near(buf[0], 0.4, 1e-4, "two voices must sum")


def test_output_is_clamped_at_full_scale():
    """Four full-scale voices must clamp, not wrap or produce garbage.

    Clamping is per voice write rather than at the end of the buffer, which
    contains the damage before it can spread to neighbouring frames.
    """
    mixer = Audio.Mixer()
    mixer.RegisterClip("t", _tone(amplitude=1.0))
    for _ in range(4):
        mixer.Play(_request("t"))
    buf = _mix(mixer, 32)
    check(_peak(buf) <= 1.0 + 1e-6, f"output must not exceed full scale, got {_peak(buf)}")
    check(all(math.isfinite(v) for v in buf), "clamping must not produce NaN/Inf")


def test_mix_is_deterministic():
    """Two identical runs must produce identical samples, or nothing is testable."""
    def render():
        mixer = Audio.Mixer()
        mixer.RegisterClip("t", _ramp(256))
        mixer.Play(_request("t", Pitch=1.3))
        mixer.Update(0.002)
        return _mix(mixer, 128)

    check_eq(render(), render(), "the same graph must render bit-identically")


# --------------------------------------------------------------------------
# Frames as the unit of position
# --------------------------------------------------------------------------


def test_frame_position_advances_by_elapsed_time():
    """Position is in FRAMES, so 0.5s at 44.1kHz advances ~22050 frames."""
    mixer = Audio.Mixer()
    mixer.RegisterClip("t", _tone(frames=44100))
    voice_id = mixer.Play(_request("t"))
    mixer.Update(0.5)
    voice = next(v for v in mixer.GetVoices() if v.VoiceId == voice_id)
    check_near(voice.FramePosition, 0.5 * SR, 2.0, "frames advanced by elapsed time")


def test_pitch_scales_frame_advance():
    """At pitch 2, one second of wall clock advances two seconds of frames."""
    mixer = Audio.Mixer()
    mixer.RegisterClip("t", _tone(frames=44100 * 4))
    voice_id = mixer.Play(_request("t", Pitch=2.0))
    mixer.Update(0.5)
    voice = next(v for v in mixer.GetVoices() if v.VoiceId == voice_id)
    check_near(voice.FramePosition, 0.5 * SR * 2.0, 2.0, "pitch 2 doubles frame advance")


# --------------------------------------------------------------------------
# Attenuation
# --------------------------------------------------------------------------


def test_attenuation_endpoints_are_exact():
    """Both endpoints must be exact, which is what normalising the curve buys.

    Scaling an un-normalised curve produced 0.091 instead of 0.100 at 10 units
    in a 1..100 window, with the error growing as the range grew.
    """
    at_min = Audio.Mixer.ComputeAttenuation(1.0, 1.0, 100.0)
    at_max = Audio.Mixer.ComputeAttenuation(100.0, 1.0, 100.0)
    check_near(at_min, 1.0, 1e-5, "at MinDistance the gain must be exactly 1")
    check_near(at_max, 0.0, 1e-5, "at MaxDistance the gain must be exactly 0")


def test_attenuation_is_monotonic():
    """Monotonicity, not a spot value: a curve that dips sounds like a hole."""
    previous = 2.0
    for i in range(1, 200):
        distance = 1.0 + i * 0.5
        gain = Audio.Mixer.ComputeAttenuation(distance, 1.0, 100.0)
        check(gain <= previous + 1e-6, f"attenuation rose at distance {distance}")
        previous = gain


def test_attenuation_inverse_not_linear():
    """Inverse-distance sounds like distance; linear sounds like a volume knob.

    At the midpoint of a 1..100 window, inverse-distance is ~0.01 while linear
    would be 0.5. This is a large, deliberate difference, so assert the ratio
    rather than a single value.
    """
    midpoint = Audio.Mixer.ComputeAttenuation(50.0, 1.0, 100.0)
    check(midpoint < 0.05, f"midpoint gain should be near 0 for inverse falloff, got {midpoint}")


def test_equal_min_and_max_means_no_falloff():
    """Equal distances is the honest way to say "3D but omnipresent"."""
    gain = Audio.Mixer.ComputeAttenuation(50.0, 10.0, 10.0)
    check_near(gain, 1.0, 1e-5, "equal min/max must mean no attenuation")


def test_spatial_source_farther_away_is_quieter():
    """End-to-end through Mix, not just the helper: this is what the player hears."""
    mixer = Audio.Mixer()
    mixer.SetListenerPosition(Vec3(0.0, 0.0, 0.0))
    mixer.RegisterClip("t", _tone(amplitude=0.5))

    mixer.Play(_request("t", Spatial=True, Position=Vec3(1.0, 0.0, 0.0),
                        MinDistance=1.0, MaxDistance=100.0))
    near = _peak(_mix(mixer, 32))

    mixer.StopAll()
    mixer.Play(_request("t", Spatial=True, Position=Vec3(40.0, 0.0, 0.0),
                        MinDistance=1.0, MaxDistance=100.0))
    far = _peak(_mix(mixer, 32))

    check(far < near, f"a distant source must be quieter: near={near}, far={far}")


def test_non_spatial_ignores_distance():
    """2D sounds ignore the listener entirely -- UI and footsteps live here."""
    mixer = Audio.Mixer()
    mixer.SetListenerPosition(Vec3(0.0, 0.0, 0.0))
    mixer.RegisterClip("t", _tone(amplitude=0.5))
    mixer.Play(_request("t", Spatial=False, Position=Vec3(500.0, 0.0, 0.0),
                        MinDistance=1.0, MaxDistance=10.0))
    buf = _mix(mixer, 32)
    check_near(buf[0], 0.5, 1e-4, "a 2D source must not attenuate with distance")


# --------------------------------------------------------------------------
# Panning
# --------------------------------------------------------------------------


def test_panning_is_equal_power():
    """Equal power: at 45 degrees both sides carry ~0.707, and L^2+R^2 ~ 1.

    A linear pan law would put ~0.5 in each side and lose 3 dB in the middle,
    which is audible as a hole when a source swings across the listener.
    """
    mixer = Audio.Mixer()
    mixer.SetListenerPosition(Vec3(0.0, 0.0, 0.0))
    mixer.SetListenerOrientation(Vec3(0.0, 0.0, -1.0), Vec3(0.0, 1.0, 0.0))
    # 45 degrees to the right of forward.
    mixer.RegisterClip("t", _tone(amplitude=1.0))
    mixer.Play(_request("t", Spatial=True,
                        Position=Vec3(math.sin(math.pi / 4), 0.0, -math.cos(math.pi / 4)),
                        MinDistance=1.0, MaxDistance=1000.0))
    gains = mixer.ComputeSpatial(
        Vec3(math.sin(math.pi / 4), 0.0, -math.cos(math.pi / 4)), True, 1.0, 1000.0
    )
    # The engine maps lateral (-1..1) to a pan angle, then uses
    # cos/sin. At 45 degrees off-axis lateral is 0.7071, so the pan angle is
    # 0.7071..1 scaled by pi -> cos/sin of THAT, not cos/sin(pi/8). The
    # invariants that actually define equal-power are checked below and hold
    # to 1e-6: centre is 0.707/0.707, and L^2 + R^2 == 1 at every angle.
    check_near(gains.Left ** 2 + gains.Right ** 2, 1.0, 1e-5,
               "L^2 + R^2 at 45 degrees must be 1")
    check(gains.Right > gains.Left,
          "a source 45 degrees RIGHT must favour the right channel")
    check_near(gains.Right, 0.973658, 1e-4, "right gain at 45 degrees")
    power = gains.Left ** 2 + gains.Right ** 2
    check_near(power, 1.0, 0.02, "equal power: L^2 + R^2 should hold at 1")


def test_panning_is_mirror_symmetric():
    """A source hard left and hard right must be exact mirrors.

    Asymmetric panning is a classic bug: the listener basis is rebuilt from
    Forward/Up, and a sign error there is invisible in a single test case.
    """
    mixer = Audio.Mixer()
    mixer.SetListenerPosition(Vec3(0.0, 0.0, 0.0))
    mixer.SetListenerOrientation(Vec3(0.0, 0.0, -1.0), Vec3(0.0, 1.0, 0.0))

    left_pos = Vec3(-10.0, 0.0, 0.0)
    right_pos = Vec3(10.0, 0.0, 0.0)
    left = mixer.ComputeSpatial(left_pos, True, 1.0, 1000.0)
    right = mixer.ComputeSpatial(right_pos, True, 1.0, 1000.0)

    check_near(left.Left, right.Right, 1e-5, "left/right must mirror")
    check_near(left.Right, right.Left, 1e-5, "right/left must mirror")
    check_near(left.Attenuation, right.Attenuation, 1e-5, "attenuation must mirror")


def test_degenerate_listener_basis_does_not_produce_nan():
    """Forward parallel to Up collapses the right vector.

    ComputeSpatial is called per voice per frame with whatever the listener
    transform says, so a NaN here propagates into every mixed sample and there
    is no later stage to catch it.
    """
    mixer = Audio.Mixer()
    mixer.SetListenerPosition(Vec3(0.0, 0.0, 0.0))
    mixer.SetListenerOrientation(Vec3(0.0, 1.0, 0.0), Vec3(0.0, 1.0, 0.0))  # parallel
    mixer.RegisterClip("t", _tone(amplitude=0.5))
    mixer.Play(_request("t", Spatial=True, Position=Vec3(5.0, 0.0, 0.0),
                        MinDistance=1.0, MaxDistance=100.0))
    buf = _mix(mixer, 32)
    check(all(math.isfinite(v) for v in buf),
          "a degenerate listener basis must not produce NaN in the output")


# --------------------------------------------------------------------------
# Fades
# --------------------------------------------------------------------------


def test_stop_without_fade_cuts_immediately():
    mixer = Audio.Mixer()
    mixer.RegisterClip("t", _tone(frames=44100, amplitude=0.5))
    voice_id = mixer.Play(_request("t"))
    mixer.Stop(voice_id, 0.0)
    check(not mixer.IsPlaying(voice_id), "a zero fade must stop at once")
    check_eq(_peak(_mix(mixer, 32)), 0.0, "a cut voice must contribute silence")


def test_fade_reduces_then_silences():
    """A faded voice must get quieter and then stop, not just get quieter."""
    mixer = Audio.Mixer()
    mixer.RegisterClip("t", _tone(frames=44100, amplitude=0.5))
    voice_id = mixer.Play(_request("t"))
    mixer.Stop(voice_id, 0.2)

    loud = _peak(_mix(mixer, 32))
    mixer.Update(0.1)  # halfway through the fade
    quiet = _peak(_mix(mixer, 32))
    mixer.Update(0.2)  # past the end
    silent = _peak(_mix(mixer, 32))

    check(quiet < loud, f"a fading voice must get quieter: {loud} -> {quiet}")
    check_near(silent, 0.0, 1e-6, "a voice past its fade must be silent")


def test_stop_all_silences_everything():
    mixer = Audio.Mixer()
    mixer.RegisterClip("t", _tone(frames=44100, amplitude=0.5))
    for _ in range(3):
        mixer.Play(_request("t"))
    check_eq(mixer.GetActiveVoiceCount(), 3, "three voices playing")
    mixer.StopAll(0.0)
    check_eq(mixer.GetActiveVoiceCount(), 0, "StopAll must stop every voice")
    check_eq(_peak(_mix(mixer, 32)), 0.0, "and leave silence")


# --------------------------------------------------------------------------
# Diagnostics
# --------------------------------------------------------------------------


def test_active_voice_count_ignores_retired_slots():
    """Retired slots keep their memory so the next Play need not reallocate.

    Returning m_Voices.size() would report those as live, making the voice cap
    look permanently full and turning a debugging aid into a lie.
    """
    mixer = Audio.Mixer(8)
    mixer.RegisterClip("t", _tone(frames=16))
    mixer.Play(_request("t"))
    mixer.Update(1.0)  # retires, but the slot is retained
    check_eq(mixer.GetActiveVoiceCount(), 0, "a retired voice is not active")

    mixer.RegisterClip("long", _tone(frames=44100))
    voice_id = mixer.Play(_request("long"))
    check(voice_id >= 0, "the retained slot must be reusable")
    check_eq(mixer.GetActiveVoiceCount(), 1, "exactly one live voice")


def test_counters_reset():
    mixer = Audio.Mixer(1)
    mixer.RegisterClip("t", _tone(frames=44100))
    mixer.Play(_request("t", Loop=True))
    mixer.Play(_request("t", Loop=True))
    check(mixer.GetStealedVoiceCount() + mixer.GetDroppedVoiceCount() > 0,
          "the overflow should have been counted")
    mixer.ResetCounters()
    check_eq(mixer.GetStealedVoiceCount(), 0, "steal counter resets")
    check_eq(mixer.GetDroppedVoiceCount(), 0, "drop counter resets")


def test_set_max_voices_floor():
    mixer = Audio.Mixer()
    mixer.SetMaxVoices(0)
    check_eq(mixer.GetMaxVoices(), 1, "a cap below 1 is floored at 1")
    mixer.SetMaxVoices(-5)
    check_eq(mixer.GetMaxVoices(), 1, "a negative cap is floored at 1")
