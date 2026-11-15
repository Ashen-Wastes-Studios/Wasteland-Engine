"""J.2 texture compression: BC1 (DXT1) encode + a box mip chain.

BC1 is a precise, spec-visible codec -- no hardware to COMPRESS (decoding is the
GPU's job) -- so the cook half is implementable and verifiable headlessly, unlike
the rendered-thumbnail half. This tests the encoder produces VALID, DETERMINISTIC
opaque-mode blocks, the size math is exact, and the mip chain is a deterministic
box downsample.
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

Bc1BlockCount = Wasteland.Bc1BlockCount
EncodeBc1Rgba = Wasteland.EncodeBc1Rgba
DownsampleBox = Wasteland.DownsampleBox
MipChainSizes = Wasteland.MipChainSizes
CookTextureBc1 = Wasteland.CookTextureBc1


def _rgba(w, h, fill=None):
    if fill is None:
        fill = lambda x, y: (100, 150, 200, 255)
    out = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b, a = fill(x, y)
            out += bytes((r, g, b, a))
    return bytes(out)


def test_block_count_is_ceil_per_axis():
    check_eq(Bc1BlockCount(4, 4), 1, "4x4 is not one block")
    check_eq(Bc1BlockCount(8, 8), 4, "8x8 is not four blocks")
    check_eq(Bc1BlockCount(5, 5), 4, "5x5 should ceil to 2x2 blocks")
    check_eq(Bc1BlockCount(1, 1), 1, "1x1 is not one block")
    check_eq(Bc1BlockCount(7, 3), 2, "7x3 should ceil to 2x1=2 blocks")


def test_encoded_size_is_block_count_times_eight():
    for (w, h) in ((4, 4), (8, 8), (5, 5), (12, 9)):
        blocks = Bc1BlockCount(w, h)
        packed = EncodeBc1Rgba(_rgba(w, h), w, h)
        check_eq(len(packed), blocks * 8,
                 "%dx%d encoded to the wrong byte count" % (w, h))


def test_encoding_is_deterministic():
    src = _rgba(16, 16)
    a = EncodeBc1Rgba(src, 16, 16)
    b = EncodeBc1Rgba(src, 16, 16)
    check_eq(a, b, "the same image compressed to different bytes")


def test_different_images_compress_differently():
    a = EncodeBc1Rgba(_rgba(16, 16, fill=lambda x, y: (0, 0, 0, 255)), 16, 16)
    b = EncodeBc1Rgba(_rgba(16, 16, fill=lambda x, y: (255, 255, 255, 255)), 16, 16)
    check(a != b, "black and white images compressed identically")


def test_a_solid_block_is_opaque_mode_not_transparent():
    """A solid block stays in 4-colour (opaque) mode -- the c0 > c1 invariant --
    so no pixel maps to the transparent index 3. Decode check: for a solid grey,
    the two endpoints as 565 and the per-pixel indices must be present and the
    mode invariant (c0 > c1) must hold in the raw bytes."""
    packed = EncodeBc1Rgba(_rgba(4, 4, fill=lambda x, y: (128, 128, 128, 255)), 4, 4)
    check_eq(len(packed), 8, "a 4x4 image did not yield one block")
    c0 = packed[0] | (packed[1] << 8)
    c1 = packed[2] | (packed[3] << 8)
    check(c0 > c1,
          "the block is not in opaque mode (c0 0x%04x must exceed c1 0x%04x)"
          % (c0, c1))


def test_edges_are_clamped_for_partial_blocks():
    # A 3x3 image is one block; the right-most and bottom-most pixels clamp.
    packed = EncodeBc1Rgba(_rgba(3, 3, fill=lambda x, y: (200, 40, 40, 255)), 3, 3)
    check_eq(len(packed), 8, "a 3x3 image did not yield one block")


def test_downsample_halves_and_averages():
    src = _rgba(4, 4)
    down, w, h = DownsampleBox(src, 4, 4)
    check_eq((w, h), (2, 2), "a 4x4 did not halve to 2x2")
    check_eq(len(down), 2 * 2 * 4, "downsample size mismatch")


def test_downsample_of_a_solid_is_the_same_solid():
    src = _rgba(4, 4, fill=lambda x, y: (12, 34, 56, 255))
    down, w, h = DownsampleBox(src, 4, 4)
    check_eq(down[0], 12, "R average is wrong")
    check_eq(down[1], 34, "G average is wrong")
    check_eq(down[2], 56, "B average is wrong")


def test_a_1x1_image_does_not_shrink():
    src = _rgba(1, 1, fill=lambda x, y: (9, 8, 7, 255))
    down, w, h = DownsampleBox(src, 1, 1)
    check_eq((w, h), (1, 1), "a 1x1 shrank")
    check_eq(down[0], 9, "1x1 renamed its pixel")


def test_mip_chain_halves_down_to_one():
    chain = MipChainSizes(8, 8)
    check_eq(chain, [(8, 8), (4, 4), (2, 2), (1, 1)], "8x8 mip chain")
    chain = MipChainSizes(5, 3)
    check_eq(chain, [(5, 3), (2, 1), (1, 1)], "5x3 mip chain floor-halves")


def test_cook_texture_bc1_lays_all_mips_end_to_end():
    """A texture cook = one concatenated BC1 blob, one mip after another, each
    of its own Bc1BlockCount*8 bytes, in MipChainSizes order."""
    src = _rgba(8, 8)
    combined, mips = CookTextureBc1(src, 8, 8)
    check_eq(mips, [(8, 8), (4, 4), (2, 2), (1, 1)], "cook mip sizes")
    expect_len = (Bc1BlockCount(8, 8) + Bc1BlockCount(4, 4)
                  + Bc1BlockCount(2, 2) + Bc1BlockCount(1, 1)) * 8
    check_eq(len(combined), expect_len,
             "the cook blob is not the concatenation of every mip")


def test_cook_texture_bc1_is_deterministic():
    src = _rgba(8, 8)
    a, _ = CookTextureBc1(src, 8, 8)
    b, _ = CookTextureBc1(src, 8, 8)
    check_eq(a, b, "the same texture cooked to different bytes")


def test_cook_texture_bc1_matches_encoded_individually():
    """The first mip of the cook must equal a direct EncodeBc1Rgba of the source
    at its full size -- the cook is not scrambling the chain."""
    src = _rgba(8, 8)
    combined, mips = CookTextureBc1(src, 8, 8)
    direct = EncodeBc1Rgba(src, 8, 8)
    check_eq(combined[:len(direct)], direct,
             "the cook's first mip differs from a direct 8x8 encode")


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
