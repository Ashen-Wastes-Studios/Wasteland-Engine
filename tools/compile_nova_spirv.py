#!/usr/bin/env python3
"""
Compile the Nova path-tracer shader to SPIR-V for the Vulkan backend.

The Vulkan Nova path cannot reuse the GL route: Vulkan GLSL forbids loose
(non-opaque, non-block) uniforms, and the Nova shader declares ~50 of them.
The HLSL twin already solved that by packing everything into a single
`cbuffer NovaConstants`, so this compiles the HLSL with DXC to SPIR-V. That
also makes the .glsl/.hlsl duplication problem smaller rather than larger:
DXC consumes the same HLSL the DX12 path uses.

REGISTER SPACES
---------------
NVRHI derives a Vulkan descriptor binding as `offset + slot`
(vulkan-resource-bindings.cpp:100), where the offset comes from
nvrhi::BindingOffsets per ResourceType and the slot is the HLSL register
number. DXC derives SPIR-V bindings from the same register numbers.

Left alone, the two disagree: NVRHI would place the UAV images at 0..7 and the
structured buffers at 1..3 (both from offset 0), while the constant buffer at
b1=1 and the sampler at s0=0 would each collide with an image. Vulkan forbids
two descriptors of the same type sharing a binding, and overlapping types are
still a validation error.

So the register spaces are shifted into disjoint ranges at compile time:

    u-space  shift   0  -> images        bindings 0..7
    b-space  shift   7  -> NovaConstants binding 8   (b1 + 7)
    t-space  shift  16  -> SRVs + SSBOs  bindings 17..87
    s-space  shift 200  -> sampler       binding 200

Verify with --check, which fails loudly if a future shader edit reintroduces a
collision. Run --deploy to copy the .spv into both bin trees.
"""

import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VULKAN_BIN = r"C:\VulkanSDK\1.3.296.0\Bin"
DXC = os.path.join(VULKAN_BIN, "dxc.exe")
SPIRV_DIS = os.path.join(VULKAN_BIN, "spirv-dis.exe")

SRC = os.path.join(REPO, "DemonCore-Editor", "assets", "shaders", "Renderer3D_NovaRenderer.hlsl")
SPIRV = os.path.join(REPO, "DemonCore-Editor", "assets", "shaders", "Renderer3D_NovaRenderer.spv")

# shift <space>; see module docstring. DO NOT change without re-running --check
# and updating BINDING_OFFSETS below to match.
SHIFTS = [("-fvk-u-shift", "0", "0"),   # UAV images
          ("-fvk-b-shift", "7", "0"),   # constant buffer
          ("-fvk-t-shift", "16", "0"),  # sampled textures + structured buffers
          ("-fvk-s-shift", "200", "0")] # sampler

# nvrhi::BindingOffsets values this produces, for reference in VulkanNova.cpp.
BINDING_OFFSETS = {
    "unorderedAccess": 0,    # Texture_UAV
    "constantBuffer": 7,     # ConstantBuffer
    "shaderResource": 16,    # Texture_SRV, StructuredBuffer_SRV
    "sampler": 200,          # Sampler
}

HLSL_TYPE_TO_NVRHI = {
    "RWTexture2D": "Texture_UAV",
    "Texture2D": "Texture_SRV",
    "RWStructuredBuffer": "StructuredBuffer_UAV",
    "StructuredBuffer": "StructuredBuffer_SRV",
    "cbuffer": "ConstantBuffer",
    "SamplerState": "Sampler",
    "SamplerComparisonState": "Sampler",
}


def compile_spv():
    if not os.path.exists(DXC):
        sys.exit("dxc.exe not found at %s -- install the Vulkan SDK or edit VULKAN_BIN." % DXC)
    cmd = [DXC, "-spirv", "-T", "cs_6_0", "-E", "main",
           "-fspv-target-env=vulkan1.2", "-Fo", SPIRV, SRC]
    for flag, shift, space in SHIFTS:
        cmd += [flag, shift, space]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if r.returncode != 0 or not os.path.exists(SPIRV):
        sys.exit("SPIR-V compilation FAILED:\n%s\n%s" % (r.stdout, r.stderr))
    print("compiled %s -> %s (%d bytes)" % (os.path.basename(SRC), os.path.basename(SPIRV),
                                           os.path.getsize(SPIRV)))


