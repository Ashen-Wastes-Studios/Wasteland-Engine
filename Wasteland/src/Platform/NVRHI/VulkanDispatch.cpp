#include "wlpch.h"

// INTENTIONALLY EMPTY.
//
// The vulkan.hpp dispatch-loader storage used to be defined here. It now lives in
// nvrhi's own library (vendor/nvrhi/src/vulkan/nvrhi-dispatch-storage.cpp), because
// the layout of that object is fixed at the point vulkan.hpp is included and the two
// modules did not agree on it: same header (352), same macros, same process, yet the
// engine saw a 6688-byte table with vkCreateSemaphore at offset 336 while nvrhi saw
// 6672 bytes at offset 320. One object read through two layouts means every device
// call from the backend lands on the wrong function.
//
// Do NOT define VULKAN_HPP_DEFAULT_DISPATCHER_STORAGE here, and do not call
// VULKAN_HPP_DEFAULT_DISPATCHER.init() from engine code: the backend initialises its
// own table inside createDevice, and engine code reaches the driver through the plain
// ::vk* entry points.
