"""Texture thumbnails: decode, aspect-fit letterbox, area-average resample.

Phase F.3 of the parity plan, headless half. The GL upload is NOT here -- it
needs a device and belongs to the panel. What is here is the part that can be
actually verified: turning a PNG on disk into a 256x256 RGBA buffer that obeys
the cache's contract.

The plan's gate for F.3 is "a texture thumbnail is not the fallback icon" and
"a corrupt asset yields the icon, not a crash". Both are in this file, plus the
geometry that makes the first one meaningful: a 16x9 texture must NOT be
stretched into a square cell, because a grid of stretched thumbnails is the
most obvious visual defect in a content browser.
"""
import atexit
import os
import shutil
import struct
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runner import check, check_eq

import wlpath

wlpath.setup()

import Wasteland

Editor = Wasteland.Editor
ThumbnailKind = Editor.ThumbnailKind

_TMP_DIRS = []


def _cleanup():
    for path in _TMP_DIRS:
        shutil.rmtree(path, ignore_errors=True)


atexit.register(_cleanup)


def _tempdir():
    path = tempfile.mkdtemp(prefix="wasteland_texthumb_")
    _TMP_DIRS.append(path)
    return path


# ---------------------------------------------------------------------------
# A minimal PNG writer
# ---------------------------------------------------------------------------
# Hand-written because the test interpreter is isolated and ships no PIL. This
# is the same reasoning as the engine's own hand-written WritePNG: an image
# dependency would be a new requirement to install, version and keep in sync.


def _png_chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def _png(width, height, pixel_at, channels=4):
    """Encode an image as PNG. `pixel_at(x, y)` returns a tuple of `channels`."""
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type 0 = None
        for x in range(width):
            raw.extend(pixel_at(x, y))
    colour_type = {1: 0, 3: 2, 4: 6}[channels]
    ihdr = struct.pack(">IIBBBBB", width, height, 8, colour_type, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + _png_chunk(b"IHDR", ihdr) +
            _png_chunk(b"IDAT", zlib.compress(bytes(raw))) +
            _png_chunk(b"IEND", b""))


def _write_png(name, width, height, pixel_at, channels=4):
    path = os.path.join(_tempdir(), name)
    with open(path, "wb") as handle:
        handle.write(_png(width, height, pixel_at, channels))
    return path


def _pixel(entry, x, y):
    index = (y * entry.Width + x) * 4
    pixels = entry.Pixels
    return (pixels[index], pixels[index + 1], pixels[index + 2], pixels[index + 3])


_BACKGROUND = (32, 34, 40)


# ---------------------------------------------------------------------------
# Decode
# ---------------------------------------------------------------------------

def test_load_image_decodes_a_png_to_rgba():
    path = _write_png("solid.png", 8, 6, lambda x, y: (200, 100, 50, 255))
    image = Editor.LoadImage(path)
    check(image.Ok(), "a valid PNG failed to decode: %s" % image.Error)
    check_eq(image.Width, 8, "decoded width")
    check_eq(image.Height, 6, "decoded height")
    check_eq(len(image.Pixels), 8 * 6 * 4, "decoded as RGBA, four channels per pixel")
    index = (3 * 8 + 2) * 4
    check_eq(tuple(image.Pixels[index:index + 4]), (200, 100, 50, 255),
             "the pixel round-trips through the PNG encoder")


def test_load_image_from_memory_agrees_with_load_image():
    """The disk cache reads a cached thumbnail back from BYTES, so the memory
    path is the one that actually runs in production -- it must agree with the
    file path exactly."""
    path = _write_png("mem.png", 5, 4, lambda x, y: (10, 20, 30, 255))
    with open(path, "rb") as handle:
        data = handle.read()

    from_file = Editor.LoadImage(path)
    from_memory = Editor.LoadImageFromMemory(data)

    check(from_file.Ok(), "file decode failed: %s" % from_file.Error)
    check(from_memory.Ok(), "memory decode failed: %s" % from_memory.Error)
    check_eq(from_memory.Width, from_file.Width, "memory width")
    check_eq(from_memory.Height, from_file.Height, "memory height")
    check_eq(list(from_memory.Pixels), list(from_file.Pixels),
             "memory and file decodes are byte-identical")


def test_a_greyscale_png_is_still_returned_as_rgba():
    """stb_image returns the file's NATIVE channel count unless told otherwise,
    so a greyscale PNG would come back 1-channel and every consumer assuming
    RGBA would read three bytes past the buffer."""
    path = _write_png("grey.png", 4, 4, lambda x, y: (128,), channels=1)
    image = Editor.LoadImage(path)
    check(image.Ok(), "greyscale decode failed: %s" % image.Error)
    check_eq(len(image.Pixels), 4 * 4 * 4, "greyscale expanded to four channels")
    check_eq(tuple(image.Pixels[0:4]), (128, 128, 128, 255),
             "greyscale becomes RGBA with full alpha")


