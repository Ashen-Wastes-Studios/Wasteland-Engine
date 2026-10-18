"""Build RIFF/WAVE byte streams in memory, for the audio loader tests.

Every WAV in the suite is constructed here rather than shipped as a binary
fixture. Two reasons: a fixture is opaque (a failing assertion tells you a
number is wrong but not what was encoded), and a generated file makes the
*intent* of each case readable — "8-bit unsigned silence" is a two-line call,
whereas a 78-byte blob is not.

The writer is deliberately dumb and literal. It does not pad chunks to even
lengths unless asked, and it does not insert a LIST chunk unless asked, because
those are exactly the malformed inputs the loader has to survive.
"""

from __future__ import annotations

import struct
from typing import Iterable, Sequence

FMT_PCM = 1
FMT_IEEE_FLOAT = 3
FMT_EXTENSIBLE = 0xFFFE


def _chunk(chunk_id: bytes, payload: bytes, *, pad: bool = True) -> bytes:
    """A RIFF chunk: 4-byte id, 4-byte LE size, payload, optional pad byte.

    Chunks are word-aligned in the RIFF spec: an odd-sized payload is followed
    by a pad byte that is NOT counted in the size field. Set ``pad=False`` to
    deliberately violate that and test the loader's resynchronisation.
    """
    out = chunk_id + struct.pack("<I", len(payload)) + payload
    if pad and len(payload) % 2 == 1:
        out += b"\x00"
    return out


def fmt_chunk(
    *,
    format_tag: int = FMT_PCM,
    channels: int = 1,
    sample_rate: int = 44100,
    bits_per_sample: int = 16,
    extensible_subformat: int | None = None,
) -> bytes:
    """A ``fmt `` chunk.

    ``extensible_subformat`` builds a WAVE_FORMAT_EXTENSIBLE header, which hides
    the real codec in the tail of a 16-byte GUID. That is the case where a naive
    loader reads format tag 0xFFFE and gives up.
    """
    if extensible_subformat is None:
        body = struct.pack(
            "<HHIIHH",
            format_tag,
            channels,
            sample_rate,
            sample_rate * channels * bits_per_sample // 8,  # byte rate
            channels * bits_per_sample // 8,  # block align
            bits_per_sample,
        )
    else:
        # cbSize(2) + validBits(2) + channelMask(4) + SubFormat GUID(16)
        guid = struct.pack("<I", extensible_subformat) + b"\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71"
        body = struct.pack(
            "<HHIIHH",
            FMT_EXTENSIBLE,
            channels,
            sample_rate,
            sample_rate * channels * bits_per_sample // 8,
            channels * bits_per_sample // 8,
            bits_per_sample,
        )
        body += struct.pack("<HHI", 22, bits_per_sample, 0x3) + guid
    return _chunk(b"fmt ", body)


def data_chunk(samples: bytes, *, declared_size: int | None = None, pad: bool = True) -> bytes:
    """A ``data`` chunk.

    ``declared_size`` writes a size that disagrees with the payload length,
    which is how a truncated or over-claiming file is simulated.
    """
    size = len(samples) if declared_size is None else declared_size
    out = b"data" + struct.pack("<I", size) + samples
    if pad and len(samples) % 2 == 1:
        out += b"\x00"
    return out


def riff(chunks: Iterable[bytes], *, riff_id: bytes = b"RIFF", wave_id: bytes = b"WAVE") -> bytes:
    """Assemble a RIFF container around already-encoded chunks.

    The RIFF size field counts everything after itself, i.e. the ``WAVE`` tag
    plus every chunk including their headers and pad bytes.
    """
    body = wave_id + b"".join(chunks)
    return riff_id + struct.pack("<I", len(body)) + body


# ---- sample encoders -------------------------------------------------------
#
# Each takes float samples in [-1, 1] and returns the raw bytes for the `data`
# chunk. They are the *inverse* of the loader's SampleToFloat, written
# independently so a shared misreading cannot make a test pass falsely.


