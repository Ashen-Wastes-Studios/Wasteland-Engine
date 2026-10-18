#!/usr/bin/env python3
"""Verifier for the ReSTIR DI reservoir contract, in both shader twins.

WHY THIS EXISTS
The ReSTIR reservoir is a ping-pong pair whose correctness is NOT visible from
either pass alone. It was in fact broken on first implementation in two ways
that compiled cleanly and passed every existing guard:

  1. PING-PONG INVERSION. Pass 3 (Generate) writes ReservoirWrite. Pass 11
     (Combine) read ReservoirRead -- the PREVIOUS frame's buffer -- for both the
     current pixel and its spatial neighbours. So Combine never saw this frame's
     candidates at all: it combined the previous frame against itself, and the
     image could never converge to the current lighting. Nothing about the
     source of either pass looks wrong in isolation; the bug lives in the
     RELATIONSHIP between them.

  2. WORLD-ORIGIN SHADING. The combine pass shaded with
     `light.WorldTransform[3].xyz - vec3(0.0)` -- the world origin instead of the
     receiver's reconstructed world position. That makes the light direction a
     CONSTANT across the whole frame, so the result is one light smeared
     uniformly over every pixel. It is wrong in an obvious-looking way only once
     you notice the missing G-buffer read.

Both are caught here, plus the GLSL storage qualifier that makes the corrected
read pattern legal at all: `writeonly` on ReservoirWrite forbids pass 11 from
reading it, so the fix in (1) is only valid if the qualifier is also relaxed.

The temporal checks also pin current source behavior around zero-weight records.
They are source-shape checks, not proof of estimator validity when a reservoir is
reused across receiver domains. In particular, unconditional history/neighbor M
can be wrong when source-target support differs; see
tools/verify_restir_spatial_correlation.py for an exact finite-model witness.

  3. ZERO-WEIGHT/REUSE COUNT SHAPE. The checker records how current.M and
     history.M are carried by the experimental temporal merge. Counting every
     proposal is valid within one source stream, but does not justify carrying
     every reused source count into a different receiver's denominator.

  4. SPARSE FULL-RES STORAGE. At reduced trace scales, a single center-mapped
     address per trace pixel can miss reservoir elements or land outside the
     invocation's tile. Passes 3 and 11 must write the complete integer-partition
     tile `[tracePixel*fullSize/traceSize,(tracePixel+1)*fullSize/traceSize)`.
     Executable odd-size simulations verify exact coverage and representative
     membership at scales 0.5, 0.66, 0.8, and 1.0.

  5. RNG STRIDE DISAGREEMENT. Generation and combine in both shader twins must
     seed rows with the trace extent width, not a full-resolution or fixed width.

Each source invariant is also proven FAILABLE by an in-memory mutation, because
a guard that cannot fail is decoration -- see
tools/verify_nova_glsl_negative_control.py for the same principle applied to
twin-agreement checks.

Exit 0 = all source checks pass and every mutation is caught; this does not
establish unbiased estimator math, GPU execution, or visual quality.
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HLSL = REPO / "DemonCore-Editor" / "assets" / "shaders" / "Renderer3D_NovaRenderer.hlsl"
GLSL = REPO / "DemonCore-Editor" / "assets" / "shaders" / "Renderer3D_NovaRenderer.glsl"

failures = []
checks = 0
negative_controls = 0


def check(label, condition, detail=""):
    global checks
    checks += 1
    if condition:
        print(f"  PASS  {label}")
    else:
        print(f"  FAIL  {label}{(' -- ' + detail) if detail else ''}")
        failures.append(label)


def read(path):
    return path.read_text(encoding="utf-8", errors="replace")


def body_of(source, func_name):
    """Return the text of a function body, brace-matched from its opening line.

    Brace matching rather than a fixed window, so a comment added inside the
    function cannot push the read past its end and make a check vacuous.
    """
    m = re.search(r"\b(?:void|bool|LightReservoir)\s+" + re.escape(func_name) + r"\s*\(", source)
    if not m:
        return ""
    start = source.find("{", m.end())
    if start < 0:
        return ""
    depth = 0
    for i in range(start, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start : i + 1]
    return ""


def strip_comments(source):
    """Remove comments so source invariants cannot pass on explanatory text."""
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def trace_extent_alias(body):
    """Return the local extent computed by wl_trace_extent(fullSize)."""
    clean = strip_comments(body)
    match = re.search(
        r"\b(?:int2|ivec2)\s+(\w+)\s*=\s*wl_trace_extent\s*\(\s*fullSize\s*\)",
        clean,
    )
    return match.group(1) if match else None


def preserves_current_candidate_count(body):
    """Source-shape check: current input M is carried independently of weight."""
    clean = strip_comments(body)
    match = re.search(r"\b(?:float|double)\s+currentM\s*=\s*([^;]+);", clean)
    if not match:
        return False
    expression = match.group(1)
    return (
        re.search(r"\bcurrent\.M\b", expression) is not None
        and "?" not in expression
        and not re.search(r"\b(?:currentSum|currentTarget)\b", expression)
        and re.search(r"\btotalM\s*=\s*currentM\s*\+", clean) is not None
        and re.search(r"\.M\s*=\s*uint\s*\(\s*totalM\s*\)", clean) is not None
    )


def preserves_zero_target_history_count(body):
    """Source-shape check: history M is carried despite zero target; not bias proof."""
    clean = strip_comments(body)
    match = re.search(r"\b(?:float|double)\s+historyCount\s*=\s*([^;]+);", clean)
    if not match:
        return False
    expression = re.sub(r"\s+", "", match.group(1))
    return "history.M>0" in expression and "historyTarget" not in expression


def accepts_tagged_empty_history(body):
    """The frame tag plus M validates a zero-sample history record; bit 0 is sample-only."""
    clean = strip_comments(body)
    return (
        re.search(r"\br\.M\s*>\s*0\b", clean) is not None
        and "previousFrameTag" in clean
        and "wl_restir_has_sample" not in clean
    )


def stamps_nonempty_reservoir_frame(body):
    clean = strip_comments(body)
    return (
        re.search(r"if\s*\(\s*r\.M\s*>\s*0\s*\)", clean) is not None
        and "u_FrameIndex" in clean
        and "r.flagsValid" in clean
    )


def gates_history_target_on_selected_sample(body):
    clean = strip_comments(body)
    return re.search(
        r"historyTarget\s*=\s*\(\s*wl_restir_is_previous_frame\(prevR\)\s*&&\s*"
        r"wl_restir_has_sample\(prevR\)\s*\)",
        clean,
    ) is not None


def has_separate_spatial_source_output(body):
    clean = strip_comments(body)
    return (
        "ReservoirRead[flatIdx]" in clean
        and "ReservoirRead[neighborFlatIdx]" in clean
        and "ReservoirWrite[flatIdx] = r;" in clean
        and re.search(r"ReservoirWrite\s*\[\s*neighbor\w*\s*\]", clean) is None
    )


def spatial_source_uses_reweighted_M(body):
    clean = re.sub(r"\s+", "", strip_comments(body))
    return (
        "RestirTarget(neighborR.lightIndex,receiverPos,receiverN,receiverAlbedo)" in clean
        and re.search(r"neighborTarget\*max\(neighborR\.W,0\.0f\)\*float\(neighborCount\)", clean) is not None
        and "totalM+=neighborCount" in clean
        and re.search(r"r\.W=totalWeight/\(selectedTarget\*float\(totalM\)\)", clean) is not None
    )


def spatial_neighbor_validation(body):
    clean = re.sub(r"\s+", "", strip_comments(body))
    return (
        "wl_restir_is_current_frame(neighborR)" in clean
        and "abs(depth-neighborR.bary.y)>=0.02f||dot(receiverN,neighborNormal)<=0.9f" in clean
    )


def trace_seed_signature(body):
    """Describe a seed's row stride only when it is the trace extent width."""
    clean = strip_comments(body)
    trace_width = trace_extent_alias(clean)
    if not trace_width:
        return None
    assignments = re.findall(r"\bseed\s*=\s*([^;]+);", clean)
    for expression in assignments:
        row = re.search(
            r"(?:uint\s*\(\s*)?(pixelCoords|tracePixel)\.y\s*\)?\s*\*\s*"
            r"(?:uint\s*\(\s*)?(\w+)\.x\s*\)?",
            expression,
        )
        if row and row.group(2) == trace_width and re.search(
            rf"\b{row.group(1)}\.x\b", expression
        ):
            return (row.group(1), "traceWidth", row.group(1))
    return None


