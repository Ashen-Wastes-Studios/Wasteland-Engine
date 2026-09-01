#include "wlpch.h"
// This file provides the Vulkan-Hpp default dispatch loader storage.
// NVRHI's vulkan backend requires VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
// and exactly one translation unit must define the dynamic dispatcher.
// See vulkan_hpp_macros.hpp: VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

// Instantiate the storage for the default dynamic dispatch loader
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