def test_a_corrupt_file_reports_an_error_and_does_not_raise():
    """The plan's negative control: a corrupt asset yields the icon, not a
    crash. An exception out of LoadImage would propagate through the UI pass."""
    path = os.path.join(_tempdir(), "broken.png")
    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n" + b"not a real png at all")

    image = Editor.LoadImage(path)
    check(not image.Ok(), "a corrupt PNG reported success")
    check(len(image.Error) > 0, "a corrupt PNG gave no error message")
    check_eq(image.Width, 0, "a corrupt PNG reports zero width")


def test_a_missing_file_reports_an_error_and_does_not_raise():
    image = Editor.LoadImage(os.path.join(_tempdir(), "does-not-exist.png"))
    check(not image.Ok(), "a missing file reported success")
    check(len(image.Error) > 0, "a missing file gave no error message")


# ---------------------------------------------------------------------------
# Letterbox geometry
# ---------------------------------------------------------------------------

def test_a_wide_texture_is_letterboxed_above_and_below():
    """A 2:1 texture in a square cell: full width, half height, centred. The
    plan's contract -- the longer axis fills the square, the shorter is centred,
    and stretching is forbidden."""
    path = _write_png("wide.png", 512, 256, lambda x, y: (255, 0, 0, 255))
    entry = Editor.MakeTextureThumbnail(Editor.LoadImage(path), 256,
                                        _BACKGROUND[0], _BACKGROUND[1],
                                        _BACKGROUND[2])

    check_eq(entry.Width, 256, "thumbnail width")
    check_eq(entry.Height, 256, "thumbnail height")
    check_eq(len(entry.Pixels), 256 * 256 * 4, "thumbnail is RGBA")
    check_eq(_pixel(entry, 128, 128), (255, 0, 0, 255), "the centre is image")

    # Rows 0..63 and 192..255 are background: the image occupies rows 64..191.
    check_eq(_pixel(entry, 128, 0), _BACKGROUND + (255,), "top row is letterbox")
    check_eq(_pixel(entry, 128, 63), _BACKGROUND + (255,), "row 63 is letterbox")
    check_eq(_pixel(entry, 128, 64), (255, 0, 0, 255), "row 64 is image")
    check_eq(_pixel(entry, 128, 191), (255, 0, 0, 255), "row 191 is image")
    check_eq(_pixel(entry, 128, 192), _BACKGROUND + (255,), "row 192 is letterbox")
    check_eq(_pixel(entry, 0, 0), _BACKGROUND + (255,), "the corner is letterbox")


def test_a_tall_texture_is_letterboxed_left_and_right():
    path = _write_png("tall.png", 256, 512, lambda x, y: (0, 255, 0, 255))
    entry = Editor.MakeTextureThumbnail(Editor.LoadImage(path), 256,
                                        _BACKGROUND[0], _BACKGROUND[1],
                                        _BACKGROUND[2])

    check_eq(_pixel(entry, 128, 128), (0, 255, 0, 255), "the centre is image")
    # The image occupies columns 64..191; 0..63 and 192..255 are letterbox.
    check_eq(_pixel(entry, 0, 128), _BACKGROUND + (255,), "column 0 is letterbox")
    check_eq(_pixel(entry, 63, 128), _BACKGROUND + (255,), "column 63 is letterbox")
    check_eq(_pixel(entry, 64, 128), (0, 255, 0, 255), "column 64 is image")
    check_eq(_pixel(entry, 191, 128), (0, 255, 0, 255), "column 191 is image")
    check_eq(_pixel(entry, 192, 128), _BACKGROUND + (255,), "column 192 is letterbox")


def test_a_square_texture_fills_the_cell_with_no_letterbox():
    path = _write_png("square.png", 128, 128, lambda x, y: (0, 0, 255, 255))
    entry = Editor.MakeTextureThumbnail(Editor.LoadImage(path), 256,
                                        _BACKGROUND[0], _BACKGROUND[1],
                                        _BACKGROUND[2])
    for corner in ((0, 0), (255, 0), (0, 255), (255, 255)):
        check_eq(_pixel(entry, corner[0], corner[1]), (0, 0, 255, 255),
                 "corner %s is image, not letterbox" % (corner,))


# ---------------------------------------------------------------------------
# Resample quality
# ---------------------------------------------------------------------------

def test_a_uniform_texture_survives_the_downscale():
    """A constant-colour image is the area-average's identity case: any
    resampler that drifts here is broken, and it is the cheapest possible
    check of that."""
    path = _write_png("uniform.png", 512, 512, lambda x, y: (77, 88, 99, 255))
    entry = Editor.MakeTextureThumbnail(Editor.LoadImage(path), 256,
                                        _BACKGROUND[0], _BACKGROUND[1],
                                        _BACKGROUND[2])
    for probe in ((0, 0), (128, 128), (255, 255), (64, 200)):
        check_eq(_pixel(entry, probe[0], probe[1]), (77, 88, 99, 255),
                 "probe %s keeps the source colour" % (probe,))


