#pragma once

// Vulkan call tracer -- a stand-in for a debugger on a machine where none can be
// installed.
//
// WHY THIS EXISTS
// nvrhi::vulkan::createDevice faults inside nvoglv64.dll (access violation) while
// Wasteland's own translation unit can create and destroy an identical timeline
// semaphore on the same device, through the same dispatcher slot, microseconds
// earlier. A `fprintf` placed as the first statement of the faulting constructor
// body never printed, so the fault is either before that body or the symbol name
// is an artefact of /OPT:ICF folding -- i.e. the crash-report frame is not
// trustworthy. What is left to learn is WHICH driver entry point is being entered
// and WITH WHICH ARGUMENTS.
//
// So: replace the interesting entries of the default dispatch table with logging
// trampolines that print their arguments and then tail-call the real function.
// The last line printed before the crash is the call that died, with its actual
// arguments -- including the handles and any pNext chain.
//
// Gated behind WL_VK_TRACE=1. Off by default: the wrappers defeat the driver's
// own call-direct optimisations and print to stderr on every call.

#if defined(WL_PLATFORM_WINDOWS)

#include <vulkan/vulkan.h>
#include <vulkan/vulkan.hpp>

#include <cstdio>
#include <cstdlib>

namespace Wasteland::VulkanTrace
{
	inline bool Enabled()
	{
		static const bool enabled = []()
		{
			const char *v = std::getenv("WL_VK_TRACE");
			return v && v[0] && v[0] != '0';
		}();
		return enabled;
	}

	inline void Log(const char *what, const void *a = nullptr, const void *b = nullptr,
		const void *c = nullptr, const void *d = nullptr)
	{
		if (!Enabled())
			return;
		// Unbuffered: the process dies inside the next driver call, so anything
		// still sitting in a stdio buffer is lost.
		std::fprintf(stderr, "VKTRACE %-32s %p %p %p %p\n", what, a, b, c, d);
		std::fflush(stderr);
	}

	// The original entry points, captured before we overwrite the table.
	inline PFN_vkCreateSemaphore pCreateSemaphore = nullptr;
	inline PFN_vkDestroySemaphore pDestroySemaphore = nullptr;
	inline PFN_vkCreateCommandPool pCreateCommandPool = nullptr;
	inline PFN_vkAllocateCommandBuffers pAllocateCommandBuffers = nullptr;
	inline PFN_vkGetDeviceQueue pGetDeviceQueue = nullptr;
	inline PFN_vkGetPhysicalDeviceMemoryProperties2 pGetPhysicalDeviceMemoryProperties2 = nullptr;
	inline PFN_vkCreateDescriptorPool pCreateDescriptorPool = nullptr;
	inline PFN_vkCreateBuffer pCreateBuffer = nullptr;
	inline PFN_vkCreateFence pCreateFence = nullptr;
	inline PFN_vkCreateQueryPool pCreateQueryPool = nullptr;
	inline PFN_vkGetPhysicalDeviceFeatures2 pGetPhysicalDeviceFeatures2 = nullptr;
	inline PFN_vkCreatePipelineCache pCreatePipelineCache = nullptr;
	inline PFN_vkCreateDescriptorSetLayout pCreateDescriptorSetLayout = nullptr;
	inline PFN_vkCreatePipelineLayout pCreatePipelineLayout = nullptr;
	inline PFN_vkCreateSampler pCreateSampler = nullptr;

	inline VKAPI_ATTR VkResult VKAPI_CALL TraceCreateSemaphore(
		VkDevice device, const VkSemaphoreCreateInfo *ci,
		const VkAllocationCallbacks *alloc, VkSemaphore *out)
	{
		const VkSemaphoreTypeCreateInfo *t = nullptr;
		if (ci && ci->pNext)
		{
			const auto *base = static_cast<const VkBaseInStructure *>(ci->pNext);
			if (base->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO)
				t = reinterpret_cast<const VkSemaphoreTypeCreateInfo *>(base);
		}
		Log("vkCreateSemaphore", device, ci, alloc, out);
		if (t)
			Log("  -> timeline type", reinterpret_cast<const void *>(static_cast<uintptr_t>(t->semaphoreType)),
				reinterpret_cast<const void *>(static_cast<uintptr_t>(t->initialValue)));
		return pCreateSemaphore(device, ci, alloc, out);
	}

	inline VKAPI_ATTR void VKAPI_CALL TraceDestroySemaphore(
		VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *alloc)
	{
		Log("vkDestroySemaphore", device, semaphore, alloc);
		pDestroySemaphore(device, semaphore, alloc);
	}

	inline VKAPI_ATTR VkResult VKAPI_CALL TraceCreateCommandPool(
		VkDevice device, const VkCommandPoolCreateInfo *ci,
		const VkAllocationCallbacks *alloc, VkCommandPool *out)
	{
		Log("vkCreateCommandPool", device, ci, alloc, out);
		return pCreateCommandPool(device, ci, alloc, out);
	}