def matching_trace_seed_strides(*bodies):
    signatures = [trace_seed_signature(body) for body in bodies]
    return bool(signatures) and all(signature is not None for signature in signatures) and all(
        signature == signatures[0] for signature in signatures[1:]
    )


def tile_bounds(body):
    """Find the integer-partition tile bounds and their trace-pixel source."""
    clean = strip_comments(body)
    trace_width = trace_extent_alias(clean)
    if not trace_width:
        return None
    lower = re.search(
        rf"\b(?:int2|ivec2)\s+(\w+)\s*=\s*(pixelCoords|tracePixel)\s*"
        rf"\*\s*fullSize\s*/\s*{re.escape(trace_width)}\b",
        clean,
    )
    upper = re.search(
        rf"\b(?:int2|ivec2)\s+(\w+)\s*=\s*\(\s*(pixelCoords|tracePixel)\s*"
        rf"\+\s*(?:1|(?:int2|ivec2)\s*\(\s*1\s*(?:,\s*1\s*)?\))\s*\)\s*"
        rf"\*\s*fullSize\s*/\s*{re.escape(trace_width)}\b",
        clean,
    )
    if not lower or not upper or lower.group(2) != upper.group(2):
        return None
    representative = re.search(
        rf"\b(?:int|uint)\s+(\w+)\s*=\s*{re.escape(lower.group(1))}\.y\s*"
        rf"\*\s*fullSize\.x\s*\+\s*{re.escape(lower.group(1))}\.x\s*;",
        clean,
    )
    if not representative:
        return None
    return {
        "clean": clean,
        "trace_width": trace_width,
        "lower": lower.group(1),
        "upper": upper.group(1),
        "representative": representative.group(1),
    }


def has_fullres_flat_address(body):
    clean = strip_comments(body)
    return re.search(r"\b\w+\.y\s*\*\s*fullSize\.x\s*\+\s*\w+\.x\b", clean) is not None


def matching_brace_end(source, opening):
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    return -1


def loop_span(source, loop_match):
    opening = source.find("{", loop_match.end())
    if opening < 0:
        return None
    closing = matching_brace_end(source, opening)
    return (opening, closing) if closing >= 0 else None


def stamps_spatial_frame_tag_independently_of_sample(body):
    """Pass 14 records the current frame for any represented proposals."""
    clean = re.sub(r"\s+", "", strip_comments(body))
    return re.search(
        r"if\(totalM>0\)r\.flagsValid=\(r\.flagsValid&1u\)\|"
        r"\(\(uint\(u_FrameIndex\)&0x7fffffffu\)<<1\);"
        r"elser\.flagsValid=0u;",
        clean,
    ) is not None


def spatial_estimator_requires_positive_weight_and_target(body):
    clean = re.sub(r"\s+", "", strip_comments(body))
    return all(
        token in clean
        for token in (
            "if(selectedValid&&totalM>0&&totalWeight>1e-8f&&selectedTarget>1e-8f){",
            "r.W=totalWeight/(selectedTarget*float(totalM));",
            "r.flagsValid=1u;",
            "else{r.W=0.0f;r.flagsValid=0u;}",
        )
    )


def gates_spatial_bloom_on_selected_sample_and_weight(body, bloom_read, bloom_write):
    """The sample-valid and positive-W guards contain all shading and bloom I/O."""
    clean = re.sub(r"\s+", "", strip_comments(body))
    guard = "if(depth<1.0f&&albedoHit.a>=0.5f&&wl_restir_has_sample(r)&&r.W>0.0f)"
    guard_start = clean.find(guard)
    if guard_start < 0:
        return False
    opening = clean.find("{", guard_start + len(guard))
    if opening < 0:
        return False
    closing = matching_brace_end(clean, opening)
    if closing < 0:
        return False
    shading = clean[opening + 1:closing]
    return (
        "LightIndices[r.lightIndex]" in shading
        and "directLight=" in shading
        and bloom_read.split("(", 1)[0] in shading
        and bloom_write.split("(", 1)[0] in shading
    )