def test_an_alternating_pattern_downscales_to_its_area_average():
    """The strong test. A 256px checkerboard of black/white columns, downscaled
    4:1, must average to 128 at every pixel: each destination pixel covers two
    black and two white source columns.

    Nearest-neighbour would give 0 or 255 -- an obvious checkerboard at 64px.
    A bilinear sampler with the wrong phase would give the same. Only an
    area average gives 128 everywhere."""
    path = _write_png("checker.png", 256, 256,
                      lambda x, y: (255, 255, 255, 255) if x % 2 == 0
                      else (0, 0, 0, 255))
    entry = Editor.MakeTextureThumbnail(Editor.LoadImage(path), 64,
                                        _BACKGROUND[0], _BACKGROUND[1],
                                        _BACKGROUND[2])

    worst = 0
    for y in range(0, 64, 7):
        for x in range(0, 64, 7):
            pixel = _pixel(entry, x, y)
            for channel in range(3):
                worst = max(worst, abs(pixel[channel] - 128))
    check(worst <= 2,
          "the checkerboard averaged to a grey off by %d levels, expected <=2 "
          "(nearest or wrong-phase bilinear gives 0/255)" % worst)


def test_transparency_does_not_bleed_a_background_colour():
    """A fully transparent image must come back as the letterbox colour, not as
    a black square. The resample premultiplies by alpha for exactly this
    reason; a straight average would return (0,0,0,0) and the panel would
    composite it over whatever is behind the cell."""
    path = _write_png("transparent.png", 64, 64, lambda x, y: (255, 0, 0, 0))
    entry = Editor.MakeTextureThumbnail(Editor.LoadImage(path), 64,
                                        _BACKGROUND[0], _BACKGROUND[1],
                                        _BACKGROUND[2])
    check_eq(_pixel(entry, 32, 32), _BACKGROUND + (255,),
             "a transparent image shows the letterbox colour")


def test_a_partially_transparent_edge_keeps_its_own_colour():
    """A half-transparent red pixel must not darken toward black."""
    path = _write_png("semitrans.png", 2, 2, lambda x, y: (255, 0, 0, 128))
    entry = Editor.MakeTextureThumbnail(Editor.LoadImage(path), 2,
                                        _BACKGROUND[0], _BACKGROUND[1],
                                        _BACKGROUND[2])
    pixel = _pixel(entry, 1, 1)
    check(pixel[0] >= 200,
          "a half-transparent red pixel lost its colour: got %r" % (pixel,))
    check(abs(pixel[3] - 128) <= 2,
          "alpha did not survive the resample: got %d" % pixel[3])


# ---------------------------------------------------------------------------
# The plan's F.3 gate
# ---------------------------------------------------------------------------

def test_a_texture_thumbnail_is_not_the_fallback_icon():
    """The plan's own gate for F.3. Without it, a generator that always returned
    the icon would pass every other test here."""
    path = _write_png("photo.png", 64, 64, lambda x, y: (150, 160, 170, 255))
    entry = Editor.MakeTextureThumbnail(Editor.LoadImage(path), 64,
                                        _BACKGROUND[0], _BACKGROUND[1],
                                        _BACKGROUND[2])
    icon = Editor.GenerateIcon(ThumbnailKind.Texture)

    check_eq(entry.Kind, ThumbnailKind.Texture, "the entry is a texture")
    check(entry.Ready, "the entry is ready")
    check(bytes(entry.Pixels) != bytes(icon.Pixels),
          "a real texture produced the fallback icon")
    # And it is a real texture: uniform 150,160,170, nothing like the icon's
    # four-colour silhouette.
    check_eq(_pixel(entry, 32, 32), (150, 160, 170, 255),
             "the thumbnail shows the source image")


def test_a_failed_decode_yields_the_fallback_icon_not_a_crash():
    """The plan's negative control: a corrupt asset yields the icon, not a
    crash. MakeTextureThumbnail on a failed decode must return a Failed entry
    that the caller can replace with GenerateIcon(Texture)."""
    broken = Editor.LoadImage(os.path.join(_tempdir(), "missing.png"))
    check(not broken.Ok(), "the fixture must be a failed decode")
    entry = Editor.MakeTextureThumbnail(broken, 64,
                                        _BACKGROUND[0], _BACKGROUND[1],
                                        _BACKGROUND[2])
    check(entry.Failed, "a failed decode is reported as Failed")
    check_eq(entry.Width, 64, "a failed entry still has the cell geometry")
    check_eq(len(entry.Pixels), 64 * 64 * 4, "a failed entry still has pixels")

    # The caller's fallback path.
    icon = Editor.GenerateIcon(ThumbnailKind.Texture)
    check(not icon.IsEmpty(), "the fallback icon exists for a failed decode")


def test_the_thumbnail_background_default_is_the_browser_panel_colour():
    """The letterbox is the panel colour, not black or white, so an
    aspect-mismatched thumbnail does not read as a missing image."""
    background = Editor.ThumbnailBackground()
    check_eq((background.R, background.G, background.B), (32, 34, 40),
             "the default letterbox colour")


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
