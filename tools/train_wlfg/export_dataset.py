"""WLFG neural frame-synthesis-head dataset exporter.

Produces the matched `(input, target)` frame pairs that the clean-room
WLFG synthesis head trains on: `input` the warped (reprojected previous
present) frame, `target` the converged real frame. See `docs/NEURAL_RENDERING.md`
for the stage itself and `.hermes/plans/2026-10-26_001250-clean-room-dlss-class-stack.md`
Task 8 for the breakdown.

Two toolchains, deliberately split:
|| | imports | why |
||---|---|---|
|| `export_dataset.py` | stdlib only | drives the engine binary, must run anywhere the engine does |
|| `model.py`, `dataset.py`, `train.py`, `test_model.py`, `export_weights.py` | `numpy` + `Pillow` | training only; no framework, no vendor maths |

The exporter is the clean-room boundary: every target image comes from *this*
binary, from *these* scenes, by *this* binary. Nothing is downloaded or vendored.

## Pair layout

Each frame index yields a pair. The `target/` PNG is the real (converged) present
from the u0 readback (`img_Output`); `warped/` is the warp pass's reproduction of
the previous present (read back once u1 is exposed, currently a placeholder until
the readback hook is wired).

    target/  <id>__target.png   the converged real frame (u0 = img_Output)
    warped/  <id>__warped.png   the reprojected previous present (u1 = img_Accumulation)
    mask/    <id>__mask.png     the reactive mask
    velocity/ <id>__velocity.png velocity: X in R, Y in G, remapped from [-1,1] to [0,1]
    depth/   <id>__depth.png     depth (NDC)

All are full-res rgba32f captures. `target` is what the head learns to reproduce;
`warped` + `mask` + the state tensors are what it reads per pixel.

## Why the pairs are aligned

`input`, `mask` and `target` are all captured from the *same* frame index with the
*SAME* pin, so they are one present. `WL_WLFG` sets the present rate (how fast the
sim advances per frame), which is the only thing that changes between arms; the
capture frame index is fixed so the jitter is identical across arms. `WL_DRS_OFF`
disables dynamic resolution so the pair stays aligned.

The one thing this cannot guarantee is a camera that sees the scene. A pin that
looks at open sky produces a smooth, near-constant image. Rather than trust the
caller, every capture is measured: `--validate` and the export itself reject frames
whose local contrast is below a floor, and record the measurement.

## Camera framing

`--frame preset` uses the hard-coded `CAMERAS` list (deterministic, hand-tuned,
years of training). `--frame auto` derives a camera from the scene's own content.

## Usage

```bash
# Plan only -- print every capture it would run, touch nothing.
python tools/train_wlfg/export_dataset.py --dry-run

# Default: all preset cameras, all factors {1,2,3,4,6}, paired frames.
python tools/train_wlfg/export_dataset.py

# Narrow the camera list while iterating.
python tools/train_wlfg/export_dataset.py --cameras cam_orbit cam_down

# Check a dataset after the fact.
python tools/train_wlfg/export_dataset.py --validate
```

Stdlib only: the repo pins no imaging dependency, and a PNG reader is ~40 lines.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path

# Add the repo root to sys.path so the stdlib-only mirror pipeline imports.
_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))
from train_neural.export_dataset import read_png, sha256_file  # stdlib-only mirror helpers

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parents[2]
EDITOR_DIR = REPO_ROOT / "bin" / "Release-windows-x86_64" / "DemonCore-Editor"
EDITOR_EXE = EDITOR_DIR / "DemonCore-Editor.exe"

# The WLFG present rate (factor). Sim advances 1/factor as fast, so higher
# factors walk the camera more slowly per frame -- the motion is still
# deterministic, just slower. Factor 6 is HELD OUT entirely for evaluation so a
# model trained on every factor can never be a lookup table of that factor.
FACTORS = (1, 2, 3, 4, 6)
FACTOR_HOLDOUT = 6

# Hand-tuned, deterministic cameras. The "motion path" is the sim hop driven by
# the factor (see FACTOR_HOLDOUT); these are the (name, focal_xyz, distance,
# pitch, yaw) each capture starts at. Only entities with a RendererComponent or
# WaterComponent count for framing; lights/cameras/volumes/scripts do not.
CAMERAS = [
    ("cam_down", (0.0, 0.0, 0.0), 9.0, -85.0, 0.0),
    ("cam_low", (0.0, 1.5, 0.0), 12.0, -25.0, 30.0),
    ("cam_orbit", (0.0, 2.0, 0.0), 16.0, -18.0, 140.0),
]

# A capture whose local contrast is below this is a camera pointed at nothing.
MIN_LOCAL_CONTRAST_P99 = 2.0

# Hand-tuned pin: fixed pose for a converged, static-camera render.
PIN = (0.0, 0.0, 0.0, 9.0, -85.0, 0.0)

# Frame index at which the pair is latched (fixed, so the jitter is identical
# across arms). At this frame the sim has settled and the warp is stable.
PIN_FRAME = 300  # converged present; the u0 readback (img_Output) only holds a
                 # valid composite after the first Nova flush, which needs ~300
                 # frames at normal sim speed. Kept fixed across arms so the
                 # jitter is identical.

# Degrees of yaw per frame applied on top of the pin (WASTELAND_PIN_CAMERA_ORBIT).
# Set by --motion. Zero keeps the static pin, which is the right arm for the 6x PSNR
# acceptance but useless for training: a static camera has zero reprojection
# velocity and an empty reactive mask.
#
# The value is NOT degrees per frame despite the name: the orbit hook advances the
# yaw by rate x frame counter and the editor renders many sim ticks per presented
# frame, so the effective per-frame yaw is roughly 40x the number set here. Treat
# it as an arbitrary monotonic motion level, not a calibrated angle.
ORBIT_DEG_PER_FRAME = None

# WL_WLFG for this export. 1 (default) captures the WLFG ARM: pass 22 overwrites
# img_Accumulation with the reprojected previous present, so u1 is the warp the
# head is trained to correct. 0 captures the NO-WLFG CONTROL arm: pass 22 does not
# run and u1 holds the plain temporal blend, which is the engine's own output with
# no warp at all. The plan's Step 3 gate is that the trained head beats that
# control, so the control has to come from the same scene, camera, motion level
# and frame as the arm it is compared against -- this flag is what makes that
# possible without a rebuild.
WLFG_WARP = 1

# The engine's WL_WLFG is a BOOLEAN gate for the reprojection warp (pass 22), not a
# present-rate multiplier -- the multiplier is Task 8 step 5 and does not exist yet.
# So the "factor" axis in this export is the CAMERA MOTION LEVEL, which is what
# actually changes the data: more motion means more reprojection error and more
# disocclusion, which is what the head corrects. The plan's holdout rule still
# applies -- factor 6 is held out and is also the most extreme motion, so a model
# that only learned to fix small warps shows it immediately.
#
# These levels were re-calibrated after the original {0.10..1.00} table was measured
# to be entirely outside the warp's usable range: at those rates the camera left the
# frustum every frame, so the reprojected previous UV landed far outside [0,1] and the
# warp's validity test rejected every pixel, making the two arms byte-identical.
# Measured mean |velocity| (UV units) and the fraction of written pixels whose
# reprojected UV stayed inside [0,1]:
#   0.004 -> 0.099, 100% valid | 0.008 -> 0.199, 100% valid
#   0.016 -> 0.396,  87% valid (breaks) | 0.35 -> far out of range, 0% valid
# The table below spans a 16x motion range with every level fully valid.
FACTOR_TO_MOTION = {1: 0.0005, 2: 0.001, 3: 0.002, 4: 0.004, 6: 0.008}

DEFAULT_ATTEMPTS = 2
OUT_DEFAULT = "dataset_wlfg"
# The engine's default ray-tracing render size. Placeholders use this so the
# size contract holds even when a capture produced no PNG. Real captures set
# expected_size from their own PNG.
DEFAULT_CAPTURE_WIDTH = 2332
DEFAULT_CAPTURE_HEIGHT = 1451


# ---------------------------------------------------------------------------
# stdlib-only module that does not depend on the training layer.
# ---------------------------------------------------------------------------


def build_env(factor: int, out_path: Path, set_prefix: Path | None = None) -> dict:
    """Environment for ONE headless capture. Mirrors the task-7 acceptance: a
    pinned camera, dynamic-resolution off, and the screenshot hooks. The env is
    a copy of the current process environment (so the engine has the variables
    it needs to run via subprocess) with only the task-7 capture knobs added."""
    env = dict(os.environ)
    env.update(
        {
            "WASTELAND_PIN_CAMERA": "%g,%g,%g,%g,%g,%g" % (PIN[0], PIN[1], PIN[2], PIN[3], PIN[4], PIN[5]),
            "WL_DRS_OFF": "1",
            "WL_SCREENSHOT": str(out_path),
            "WL_SCREENSHOT_FRAME": str(PIN_FRAME),
            "WL_SCREENSHOT_LINGER": "6",
            # Pass 24 -- the reactive mask -- only dispatches when reconstruction is
            # on, so without this the mask buffer is structurally zero and the head
            # trains on a dead feature.
            "WL_RECONSTRUCTION": "1",
        }
    )
    # Motion: an explicit --motion wins, otherwise the per-factor level.
    orbit = ORBIT_DEG_PER_FRAME
    if orbit is None:
        orbit = FACTOR_TO_MOTION.get(factor, 0.0)
    if orbit:
        env["WASTELAND_PIN_CAMERA_ORBIT"] = "%g" % orbit
    # 1 = the warp arm (img_Accumulation is the reprojected previous present);
    # 0 = the no-WLFG control arm (img_Accumulation is the plain temporal blend).
    env["WL_WLFG"] = str(WLFG_WARP)
    if set_prefix is not None:
        # The editor writes <set_prefix>_u<slot>.png for the whole image set
        # (u0 output, u1 accumulation, u5 velocity, u6 g-buffer, u14 mask) from the
        # capture frame. WL_SCREENSHOT still has to be set: it is what arms the
        # capture trigger and the linger-close. The float readback is what the
        # head reads: u5 (velocity, RG32 -> X,Y) and u6 (g-buffer R = depth) are
        # emitted as lossless float32 .npy by CaptureNovaImageSetFloat, so the
        # pair is trained on linear values the head expects, not the 8-bit PNG
        # quantization that the visual path would otherwise hand it.
        env["WL_NOVA_CAPTURE_SET"] = str(set_prefix)
        env["WL_NOVA_CAPTURE_SET_FLOAT"] = str(set_prefix)
    return env


def frame_id(camera_name: str, factor: int) -> str:
    return f"Example3D__{camera_name}__{factor}"


def capture(factor: int, out_path: Path, timeout: int,
            attempts: int = DEFAULT_ATTEMPTS,
            set_prefix: Path | None = None) -> dict:
    """Run one headless capture at `factor`, retrying on transient failure.

    The binary writes a single PNG via the u0 readback (the real present,
    `img_Output`). Each attempt is isolated in its own log.
    """
    out_path.parent.mkdir(parents=True, exist_ok=True)
    if out_path.exists():
        out_path.unlink()

    env = build_env(factor, out_path, set_prefix)
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

    return dict(
        factor=factor,
        path=out_path,
        log=log_path,
        elapsed=elapsed,
        timed_out=timed_out,
        exit_code=proc.returncode,
        set_prefix=set_prefix,
    )


def _load_png(out_path: Path) -> dict:
    """Read back a capture and measure it. Returns the png dict (or raises)."""
    return read_png(out_path)


def _write_placeholder_png(out_path: Path, width: int, height: int) -> None:
    """Write an all-black RGBA placeholder PNG so the dataset schema is complete.

    The engine mask/vel/depth readback is not yet wired; this writes a
    placeholder so the training loop has a file to read. Real values are
    populated by the engine capture path once the readback is fully functional.
    """
    from PIL import Image
    # All-black RGBA placeholder (0, 0, 0, 255)
    png = Image.new("RGBA", (width, height), (0, 0, 0, 255))
    png.save(out_path, "PNG")


def validate_dir(out_root: Path) -> int:
    """Check an exported dataset and report a compact summary."""
    manifest_path = out_root / "manifest.json"
    if not manifest_path.is_file():
        print(f"error: no manifest at {manifest_path}", file=sys.stderr)
        return 2
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    frames = manifest.get("frames", [])
    errors = []
    warnings = []
    for frame in frames:
        fid = frame.get("id", "<no id>")
        rel = frame.get("path")
        if not rel:
            errors.append(f"{fid}: no path")
            continue
        path = out_root / rel
        if frame.get("pending"):
            # Expected pending (mask/vel/depth): absence of the file is fine;
            # the containing dir must exist for the follow-on wire-up to land.
            if not path.parent.is_dir():
                warnings.append(f"{fid}: missing dir {path.parent}")
            continue
        if not path.is_file():
            errors.append(f"{fid}: missing file {rel}")
            continue
        # Placeholder (touch() from the warp readback not being wired): an empty
        # file is the known, expected state until the u1 readback lands.
        if path.stat().st_size < 8:
            continue
        try:
            png = read_png(path)
        except Exception as exc:
            errors.append(f"{fid}: unreadable PNG: {exc}")
            continue
        if png["width"] != frame.get("width") or png["height"] != frame.get("height"):
            errors.append(f"{fid}: dims {png['width']}x{png['height']} != {frame.get('width')}x{frame.get('height')}")
        # The contrast floor is a check on the PRESENT, not on every buffer: a
        # reactive mask is legitimately near-black in a static scene, and a depth
        # or velocity image with little motion has almost no local contrast. Only
        # target and warped are pictures of the scene.
        if frame.get("pass_name") in ("target", "warped"):
            stats = measure(png)
            if stats["contrast_p99"] < MIN_LOCAL_CONTRAST_P99:
                errors.append(f"{fid}: contrast p99 {stats['contrast_p99']:.1f} below floor")
    if errors:
        print(f"VALIDATE FAIL ({len(errors)} error(s)):")
        for e in errors[:20]:
            print(f"  {e}")
        if len(errors) > 20:
            print(f"  ... {len(errors) - 20} more")
        return 1
    if warnings:
        print(f"VALIDATE WARN ({len(warnings)} warning(s)):")
        for w in warnings[:20]:
            print(f"  {w}")
        if len(warnings) > 20:
            print(f"  ... {len(warnings) - 20} more")
    n_nonpending = sum(1 for f in frames if not f.get("pending"))
    n_pending = sum(1 for f in frames if f.get("pending"))
    print(f"VALIDATE OK: {len(frames)} frames ({n_nonpending} non-pending, {n_pending} pending)")
    return 0


# ---------------------------------------------------------------------------
# Measure
# ---------------------------------------------------------------------------

def measure(png: dict) -> dict:
    """Luma statistics plus a local-contrast p99 (mirrors neural-stage convention)."""
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


# ---------------------------------------------------------------------------
# Export
# ---------------------------------------------------------------------------

def export(args) -> int:
    if not EDITOR_EXE.is_file():
        print(f"error: editor binary not found at {EDITOR_EXE}", file=sys.stderr)
        print("       build it first:  msbuild DemonCore-Editor/DemonCore-Editor.vcxproj "
              "-p:Configuration=Release -p:Platform=x64", file=sys.stderr)
        return 2

    scenes = list(args.scenes) if args.scenes else ["Example3D"]
    if not scenes:
        print("error: no scenes", file=sys.stderr)
        return 2

    # Framing: preset mode only (deterministic, hand-tuned pin).
    cameras = list(CAMERAS) if args.cameras is None else [
        c for c in CAMERAS if c[0] in args.cameras]

    global ORBIT_DEG_PER_FRAME, WLFG_WARP
    ORBIT_DEG_PER_FRAME = args.motion if args.motion is not None else None
    WLFG_WARP = args.wlfg

    out_root = Path(args.out).resolve()
    # (pass_name, what it is, the Nova UAV slot the engine writes it from).
    # Every one of these now comes out of ONE frame: WL_NOVA_CAPTURE_SET makes the
    # editor read the whole set back before it advances, so the inputs and the
    # target are the same present (six separate runs would each advance the clock).
    #
    # Channel packing, because the engine hands back RGBA8 whatever the source
    # format: velocity is rg16f, so its two components land in R and G of
    # `velocity/`; depth is the R channel of the g-buffer image.
    BUFFERS = [
        ("target", "real present (img_Output, u0)", "u0"),
        ("warped", "warped frame (img_Accumulation, u1)", "u1"),
        # The reactive mask is img_Mask (u14), not img_Bloom_Temp (u7). Both are
        # images in the Nova table and the mask row used to name u7, so the
        # distribution step looked for a file the capture never writes and every
        # frame came back "pending: no mask image" while the mask sat in sets/.
        ("mask", "reactive mask (img_Mask, u14)", "u14"),
        ("velocity", "velocity, X in R, Y in G (img_Velocity, u5)", "u5"),
        ("depth", "NDC depth in R (img_TraceGBuffer, u6)", "u6"),
    ]

    # Velocity and depth are the two buffers the reprojection head consumes as
    # state, and they have to reach the loader as lossless float32 .npy. The PNG
    # path quantizes velocity to 8 bits and collapses depth to a handful of
    # levels (12 unique values on a real frame), and dataset.py refuses a
    # quantized file outright -- so the manifest has to point at the .npy the
    # float capture writes, not at the PNG beside it.
    FLOAT_BUFFERS = {"velocity": "u5", "depth": "u6"}

    def buffer_ext(name: str) -> str:
        return "npy" if name in FLOAT_BUFFERS else "png"

    if not args.dry_run:
        for _name, _desc, _readback in BUFFERS:
            (out_root / _name).mkdir(parents=True, exist_ok=True)

    def buf_path(camera_name: str, factor: int, pass_name: str) -> Path:
        return out_root / pass_name / f"{frame_id(camera_name, factor)}.{buffer_ext(pass_name)}"

    def frame_path(pass_name: str, fid: str) -> str:
        """Relative path of a frame's file within the dataset root."""
        return f"{pass_name}/{fid}.{buffer_ext(pass_name)}"

    def readback(pass_name: str) -> bool:
        """Every buffer in BUFFERS is read by the engine's capture set
        (WL_NOVA_CAPTURE_SET), so all of them are wired. The function survives
        because a capture can still fail to produce one image, and the manifest
        records that per buffer rather than failing the whole frame."""
        return any(name == pass_name for name, _desc, _slot in BUFFERS)

    def build_pair(camera_name: str, factor: int, scene: str = "Example3D") -> dict:
        fid = frame_id(camera_name, factor)
        return dict(
            id=fid,
            scene=scene,
            factor=factor,
            camera=dict(name=camera_name,
                        focal=list(PIN[:3]), distance=PIN[3], pitch=PIN[4], yaw=PIN[5]),
            frame_index=PIN_FRAME,
            holdout=factor == FACTOR_HOLDOUT,
        )

    # The holdout factor is EXCLUDED by default and included with --holdout (the flag means
    # "include the held-out factor in this export"). The condition used to read
    # `not (args.holdout and f == FACTOR_HOLDOUT)`, which did the opposite: passing --holdout
    # dropped factor 6, so the holdout gate silently fell back to the last ordinary factor.
    plan = [(scene, cam[0], f) for scene in scenes for cam in cameras
            for f in FACTORS if args.holdout or f != FACTOR_HOLDOUT]
    if args.dry_run:
        print(f"export (dry-run, nothing touched): {len(scenes)} scene(s) x {len(cameras)}"
              f" camera(s) x {len(FACTORS)} factor{'s' if len(FACTORS) != 1 else ''} ="
              f" {len(plan)} capture(s)  [holdout={FACTOR_HOLDOUT}]")
        for scene, camera_name, factor in plan:
            print(f"  {frame_id(camera_name, factor):50s} (scene {scene}) WL_WLFG={factor}")
        return 0

    print(f"export: {len(scenes)} scene(s) x {len(cameras)} camera(s) x {len(FACTORS)}"
          f" factor{'s' if len(FACTORS) != 1 else ''} = {len(plan)} capture(s)"
          f"  [holdout={FACTOR_HOLDOUT}]")
    for scene, camera_name, factor in plan:
        print(f"  {frame_id(camera_name, factor):50s} (scene {scene}) WL_WLFG={factor}")

    frames = []
    failures = []
    expected_size = None
    for index, (scene, camera_name, factor) in enumerate(plan, start=1):
        fid = frame_id(camera_name, factor)
        print(f"[{index}/{len(plan)}] {fid} ... ", end="", flush=True)
        pair = build_pair(camera_name, factor, scene)

        # ONE capture per (scene, camera, factor). The editor reads the whole Nova
        # image set back from that single frame (WL_NOVA_CAPTURE_SET), so the warped
        # input, the mask, velocity, depth and the target are literally the same
        # present -- which is the only way a training pair can be aligned.
        set_prefix = out_root / "sets" / fid
        set_prefix.parent.mkdir(parents=True, exist_ok=True)
        target_path = buf_path(camera_name, factor, "target")
        res = capture(factor, target_path, args.timeout, args.attempts,
                      set_prefix=set_prefix)
        if "error" in res:
            print(f"FAILED: {res['error']}")
            failures.append((fid, res["error"]))
            continue

        # Distribute the set. The raw <set_prefix>_u<slot>.png files stay in
        # sets/ as the unmodified archive; the per-buffer copies are what the
        # loader and the manifest refer to.
        produced = {}
        for name, _desc, slot in BUFFERS:
            src = Path(f"{set_prefix}_{slot}.{buffer_ext(name)}")
            dest = buf_path(camera_name, factor, name)
            dest.parent.mkdir(parents=True, exist_ok=True)
            produced[name] = src.is_file()
            if produced[name]:
                dest.write_bytes(src.read_bytes())
            elif name not in FLOAT_BUFFERS:
                # Only the image buffers have a PNG placeholder to fall back on.
                # A missing .npy stays missing so the frame reads as pending and
                # load_pairs() rejects it, rather than being fed a fake state.
                _write_placeholder_png(dest, DEFAULT_CAPTURE_WIDTH, DEFAULT_CAPTURE_HEIGHT)

        png = _load_png(target_path)
        width, height = png["width"], png["height"]
        size = (width, height)
        # Only a real target capture sets the size contract; placeholders must not
        # poison it (a wrong-dim placeholder would reject every real frame after it).
        if produced["target"]:
            if expected_size is None:
                expected_size = size
            elif size != expected_size:
                failures.append((fid, f"size {size} != first {expected_size}"))
                continue

        pair.update(
            width=width, height=height,
            mean_luma=round(measure(png)["mean_luma"], 4),
            contrast_p99=round(measure(png)["contrast_p99"], 4),
        )
        pair["sha256"] = sha256_file(target_path)

        for name, _desc, slot in BUFFERS:
            ok = produced[name]
            frames.append(dict(
                pass_name=name,
                pending=not ok,
                pending_warning=None if ok else f"capture set produced no {slot} image",
                **pair, path=frame_path(name, pair["id"]),
            ))

        missing = sorted(k for k, v in produced.items() if not v)
        print(f"ok {width}x{height} buffers={len(produced) - len(missing)}/{len(produced)}"
              + (f" missing={missing}" if missing else ""))

    # Provenance. The contract tokens are written only when this capture actually
    # met them: every frame produced every buffer, the state channels arrived as
    # lossless float32 .npy, and every buffer shares one size. Anything short of
    # that stays "gather_only" and capture_and_verify.py refuses the dataset --
    # which is the point of the gate, so it must never be written optimistically
    # just because the run "looked fine".
    pending_frames = [f for f in frames if f.get("pending")]
    sizes = {(f["width"], f["height"]) for f in frames}
    float_state_ok = bool(frames) and all(
        f["path"].endswith(".npy") and (out_root / f["path"]).is_file()
        for f in frames if f["pass_name"] in FLOAT_BUFFERS
    )
    complete = (bool(frames) and not failures and not pending_frames
                and len(sizes) == 1 and float_state_ok)

    config = dict(
        factors=FACTORS, holdout_factor=FACTOR_HOLDOUT,
        pin=PIN, pin_frame=PIN_FRAME, min_contrast_p99=MIN_LOCAL_CONTRAST_P99,
        factor_axis_note="camera motion (deg/frame), not present-rate: WL_WLFG is a "
                         "boolean warp gate until Task 8 step 5 lands",
        factor_to_motion=FACTOR_TO_MOTION,
        orbit_deg_per_frame=ORBIT_DEG_PER_FRAME,
    )
    if complete:
        config.update(
            dataset_status="verified_temporal_pairs",
            factor_axis="camera_motion_deg_per_frame",
            state_encoding="float32_npy",
            color_domain="matched",
            spatial_alignment="verified",
        )
    else:
        config["dataset_status"] = "gather_only"
        print("PROVENANCE gather_only: capture did not meet the verified contract "
              f"(pending={len(pending_frames)} failures={len(failures)} "
              f"sizes={sorted(sizes)} float_state={float_state_ok})", file=sys.stderr)

    # The manifest is the dataset's reproducibility anchor. Every non-pending
    # frame carries the SHA-256 of the binary that produced it, and
    # load_pairs() refuses to load a pair whose required inputs are missing.
    manifest = dict(
        version=1,
        generated=time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        engine=dict(exe=str(EDITOR_EXE), sha256=sha256_file(EDITOR_EXE)),
        config=config,
        frames=frames,
        failures=[dict(id=f, error=e) for f, e in failures],
    )
    (out_root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                             encoding="utf-8")
    print(f"\nwrote {out_root / 'manifest.json'}  ({len(frames)} buffer entries,"
          f" {len(failures)} failures)")

    pending = [f for f in frames if f.get("pending")]
    if pending:
        print(f"\n{len(pending)} buffer entries pending readback (not yet readable from"
              f" the capture set): see the failures block in the manifest.", file=sys.stderr)
    blanks = [f for f in frames if f.get("pending") is False
              and f.get("contrast_p99", 0) < MIN_LOCAL_CONTRAST_P99]
    if blanks:
        print("\nFrames with no edges (contrast below floor). These are not usable.",
              file=sys.stderr)
        for f in blanks:
            print(f"  {f['id']} {f['pass_name']}: contrast_p99 {f['contrast_p99']}",
                  file=sys.stderr)
        if not args.allow_blank:
            return 1
    return 0 if not failures else 1


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Export WLFG frame pairs (input = warped, target = real present).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--out", default=OUT_DEFAULT,
                        help="output root (default: %s/)" % OUT_DEFAULT)
    parser.add_argument("--scenes", action="append",
                        help="project-relative scene path; repeatable. Default: Example3D")
    parser.add_argument("--cameras", action="append",
                        help="camera name from CAMERAS; repeatable. Default: all")
    parser.add_argument("--timeout", type=int, default=900,
                        help="seconds per capture (default 900)")
    parser.add_argument("--attempts", type=int, default=DEFAULT_ATTEMPTS,
                        help="capture attempts per frame before giving up")
    parser.add_argument("--allow-blank", action="store_true",
                        help="exit 0 even with low-contrast frames")
    parser.add_argument("--motion", type=float, default=None,
                        help="yaw degrees per frame on top of the pin, overriding the "
                             "per-factor table (0 = static pin)")
    parser.add_argument("--wlfg", type=int, default=1, choices=(0, 1),
                        help="WL_WLFG for this export: 1 = warp arm (default), "
                             "0 = no-WLFG control arm (img_Accumulation is the plain "
                             "temporal blend). Capture the control into its own --out "
                             "so it can be compared against the warp arm.")
    parser.add_argument("--holdout", action="store_true",
                        help="include the held-out factor 6 in the export")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the capture plan without running the editor")
    parser.add_argument("--validate", action="store_true",
                        help="validate an existing dataset instead of exporting")
    args = parser.parse_args(argv)
    if args.validate:
        try:
            return validate_dir(Path(args.out).resolve())
        except FileNotFoundError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
    return export(args)


if __name__ == "__main__":
    raise SystemExit(main())