	inline VKAPI_ATTR VkResult VKAPI_CALL TraceAllocateCommandBuffers(
		VkDevice device, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *out)
	{
		Log("vkAllocateCommandBuffers", device, ai, out);
		return pAllocateCommandBuffers(device, ai, out);
	}

	inline VKAPI_ATTR void VKAPI_CALL TraceGetDeviceQueue(
		VkDevice device, uint32_t family, uint32_t index, VkQueue *out)
	{
		Log("vkGetDeviceQueue", device, reinterpret_cast<const void *>(static_cast<uintptr_t>(family)),
			reinterpret_cast<const void *>(static_cast<uintptr_t>(index)), out);
		pGetDeviceQueue(device, family, index, out);
	}

	inline VKAPI_ATTR void VKAPI_CALL TraceGetPhysicalDeviceMemoryProperties2(
		VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2 *props)
	{
		Log("vkGetPhysDevMemoryProperties2", physicalDevice, props);
		pGetPhysicalDeviceMemoryProperties2(physicalDevice, props);
	}

	inline VKAPI_ATTR VkResult VKAPI_CALL TraceCreateDescriptorPool(
		VkDevice device, const VkDescriptorPoolCreateInfo *ci,
		const VkAllocationCallbacks *alloc, VkDescriptorPool *out)
	{
		Log("vkCreateDescriptorPool", device, ci, alloc, out);
		return pCreateDescriptorPool(device, ci, alloc, out);
	}

	inline VKAPI_ATTR VkResult VKAPI_CALL TraceCreateBuffer(
		VkDevice device, const VkBufferCreateInfo *ci,
		const VkAllocationCallbacks *alloc, VkBuffer *out)
	{
		Log("vkCreateBuffer", device, ci, alloc, out);
		return pCreateBuffer(device, ci, alloc, out);
	}

	inline VKAPI_ATTR VkResult VKAPI_CALL TraceCreateFence(
		VkDevice device, const VkFenceCreateInfo *ci,
		const VkAllocationCallbacks *alloc, VkFence *out)
	{
		Log("vkCreateFence", device, ci, alloc, out);
		return pCreateFence(device, ci, alloc, out);
	}

	inline VKAPI_ATTR VkResult VKAPI_CALL TraceCreateQueryPool(
		VkDevice device, const VkQueryPoolCreateInfo *ci,
		const VkAllocationCallbacks *alloc, VkQueryPool *out)
	{
		Log("vkCreateQueryPool", device, ci, alloc, out);
		return pCreateQueryPool(device, ci, alloc, out);
	}


	inline VKAPI_ATTR void VKAPI_CALL TraceGetPhysicalDeviceFeatures2(
		VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2 *features)
	{
		Log("vkGetPhysicalDeviceFeatures2", physicalDevice, features);
		pGetPhysicalDeviceFeatures2(physicalDevice, features);
	}

	inline VKAPI_ATTR VkResult VKAPI_CALL TraceCreatePipelineCache(
		VkDevice device, const VkPipelineCacheCreateInfo *ci,
		const VkAllocationCallbacks *alloc, VkPipelineCache *out)
	{
		Log("vkCreatePipelineCache", device, ci, alloc, out);
		return pCreatePipelineCache(device, ci, alloc, out);
	}

	inline VKAPI_ATTR VkResult VKAPI_CALL TraceCreateDescriptorSetLayout(
		VkDevice device, const VkDescriptorSetLayoutCreateInfo *ci,
		const VkAllocationCallbacks *alloc, VkDescriptorSetLayout *out)
	{
		Log("vkCreateDescriptorSetLayout", device, ci, alloc, out);
		return pCreateDescriptorSetLayout(device, ci, alloc, out);
	}

	inline VKAPI_ATTR VkResult VKAPI_CALL TraceCreatePipelineLayout(
		VkDevice device, const VkPipelineLayoutCreateInfo *ci,
		const VkAllocationCallbacks *alloc, VkPipelineLayout *out)
	{
		Log("vkCreatePipelineLayout", device, ci, alloc, out);
		return pCreatePipelineLayout(device, ci, alloc, out);
	}

	inline VKAPI_ATTR VkResult VKAPI_CALL TraceCreateSampler(
		VkDevice device, const VkSamplerCreateInfo *ci,
		const VkAllocationCallbacks *alloc, VkSampler *out)
	{
		Log("vkCreateSampler", device, ci, alloc, out);
		return pCreateSampler(device, ci, alloc, out);
	}

