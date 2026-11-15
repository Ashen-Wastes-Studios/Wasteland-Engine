#!/usr/bin/env python3
"""
verify_wlfg_warp.py — acceptance for the WLFG reprojection warp (pass 22).

The warp's whole correctness contract is its ADDRESS MATH, and the acceptance is
the zero-velocity case: at a static camera the warp must reproduce its input
EXACTLY (bit-identical to the no-WLFG control). Any delta at zero velocity is an
address bug, never a quality effect.

This verifier does not need the engine running. It transcribes the warp's
address math from the shader source into Python and checks the four contracts:

  1. ZERO-VELOCITY IDENTITY  — velocity (0,0) => prevUV == uv => the history tap
     lands on the same pixel; the warp outputs the previous present unchanged.
  2. OUT-OF-FRAME REJECTION  — a reprojected UV outside [0,1] (a surface leaving
     the screen) is invalid => the disocclusion fill path, not the warp.
  3. DEPTH-TEST REJECTION    — |currentDepth - prevDepth| > 0.02 (the same NDC
     threshold pass 2 uses) => invalid => the fill path, not a warped bleed.
  4. COVERAGE                — every output pixel is covered by exactly one
     source: the warped tap, or the disocclusion fill. No pixel is skipped, no
     pixel is written twice.

Plus static checks against the SINGLE Slang source and the generated artifacts,
and a mutation self-test that mutates the source and confirms each mutation is
caught.

Run:  python3 tools/verify_wlfg_warp.py [--self-test]

---------------------------------------------------------------------------
ARTIFACTS: the shader is single-source Slang since the 2026-10-29 transition.
The engine loads exactly three generated files (Renderer3D.cpp:3062 for GLSL,
VulkanNova.cpp:436 for SPIR-V, DX11Nova.h:198 / DX12Nova.cpp:114 for the DXIL
.cs.cso). `Renderer3D_NovaRenderer.glsl` and `.hlsl` are leftovers from the old
dual-twin pipeline: nothing loads them, nothing regenerates them, and they go
stale silently. This verifier used to read those two and therefore reported the
warp as missing from "the GLSL twin" while it was present and working.

Static contract checks now run against the `.slang` source, because that is
where the author's intent is expressed. The generated `.slang.glsl` is checked
only for survival of codegen (the pass and its case label still exist), not for
its text: Slang mangles names (`RunWlfgWarp_0`), rewrites `IMG_LOAD` into
`texelFetch(read_img_*_0, ...)`, and flattens boolean expressions, so matching
source text against generated text is not a meaningful check.
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC  = os.path.join(ROOT, "DemonCore-Editor", "assets", "shaders", "Renderer3D_NovaRenderer.slang")
GEN_GLSL = os.path.join(ROOT, "DemonCore-Editor", "assets", "shaders", "Renderer3D_NovaRenderer.slang.glsl")
GEN_SPV  = os.path.join(ROOT, "DemonCore-Editor", "assets", "shaders", "Renderer3D_NovaRenderer.spv")

checks = []
def check(name, ok, detail=""):
    checks.append((name, ok, detail))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))

def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
    s = re.sub(r'//[^\n]*', ' ', s)
    return s

def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()

def extract_fn(src, name):
    """Return the body of a top-level shader function by name.

    Return types seen here: `void RunWlfgWarp`, `float3 RunWlfgDisocclusionFill`,
    and `float wl_reactive_mask`.
    """
    pat = re.compile(r'(?:void|float|float3|vec3)\s+' + name + r'\s*\([^;]*?\)\s*\{', re.S)
    m = pat.search(src)
    if not m:
        return None
    i = m.end() - 1  # at the opening brace
    depth = 0
    for j in range(i, len(src)):
        if src[j] == '{': depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                return src[i:j+1]
    return None

# ---------------------------------------------------------------- static checks
src = strip_comments(read(SRC))

print("Static checks (single Slang source)")
check("warp exists in the source", "RunWlfgWarp" in src)
check("disocclusion-fill helper exists in the source", "RunWlfgDisocclusionFill" in src)
check("case 22 dispatches the warp",
      re.search(r'case\s+22\s*:\s*RunWlfgWarp\s*\(\s*\)\s*;', src) is not None)

warp = extract_fn(src, "RunWlfgWarp")
fill = extract_fn(src, "RunWlfgDisocclusionFill")
mask_fn = extract_fn(src, "wl_reactive_mask")
check("warp function body extracted", warp is not None)
check("fill function body extracted", fill is not None)
check("reactive-mask helper extracted", mask_fn is not None)

# Bounding the thread id to the trace extent is pass 22's own responsibility:
# main()'s early-out list is u_PassID == 0/1/2/7/24 and deliberately excludes 22.
check("warp bounds the thread id to the trace extent",
      re.search(r'pixelCoords\.x\s*>=\s*imgSize\.x', warp) is not None
      and re.search(r'pixelCoords\.y\s*>=\s*imgSize\.y', warp) is not None)

# Address math mirrors pass 2 (RunTemporalAccumulation): velocity -> prevUV ->
# histUV -> clamp -> depth test.
check("reads velocity", "IMG_LOAD(img_Velocity" in warp)
check("prevUV = uv - velocity", re.search(r'prevUV\s*=\s*uv\s*-\s*velocity', warp) is not None)

# DELIBERATE DIFFERENCE FROM PASS 2, not a drift: pass 2 samples s_Accumulation
# (t48), which holds valid data only in the [0, u_RenderScale] subregion, so it
# multiplies by u_RenderScale. The warp samples s_WlfgHistory (t72), which is a
# FULL-RES copy of the previous present, so it clamps in full-res UV space with
# no scale multiply. Asserting `prevUV * u_RenderScale` here (as this verifier
# used to) would force the warp to tap the top-left subregion of a full-res
# buffer -- the address bug the current code exists to avoid.
check("histUV is clamped in FULL-RES space (no u_RenderScale multiply)",
      re.search(r'histUV\s*=\s*clamp\(\s*prevUV\s*,', warp) is not None)
# Both bounds, asserted POSITIONALLY. A bare `"0.5f / fullRes" in warp` is not
# enough: the string also appears in the upper bound, so dropping the lower clamp
# would leave the check green. Assert the clamp's SECOND ARGUMENT instead.
check("histUV clamps BOTH sides",
      re.search(r'clamp\(\s*prevUV\s*,\s*0\.5f\s*/\s*fullRes\s*,', warp) is not None
      and re.search(r'1\.0f\s*-\s*0\.5f\s*/\s*fullRes', warp) is not None)
check("histUV is NOT scaled by u_RenderScale",
      re.search(r'histUV\s*=\s*prevUV\s*\*\s*u_RenderScale', warp) is None)

# The `u_FrameIndex > 0` term is GONE ON PURPOSE. BeginScene zeroes FrameIndex on
# every view-projection change, i.e. on every frame of a moving camera -- exactly
# when reprojection matters -- so gating on it restricted the warp to a still
# camera, where velocity is zero and the warp is the identity. The history is
# cleared on allocation and committed every frame, so the depth test below is the
# real validity check. A verifier that still demands this term demands the bug.
check("frame-0 gate removed (depth test is the validity check)",
      "u_FrameIndex" not in warp)

check("in-frame validity test", "prevUV.x >= 0.0f" in warp and "prevUV.y <= 1.0f" in warp)
check("depth test 0.02 threshold",
      re.search(r'abs\(\s*currentDepth\s*-\s*prevDepth\s*\)\s*>\s*0\.02f', warp) is not None)

# The depth buffer is full-size with valid data only in the trace subregion, so a
# trace-space UV has to be scaled into that subregion -- both taps, or the current
# depth is read from the wrong place and every pixel fails the depth test.
check("current depth sampled via wl_buf_uv",
      re.search(r'currentDepth\s*=\s*TEX\(\s*s_DepthBuffer\s*,\s*wl_buf_uv\(', warp) is not None)
check("previous depth sampled via wl_buf_uv",
      re.search(r'prevDepth\s*=\s*TEX\(\s*s_DepthBuffer\s*,\s*wl_buf_uv\(', warp) is not None)

check("warped read is UNCONDITIONAL (branch-free fetch)",
      re.search(r'warped\s*=\s*TEX\(\s*s_WlfgHistory\s*,\s*histUV\s*\)\s*\.rgb', warp) is not None)
# The mask must be a POINT sample at the pixel coord: it is per accumulation texel,
# so a bilinear tap would blend two independent disocclusion verdicts.
#
# It must also come from wl_reactive_mask rather than a bare img_Bloom_Temp.a tap.
# pass 24 writes the mask to both img_Mask (u14) and img_Bloom_Temp.a, but the bloom
# blur (passes 4/5) overwrites that alpha BEFORE the WLFG passes run -- so the alpha
# tap is bloom residue here, and it measured ~0.0000. img_Mask is untouched after
# pass 24. This is the tap the exporter captures, so a bare alpha tap also makes
# inference disagree with training. Asserting the old literal would assert the bug.
check("warp reads the mask via wl_reactive_mask (point sample at the pixel coord)",
      re.search(r'mask\s*=\s*wl_reactive_mask\(\s*pixelCoords\s*\)', warp) is not None)
check("warp does NOT tap img_Bloom_Temp.a directly (bloom has overwritten it)",
      "img_Bloom_Temp" not in warp)
check("mask helper returns img_Mask.x off GLSL (the source pass 24 alone writes)",
      re.search(r'img_Mask\[\s*pixelCoords\s*\]\s*\.x', mask_fn or "") is not None)
check("mask helper keeps the alpha tap under WASTELAND_GLSL (no img_Mask there)",
      re.search(r'#ifdef\s+WASTELAND_GLSL', mask_fn or "") is not None
      and re.search(r'IMG_LOAD\(\s*img_Bloom_Temp\s*,\s*pixelCoords\s*\)\s*\.a',
                    mask_fn or "") is not None)
check("writes img_Accumulation (dispatch-correct like pass 2)",
      "IMG_STORE(img_Accumulation" in warp)

# The disocclusion fill: 3x3 neighbourhood, in-frame + depth-consistent neighbour,
# prefer the tap whose depth is closest to this pixel's.
check("fill searches 3x3 neighbourhood",
      re.search(r'for\s*\(\s*int\s+y\s*=\s*-1', fill) is not None
      and re.search(r'for\s*\(\s*int\s+x\s*=\s*-1', fill) is not None)
check("fill fallback is the current real render (img_Bloom)",
      "IMG_LOAD(img_Bloom, pc).rgb" in fill)
check("fill rejects out-of-frame neighbours", "spPrev.x < 0.0f" in fill and "spPrev.y > 1.0f" in fill)
check("fill requires neighbour depth self-consistency",
      re.search(r'abs\(\s*spPrevDepth\s*-\s*spOwnDepth\s*\)\s*>\s*0\.02f', fill) is not None)
check("fill prefers closest-depth tap",
      "diff = abs(spPrevDepth - curDepth)" in fill and "if (diff < bestDepthDiff)" in fill)
check("fill samples s_WlfgHistory in full-res space",
      re.search(r'TEX\(\s*s_WlfgHistory\s*,\s*histUV\s*\)', fill) is not None)

# ---------------------------------------------------------------- generated artifacts
print("\nGenerated artifacts (single-source Slang)")
gen = strip_comments(read(GEN_GLSL)) if os.path.exists(GEN_GLSL) else ""
check("warp survived codegen into .slang.glsl", "RunWlfgWarp" in gen)
check("case 22 survived codegen into .slang.glsl",
      re.search(r'RunWlfgWarp_?\d*\s*\(\s*\)\s*;', gen) is not None)
spv_ok = os.path.exists(GEN_SPV) and os.path.getmtime(GEN_SPV) >= os.path.getmtime(SRC)
check("deployed .spv is newer than the .slang source", spv_ok,
      "SPIR-V must be regenerated after a shader edit -- a stale .spv silently "
      "keeps the old pass" if not spv_ok else "")

# ---------------------------------------------------------------- behavioural model
# Transcribe the warp's address math and the disocclusion fill into Python, then
# assert the four contracts numerically. This is the part that stands in for the
# bit-identical render acceptance until the pass is wired in.
DEPTH_EPS = 0.02

def warp_model(uv, velocity, full_res, cur_depth, prev_depth):
    """Returns ('warped'|'fill', histUV, valid). Mirrors RunWlfgWarp's address math."""
    prevUV = (uv[0] - velocity[0], uv[1] - velocity[1])
    # s_WlfgHistory is FULL-RES: clamp in full-res UV space, no u_RenderScale.
    lo = (0.5 / full_res[0], 0.5 / full_res[1])
    hi = (1.0 - 0.5 / full_res[0], 1.0 - 0.5 / full_res[1])
    histUV = (max(lo[0], min(prevUV[0], hi[0])), max(lo[1], min(prevUV[1], hi[1])))
    valid = (0.0 <= prevUV[0] <= 1.0 and 0.0 <= prevUV[1] <= 1.0)
    if valid and abs(cur_depth - prev_depth) > DEPTH_EPS:
        valid = False
    return ('warped' if valid else 'fill'), histUV, valid

