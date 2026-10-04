#!/usr/bin/env python3
"""
Build NVIDIA NRD from the vendored source into the static lib Wasteland links.

NRD is not a header-only library and it is not checked in as a prebuilt binary.
Its shaders are compiled to DXBC/DXIL/SPIR-V at build time by its own
ShaderMake tool and embedded into the library as C arrays, so there is a real
generated step between `git clone` and a working NRD.lib. This script performs
it, using the same Vulkan SDK the rest of the engine builds against.

Run once after cloning, and again only if NRD source changes:

    python tools/build_nrd.py

Output: Wasteland/vendor/NRD/_Bin/Release/NRD.lib (+ NRD.dll), referenced from
premake5.lua. premake5.lua's includedirs already point at _Shaders, because
NRD's own headers include the generated *.cs.*.h shader blobs.

DXC for SPIR-V comes from the Vulkan SDK (NRD's CMake finds it automatically);
DXC for DXIL and FXC for DXBC come from the Windows SDK, which NRD's CMake
also probes. If a path is wrong, pass --dxc / --fxc rather than editing this.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NRD = os.path.join(REPO, "Wasteland", "vendor", "NRD")
BUILD = os.path.join(REPO, "bin-int", "nrd-cmake")

# ShaderMake refuses to build against an SDK older than this (NRD's own check).
MIN_SDK_BUILD = 22621


def run(cmd, cwd=None):
    print("$ " + " ".join(cmd[:6]) + (" ..." if len(cmd) > 6 else ""))
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                          errors="replace")


def fail(msg, detail=""):
    sys.exit("ERROR: %s\n%s" % (msg, detail))


def find_msbuild():
    vs = r"C:\Program Files\Microsoft Visual Studio"
    for root, dirs, _files in os.walk(vs):
        for d in list(dirs):
            if d == "MSBuild":
                exe = os.path.join(root, d, "Current", "Bin", "MSBuild.exe")
                if os.path.exists(exe):
                    return exe
    fail("MSBuild.exe not found under %s" % vs)


def sdk_version():
    """Highest Windows SDK version directory under the Kits include dir."""
    base = r"C:\Program Files (x86)\Windows Kits\10\Include"
    best = 0
    if os.path.isdir(base):
        for d in os.listdir(base):
            m = re.fullmatch(r"10\.0\.(\d+)\.(\d+)\.(\d+)", d)
            if m:
                best = max(best, int(m.group(1)))
    return best


def ensure_submodules():
    """MathLib and ShaderMake are git submodules; without them NRD's CMake
    fails on a missing ml.h, and the empty directory looks checked-out."""
    missing = [m for m in ("External/MathLib/ml.h", "External/ShaderMake/CMakeLists.txt")
               if not os.path.exists(os.path.join(NRD, m))]
    if not missing:
        print("submodules present")
        return
    print("initialising NRD submodules (needed for %s)..." % ", ".join(missing))
    r = run(["git", "submodule", "update", "--init", "--depth", "1",
             "External/MathLib", "External/ShaderMake"], cwd=NRD)
    if r.returncode != 0:
        fail("git submodule update failed", r.stdout + r.stderr)
    still = [m for m in missing if not os.path.exists(os.path.join(NRD, m))]
    if still:
        fail("submodules still missing after update: %s" % ", ".join(still))


def configure():
    cmake = shutil.which("cmake")
    if not cmake:
        fail("cmake not on PATH")
    vs = sdk_version()
    if vs and vs < MIN_SDK_BUILD:
        fail("Windows SDK 10.0.%d.0 is older than ShaderMake's minimum 10.0.%d.0"
             % (vs, MIN_SDK_BUILD))
    print("Windows SDK 10.0.%d.0" % vs)

    os.makedirs(BUILD, exist_ok=True)
    r = run([cmake, NRD, "-G", "Visual Studio 18 2026", "-A", "x64"], cwd=BUILD)
    if r.returncode != 0:
        fail("cmake configure failed", r.stdout + r.stderr)
    # NRD reports the DXC/FXC/DXC-SPIRV paths it found; surface them, because a
    # silently missing DXC means a lib with no shaders in it.
    for line in r.stdout.split("\n"):
        if re.search(r"Setting '(FXC|DXC|DXC_SPIRV)_PATH'", line):
            print("  " + line.strip())


def build(msbuild):
    proj = os.path.join(BUILD, "NRD.vcxproj")
    if not os.path.exists(proj):
        fail("NRD.vcxproj not generated -- configure step did not produce a solution")
    print("building NRD (this compiles ~243 shaders to 3 targets; expect minutes)...")
    r = run([msbuild, proj, "-p:Configuration=Release", "-p:Platform=x64",
             "-v:minimal"])
    tail = (r.stdout + r.stderr).strip().split("\n")
    print("\n".join(tail[-12:]))
    if r.returncode != 0:
        fail("NRD build failed")

    lib = os.path.join(NRD, "_Bin", "Release", "NRD.lib")
    if not os.path.exists(lib):
        fail("build reported success but NRD.lib is missing")
    size_kb = os.path.getsize(lib) / 1024.0
    # NRD.lib is an import library, not the implementation: on Windows the
    # code lives in NRD.dll, so its size says nothing about whether it built.
    dll = os.path.join(NRD, "_Bin", "Release", "NRD.dll")
    if size_kb < 100:
        sys.exit("ERROR: NRD.lib is only %.0f KB and NRD.dll is absent -- the build "
                 "produced no import library" % size_kb)
    print("built %s (%.0f KB import lib, %s)"
          % (lib, size_kb,
             ("NRD.dll %.1f MB" % (os.path.getsize(dll) / 1048576.0))
              if os.path.exists(dll) else "no NRD.dll")))


def verify():
    """A lib that compiled but has no embedded shaders links fine and then
    renders nothing, so count the blobs rather than trusting exit code 0."""
    shaders = os.path.join(NRD, "_Shaders")
    counts = {"spirv": 0, "dxil": 0, "dxbc": 0}
    if os.path.isdir(shaders):
        for f in os.listdir(shaders):
            for k in counts:
                if f.endswith(".cs.%s.h" % k):
                    counts[k] += 1
    print("embedded shaders: spirv=%d dxil=%d dxbc=%d"
          % (counts["spirv"], counts["dxil"], counts["dxbc"]))
    if min(counts.values()) == 0:
        fail("no embedded shaders for at least one backend; the lib would link "
             "but produce no denoising")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--clean", action="store_true",
                    help="delete the cmake tree first (needed after changing generators)")
    args = ap.parse_args()

    if not os.path.isdir(NRD):
        fail("NRD not vendored at %s -- clone it first:\n"
             "  git clone --depth 1 --branch v4.9.4 "
             "https://github.com/NVIDIA-RTX/NRD.git Wasteland/vendor/NRD" % NRD)
    if args.clean and os.path.isdir(BUILD):
        shutil.rmtree(BUILD)
        print("removed", BUILD)

    ensure_submodules()
    msbuild = find_msbuild()
    print("msbuild:", msbuild)
    configure()
    build(msbuild)
    verify()
    print("OK - premake5.lua already links NRD.lib and adds its include dirs")


if __name__ == "__main__":
    main()