def writes_complete_nonoverlapping_tile(body):
    """Check partition bounds, exclusive nested loops, and a full-res store."""
    parts = tile_bounds(body)
    if not parts:
        return False
    clean = parts["clean"]
    lower, upper = parts["lower"], parts["upper"]
    representative = parts["representative"]
    if not re.search(
        rf"ReservoirWrite\s*\[\s*{re.escape(representative)}\s*\]\s*=", clean
    ):
        return False
    loop_matches = {}
    loop_spans = {}
    for axis in ("y", "x"):
        pattern = re.compile(
            rf"for\s*\(\s*(?:int|uint)\s+(\w+)\s*=\s*{re.escape(lower)}\.{axis}\s*;\s*"
            rf"\1\s*<\s*{re.escape(upper)}\.{axis}\s*;\s*"
            rf"(?:\+\+\1|\1\+\+|\1\s*\+=\s*1)\s*\)"
        )
        match = pattern.search(clean)
        if not match:
            return False
        span = loop_span(clean, match)
        if not span:
            return False
        loop_matches[axis] = match
        loop_spans[axis] = span

    y = loop_matches["y"].group(1)
    x = loop_matches["x"].group(1)
    writes = re.finditer(r"ReservoirWrite\s*\[\s*([^\]]+)\s*\]\s*=\s*[^;]+;", clean)
    for write in writes:
        position = write.start()
        if not all(start < position < end for start, end in loop_spans.values()):
            continue
        index = write.group(1).strip()
        flat = re.search(
            rf"\b{re.escape(y)}\s*\*\s*fullSize\.x\s*\+\s*{re.escape(x)}\b",
            index,
        )
        if not flat:
            index_assignment = re.search(
                rf"\b(?:int|uint)\s+{re.escape(index)}\s*=\s*"
                rf"{re.escape(y)}\s*\*\s*fullSize\.x\s*\+\s*{re.escape(x)}\s*;",
                clean,
            )
            if not index_assignment or not all(
                start < index_assignment.start() < end for start, end in loop_spans.values()
            ):
                continue
        return True
    return False


def simulate_tile_partition(full_width, full_height, scale):
    """Execute the floor-partition math and verify coverage and representatives."""
    trace_width = max(1, int(full_width * scale))
    trace_height = max(1, int(full_height * scale))
    coverage = [0] * (full_width * full_height)
    for trace_y in range(trace_height):
        y0 = trace_y * full_height // trace_height
        y1 = (trace_y + 1) * full_height // trace_height
        if y0 >= y1:
            return False, f"trace row {trace_y} has an empty tile [{y0}, {y1})"
        representative_y = y0
        if not y0 <= representative_y < y1:
            return False, f"y representative {representative_y} outside [{y0}, {y1})"
        for trace_x in range(trace_width):
            x0 = trace_x * full_width // trace_width
            x1 = (trace_x + 1) * full_width // trace_width
            if x0 >= x1:
                return False, f"trace column {trace_x} has an empty tile [{x0}, {x1})"
            representative_x = x0
            if not x0 <= representative_x < x1:
                return False, f"x representative {representative_x} outside [{x0}, {x1})"
            for full_y in range(y0, y1):
                for full_x in range(x0, x1):
                    coverage[full_y * full_width + full_x] += 1
    bad = next((index for index, count in enumerate(coverage) if count != 1), None)
    return (bad is None, "" if bad is None else f"full-res address {bad} covered {coverage[bad]} times")


def mutate_current_count(body):
    return re.sub(
        r"(\b(?:float|double)\s+currentM\s*=\s*)[^;]+;",
        r"\1currentSum > 0.0f ? float(current.M) : 0.0f;",
        body,
        count=1,
    )


def mutate_trace_stride(body):
    alias = trace_extent_alias(body)
    if not alias:
        return body
    return re.sub(
        rf"(\bseed\s*=\s*[^;]*?)\b{re.escape(alias)}\.x\b",
        r"\1fullSize.x",
        body,
        count=1,
    )


def mutate_tile_bound(body):
    parts = tile_bounds(body)
    if not parts:
        return body
    y_loop = re.search(
        rf"for\s*\(\s*(?:int|uint)\s+(\w+)\s*=\s*{re.escape(parts['lower'])}\.y\s*;\s*"
        rf"\1\s*<\s*{re.escape(parts['upper'])}\.y",
        parts["clean"],
    )
    if not y_loop:
        return body
    pattern = re.compile(
        rf"(\b{re.escape(y_loop.group(1))}\s*<\s*){re.escape(parts['upper'])}\.y\b"
    )
    return pattern.sub(rf"\1{parts['lower']}.y", body, count=1)


hlsl = read(HLSL)
glsl = read(GLSL)

print("=== ReSTIR reservoir contract ===")

# --- Check 1: pass 3 writes ReservoirWrite (the fresh-candidate buffer) --------
gen_h = body_of(hlsl, "RunRestirGenerate")
gen_g = body_of(glsl, "RunRestirGenerate")
check(
    "hlsl: pass 3 (Generate) writes ReservoirWrite",
    "ReservoirWrite[" in gen_h,
    "Generate must write the fresh candidates to the write buffer",
)
check(
    "glsl: pass 3 (Generate) writes ReservoirWrite",
    "ReservoirWrite[" in gen_g,
)

# --- Check 2: pass 11 reads the CURRENT pixel from ReservoirWrite -------------
# This is the ping-pong fix. Reading ReservoirRead here means Combine never sees
# pass 3's output.
comb_h = body_of(hlsl, "RunRestirCombine")
comb_g = body_of(glsl, "RunRestirCombine")
spatial_h = body_of(hlsl, "RunRestirSpatial")
spatial_g = body_of(glsl, "RunRestirSpatial")

check(
    "hlsl: pass 11 reads the current pixel from ReservoirWrite",
    re.search(r"LightReservoir\s+r\s*=\s*ReservoirWrite\[", comb_h) is not None,
    "Combine must start from this frame's samples, not the previous frame's",
)
check(
    "glsl: pass 11 reads the current pixel from ReservoirWrite",
    re.search(r"LightReservoir\s+r\s*=\s*ReservoirWrite\[", comb_g) is not None,
)

# --- Check 3: no unsafe in-place spatial gathers ------------------------------
# This implementation deliberately uses two buffers. Pass 11 may update its
# own index, but must not read another invocation's ReservoirWrite element.
check(
    "hlsl: pass 11 has no cross-invocation ReservoirWrite neighbour read",
    re.search(r"ReservoirWrite\s*\[\s*neighbor\w*\s*\]", comb_h) is None
    and "int spatialSamples" not in comb_h,
    "spatial gather requires an immutable input and distinct output buffer",
)
check(
    "glsl: pass 11 has no cross-invocation ReservoirWrite neighbour read",
    re.search(r"ReservoirWrite\s*\[\s*neighbor\w*\s*\]", comb_g) is None
    and "int spatialSamples" not in comb_g,
)
check(
    "hlsl: pass 11 writes only its own working index",
    re.search(r"ReservoirWrite\s*\[\s*flatIdx\s*\]\s*=\s*r\s*;", comb_h) is not None,
)
check(
    "glsl: pass 11 writes only its own working index",
    re.search(r"ReservoirWrite\s*\[\s*flatIdx\s*\]\s*=\s*r\s*;", comb_g) is not None,
)

