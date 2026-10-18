"""Audio core: WAV decoding and the device-independent mixer.

Re-derived 2026-10-01 (AVB.0). The previous suite described in
``.hermes/plans/item-14-long-tail-systems.md`` is not present in the tree, so
these tests are written from the implementation's own stated contracts:

* ``AudioClip.h``  — always mono float32 in [-1, 1]; multichannel downmixed.
* ``AudioClip.cpp`` — 8-bit is UNSIGNED (centre 128); channels are AVERAGED, not
  summed; 32-bit is float-only; a chunk claiming more than the file holds is
  clamped, not trusted.
* ``Mixer.h``      — pure computation, no device; voice pool with priority
  stealing; inverse-distance attenuation normalised to [min, max]; equal-power
  panning; per-voice fades; frames are the unit of playback position.

Every assertion is on a read-back value or an output sample. None of these tests
check that a call "did not throw" — a mixer that raises nothing and emits silence
passes such a test and fails the player.
"""
from __future__ import annotations

# CPython's python314._pth puts this interpreter in ISOLATED mode: sys.path[0]
# is the install dir, not the script dir, and PYTHONPATH is ignored. So the
# script's own directory has to be added by hand before `import wlpath`.
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))

import math
import os
import struct
import tempfile

import wlpath
from runner import check, check_eq, check_ge, check_le, check_near

wlpath.setup()

import wavbuild as wb  # noqa: E402
from Wasteland import Audio  # noqa: E402

SR = 44100

_LOAD_RESULT_NAMES = {
    0: "Success",
    1: "FileNotFound",
    2: "InvalidFormat",
    3: "UnsupportedEncoding",
    4: "TruncatedFile",
    5: "EmptyClip",
}


def _tmpwav(data: bytes) -> str:
    """Write bytes to a temp .wav and return its path."""
    fd, path = tempfile.mkstemp(suffix=".wav")
    with os.fdopen(fd, "wb") as handle:
        handle.write(data)
    return path


def _load(data: bytes):
    """Load WAV bytes; returns ``(clip_or_None, LoadResult)``.

    ``AudioClip.LoadWav`` is bound to return the two-tuple
    ``(AudioClip, LoadResult)``, so the failure reason is directly assertable
    rather than inferred from a null clip. Asserting the *reason* matters: a
    truncated file and an unsupported codec are a content bug and a missing
    codec respectively, and "it returned null" cannot tell them apart.
    """
    path = _tmpwav(data)
    try:
        return Audio.AudioClip.LoadWav(path)
    finally:
        os.unlink(path)


def _request(name: str = "t", **kwargs):
    """Build a PlayRequest.

    ``PlayRequest`` is bound default-construct only — pybind11 has no kwargs
    constructor for it — so fields are assigned after construction. Note that
    the ``Clip`` member of the C++ struct is NOT bound; a request is addressed
    by ``ClipName`` from Python.
    """
    request = Audio.PlayRequest()
    request.ClipName = name
    for key, value in kwargs.items():
        setattr(request, key, value)
    return request


# --------------------------------------------------------------------------
# AudioClip: construction
# --------------------------------------------------------------------------


def test_from_samples_basics():
    clip = Audio.AudioClip.FromSamples([0.0] * 128, 48000)
    check(clip is not None, "FromSamples returned None")
    check(clip.IsValid(), "clip built from 128 samples should be valid")
    check_eq(clip.GetSampleRate(), 48000, "sample rate")
    check_eq(clip.GetChannels(), 1, "FromSamples is always mono")
    check_eq(clip.GetFrameCount(), 128, "frame count")
    check_near(clip.GetDuration(), 128 / 48000.0, 1e-6, "duration in seconds")


def test_from_samples_computes_peak():
    clip = Audio.AudioClip.FromSamples([0.0, 0.5, -0.25, 0.9], SR)
    check_near(clip.GetPeak(), 0.9, 1e-6, "peak is the largest absolute sample")


def test_empty_clip_is_invalid():
    """An empty clip must be invalid, or it plays forever in silence."""
    clip = Audio.AudioClip.FromSamples([], SR)
    check(not clip.IsValid(), "an empty clip must not report valid")


def test_from_samples_clamps_bad_rate():
    """A non-positive rate falls back to 44100 rather than dividing by zero."""
    clip = Audio.AudioClip.FromSamples([0.1, 0.2], 0)
    check(clip.IsValid(), "clip with bad rate should still be valid")
    check_eq(clip.GetSampleRate(), 44100, "fallback sample rate")


