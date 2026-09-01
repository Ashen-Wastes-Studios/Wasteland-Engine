# NVRHI Integration Guide

This document describes how to build and integrate NVRHI (NVIDIA Rendering Hardware Interface) with the Wasteland engine.

## Overview

NVRHI provides DirectX 11, DirectX 12, and Vulkan backends alongside the existing OpenGL implementation. The engine now supports runtime API switching between:

- **OpenGL** (existing)
- **DirectX 11** (via NVRHI)
- **DirectX 12** (via NVRHI)
- **Vulkan** (via NVRHI)

## Prerequisites

- **Windows SDK** (for DirectX 11/12) — latest, with `d3d11.lib`, `d3d12.lib`, `dxgi.lib`, `dxguid.lib`
- **Vulkan SDK 1.3.296.0** — Download from https://vulkan.lunarg.com/sdk/home (engine is pinned to this SDK; `premake5.lua` hard-codes `C:/VulkanSDK/1.3.296.0/Include` and `Lib`). Newer SDK minor versions work but require updating `IncludeDir["VulkanSDK"]` and `libdirs`.
- **CMake** 3.16+ (for building NVRHI)
- **Visual Studio 2022** toolset `v145` (`Wasteland.slnx` / `premake5.lua` `toolset "v145"`)

## Building NVRHI

NVRHI uses CMake and must be built separately before building the main project. It is consumed as a **static library** (`NVRHI_BUILD_SHARED=OFF`).

### Step 1: Configure NVRHI with CMake

```bash
cd Wasteland\vendor\nvrhi
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
```

Optional flags (all ON by default in this repo):

- `-DNVRHI_BUILD_D3D11=ON`
- `-DNVRHI_BUILD_D3D12=ON`
- `-DNVRHI_BUILD_VULKAN=ON`
- `-DNVRHI_WITH_DXCOMPILER=ON`
- `-DNVRHI_WITH_VALIDATION=ON`

### Step 2: Build NVRHI — both configs

> `premake5.lua` expects `Wasteland/vendor/nvrhi/build/%{cfg.buildcfg}/nvrhi*.lib`. Build **both** Release and Debug or the matching Wasteland config will fail to link.

```bash
cmake --build . --config Release
cmake --build . --config Debug
```

Outputs:

- `Wasteland/vendor/nvrhi/build/Release/nvrhi.lib`
- `Wasteland/vendor/nvrhi/build/Release/nvrhi_d3d11.lib`
- `Wasteland/vendor/nvrhi/build/Release/nvrhi_d3d12.lib`
- `Wasteland/vendor/nvrhi/build/Release/nvrhi_vk.lib`
- `Wasteland/vendor/nvrhi/build/Debug/nvrhi*.lib` (same set)

DirectX headers and Vulkan headers are fetched as CMake deps:

- `build/_deps/directx_headers-src/include`
- `build/_deps/vulkan_headers-src/include` (may differ from `C:/VulkanSDK/1.3.296.0/Include`; see Dispatcher note below)

### Step 3: Regenerate and build Wasteland

```bash
premake5 vs2022
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Wasteland.slnx -p:Configuration=Release -p:Platform=x64 -m
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Wasteland.slnx -p:Configuration=Debug   -p:Platform=x64 -m
```

Verify with real MSBuild output — not a dry run. Both configs must succeed.

## Linking NVRHI Libraries (premake5.lua)

The committed `premake5.lua` already wires this. For reference:

### Include dirs

```lua
IncludeDir["NVRHI"]          = "Wasteland/vendor/nvrhi/include"
IncludeDir["DirectXHeaders"] = "Wasteland/vendor/nvrhi/build/_deps/directx_headers-src/include"
IncludeDir["VulkanHeaders"]  = "Wasteland/vendor/nvrhi/build/_deps/vulkan_headers-src/include"
IncludeDir["VulkanSDK"]      = "C:/VulkanSDK/1.3.296.0/Include"
```