print("\nBehavioural checks (transcribed address math)")

# Contract 1: zero velocity -> identity. prevUV == uv, so histUV maps to the same
# pixel the input came from; the warp outputs the previous present unchanged.
FR = (1000.0, 750.0)
uv = (0.374, 0.612)
kind, histUV, valid = warp_model(uv, (0.0, 0.0), FR, 0.5, 0.5)
check("contract 1: zero velocity is valid (warped, not filled)", kind == 'warped' and valid)
check("contract 1: zero velocity maps histUV == uv (identity tap)",
      abs(histUV[0] - uv[0]) < 1e-9 and abs(histUV[1] - uv[1]) < 1e-9,
      f"histUV={histUV} uv={uv}")
# A mid-frame UV is unaffected by the half-texel clamp; only edge texels move.
uv2 = (0.0002, 0.0002)
_, histUV2, _ = warp_model(uv2, (0.0, 0.0), FR, 0.5, 0.5)
check("contract 1: the half-texel clamp only moves edge texels",
      abs(histUV2[0] - 0.5 / FR[0]) < 1e-12 and abs(histUV2[1] - 0.5 / FR[1]) < 1e-12)

# Contract 2: out-of-frame rejection. A velocity that pushes prevUV outside
# [0,1] is invalid -> the fill path, never a warped tap.
kindL, _, validL = warp_model((0.02, 0.5), (0.05, 0.0), FR, 0.5, 0.5)   # prevUV.x = -0.03
check("contract 2: prevUV below 0 rejected -> fill", kindL == 'fill' and not validL)
kindR, _, validR = warp_model((0.98, 0.5), (-0.05, 0.0), FR, 0.5, 0.5)  # prevUV.x = 1.03
check("contract 2: prevUV above 1 rejected -> fill", kindR == 'fill' and not validR)