# --------------------------------------------------------------------------
# AudioClip: WAV decoding, bit depths
# --------------------------------------------------------------------------


def test_16bit_silence_is_exact_zero():
    """16-bit silence must be exactly 0.0, not 1/32768 or similar."""
    clip, res = _load(wb.silence(32, bits=16))
    check(clip is not None, "16-bit silence failed to load")
    check(clip.IsValid(), "16-bit silence invalid")
    check_eq(clip.GetFrameCount(), 32, "frame count")
    peak = clip.GetPeak()
    check_eq(peak, 0.0, "16-bit silence peak must be exactly zero")


def test_8bit_silence_is_exact_zero():
    """The classic off-by-128 bug: 8-bit WAV is UNSIGNED, centre is 128.

    A loader that treats it as signed decodes silence as -1.0 and every
    8-bit asset comes out as full-scale noise.
    """
    clip, res = _load(wb.silence(32, bits=8))
    check(clip is not None, "8-bit silence failed to load")
    check_eq(clip.GetPeak(), 0.0, "8-bit silence must peak at 0, not -1")


def test_24bit_pcm_loads():
    # 32-bit is deliberately NOT tested as PCM: AudioClip rejects it, and
    # test_32bit_pcm_rejected covers that. Only float is valid at 32 bits.
    clip, result = _load(wb.silence(16, bits=24))
    check(clip is not None, "24-bit silence failed to load")
    check_eq(result, Audio.LoadResult.Success, "24-bit must report Success")
    check_eq(clip.GetPeak(), 0.0, "24-bit silence peak")


def test_32bit_float_loads():
    clip, result = _load(wb.silence(16, bits=32, float_fmt=True))
    check(clip is not None, "32-bit float failed to load")
    check_eq(result, Audio.LoadResult.Success, "32-bit float must report Success")
    check_eq(clip.GetPeak(), 0.0, "float silence peak")


def test_16bit_full_scale_amplitude():
    """A 0.5-amplitude 16-bit tone should decode to about 0.5, within quantisation."""
    data = wb.simple_wav([0.5, -0.5] * 32, bits=16)
    clip, _ = _load(data)
    check(clip is not None, "tone failed to load")
    # 16-bit quantisation at 0.5 is 1/32767 ~= 3.05e-5.
    check_near(clip.GetPeak(), 0.5, 1e-3, "16-bit peak amplitude round-trip")


def test_float32_bit_exact():
    """Float WAV is stored verbatim, so it must round-trip exactly."""
    samples = [0.0, 0.25, -0.75, 1.0, -1.0]
    clip, _ = _load(wb.simple_wav(samples, bits=32, float_fmt=True))
    check(clip is not None, "float tone failed to load")
    check_near(clip.GetPeak(), 1.0, 1e-6, "float peak is exact")


# --------------------------------------------------------------------------
# AudioClip: multichannel downmix
# --------------------------------------------------------------------------


def test_stereo_downmix_is_average_not_sum():
    """Opposite-phase stereo must CANCEL to silence.

    Averaging gives 0. Summing would give +/-2 and clip. This is the test that
    distinguishes the two, because a sum is "louder" and would pass a naive
    "did it load" check.
    """
    left = [0.5] * 32
    right = [-0.5] * 32
    interleaved = [v for pair in zip(left, right) for v in pair]
    clip, _ = _load(wb.simple_wav(interleaved, bits=16, channels=2))
    check(clip is not None, "stereo file failed to load")
    check_eq(clip.GetChannels(), 1, "stereo must downmix to mono")
    check_near(clip.GetPeak(), 0.0, 1e-3, "opposite-phase stereo must cancel")


def test_stereo_same_phase_is_not_double_loud():
    """Identical channels must decode to the same amplitude as one channel.

    A sum instead of an average would double the level, so every stereo asset
    would be 6 dB hotter than the mono version of the same sound.
    """
    mono_peak = _load(wb.simple_wav([0.5] * 32, bits=16, channels=1))[0].GetPeak()
    both = [v for pair in ((0.5, 0.5) for _ in range(32)) for v in pair]
    stereo_peak = _load(wb.simple_wav(both, bits=16, channels=2))[0].GetPeak()
    check_near(stereo_peak, mono_peak, 1e-3, "stereo sum vs average")


# --------------------------------------------------------------------------
# AudioClip: malformed input
# --------------------------------------------------------------------------