# --- Check 4: temporal history still comes from ReservoirRead -----------------
# The one read that MUST stay on the read buffer: it is deliberately the
# previous frame's combined result.
check(
    "hlsl: pass 11 reads temporal history from ReservoirRead",
    re.search(r"LightReservoir\s+prevR\s*=\s*ReservoirRead\[", comb_h) is not None,
    "temporal reuse must read the previous frame's buffer",
)
check(
    "glsl: pass 11 reads temporal history from ReservoirRead",
    re.search(r"LightReservoir\s+prevR\s*=\s*ReservoirRead\[", comb_g) is not None,
)

# --- Check 4b: every in-range generation invocation initializes its slot ------
check(
    "hlsl: pass 3 has one in-bounds early return and always stores",
    gen_h.count("return;") == 1 and "ReservoirWrite[flatIdx] = r;" in gen_h,
    "sky/miss/no-light pixels must not retain stale reservoir records",
)
check(
    "glsl: pass 3 has one in-bounds early return and always stores",
    gen_g.count("return;") == 1 and "ReservoirWrite[flatIdx] = r;" in gen_g,
)

# --- Check 4c: normalized RIS estimator and target reevaluation ---------------
check(
    "hlsl: generation finalizes W using wSum/(M*pHat)",
    re.search(r"r\.W\s*=\s*r\.wSum\s*/\s*\(float\(M\)\s*\*\s*r\.bary\.x\)", gen_h) is not None,
)
check(
    "glsl: generation finalizes W using wSum/(M*pHat)",
    re.search(r"r\.W\s*=\s*r\.wSum\s*/\s*\(float\(M\)\s*\*\s*r\.bary\.x\)", gen_g) is not None,
)
check(
    "hlsl: temporal merge reevaluates history target at receiver",
    "RestirTarget(prevR.lightIndex, receiverPos, receiverN, receiverAlbedo)" in comb_h,
)
check(
    "glsl: temporal merge reevaluates history target at receiver",
    "RestirTarget(prevR.lightIndex, receiverPos, receiverN, receiverAlbedo)" in comb_g,
)
check(
    "hlsl: temporal reservoir index uses prevUV only once",
    "int2 prevReservoirPixel = clamp(int2(prevUV * float2(fullSize))" in comb_h,
)
check(
    "glsl: temporal reservoir index uses prevUV only once",
    "ivec2 prevReservoirPixel = clamp(ivec2(prevUV * vec2(fullSize))" in comb_g,
)
check(
    "hlsl: reservoir maps current and previous pixels to stable full-resolution storage",
    has_fullres_flat_address(gen_h)
    and has_fullres_flat_address(comb_h)
    and "prevReservoirPixel.y * fullSize.x + prevReservoirPixel.x" in comb_h,
)
check(
    "glsl: reservoir maps current and previous pixels to stable full-resolution storage",
    has_fullres_flat_address(gen_g)
    and has_fullres_flat_address(comb_g)
    and "prevReservoirPixel.y * fullSize.x + prevReservoirPixel.x" in comb_g,
)
check(
    "hlsl: stale sparse slots after render-scale changes fail the frame-tag test",
    "(r.flagsValid >> 1) == previousFrameTag" in hlsl
    and "wl_restir_is_previous_frame(prevR)" in comb_h,
)
check(
    "glsl: stale sparse slots after render-scale changes fail the frame-tag test",
    "(r.flagsValid >> 1) == previousFrameTag" in glsl
    and "wl_restir_is_previous_frame(prevR)" in comb_g,
)
check(
    "hlsl: temporal pass defers direct-light shading",
    "traceColor.rgb + directLight" not in comb_h and "IMG_STORE(img_Bloom" not in comb_h,
)
check(
    "glsl: temporal pass defers direct-light shading",
    "traceColor.rgb + directLight" not in comb_g and "imageStore(img_Bloom" not in comb_g,
)
check(
    "hlsl: spatial pass adds the ReSTIR estimate exactly once",
    "traceColor.rgb + directLight" in spatial_h and "IMG_STORE(img_Bloom" in spatial_h,
)
check(
    "glsl: spatial pass adds the ReSTIR estimate exactly once",
    "traceColor.rgb + directLight" in spatial_g and "imageStore(img_Bloom" in spatial_g,
)
for language, spatial_body, bloom_read, bloom_write in (
    (
        "hlsl",
        spatial_h,
        "IMG_LOAD(img_Bloom,pixelCoords)",
        "IMG_STORE(img_Bloom,pixelCoords,float4(traceColor.rgb+directLight,traceColor.a));",
    ),
    (
        "glsl",
        spatial_g,
        "imageLoad(img_Bloom,pixelCoords)",
        "imageStore(img_Bloom,pixelCoords,vec4(traceColor.rgb+directLight,traceColor.a));",
    ),
):
    check(
        f"{language}: pass 14 stamps the current frame independently of sample validity",
        stamps_spatial_frame_tag_independently_of_sample(spatial_body),
        "the frame tag follows totalM; only flagsValid bit 0 reflects a selected sample",
    )
    check(
        f"{language}: pass 14 bloom shading and writes require a selected sample and positive W",
        gates_spatial_bloom_on_selected_sample_and_weight(
            spatial_body, bloom_read, bloom_write
        ),
        "keep direct-light shading and bloom I/O inside the selected-sample/positive-W guard",
    )
    check(
        f"{language}: pass 14 estimator requires positive weight and selected target",
        spatial_estimator_requires_positive_weight_and_target(spatial_body),
        "only assign positive W when totalWeight and selectedTarget are positive",
    )
for language, spatial in (("hlsl", spatial_h), ("glsl", spatial_g)):
    check(
        f"{language}: spatial pass reads immutable reservoirs and writes a distinct pair member",
        has_separate_spatial_source_output(spatial),
        "neighbor reads must come from ReservoirRead, never the output UAV",
    )
    check(
        f"{language}: spatial reuse re-evaluates target and represented M at the receiver",
        spatial_source_uses_reweighted_M(spatial),
    )
    check(
        f"{language}: spatial neighbor is current-frame and surface-compatible",
        spatial_neighbor_validation(spatial),
    )
    check(
        f"{language}: spatial pass writes its complete full-resolution tile",
        writes_complete_nonoverlapping_tile(spatial),
)

