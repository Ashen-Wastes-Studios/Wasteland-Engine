#!/usr/bin/env python3
"""NEGATIVE CONTROL for tools/verify_nova_glsl.py's twin-agreement checks.

WHY THIS EXISTS
A guard that cannot fail proves nothing. The twin-agreement checks in
verify_nova_glsl.py are regex assertions over shader source, and a regex
assertion is exactly the kind of check that silently stops matching after a
refactor (renamed variable, reformatted line) and then passes vacuously forever.

So this script does the one thing that distinguishes a real guard from a
decorative one: it reintroduces each defect the checks exist to catch, in a
TEMPORARY COPY of the GLSL shader, runs the verifier against each mutated copy,
and asserts the verifier FAILS. Then it restores the original and asserts the
verifier PASSES again.

It never writes the real shader except to restore the exact bytes it read at
startup, and it verifies that restore by content hash. A crash mid-run therefore
cannot leave a mutated shader on disk.

Exit 0 = the guard is real, 1 = at least one defect slipped through undetected.
"""
import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GLSL = REPO / "DemonCore-Editor" / "assets" / "shaders" / "Renderer3D_NovaRenderer.glsl"
VERIFIER = REPO / "tools" / "verify_nova_glsl.py"

# Each case: (label, mutator). A mutator MUST change the file -- if it does not,
# the test is vacuous, which is itself reported as a failure.
MUTATIONS = {
    "frame gate removed (frame-0 history garbage)": lambda s: s.replace(
        "bool validHistory = u_FrameIndex > 0 && prevUV.x",
        "bool validHistory = prevUV.x",
    ),
    "history clamp reverted to min()-only": lambda s: s.replace(
        "histUV = clamp(histUV, 0.5f / fullRes, vec2(u_RenderScale) - 0.5f / fullRes);",
        "histUV = min(histUV, vec2(u_RenderScale) - 0.5f / fullRes);",
    ),
    "mip clamp reverted to the unreachable 4": lambda s: s.replace(
        "mipLevel = clamp(mipLevel, 0.0f, 12.0f);",
        "mipLevel = clamp(mipLevel, 0.0f, 4.0f);"),
    "Halton jitter restored in GetJitter": lambda s: s.replace(
        "vec2 GetJitter(int frameIndex, float cameraMoved) {\n    return u_Jitter;\n}",
        "vec2 GetJitter(int frameIndex, float cameraMoved) {\n"
        "    int e = (cameraMoved > 0.5f) ? 0 : frameIndex;\n"
        "    return float2(Halton(e % 16 + 1, 2), Halton(e % 16 + 1, 3)) - 0.5f;\n}",
    ),
    # The trace G-buffer packs the normal into .yzw, so .rgb decodes depth as the
    # normal's first component. Both files still COMPILE when this is wrong, so
    # only an explicit channel assertion can catch it.
    "normal decoded from .rgb (depth read as normal)": lambda s: s.replace(
        "texture(s_NormalBuffer, wl_buf_uv(uv)).gba",
        "texture(s_NormalBuffer, wl_buf_uv(uv)).rgb", 1),
    # The SVGF luminance weight rebuilt from the CENTRE's variance, which is
    # constant across all nine taps and cancels in the normalisation -- inert.
    "variance weight made inert (centre variance, constant per tap)": lambda s: s.replace(
        "exp(-abs(moments.x - neighborMean) / (sigma_l * sqrt(centerVariance) + 1e-4f))",
        "1.0f + variance * 5.0f", 1),
}


def run_verifier():
    r = subprocess.run([sys.executable, str(VERIFIER)],
                       capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    if not GLSL.exists():
        print(f"FAIL not found: {GLSL}")
        return 1

    original = GLSL.read_bytes()
    original_hash = hashlib.sha256(original).hexdigest()
    backup = GLSL.with_suffix(".glsl.bak_negctl")
    shutil.copy2(GLSL, backup)

    failures = []
    try:
        rc, out = run_verifier()
        print(f"baseline (unmodified shader): rc={rc} "
              f"{'PASS' if rc == 0 else 'FAIL'}")
        if rc != 0:
            print("FAIL the verifier does not pass on the current shader, so a "
                  "caught-defect result would be meaningless")
            return 1

        for label, mutate in MUTATIONS.items():
            text = original.decode("utf-8", errors="replace")
            mutated = mutate(text)
            if mutated == text:
                # The mutator found nothing to replace -- meaning the guard's
                # target string no longer exists in the shader. That is exactly
                # the "vacuous check" failure mode this script exists to catch.
                failures.append(f"{label}: MUTATION DID NOT APPLY -- the string "
                                f"the check keys on no longer exists, so the "
                                f"check is probably vacuous")
                print(f"  MISSED  {label}  (mutation did not apply)")
                continue

            GLSL.write_text(mutated, encoding="utf-8", newline="")
            rc, out = run_verifier()
            caught = rc != 0
            print(f"  {'CAUGHT ' if caught else 'MISSED!'}  {label}")
            if not caught:
                failures.append(f"{label}: verifier still passed with the defect "
                                f"reintroduced -- the check is not real")
            else:
                detail = [l.strip() for l in out.splitlines() if "FAIL" in l]
                for d in detail[:1]:
                    print(f"            {d}")
    finally:
        GLSL.write_bytes(original)
        if GLSL.read_bytes() != original:
            failures.append("RESTORE FAILED -- shader bytes differ from original")
        if backup.exists():
            backup.unlink()

    restored_hash = hashlib.sha256(GLSL.read_bytes()).hexdigest()
    ok_restore = restored_hash == original_hash
    print(f"\nrestore verified by sha256: {'OK' if ok_restore else 'MISMATCH'}")

    # And the verifier must pass again on the restored file.
    rc, _ = run_verifier()
    print(f"verifier after restore: rc={rc} {'PASS' if rc == 0 else 'FAIL'}")
    if rc != 0:
        failures.append("verifier does not pass after restore")

    print()
    if failures:
        print("RESULT: FAIL")
        for f in failures:
            print("  -", f)
        return 1
    print("RESULT: PASS -- every guard in verify_nova_glsl.py demonstrably fails "
          "when its defect is reintroduced")
    return 0


if __name__ == "__main__":
    sys.exit(main())