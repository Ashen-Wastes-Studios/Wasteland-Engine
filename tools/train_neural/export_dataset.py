#!/usr/bin/env python3
"""Export matched (input, ground-truth) frame pairs from the Wasteland engine.

This is the data source for the clean-room neural rendering stage described in
``docs/NEURAL_RENDERING.md``. Everything here is the engine's own output: the
"clean room" requirement is satisfied by construction, because no third-party
imagery ever enters the tree -- the targets are rendered by this renderer, from
these scenes, by this binary.

How a capture works
-------------------
The editor has a headless capture harness driven entirely by environment
variables (see ``EditorLayer::OnAttach``):

    WASTELAND_PIN_CAMERA   <fx,fy,fz>,<distance>,<pitch>,<yaw>
    WL_CAPTURE_SCENE       <path>   scene to load (absolute or project-relative)
    WL_CAPTURE_PRESET      <0..3>   quality preset
    WL_CAPTURE_SCALE       <f>      render scale, 0.25..1.0
    WL_CAPTURE_NEURAL      <0|1>    in-sample neural rendering
    WL_CAPTURE_ACCUM       <0|1>    temporal accumulation
    WL_DRS_OFF             present  disables dynamic resolution
    WL_SCREENSHOT          <path>   where to write the PNG
    WL_SCREENSHOT_FRAME    <n>      first frame of the capture window
    WL_SCREENSHOT_LINGER   <n>      close n frames after the shot lands

The process closes itself once the linger elapses, so this script waits for
exit rather than killing it -- a killed process can be torn down mid-readback.

Why the pairs are aligned
-------------------------
Both passes of a pair use the SAME scene, the SAME pinned camera and the SAME
capture frame index. Jitter is seeded by the frame index, so a fixed index means
a fixed sub-pixel offset. The two passes differ in exactly one thing, the render
scale, which is the degradation the neural stage exists to undo. Nothing about
the pair depends on wall-clock time, mouse state or the editor's own camera.

The one thing this cannot guarantee is a camera that sees the scene. A pin that
looks at open sky produces a smooth, near-constant image that is a perfectly
valid render and a perfectly useless training target. Rather than trust the
caller, every capture is measured: ``--validate`` and the export itself reject
frames whose local contrast is below a floor, and record the measurement in the
manifest so a bad camera is visible in the data instead of discovered in a
training curve.

Usage
-----
    python tools/train_neural/export_dataset.py             # export
    python tools/train_neural/export_dataset.py --dry-run   # print the plan
    python tools/train_neural/export_dataset.py --validate  # check a dataset
    python tools/train_neural/export_dataset.py --scenes scenes/Example3D.wastescene

Stdlib only: the repo pins no imaging dependency, and a PNG reader is ~40 lines.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import statistics
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parents[2]
EDITOR_DIR = REPO_ROOT / "bin" / "Release-windows-x86_64" / "DemonCore-Editor"
EDITOR_EXE = EDITOR_DIR / "DemonCore-Editor.exe"

# Scenes that are 2D-only or otherwise have no 3D content worth training on.
SCENE_EXCLUDE = {"Example2D.wastescene"}

# Cameras, as (name, focal_xyz, distance, pitch, yaw). The editor camera's
# pitch is degrees from horizontal, yaw degrees around +Y, matching
# EditorCamera::MouseRotate.
#
# Used only with --frame preset. The DEFAULT is --frame auto, which derives the
# framing from the scene's own content, because these hardcoded values only work
# for scenes of a similar extent -- the first version of this list was tuned on
# Example3D and rendered nothing at all for DemoGame and WaterShowcase.
CAMERAS = [
    ("cam_down", (0.0, 0.0, 0.0), 9.0, -85.0, 0.0),
    ("cam_low", (0.0, 1.5, 0.0), 12.0, -25.0, 30.0),
    ("cam_orbit", (0.0, 2.0, 0.0), 16.0, -18.0, 140.0),
]

# Auto-framing: how far outside the content to stand, as a multiple of the
# median entity distance from the content centre, and the clamp on either side.
# 2.5x puts a compact cluster in frame with margin; the floor keeps a scene of
# 1 m props from being framed so close that one object fills the view.
#
# The ceiling is PER SCENE, not a constant, and that is the whole point. A fixed
# 150 m ceiling is right for a compact scene and catastrophic for an open world:
# DemoGame's props are spread over 2.6 km (median entity distance 924 m), so 150 m
# framed a patch of near-black ground with two quads in a void -- contrast p99
# 2.5, and the scene genuinely has geometry. At 1228 m the same scene measures
# p99 26.3. Scaling the ceiling with the scene's own p90 distance fixes it.
AUTO_DISTANCE_FACTOR = 2.5
AUTO_DISTANCE_MIN = 12.0
AUTO_DISTANCE_MAX_FLOOR = 150.0

# Yaw angles tried at the derived distance, all pitched down. A single yaw is
# not enough: which side of a cluster has content is not predictable from the
# bounding box, and a camera on the wrong side sees the back of one object and
# open space behind it.
AUTO_YAWS = (0.0, 90.0, 200.0)
AUTO_PITCH = -30.0

# A capture whose local contrast is below this is a camera pointed at nothing.
# Measured on a known-good capture of Example3D (7 instances, floor + props):
# p99 local contrast ~16, median ~1. A camera pointed at open sky measures
# p99 below 2. The floor sits an order of magnitude below the good case and
# above the bad one, so it separates them without being tuned to either.
#
# NOTE what this actually measures: EDGES, not content. A flat water plane or a
# featureless ground also scores low, and that is a correct render of genuinely
# low-detail content. A low score means "nothing here to reconstruct", whether
# the cause is sky or a blank surface -- treat it as a signal to move the
# camera, not as proof of a rendering defect.
MIN_LOCAL_CONTRAST_P99 = 4.0
# And an absolute luma floor, so a genuinely dark-but-valid frame is not
# rejected while an uninitialised one is.
MIN_MEAN_LUMA = 1.0

# The converged frame index. Past frame 4 the accumulation alpha drops to its
# 0.02 floor, so the accumulated image is a running average over roughly the
# last 50 frames and the sub-pixel jitter has been averaged out. 400 is far
# enough past that to be at steady state, and cheap: the editor runs at ~15 fps
# headless, so this is under 30 s per capture.
#
# This frame index is load-bearing for BOTH the converged and the single-sample
# passes. It is not just "how long to let the accumulation settle": it is also
# the first frame at which the capture has a STABLE SIZE and a fully loaded
# scene. Measured on Example3D, camera auto_y0, scale 0.5:
#
#     frame  1 -> blank (mean luma 0.0, contrast p99 0.0)
#     frame  2 -> 1280x720     (a default, not the render target)
#     frame  3 -> 3840x2092    (the window, chrome included)
#     frame  6 -> 2332x1451    (settled)
#
# So a single-sample pass at frame 1 captures nothing, and one at frame 2 or 3
# captures the WRONG SIZE -- and the size is not just cosmetic: the pair is
# compared pixel-wise against the gt, so a differently-sized input silently
# becomes a resize artefact in the training data. Both failure modes are
# invisible in the manifest's mean_luma until the size is checked.
CONVERGED_FRAME = 400

# A single-sample capture: no temporal blend, so one frame's worth of noise.
#
# Deliberately the SAME frame as the converged passes, with accumulation off.
# The "noisy" property comes from the missing temporal blend, not from the
# frame being early -- and only a late frame has a settled size and a loaded
# scene. This is the pass the neural stage actually has work to do on: the
# converged `input` is already within 0.1 luma of the gt (affine headroom
# +0.08 dB), while a single frame sits ~5 dB below it (affine headroom
# +5.1 dB). Training on converged pairs teaches the network to undo a
# brightness difference that the renderer no longer has.
NOISY_FRAME = CONVERGED_FRAME

# Passes. `gt` is the reconstruction target; `input` is what the network sees.
# Both are converged by default so the pair differs ONLY in render scale --
# the cleanest possible pairing, and the one whose alignment is easiest to
# reason about. `noisy` is the harder variant: a single low-res sample, which
# is where the headroom is.
PASSES = {
    "gt": dict(scale=1.0, accumulate=True, frame=CONVERGED_FRAME, neural=1),
    "input": dict(scale=0.5, accumulate=True, frame=CONVERGED_FRAME, neural=1),
    "noisy": dict(scale=0.5, accumulate=False, frame=NOISY_FRAME, neural=1),
}

MANIFEST_VERSION = 1

# Capture attempts per frame before giving up. Two is enough for the transient
# GPU resets this engine is prone to on Ultra, and cheap: a failed attempt dies
# in seconds, not minutes.
DEFAULT_ATTEMPTS = 2


# --------------------------------------------------------------------------
# Minimal PNG reader (stdlib only)
# --------------------------------------------------------------------------

_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def read_png(path: Path) -> dict:
    """Return {width, height, channels, pixels} for a non-interlaced PNG.

    Supports 8-bit greyscale, greyscale+alpha, RGB, RGBA and palette. Palette
    is expanded to RGB because the editor's writer emits colour type 2 or 6 and
    nothing here needs the palette path -- it exists so a palette PNG from some
    other tool does not crash the validator.
    """
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")

    pos = 8
    idat = bytearray()
    width = height = bit_depth = color_type = interlace = None
    palette = None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos : pos + 4])
        ctype = data[pos + 4 : pos + 8]
        chunk = data[pos + 8 : pos + 8 + length]
        if ctype == b"IHDR":
            width, height, bit_depth, color_type, _comp, _filt, interlace = struct.unpack(
                ">IIBBBBB", chunk
            )
        elif ctype == b"PLTE":
            palette = [tuple(chunk[i : i + 3]) for i in range(0, len(chunk), 3)]
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
        pos += 12 + length

    if width is None:
        raise ValueError(f"{path}: no IHDR")
    if bit_depth != 8:
        raise ValueError(f"{path}: {bit_depth}-bit PNG unsupported (need 8)")
    if interlace != 0:
        raise ValueError(f"{path}: interlaced PNG unsupported")
    if color_type not in _CHANNELS:
        raise ValueError(f"{path}: colour type {color_type} unsupported")

    channels = _CHANNELS[color_type]
    stride = width * channels
    raw = zlib.decompress(bytes(idat))

    out = bytearray(height * stride)
    prev = bytearray(stride)
    src = 0
    for y in range(height):
        filter_type = raw[src]
        src += 1
        line = bytearray(raw[src : src + stride])
        src += stride
        if filter_type == 1:  # Sub
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_type == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filter_type == 3:  # Average
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif filter_type == 4:  # Paeth
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        elif filter_type != 0:
            raise ValueError(f"{path}: unknown filter {filter_type} on row {y}")
        out[y * stride : (y + 1) * stride] = line
        prev = line

    # Normalise to RGB. Greyscale replicates; alpha is dropped; palette expands.
    if color_type == 3:
        if palette is None:
            raise ValueError(f"{path}: palette PNG with no PLTE")
        rgb = bytearray(width * height * 3)
        for i in range(width * height):
            rgb[i * 3 : i * 3 + 3] = palette[out[i]][:3]
        return dict(width=width, height=height, channels=3, pixels=bytes(rgb))
    if channels == 1:
        rgb = bytearray(width * height * 3)
        for i in range(width * height):
            v = out[i]
            rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = v
        return dict(width=width, height=height, channels=3, pixels=bytes(rgb))
    if channels == 2:
        rgb = bytearray(width * height * 3)
        for i in range(width * height):
            v = out[i * 2]
            rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = v
        return dict(width=width, height=height, channels=3, pixels=bytes(rgb))
    if channels == 4:
        rgb = bytearray(width * height * 3)
        for i in range(width * height):
            rgb[i * 3 : i * 3 + 3] = out[i * 4 : i * 4 + 3]
        return dict(width=width, height=height, channels=3, pixels=bytes(rgb))
    return dict(width=width, height=height, channels=3, pixels=bytes(out))


def measure(png: dict) -> dict:
    """Luma statistics plus a local-contrast p99.

    Local contrast is the mean absolute horizontal difference a few pixels
    apart, sampled on a stride so a 2332x1451 frame stays cheap. It is the
    discriminator between "a render of a scene" and "a render of open sky":
    geometry produces edges, sky does not.
    """
    w, h = png["width"], png["height"]
    px = png["pixels"]
    total = w * h

    luma = [0.0] * total
    sums = [0, 0, 0]
    for i in range(total):
        r = px[i * 3]
        g = px[i * 3 + 1]
        b = px[i * 3 + 2]
        sums[0] += r
        sums[1] += g
        sums[2] += b
        luma[i] = 0.2126 * r + 0.7152 * g + 0.0722 * b

    diffs = []
    step = 4
    for y in range(0, h, 7):
        row = y * w
        for x in range(step, w - step, 7):
            diffs.append(abs(luma[row + x + step] - luma[row + x - step]))
    diffs.sort()

    return dict(
        mean_luma=sum(luma) / total,
        max_luma=max(luma),
        min_luma=min(luma),
        mean_rgb=[s / total for s in sums],
        contrast_p99=diffs[int(len(diffs) * 0.99)] if diffs else 0.0,
        contrast_max=diffs[-1] if diffs else 0.0,
    )


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


# --------------------------------------------------------------------------
# Capture
# --------------------------------------------------------------------------


def build_env(scene: str, camera: tuple, pass_name: str, out_path: Path) -> dict:
    name, focal, distance, pitch, yaw = camera
    spec = PASSES[pass_name]
    env = dict(os.environ)
    env.update(
        {
            "WASTELAND_PIN_CAMERA": "%g,%g,%g,%g,%g,%g"
            % (focal[0], focal[1], focal[2], distance, pitch, yaw),
            "WL_CAPTURE_SCENE": scene,
            "WL_CAPTURE_PRESET": "3",
            "WL_CAPTURE_SCALE": repr(spec["scale"]),
            "WL_CAPTURE_NEURAL": str(spec["neural"]),
            "WL_CAPTURE_ACCUM": "1" if spec["accumulate"] else "0",
            "WL_DRS_OFF": "1",
            "WL_SCREENSHOT": str(out_path),
            "WL_SCREENSHOT_FRAME": str(spec["frame"]),
            # Long enough for the shot to land inside the retry window, short
            # enough not to sit there after it has. The process closes itself.
            "WL_SCREENSHOT_LINGER": "6",
        }
    )
    return env


def capture(scene: str, camera: tuple, pass_name: str, out_path: Path, timeout: int,
            attempts: int = DEFAULT_ATTEMPTS) -> dict:
    """Run one headless capture, retrying on failure.

    A failed capture is almost always a transient GPU hiccup rather than a bad
    scene: the editor dies mid-run -- a device reset / TDR is a known failure
    mode on Ultra -- and the PNG never appears. Retrying costs a few seconds and
    turns a 13-minute export that dies at frame 70 into one that completes.
    Measured: a 72-frame run had 3 failures, all transient, and all three
    succeeded on an immediate re-run.
    """
    result = None
    for attempt in range(1, attempts + 1):
        result = _capture_once(scene, camera, pass_name, out_path, timeout)
        result["attempt"] = attempt
        result["attempts"] = attempts
        if "error" not in result:
            return result
        if attempt < attempts:
            print(f"         attempt {attempt}/{attempts} failed: {result['error']}"
                  f" -- retrying in 5s", flush=True)
            time.sleep(5)
    return result


def _capture_once(scene: str, camera: tuple, pass_name: str, out_path: Path,
                  timeout: int) -> dict:
    """One capture attempt. Returns a dict of what happened."""
    name = camera[0]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    if out_path.exists():
        out_path.unlink()

    env = build_env(scene, camera, pass_name, out_path)
    log_path = out_path.with_suffix(".log")

    started = time.time()
    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        proc = subprocess.Popen(
            [str(EDITOR_EXE)],
            cwd=str(EDITOR_DIR),
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        try:
            proc.wait(timeout=timeout)
            timed_out = False
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            timed_out = True
    elapsed = time.time() - started

    result = dict(
        camera=name,
        pass_name=pass_name,
        path=out_path,
        log=log_path,
        elapsed=elapsed,
        timed_out=timed_out,
        exit_code=proc.returncode,
    )

    if timed_out:
        result["error"] = f"timed out after {timeout}s"
        return result
    if not out_path.exists():
        result["error"] = "no PNG written"
        return result
    if out_path.stat().st_size == 0:
        result["error"] = "zero-byte PNG"
        return result

    try:
        png = read_png(out_path)
        result["png"] = png
        result["stats"] = measure(png)
        result["sha256"] = sha256_file(out_path)
    except Exception as exc:  # malformed PNG, truncated write, bad filter
        result["error"] = f"unreadable PNG: {exc}"
    return result


# --------------------------------------------------------------------------
# Export
# --------------------------------------------------------------------------


def discover_scenes() -> list:
    scene_dir = EDITOR_DIR / "assets" / "scenes"
    if not scene_dir.is_dir():
        return []
    found = sorted(p.name for p in scene_dir.glob("*.wastescene"))
    return [f"scenes/{n}" for n in found if n not in SCENE_EXCLUDE]


# --------------------------------------------------------------------------
# Auto-framing
# --------------------------------------------------------------------------

# Components that draw geometry. Lights, cameras, volumes and scripts have a
# transform but nothing to reconstruct, so they are excluded: framing a scene
# around a sun direction or a 10 km fog volume produces a camera a kilometre
# from anything worth looking at.
_RENDERER_SUFFIXES = ("Renderer", "Water")


def scene_content(path: Path):
    """(centre, distances) for the renderable entities in a scene.

    Returns (None, 0) when the scene has nothing renderable.

    The .wastescene YAML is machine-generated and regular -- entities at indent
    2, components at 4, fields at 6 -- so this is a line scan rather than a YAML
    parse. PyYAML is not a dependency and the format has no anchors, tags or
    flow collections beyond the three-element vectors matched here.

    The centre is the per-axis MEDIAN of entity translations, not the mean and
    not the bounding-box centre. Both of those are dragged away from the props
    by a single large ground plane: Example3D's ground is 10000 units across,
    which put the bbox centre at y=27 and made the "centre" of the scene a point
    in mid-air above it. The median of 8 entities is not moved by one of them
    being huge.
    """
    text = path.read_text(encoding="utf-8", errors="replace").splitlines()
    translations = [
        e["translation"] for e in _iter_entities(text) if e["renders"] and e["translation"]
    ]
    if not translations:
        return None, 0

    centre = (
        statistics.median(t[0] for t in translations),
        statistics.median(t[1] for t in translations),
        statistics.median(t[2] for t in translations),
    )
    distances = sorted(
        max(abs(t[0] - centre[0]), abs(t[1] - centre[1]), abs(t[2] - centre[2]))
        for t in translations
    )
    return centre, distances


def _iter_entities(lines):
    """Yield {'translation': tuple|None, 'renders': bool} per entity block."""
    current = None
    for line in lines:
        if line.startswith("  - Entity:"):
            if current is not None:
                yield current
            current = {"translation": None, "renders": False}
            continue
        if current is None:
            continue
        stripped = line.strip()
        if stripped.startswith("Translation:"):
            match = re.match(
                r"Translation:\s*\[\s*([-\d.eE+]+)\s*,\s*([-\d.eE+]+)\s*,"
                r"\s*([-\d.eE+]+)\s*\]",
                stripped,
            )
            if match:
                current["translation"] = tuple(float(g) for g in match.groups())
        elif stripped.endswith("Component:"):
            if stripped[: -len("Component:")].strip().endswith(_RENDERER_SUFFIXES):
                current["renders"] = True
    if current is not None:
        yield current


def derive_cameras(scene: str):
    """Cameras derived from the scene's own content. [] if it cannot be framed.

    The distance is a multiple of the median entity distance from the content
    centre, clamped. Three yaws at one distance, because which side of a cluster
    has content is not something a bounding box can tell you.
    """
    path = EDITOR_DIR / "assets" / scene
    if not path.is_file():
        return []
    centre, distances = scene_content(path)
    if centre is None or not distances:
        return []

    median = distances[len(distances) // 2]
    p90 = distances[min(len(distances) - 1, int(len(distances) * 0.90))]
    # The distance itself comes from the MEDIAN spread only. Adding a p90 term to
    # the distance looked reasonable and is not: Example3D has a few entities
    # ~56 m out, which pushed it from 13 m (p99 9.6-19.4) to 28 m for no gain.
    # The p90 enters only as the ceiling, which is what actually fixes the
    # spread-out scenes.
    distance = median * AUTO_DISTANCE_FACTOR
    distance = max(AUTO_DISTANCE_MIN, min(max(AUTO_DISTANCE_MAX_FLOOR, p90), distance))

    return [
        (f"auto_y{int(yaw)}", centre, round(distance, 3), AUTO_PITCH, yaw)
        for yaw in AUTO_YAWS
    ]


def frame_id(scene: str, camera_name: str, pass_name: str) -> str:
    return f"{Path(scene).stem}__{camera_name}__{pass_name}"


def size_mismatch(size: tuple, expected: tuple | None) -> str | None:
    """Return an error string if `size` is not the run's settled resolution.

    Kept as a function so the guard can be tested without running the editor.
    `expected=None` means "nothing captured yet", which is never a mismatch --
    the first frame DEFINES the run's size.
    """
    if expected is None or size == expected:
        return None
    return (f"capture is {size[0]}x{size[1]}, this run's other frames are "
            f"{expected[0]}x{expected[1]} -- the render target had not settled; "
            f"use a later --frame")


def export(args) -> int:
    if not EDITOR_EXE.is_file():
        print(f"error: editor binary not found at {EDITOR_EXE}", file=sys.stderr)
        print("       build it first:  msbuild DemonCore-Editor/DemonCore-Editor.vcxproj "
              "-p:Configuration=Release -p:Platform=x64", file=sys.stderr)
        return 2

    # --scenes is repeatable, so argparse hands back a list; a bare string here
    # would be iterated character by character.
    scenes = list(args.scenes) if args.scenes else discover_scenes()
    if not scenes:
        print("error: no scenes found", file=sys.stderr)
        return 2

    # Framing. `auto` derives a camera per scene from that scene's own content;
    # `preset` uses the hand-tuned CAMERAS list, which is only valid for scenes
    # of a similar extent to the one it was tuned on.
    per_scene_cameras = {}
    if args.frame == "auto":
        for scene in scenes:
            derived = derive_cameras(scene)
            if not derived:
                print(f"error: cannot frame '{scene}' -- no renderable entities",
                      file=sys.stderr)
                return 2
            per_scene_cameras[scene] = derived
            print(f"frame {Path(scene).stem}: focal "
                  f"({derived[0][1][0]:.1f},{derived[0][1][1]:.1f},{derived[0][1][2]:.1f}) "
                  f"distance {derived[0][2]:.1f} pitch {derived[0][3]:.0f} "
                  f"yaws {[int(c[4]) for c in derived]}")
        cameras = None
    else:
        cameras = (
            [(n, f, d, p, y) for (n, f, d, p, y) in CAMERAS if n in args.cameras]
            if args.cameras
            else list(CAMERAS)
        )
    passes = list(args.passes) if args.passes else ["gt", "input"]

    out_root = Path(args.out).resolve()
    (out_root / "gt").mkdir(parents=True, exist_ok=True)
    (out_root / "input").mkdir(parents=True, exist_ok=True)
    (out_root / "noisy").mkdir(parents=True, exist_ok=True)

    # Manifest repair is a read-only pass over the PNGs; it must not also run a
    # capture plan, and it needs the same scene list the capture would use.
    if args.rebuild_manifest:
        return rebuild_manifest(scenes, out_root, passes)

    def cameras_for(scene):
        return per_scene_cameras[scene] if args.frame == "auto" else cameras

    plan = [
        (scene, cam, p)
        for scene in scenes
        for cam in cameras_for(scene)
        for p in passes
    ]
    camera_count = len(plan) // max(1, len(scenes) * len(passes))
    print(f"export: {len(scenes)} scene(s) x {camera_count} camera(s) x {len(passes)} pass(es)"
          f" = {len(plan)} capture(s)  [frame={args.frame}]")
    for scene, cam, p in plan:
        print(f"  {frame_id(scene, cam[0], p):50s} scale={PASSES[p]['scale']}")

    if args.dry_run:
        return 0

    frames = []
    failures = []
    # Set by the first successful capture; every later one must match it. See
    # the check in the loop for why a size mismatch is fatal rather than a
    # weaker sample.
    expected_size = None
    for index, (scene, cam, pass_name) in enumerate(plan, start=1):
        fid = frame_id(scene, cam[0], pass_name)
        print(f"[{index}/{len(plan)}] {fid} ... ", end="", flush=True)
        out_path = out_root / pass_name / f"{fid}.png"
        res = capture(scene, cam, pass_name, out_path, args.timeout, args.attempts)

        if "error" in res:
            print(f"FAILED: {res['error']}")
            failures.append((fid, res["error"]))
            continue

        stats = res["stats"]
        blank = stats["contrast_p99"] < MIN_LOCAL_CONTRAST_P99
        print(f"ok {stats['mean_luma']:.1f} luma, contrast p99 {stats['contrast_p99']:.1f}"
              f"  ({res['elapsed']:.0f}s)")

        # Every capture in a run must be the same size. The pairs are compared
        # pixel-wise, so a frame that came out at a different resolution is not
        # a weaker sample -- it is a DIFFERENT IMAGE, and the manifest would
        # record it as if it were a valid one. `--validate` cannot catch this
        # because it only checks each PNG against the size the manifest already
        # claims. Measured: the same camera at frame 2 came out 1280x720 and at
        # frame 3 came out 3840x2092, against 2332x1451 once settled. Refuse the
        # run rather than emit a silently misaligned dataset.
        size = (res["png"]["width"], res["png"]["height"])
        problem = size_mismatch(size, expected_size)
        if problem is None:
            expected_size = size
        else:
            failures.append((fid, problem))
            print(f"FAILED: {problem}")
            continue

        frames.append(
            dict(
                id=fid,
                scene=scene,
                camera=dict(
                    name=cam[0],
                    focal=list(cam[1]),
                    distance=cam[2],
                    pitch=cam[3],
                    yaw=cam[4],
                ),
                pass_name=pass_name,
                render_scale=PASSES[pass_name]["scale"],
                quality_preset=3,
                neural=PASSES[pass_name]["neural"],
                accumulate=PASSES[pass_name]["accumulate"],
                frame_index=PASSES[pass_name]["frame"],
                width=res["png"]["width"],
                height=res["png"]["height"],
                attempts=res.get("attempts", 1),
                path=f"{pass_name}/{fid}.png",
                sha256=res["sha256"],
                mean_luma=round(stats["mean_luma"], 4),
                contrast_p99=round(stats["contrast_p99"], 4),
                blank=blank,
            )
        )

    manifest_path = out_root / "manifest.json"

    # MERGE with any existing manifest rather than replacing it.
    #
    # The manifest is the dataset's reproducibility anchor -- every PNG carries
    # the SHA-256 of the binary that produced it, and load_pairs() refuses to
    # read a dataset with an incomplete manifest. Writing a fresh manifest per
    # run made incremental re-runs self-defeating: re-running one scene to fill
    # a timeout gap would discard the other scenes' entries and leave the
    # dataset looking 9 frames short. Frames are keyed by id, so a re-run
    # overwrites its own entries in place and leaves the rest alone.
    merged: dict = {}
    if manifest_path.is_file():
        try:
            previous = json.loads(manifest_path.read_text(encoding="utf-8"))
            for entry in previous.get("frames", []):
                merged[entry["id"]] = entry
        except (ValueError, KeyError, OSError):
            # A corrupt or unreadable manifest is not a reason to lose a run;
            # start clean and say so.
            print(f"warning: existing manifest unreadable, starting fresh")
            merged = {}
    for entry in frames:
        merged[entry["id"]] = entry

    # Drop entries whose PNG is no longer on disk. The frames list only covers
    # this run, so without this a stale entry from an earlier run would survive
    # a delete and load_pairs() would then fail on a missing file.
    kept = []
    for entry in merged.values():
        if (out_root / entry["path"]).is_file():
            kept.append(entry)
        else:
            print(f"  dropped stale manifest entry: {entry['id']}")
    kept.sort(key=lambda e: e["id"])

    # Failures are recomputed from the merged set, not carried over: a frame
    # that failed in an earlier run but succeeded here is not a failure any
    # more, and one that failed here but is absent from the merged set should
    # still be reported.
    missing_ids = {f for f, _ in failures}
    still_missing = [
        dict(id=e["id"], error="no PNG written (earlier run)")
        for e in merged.values()
        if e["id"] in missing_ids
    ]

    manifest = dict(
        version=MANIFEST_VERSION,
        generated=time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        engine=dict(
            exe=str(EDITOR_EXE),
            sha256=sha256_file(EDITOR_EXE),
        ),
        config=dict(
            converged_frame=CONVERGED_FRAME,
            noisy_frame=NOISY_FRAME,
            min_contrast_p99=MIN_LOCAL_CONTRAST_P99,
        ),
        frames=kept,
        failures=[dict(id=f, error=e) for f, e in failures] + still_missing,
    )
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"\nwrote {manifest_path}  ({len(kept)} frames, "
          f"{len(manifest['failures'])} failures)")

    if failures:
        print("\ncapture failures:", file=sys.stderr)
        for fid, err in failures:
            print(f"  {fid}: {err}", file=sys.stderr)
        return 1

    blanks = [f for f in frames if f["blank"]]
    if blanks:
        print(
            "\nThese frames rendered with no edges (contrast below floor). That means\n"
            "there is nothing in them for a network to reconstruct -- sky, or a flat\n"
            "surface. The frames are still written and recorded as blank, but a\n"
            "dataset containing them is not usable.",
            file=sys.stderr,
        )
        for f in blanks:
            print(f"  {f['id']}: contrast p99 {f['contrast_p99']}, mean luma {f['mean_luma']}",
                  file=sys.stderr)
        print("\nFix the camera for those scenes, or pass --allow-blank to accept them.",
              file=sys.stderr)
        if not args.allow_blank:
            return 1
    return 0


def rebuild_manifest(scenes: list, out_root: Path, passes: list) -> int:
    """Reconstruct manifest.json from the PNGs already on disk.

    Why this exists: the manifest is what load_pairs() trusts, and it used to be
    written once per run. An incremental re-run -- the only sane way to fill a
    handful of timeout gaps -- therefore *shrank* the manifest to that run's
    frames and made the dataset look incomplete even though every PNG was
    present. Rebuilding from disk is also the honest recovery when a manifest is
    lost or hand-edited.

    Everything here is derived, nothing is invented: the camera comes from the
    scene file (same derivation the capture used), the size and the SHA-256 come
    from the PNG, and mean_luma / contrast_p99 are recomputed from its pixels.
    """
    print(f"rebuilding manifest from {out_root} ...")
    derived = {}
    for scene in scenes:
        cams = derive_cameras(scene)
        if not cams:
            print(f"error: cannot frame '{scene}' -- no renderable entities",
                  file=sys.stderr)
            return 2
        derived[scene] = cams

    frames = []
    missing = []
    for scene, cams in derived.items():
        stem = Path(scene).stem
        for (name, focal, dist, pitch, yaw) in cams:
            for pass_name in passes:
                fid = f"{stem}__{name}__{pass_name}"
                rel = f"{pass_name}/{fid}.png"
                path = out_root / rel
                if not path.is_file():
                    missing.append(fid)
                    continue
                png = read_png(path)
                stats = measure(png)
                frames.append(dict(
                    id=fid,
                    scene=scene,
                    camera=dict(name=name, focal=list(focal), distance=dist,
                                pitch=pitch, yaw=yaw),
                    pass_name=pass_name,
                    render_scale=PASSES[pass_name]["scale"],
                    quality_preset=3,
                    neural=1,
                    accumulate=PASSES[pass_name]["accumulate"],
                    frame_index=PASSES[pass_name]["frame"],
                    width=png["width"],
                    height=png["height"],
                    attempts=0,
                    path=rel,
                    sha256=sha256_file(path),
                    mean_luma=round(stats["mean_luma"], 4),
                    contrast_p99=round(stats["contrast_p99"], 4),
                    blank=stats["contrast_p99"] < MIN_LOCAL_CONTRAST_P99,
                ))
    frames.sort(key=lambda e: e["id"])

    manifest = dict(
        version=MANIFEST_VERSION,
        generated=time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        engine=dict(exe=str(EDITOR_EXE), sha256=sha256_file(EDITOR_EXE)),
        config=dict(
            converged_frame=CONVERGED_FRAME,
            noisy_frame=NOISY_FRAME,
            min_contrast_p99=MIN_LOCAL_CONTRAST_P99,
        ),
        frames=frames,
        failures=[dict(id=f, error="no PNG on disk") for f in sorted(missing)],
    )
    (out_root / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out_root / 'manifest.json'}  ({len(frames)} frames, "
          f"{len(missing)} missing)")
    for fid in sorted(missing):
        print(f"  missing: {fid}")
    return 1 if missing else 0


# --------------------------------------------------------------------------
# Validate
# --------------------------------------------------------------------------


def validate(args) -> int:
    out_root = Path(args.out).resolve()
    manifest_path = out_root / "manifest.json"
    if not manifest_path.is_file():
        print(f"error: no manifest at {manifest_path}", file=sys.stderr)
        return 2

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    frames = manifest.get("frames", [])
    errors = []
    warnings = []

    if not frames:
        errors.append("manifest has no frames")

    by_id = {}
    for frame in frames:
        fid = frame.get("id", "<no id>")
        by_id[fid] = frame

        rel = frame.get("path")
        if not rel:
            errors.append(f"{fid}: no path")
            continue
        path = out_root / rel
        if not path.is_file():
            errors.append(f"{fid}: missing file {rel}")
            continue
        size = path.stat().st_size
        if size == 0:
            errors.append(f"{fid}: zero-byte file {rel}")
            continue

        digest = frame.get("sha256")
        if not digest:
            errors.append(f"{fid}: no sha256")
        elif sha256_file(path) != digest:
            errors.append(f"{fid}: sha256 mismatch (file changed since export)")

        try:
            png = read_png(path)
        except Exception as exc:
            errors.append(f"{fid}: unreadable PNG: {exc}")
            continue

        if png["width"] != frame.get("width") or png["height"] != frame.get("height"):
            errors.append(
                f"{fid}: dimensions {png['width']}x{png['height']} do not match manifest "
                f"{frame.get('width')}x{frame.get('height')}"
            )

        stats = measure(png)
        if stats["mean_luma"] < MIN_MEAN_LUMA:
            errors.append(f"{fid}: mean luma {stats['mean_luma']:.3f} below floor"
                         f" {MIN_MEAN_LUMA} -- image is black")
        if stats["contrast_p99"] < MIN_LOCAL_CONTRAST_P99:
            # An error, not a warning: a frame with no edges has nothing for the
            # network to reconstruct, so the pair is unusable however valid the
            # render is. Silently accepting it is the failure mode this gate
            # exists to prevent.
            errors.append(
                f"{fid}: local contrast p99 {stats['contrast_p99']:.1f} below floor"
                f" {MIN_LOCAL_CONTRAST_P99} -- no edges, so nothing to reconstruct"
                f" (sky or a flat surface; move the camera)"
            )

    # Pairing: every gt must have exactly one input with the same scene+camera.
    def key(frame, drop):
        cam = frame.get("camera", {})
        return (
            frame.get("scene"),
            cam.get("name"),
            tuple(cam.get("focal", [])),
            cam.get("distance"),
            cam.get("pitch"),
            cam.get("yaw"),
        )

    gt_keys = {key(f, "pass_name"): f for f in frames if f.get("pass_name") == "gt"}
    input_keys = [key(f, "pass_name") for f in frames if f.get("pass_name") == "input"]

    for k, frame in gt_keys.items():
        count = input_keys.count(k)
        if count == 0:
            errors.append(f"{frame['id']}: no matching input/ frame")
        elif count > 1:
            errors.append(f"{frame['id']}: {count} matching input/ frames (expected 1)")
    for k in input_keys:
        if k not in gt_keys:
            errors.append(f"input frame {k} has no matching gt/ frame")

    # Pair geometry must agree: same camera, and gt is the full-res member.
    for frame in frames:
        if frame.get("pass_name") != "gt":
            continue
        match = next((f for f in frames
                      if f.get("pass_name") == "input" and key(f, "") == key(frame, "")), None)
        if match is None:
            continue
        if match["render_scale"] >= frame["render_scale"]:
            warnings.append(
                f"{frame['id']}: input render_scale {match['render_scale']} is not below "
                f"gt render_scale {frame['render_scale']}"
            )
        if (match["width"], match["height"]) != (frame["width"], frame["height"]):
            errors.append(
                f"{frame['id']}: input is {match['width']}x{match['height']} but gt is "
                f"{frame['width']}x{frame['height']}"
            )

    print(f"validated {len(frames)} frame(s) in {manifest_path}")
    print(f"  gt:     {len(gt_keys)}")
    print(f"  input:  {len(input_keys)}")
    for w in warnings:
        print(f"  WARN  {w}")
    for e in errors:
        print(f"  ERROR {e}")
    if errors:
        print(f"\n{len(errors)} error(s)")
        return 1
    if warnings:
        print(f"\nok, with {len(warnings)} warning(s)")
    else:
        print("\nok")
    return 0


# --------------------------------------------------------------------------


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Export matched neural-rendering frame pairs from the Wasteland engine.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--out", default="dataset",
                        help="output root (default: dataset/)")
    parser.add_argument("--scenes", action="append",
                        help="project-relative scene path; repeatable. Default: all scenes"
                             " in assets/scenes except 2D ones")
    parser.add_argument("--cameras", action="append",
                        help="camera name from CAMERAS; repeatable. Default: all. "
                             "Ignored under --frame auto")
    parser.add_argument("--frame", choices=("auto", "preset"), default="auto",
                        help="auto (default) derives a camera per scene from that"
                             " scene's own content; preset uses the hand-tuned CAMERAS"
                             " list, which only fits scenes of a similar extent")
    parser.add_argument("--passes", action="append", choices=sorted(PASSES),
                        help="pass to export; repeatable. Default: gt and input")
    parser.add_argument("--timeout", type=int, default=900,
                        help="seconds to wait for one capture (default 900)")
    parser.add_argument("--attempts", type=int, default=DEFAULT_ATTEMPTS,
                        help="capture attempts per frame before giving up "
                             "(default %(default)s); transient GPU resets are common on Ultra")
    parser.add_argument("--allow-blank", action="store_true",
                        help="exit 0 even if some frames rendered with no edges")
    parser.add_argument("--validate", action="store_true",
                        help="validate an existing dataset instead of exporting")
    parser.add_argument("--rebuild-manifest", action="store_true",
                        help="reconstruct manifest.json from the PNGs already on "
                             "disk, without running the editor")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the capture plan without running the editor")
    args = parser.parse_args(argv)

    if args.validate:
        return validate(args)
    return export(args)


if __name__ == "__main__":
    raise SystemExit(main())