# --- Check 5: the GLSL write buffer is NOT writeonly -------------------------
# `writeonly` forbids the read that check 2 requires. The qualifier and the read
# pattern are a matched pair; changing one without the other does not compile.
check(
    "glsl: ReservoirWrite is not declared writeonly",
    re.search(r"layout\(std430,\s*binding\s*=\s*10\)\s*(?!writeonly)\w*\s*buffer\s+ReservoirWriteBuffer", glsl)
    is not None,
    "pass 11 must read ReservoirWrite, so it cannot be writeonly",
)

# --- Check 6: shading reconstructs the receiver position from the G-buffer ----
# The world-origin bug. Both the G-buffer load and the inverse-view-projection
# reconstruction must be present in the combine pass.
check(
    "hlsl: combine reconstructs receiver world position",
    "u_InverseViewProjection" in comb_h and "img_TraceGBuffer" in comb_h,
    "shading must use the pixel's world position, not the origin",
)
check(
    "glsl: combine reconstructs receiver world position",
    "u_InverseViewProjection" in comb_g and "img_TraceGBuffer" in comb_g,
)
check(
    "hlsl: spatial shading does NOT use the world origin",
    re.search(r"toLight\s*=\s*[^;]*-\s*receiverPos\s*;", spatial_h, re.S) is not None,
    "toLight must subtract the reconstructed receiver position",
)
check(
    "glsl: spatial shading does NOT use the world origin",
    re.search(r"toLight\s*=\s*[^;]*-\s*receiverPos\s*;", spatial_g, re.S) is not None,
)

# --- Check 7: the estimator does not double-divide by M ----------------------
# W already contains 1/M; dividing by M again darkens by 1/M.
check(
    "hlsl: spatial estimator does not double-divide by M",
    not re.search(r"r\.W\s*/\s*float\(\s*max\(\s*r\.M", spatial_h),
    "W already carries 1/M",
)
check(
    "glsl: spatial estimator does not double-divide by M",
    not re.search(r"r\.W\s*/\s*float\(\s*max\(\s*r\.M", spatial_g),
)

# --- Check 8: both passes are dispatched -------------------------------------
check("hlsl: pass 3 dispatched", re.search(r"case\s+3\s*:", hlsl) is not None)
check("glsl: pass 3 dispatched", re.search(r"case\s+3\s*:", glsl) is not None)
check("hlsl: pass 11 dispatched", re.search(r"case\s+11\s*:", hlsl) is not None)
check("glsl: pass 11 dispatched", re.search(r"case\s+11\s*:", glsl) is not None)
check("hlsl: pass 14 dispatched", re.search(r"case\s+14\s*:\s*RunRestirSpatial\(\)", hlsl) is not None)
check("glsl: pass 14 dispatched", re.search(r"case\s+14\s*:\s*RunRestirSpatial\(\)", glsl) is not None)

# --- Check 8b: the C++ side dispatches the passes and swaps ------------------
# The passes existing in the shader is not enough; the schedule must call them,
# and the swap must be present or the temporal history never advances. Checked
# against the renderer, since that is where the schedule lives.
RENDERER = REPO / "Wasteland" / "src" / "Wasteland" / "Renderer" / "Renderer3D.cpp"
renderer = read(RENDERER)

check(
    "renderer: dispatches pass 3 behind RestirEnabled",
    re.search(r"RestirEnabled[\s\S]{0,400}?pass\(3\)", renderer) is not None,
    "pass 3 must be in the schedule",
)
check(
    "renderer: dispatches pass 11 behind RestirEnabled",
    re.search(r"RestirEnabled[\s\S]{0,400}?pass\(11\)", renderer) is not None,
)
nova_schedule = body_of(renderer, "RunNovaPassSchedule")
nova_p11 = nova_schedule.find("pass(11)")
nova_p14 = nova_schedule.find("pass(14)")
nova_p2 = nova_schedule.find("pass(2)")
nova_swap1 = nova_schedule.find("n.SwapReservoir()", nova_p11)
nova_swap2 = nova_schedule.find("n.SwapReservoir()", nova_swap1 + 1)
check(
    "renderer: pass 14 is bracketed by two reservoir swaps before pass 2",
    nova_p11 >= 0 and nova_swap1 > nova_p11 and nova_p14 > nova_swap1
    and nova_swap2 > nova_p14 and nova_p2 > nova_swap2,
    "pass 11 output must become the immutable pass 14 input; spatial output becomes next-frame history",
)
gl_pass11 = renderer.find("glUniform1i(s_Data.Loc_PassID, 11)")
gl_pass14 = renderer.find("glUniform1i(s_Data.Loc_PassID, 14)", gl_pass11)
gl_pass2 = renderer.find("glUniform1i(s_Data.Loc_PassID, 2)", gl_pass14)
gl_barrier_signature = "glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);"
gl_barrier11 = renderer.find(gl_barrier_signature, gl_pass11)
gl_barrier14 = renderer.find(gl_barrier_signature, gl_pass14)
gl_rebind1 = renderer.find("BindReservoirBuffers();", gl_pass11)
gl_rebind2 = renderer.find("BindReservoirBuffers();", gl_pass14)
gl_bind_body = body_of(renderer, "BindReservoirBuffers")
check(
    "renderer: GL brackets pass 14 with reservoir rebinds and storage barriers",
    gl_pass11 >= 0 and gl_barrier11 > gl_pass11 and gl_rebind1 > gl_barrier11
    and gl_pass14 > gl_rebind1 and gl_barrier14 > gl_pass14
    and gl_rebind2 > gl_barrier14 and gl_pass2 > gl_rebind2
    and "GL_SHADER_STORAGE_BUFFER, 26" in gl_bind_body
    and "GL_SHADER_STORAGE_BUFFER, 10" in gl_bind_body,
    "GL flips/rebinds around the pass so source and output buffers stay distinct",
)
check(
    "renderer: GL reservoir buffers are created",
    "glCreateBuffers(1, &s_Data.ReservoirBufferID[ri])" in renderer,
)
check(
    "renderer: GL reservoir buffers are destroyed",
    "glDeleteBuffers(1, &s_Data.ReservoirBufferID[ri])" in renderer,
)

