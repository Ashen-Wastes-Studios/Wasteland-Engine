#!/usr/bin/env python3
"""Source-contract checks for VulkanNova's descriptor and clear lifecycle.

This does not exercise Vulkan, NVRHI, shader execution, or GPU correctness.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
CPP_PATH = ROOT / "Wasteland" / "src" / "Platform" / "NVRHI" / "VulkanNova.cpp"
HEADER_PATH = ROOT / "Wasteland" / "src" / "Platform" / "NVRHI" / "VulkanNova.h"
CONTEXT_CPP_PATH = ROOT / "Wasteland" / "src" / "Platform" / "NVRHI" / "NVRHIContext.cpp"
CONTEXT_HEADER_PATH = ROOT / "Wasteland" / "src" / "Platform" / "NVRHI" / "NVRHIContext.h"
FRAMEBUFFER_CPP_PATH = ROOT / "Wasteland" / "src" / "Platform" / "NVRHI" / "NVRHIFramebuffer.cpp"
FRAMEBUFFER_HEADER_PATH = ROOT / "Wasteland" / "src" / "Platform" / "NVRHI" / "NVRHIFramebuffer.h"
IMGUI_LAYER_PATH = ROOT / "Wasteland" / "src" / "Wasteland" / "ImGui" / "ImGuiLayer.cpp"


def body_span(source: str, signature: str) -> tuple[int, int] | None:
    """Return the brace-body span, ignoring C++ comments and quoted strings."""
    start = source.find(signature)
    if start < 0:
        return None
    opening = source.find("{", start + len(signature))
    if opening < 0:
        return None
    depth = 1
    i = opening + 1
    state = "code"
    while i < len(source):
        ch = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""
        if state == "line_comment":
            if ch == "\n":
                state = "code"
        elif state == "block_comment":
            if ch == "*" and nxt == "/":
                state = "code"
                i += 1
        elif state in ("string", "char"):
            if ch == "\\":
                i += 1
            elif (state == "string" and ch == '"') or (state == "char" and ch == "'"):
                state = "code"
        else:
            if ch == "/" and nxt == "/":
                state = "line_comment"
                i += 1
            elif ch == "/" and nxt == "*":
                state = "block_comment"
                i += 1
            elif ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return opening + 1, i
        i += 1
    return None


def method_body(source: str, signature: str) -> str:
    span = body_span(source, signature)
    return "" if span is None else source[span[0]:span[1]]


def replace_in_body(source: str, signature: str, old: str, new: str) -> str | None:
    span = body_span(source, signature)
    if span is None:
        return None
    start, end = span
    body = source[start:end]
    if old not in body:
        return None
    return source[:start] + body.replace(old, new, 1) + source[end:]


def violations(cpp: str, header: str) -> list[str]:
    errors: list[str] = []
    checks = [
        ("inline Set delegates to SetBytes", "void Set(const char *name", header,
         ("std::is_trivially_copyable<T>::value", "SetBytes(name, &value, sizeof(T), index);")),
        ("SetBytes has an out-of-line definition", "void VulkanNova::SetBytes(", cpp,
         ("FindConstant(name)", "std::memcpy(m_ConstantBytes.data() + offset, value, size);")),
        ("new structured buffers dirty the bindings", "void VulkanNova::EnsureStructuredBuffer(", cpp,
         ("m_StructuredBuffers[slot] = buffer;", "m_BindingSetDirty = true;")),
        ("empty uploads allocate and initialize one dummy record", "void VulkanNova::Upload(", cpp,
         ("count == 0 ? 1u", "zeroRecord.assign(stride, 0)", "uploadData = zeroRecord.data();")),
        ("structured upload size is checked before narrowing", "void VulkanNova::Upload(", cpp,
         ("std::numeric_limits<uint32_t>::max", "static_cast<uint32_t>(byteCount)")),
        ("new image sets request a deferred clear", "void VulkanNova::CreateImages(", cpp,
         ("m_ImagesNeedClear = true;",)),
        ("pass images have storage usage; snapshots do not", "void VulkanNova::CreateImages(", cpp,
         ("desc.isUAV = true;", "ResourceStates::CopySource;", "snapDesc.isUAV = false;",
          "ResourceStates::CopyDest |", "ResourceStates::ShaderResource;",
          "snapDesc.keepInitialState = true;")),
        ("dirty image snapshots copy from the live image", "void VulkanNova::CopyImageToSnapshot(", cpp,
         ("snapshotDirty", "RecordCopy(m_Images[imageIndex].snapshot, m_Images[imageIndex].texture);",
          "m_Images[imageIndex].snapshotDirty = false;")),
        ("scene descriptor array uses a real fallback", "void VulkanNova::CreateBindingSet(", cpp,
         ("if (!m_HistorySnapshot)", "m_SceneSRVs.resize(kSceneTextureCount, m_HistorySnapshot);",
          "BindingSetItem::Texture_SRV(\n                15, m_SceneSRVs[i])")),
        ("dispatch refreshes all scene slots and rebuilds dirty bindings", "void VulkanNova::Dispatch(", cpp,
         ("i < kSceneTextureCount", "ResolveSceneTexture(textures[i].get())",
          "m_BindingSetDirty = true;", "if (m_BindingSetDirty || !m_BindingSet)", "CreateBindingSet();")),
        ("deferred image/history clear precedes dispatch", "void VulkanNova::Dispatch(", cpp,
         ("if (m_ImagesNeedClear)", "RecordImageHistoryClear();", "setComputeState(state)")),
        ("Clear defers full clears when no renderer list is open", "void VulkanNova::Clear(", cpp,
         ("m_ImagesNeedClear = true;", "m_ReservoirsNeedClear = true;",
          "if (!EnsureCommandList() || !m_Context->IsCommandListOpen())",
          "RecordImageHistoryClear();")),
    ]
    for label, signature, source, required in checks:
        body = method_body(source, signature)
        if not body or any(item not in body for item in required):
            errors.append(label)

    # The compiled shader reads these two UAV images through their read-only
    # aliases during ReSTIR. They must be included in both the SPIR-V SRV table
    # and the snapshot-image map; omitting them makes VulkanNova reject its own
    # checked-in SPIR-V before the first dispatch.
    active_read_slots = (
        "kReadImageCount = 8;" in header and
        "kReadImageSlot[kReadImageCount] = {64, 65, 66, 68, 69, 70, 71, 73};" in cpp and
        "kReadImageIndex[kReadImageCount] = {0, 1, 2, 4, 5, 6, 7, 8};" in cpp
    )
    if not active_read_slots:
        errors.append("Vulkan read-image descriptors cover active shader SRVs")

    dispatch_body = method_body(cpp, "void VulkanNova::Dispatch(")
    if (not dispatch_body or
        dispatch_body.find("RefreshImageSnapshotsForPass(pass);") < 0 or
        dispatch_body.find("m_CommandList->dispatch((w + 7) / 8, (h + 7) / 8, 1);") < 0 or
        dispatch_body.find("MarkImageSnapshotsDirtyAfterPass(pass);") < 0 or
        dispatch_body.find("RefreshImageSnapshotsForPass(pass);") >
        dispatch_body.find("m_CommandList->dispatch((w + 7) / 8, (h + 7) / 8, 1);") or
        dispatch_body.find("MarkImageSnapshotsDirtyAfterPass(pass);") <
        dispatch_body.find("m_CommandList->dispatch((w + 7) / 8, (h + 7) / 8, 1);")):
        errors.append("dispatch refreshes dirty inputs before and marks outputs after the GPU pass")

    refresh_body = method_body(cpp, "void VulkanNova::RefreshImageSnapshotsForPass(")
    required_refreshes = (
        "case 2:", "CopyImageToSnapshot(2);", "CopyImageToSnapshot(5);",
        "case 3:", "CopyImageToSnapshot(4);", "CopyImageToSnapshot(6);",
        "case 11:", "case 12:", "CopyImageToSnapshot(8);",
        "case 13:", "CopyImageToSnapshot(1);",
    )
    if not refresh_body or any(token not in refresh_body for token in required_refreshes):
        errors.append("pass-local snapshot refresh covers temporal, ReSTIR, and SVGF inputs")
    pass14_refresh = re.search(
        r"case 14:(.*?)(?=\n\s*case \d+:|\n\s*default:|\n\s*})", refresh_body, re.S
    )
    if not pass14_refresh or any(token not in pass14_refresh.group(1) for token in (
        "CopyImageToSnapshot(2);", "CopyImageToSnapshot(4);", "CopyImageToSnapshot(6);"
    )):
        errors.append("pass 14 refreshes current bloom, albedo, and trace-G-buffer snapshots")
    pass11_refresh = re.search(
        r"case 11:(.*?)(?=\n\s*case \d+:|\n\s*default:|\n\s*})", refresh_body, re.S
    )
    if not pass11_refresh or "CopyImageToSnapshot(2);" in pass11_refresh.group(1) or any(
        token not in pass11_refresh.group(1) for token in (
            "CopyImageToSnapshot(4);", "CopyImageToSnapshot(5);", "CopyImageToSnapshot(6);"
        )
    ):
        errors.append("pass 11 refreshes G-buffer/velocity only after temporal shading is deferred")

    dirty_body = method_body(cpp, "void VulkanNova::MarkImageSnapshotsDirtyAfterPass(")
    required_writes = (
        "case 0: markDirty(5);", "case 1: markDirty(2); markDirty(4); markDirty(6);",
        "case 2: markDirty(1);", "case 12: markDirty(8);",
        "case 13: markDirty(7);",
    )
    if not dirty_body or any(token not in dirty_body for token in required_writes):
        errors.append("snapshot dirty tracking follows each shader pass output")
    pass14_dirty = re.search(
        r"case 14:(.*?)(?=\n\s*case \d+:|\n\s*default:|\n\s*})", dirty_body, re.S
    )
    if not pass14_dirty or "markDirty(2);" not in pass14_dirty.group(1):
        errors.append("pass 14 marks bloom dirty before temporal accumulation reads it")
    pass11_dirty = re.search(
        r"case 11:(.*?)(?=\n\s*case \d+:|\n\s*default:|\n\s*})", dirty_body, re.S
    )
    if pass11_dirty and "markDirty" in pass11_dirty.group(1):
        errors.append("pass 11 does not dirty images after becoming reservoir-only")

    clear_body = method_body(cpp, "void VulkanNova::RecordImageHistoryClear(")
    if not clear_body or "m_ImagesNeedClear = false;" not in clear_body:
        errors.append("completed image/history clear resets the pending flag")
    return errors


def interop_violations(context_cpp: str, context_header: str,
                       framebuffer_cpp: str, framebuffer_header: str,
                       imgui_layer: str) -> list[str]:
    errors: list[str] = []
    if "SetVulkanImGuiComputeOutput(nvrhi::TextureHandle texture)" not in context_header:
        errors.append("context exposes compute-output ImGui registration")
    if "RecordVulkanImGuiComputeOutputReturnTransition()" not in context_header:
        errors.append("context exposes compute-output return transition")

    to_ui = method_body(context_cpp, "void NVRHIContext::RecordVulkanImGuiAttachmentTransition(")
    if not to_ui or any(token not in to_ui for token in (
        "m_VkImGuiComputeOutput", "VK_IMAGE_LAYOUT_GENERAL",
        "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL", "VK_ACCESS_SHADER_WRITE_BIT",
        "VK_ACCESS_SHADER_READ_BIT", "VK_PIPELINE_STAGE_ALL_COMMANDS_BIT",
        "VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT", "m_VkImGuiComputeOutputReturn = texture")):
        errors.append("compute output transitions GENERAL to shader-read and retains return handle")

    to_compute = method_body(context_cpp, "void NVRHIContext::RecordVulkanImGuiComputeOutputReturnTransition(")
    if not to_compute or any(token not in to_compute for token in (
        "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL", "VK_IMAGE_LAYOUT_GENERAL",
        "VK_ACCESS_SHADER_READ_BIT", "VK_ACCESS_SHADER_WRITE_BIT",
        "VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT", "VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT")):
        errors.append("UI sampling returns compute output to GENERAL before next dispatch")

    external = method_body(framebuffer_cpp, "void *NVRHIFramebuffer::GetExternalTextureImGuiTexID(")
    if not external or any(token not in external for token in (
        "context->SetVulkanImGuiComputeOutput", "GetVulkanImGuiTexID")):
        errors.append("external NVRHI texture gets cached ImGui descriptor and compute registration")
    if "GetExternalTextureImGuiTexID(nvrhi::TextureHandle texture) const" not in framebuffer_header:
        errors.append("framebuffer exposes a typed external-texture ImGui accessor")
    release = method_body(framebuffer_cpp, "bool NVRHIFramebuffer::ReleaseVulkanImGuiDescriptorSet(")
    if not release or any(token not in release for token in (
        "m_VkImGuiCachePool", "GetVkImGuiDescriptorPool()",
        "vkDeviceWaitIdle(context->GetVkDevice())")):
        errors.append("descriptor replacement waits for in-flight UI use and respects pool ownership")

    begin_frame = method_body(context_cpp, "void NVRHIContext::BeginFrame(")
    if not begin_frame or any(token not in begin_frame for token in (
        "m_VkImGuiComputeOutput = nullptr;", "m_VkImGuiComputeOutputReturn = nullptr;")):
        errors.append("aborted frame cannot replay compute-output transitions")

    end_pass = imgui_layer.find("vkCmdEndRenderPass(cmd);")
    return_barrier = imgui_layer.find("ctx->RecordVulkanImGuiComputeOutputReturnTransition();", end_pass)
    end_cmd = imgui_layer.find("vkEndCommandBuffer(cmd);", end_pass)
    if end_pass < 0 or return_barrier < 0 or end_cmd < 0 or not end_pass < return_barrier < end_cmd:
        errors.append("ImGui records compute-output return barrier after render pass and before submit")
    return errors


def renderer_violations(renderer_cpp: str, renderer_header: str) -> list[str]:
    errors: list[str] = []
    if '#include "Platform/NVRHI/VulkanNova.h"' not in renderer_cpp:
        errors.append("Renderer3D includes VulkanNova")
    if "static std::unique_ptr<VulkanNova> s_NovaVk;" not in renderer_cpp:
        errors.append("Renderer3D owns a VulkanNova instance")

    init = method_body(renderer_cpp, "void Renderer3D::Init(")
    if not init or any(token not in init for token in (
        "RendererAPI::API::NVRHI_Vulkan", "std::make_unique<VulkanNova>()",
        "s_NovaVk->Init()", "s_NovaVk->Resize(", "s_NovaVk.reset();",
        "s_Data.RayTracingEnabled=false")):
        errors.append("VulkanNova init, sizing and raster fallback are wired")

    flush_backend = method_body(renderer_cpp, "static void FlushVulkanNova(")
    if not flush_backend or any(token not in flush_backend for token in (
        "n.Resize(", "SceneInstancesChanged()", "BuildAndFlattenBVH(",
        "n.Upload(0", "n.Upload(1", "n.Upload(2", "n.Set(\"u_InstanceCount\"",
        "n.Set(\"u_FrameIndex\"", "n.Set(\"u_CameraPosition\"",
        "n.Set(\"u_InverseViewProjection\"", "n.Set(\"u_ViewProjection\"",
        "n.Set(\"u_RenderScale\"", "n.Set(\"u_FogCount\"",
        "n.Set(\"u_CloudCount\"", "n.Set(\"u_ALightCount\"",
        "std::vector<Ref<Texture2D>> textures", "s_Data.TextureSlots",
        "RunNovaPassSchedule(n, textures);")):
        errors.append("Vulkan flush uploads scene state/uniforms and runs shared pass schedule")

    dx12_flush = method_body(renderer_cpp, "static void FlushDX12Nova(")
    if dx12_flush and flush_backend:
        set_pattern = re.compile(r'\bn\.Set\(\s*"([^"]+)"')
        dx12_uniforms = set(set_pattern.findall(dx12_flush))
        vk_uniforms = set(set_pattern.findall(flush_backend))
        upload_pattern = re.compile(r'\bn\.Upload\(\s*(\d+)')
        dx12_slots = set(upload_pattern.findall(dx12_flush))
        vk_slots = set(upload_pattern.findall(flush_backend))
        # Slot 3 is a deliberate DX12-only TerrainNovaVertex stream. Renderer3D's
        # DX12-only terrain guard prevents Vulkan from receiving terrain shape 8.
        if (dx12_uniforms != vk_uniforms or
                vk_slots != {"0", "1", "2"} or
                dx12_slots != {"0", "1", "2", "3"}):
            errors.append("Vulkan and DX12 Nova preserve uniform parity and expected structured-buffer slots")
    else:
        errors.append("DX12 reference flush exists for Vulkan upload parity")

    flush = method_body(renderer_cpp, "void Renderer3D::Flush(")
    if not flush or any(token not in flush for token in (
        "s_NovaVk && s_Data.RayTracingEnabled", "FlushVulkanNova();")):
        errors.append("active VulkanNova dispatch precedes raster fallback")

    shutdown = method_body(renderer_cpp, "void Renderer3D::Shutdown(")
    if not shutdown or "s_NovaVk.reset();" not in shutdown:
        errors.append("VulkanNova is destroyed before its NVRHI context")
    clear = method_body(renderer_cpp, "static void ClearAccumulationBuffers(")
    if not clear or "s_NovaVk->Clear();" not in clear:
        errors.append("VulkanNova history resets with renderer accumulation")
    resize = method_body(renderer_cpp, "void Renderer3D::ResizeRayTraceTarget(")
    if not resize or "s_NovaVk->Resize(" not in resize:
        errors.append("VulkanNova output follows viewport/render-scale resize")
    output = method_body(renderer_cpp, "void* Renderer3D::GetRayTraceTargetNativeSRV(")
    if not output or "if (s_NovaVk)" not in output or "s_NovaVk->Output()" not in output:
        errors.append("VulkanNova output is exposed through the renderer's native texture accessor")
    setter = method_body(renderer_cpp, "void Renderer3D::SetRayTracingEnabled(")
    if not setter or "s_NovaVk != nullptr" not in setter:
        errors.append("ray-tracing toggle recognizes only a constructed VulkanNova backend")
    if "GetRayTraceTargetNativeSRV()" not in renderer_header:
        errors.append("renderer header declares the native output accessor")
    return errors


def editor_violations(editor_cpp: str) -> list[str]:
    errors: list[str] = []
    if '#include "Platform/NVRHI/NVRHIFramebuffer.h"' not in editor_cpp:
        errors.append("editor includes the typed NVRHI framebuffer bridge")
    viewport = method_body(editor_cpp, "void EditorLayer::OnImGuiRender(")
    if not viewport or any(token not in viewport for token in (
        "Renderer3D::GetRayTraceTargetNativeSRV()", "RendererAPI::API::NVRHI_Vulkan",
        "dynamic_cast<NVRHIFramebuffer", "nvrhi::TextureHandle(",
        "GetExternalTextureImGuiTexID(")):
        errors.append("Vulkan viewport wraps the Nova output and requests the cached ImGui descriptor")
    return errors


def main() -> int:
    cpp = CPP_PATH.read_text(encoding="utf-8")
    header = HEADER_PATH.read_text(encoding="utf-8")
    context_cpp = CONTEXT_CPP_PATH.read_text(encoding="utf-8")
    context_header = CONTEXT_HEADER_PATH.read_text(encoding="utf-8")
    framebuffer_cpp = FRAMEBUFFER_CPP_PATH.read_text(encoding="utf-8")
    framebuffer_header = FRAMEBUFFER_HEADER_PATH.read_text(encoding="utf-8")
    imgui_layer = IMGUI_LAYER_PATH.read_text(encoding="utf-8")
    renderer_cpp = (ROOT / "Wasteland" / "src" / "Wasteland" / "Renderer" / "Renderer3D.cpp").read_text(encoding="utf-8")
    renderer_header = (ROOT / "Wasteland" / "src" / "Wasteland" / "Renderer" / "Renderer3D.h").read_text(encoding="utf-8")
    editor_cpp = (ROOT / "DemonCore-Editor" / "src" / "EditorLayer.cpp").read_text(encoding="utf-8")
    errors = violations(cpp, header)
    errors.extend(interop_violations(context_cpp, context_header, framebuffer_cpp,
                                     framebuffer_header, imgui_layer))
    errors.extend(renderer_violations(renderer_cpp, renderer_header))
    errors.extend(editor_violations(editor_cpp))
    for error in errors:
        print(f"FAIL {error}")
    if errors:
        print(f"RESULT: FAIL -- {len(errors)} contract violation(s)")
        return 1

    controls = [
        ("structured-buffer invalidation", "void VulkanNova::EnsureStructuredBuffer(",
         "m_BindingSetDirty = true;", "/* regression */"),
        ("zero-count dummy allocation", "void VulkanNova::Upload(",
         "count == 0 ? 1u", "count == 0 ? 0u"),
        ("initial image clear", "void VulkanNova::CreateImages(",
         "m_ImagesNeedClear = true;", "/* regression */"),
        ("storage-image usage", "void VulkanNova::CreateImages(",
         "desc.isUAV = true;", "desc.isUAV = false;"),
    ]
    for label, signature, old, new in controls:
        mutated = replace_in_body(cpp, signature, old, new)
        if mutated is None or not violations(mutated, header):
            print(f"FAIL negative control did not detect {label}")
            return 1
        print(f"PASS negative control catches {label}")

    interop_controls = [
        ("compute-to-UI old layout", context_cpp,
         "void NVRHIContext::RecordVulkanImGuiAttachmentTransition(",
         "? VK_IMAGE_LAYOUT_GENERAL", "? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL"),
        ("UI-to-compute new layout", context_cpp,
         "void NVRHIContext::RecordVulkanImGuiComputeOutputReturnTransition(",
         "barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;",
         "barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;"),
        ("descriptor idle wait", framebuffer_cpp,
         "bool NVRHIFramebuffer::ReleaseVulkanImGuiDescriptorSet(",
         "VkResult waitResult = vkDeviceWaitIdle(context->GetVkDevice());",
         "VkResult waitResult = VK_SUCCESS;"),
    ]
    for label, source, signature, old, new in interop_controls:
        mutated = replace_in_body(source, signature, old, new)
        if mutated is None or not interop_violations(
                mutated if source is context_cpp else context_cpp,
                context_header,
                mutated if source is framebuffer_cpp else framebuffer_cpp,
                framebuffer_header,
                imgui_layer):
            print(f"FAIL interop negative control did not detect {label}")
            return 1
        print(f"PASS interop negative control catches {label}")

    bad_imgui_order = imgui_layer.replace(
        "ctx->RecordVulkanImGuiComputeOutputReturnTransition();", "/* regression */", 1)
    if bad_imgui_order == imgui_layer or not interop_violations(
            context_cpp, context_header, framebuffer_cpp, framebuffer_header, bad_imgui_order):
        print("FAIL interop negative control did not detect missing return-barrier call")
        return 1
    print("PASS interop negative control catches missing return-barrier call")

    renderer_controls = [
        ("VulkanNova construction", renderer_cpp,
         "void Renderer3D::Init(", "std::make_unique<VulkanNova>()", "std::make_unique<DX11Nova>()"),
        ("VulkanNova history clear", renderer_cpp,
         "static void ClearAccumulationBuffers(", "s_NovaVk->Clear();", "s_Nova12->Clear();"),
        ("VulkanNova pass schedule", renderer_cpp,
         "static void FlushVulkanNova(", "RunNovaPassSchedule(n, textures);", "RunNovaPassSchedule(n, {});"),
        ("Vulkan uniform parity", renderer_cpp,
         "static void FlushVulkanNova(",
         'n.Set("u_SkyBottomColor",s_Data.SkyBottomColor);', "/* regression */"),
    ]
    for label, source, signature, old, new in renderer_controls:
        mutated = replace_in_body(source, signature, old, new)
        if mutated is None or not renderer_violations(mutated, renderer_header):
            print(f"FAIL renderer negative control did not detect {label}")
            return 1
        print(f"PASS renderer negative control catches {label}")

    bad_editor_route = replace_in_body(
        editor_cpp, "void EditorLayer::OnImGuiRender(",
        "GetExternalTextureImGuiTexID(", "GetColorAttachmentImGuiTexID(")
    if bad_editor_route is None or not editor_violations(bad_editor_route):
        print("FAIL editor negative control did not detect bypassed external-texture descriptor path")
        return 1
    print("PASS editor negative control catches bypassed external-texture descriptor path")

    total_controls = len(controls) + len(interop_controls) + 1
    total_controls += len(renderer_controls)
    total_controls += 1
    print("PASS: 12 VulkanNova, 9 interop, 11 renderer-runtime and 2 editor-route contracts")
    print(f"RESULT: PASS -- {total_controls}/{total_controls} negative controls provably failable")
    print("This guards source structure only; Vulkan runtime/GPU correctness is untested.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