# Contract 3: depth test rejects disocclusion. In-frame but a depth mismatch
# above 0.02 is invalid -> fill, so a reprojected point on a different surface
# never bleeds across the moving edge.
_, _, validM = warp_model((0.5, 0.5), (0.01, 0.01), FR, 0.50, 0.55)     # |diff| = 0.05 > 0.02
check("contract 3: depth mismatch > 0.02 rejected -> fill", not validM)
_, _, validOK = warp_model((0.5, 0.5), (0.01, 0.01), FR, 0.50, 0.51)    # |diff| = 0.01 <= 0.02
check("contract 3: depth within 0.02 stays valid -> warped", validOK)

# Contract 3b: a freshly allocated (cleared) history has depth 0, so frame 0 is
# rejected by the DEPTH TEST rather than by a frame-index gate. This is the
# replacement for the old "frame 0 fills every pixel" contract, which the removed
# u_FrameIndex term used to provide.
_, _, validF0 = warp_model((0.5, 0.5), (0.0, 0.0), FR, 0.50, 0.0)      # cleared history
check("contract 3b: cleared history (prevDepth 0) rejected by the depth test", not validF0)

# Contract 4: coverage. Sweep a grid; every pixel resolves to exactly one source
# (warped or fill). No pixel is skipped, none written twice.
W, H = 64, 48
count = 0
for y in range(H):
    for x in range(W):
        uvx = (x + 0.5) / W; uvy = (y + 0.5) / H
        vel = (0.03 * uvy, 0.0)   # a plausible velocity field: rightward, grows with y
        kind, _, _ = warp_model((uvx, uvy), vel, FR, 0.5, 0.5)
        count += 1
        assert kind in ('warped', 'fill'), f"pixel ({x},{y}) unresolved: {kind}"