Order matters for `vulkan.hpp` — `VulkanHeaders` (deps) and `VulkanSDK` both provide `vulkan/vulkan.hpp` but may be different patch versions. The engine defines `VULKAN_HPP_DISPATCH_LOADER_DYNAMIC` in exactly one TU (see below).

### Lib dirs

```lua
libdirs
{
    pythonpath .. "/libs",
    "Wasteland/vendor/nvrhi/build/%{cfg.buildcfg}",
    "C:/VulkanSDK/1.3.296.0/Lib"
}
```

### Links (Wasteland StaticLib)

```lua
links { "GLFW", "Glad", "ImGui", "yaml-cpp", "Box2D", "Box3D", "python314",
        "nvrhi", "nvrhi_d3d11", "nvrhi_d3d12", "nvrhi_vk" }

filter "system:windows"
    links { "opengl32.lib", "d3d11.lib", "d3d12.lib", "dxgi.lib",
            "d3dcompiler.lib", "dxguid.lib", "vulkan-1.lib", "volk.lib" }
```

> Do not add `nvrhi_common` / `nvrhi_validation` — recent NVRHI builds emit the unified `nvrhi*.lib` set above. The old guide's `build/src/...` path is stale.

## Vulkan Backend — Critical Integration Notes

These are not optional. Missing any one reproduces the `nvoglv64.dll` crash seen in `Queue::Queue`.

### 1) Dynamic dispatcher storage — exactly one TU

`Wasteland/src/Platform/NVRHI/NVRHIContext.cpp` enables `VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1` and includes `vulkan/vulkan.hpp` **without** defining storage. The storage lives in:

```
Wasteland/src/Platform/NVRHI/VulkanDispatch.cpp  →  VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
```

Do **not** add `VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE` to `NVRHIContext.cpp` — duplicate storage creates two `VULKAN_HPP_DEFAULT_DISPATCHER` globals. `NVRHIContext::CreateDeviceVulkan()` would then init one while `nvrhi::vulkan::Queue::Queue` dispatches through the other (null), crashing in `nvoglv64.dll` at `Queue::Queue → createSemaphore`.

### 2) Static-lib dispatcher init (vendor patch)

`Wasteland/vendor/nvrhi/src/vulkan/vulkan-device.cpp` upstream only inits the dispatcher when `NVRHI_SHARED_LIBRARY_BUILD` is defined:

```cpp
#if defined(NVRHI_SHARED_LIBRARY_BUILD)
    VULKAN_HPP_DEFAULT_DISPATCHER.init(instance, proc, device);
#endif
```

This repo builds NVRHI as **STATIC** (`NVRHI_BUILD_SHARED=OFF`), so the gate left NVRHI's TU dispatch zero-initialised. `Queue` then called `context.device.createSemaphore(... timeline ...)` through a null dispatch into `nvoglv64.dll`.

**Required patch** (`vendor/nvrhi/src/vulkan/vulkan-device.cpp:38`):

```cpp
DeviceHandle createDevice(const DeviceDesc& desc)
{
#if defined(NVRHI_SHARED_LIBRARY_BUILD)
    // shared path: load via DynamicLoader
    VULKAN_HPP_DEFAULT_DISPATCHER.init(desc.instance, proc, desc.device);
#else
    // Static build: NVRHI's TU may have a separate DispatchLoaderDynamic
    // instance when built against a different vulkan_headers copy. Always init.
    VULKAN_HPP_DEFAULT_DISPATCHER.init(desc.instance, vkGetInstanceProcAddr, desc.device);
#endif
    Device* device = new Device(desc);
    return DeviceHandle::Create(device);
}
```

Rebuild NVRHI after patching (`cmake --build . --config Release` and `Debug`), then rebuild Wasteland. This patch is the fix for the stack:

```
nvoglv64.dll!00007ffa52644fe5()
nvrhi::vulkan::Queue::Queue(..., vk::Queue, uint32_t)
nvrhi::vulkan::Device::Device(DeviceDesc const&)
nvrhi::vulkan::createDevice(DeviceDesc const&)
Wasteland::NVRHIContext::CreateDeviceVulkan() Line 875
Wasteland::NVRHIContext::Init() Line 172
Wasteland::WindowsWindow::ExecuteSwitch()
```

`Wasteland/src/Platform/NVRHI/NVRHIContext.cpp:851` also does `VULKAN_HPP_DEFAULT_DISPATCHER.init(m_VkInstance, vkGetInstanceProcAddr, m_VkDevice)` before `nvrhi::vulkan::createDevice` — keep both inits; they target the same global when headers match and the vendor TU when they don't.

### 3) Timeline semaphores — core 1.2 vs KHR extension

NVRHI's Vulkan backend **requires** timeline semaphores (`Queue::Queue` creates a `vk::SemaphoreType::eTimeline` tracking semaphore on every queue). On Vulkan 1.2+ this is core (`VkPhysicalDeviceVulkan12Features::timelineSemaphore`). On 1.1 it is `VK_KHR_timeline_semaphore` + `VkPhysicalDeviceTimelineSemaphoreFeatures`.

Common mistake: chaining **both** structs in `VkDeviceCreateInfo::pNext` triggers:

```
VUID-VkDeviceCreateInfo-pNext-02830: pNext chain includes VkPhysicalDeviceVulkan12Features,
then it must not include VkPhysicalDeviceTimelineSemaphoreFeatures
```

Seen on driver `591.59` / `NVIDIA GeForce RTX 3060 Ti apiVersion 1.4.325` with validation layers enabled. `vkCreateDevice` may still return `VK_SUCCESS` but the validation error is a spec violation and must be fixed.

**Correct logic** in `NVRHIContext::CreateVulkanDevice()`:

```cpp
VkPhysicalDeviceVulkan12Features vulkan12Query{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
VkPhysicalDeviceTimelineSemaphoreFeatures timelineQueryKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
vulkan12Query.pNext = &timelineQueryKHR;
VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &vulkan12Query };
vkGetPhysicalDeviceFeatures2(phys, &f2);

bool via12  = vulkan12Query.timelineSemaphore == VK_TRUE;
bool viaKHR = timelineQueryKHR.timelineSemaphore == VK_TRUE;

if (via12) {
    VkPhysicalDeviceVulkan12Features enabled12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    enabled12.timelineSemaphore = VK_TRUE;
    // Do NOT push VK_KHR_timeline_semaphore and do NOT chain the KHR struct
    createInfo.pNext = &enabled12;
} else if (viaKHR) {
    VkPhysicalDeviceTimelineSemaphoreFeatures enabledKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    enabledKHR.timelineSemaphore = VK_TRUE;
    deviceExtensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    createInfo.pNext = &enabledKHR;
}
```

Detection must query `VkPhysicalDeviceVulkan12Features` first; only fall back to KHR enumeration on 1.1 drivers. On `1.4.325` both queries return true — prefer the core path.

### 4) Window creation for Vulkan

GLFW creates an OpenGL context by default. For Vulkan the window must be created with **no client API**:

```cpp
glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
glfwCreateWindow(...);
```

`WindowsWindow::ExecuteSwitch()` recreates the GLFW window when switching between `GL` and `NO_API` targets. The ImGui backend is shut down before recreate (`ImGui: Shutting down backend for window recreate`) and re-initialised after (`NVRHI Context initialized successfully`).

### 5) NVRHI DeviceDesc — no presentQueue

`nvrhi::vulkan::DeviceDesc` has `graphicsQueue` / `computeQueue` / `transferQueue` (+ indices) but **no** `presentQueue`. Present is handled by the Vulkan swapchain, not NVRHI. Passing a `presentQueue` member is a stale API and will not compile.

