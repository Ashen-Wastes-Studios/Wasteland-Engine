// NOTE: no trailing newline on this line in the source -- the only
// file in the batch like that, which is why the closing brace below has
// no leading newline either.
#include "wlpch.h"
// This file provides the Vulkan-Hpp default dispatch loader storage.
// NVRHI's vulkan backend requires VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
// and exactly one translation unit must define the dynamic dispatcher.
// DEFECT (stale comment): it says "exactly one translation unit must
// define the dynamic dispatcher", then this file does precisely that
// -- and NVRHITexture.cpp / NVRHIBuffer.cpp / NVRHIShader.h all reach
// NVRHIContext.h, which almost certainly includes vulkan.hpp too. This
// holds only as long as none of them defines the macro; it is the kind
// of invariant a build break should surface immediately, not a comment.
// See vulkan_hpp_macros.hpp: VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
// MUST precede the vulkan.hpp include: the macro changes vulkan.hpp's
// declarations, so including it first would silently compile without
// dynamic dispatch and fail at link or at runtime with an unloaded
// function pointer. Hence the odd include order in this file.
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

// Instantiate the storage for the default dynamic dispatch loader
// A macro that expands to the storage definition, at namespace scope,
// with no trailing semicolon in the source -- the macro supplies its
// own punctuation. DEFECT (ignored result): a loader-init failure is
// invisible here; nothing checks that the dispatch table was
// populated before the first Vulkan call.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