def test_missing_file_returns_none():
    clip, result = Audio.AudioClip.LoadWav(
        os.path.join(tempfile.gettempdir(), "no_such_file_xyz.wav")
    )
    check(clip is None, "loading a nonexistent file must return None, not crash")
    check_eq(result, Audio.LoadResult.FileNotFound, "reason must be FileNotFound")


def test_not_a_riff_file():
    clip, result = _load(b"NOTRIFFATALL" + b"\x00" * 64)
    check(clip is None, "a non-RIFF file must fail to load")
    check_eq(result, Audio.LoadResult.InvalidFormat, "reason must be InvalidFormat")


def test_riff_but_not_wave():
    body = b"AVI " + b"\x00" * 32
    data = b"RIFF" + struct.pack("<I", len(body)) + body
    clip, result = _load(data)
    check(clip is None, "RIFF/AVI must fail to load")
    check_eq(result, Audio.LoadResult.InvalidFormat, "reason must be InvalidFormat")


def test_header_claims_more_than_file_holds():
    """A data chunk declaring more bytes than exist must be CLAMPED.

    Trusting the header here reads past the end of the buffer, which is the
    whole class of WAV-parser crashes.
    """
    payload = wb.encode_pcm16([0.5] * 8)
    data = wb.riff([
        wb.fmt_chunk(bits_per_sample=16, channels=1, sample_rate=SR),
        wb.data_chunk(payload, declared_size=len(payload) + 4096),
    ])
    clip, _ = _load(data)
    check(clip is not None, "an over-claiming data chunk should still load")
    check_le(clip.GetFrameCount(), 8, "must not read more frames than the file holds")


def test_data_chunk_not_whole_frames_drops_partial():
    """A trailing partial frame is dropped, not read as a sample."""
    # One stray byte is half a 16-bit frame. (Two bytes would be a WHOLE frame,
    # so the loader would correctly report 5 -- partial means sub-frame.)
    payload = wb.encode_pcm16([0.5] * 4) + b"\x01"
    data = wb.riff([
        wb.fmt_chunk(bits_per_sample=16, channels=1, sample_rate=SR),
        wb.data_chunk(payload),
    ])
    clip, _ = _load(data)
    check(clip is not None, "a payload with a sub-frame tail should still load")
    check_eq(clip.GetFrameCount(), 4, "the partial frame must be dropped")


def test_empty_data_chunk_rejected():
    data = wb.riff([
        wb.fmt_chunk(bits_per_sample=16, channels=1, sample_rate=SR),
        wb.data_chunk(b""),
    ])
    clip, result = _load(data)
    check(clip is None, "a zero-length data chunk must not produce a valid clip")
    check_eq(result, Audio.LoadResult.EmptyClip, "reason must be EmptyClip")


def test_missing_fmt_chunk_rejected():
    # Pad the data chunk so the file clears the loader's 44-byte minimum and it
    # actually reaches the chunk walk. Otherwise it is rejected as TruncatedFile
    # first, which would test the size guard rather than the missing fmt.
    data = wb.riff([wb.data_chunk(wb.encode_pcm16([0.1] * 64))])
    check(len(data) >= 44, "fixture must clear the 44-byte minimum")
    clip, result = _load(data)
    check(clip is None, "a WAV with no fmt chunk must fail")
    check_eq(result, Audio.LoadResult.InvalidFormat, "reason must be InvalidFormat")


def test_unsupported_encoding_rejected():
    """A compressed codec must be reported, not mis-decoded into noise."""
    data = wb.riff([
        wb.fmt_chunk(format_tag=0x11, bits_per_sample=4, channels=1, sample_rate=SR),  # IMA ADPCM
        wb.data_chunk(b"\x00" * 32),
    ])
    clip, result = _load(data)
    check(clip is None, "ADPCM must not be decoded")
    check_eq(result, Audio.LoadResult.UnsupportedEncoding, "reason must be UnsupportedEncoding")


def test_32bit_pcm_rejected():
    """32-bit is only valid as float; a 32-bit PCM file would need a bogus /128."""
    data = wb.riff([
        wb.fmt_chunk(format_tag=wb.FMT_PCM, bits_per_sample=32, channels=1, sample_rate=SR),
        wb.data_chunk(b"\x00" * 32),
    ])
    clip, result = _load(data)
    check(clip is None, "32-bit PCM must be rejected, not scaled by 1/128")
    check_eq(result, Audio.LoadResult.UnsupportedEncoding, "reason must be UnsupportedEncoding")