### 6) DXGI swapchain

All NVRHI backends (DX11/D3D12/Vulkan window surface) are unified on `IDXGISwapChain3` for backbuffer retrieval. Do not mix `IDXGISwapChain` / `IDXGISwapChain1` paths.

## Using the API

### Switching Rendering APIs at Runtime

```cpp
#include "Wasteland/Renderer/RenderCommand.h"

Wasteland::RenderCommand::SetAPI(Wasteland::RendererAPI::API::NVRHI_DX12);
Wasteland::RenderCommand::Init();

Wasteland::RenderCommand::SetAPI(Wasteland::RendererAPI::API::NVRHI_Vulkan);
Wasteland::RenderCommand::Init();

Wasteland::RenderCommand::SetAPI(Wasteland::RendererAPI::API::OpenGL);
Wasteland::RenderCommand::Init();
```

### Querying Current API

```cpp
auto api = Wasteland::RendererAPI::GetAPI();
if (api == Wasteland::RendererAPI::API::NVRHI_Vulkan) { /* ... */ }
```

## Implementation Status

### Phase 1: Foundation ✅
- [x] NVRHI submodule added
- [x] RendererAPI enum extended
- [x] NVRHIRendererAPI stub created
- [x] RenderCommand::SetAPI() implemented
- [x] premake5.lua updated with include paths

### Phase 2: Core Rendering ✅
- [x] NVRHIContext (device creation, swapchain for DX11/DX12/Vulkan)
- [x] NVRHIBuffer, NVRHITexture, NVRHIShader (HLSL, reflection)
- [x] NVRHIVertexArray, NVRHIFramebuffer, pipeline caching
- [x] HLSL shaders (`FlatColor.hlsl`, `Renderer2D_*.hlsl`, `Renderer3D_Basic.hlsl`)

### Phase 3: Advanced Features (In Progress)
- [x] Compute dispatch & SSBO (`NVRHIStructuredBuffer`)
- [x] NovaRenderer HLSL
- [ ] Renderer3D full pipeline, image load/store/UAV

### Phase 4: API Switching & Polish (In Progress)
- [x] Editor UI dropdown, ImGuiLayer hooks
- [x] Vulkan backend with framebuffer recreation on switch
- [x] Window recreate (GL ↔ NO_API) and swapchain via IDXGISwapChain3
- [ ] Persistent API selection (config)
- [ ] State preservation across switches

## Known Limitations

1. **Shader compatibility** — GLSL ≠ HLSL/SPIR-V. Vulkan via NVRHI consumes HLSL (compiled with DXC) or SPIR-V.
2. **Line width** — variable width only on Vulkan.
3. **Compute** — NovaRenderer port is ongoing.

## Troubleshooting

### Build Errors

**`Cannot open include file: 'nvrhi/nvrhi.h'`**  
Add `IncludeDir["NVRHI"]` to `includedirs` and ensure `vendor/nvrhi/include` exists (submodule).

**`Cannot open library 'nvrhi_*.lib'`**  
Build NVRHI first (both `Release` and `Debug`). Check that `Wasteland/vendor/nvrhi/build/<Config>/nvrhi*.lib` exists — not `build/src/...`.

**`LNK2019` for NVRHI symbols**  
Add `nvrhi`, `nvrhi_d3d11`, `nvrhi_d3d12`, `nvrhi_vk` to `links` and `Wasteland/vendor/nvrhi/build/%{cfg.buildcfg}` to `libdirs`.

**`premake5.lua` corruption after edit**  
Premake is sensitive to stray UTF-8/BOM edits. If `premake5 vs2022` fails, `git checkout -- premake5.lua` and re-apply the minimal patch.

### Runtime Errors

**`Failed to create D3D12 device`**  
GPU/driver must support D3D12. Try without debug layer (`WL_DEBUG` off).