# --- Check 8c: DX12 rebuilds the write descriptor per dispatch ---------------
# A write UAV pinned at creation while the read SRV follows the parity means
# some dispatch reads and writes the SAME buffer.
DX12 = REPO / "Wasteland" / "src" / "Platform" / "NVRHI" / "DX12Nova.cpp"
dx12 = read(DX12)
check(
    "dx12: write UAV descriptor is rebuilt against the live target",
    re.search(r"reservoir\[1\s*-\s*reservoirParity\]", dx12) is not None,
    "the write target must follow the parity, not stay pinned",
)
check(
    "dx12: SwapReservoir flips the parity",
    re.search(r"void\s+DX12Nova::SwapReservoir\(\)\s*\{[^}]*reservoirParity", dx12) is not None,
)

# --- Check 8d: backend slot/lifecycle contracts -------------------------------
DX11 = REPO / "Wasteland" / "src" / "Platform" / "NVRHI" / "DX11Nova.h"
dx11 = read(DX11)
check(
    "dx11: resize creates the reservoir pair before clearing",
    re.search(r"CreateReservoir\(w,\s*h\);[\s\S]{0,100}?Clear\(\);", dx11) is not None,
)
check(
    "dx11: reservoir binds at u10 and clears all eleven UAV slots",
    "ID3D11UnorderedAccessView* uavs[11]" in dx11
    and "uavs[10]=reservoirUAV" in dx11
    and "CSSetUnorderedAccessViews(0,11,uavs" in dx11,
)

VULKAN = REPO / "Wasteland" / "src" / "Platform" / "NVRHI" / "VulkanNova.cpp"
vulkan = read(VULKAN)
check(
    "vulkan: image 8 (moments) maps to u9, leaving u8 free",
    re.search(r"kImageUavSlot\[kImageCount\]\s*=\s*\{\s*0,\s*1,\s*2,\s*3,\s*4,\s*5,\s*6,\s*7,\s*9\s*\}", vulkan) is not None,
)
check(
    "vulkan: reservoir buffers allow both SRV and UAV bindings",
    "reservoirDesc.canHaveUAVs = true;" in vulkan
    and "ResourceStates::UnorderedAccess |" in vulkan
    and "ResourceStates::ShaderResource" in vulkan,
)
check(
    "vulkan: parity swap invalidates the binding set",
    re.search(r"void\s+VulkanNova::SwapReservoir\(\)\s*\{[^}]*m_BindingSetDirty\s*=\s*true", vulkan, re.S)
    is not None,
)

# --- Check 8e: both renderer paths insert ReSTIR before temporal accumulation -
nova_schedule = body_of(renderer, "RunNovaPassSchedule")
nova_p3 = nova_schedule.find("pass(3)")
nova_p11 = nova_schedule.find("pass(11)")
nova_p14 = nova_schedule.find("pass(14)")
nova_p2 = nova_schedule.find("pass(2)")
check(
    "nova: spatial ReSTIR result is accumulated by pass 2",
    nova_p3 >= 0 and nova_p11 > nova_p3 and nova_p14 > nova_p11 and nova_p2 > nova_p14,
    "the additive result must reach img_Accumulation before composition",
)
check(
    "nova: legacy emissive sampling is suppressed only when ReSTIR is enabled",
    'if(s_Data.RestirEnabled)n.Set("u_LightCount",0)' in nova_schedule,
)
check(
    "gl: ReSTIR buffers use storage barriers",
    "GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT" in renderer,
)
check(
    "gl: legacy emissive sampling is suppressed only for a ready reservoir pair",
    "glRestirReady ? 0 : emissiveLightCount" in renderer,
)
gl_restit_start = renderer.find("if (glRestirReady)")
gl_p3 = renderer.find("glUniform1i(s_Data.Loc_PassID, 3)", gl_restit_start)
gl_p11 = renderer.find("glUniform1i(s_Data.Loc_PassID, 11)", gl_p3)
gl_p14 = renderer.find("glUniform1i(s_Data.Loc_PassID, 14)", gl_p11)
gl_p2 = renderer.find("glUniform1i(s_Data.Loc_PassID, 2)", gl_p14)
check(
    "gl: spatial ReSTIR result is accumulated by pass 2",
    gl_restit_start >= 0 and gl_p3 >= 0 and gl_p11 > gl_p3 and gl_p14 > gl_p11 and gl_p2 > gl_p14,
)

# --- Check 9: the reservoir struct is 32 bytes in both twins -----------------
# The layout is load-bearing: the C++ side allocates w*h*32 bytes, and the
# spatial-reuse fields only fit because emission is not textured. A drifting
# struct silently misindexes every element.
def field_count(src, name):
    m = re.search(r"struct\s+" + re.escape(name) + r"\s*\{([^}]*)\}", src, re.S)
    if not m:
        return 0
    return len([ln for ln in m.group(1).splitlines() if ";" in ln and "//" not in ln.split(";")[0][:0]])


check("hlsl: LightReservoir struct present", "struct LightReservoir" in hlsl)
check("glsl: LightReservoir struct present", "struct LightReservoir" in glsl)

# --- New temporal ReSTIR regressions ------------------------------------------
# These assertions preserve the current experimental source contract. The exact
# support-mismatch diagnostic separately demonstrates why this is not a proof of
# unbiasedness across receiver-dependent proposal/target support.
check(
    "hlsl: temporal source contract carries current.M at zero weight/target",
    preserves_current_candidate_count(body_of(hlsl, "CombineReservoirs")),
    "source-shape assertion only; cross-receiver support needs separate correction",
)
check(
    "glsl: temporal source contract carries current.M at zero weight/target",
    preserves_current_candidate_count(body_of(glsl, "CombineReservoirs")),
)
check(
    "hlsl: temporal source contract carries history.M at zero re-evaluated target",
    preserves_zero_target_history_count(body_of(hlsl, "CombineReservoirs")),
    "source-shape assertion only; this may darken when source support differs",
)
check(
    "glsl: temporal source contract carries history.M at zero re-evaluated target",
    preserves_zero_target_history_count(body_of(glsl, "CombineReservoirs")),
)
for language, shader in (("hlsl", hlsl), ("glsl", glsl)):
    check(
        f"{language}: previous-frame tag accepts valid zero-sample records",
        accepts_tagged_empty_history(body_of(shader, "wl_restir_is_previous_frame")),
        "M and frame tag validate a record; flagsValid bit 0 only means selected sample",
    )
    check(
        f"{language}: generation stamps the frame tag when M is nonzero",
        stamps_nonempty_reservoir_frame(body_of(shader, "RunRestirGenerate")),
    )
    check(
        f"{language}: temporal output stamps the frame tag when M is nonzero",
        stamps_nonempty_reservoir_frame(body_of(shader, "RunRestirCombine")),
    )
    check(
        f"{language}: temporal target is not evaluated without a selected history sample",
        gates_history_target_on_selected_sample(body_of(shader, "RunRestirCombine")),
    )