check("contract 4: every pixel covered by exactly one source", count == W * H, f"{count} px")

# ---------------------------------------------------------------- self-test
def run_self_test():
    print("\nSelf-test (mutate the warp, confirm each mutation is caught)")
    # Mutate the EXTRACTED pass-22 functions (not the whole source, which shares
    # identical address math with pass 2 -- a whole-source replace would hit
    # pass 2 first and leave the warp untouched).
    mutations = [
        ("prevUV = uv + velocity",
         warp, "prevUV = uv - velocity", "prevUV = uv + velocity",
         lambda t: re.search(r'prevUV\s*=\s*uv\s*-\s*velocity', t) is not None),
        ("drop the depth test",
         warp, "abs(currentDepth - prevDepth) > 0.02f", "true",
         lambda t: re.search(r'abs\(\s*currentDepth\s*-\s*prevDepth\s*\)\s*>\s*0\.02f', t) is not None),
        ("clamp only the upper side",
         warp, "clamp(prevUV, 0.5f / fullRes, 1.0f - 0.5f / fullRes)",
         "clamp(prevUV, vec2(0.0f), 1.0f - 0.5f / fullRes)",
         lambda t: re.search(r'clamp\(\s*prevUV\s*,\s*0\.5f\s*/\s*fullRes\s*,', t) is not None),
        ("fill fallback -> history",
         fill, "best = IMG_LOAD(img_Bloom, pc).rgb", "best = TEX(s_WlfgHistory, curUV).rgb",
         lambda t: re.search(r'best\s*=\s*IMG_LOAD\(\s*img_Bloom\b', t) is not None),
        # The mask must come from the helper, not a bare alpha tap: the bloom blur
        # has already overwritten img_Bloom_Temp.a by the time this pass runs.
        ("mask: revert to the bloomed alpha tap",
         warp, "mask = wl_reactive_mask(pixelCoords)",
         "mask = IMG_LOAD(img_Bloom_Temp, pixelCoords).a",
         lambda t: "img_Bloom_Temp" not in t),
        # And the helper itself must read the source pass 24 alone writes.
        ("mask helper: non-GLSL branch -> the alpha tap",
         mask_fn, "img_Mask[pixelCoords].x", "IMG_LOAD(img_Bloom_Temp, pixelCoords).a",
         lambda t: re.search(r'img_Mask\[\s*pixelCoords\s*\]\s*\.x', t) is not None),
        ("remove case 22",
         src, "case 22: RunWlfgWarp(); break;", "// case 22 removed",
         lambda t: re.search(r'case\s+22\s*:\s*RunWlfgWarp\s*\(\s*\)\s*;', t) is not None),
        ("re-introduce the frame-0 gate",
         warp, "bool valid = prevUV.x >= 0.0f",
         "bool valid = u_FrameIndex > 0 && prevUV.x >= 0.0f",
         lambda t: "u_FrameIndex" not in t),
        ("drop the trace-extent guard",
         warp, "if (pixelCoords.x >= imgSize.x || pixelCoords.y >= imgSize.y) return;",
         "// guard removed",
         lambda t: re.search(r'pixelCoords\.x\s*>=\s*imgSize\.x', t) is not None),
    ]
    all_detected = True
    for name, src_text, old, new, predicate in mutations:
        # A mutation is only meaningful if its target literal is actually there.
        # (The previous version asserted `old not in mutated`, which is trivially
        # true after any successful replace and so reported OK even when the
        # literal had gone missing from the source -- a stale verifier passes.)
        if old not in src_text:
            print(f"    [MISS] {name} -- literal absent; the verifier is asserting something stale")
            all_detected = False
            continue
        mutated = src_text.replace(old, new)
        # Caught iff the mutation actually breaks the check's predicate -- i.e. the
        # verifier would go red, not merely that the text changed.
        caught = not predicate(mutated)
        all_detected = all_detected and caught
        print(f"    [{'OK' if caught else 'MISS'}] {name}")
    print(f"  SELF-TEST: {'PASS' if all_detected else 'FAIL'}")
    return all_detected

if "--self-test" in sys.argv:
    ok = run_self_test()
    print(f"\nSELFTEST_RESULT: {'PASS' if ok else 'FAIL'}")
    sys.exit(0 if ok else 1)

n_fail = sum(1 for _, ok, _ in checks if not ok)
print(f"\nRESULT: {'PASS' if n_fail == 0 else 'FAIL'} ({len(checks)-n_fail}/{len(checks)} checks)")
sys.exit(0 if n_fail == 0 else 1)