**`Vulkan instance creation failed`**  
Install Vulkan SDK 1.3.296.0 and ensure `vulkan-1.dll` is on PATH. Check `vkEnumerateInstanceExtensionProperties` for `VK_KHR_surface` / `VK_KHR_win32_surface` from GLFW.

**`nvoglv64.dll` crash in `nvrhi::vulkan::Queue::Queue` → `createSemaphore`**

Stack signature:

```
nvoglv64.dll!00007ffa52644fe5()
nvoglv64.dll!00007ffa52ae8720()
nvrhi::vulkan::Queue::Queue(VulkanContext const&, CommandQueue, vk::Queue, uint32_t)
nvrhi::vulkan::Device::Device(DeviceDesc const&)
nvrhi::vulkan::createDevice(DeviceDesc const&)
Wasteland::NVRHIContext::CreateDeviceVulkan() Line 865
Wasteland::NVRHIContext::Init() Line 172
Wasteland::WindowsWindow::ExecuteSwitch()
```

Causes and fixes:

1. **Null dispatch** — see Dispatcher notes above. Apply the `vulkan-device.cpp` static-lib patch and ensure `VulkanDispatch.cpp` is the sole storage TU. Rebuild **both** NVRHI configs and Wasteland.
2. **Missing timeline semaphore feature** — device was created without `timelineSemaphore = VK_TRUE`. Fix the `pNext` chain per Timeline Semaphores note. With validation layers (`WL_DEBUG`), the diagnostic log should show `Query Vulkan12.timelineSemaphore=true` and `Enabling timeline semaphores via VkPhysicalDeviceVulkan12Features (core 1.2)` on 1.4 drivers.
3. **Wrong `vulkan.hpp` copy** — NVRHI's `_deps/vulkan_headers` vs `VulkanSDK` mismatch can give two different `DispatchLoaderDynamic` types. Keep both SDK and deps on 1.3.x and rebuild after any SDK upgrade.

**`VUID-VkDeviceCreateInfo-pNext-02830` validation error**  
You chained both `VkPhysicalDeviceVulkan12Features` and `VkPhysicalDeviceTimelineSemaphoreFeatures`. Use only one per the Timeline Semaphores note.

**`LNK1104: cannot open ...DemonCore-Editor.exe`**  
The previous run is still attached to `VsDebugConsole` (Visual Studio Code debugger) and did not exit after the `nvogl` crash. `taskkill /F /PID <pid>` returns "no running instance" while `tasklist` still shows it. Kill the parent `VsDebugConsole` or reboot; `psexec -s taskkill /F /PID <pid>` as SYSTEM also works. Until then `Release` is still the old crashing binary — test `Debug` (`bin/Debug-windows-x86_64/DemonCore-Editor/DemonCore-Editor.exe`) which rebuilt cleanly.

**Vulkan validation layers not found**  
`VK_LAYER_KHRONOS_validation` comes from the Vulkan SDK. In `WL_DEBUG` the engine enables it and `VK_EXT_debug_utils`. Install the SDK or run `Release`.

## Next Steps

1. Build NVRHI (both configs) per Building NVRHI
2. `premake5 vs2022`, then MSBuild `Release` + `Debug`
3. Run `Debug` first with `WL_DEBUG` validation layers to catch `pNext` / dispatch errors
4. Use Editor UI dropdown to switch APIs; check `WastelandProfile-*.json` for frame timing
5. Implement remaining Renderer3D pipeline and persistent API selection

## Resources

- NVRHI GitHub: https://github.com/NVIDIAGameWorks/nvrhi
- NVRHI docs: https://github.com/NVIDIAGameWorks/nvrhi/blob/main/doc/README.md
- Vulkan spec — `VUID-VkDeviceCreateInfo-pNext-02830`: https://vulkan.lunarg.com/doc/view/1.3.296.0/windows/1.3-extensions/vkspec.html#VUID-VkDeviceCreateInfo-pNext-02830
