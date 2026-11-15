#!/usr/bin/env python3
"""T0.3 A/B harness for the effects register (WL_EFFECTS).

Runs the editor twice against the SAME pinned camera and frame, differing only
in the effects bitmask:
  arm A: WL_EFFECTS ABSENT  -> u_EffectsFlags = 0 -> no debug tint
  arm B: WL_EFFECTS=1       -> bit 0 (WL_EFLAGS_DEBUG_TINT) -> magenta cast

Both capture the Nova output PNG (WL_SCREENSHOT). The arms must differ: arm B
must be measurably more magenta / shifted than arm A. This is the ground-truth
check that u_EffectsFlags is declared AND read (Set()/glGetUniformLocation are
silent on a name miss, so a plumbed-but-unread flag would return two identical
PNGs).

Usage: python3 tools/ab_effects_flags.py [/path/to/outdir]
"""
import os, sys, glob, subprocess, time

REPO = r"C:\dev\Wasteland"
EXE = os.path.join(REPO, "bin", "Release-windows-x86_64", "DemonCore-Editor", "DemonCore-Editor.exe")
WORK = os.path.join(REPO, "bin", "Release-windows-x86_64", "DemonCore-Editor")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "ab_tint")

FRAME = os.environ.get("WL_SCREENSHOT_FRAME_FORCE", "120")
PIN = os.environ.get("WASTELAND_PIN_CAMERA",
                     "0,20,0,12,20,45")  # origin(0,20,0) distance 12 pitch 20 yaw 45

def capture(tag, effects):
    sh = os.path.join(OUT, f"effects_{tag}.png")
    env = os.environ.copy()
    env["WL_SCREENSHOT"] = sh
    env["WL_SCREENSHOT_FRAME"] = FRAME
    env["WL_SCREENSHOT_LINGER"] = "6"
    env["WASTELAND_PIN_CAMERA"] = PIN
    env["WL_FREEZE_TIME"] = "1"
    env["WL_AA_FILTER"] = "11"  # ATrous denoise baseline (per export_dataset recipe)
    if effects is not None:
        env["WL_EFFECTS"] = str(effects)
    else:
        env.pop("WL_EFFECTS", None)
    os.makedirs(OUT, exist_ok=True)
    proc = subprocess.Popen([EXE], cwd=WORK, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    # LINGER makes the engine close n frames after the shot; reap with a bound.
    try:
        out, _ = proc.communicate(timeout=120)
    except subprocess.TimeoutExpired:
        proc.kill()
        out = proc.stdout.read() if proc.stdout else ""
    # engine may log the override; surface it for the "did the override fire" proof
    flagged = [ln for ln in (out or "").splitlines() if "Effects-register override" in ln]
    ok = os.path.exists(sh)
    print(f"arm {tag:>2} effects={effects}: saved={ok} sz={os.path.getsize(sh) if ok else 0} override_log={flagged}")
    return sh

def mean_rgb(png):
    from struct import unpack  # read PNG IHDR + decode via PyPNG-free path
    # cheap decode: use PIL if present, else a minimal PNG decoder
    try:
        from PIL import Image
        im = Image.open(png).convert("RGB")
        px = im.load()
        w, h = im.size
        tot = [0.0, 0.0, 0.0]
        for y in range(0, h, max(1, h // 64)):
            for x in range(0, w, max(1, w // 64)):
                c = px[x, y]
                tot[0] += c[0]; tot[1] += c[1]; tot[2] += c[2]
        n = (h // max(1, h//64) - 1) * (w // max(1, w//64) - 1) + 1
        return [t / n for t in tot]
    except ImportError:
        return None

a = capture("A", None)
b = capture("B", 1)   # 0x1 = DEBUG_TINT

ma, mb = mean_rgb(a), mean_rgb(b)
print(f"\narm A mean RGB: {['%.1f' % v for v in ma] if ma else 'n/a'}")
print(f"arm B mean RGB: {['%.1f' % v for v in mb] if mb else 'n/a'}")
if ma and mb:
    # magenta cast = red+blue pulled up relative to green
    pa = ma[0] + ma[2] - 2 * ma[1]
    pb = mb[0] + mb[2] - 2 * mb[1]
    print(f"magenta metric (R+B-2G): A={pa:.1f}  B={pb:.1f}  delta={pb-pa:.1f}")
    print("PASS  flag is READ (B differs from A, B more magenta)" if pb - pa > 4.0
          else "CHECK  arms close — the flag may be plumbed-but-unread, or capture sits on the wrong frame")