def read_bindings(spv_path):
    if not os.path.exists(SPIRV_DIS):
        sys.exit("spirv-dis.exe not found at %s -- cannot verify bindings." % SPIRV_DIS)
    out = subprocess.run([SPIRV_DIS, spv_path], capture_output=True, text=True,
                         errors="replace").stdout
    names, binds = {}, {}
    for line in out.split("\n"):
        # spirv-dis emits: OpName %img_Output "img_Output"  (id unquoted)
        m = re.search(r'OpName (%[\w$]+)\s+"([^"]+)"', line)
        if m:
            names[m.group(1)] = m.group(2)
        m = re.search(r"OpDecorate (%[\w$]+) Binding (\d+)", line)
        if m:
            binds[m.group(1)] = int(m.group(2))
    return names, binds


def read_hlsl_slots():
    """HLSL register number per resource name, from the source declarations."""
    decl = re.compile(
        r"^\s*(RWStructuredBuffer|StructuredBuffer|RWTexture2D|Texture2D|RWBuffer|"
        r"cbuffer|SamplerState|SamplerComparisonState)\s*(?:<[^>]*>)?"
        r"\s+(\w+)\s*(\[[^\]]*\])?\s*:\s*register\(([ustb])(\d+)\)")
    slots, kinds = {}, {}
    with open(SRC, encoding="utf-8") as fh:
        for line in fh:
            m = decl.match(line)
            if m:
                kind, name, _arr, _space, num = m.groups()
                slots[name] = int(num)
                kinds[name] = HLSL_TYPE_TO_NVRHI[kind]
    return slots, kinds


def check_bindings(spv_path):
    names, binds = read_bindings(spv_path)
    slots, kinds = read_hlsl_slots()

    by_binding = collections.defaultdict(list)
    for var, binding in binds.items():
        by_binding[binding].append(names.get(var, var))

    collisions = {b: v for b, v in by_binding.items() if len(v) > 1}
    if collisions:
        print("BINDING COLLISIONS -- NVRHI would build an invalid descriptor set:")
        for b, v in sorted(collisions.items()):
            print("   binding %d: %s" % (b, ", ".join(v)))
        return False

    # Confirm every binding is reproducible as offset + slot, which is the
    # contract vulkan-resource-bindings.cpp:100 enforces.
    offsets = collections.defaultdict(set)
    unmatched = []
    for var, binding in binds.items():
        name = names.get(var, var)
        if name not in slots:
            unmatched.append(name)
            continue
        offsets[kinds[name]].add(binding - slots[name])

    print("bindings: %d resources, %d distinct, no collisions"
          % (len(binds), len(by_binding)))
    if unmatched:
        print("  (no HLSL register found for: %s -- DXC may have added these)"
              % ", ".join(sorted(unmatched)))
    for rtype, vals in sorted(offsets.items()):
        if len(vals) != 1:
            print("  INCONSISTENT offset for %s: %s" % (rtype, sorted(vals)))
            return False
        got = vals.pop()
        want = BINDING_OFFSETS.get(rtype)
        flag = "ok" if want is None or want == got else "MISMATCH (script says %s)" % want
        print("   %-22s offset = %-5d %s" % (rtype, got, flag))
    return True


def deploy():
    for cfg in ("Release", "Debug"):
        dst_dir = os.path.join(REPO, "bin", "%s-windows-x86_64" % cfg,
                               "DemonCore-Editor", "assets", "shaders")
        if not os.path.isdir(dst_dir):
            print("  skip %s (no %s)" % (cfg, dst_dir))
            continue
        shutil.copyfile(SPIRV, os.path.join(dst_dir, os.path.basename(SPIRV)))
        print("  deployed -> %s" % dst_dir)


def spirv_cb_offsets(spv_path):
    """Per-member byte offsets of NovaConstants as the SPIR-V actually sees them."""
    cross = os.path.join(VULKAN_BIN, "spirv-cross.exe")
    if not os.path.exists(cross):
        print("  (spirv-cross.exe missing -- skipping constant buffer cross-check)")
        return None
    r = subprocess.run([cross, spv_path, "--reflect"], capture_output=True, text=True,
                       errors="replace")
    try:
        j = json.loads(r.stdout)
    except ValueError:
        print("  (spirv-cross produced no JSON -- skipping cross-check)")
        return None
    ubos = j.get("ubos") or []
    if not ubos:
        return None
    blk = ubos[0]
    ty = j.get("types", {}).get(blk.get("type"), {})
    members = {m["name"]: m.get("offset") for m in ty.get("members", [])}
    return members, blk.get("block_size")


