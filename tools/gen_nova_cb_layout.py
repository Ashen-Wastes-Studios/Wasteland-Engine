#!/usr/bin/env python3
"""Generate the Nova constant-buffer layout header from a compiled DXIL blob.

WHY THIS EXISTS
---------------
`DX12Nova::Init()` used `D3DReflect` to learn the byte offsets of every member of
the `NovaConstants` cbuffer. `D3DReflect` is the **DXBC (shader model 5.x)**
reflection entry point. Slang compiles the Nova shader to shader model 6.0, which
lands in the bytecode as **DXIL**, so `D3DReflect` returns `E_INVALIDARG`
(0x80070057) and DX12 Nova threw before creating a pipeline state. The editor then
silently fell back to the raster path.

Rather than add a runtime dependency on `dxcompiler.dll` (and a second code path
for "reflection unavailable"), the layout is extracted here, at shader-build time,
from `dxc -dumpbin`, and emitted as a plain C++ table. The engine then has no
reflection dependency at all: it is a header include and a lookup.

`-dumpbin` prints, for the shader's cbuffer:

    ; cbuffer NovaConstants_0
    ; {
    ;   struct hostlayout.NovaConstants_0
    ;   {
    ;           float3 u_CameraPosition_0;                ; Offset:    0
    ;           column_major float4x4 u_InverseViewProjection_0;; Offset:   16
    ;   } NovaConstants_0;                                ; Offset:    0 Size:  3708

Each variable's SIZE is not printed, so it is derived as `next.offset - offset`,
with the last variable as `cbufferSize - offset`. That span can be larger than the
member's real size where the compiler inserts padding, which is safe: `Set()` uses
the size only as an upper bound, so a padded span can never reject a valid write.

Slang also mangles every global with a `_<entry-id>` suffix (`u_PassID_0`), while
the engine's `Set()` calls use the source name (`u_PassID`). The generated entries
carry the SOURCE name (suffix stripped) and the engine additionally indexes by the
raw name, so either spelling resolves.

Usage
-----
    python3 tools/gen_nova_cb_layout.py            # write the header
    python3 tools/gen_nova_cb_layout.py --check    # verify the committed header

`--check` is the staleness gate: it regenerates into memory and diffs against the
file on disk, exiting non-zero if they differ. Wire it into the shader pipeline so
a regenerated `.cs.cso` without a regenerated header is a hard failure instead of a
silently wrong render.
"""

import argparse
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DXC = r"C:\VulkanSDK\1.3.296.0\Bin\dxc.exe"
CSO = os.path.join(REPO, "DemonCore-Editor", "assets", "shaders",
                   "Renderer3D_NovaRenderer.cs.cso")
OUT = os.path.join(REPO, "Wasteland", "src", "Platform", "NVRHI",
                   "NovaShaderReflection.h")

CBUFFER = "NovaConstants"

# A variable line looks like:
#   ;           float3 u_CameraPosition_0;                ; Offset:    0
#   ;           float3 u_ALightPos_0[32];                 ; Offset: 1136
#   ;           column_major float4x4 u_ViewProjection_0; ; Offset:  192
# The name is the identifier immediately before an optional `[count]`, and the
# line must not be a closing brace (those also carry `; Offset:`).
VAR_RE = re.compile(
    r"^;\s+(?P<type>.+?)\s+(?P<name>[A-Za-z_]\w*)(?:\[\d+\])?\s*;\s*;\s*Offset:\s*(?P<off>\d+)\s*$"
)
#   ;   } NovaConstants_0;                                ; Offset:    0 Size:  3708
SIZE_RE = re.compile(r"^\;\s*\}\s*" + CBUFFER + r"\w*;\s*;\s*Offset:\s*\d+\s+Size:\s*(?P<size>\d+)\s*$")

_SUFFIX_RE = re.compile(r"_\d+$")


def run_dumpbin(cso_path):
    """Return `dxc -dumpbin` output for the blob, or exit with a clear message."""
    if not os.path.exists(cso_path):
        sys.exit("Nova DXIL blob not found: %s\nRun tools/compile_nova_slang.py first." % cso_path)
    if not os.path.exists(DXC):
        sys.exit("dxc.exe not found at %s -- install the Vulkan SDK." % DXC)
    proc = subprocess.run([DXC, "-dumpbin", cso_path],
                          capture_output=True, text=True, errors="replace")
    text = proc.stdout + proc.stderr
    if "NovaConstants" not in text:
        sys.exit("dxc -dumpbin produced no NovaConstants block:\n%s" % text[:2000])
    return text


