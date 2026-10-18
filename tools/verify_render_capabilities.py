#!/usr/bin/env python3
"""Guard Wasteland's runtime render-capability map against phantom RT support.

This is a source-level contract test, not a GPU device-support test. It ensures
that capabilities describe paths currently wired into Renderer3D rather than
API names or presumed hardware features.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
CPP = ROOT / "Wasteland" / "src" / "Wasteland" / "Renderer" / "RenderCapabilities.cpp"
HEADER = ROOT / "Wasteland" / "src" / "Wasteland" / "Renderer" / "RenderCapabilities.h"


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


def violations(cpp: str, header: str) -> list[str]:
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
        for field in ("hardwareRT", "inlineRaytracing", "rayQuery", "tensorCores"):
            if assigned_true(body, field):
                found.append(f"{api}: {field} advertised before an engine/device RT path exists")
        if re.search(r"\bcaps\.maxRecursionDepth\s*=\s*[1-9]\d*\s*;", body):
            found.append(f"{api}: recursion limit advertised without a device/path query")

    for field in ("hardwareRT", "inlineRaytracing", "rayQuery", "tensorCores"):
        if re.search(r"\bbool\s+" + re.escape(field) + r"\s*=\s*true\s*;", header):
            found.append(f"header default enables {field}")
    if not re.search(r"case\s+FeatureTier::HardwareRT\s*:\s*if\s*\(hardwareRT\)", header):
        found.append("HardwareRT tier no longer gates on the runtime capability")
    if not re.search(r"if\s*\(computeRaytracing\)\s*return\s+FeatureTier::ComputeEmulated", header):
        found.append("HardwareRT tier no longer degrades to the available compute path")
    return found


def main() -> int:
    cpp = CPP.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    errors = violations(cpp, header)
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

    print("PASS: compute tracing is exposed only on active GL/DX11/DX12 paths")
    print("PASS: DX12/Vulkan hardware RT, ray query, inline RT, tensor and recursion remain unclaimed")
    print("PASS: HardwareRT tier degrades to the compute path")
    print("PASS: negative control catches a phantom hardware-RT claim")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
