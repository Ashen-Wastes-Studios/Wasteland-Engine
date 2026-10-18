#!/usr/bin/env python3
"""Verify portable texture sampling and backend-specific texture bindings."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SHADERS = (
    ROOT / "DemonCore-Editor/assets/shaders/Renderer2D_Quad.glsl",
    ROOT / "DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl",
    ROOT / "Sandbox/assets/shaders/Texture.glsl",
)
FETCH = re.compile(
    r"\b(?:texture\w*|texelFetch)\s*\(\s*"
    r"(?P<sampler>[A-Za-z_]\w*)\s*\[\s*(?P<index>[^\]]+)\s*\]"
)
GL_IMAGE = re.compile(
    r"layout\s*\([^)]*?\bbinding\s*=\s*(\d+)[^)]*\)\s*"
    r"uniform\s+(?:(?:readonly|writeonly)\s+)?image\w+\s+(\w+)"
)


def check_source(name: str, source: str) -> tuple[int, list[str]]:
    fetches = list(FETCH.finditer(source))
    errors = [
        f"{name}: sampler {match['sampler']} uses non-constant index "
        f"{match['index'].strip()!r} at offset {match.start()}"
        for match in fetches
        if not match["index"].strip().isdecimal()
    ]
    indices = [int(match["index"].strip()) for match in fetches
               if match["index"].strip().isdecimal()]
    if sorted(indices) != list(range(32)):
        errors.append(f"{name}: expected one literal fetch for each slot 0..31; got {indices}")
    return len(fetches), errors


def check_gl_image_bindings(name: str, source: str) -> tuple[int, list[str]]:
    bindings = [(int(match[1]), match[2]) for match in GL_IMAGE.finditer(source)]
    errors = [
        f"{name}: image {image} binding {binding} exceeds the supported 0..7 range"
        for binding, image in bindings
        if binding > 7
    ]
    if not bindings:
        errors.append(f"{name}: no image bindings found (guard would be vacuous)")
    return len(bindings), errors


def check_dx12_texture_resource() -> list[str]:
    renderer = (ROOT / "Wasteland/src/Wasteland/Renderer/Renderer3D.cpp").read_text(encoding="utf-8")
    texture_header = (ROOT / "Wasteland/src/Platform/NVRHI/NVRHITexture.h").read_text(encoding="utf-8")
    errors = []
    start = renderer.find("std::vector<ID3D12Resource*> textures;")
    end = renderer.find("RunNovaPassSchedule(n, textures);", start)
    if start < 0 or end < 0:
        return ["DX12 Nova texture vector not found (guard would be vacuous)"]
    dx12_binding = renderer[start:end]
    if "GetNativeSRV()" in dx12_binding:
        errors.append("DX12 Nova is receiving the D3D11 SRV accessor instead of an ID3D12Resource")
    if "GetNativeTextureResource()" not in dx12_binding:
        errors.append("DX12 Nova must pass each texture's native D3D12 resource")
    if "GetNativeTextureResource()" not in texture_header or "D3D12_Resource" not in texture_header:
        errors.append("NVRHI texture wrapper does not expose its native D3D12 resource")
    if "(ID3D12Resource*)input->GetNativeSRV()" in renderer:
        errors.append("DX12 material-map generation still casts a D3D11 SRV to ID3D12Resource")
    return errors


def main() -> int:
    failures = []
    for path in SHADERS:
        count, errors = check_source(path.relative_to(ROOT).as_posix(), path.read_text(encoding="utf-8"))
        if count == 0:
            failures.append(f"{path.relative_to(ROOT)}: no sampler-array fetches found (guard would be vacuous)")
        failures.extend(errors)

    nova_glsl = SHADERS[1].read_text(encoding="utf-8")
    image_count, errors = check_gl_image_bindings("Renderer3D_NovaRenderer.glsl", nova_glsl)
    if image_count != 9:
        errors.append(f"Renderer3D_NovaRenderer.glsl: expected 9 image declarations, found {image_count}")
    failures.extend(errors)
    failures.extend(check_dx12_texture_resource())

    # Prove the detector rejects the defect this guard is meant to catch.
    count, errors = check_source("negative-control", "texture(u_Texture[dynamicSlot], uv);")
    if count != 1 or not errors:
        failures.append("negative control did not reject a dynamic sampler-array index")
    _, errors = check_gl_image_bindings(
        "negative-control", "layout(rg32f, binding = 9) uniform image2D moments;"
    )
    if not errors:
        failures.append("negative control did not reject an out-of-range GL image unit")

    if failures:
        print("TEXTURE SAMPLER INDEXING: FAIL")
        print("\n".join(f"- {failure}" for failure in failures))
        return 1

    print("TEXTURE SAMPLER INDEXING: PASS")
    for path in SHADERS:
        count, _ = check_source(path.relative_to(ROOT).as_posix(), path.read_text(encoding="utf-8"))
        print(f"{path.relative_to(ROOT)}: {count} literal-index fetches")
    print("negative control: dynamic index rejected")
    print("GL image units: within 0..7; out-of-range negative control rejected")
    print("DX12 texture array: native ID3D12Resource accessor wired")
    return 0


if __name__ == "__main__":
    sys.exit(main())