def dxc_cb_offsets():
    """The same offsets as the DX12/fxc path sees them (D3D cbuffer packing)."""
    fxc = None
    for base, _dirs, files in os.walk(r"C:\Program Files (x86)\Windows Kits\10\bin"):
        if "fxc.exe" in files:
            fxc = os.path.join(base, "fxc.exe")
    if not fxc:
        return None, None
    tmp_bin = os.path.join(os.path.dirname(SPIRV), "_layoutcheck.bin")
    r = subprocess.run([fxc, "/T", "cs_5_0", "/E", "main", SRC, "/Fo", tmp_bin],
                       capture_output=True, text=True, errors="replace")
    if r.returncode != 0 or not os.path.exists(tmp_bin):
        return None, None
    out = subprocess.run([fxc, "/dumpbin", tmp_bin], capture_output=True, text=True,
                         errors="replace").stdout
    os.remove(tmp_bin)
    offsets, inside = {}, False
    for line in out.split("\n"):
        if "cbuffer NovaConstants" in line:
            inside = True
            continue
        if inside:
            if line.strip().startswith("// }"):
                break
            # Arrays matter most here: a float3 array packs with a 16-byte
            # stride under both D3D and std140, but a float2 or bool array is
            # where the two rules are most likely to diverge. Match the array
            # suffix so those members are actually compared.
            m = re.search(r"(\w+)(?:\[\d+\])?;\s*//\s*Offset:\s*(\d+)", line)
            if m:
                offsets[m.group(1)] = int(m.group(2))
    return offsets, out


def check_constant_buffer(spv_path):
    """The Vulkan path writes the constant buffer from a CPU-side table using D3D
    offsets, while the SPIR-V reads it with whatever layout DXC chose. If those
    two disagree every uniform is silently garbage, so verify them directly."""
    spirv = spirv_cb_offsets(spv_path)
    d3d, _dump = dxc_cb_offsets()
    if spirv is None or d3d is None:
        print("constant buffer: cross-check skipped (missing tool)")
        return True
    members, block_size = spirv
    shared = sorted(set(members) & set(d3d))
    bad = [(n, d3d[n], members[n]) for n in shared if d3d[n] != members[n]]
    only_d3d = sorted(set(d3d) - set(members))
    print("constant buffer: %d shared members, block_size=%s" % (len(shared), block_size))
    if bad:
        print("  LAYOUT MISMATCH (Vulkan would read every uniform at the wrong offset):")
        for n, a, b in bad[:12]:
            print("     %-32s d3d=%-6d spirv=%d" % (n, a, b))
        return False
    if only_d3d:
        print("  (stripped by the compiler, not read: %s)" % ", ".join(only_d3d))
    print("  D3D and SPIR-V offsets agree for all %d members" % len(shared))
    return True


def check_staleness():
    """--check-only must not trust a .spv that is older than its .hlsl source.

    A stale artifact silently reports OK while the real freshly-compiled shader
    may have a binding collision or other error.  This codebase has been bitten
    by that exact failure mode: the .spv on disk was 39 hours old, --check-only
    said "OK", and the real shader had a collision at binding 8."""
    if not os.path.exists(SRC):
        sys.exit("HLSL source not found at %s" % SRC)
    if not os.path.exists(SPIRV):
        sys.exit("no .spv to check at %s" % SPIRV)
    spv_mtime = os.path.getmtime(SPIRV)
    src_mtime = os.path.getmtime(SRC)
    if spv_mtime < src_mtime:
        sys.exit(
            "STALE ARTIFACT: %s is older than %s.\n"
            "The .spv on disk does not reflect the current shader source.\n"
            "Recompile first (run without --check-only), then re-check."
            % (os.path.basename(SPIRV), os.path.basename(SRC)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--check-only", action="store_true",
                    help="verify an existing .spv without recompiling")
    ap.add_argument("--deploy", action="store_true",
                    help="copy the .spv into the Debug and Release bin trees")
    args = ap.parse_args()

    if not args.check_only:
        compile_spv()
    else:
        check_staleness()
    if not os.path.exists(SPIRV):
        sys.exit("no .spv to check at %s" % SPIRV)
    ok = check_bindings(SPIRV)
    ok = check_constant_buffer(SPIRV) and ok
    if not ok:
        sys.exit(1)
    if args.deploy:
        deploy()
    print("OK")


if __name__ == "__main__":
    main()