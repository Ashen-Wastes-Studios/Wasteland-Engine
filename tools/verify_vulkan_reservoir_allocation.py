#!/usr/bin/env python3
"""Guard VulkanNova's ReSTIR buffer-size arithmetic against integer wrap.

The source contract requires a widened pixel product and a checked multiply
before any size-dependent GPU resources are allocated. The Python boundary model
is not a GPU allocation test or proof of backend runtime behavior.
"""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "Wasteland" / "src" / "Platform" / "NVRHI" / "VulkanNova.cpp"
U32_MAX = (1 << 32) - 1
U64_MAX = (1 << 64) - 1
BYTES_PER_RESERVOIR = 32


class ContractError(AssertionError):
    pass


def require(condition, message):
    if not condition:
        raise ContractError(message)


def strip_comments(source):
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def function_body(source, signature, label):
    code = strip_comments(source)
    match = re.search(signature, code)
    require(match is not None, f"{label}: definition not found")
    opening = code.find("{", match.end())
    require(opening >= 0, f"{label}: body not found")
    depth = 0
    for index in range(opening, len(code)):
        if code[index] == "{":
            depth += 1
        elif code[index] == "}":
            depth -= 1
            if depth == 0:
                return code[opening + 1 : index]
    raise ContractError(f"{label}: unterminated body")


def check_source(source):
    body = function_body(
        source,
        r"\bvoid\s+VulkanNova::CreateImages\s*\(",
        "VulkanNova::CreateImages",
    )
    normalized = re.sub(r"\s+", "", body)
    require(
        "constexpruint64_tkReservoirBytesPerPixel=32;" in normalized,
        "reservoir stride must be explicitly 32 bytes in uint64 arithmetic",
    )
    require(
        "constuint64_tpixelCount=static_cast<uint64_t>(w)*static_cast<uint64_t>(h);" in normalized,
        "pixel count must widen both dimensions before multiplication",
    )
    guard = re.search(
        r"if\(pixelCount>std::numeric_limits<uint64_t>::max\(\)/kReservoirBytesPerPixel\)",
        normalized,
    )
    require(guard is not None, "pixelCount * stride must be checked before multiplication")
    require(
        'throwstd::overflow_error("VulkanNova:reservoirbuffersizeoverflow")' in normalized,
        "overflow must fail explicitly instead of wrapping into a smaller allocation",
    )
    require(
        "constuint64_treservoirByteSize=pixelCount*kReservoirBytesPerPixel;" in normalized,
        "reservoir byte size must remain uint64_t after the checked multiply",
    )
    require(
        "reservoirDesc.byteSize=reservoirByteSize;" in normalized,
        "NVRHI BufferDesc must receive the checked 64-bit byte size",
    )
    require(
        guard.start() < normalized.find("m_Device->createTexture")
        and guard.start() < normalized.find("m_Device->createBuffer"),
        "size overflow must be rejected before texture/buffer allocation begins",
    )


def checked_bytes(width, height):
    require(0 <= width <= U32_MAX and 0 <= height <= U32_MAX, "dimensions outside uint32 range")
    pixels = width * height
    if pixels > U64_MAX // BYTES_PER_RESERVOIR:
        raise OverflowError("uint64 reservoir byte-size overflow")
    return pixels * BYTES_PER_RESERVOIR


def expect_rejected(label, source):
    try:
        check_source(source)
    except ContractError as error:
        print(f"PASS negative control: {label} rejected ({error})")
        return
    raise ContractError(f"negative control failed: accepted {label}")


def main():
    source = SOURCE.read_text(encoding="utf-8")
    check_source(source)
    print("PASS VulkanNova source widens, checks, and retains reservoir byte size")

    require(checked_bytes(1920, 1080) == 66_355_200, "1080p two-buffer slot size fixture failed")
    require(checked_bytes(U32_MAX, 1) == U32_MAX * BYTES_PER_RESERVOIR, "single-row uint32 boundary failed")
    try:
        checked_bytes(U32_MAX, U32_MAX)
    except OverflowError:
        print("PASS arithmetic model rejects a uint32-dimension product that overflows uint64 bytes")
    else:
        raise ContractError("arithmetic model accepted uint64 overflow dimensions")

    unguarded = re.sub(
        r"if\s*\(\s*pixelCount\s*>\s*std::numeric_limits<uint64_t>::max\(\)\s*/\s*kReservoirBytesPerPixel\s*\)\s*throw\s+std::overflow_error\([^;]+;",
        "",
        source,
        count=1,
    )
    require(unguarded != source, "could not construct missing-overflow-guard mutation")
    expect_rejected("missing overflow guard", unguarded)

    narrowed = source.replace(
        "const uint64_t reservoirByteSize = pixelCount * kReservoirBytesPerPixel;",
        "const uint32_t reservoirByteSize = w * h * 32;",
        1,
    )
    require(narrowed != source, "could not construct narrowed-byte-size mutation")
    expect_rejected("narrowed uint32 byte size", narrowed)

    print("Source/model checks only: no Vulkan GPU allocation, dispatch, or visual proof.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ContractError, OSError) as error:
        print(f"FAIL Vulkan reservoir allocation contract: {error}", file=sys.stderr)
        raise SystemExit(1)