def encode_pcm8(samples: Sequence[float]) -> bytes:
    """8-bit UNSIGNED. Silence is 128, not 0 — the classic off-by-128 bug."""
    out = bytearray()
    for s in samples:
        v = int(round(max(-1.0, min(1.0, s)) * 127.0)) + 128
        out.append(max(0, min(255, v)))
    return bytes(out)


def encode_pcm16(samples: Sequence[float]) -> bytes:
    out = bytearray()
    for s in samples:
        v = int(round(max(-1.0, min(1.0, s)) * 32767.0))
        out += struct.pack("<h", max(-32768, min(32767, v)))
    return bytes(out)


def encode_pcm24(samples: Sequence[float]) -> bytes:
    """24-bit packed into the low 3 bytes of a 4-byte word's worth of range."""
    out = bytearray()
    for s in samples:
        v = int(round(max(-1.0, min(1.0, s)) * 8388607.0))
        v = max(-8388608, min(8388607, v))
        out += struct.pack("<i", v)[:3]  # low 3 bytes, little-endian
    return bytes(out)


def encode_pcm32(samples: Sequence[float]) -> bytes:
    out = bytearray()
    for s in samples:
        v = int(round(max(-1.0, min(1.0, s)) * 2147483647.0))
        out += struct.pack("<i", max(-2147483648, min(2147483647, v)))
    return bytes(out)


def encode_float32(samples: Sequence[float]) -> bytes:
    out = bytearray()
    for s in samples:
        out += struct.pack("<f", s)
    return bytes(out)


ENCODERS = {
    (FMT_PCM, 8): encode_pcm8,
    (FMT_PCM, 16): encode_pcm16,
    (FMT_PCM, 24): encode_pcm24,
    (FMT_PCM, 32): encode_pcm32,
    (FMT_IEEE_FLOAT, 32): encode_float32,
}


def simple_wav(
    samples: Sequence[float],
    *,
    bits: int = 16,
    channels: int = 1,
    sample_rate: int = 44100,
    float_fmt: bool = False,
    interleave: bool = True,
) -> bytes:
    """Build a complete, valid WAV.

    ``interleave=False`` with ``channels=2`` lays the two channels out as two
    concatenated blocks instead of interleaved frames, which is malformed and
    exists to prove the loader does not silently accept it as valid audio.
    """
    tag = FMT_IEEE_FLOAT if float_fmt else FMT_PCM
    encoder = ENCODERS[(tag, bits)]

    if channels == 1:
        payload = encoder(samples)
    else:
        frames = len(samples) // channels
        if interleave:
            # Proper interleaved frames: encode each channel, then weave them
            # back together at sample granularity.
            lanes = [encoder(samples[ch::channels]) for ch in range(channels)]
            sample_bytes = len(lanes[0]) // frames
            merged = bytearray()
            for i in range(frames):
                for ch in range(channels):
                    off = i * sample_bytes
                    merged += lanes[ch][off : off + sample_bytes]
            payload = bytes(merged)
        else:
            # Deliberately malformed: planar blocks instead of interleaved.
            payload = b"".join(encoder(samples[ch::channels]) for ch in range(channels))

    return riff([fmt_chunk(format_tag=tag, channels=channels,
                           sample_rate=sample_rate, bits_per_sample=bits),
                 data_chunk(payload)])


def silence(n: int = 16, *, bits: int = 16, channels: int = 1,
            sample_rate: int = 44100, float_fmt: bool = False) -> bytes:
    return simple_wav([0.0] * (n * channels), bits=bits, channels=channels,
                      sample_rate=sample_rate, float_fmt=float_fmt)


def sine(n: int, freq: float = 440.0, sample_rate: int = 44100,
         amplitude: float = 0.5, *, bits: int = 16, channels: int = 1,
         float_fmt: bool = False) -> bytes:
    import math

    step = 2.0 * math.pi * freq / sample_rate
    return simple_wav([amplitude * math.sin(step * i) for i in range(n)],
                      bits=bits, channels=channels, sample_rate=sample_rate,
                      float_fmt=float_fmt)


def list_chunk(payload: bytes = b"INFOhello") -> bytes:
    """A LIST chunk, which the loader must skip rather than read as samples."""
    return _chunk(b"LIST", payload)
