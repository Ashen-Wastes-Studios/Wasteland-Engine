#!/usr/bin/env python3
"""Guard Wasteland's runtime render-capability map against phantom RT support.

This is a source-level contract test, not a GPU device-support test. It ensures
that capabilities describe paths currently wired into Renderer3D rather than
API names or presumed hardware features.

Two distinct rules live here, and they are not the same rule:

  1. RT fields (hardwareRT, inlineRaytracing, rayQuery, tensorCores) must stay
     UNCLAIMED until a Wasteland RT path actually exists. A device can advertise
     DXR and the engine can still have nothing that dispatches it -- that is the
     phantom-support failure this file was written to catch.

  2. Mesh fields (meshShaders, amplificationShaders) are a DEVICE fact, so they
     may be probed -- but only from the device, never hard-coded per API case and
     never defaulted true in the header. `nvrhi::Feature::Meshlets` is the only
     sanctioned source.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
CPP = ROOT / "Wasteland" / "src" / "Wasteland" / "Renderer" / "RenderCapabilities.cpp"
HEADER = ROOT / "Wasteland" / "src" / "Wasteland" / "Renderer" / "RenderCapabilities.h"
RENDERER = ROOT / "Wasteland" / "src" / "Wasteland" / "Renderer" / "Renderer3D.cpp"

# Fields that must never be claimed before a Wasteland RT path exists.
RT_FIELDS = ("hardwareRT", "inlineRaytracing", "rayQuery", "tensorCores")
# Fields that are device facts: probed, never constant.
DEVICE_FIELDS = ("meshShaders", "amplificationShaders")

# The reconstruction tiers and the passes each one implies. A tier that
# resolves to a pass nothing dispatches is a phantom tier -- the same failure
# class as phantom RT support, one level up.
RECONSTRUCT_TIERS = {
    "ReconstructSpatial": (15, 24),   # WLSR RCAS + reactive mask
    "ReconstructTemporal": (20, 21),  # + WLRR kernel family + residual head
    "ReconstructFull": (22, 23),      # + WLFG warp + neural synthesis head
}


def case_body(source: str, api: str) -> str:
    marker = f"case RendererAPI::API::{api}:"
    start = source.find(marker)
    if start < 0:
        return ""
    end = source.find("case RendererAPI::API::", start + len(marker))
    if end < 0:
        end = source.find("        }", start)
    return source[start + len(marker):end]


def assigned_true(body: str, member: str) -> bool:
    return re.search(r"\bcaps\." + re.escape(member) + r"\s*=\s*true\s*;", body) is not None


def violations(cpp: str, header: str, renderer: str = "") -> list[str]:
    found = []
    expected_compute = {
        "OpenGL": True,
        "NVRHI_DX11": True,
        "NVRHI_DX12": True,
        "NVRHI_Vulkan": False,
    }
    for api, expected in expected_compute.items():
        body = case_body(cpp, api)
        if not body:
            found.append(f"missing API case: {api}")
            continue
        got = assigned_true(body, "computeRaytracing")
        if got != expected:
            found.append(f"{api}: computeRaytracing expected {expected}, got {got}")
        for field in RT_FIELDS:
            if assigned_true(body, field):
                found.append(f"{api}: {field} advertised before an engine/device RT path exists")
        for field in DEVICE_FIELDS:
            if assigned_true(body, field):
                found.append(f"{api}: {field} hard-coded true instead of probed from the device")
        if re.search(r"\bcaps\.maxRecursionDepth\s*=\s*[1-9]\d*\s*;", body):
            found.append(f"{api}: recursion limit advertised without a device/path query")

    for field in RT_FIELDS + DEVICE_FIELDS:
        if re.search(r"\bbool\s+" + re.escape(field) + r"\s*=\s*true\s*;", header):
            found.append(f"header default enables {field}")

    # The device probe must actually be merged into the map before it is
    # published. Without this line a future refactor can add the probe function
    # and never call it, and every capability would silently read false.
    if not re.search(r"MergeDeviceFeatureCapabilities\s*\(\s*caps\s*\)", cpp):
        found.append("device feature probe is not merged into the capability map")

    if not re.search(r"case\s+FeatureTier::HardwareRT\s*:\s*if\s*\(hardwareRT\)", header):
        found.append("HardwareRT tier no longer gates on the runtime capability")
    if not re.search(r"if\s*\(computeRaytracing\)\s*return\s+FeatureTier::ComputeEmulated", header):
        found.append("HardwareRT tier no longer degrades to the available compute path")

    # The reconstruction tiers: declared, named, resolved by API, and each one
    # backed by a pass that is actually dispatched. A tier with no dispatched
    # pass is a phantom tier.
    for tier in RECONSTRUCT_TIERS:
        if not re.search(r"^\s*" + tier + r"\s*=\s*\d+", header, re.M):
            found.append(f"header no longer declares the {tier} tier")
        if not re.search(r"case\s+FeatureTier::" + tier + r"\s*:", cpp):
            found.append(f"TierName no longer names {tier}")
    if not re.search(r"\(probe\s*>\s*best\)\s*\?\s*best\s*:", header):
        found.append("the reconstruction resolver no longer degrades a probe to the backend level")
    # A vendor / tensor-core gate would make the stack RTX-only. The plan rules
    # it out explicitly; assert it rather than trusting the comment.
    if re.search(r"if\s*\(\s*tensorCores", header):
        found.append("the tier resolver is gated on tensorCores (vendor-class gate)")
    if renderer:
        for tier, passes in RECONSTRUCT_TIERS.items():
            for p in passes:
                if not re.search(r"\bpass\(\s*" + str(p) + r"\s*[,)]", renderer):
                    found.append(f"{tier} resolves to pass {p}, which the renderer never dispatches")
    return found


def main() -> int:
    cpp = CPP.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    renderer = RENDERER.read_text(encoding="utf-8") if RENDERER.exists() else ""
    errors = violations(cpp, header, renderer)
    if errors:
        for item in errors:
            print(f"FAIL {item}")
        return 1

    # Negative control: deliberately claim DX12 hardware RT and ensure this
    # checker rejects the false positive rather than merely matching comments.
    dx12 = case_body(cpp, "NVRHI_DX12")
    mutated = cpp.replace(dx12, dx12.replace("caps.computeRaytracing = true;", "caps.computeRaytracing = true;\n            caps.hardwareRT = true;", 1), 1)
    if not violations(mutated, header):
        print("FAIL negative control: phantom DX12 hardware-RT claim was not caught")
        return 1

    # Negative control 2: hard-code a mesh capability per API case. Same class of
    # defect as the RT one -- a capability asserted rather than measured -- and a
    # checker that only catches the RT shape is decoration.
    mutated_mesh = cpp.replace(dx12, dx12.replace(
        "caps.computeRaytracing = true;",
        "caps.computeRaytracing = true;\n            caps.meshShaders = true;", 1), 1)
    if not violations(mutated_mesh, header):
        print("FAIL negative control: hard-coded DX12 mesh-shader claim was not caught")
        return 1

    # Negative control 3: default a device capability to true in the header.
    mutated_header = header.replace("bool meshShaders = false;",
                                    "bool meshShaders = true;", 1)
    if not violations(cpp, mutated_header):
        print("FAIL negative control: header-default mesh-shader claim was not caught")
        return 1

    # Negative control 4: drop the device-probe merge call entirely.
    mutated_merge = cpp.replace("MergeDeviceFeatureCapabilities(caps);", "", 1)
    if not violations(mutated_merge, header):
        print("FAIL negative control: a missing device-probe merge was not caught")
        return 1

    # Negative control 5: gate the reconstruction tiers on tensorCores. The plan
    # rules a vendor/tensor-core gate out explicitly; a checker that only catches
    # phantom RT fields would let the stack become RTX-only silently.
    mutated_tier = header.replace("if (hardwareRT)", "if (hardwareRT && tensorCores)", 1)
    if not violations(cpp, mutated_tier):
        print("FAIL negative control: a tensorCores-gated tier was not caught")
        return 1

    # Negative control 6: a tier that resolves to a pass nothing dispatches.
    mutated_dispatch = renderer.replace("pass(20);", "", 1)
    if not violations(cpp, header, mutated_dispatch):
        print("FAIL negative control: a phantom tier (pass 20 undispatched) was not caught")
        return 1

    print("PASS: compute tracing is exposed only on active GL/DX11/DX12 paths")
    print("PASS: DX12/Vulkan hardware RT, ray query, inline RT, tensor and recursion remain unclaimed")
    print("PASS: mesh-shader capability is probed from the device, never hard-coded")
    print("PASS: device feature probe is merged into the capability map")
    print("PASS: HardwareRT tier degrades to the compute path")
    print("PASS: the three reconstruction tiers are declared, named and resolved by API")
    print("PASS: every reconstruction tier is backed by a dispatched pass")
    print("PASS: 6 negative controls catch a phantom RT claim, a hard-coded mesh claim,")
    print("       a header-defaulted mesh claim, a missing probe merge, a tensorCores-gated")
    print("       tier and a phantom reconstruction tier")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