def test_bad_bit_depth_rejected():
    data = wb.riff([
        wb.fmt_chunk(format_tag=wb.FMT_PCM, bits_per_sample=12, channels=1, sample_rate=SR),
        wb.data_chunk(b"\x00" * 32),
    ])
    clip, result = _load(data)
    check(clip is None, "12-bit must be rejected")
    check_eq(result, Audio.LoadResult.UnsupportedEncoding, "reason must be UnsupportedEncoding")


def test_extensible_header_accepted():
    """WAVE_FORMAT_EXTENSIBLE hides the real tag in a GUID; PCM must still load."""
    data = wb.riff([
        wb.fmt_chunk(format_tag=wb.FMT_EXTENSIBLE, bits_per_sample=16,
                     channels=1, sample_rate=SR, extensible_subformat=wb.FMT_PCM),
        wb.data_chunk(wb.encode_pcm16([0.5] * 8)),
    ])
    clip, _ = _load(data)
    check(clip is not None, "extensible PCM must load")
    check_near(clip.GetPeak(), 0.5, 1e-3, "extensible PCM peak")


def test_unknown_chunk_is_skipped():
    """A LIST chunk before fmt must be skipped, not read as sample data."""
    data = wb.riff([
        wb.list_chunk(),
        wb.fmt_chunk(bits_per_sample=16, channels=1, sample_rate=SR),
        wb.data_chunk(wb.encode_pcm16([0.5] * 8)),
    ])
    clip, _ = _load(data)
    check(clip is not None, "a file with a LIST chunk must load")
    check_eq(clip.GetFrameCount(), 8, "LIST chunk must not be mistaken for samples")


# --------------------------------------------------------------------------
# Mixer: voice pool
# --------------------------------------------------------------------------


def _tone(frames: int = 256, amplitude: float = 0.5, rate: int = SR) -> object:
    return Audio.AudioClip.FromSamples([amplitude] * frames, rate)


def test_clip_registry():
    mixer = Audio.Mixer()
    clip = _tone()
    mixer.RegisterClip("t", clip)
    check(mixer.HasClip("t"), "registered clip should be found")
    check(not mixer.HasClip("nope"), "unregistered clip must not be found")
    check(mixer.GetClip("t") is not None, "GetClip returns the clip")
    check("t" in list(mixer.GetClipNames()), "clip name should be listed")


def test_playing_a_missing_clip_returns_minus_one():
    """A sound that was never imported must not take the game down mid-firefight."""
    mixer = Audio.Mixer()
    voice_id = mixer.Play(_request("does_not_exist"))
    check_eq(voice_id, -1, "missing clip must return -1")


def test_play_returns_a_voice_id():
    mixer = Audio.Mixer()
    mixer.RegisterClip("t", _tone())
    voice_id = mixer.Play(_request("t"))
    check(voice_id >= 0, "a valid play should return a non-negative voice id")
    check(mixer.IsPlaying(voice_id), "voice should be playing")
    check_eq(mixer.GetActiveVoiceCount(), 1, "one active voice")


def test_voice_retires_when_clip_ends():
    """A one-shot must retire, freeing its slot. Otherwise the pool fills with
    silence and every later sound is dropped at the cap."""
    mixer = Audio.Mixer()
    mixer.RegisterClip("t", _tone(frames=32))
    voice_id = mixer.Play(_request("t"))
    check(mixer.IsPlaying(voice_id), "should start playing")
    mixer.Update(1.0)  # 1 second is far longer than 32 frames at 44.1 kHz
    check(not mixer.IsPlaying(voice_id), "a finished one-shot must retire")
    check_eq(mixer.GetActiveVoiceCount(), 0, "no active voices after retirement")


def test_loop_wraps_instead_of_retiring():
    mixer = Audio.Mixer()
    mixer.RegisterClip("t", _tone(frames=32))
    voice_id = mixer.Play(_request("t", Loop=True))
    mixer.Update(2.0)
    check(mixer.IsPlaying(voice_id), "a loop must not retire")
    check_eq(mixer.GetActiveVoiceCount(), 1, "loop still counts as one voice")