	// Install the trampolines. Call AFTER VULKAN_HPP_DEFAULT_DISPATCHER.init(...)
	// (init overwrites every entry) and before the first NVRHI call.
	inline void Install()
	{
		if (!Enabled())
			return;

		auto &d = VULKAN_HPP_DEFAULT_DISPATCHER;
		// ABI fingerprint of the dispatcher as THIS translation unit sees it. Compared
		// against the same values printed from nvrhi's TU: a differing size or member
		// offset means the shared storage object is viewed through two different
		// vulkan.hpp layouts, which shifts every slot and makes device calls land on
		// the wrong function -- exactly the "vkGetImageSparseMemoryRequirements2:
		// Invalid device" report seen from the backend.
		std::fprintf(stderr,
			"VKABI app: header=%d size=%zu offSem=%lld offBuf=%lld obj=%p\n",
			(int)VK_HEADER_VERSION, sizeof(d),
			(long long)((const char *)&d.vkCreateSemaphore - (const char *)&d),
			(long long)((const char *)&d.vkCreateBuffer - (const char *)&d),
			(void *)&d);
		std::fflush(stderr);

			fprintf(stderr,
				"VKMACROS %s: win32=%d beta=%d noexc=%d noenh=%d nosmart=%d typeconv=%d dynloader=%d noprotos=%d dyn=%d hppver=%d\\n",
				"app",
#ifdef VK_USE_PLATFORM_WIN32_KHR
				1,
#else
				0,
#endif
#ifdef VK_ENABLE_BETA_EXTENSIONS
				1,
#else
				0,
#endif
#ifdef VULKAN_HPP_NO_EXCEPTIONS
				1,
#else
				0,
#endif
#ifdef VULKAN_HPP_DISABLE_ENHANCED_MODE
				1,
#else
				0,
#endif
#ifdef VULKAN_HPP_NO_SMART_HANDLE
				1,
#else
				0,
#endif
#ifdef VULKAN_HPP_TYPESAFE_CONVERSION
				1,
#else
				0,
#endif
#ifdef VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL
				1,
#else
				0,
#endif
#ifdef VK_NO_PROTOTYPES
				1,
#else
				0,
#endif
				(int)VULKAN_HPP_DISPATCH_LOADER_DYNAMIC, 0);
			fflush(stderr);
		Log("Install: dispatcher entries",
			reinterpret_cast<const void *>(d.vkCreateSemaphore),
			reinterpret_cast<const void *>(d.vkCreateCommandPool),
			reinterpret_cast<const void *>(d.vkAllocateCommandBuffers));

		pCreateSemaphore = d.vkCreateSemaphore;
		pDestroySemaphore = d.vkDestroySemaphore;
		pCreateCommandPool = d.vkCreateCommandPool;
		pAllocateCommandBuffers = d.vkAllocateCommandBuffers;
		pGetDeviceQueue = d.vkGetDeviceQueue;
		pGetPhysicalDeviceMemoryProperties2 = d.vkGetPhysicalDeviceMemoryProperties2;
		pCreateDescriptorPool = d.vkCreateDescriptorPool;
		pCreateBuffer = d.vkCreateBuffer;
		pCreateFence = d.vkCreateFence;
		pCreateQueryPool = d.vkCreateQueryPool;
		pGetPhysicalDeviceFeatures2 = d.vkGetPhysicalDeviceFeatures2;
		pCreatePipelineCache = d.vkCreatePipelineCache;
		pCreateDescriptorSetLayout = d.vkCreateDescriptorSetLayout;
		pCreatePipelineLayout = d.vkCreatePipelineLayout;
		pCreateSampler = d.vkCreateSampler;

		d.vkCreateSemaphore = &TraceCreateSemaphore;
		d.vkDestroySemaphore = &TraceDestroySemaphore;
		d.vkCreateCommandPool = &TraceCreateCommandPool;
		d.vkAllocateCommandBuffers = &TraceAllocateCommandBuffers;
		d.vkGetDeviceQueue = &TraceGetDeviceQueue;
		d.vkGetPhysicalDeviceMemoryProperties2 = &TraceGetPhysicalDeviceMemoryProperties2;
		d.vkCreateDescriptorPool = &TraceCreateDescriptorPool;
		d.vkCreateBuffer = &TraceCreateBuffer;
		d.vkCreateFence = &TraceCreateFence;
		d.vkCreateQueryPool = &TraceCreateQueryPool;
		d.vkGetPhysicalDeviceFeatures2 = &TraceGetPhysicalDeviceFeatures2;
		d.vkCreatePipelineCache = &TraceCreatePipelineCache;
		d.vkCreateDescriptorSetLayout = &TraceCreateDescriptorSetLayout;
		d.vkCreatePipelineLayout = &TraceCreatePipelineLayout;
		d.vkCreateSampler = &TraceCreateSampler;

		Log("Install: trampolines in place",
			reinterpret_cast<const void *>(d.vkCreateSemaphore),
			reinterpret_cast<const void *>(d.vkCreateCommandPool),
			reinterpret_cast<const void *>(d.vkAllocateCommandBuffers));
	}
}

#endif // WL_PLATFORM_WINDOWS