def parse_layout(text):
    """Extract [(source_name, offset)] and the total cbuffer size."""
    lines = text.splitlines()
    in_cb = False
    entries = []
    total = None

    for line in lines:
        stripped = line.strip()
        if not in_cb:
            if stripped.startswith("; cbuffer " + CBUFFER):
                in_cb = True
            continue

        m = SIZE_RE.match(line)
        if m:
            total = int(m.group("size"))
            break

        m = VAR_RE.match(line)
        if m:
            # Closing braces also carry a `; Offset:` column and can therefore
            # match the variable regex (`;   } NovaConstants_0;  ; Offset: 0`).
            # They are structure, not members -- a name captured from one is a
            # phantom entry whose offset (0) also poisons the previous member's
            # derived size.
            if "}" in line:
                continue
            raw = m.group("name")
            offset = int(m.group("off"))
            if raw.startswith("$"):  # anonymous struct members, never addressable
                continue
            entries.append((_SUFFIX_RE.sub("", raw), offset))

    if total is None:
        sys.exit("Could not find the closing `} %s; ... Size:` line in the dump." % CBUFFER)
    if not entries:
        sys.exit("No variables parsed from the %s cbuffer." % CBUFFER)

    # A duplicate source name would make the lookup ambiguous (`Set()` would
    # silently hit whichever entry was emitted last). Fail loudly instead.
    seen = {}
    for name, off in entries:
        if name in seen:
            sys.exit("Duplicate cbuffer member after suffix stripping: %s" % name)
        seen[name] = off

    return entries, total


def render(entries, total, cso_path):
    """Render the header. Deterministic: no timestamps, no absolute paths."""
    lines = []
    lines.append("// GENERATED FILE -- DO NOT EDIT BY HAND.")
    lines.append("//")
    lines.append("// Produced by tools/gen_nova_cb_layout.py from the DXIL blob")
    lines.append("//   DemonCore-Editor/assets/shaders/%s" % os.path.basename(cso_path))
    lines.append("//")
    lines.append("// This table replaces the D3DReflect() call that used to live in")
    lines.append("// DX12Nova::Init(). D3DReflect reads DXBC (SM5.x) and returns E_INVALIDARG")
    lines.append("// on the SM6 DXIL container Slang emits, so the layout is extracted at")
    lines.append("// shader-build time instead of at engine runtime.")
    lines.append("//")
    lines.append("// Variable size is the span to the next member (last member: to the end of")
    lines.append("// the cbuffer). Padding makes a span larger than the member, never smaller,")
    lines.append("// so it is a safe upper bound for DX12Nova::Set()'s bounds check.")
    lines.append("//")
    lines.append("// Regenerate with: python3 tools/gen_nova_cb_layout.py")
    lines.append("// Verify with:     python3 tools/gen_nova_cb_layout.py --check")
    lines.append("")
    lines.append("#pragma once")
    lines.append("")
    lines.append("namespace Wasteland {")
    lines.append("namespace NovaReflection {")
    lines.append("")
    lines.append("struct CbVariable")
    lines.append("{")
    lines.append("    const char *Name;")
    lines.append("    unsigned int Offset;")
    lines.append("    unsigned int Size;")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr unsigned int kCbSize = %du;" % total)
    lines.append("inline constexpr unsigned int kCbVariableCount = %du;" % len(entries))
    lines.append("")
    lines.append("inline constexpr CbVariable kCbVariables[kCbVariableCount] = {")
    for i, (name, off) in enumerate(entries):
        end = entries[i + 1][1] if i + 1 < len(entries) else total
        size = max(end - off, 0)
        lines.append('    {"%s", %uu, %uu},' % (name, off, size))
    lines.append("};")
    lines.append("")
    lines.append("} // namespace NovaReflection")
    lines.append("} // namespace Wasteland")
    lines.append("")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="verify the committed header matches the blob; exit 1 if stale")
    ap.add_argument("--cso", default=CSO, help="DXIL blob to read (default: the deployed Nova blob)")
    args = ap.parse_args()

    entries, total = parse_layout(run_dumpbin(args.cso))
    text = render(entries, total, args.cso)

    if args.check:
        if not os.path.exists(OUT):
            sys.exit("STALE: %s does not exist. Run tools/gen_nova_cb_layout.py." % OUT)
        with open(OUT, "r", encoding="utf-8", newline="") as fh:
            on_disk = fh.read()
        if on_disk.replace("\r\n", "\n") != text:
            sys.exit("STALE: %s does not match the DXIL blob.\n"
                     "Run: python3 tools/gen_nova_cb_layout.py" % OUT)
        print("OK: %s matches the blob (%d members, cbuffer %d bytes)"
              % (os.path.relpath(OUT, REPO), len(entries), total))
        return

    # newline="\n": the header is compared with the other sources' LF form and
    # git normalises on commit, so writing CRLF here would make --check flap.
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("wrote %s (%d members, cbuffer %d bytes)"
          % (os.path.relpath(OUT, REPO), len(entries), total))


if __name__ == "__main__":
    main()