def test_negative_pitch_is_clamped():
    """A negative pitch would make the read position run backwards forever."""
    mixer = Audio.Mixer()
    mixer.RegisterClip("t", _tone(frames=44100))  # 1 second, so it survives the update
    voice_id = mixer.Play(_request("t", Pitch=-2.0))
    check(voice_id >= 0, "play with negative pitch should not fail outright")
    mixer.Update(0.5)
    # Negative pitch is clamped to 1, so the read position advances FORWARD.
    # A voice that ran backwards would either stall or never retire.
    check(mixer.IsPlaying(voice_id), "a clamped-pitch voice must still be playing")
    voice = next(v for v in mixer.GetVoices() if v.VoiceId == voice_id)
    check_ge(voice.FramePosition, 1.0, "playback position must advance, not reverse")


def test_extreme_pitch_does_not_hang():
    mixer = Audio.Mixer()
    mixer.RegisterClip("t", _tone(frames=64))
    mixer.Play(_request("t", Pitch=100.0, Loop=True))
    mixer.Update(1.0)
    # Mix() allocates and returns its own interleaved stereo buffer; it takes
    # no output argument, because a device wants a fresh block it hands on.
    buf = mixer.Mix(256)
    check_eq(len(buf), 512, "Mix returns 2 channels per frame")
    check(all(math.isfinite(v) for v in buf), "extreme pitch must not produce NaN/Inf")


# --------------------------------------------------------------------------
# Mixer: voice cap, priority, stealing
# --------------------------------------------------------------------------


def test_voice_cap_drops_when_nothing_can_be_stolen():
    # Ties are stealable BY DESIGN (see FindStealCandidate), so an equal-priority
    # overflow steals rather than drops. To observe a genuine DROP, fill every
    # slot with a LOOP, which is never stealable, then overflow.
    mixer = Audio.Mixer(4)
    mixer.RegisterClip("bed", _tone(frames=44100, amplitude=0.5))
    for _ in range(4):
        mixer.Play(_request("bed", Priority=10, Loop=True))
    check_eq(mixer.GetActiveVoiceCount(), 4, "pool should be full")

    dropped_before = mixer.GetDroppedVoiceCount()
    voice_id = mixer.Play(_request("bed", Priority=10))
    check_eq(voice_id, -1, "an unstealable full pool must drop the new sound")
    check_eq(mixer.GetActiveVoiceCount(), 4, "pool must not exceed its cap")
    check_eq(mixer.GetDroppedVoiceCount(), dropped_before + 1, "the drop must be counted")


def test_equal_priority_steals_rather_than_dropping():
    """The documented tie rule: the cap exists to bound one-shots, so the fourth
    footstep in a burst must displace an earlier one, not be lost."""
    mixer = Audio.Mixer(2)
    mixer.RegisterClip("t", _tone(frames=44100, amplitude=0.5))
    first = mixer.Play(_request("t", Priority=5))
    second = mixer.Play(_request("t", Priority=5))
    check(first >= 0 and second >= 0, "both should start")
    third = mixer.Play(_request("t", Priority=5))
    check(third >= 0, "an equal-priority overflow must be STEALED, not dropped")
    check_eq(mixer.GetActiveVoiceCount(), 2, "pool stays at its cap")
    check(mixer.GetStealedVoiceCount() > 0, "the steal must be counted")
    check_eq(mixer.GetDroppedVoiceCount(), 0, "a tie must not count as a drop")


def test_higher_priority_steals_from_lower():
    mixer = Audio.Mixer(2)
    mixer.RegisterClip("quiet", _tone(frames=44100, amplitude=0.1))
    low = mixer.Play(_request("quiet", Priority=0))
    check(low >= 0, "low-priority voice should start")
    # Fill the pool so the next play must steal rather than take a free slot.
    mixer.Play(_request("quiet", Priority=0))
    check_eq(mixer.GetActiveVoiceCount(), 2, "pool should be full")
    mixer.Play(_request("quiet", Priority=10))
    check_eq(mixer.GetActiveVoiceCount(), 2, "stealing must not grow the pool")
    check(mixer.GetStealedVoiceCount() > 0, "a steal must be counted")
    check(mixer.IsPlaying(low) is False, "the low-priority victim should be gone")


def test_loop_is_never_stolen():
    """A looping bed that gets stolen is a hole in the music, not a bug fix."""
    mixer = Audio.Mixer(2)
    mixer.RegisterClip("bed", _tone(frames=4096, amplitude=0.1))
    mixer.Play(_request("bed", Priority=0, Loop=True))
    for _ in range(4):
        mixer.Play(_request("bed", Priority=10))
    voices = [v for v in mixer.GetVoices() if v.IsPlaying()]
    check(any(v.Loop for v in voices), "the looping bed must survive")
