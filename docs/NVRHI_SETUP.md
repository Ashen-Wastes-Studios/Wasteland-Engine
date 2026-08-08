# NVRHI Integration Guide

This document describes how to build and integrate NVRHI (NVIDIA Rendering Hardware Interface) with the Wasteland engine.

## Overview

NVRHI provides DirectX 11, DirectX 12, and Vulkan backends alongside the existing OpenGL implementation. The engine now supports runtime API switching between:
- **OpenGL** (existing)
- **DirectX 11** (via NVRHI)
- **DirectX 12** (via NVRHI)
- **Vulkan** (via NVRHI)

## Prerequisites

- **Windows SDK** (for DirectX 11/12)
- **Vulkan SDK** (for Vulkan backend) - Download from https://vulkan.lunarg.com/sdk/home
- **CMake** 3.16 or higher (for building NVRHI)
- **Visual Studio 2022** with C++ tools

## Building NVRHI

NVRHI uses CMake and must be built separately before building the main project.

### Step 1: Configure NVRHI with CMake

Open a command prompt in the NVRHI directory:

```bash
cd Wasteland\vendor\nvrhi
mkdir build
cd build
```

#### For Visual Studio 2022:

```bash
cmake .. -G "Visual Studio 17 2022" -A x64
```

#### Optional CMake flags:

- `-DNVRHI_BUILD_D3D11=ON` (default: ON) - Enable DirectX 11 backend
- `-DNVRHI_BUILD_D3D12=ON` (default: ON) - Enable DirectX 12 backend
- `-DNVRHI_BUILD_VULKAN=ON` (default: ON) - Enable Vulkan backend
- `-DNVRHI_BUILD_VALIDATION=ON` (default: ON) - Enable validation layer
- `-DNVRHI_WITH_DXCOMPILER=ON` (default: ON) - Enable DXC shader compiler

Example with all backends enabled:

```bash
cmake .. -G "Visual Studio 17 2022" -A x64 -DNVRHI_BUILD_D3D11=ON -DNVRHI_BUILD_D3D12=ON -DNVRHI_BUILD_VULKAN=ON
```

### Step 2: Build NVRHI

#### Release build:

```bash
cmake --build . --config Release
```

#### Debug build:

```bash
cmake --build . --config Debug
```

This will create static libraries in:
- `build/src/Release/nvrhi_d3d11.lib`
- `build/src/Release/nvrhi_d3d12.lib`
- `build/src/Release/nvrhi_vulkan.lib`
- `build/src/Release/nvrhi_common.lib`
- `build/src/Release/nvrhi_validation.lib`

(Or in `Debug/` for debug builds)

## Linking NVRHI Libraries

After building NVRHI, you need to link the libraries in `premake5.lua`.

### Add Library Paths

Find the `libdirs` section in the Wasteland project and add:

```lua
libdirs
{
    pythonpath .. "/libs",
    "Wasteland/vendor/nvrhi/build/src/%{cfg.buildcfg}"  -- Add this line
}
```

### Add Library Links

Find the `links` section and add the NVRHI libraries:

```lua
links
{
    "GLFW",
    "Glad",
    "ImGui",
    "yaml-cpp",
    "Box2D",
    "Box3D",
    "python314",
    -- NVRHI libraries
    "nvrhi_common",
    "nvrhi_d3d11",
    "nvrhi_d3d12",
    "nvrhi_vulkan",
    "nvrhi_validation",
    -- Windows system libraries for DirectX
    "d3d11",
    "d3d12",
    "dxgi",
    "d3dcompiler",
    -- Vulkan (if SDK is installed)
    "vulkan-1"
}
```

### Add Defines

Add NVRHI defines to enable the backends:

```lua
defines
{
    "_CRT_SECURE_NO_WARNINGS",
    "YAML_CPP_STATIC_DEFINE",
    "NVRHI_D3D11",      -- Enable D3D11 backend
    "NVRHI_D3D12",      -- Enable D3D12 backend
    "NVRHI_VULKAN"      -- Enable Vulkan backend
}
```

## Using the API

### Switching Rendering APIs at Runtime