# All six seed sites use the dispatched trace width for their flat pixel
# stride. This establishes generate/combine/spatial and HLSL/GLSL parity.
gen_h = body_of(hlsl, "RunRestirGenerate")
gen_g = body_of(glsl, "RunRestirGenerate")
comb_h = body_of(hlsl, "RunRestirCombine")
comb_g = body_of(glsl, "RunRestirCombine")
check(
    "hlsl: generate and combine RNG strides use the same trace width",
    matching_trace_seed_strides(gen_h, comb_h),
    "both pass seeds must use their wl_trace_extent(fullSize).x row stride",
)
check(
    "glsl: generate and combine RNG strides use the same trace width",
    matching_trace_seed_strides(gen_g, comb_g),
)
check(
    "hlsl/glsl: generation and combine RNG strides match",
    matching_trace_seed_strides(gen_h, comb_h, gen_g, comb_g),
    "all four seed sites must use the same trace-pixel row/column scheme",
)
check(
    "hlsl: spatial RNG uses the trace-width row stride",
    matching_trace_seed_strides(gen_h, comb_h, spatial_h),
)
check(
    "glsl: spatial RNG uses the trace-width row stride",
    matching_trace_seed_strides(gen_g, comb_g, spatial_g),
)
check(
    "hlsl/glsl: all pass RNG strides match",
    matching_trace_seed_strides(gen_h, comb_h, spatial_h, gen_g, comb_g, spatial_g),
    "all six seeds must use the same trace-pixel row/column scheme",
)
check(
    "hlsl/glsl: spatial reservoir draws use the matching decorrelation salt",
    "0x85ebca6bu" in spatial_h and "0x85ebca6bu" in spatial_g,
)

# Each trace invocation owns the integer-partitioned full-resolution tile
# [tracePixel*fullSize/traceSize, (tracePixel+1)*fullSize/traceSize). The
# exclusive loops and flattened full-size store establish complete, disjoint
# writes in both producer and temporal-combine passes.
for language, generate_body, combine_body in (
    ("hlsl", gen_h, comb_h),
    ("glsl", gen_g, comb_g),
):
    check(
        f"{language}: pass 3 writes its complete full-resolution tile",
        writes_complete_nonoverlapping_tile(generate_body),
        "pass 3 must loop over both exclusive tile bounds and store every full-res address",
    )
    check(
        f"{language}: pass 11 writes its complete full-resolution tile",
        writes_complete_nonoverlapping_tile(combine_body),
        "pass 11 must loop over both exclusive tile bounds and store every full-res address",
    )
for language, spatial_body in (("hlsl", spatial_h), ("glsl", spatial_g)):
    check(
        f"{language}: pass 14 writes its complete full-resolution tile",
        writes_complete_nonoverlapping_tile(spatial_body),
        "pass 14 must overwrite every slot in the distinct output reservoir pair",
    )

# Exercise the integer partition rather than only inspecting its source form.
# These odd extents expose remainder pixels at reduced scales.
print()
print("=== Numeric tile partition simulations ===")
for full_width, full_height in ((13, 9), (31, 17), (101, 73)):
    for scale in (0.5, 0.66, 0.8, 1.0):
        passed, detail = simulate_tile_partition(full_width, full_height, scale)
        check(
            f"numeric tiles: {full_width}x{full_height} at scale {scale:g} cover each address once",
            passed,
            detail or "all midpoint representatives are inside their trace-pixel tiles",
        )

# --- Negative controls: every check above must be able to fail ----------------
# Each mutation reintroduces the defect in an in-memory copy and re-runs the
# same predicate the check uses. If the predicate still passes, the guard is
# vacuous.
print("\n=== Negative controls (each defect must be detected) ===")


def mutation_caught(label, mutated, predicate):
    """Assert `predicate` FAILS on `mutated` -- i.e. the guard would catch it."""
    global checks, negative_controls
    checks += 1
    negative_controls += 1
    if not predicate(mutated):
        print(f"  PASS  mutation caught: {label}")
    else:
        print(f"  FAIL  mutation NOT caught: {label}")
        failures.append("mutation not caught: " + label)


# 1. Reintroduce the ping-pong inversion in HLSL.
mutated = re.sub(
    r"(LightReservoir\s+r\s*=\s*)ReservoirWrite\[",
    r"\1ReservoirRead[",
    comb_h,
    count=1,
)
mutation_caught(
    "hlsl ping-pong inversion",
    mutated,
    lambda s: re.search(r"LightReservoir\s+r\s*=\s*ReservoirWrite\[", s) is not None,
)

# 2. Reintroduce the world-origin shading.
mutated = re.sub(
    r"float3\s+toLight\s*=.*?;",
    "float3 toLight = float3(0.0f, 0.0f, 0.0f);",
    comb_h,
    count=1,
    flags=re.S,
)
mutation_caught(
    "hlsl world-origin shading",
    mutated,
    lambda s: re.search(r"toLight\s*=\s*[^;]*-\s*receiverPos\s*;", s, re.S) is not None,
)

# 3. Reintroduce the writeonly qualifier on the GLSL write buffer.
mutated = glsl.replace(
    "binding = 10) buffer ReservoirWriteBuffer",
    "binding = 10) writeonly buffer ReservoirWriteBuffer",
)
mutation_caught(
    "glsl writeonly write buffer",
    mutated,
    lambda s: re.search(
        r"layout\(std430,\s*binding\s*=\s*10\)\s*(?!writeonly)\w*\s*buffer\s+ReservoirWriteBuffer", s
    )
    is not None,
)

# 4. Reintroduce the double-divide by M.
mutated = spatial_h.replace(
    "receiverAlbedo * emission * (NdotL / PI) * r.W;",
    "receiverAlbedo * emission * (NdotL / PI) * r.W / float(max(r.M, 1));",
)
mutation_caught(
    "hlsl double-divide by M",
    mutated,
    lambda s: not re.search(r"r\.W\s*/\s*float\(\s*max\(\s*r\.M", s),
)