```cpp
#include "Wasteland/Renderer/RenderCommand.h"

// Switch to DirectX 12
Wasteland::RenderCommand::SetAPI(Wasteland::RendererAPI::API::NVRHI_DX12);
Wasteland::RenderCommand::Init();

// Switch to Vulkan
Wasteland::RenderCommand::SetAPI(Wasteland::RendererAPI::API::NVRHI_Vulkan);
Wasteland::RenderCommand::Init();

// Switch back to OpenGL
Wasteland::RenderCommand::SetAPI(Wasteland::RendererAPI::API::OpenGL);
Wasteland::RenderCommand::Init();
```

### Querying Current API

```cpp
auto currentAPI = Wasteland::RendererAPI::GetAPI();
if (currentAPI == Wasteland::RendererAPI::API::NVRHI_DX12)
{
    // DirectX 12 specific code
}
```

## Implementation Status

### Phase 1: Foundation ✅
- [x] NVRHI submodule added
- [x] RendererAPI enum extended
- [x] NVRHIRendererAPI stub created
- [x] RenderCommand::SetAPI() implemented
- [x] premake5.lua updated with include paths

### Phase 2: Core Rendering ✅
- [ ] Implement NVRHIContext (device creation, swapchain)
- [ ] Implement NVRHIBuffer
- [ ] Implement NVRHITexture
- [ ] Implement NVRHIShader
- [ ] Implement NVRHIVertexArray
- [ ] Implement NVRHIFramebuffer
- [ ] Convert basic shaders to HLSL
- [ ] Get Renderer2D working

### Phase 3: Advanced Features ✅
- [ ] Compute shader support
- [ ] Convert NovaRenderer to HLSL
- [ ] Port Renderer3D
- [ ] SSBO/structured buffer support
- [ ] Image load/store/UAV support

### Phase 4: API Switching & Polish ✅
- [ ] Editor UI dropdown for API selection
- [ ] Persistent API selection (config file)
- [ ] Runtime API switching with state preservation
- [ ] Testing and bug fixes

## Known Limitations

1. **Window Context**: The current GLFW window creates an OpenGL context. For NVRHI backends, we need to create a window without OpenGL context and let NVRHI manage the swapchain. This requires modifications to `WindowsWindow.cpp`.

2. **Shader Compatibility**: OpenGL shaders (GLSL) are not compatible with NVRHI backends. HLSL shaders must be written for DX11/DX12, and optionally SPIR-V for Vulkan.

3. **Compute Shaders**: The NovaRenderer compute shader is complex and will require significant work to port to HLSL/DX11/DX12/Vulkan.

4. **Line Width**: DirectX does not support variable line width. Only Vulkan supports this feature.

## Troubleshooting

### Build Errors

**Error**: `Cannot open include file: 'nvrhi/nvrhi.h'`
- **Solution**: Make sure NVRHI include directory is in `premake5.lua` includedirs

**Error**: `Cannot open library 'nvrhi_d3d11.lib'`
- **Solution**: Build NVRHI first using CMake (see Building NVRHI section)

**Error**: `LNK2019: unresolved external symbol` for NVRHI functions
- **Solution**: Add NVRHI libraries to the `links` section in `premake5.lua`

### Runtime Errors

**Error**: `Failed to create D3D12 device`
- **Solution**: Ensure your GPU supports DirectX 12 and drivers are up to date

**Error**: `Vulkan instance creation failed`
- **Solution**: Install Vulkan SDK and ensure Vulkan runtime is installed

## Next Steps

1. Build NVRHI following the instructions above
2. Update `premake5.lua` with library paths and links
3. Regenerate Visual Studio solution: `premake5 vs2022`
4. Build the Wasteland project
5. Test with OpenGL first (should still work)
6. Implement NVRHIContext for device creation
7. Test basic NVRHI initialization

## Resources

- NVRHI GitHub: https://github.com/NVIDIAGameWorks/nvrhi
- NVRHI Documentation: https://github.com/NVIDIAGameWorks/nvrhi/blob/main/doc/README.md
- NVRHI Samples: https://github.com/NVIDIAGameWorks/nvrhi-samples