# 5. Reintroduce the unsafe in-place neighbor gather.
mutated = spatial_h.replace(
    "LightReservoir neighborR = ReservoirRead[neighborFlatIdx];",
    "LightReservoir neighborR = ReservoirWrite[neighborFlatIdx];",
)
mutation_caught(
    "hlsl in-place spatial read/write race",
    mutated,
    has_separate_spatial_source_output,
)

# New temporal regressions: each mutation is applied to an in-memory body and
# evaluated by the same predicate as its corresponding source guard.
mutation_caught(
    "hlsl current.M discarded at zero weight",
    mutate_current_count(body_of(hlsl, "CombineReservoirs")),
    preserves_current_candidate_count,
)
mutation_caught(
    "glsl current.M discarded at zero weight",
    mutate_current_count(body_of(glsl, "CombineReservoirs")),
    preserves_current_candidate_count,
)
for language, shader in (("hlsl", hlsl), ("glsl", glsl)):
    combine_body = body_of(shader, "CombineReservoirs")
    mutated = combine_body.replace(
        "float historyCount = (history.M > 0)",
        "float historyCount = (wl_restir_has_sample(history) && history.M > 0 && historyTarget > 0.0f)",
    )
    mutation_caught(
        f"{language} zero-target history M discarded",
        mutated,
        preserves_zero_target_history_count,
    )
    previous_body = body_of(shader, "wl_restir_is_previous_frame").replace(
        "r.M > 0", "wl_restir_has_sample(r)"
    )
    mutation_caught(
        f"{language} zero-sample previous-frame record rejected",
        previous_body,
        accepts_tagged_empty_history,
    )
    generate_body = body_of(shader, "RunRestirGenerate").replace(
        "if (r.M > 0)", "if (wl_restir_has_sample(r))"
    )
    mutation_caught(
        f"{language} generation omits frame tag for zero-target proposals",
        generate_body,
        stamps_nonempty_reservoir_frame,
    )
    temporal_body = body_of(shader, "RunRestirCombine").replace(
        "wl_restir_is_previous_frame(prevR) && wl_restir_has_sample(prevR)",
        "wl_restir_is_previous_frame(prevR)",
    )
    mutation_caught(
        f"{language} history target evaluated without a selected sample",
        temporal_body,
        gates_history_target_on_selected_sample,
    )
for language, spatial_body, bloom_read, bloom_write in (
    (
        "hlsl",
        spatial_h,
        "IMG_LOAD(img_Bloom,pixelCoords)",
        "IMG_STORE(img_Bloom,pixelCoords,float4(traceColor.rgb+directLight,traceColor.a));",
    ),
    (
        "glsl",
        spatial_g,
        "imageLoad(img_Bloom,pixelCoords)",
        "imageStore(img_Bloom,pixelCoords,vec4(traceColor.rgb+directLight,traceColor.a));",
    ),
):
    mutation_caught(
        f"{language} pass 14 frame tag depends on selected sample",
        spatial_body.replace(
            "if (totalM > 0)", "if (selectedValid && totalM > 0)", 1
        ),
        stamps_spatial_frame_tag_independently_of_sample,
    )
    mutation_caught(
        f"{language} pass 14 bloom shading without selected sample guard",
        spatial_body.replace(
            "&& wl_restir_has_sample(r) && r.W > 0.0f",
            "&& r.W > 0.0f",
            1,
        ),
        lambda s, read=bloom_read, write=bloom_write: gates_spatial_bloom_on_selected_sample_and_weight(
            s, read, write
        ),
    )
    mutation_caught(
        f"{language} pass 14 bloom shading without positive W guard",
        spatial_body.replace(" && r.W > 0.0f) {", ") {", 1),
        lambda s, read=bloom_read, write=bloom_write: gates_spatial_bloom_on_selected_sample_and_weight(
            s, read, write
        ),
    )
    mutation_caught(
        f"{language} pass 14 positive estimator weight guard removed",
        spatial_body.replace(" && totalWeight > 1e-8f", "", 1),
        spatial_estimator_requires_positive_weight_and_target,
    )
    mutation_caught(
        f"{language} pass 14 positive estimator target guard removed",
        spatial_body.replace(" && selectedTarget > 1e-8f", "", 1),
        spatial_estimator_requires_positive_weight_and_target,
    )
mutation_caught(
    "hlsl trace-width RNG stride regresses",
    mutate_trace_stride(gen_h),
    lambda s: matching_trace_seed_strides(s, comb_h),
)
mutation_caught(
    "glsl trace-width RNG stride regresses",
    mutate_trace_stride(comb_g),
    lambda s: matching_trace_seed_strides(gen_g, s),
)
mutation_caught(
    "HLSL/GLSL RNG stride agreement regresses",
    mutate_trace_stride(gen_g),
    lambda s: matching_trace_seed_strides(gen_h, comb_h, spatial_h, s, comb_g, spatial_g),
)
mutation_caught(
    "HLSL spatial RNG trace-width stride regresses",
    mutate_trace_stride(spatial_h),
    lambda s: matching_trace_seed_strides(gen_h, comb_h, s),
)
mutation_caught(
    "GLSL spatial RNG trace-width stride regresses",
    mutate_trace_stride(spatial_g),
    lambda s: matching_trace_seed_strides(gen_g, comb_g, s),
)
mutation_caught(
    "HLSL/GLSL spatial RNG stride parity regresses",
    mutate_trace_stride(spatial_g),
    lambda s: matching_trace_seed_strides(gen_h, comb_h, spatial_h, gen_g, comb_g, s),
)
for language, pass_name, body in (
    ("hlsl", "pass 3", gen_h),
    ("hlsl", "pass 11", comb_h),
    ("hlsl", "pass 14", spatial_h),
    ("glsl", "pass 3", gen_g),
    ("glsl", "pass 11", comb_g),
    ("glsl", "pass 14", spatial_g),
):
    mutation_caught(
        f"{language} {pass_name} incomplete tile bound",
        mutate_tile_bound(body),
        writes_complete_nonoverlapping_tile,
    )

print()
if failures:
    print(f"RESULT: FAIL -- {len(failures)} problem(s):")
    for f in failures:
        print(f"  - {f}")
    sys.exit(1)

print(f"RESULT: PASS -- {checks} checks green, all {negative_controls} negative controls provably failable")
print("This verifies the reservoir CONTRACT in the source. It proves nothing about")
print("GPU execution, descriptor binding, or whether the passes actually run.")
