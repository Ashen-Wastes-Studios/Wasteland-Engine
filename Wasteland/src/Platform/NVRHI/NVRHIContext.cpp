#include "wlpch.h"
#include "NVRHIContext.h"

#include "Wasteland/Core/Log.h"

// Vulkan headers must be included before GLFW for glfwCreateWindowSurface
// Enable dynamic Vulkan dispatch for proper device function loading
// NOTE: The storage for the default dispatcher is defined in VulkanDispatch.cpp (single TU).
// Do NOT define VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE here — duplicate storage causes
// the init in CreateDeviceVulkan to affect a different TU's dispatcher than NVRHI uses, leading to
// null device function pointers and driver crashes in nvrhi::vulkan::Queue::Queue.
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.h>
#include <vulkan/vulkan.hpp>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

// DirectX headers
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>

// NVRHI backend headers
#include <nvrhi/d3d11.h>
#include <nvrhi/d3d12.h>
#include <nvrhi/vulkan.h>
#include <nvrhi/utils.h>

namespace Wasteland
{

	NVRHIContext::NVRHIContext(GLFWwindow *windowHandle, RendererAPI::API api)
		: m_WindowHandle(windowHandle), m_API(api)
	{
	}

	NVRHIContext::~NVRHIContext()
	{
		WL_PROFILE_FUNCTION();

		// Shutdown ImGui resources before releasing device
		if (m_API == RendererAPI::API::NVRHI_Vulkan)
		{
			ShutdownVulkanImGuiResources();
		}
		else if (m_API == RendererAPI::API::NVRHI_DX12)
		{
			DestroyD3D12ImGuiCommandObjects();
			m_D3D12ImGuiSRVHeapCPU = {};
			m_D3D12ImGuiSRVHeapGPU = {};
			if (m_D3D12ImGuiSRVHeap)
			{
				static_cast<ID3D12DescriptorHeap *>(m_D3D12ImGuiSRVHeap)->Release();
				m_D3D12ImGuiSRVHeap = nullptr;
			}
		}

		// Release NVRHI objects
		m_CommandList = nullptr;
		m_Device = nullptr;

		// Release native DX objects
		if (m_DXGISwapChain3)
			static_cast<IDXGISwapChain3 *>(m_DXGISwapChain3)->Release();
		if (m_API == RendererAPI::API::NVRHI_DX11)
		{
			if (m_D3D11Context)
				static_cast<ID3D11DeviceContext *>(m_D3D11Context)->Release();
			if (m_D3D11Device)
				static_cast<ID3D11Device *>(m_D3D11Device)->Release();
		}
		else if (m_API == RendererAPI::API::NVRHI_DX12)
		{
			if (m_D3D12CommandQueue)
				static_cast<ID3D12CommandQueue *>(m_D3D12CommandQueue)->Release();
			if (m_D3D12Device)
				static_cast<ID3D12Device *>(m_D3D12Device)->Release();
		}
		else if (m_API == RendererAPI::API::NVRHI_Vulkan)
		{
			// Release Vulkan synchronization objects - guard against null device if creation failed
			VkDevice device = m_VkDevice;
			if (device)
			{
				for (size_t i = 0; i < m_VkFences.size(); i++)
				{
					if (m_VkFences[i] != VK_NULL_HANDLE)
						vkDestroyFence(device, m_VkFences[i], nullptr);
				}
				for (size_t i = 0; i < m_VkImageAvailableSemaphores.size(); i++)
				{
					if (m_VkImageAvailableSemaphores[i] != VK_NULL_HANDLE)
						vkDestroySemaphore(device, m_VkImageAvailableSemaphores[i], nullptr);
				}
				for (size_t i = 0; i < m_VkRenderFinishedSemaphores.size(); i++)
				{
					if (m_VkRenderFinishedSemaphores[i] != VK_NULL_HANDLE)
						vkDestroySemaphore(device, m_VkRenderFinishedSemaphores[i], nullptr);
				}

				// Destroy command pool
				if (m_VkCommandPool != VK_NULL_HANDLE)
					vkDestroyCommandPool(device, m_VkCommandPool, nullptr);

				// Destroy image views
				for (size_t i = 0; i < m_VkSwapchainImageViews.size(); i++)
				{
					if (m_VkSwapchainImageViews[i] != VK_NULL_HANDLE)
						vkDestroyImageView(device, m_VkSwapchainImageViews[i], nullptr);
				}

				// Destroy swapchain
				if (m_VkSwapchain != VK_NULL_HANDLE)
					vkDestroySwapchainKHR(device, m_VkSwapchain, nullptr);
			}
			else
			{
				// Device was never created - just clear vectors
				m_VkFences.clear();
				m_VkImageAvailableSemaphores.clear();
				m_VkRenderFinishedSemaphores.clear();
				m_VkSwapchainImageViews.clear();
				m_VkSwapchain = VK_NULL_HANDLE;
			}

			// Destroy device
			if (m_VkDevice != VK_NULL_HANDLE)
			{
				vkDeviceWaitIdle(m_VkDevice);
				vkDestroyDevice(m_VkDevice, nullptr);
			}

			// Destroy surface
			if (m_VkSurface != VK_NULL_HANDLE)
			{
				VkInstance instance = m_VkInstance;
				if (instance != VK_NULL_HANDLE)
					vkDestroySurfaceKHR(instance, m_VkSurface, nullptr);
			}

			// Destroy instance
			if (m_VkInstance != VK_NULL_HANDLE)
			{
				vkDestroyInstance(m_VkInstance, nullptr);
			}
		}
	}

	void NVRHIContext::Init()
	{
		WL_PROFILE_FUNCTION();

		// Get window size
		int width, height;
		glfwGetWindowSize(m_WindowHandle, &width, &height);
		m_Width = static_cast<uint32_t>(width);
		m_Height = static_cast<uint32_t>(height);

		// Create device based on API
		switch (m_API)
		{
		case RendererAPI::API::NVRHI_DX11:
			CreateDeviceDX11();
			break;
		case RendererAPI::API::NVRHI_DX12:
			CreateDeviceDX12();
			break;
		case RendererAPI::API::NVRHI_Vulkan:
			CreateDeviceVulkan();
			break;
		default:
			WL_CORE_ASSERT(false, "Unsupported NVRHI API");
			break;
		}

		CreateSwapChain();
		if (m_API == RendererAPI::API::NVRHI_Vulkan)
		{
			// Vulkan swapchain is created inside CreateDeviceVulkan after device creation
			// and uses native Vulkan APIs rather than DXGI
			CreateVulkanSwapChain();
		}
		else
		{
			CreateBackBufferFramebuffers();
		}

		// Create command list
		m_CommandList = m_Device->createCommandList();

		WL_CORE_INFO("NVRHI Context initialized successfully");
	}

	void NVRHIContext::CreateDeviceDX11()
	{
		WL_PROFILE_FUNCTION();

		// Create D3D11 device
		ID3D11Device *device = nullptr;
		ID3D11DeviceContext *context = nullptr;

		UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef WL_DEBUG
		creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

		D3D_FEATURE_LEVEL featureLevels[] = {
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0};

		HRESULT hr = D3D11CreateDevice(
			nullptr,
			D3D_DRIVER_TYPE_HARDWARE,
			nullptr,
			creationFlags,
			featureLevels,
			ARRAYSIZE(featureLevels),
			D3D11_SDK_VERSION,
			&device,
			nullptr,
			&context);

		if (FAILED(hr))
		{
			// Try without debug layer
			hr = D3D11CreateDevice(
				nullptr,
				D3D_DRIVER_TYPE_HARDWARE,
				nullptr,
				D3D11_CREATE_DEVICE_BGRA_SUPPORT,
				featureLevels,
				ARRAYSIZE(featureLevels),
				D3D11_SDK_VERSION,
				&device,
				nullptr,
				&context);
		}

		WL_CORE_ASSERT(SUCCEEDED(hr), "Failed to create D3D11 device");

		m_D3D11Device = device;
		m_D3D11Context = context;

		// Create NVRHI device
		nvrhi::d3d11::DeviceDesc deviceDesc;
		deviceDesc.context = context;
		m_Device = nvrhi::d3d11::createDevice(deviceDesc);

		WL_CORE_INFO("D3D11 device created successfully");
	}

	void NVRHIContext::CreateDeviceDX12()
	{
		WL_PROFILE_FUNCTION();

		// Create D3D12 device
		ID3D12Device *device = nullptr;
		ID3D12CommandQueue *commandQueue = nullptr;

#ifdef WL_DEBUG
		// Enable D3D12 debug layer if available (Agility SDK path)
		{
			ID3D12Debug *debugController = nullptr;
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
			{
				debugController->EnableDebugLayer();
				debugController->Release();
			}
		}
#endif

		HRESULT hr = D3D12CreateDevice(
			nullptr,
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&device));

		if (FAILED(hr))
		{
			// Try without debug layer
			hr = D3D12CreateDevice(
				nullptr,
				D3D_FEATURE_LEVEL_11_0,
				IID_PPV_ARGS(&device));
		}

		WL_CORE_ASSERT(SUCCEEDED(hr), "Failed to create D3D12 device");

		// Create command queue
		D3D12_COMMAND_QUEUE_DESC queueDesc = {};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue));
		WL_CORE_ASSERT(SUCCEEDED(hr), "Failed to create D3D12 command queue");

		m_D3D12Device = device;
		m_D3D12CommandQueue = commandQueue;

		// Create NVRHI device
		nvrhi::d3d12::DeviceDesc deviceDesc;
		deviceDesc.pDevice = device;
		deviceDesc.pGraphicsCommandQueue = commandQueue;
		m_Device = nvrhi::d3d12::createDevice(deviceDesc);

		// Create descriptor heap for ImGui (SRV heap for font texture)
		D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
		heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heapDesc.NumDescriptors = 64; // 64 descriptors for ImGui font + user textures (prev 1 was too small)
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		heapDesc.NodeMask = 0;

		ID3D12DescriptorHeap *srvHeap = nullptr;
		hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap));
		if (SUCCEEDED(hr) && srvHeap)
		{
			m_D3D12ImGuiSRVHeap = srvHeap;

			// Get CPU and GPU handles
			D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();
			D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = srvHeap->GetGPUDescriptorHandleForHeapStart();

			m_D3D12ImGuiSRVHeapCPU = cpuHandle;
			m_D3D12ImGuiSRVHeapGPU = gpuHandle;

			WL_CORE_INFO("D3D12 ImGui SRV descriptor heap created (64 descriptors)");
		}
		else
		{
			WL_CORE_ERROR("Failed to create D3D12 ImGui SRV descriptor heap");
		}

		CreateD3D12ImGuiCommandObjects();

		WL_CORE_INFO("D3D12 device created successfully");
	}

	void NVRHIContext::CreateD3D12ImGuiCommandObjects()
	{
		WL_PROFILE_FUNCTION();
		if (!m_D3D12Device || !m_D3D12CommandQueue)
			return;

		ID3D12Device *device = static_cast<ID3D12Device *>(m_D3D12Device);

		// Command allocator
		ID3D12CommandAllocator *allocator = nullptr;
		HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
		if (FAILED(hr) || !allocator)
		{
			WL_CORE_ERROR("Failed to create D3D12 ImGui command allocator");
			return;
		}
		m_D3D12ImGuiCommandAllocator = allocator;

		// Command list (closed state, will be reset each frame)
		ID3D12GraphicsCommandList *cmdList = nullptr;
		hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&cmdList));
		if (FAILED(hr) || !cmdList)
		{
			WL_CORE_ERROR("Failed to create D3D12 ImGui command list");
			allocator->Release();
			m_D3D12ImGuiCommandAllocator = nullptr;
			return;
		}
		cmdList->Close();
		m_D3D12ImGuiCommandList = cmdList;

		// Fence for synchronization
		ID3D12Fence *fence = nullptr;
		hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
		if (SUCCEEDED(hr) && fence)
		{
			m_D3D12ImGuiFence = fence;
			m_D3D12ImGuiFenceValue = 1;
			m_D3D12ImGuiFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
			if (!m_D3D12ImGuiFenceEvent)
				WL_CORE_WARN("Failed to create D3D12 fence event");
		}
		else
		{
			WL_CORE_WARN("Failed to create D3D12 ImGui fence");
		}

		// RTV heap for ImGui (2 descriptors for double buffering)
		if (!m_D3D12ImGuiRTVHeap)
		{
			D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
			rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			rtvHeapDesc.NumDescriptors = 2;
			rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
			ID3D12DescriptorHeap *rtvHeap = nullptr;
			hr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap));
			if (SUCCEEDED(hr) && rtvHeap)
			{
				m_D3D12ImGuiRTVHeap = rtvHeap;
				m_D3D12RTVDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
				WL_CORE_INFO("D3D12 ImGui RTV heap created");
			}
			else
			{
				WL_CORE_WARN("Failed to create D3D12 ImGui RTV heap");
			}
		}

		WL_CORE_INFO("D3D12 ImGui command objects created");
	}

	void NVRHIContext::DestroyD3D12ImGuiCommandObjects()
	{
		WL_PROFILE_FUNCTION();
		if (m_D3D12ImGuiFenceEvent)
		{
			CloseHandle(m_D3D12ImGuiFenceEvent);
			m_D3D12ImGuiFenceEvent = nullptr;
		}
		if (m_D3D12ImGuiFence)
		{
			static_cast<ID3D12Fence *>(m_D3D12ImGuiFence)->Release();
			m_D3D12ImGuiFence = nullptr;
		}
		if (m_D3D12ImGuiCommandList)
		{
			static_cast<ID3D12GraphicsCommandList *>(m_D3D12ImGuiCommandList)->Release();
			m_D3D12ImGuiCommandList = nullptr;
		}
		if (m_D3D12ImGuiCommandAllocator)
		{
			static_cast<ID3D12CommandAllocator *>(m_D3D12ImGuiCommandAllocator)->Release();
			m_D3D12ImGuiCommandAllocator = nullptr;
		}
		if (m_D3D12ImGuiRTVHeap)
		{
			static_cast<ID3D12DescriptorHeap *>(m_D3D12ImGuiRTVHeap)->Release();
			m_D3D12ImGuiRTVHeap = nullptr;
		}
	}

	bool NVRHIContext::SelectVulkanPhysicalDevice()
	{
		WL_PROFILE_FUNCTION();

		VkInstance instance = m_VkInstance;
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;

		// Enumerate physical devices
		uint32_t deviceCount = 0;
		vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
		if (deviceCount == 0)
		{
			WL_CORE_ERROR("Failed to find GPUs with Vulkan support");
			return false;
		}

		std::vector<VkPhysicalDevice> devices(deviceCount);
		vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

		// Prefer discrete GPU
		for (const auto &device : devices)
		{
			VkPhysicalDeviceProperties properties;
			vkGetPhysicalDeviceProperties(device, &properties);

			VkPhysicalDeviceFeatures features;
			vkGetPhysicalDeviceFeatures(device, &features);

			if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
			{
				physicalDevice = device;
				m_VkPhysicalDevice = physicalDevice;
				WL_CORE_INFO("Selected discrete GPU: {0}", properties.deviceName);
				return true;
			}
		}

		// Fall back to first available
		physicalDevice = devices[0];
		m_VkPhysicalDevice = physicalDevice;

		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties(physicalDevice, &properties);
		WL_CORE_INFO("Selected GPU: {0}", properties.deviceName);

		return true;
	}

	void NVRHIContext::GetVulkanQueueFamilyIndices()
	{
		VkPhysicalDevice physicalDevice = static_cast<VkPhysicalDevice>(m_VkPhysicalDevice);
		VkSurfaceKHR surface = static_cast<VkSurfaceKHR>(m_VkSurface);

		uint32_t queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);

		std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

		// Use sentinel to correctly handle queue family 0 as valid index
		uint32_t graphicsIndex = UINT32_MAX;
		uint32_t presentIndex = UINT32_MAX;
		for (uint32_t i = 0; i < queueFamilyCount; i++)
		{
			if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && graphicsIndex == UINT32_MAX)
			{
				graphicsIndex = i;
			}

			VkBool32 presentSupport = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface, &presentSupport);
			if (presentSupport && presentIndex == UINT32_MAX)
			{
				presentIndex = i;
			}

			if (graphicsIndex != UINT32_MAX && presentIndex != UINT32_MAX)
			{
				break;
			}
		}

		// Fallback to 0 if not found (should not happen on valid device)
		m_VkGraphicsQueueFamilyIndex = (graphicsIndex != UINT32_MAX) ? graphicsIndex : 0;
		m_VkPresentQueueFamilyIndex = (presentIndex != UINT32_MAX) ? presentIndex : m_VkGraphicsQueueFamilyIndex;

		WL_CORE_INFO("Vulkan queue families: graphics={0}, present={1}",
					 m_VkGraphicsQueueFamilyIndex, m_VkPresentQueueFamilyIndex);
	}

	void NVRHIContext::CreateVulkanSurface()
	{
		WL_PROFILE_FUNCTION();

		VkInstance instance = static_cast<VkInstance>(m_VkInstance);
		HWND hwnd = glfwGetWin32Window(m_WindowHandle);

		VkSurfaceKHR surface = VK_NULL_HANDLE;
		VkResult result = glfwCreateWindowSurface(instance, m_WindowHandle, nullptr, &surface);
		if (result != VK_SUCCESS)
		{
			WL_CORE_ERROR("Failed to create Vulkan window surface");
			return;
		}

		m_VkSurface = surface;
		WL_CORE_INFO("Vulkan surface created successfully");
	}

	void NVRHIContext::CreateVulkanDevice()
	{
		WL_PROFILE_FUNCTION();

		VkPhysicalDevice physicalDevice = static_cast<VkPhysicalDevice>(m_VkPhysicalDevice);
		VkSurfaceKHR surface = static_cast<VkSurfaceKHR>(m_VkSurface);

		GetVulkanQueueFamilyIndices();

		// Get queue family properties
		uint32_t queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);

		std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

		// Determine unique queue families
		std::vector<uint32_t> uniqueQueueFamilies;
		uniqueQueueFamilies.push_back(m_VkGraphicsQueueFamilyIndex);
		if (m_VkPresentQueueFamilyIndex != m_VkGraphicsQueueFamilyIndex)
		{
			uniqueQueueFamilies.push_back(m_VkPresentQueueFamilyIndex);
		}

		// Create queue create infos
		std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
		std::vector<float> queuePriorities = {1.0f};

		for (uint32_t queueFamily : uniqueQueueFamilies)
		{
			VkDeviceQueueCreateInfo queueCreateInfo = {};
			queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queueCreateInfo.queueFamilyIndex = queueFamily;
			queueCreateInfo.queueCount = 1;
			queueCreateInfo.pQueuePriorities = queuePriorities.data();
			queueCreateInfos.push_back(queueCreateInfo);
		}

		// Device extensions — swapchain is always required
		std::vector<const char *> deviceExtensions = {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME};

		// --- Timeline semaphore detection (required by NVRHI) ---
		// In Vulkan 1.2+ timeline semaphores are core (VkPhysicalDeviceVulkan12Features::timelineSemaphore).
		// In Vulkan 1.1 they are exposed via VK_KHR_timeline_semaphore + VkPhysicalDeviceTimelineSemaphoreFeatures.
		// The old code only checked the KHR extension string, so on 1.2+ drivers where the extension is not
		// enumerated (promoted to core) it incorrectly concluded timeline semaphores were unavailable and created
		// the device without the feature — nvrhi::vulkan::Queue then crashed in nvoglv64.dll when creating its
		// internal timeline semaphore.
		VkPhysicalDeviceFeatures deviceFeatures = {};
		// --- Diagnostic dump ---
		{
			VkPhysicalDeviceProperties propsDbg = {};
			vkGetPhysicalDeviceProperties(physicalDevice, &propsDbg);
			WL_CORE_INFO("=== Vulkan Device Diagnostic ===");
			WL_CORE_INFO("  GPU: {0} apiVersion {1}.{2}.{3}", propsDbg.deviceName, VK_VERSION_MAJOR(propsDbg.apiVersion), VK_VERSION_MINOR(propsDbg.apiVersion), VK_VERSION_PATCH(propsDbg.apiVersion));
			uint32_t extCountDbg = 0;
			vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCountDbg, nullptr);
			std::vector<VkExtensionProperties> extsDbg(extCountDbg);
			vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCountDbg, extsDbg.data());
			bool hasSwapchain = false, hasTimelineKHRDbg = false;
			for (auto &e : extsDbg) {
				if (strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)==0) hasSwapchain = true;
				if (strcmp(e.extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)==0) hasTimelineKHRDbg = true;
			}
			WL_CORE_INFO("  Extensions: swapchain={0} timelineKHR={1} total={2}", hasSwapchain, hasTimelineKHRDbg, extCountDbg);
			for (auto &e : extsDbg) {
				if (strstr(e.extensionName, "timeline") || strstr(e.extensionName, "swapchain"))
					WL_CORE_INFO("    ext: {0}", e.extensionName);
			}
		}
		bool timelineSemaphoreSupported = false;
		bool hasTimelineSemaphoreExtension = false;
		VkPhysicalDeviceVulkan12Features vulkan12FeaturesQuery = {};
		vulkan12FeaturesQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeaturesQueryKHR = {};
		timelineFeaturesQueryKHR.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;

		// Query both structs in one chain — some drivers only fill the struct you chain,
		// so chain Vulkan12 -> KHR and check both.
		{
			vulkan12FeaturesQuery.pNext = &timelineFeaturesQueryKHR;
			VkPhysicalDeviceFeatures2 features2 = {};
			features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
			features2.pNext = &vulkan12FeaturesQuery;
			vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
			WL_CORE_INFO("  Query Vulkan12.timelineSemaphore={0} KHR.timelineSemaphore={1}", (bool)vulkan12FeaturesQuery.timelineSemaphore, (bool)timelineFeaturesQueryKHR.timelineSemaphore);
			if (vulkan12FeaturesQuery.timelineSemaphore || timelineFeaturesQueryKHR.timelineSemaphore)
				timelineSemaphoreSupported = true;
			// unchain for later enable path
			vulkan12FeaturesQuery.pNext = nullptr;
		}

		if (!timelineSemaphoreSupported)
		{
			uint32_t extensionCount = 0;
			vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
			std::vector<VkExtensionProperties> availableExtensions(extensionCount);
			vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, availableExtensions.data());
			for (const auto &ext : availableExtensions)
			{
				if (strcmp(ext.extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0)
				{
					hasTimelineSemaphoreExtension = true;
					break;
				}
			}
			WL_CORE_INFO("  Fallback KHR extension present: {0}", hasTimelineSemaphoreExtension);
			if (hasTimelineSemaphoreExtension)
			{
				// Re-query KHR alone to be sure
				VkPhysicalDeviceTimelineSemaphoreFeatures ts2 = {};
				ts2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
				VkPhysicalDeviceFeatures2 features2 = {};
				features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
				features2.pNext = &ts2;
				vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
				if (ts2.timelineSemaphore)
					timelineSemaphoreSupported = true;
				WL_CORE_INFO("  Fallback KHR query timelineSemaphore={0}", (bool)ts2.timelineSemaphore);
			}
		} else {
			// Also check if KHR extension is enumerated (for enable path)
			uint32_t ec = 0;
			vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &ec, nullptr);
			std::vector<VkExtensionProperties> ae(ec);
			vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &ec, ae.data());
			for (auto &e: ae) if (strcmp(e.extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)==0) hasTimelineSemaphoreExtension = true;
		}

		// Prepare pNext to enable — chain both structs so driver sees both core and KHR enable.
		VkPhysicalDeviceVulkan12Features enabledVulkan12Features = {};
		VkPhysicalDeviceTimelineSemaphoreFeatures enabledTimelineFeaturesKHR = {};
		void *deviceCreatePNext = nullptr;

		if (timelineSemaphoreSupported)
		{
			enabledVulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
			enabledVulkan12Features.timelineSemaphore = VK_TRUE;
			enabledTimelineFeaturesKHR.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
			enabledTimelineFeaturesKHR.timelineSemaphore = VK_TRUE;
			// Only chain KHR struct if extension is actually reported — passing it without the extension
			// makes vkCreateDevice fail with VK_ERROR_EXTENSION_NOT_PRESENT on core 1.2 drivers.
			if (hasTimelineSemaphoreExtension) {
				enabledVulkan12Features.pNext = &enabledTimelineFeaturesKHR;
				deviceExtensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
				deviceCreatePNext = &enabledVulkan12Features;
				WL_CORE_INFO("Enabling timeline semaphores via VkPhysicalDeviceVulkan12Features + KHR extension (has extension)");
			} else {
				deviceCreatePNext = &enabledVulkan12Features;
				WL_CORE_INFO("Enabling timeline semaphores via VkPhysicalDeviceVulkan12Features (core 1.2, no extension)");
			}
		}
		else
		{
			WL_CORE_ERROR("Vulkan device does not support timeline semaphores — NVRHI Vulkan backend requires it. Device creation will proceed but NVRHI Queue creation will crash.");
		}

		// Device create info
		VkDeviceCreateInfo createInfo = {};
		createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		createInfo.pNext = deviceCreatePNext;
		createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
		createInfo.pQueueCreateInfos = queueCreateInfos.data();
		createInfo.pEnabledFeatures = &deviceFeatures;
		createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
		createInfo.ppEnabledExtensionNames = deviceExtensions.data();

		// Validation layers (legacy, but supported by some implementations)
		std::vector<const char *> validationLayers;
		if (m_VkValidationLayersEnabled)
		{
			validationLayers.push_back("VK_LAYER_KHRONOS_validation");
			createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
			createInfo.ppEnabledLayerNames = validationLayers.data();
		}
		else
		{
			createInfo.enabledLayerCount = 0;
		}

		VkDevice logicalDevice = VK_NULL_HANDLE;
		VkResult result = vkCreateDevice(physicalDevice, &createInfo, nullptr, &logicalDevice);
		if (result != VK_SUCCESS)
		{
			WL_CORE_ERROR("Failed to create Vulkan logical device");
			return;
		}

		m_VkDevice = logicalDevice;

		// Get queues
		VkQueue graphicsQueue = VK_NULL_HANDLE;
		VkQueue presentQueue = VK_NULL_HANDLE;
		vkGetDeviceQueue(logicalDevice, m_VkGraphicsQueueFamilyIndex, 0, &graphicsQueue);
		vkGetDeviceQueue(logicalDevice, m_VkPresentQueueFamilyIndex, 0, &presentQueue);
		m_VkGraphicsQueue = graphicsQueue;
		m_VkPresentQueue = presentQueue;

		WL_CORE_INFO("Vulkan device created successfully");
	}

	void NVRHIContext::CreateDeviceVulkan()
	{
		WL_PROFILE_FUNCTION();

		// Create Vulkan instance
		VkApplicationInfo appInfo = {};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.pApplicationName = "Wasteland Engine";
		appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.pEngineName = "Wasteland Engine";
		appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.apiVersion = VK_API_VERSION_1_2;

		// Get required extensions from GLFW
		uint32_t glfwExtensionCount = 0;
		const char **glfwExtensions;
		glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

		std::vector<const char *> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

		// Check if validation layers are available
		std::vector<const char *> validationLayers;
		bool validationLayersAvailable = false;

#ifdef WL_DEBUG
		{
			uint32_t layerCount;
			std::vector<VkLayerProperties> availableLayers;
			vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
			availableLayers.resize(layerCount);
			vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

			for (const auto &layer : availableLayers)
			{
				if (strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0)
				{
					validationLayersAvailable = true;
					break;
				}
			}

			if (validationLayersAvailable)
			{
				validationLayers.push_back("VK_LAYER_KHRONOS_validation");
				extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
				m_VkValidationLayersEnabled = true;
			}
			else
			{
				WL_CORE_WARN("VK_LAYER_KHRONOS_validation not found, disabling validation layers");
			}
		}
#endif

		VkInstanceCreateInfo createInfo = {};
		createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		createInfo.pApplicationInfo = &appInfo;
		createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
		createInfo.ppEnabledExtensionNames = extensions.data();

		if (validationLayersAvailable)
		{
			createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
			createInfo.ppEnabledLayerNames = validationLayers.data();
		}
		else
		{
			createInfo.enabledLayerCount = 0;
		}

		VkInstance instance = VK_NULL_HANDLE;
		VkResult result = vkCreateInstance(&createInfo, nullptr, &instance);
		if (result != VK_SUCCESS)
		{
			WL_CORE_ERROR("Failed to create Vulkan instance");
			return;
		}

		m_VkInstance = instance;
		WL_CORE_INFO("Vulkan instance created successfully");

		// Create surface
		CreateVulkanSurface();

		// Select physical device
		if (!SelectVulkanPhysicalDevice())
		{
			WL_CORE_ERROR("Failed to select Vulkan physical device");
			return;
		}

		// Create logical device
		CreateVulkanDevice();

		if (!m_VkDevice)
		{
			WL_CORE_ERROR("Failed to create Vulkan device");
			return;
		}

		// Initialize the Vulkan-Hpp dispatcher with device-level functions
		// This is required because NVRHI uses vulkan.hpp but we're not building with NVRHI_SHARED_LIBRARY_BUILD
		VULKAN_HPP_DEFAULT_DISPATCHER.init(m_VkInstance, vkGetInstanceProcAddr, m_VkDevice);

		// Create NVRHI Vulkan device
		nvrhi::vulkan::DeviceDesc deviceDesc;
		deviceDesc.instance = static_cast<VkInstance>(m_VkInstance);
		deviceDesc.physicalDevice = static_cast<VkPhysicalDevice>(m_VkPhysicalDevice);
		deviceDesc.device = static_cast<VkDevice>(m_VkDevice);
		deviceDesc.graphicsQueue = static_cast<VkQueue>(m_VkGraphicsQueue);
		deviceDesc.graphicsQueueIndex = m_VkGraphicsQueueFamilyIndex;
		deviceDesc.transferQueue = static_cast<VkQueue>(m_VkGraphicsQueue);
		deviceDesc.transferQueueIndex = m_VkGraphicsQueueFamilyIndex;
		deviceDesc.computeQueue = static_cast<VkQueue>(m_VkGraphicsQueue);
		deviceDesc.computeQueueIndex = m_VkGraphicsQueueFamilyIndex;

		m_Device = nvrhi::vulkan::createDevice(deviceDesc);

		if (!m_Device)
		{
			WL_CORE_ERROR("Failed to create NVRHI Vulkan device");
			return;
		}

		WL_CORE_INFO("NVRHI Vulkan device created successfully");
	}

	void NVRHIContext::CreateSwapChain()
	{
		WL_PROFILE_FUNCTION();

		// Vulkan swapchain is handled separately
		if (m_API == RendererAPI::API::NVRHI_Vulkan)
		{
			return;
		}

		HWND hwnd = glfwGetWin32Window(m_WindowHandle);
		if (!hwnd)
		{
			WL_CORE_ERROR("CreateSwapChain: Failed to get HWND from GLFW window");
			return;
		}

		// Create DXGI factory
		IDXGIFactory4 *factory = nullptr;
		HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
		if (FAILED(hr) || !factory)
		{
			WL_CORE_ERROR("CreateSwapChain: Failed to create DXGI factory (HRESULT: {0})", hr);
			return;
		}

		// Describe swap chain
		DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
		swapChainDesc.BufferCount = 2;
		swapChainDesc.Width = m_Width;
		swapChainDesc.Height = m_Height;
		swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		swapChainDesc.SampleDesc.Count = 1;

		IDXGISwapChain1 *swapChain1 = nullptr;

		if (m_API == RendererAPI::API::NVRHI_DX12)
		{
			hr = factory->CreateSwapChainForHwnd(
				static_cast<IUnknown *>(m_D3D12CommandQueue),
				hwnd,
				&swapChainDesc,
				nullptr,
				nullptr,
				&swapChain1);
		}
		else if (m_API == RendererAPI::API::NVRHI_DX11)
		{
			hr = factory->CreateSwapChainForHwnd(
				static_cast<IUnknown *>(m_D3D11Device), // Pass device, not context
				hwnd,
				&swapChainDesc,
				nullptr,
				nullptr,
				&swapChain1);
		}

		if (FAILED(hr) || !swapChain1)
		{
			WL_CORE_ERROR("CreateSwapChain: Failed to create swap chain (HRESULT: {0})", hr);
			factory->Release();
			return;
		}

		IDXGISwapChain3 *swapChain3 = nullptr;
		hr = swapChain1->QueryInterface(IID_PPV_ARGS(&swapChain3));
		if (FAILED(hr) || !swapChain3)
		{
			WL_CORE_ERROR("CreateSwapChain: Failed to query IDXGISwapChain3 (HRESULT: {0})", hr);
			swapChain1->Release();
			factory->Release();
			return;
		}
		swapChain1->Release();
		m_DXGISwapChain3 = swapChain3;
		if (m_API == RendererAPI::API::NVRHI_DX12)
			WL_CORE_INFO("Created DX12 swap chain");
		else
			WL_CORE_INFO("Created DX11 swap chain");

		factory->Release();

		CreateBackBufferFramebuffers();

		WL_CORE_INFO("Swap chain created ({0}x{1})", m_Width, m_Height);
	}

	void NVRHIContext::CreateBackBufferFramebuffers()
	{
		WL_PROFILE_FUNCTION();

		// Clear old backbuffers
		m_BackBuffers.clear();
		m_BackBufferFramebuffers.clear();

		// For Vulkan, backbuffers are created in CreateVulkanSwapChain
		if (m_API == RendererAPI::API::NVRHI_Vulkan)
		{
			return;
		}

		// Check if swap chain exists
		if ((m_API == RendererAPI::API::NVRHI_DX11 || m_API == RendererAPI::API::NVRHI_DX12) && !m_DXGISwapChain3)
		{
			WL_CORE_ERROR("CreateBackBufferFramebuffers: Swap chain is null");
			return;
		}

		// Get backbuffers from swapchain
		const uint32_t bufferCount = 2;
		m_BackBuffers.resize(bufferCount);
		m_BackBufferFramebuffers.resize(bufferCount);

		if (m_API == RendererAPI::API::NVRHI_DX11)
		{
			IDXGISwapChain3 *swapChain = static_cast<IDXGISwapChain3 *>(m_DXGISwapChain3);

			// Get all backbuffers first
			std::vector<ID3D11Texture2D *> nativeBuffers(bufferCount, nullptr);
			for (uint32_t i = 0; i < bufferCount; i++)
			{
				HRESULT hr = swapChain->GetBuffer(i, IID_PPV_ARGS(&nativeBuffers[i]));
				if (FAILED(hr) || !nativeBuffers[i])
				{
					WL_CORE_ERROR("CreateBackBufferFramebuffers: Failed to get DX11 backbuffer {0} (HRESULT: {1})", i, hr);
					// Clean up already acquired buffers
					for (uint32_t j = 0; j < i; j++)
					{
						if (nativeBuffers[j])
							nativeBuffers[j]->Release();
					}
					return;
				}
			}

			// Now create NVRHI textures and framebuffers
			for (uint32_t i = 0; i < bufferCount; i++)
			{
				// Create NVRHI texture from D3D11 texture
				nvrhi::TextureDesc desc;
				desc.width = m_Width;
				desc.height = m_Height;
				desc.format = nvrhi::Format::SRGBA8_UNORM;
				desc.isRenderTarget = true;

				// AddRef for NVRHI, then release our reference
				nativeBuffers[i]->AddRef();
				m_BackBuffers[i] = m_Device->createHandleForNativeTexture(
					nvrhi::ObjectTypes::D3D11_Resource,
					nativeBuffers[i],
					desc);
				nativeBuffers[i]->Release();

				// Create framebuffer
				nvrhi::FramebufferDesc fbDesc;
				fbDesc.addColorAttachment(m_BackBuffers[i]);
				m_BackBufferFramebuffers[i] = m_Device->createFramebuffer(fbDesc);
			}

			m_CurrentBackBufferIndex = swapChain->GetCurrentBackBufferIndex();
		}
		else if (m_API == RendererAPI::API::NVRHI_DX12)
		{
			IDXGISwapChain3 *swapChain = static_cast<IDXGISwapChain3 *>(m_DXGISwapChain3);

			// Get all backbuffers first
			std::vector<ID3D12Resource *> nativeBuffers(bufferCount, nullptr);
			for (uint32_t i = 0; i < bufferCount; i++)
			{
				HRESULT hr = swapChain->GetBuffer(i, IID_PPV_ARGS(&nativeBuffers[i]));
				if (FAILED(hr) || !nativeBuffers[i])
				{
					WL_CORE_ERROR("CreateBackBufferFramebuffers: Failed to get DX12 backbuffer {0} (HRESULT: {1})", i, hr);
					// Clean up already acquired buffers
					for (uint32_t j = 0; j < i; j++)
					{
						if (nativeBuffers[j])
							nativeBuffers[j]->Release();
					}
					return;
				}
			}

			// Now create NVRHI textures and framebuffers
			for (uint32_t i = 0; i < bufferCount; i++)
			{
				// Create NVRHI texture from D3D12 resource
				nvrhi::TextureDesc desc;
				desc.width = m_Width;
				desc.height = m_Height;
				desc.format = nvrhi::Format::SRGBA8_UNORM;
				desc.isRenderTarget = true;

				// AddRef for NVRHI, then release our reference
				nativeBuffers[i]->AddRef();
				m_BackBuffers[i] = m_Device->createHandleForNativeTexture(
					nvrhi::ObjectTypes::D3D12_Resource,
					nativeBuffers[i],
					desc);
				nativeBuffers[i]->Release();

				// Create framebuffer
				nvrhi::FramebufferDesc fbDesc;
				fbDesc.addColorAttachment(m_BackBuffers[i]);
				m_BackBufferFramebuffers[i] = m_Device->createFramebuffer(fbDesc);
			}

			m_CurrentBackBufferIndex = swapChain->GetCurrentBackBufferIndex();
			// Create RTVs for ImGui DX12 if heap exists
			if (m_D3D12ImGuiRTVHeap && m_D3D12Device)
			{
				ID3D12Device *device = static_cast<ID3D12Device *>(m_D3D12Device);
				ID3D12DescriptorHeap *rtvHeap = static_cast<ID3D12DescriptorHeap *>(m_D3D12ImGuiRTVHeap);
				D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
				for (uint32_t i = 0; i < bufferCount; i++)
				{
					if (nativeBuffers[i])
					{
						D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHandle;
						h.ptr += i * m_D3D12RTVDescriptorSize;
						device->CreateRenderTargetView(nativeBuffers[i], nullptr, h);
					}
				}
			}
		}

		WL_CORE_INFO("Created {0} backbuffer framebuffers", bufferCount);
	}

	void NVRHIContext::CreateVulkanSwapChain()
	{
		WL_PROFILE_FUNCTION();

		VkDevice device = m_VkDevice;
		VkPhysicalDevice physicalDevice = m_VkPhysicalDevice;
		VkSurfaceKHR surface = m_VkSurface;

		// Get surface capabilities
		VkSurfaceCapabilitiesKHR surfaceCapabilities;
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &surfaceCapabilities);

		// Get available surface formats
		uint32_t formatCount;
		vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
		std::vector<VkSurfaceFormatKHR> availableFormats(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, availableFormats.data());

		// Select surface format
		VkSurfaceFormatKHR surfaceFormat = availableFormats[0];
		for (const auto &availableFormat : availableFormats)
		{
			if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
				availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			{
				surfaceFormat = availableFormat;
				break;
			}
		}

		m_VkSwapchainImageFormat = static_cast<uint32_t>(surfaceFormat.format);

		// Get available present modes
		uint32_t presentModeCount;
		vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, nullptr);
		std::vector<VkPresentModeKHR> availablePresentModes(presentModeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, availablePresentModes.data());

		// Select present mode (prefer MAILBOX, fallback to FIFO)
		VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
		for (const auto &availablePresentMode : availablePresentModes)
		{
			if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR)
			{
				presentMode = availablePresentMode;
				break;
			}
		}

		// Select swap extent
		VkExtent2D extent = surfaceCapabilities.currentExtent;
		if (extent.width == 0xFFFFFFFF)
		{
			extent.width = m_Width;
			extent.height = m_Height;
		}

		// Determine image count
		uint32_t imageCount = surfaceCapabilities.minImageCount + 1;
		if (surfaceCapabilities.maxImageCount > 0 && imageCount > surfaceCapabilities.maxImageCount)
		{
			imageCount = surfaceCapabilities.maxImageCount;
		}

		m_VkImageCount = imageCount;

		// Determine sharing mode
		std::vector<uint32_t> queueFamilyIndices;
		if (m_VkGraphicsQueueFamilyIndex == m_VkPresentQueueFamilyIndex)
		{
			queueFamilyIndices.push_back(m_VkGraphicsQueueFamilyIndex);
		}
		else
		{
			queueFamilyIndices.push_back(m_VkGraphicsQueueFamilyIndex);
			queueFamilyIndices.push_back(m_VkPresentQueueFamilyIndex);
		}

		VkSharingMode sharingMode = (m_VkGraphicsQueueFamilyIndex == m_VkPresentQueueFamilyIndex)
										? VK_SHARING_MODE_EXCLUSIVE
										: VK_SHARING_MODE_CONCURRENT;

		// Determine composite alpha
		VkCompositeAlphaFlagBitsKHR compositeAlpha;
		if (surfaceCapabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
			compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
		else if (surfaceCapabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
			compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
		else if (surfaceCapabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)
			compositeAlpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
		else
			compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

		// Create swapchain
		VkSwapchainCreateInfoKHR swapchainCreateInfo = {};
		swapchainCreateInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		swapchainCreateInfo.surface = surface;
		swapchainCreateInfo.minImageCount = imageCount;
		swapchainCreateInfo.imageFormat = surfaceFormat.format;
		swapchainCreateInfo.imageColorSpace = surfaceFormat.colorSpace;
		swapchainCreateInfo.imageExtent = extent;
		swapchainCreateInfo.imageArrayLayers = 1;
		swapchainCreateInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		swapchainCreateInfo.imageSharingMode = sharingMode;
		swapchainCreateInfo.queueFamilyIndexCount = static_cast<uint32_t>(queueFamilyIndices.size());
		swapchainCreateInfo.pQueueFamilyIndices = queueFamilyIndices.data();
		swapchainCreateInfo.preTransform = surfaceCapabilities.currentTransform;
		swapchainCreateInfo.compositeAlpha = compositeAlpha;
		swapchainCreateInfo.oldSwapchain = VK_NULL_HANDLE;
		swapchainCreateInfo.presentMode = presentMode;
		swapchainCreateInfo.clipped = VK_TRUE;

		VkSwapchainKHR swapchain = VK_NULL_HANDLE;
		VkResult result = vkCreateSwapchainKHR(device, &swapchainCreateInfo, nullptr, &swapchain);
		if (result != VK_SUCCESS)
		{
			WL_CORE_ERROR("Failed to create Vulkan swapchain");
			return;
		}

		m_VkSwapchain = swapchain;

		WL_CORE_INFO("Vulkan swapchain created ({0}x{1}, {2} images)",
					 extent.width, extent.height, imageCount);

		// Get swapchain images
		vkGetSwapchainImagesKHR(device, m_VkSwapchain, &imageCount, nullptr);
		m_VkSwapchainImages.resize(imageCount);
		vkGetSwapchainImagesKHR(device, m_VkSwapchain, &imageCount, m_VkSwapchainImages.data());

		m_VkImageCount = imageCount;

		// Create image views
		m_VkSwapchainImageViews.resize(imageCount);
		m_VkBackBuffers.resize(imageCount);
		m_VkFramebuffers.resize(imageCount);

		for (uint32_t i = 0; i < imageCount; i++)
		{
			VkImageViewCreateInfo imageViewCreateInfo = {};
			imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			imageViewCreateInfo.image = static_cast<VkImage>(m_VkSwapchainImages[i]);
			imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			imageViewCreateInfo.format = surfaceFormat.format;
			imageViewCreateInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
			imageViewCreateInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
			imageViewCreateInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
			imageViewCreateInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
			imageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
			imageViewCreateInfo.subresourceRange.levelCount = 1;
			imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
			imageViewCreateInfo.subresourceRange.layerCount = 1;

			VkImageView imageView = VK_NULL_HANDLE;
			result = vkCreateImageView(device, &imageViewCreateInfo, nullptr, &imageView);
			if (result != VK_SUCCESS)
			{
				WL_CORE_ERROR("Failed to create Vulkan image view {0}", i);
				continue;
			}
			m_VkSwapchainImageViews[i] = imageView;

			// Create NVRHI texture from native Vulkan image
			nvrhi::TextureDesc desc;
			desc.width = m_Width;
			desc.height = m_Height;
			desc.format = nvrhi::Format::SBGRA8_UNORM;
			desc.isRenderTarget = true;

			m_VkBackBuffers[i] = m_Device->createHandleForNativeTexture(
				nvrhi::ObjectTypes::VK_Image,
				m_VkSwapchainImages[i],
				desc);

			// Create framebuffer
			nvrhi::FramebufferDesc fbDesc;
			fbDesc.addColorAttachment(m_VkBackBuffers[i]);
			m_VkFramebuffers[i] = m_Device->createFramebuffer(fbDesc);
		}

		// Create synchronization objects
		m_VkFences.resize(imageCount);
		m_VkImageAvailableSemaphores.resize(imageCount);
		m_VkRenderFinishedSemaphores.resize(imageCount);

		VkFenceCreateInfo fenceCreateInfo = {};
		fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

		VkSemaphoreCreateInfo semaphoreCreateInfo = {};
		semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

		for (uint32_t i = 0; i < imageCount; i++)
		{
			VkFence fence = VK_NULL_HANDLE;
			VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
			VkSemaphore renderFinishedSemaphore = VK_NULL_HANDLE;
			vkCreateFence(device, &fenceCreateInfo, nullptr, &fence);
			vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &imageAvailableSemaphore);
			vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &renderFinishedSemaphore);
			m_VkFences[i] = fence;
			m_VkImageAvailableSemaphores[i] = imageAvailableSemaphore;
			m_VkRenderFinishedSemaphores[i] = renderFinishedSemaphore;
		}

		// Create command pool
		VkCommandPoolCreateInfo commandPoolInfo = {};
		commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		commandPoolInfo.queueFamilyIndex = m_VkGraphicsQueueFamilyIndex;
		commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

		VkCommandPool commandPool = VK_NULL_HANDLE;
		result = vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool);
		if (result != VK_SUCCESS)
		{
			WL_CORE_ERROR("Failed to create Vulkan command pool");
		}
		m_VkCommandPool = commandPool;

		m_CurrentBackBufferIndex = 0;
		WL_CORE_INFO("Vulkan backbuffer framebuffers created");
	}

	void NVRHIContext::DestroyVulkanSwapChain()
	{
		WL_PROFILE_FUNCTION();

		VkDevice device = static_cast<VkDevice>(m_VkDevice);
		if (device)
			vkDeviceWaitIdle(device);

		// Destroy ImGui framebuffers first (they depend on swapchain image views)
		DestroyVulkanImGuiFramebuffers();

		if (device)
		{
			// Destroy synchronization objects created for swapchain
			for (VkFence fence : m_VkFences)
				if (fence != VK_NULL_HANDLE)
					vkDestroyFence(device, fence, nullptr);
			for (VkSemaphore sem : m_VkImageAvailableSemaphores)
				if (sem != VK_NULL_HANDLE)
					vkDestroySemaphore(device, sem, nullptr);
			for (VkSemaphore sem : m_VkRenderFinishedSemaphores)
				if (sem != VK_NULL_HANDLE)
					vkDestroySemaphore(device, sem, nullptr);
			if (m_VkCommandPool != VK_NULL_HANDLE)
				vkDestroyCommandPool(device, m_VkCommandPool, nullptr);
		}
		m_VkFences.clear();
		m_VkImageAvailableSemaphores.clear();
		m_VkRenderFinishedSemaphores.clear();
		m_VkCommandPool = nullptr;

		// Destroy image views
		if (device)
		{
			for (size_t i = 0; i < m_VkSwapchainImageViews.size(); i++)
			{
				if (m_VkSwapchainImageViews[i] != VK_NULL_HANDLE)
					vkDestroyImageView(device, m_VkSwapchainImageViews[i], nullptr);
			}
		}
		m_VkSwapchainImageViews.clear();
		m_VkSwapchainImages.clear();

		// NVRHI framebuffers are managed by NVRHI device, just clear them
		m_VkFramebuffers.clear();
		m_VkBackBuffers.clear();

		// Destroy swapchain
		if (m_VkSwapchain && device)
		{
			vkDestroySwapchainKHR(device, static_cast<VkSwapchainKHR>(m_VkSwapchain), nullptr);
			m_VkSwapchain = nullptr;
		}
		else if (m_VkSwapchain)
		{
			// Device was null but swapchain handle leaked - just clear
			m_VkSwapchain = nullptr;
		}
	}

	nvrhi::FramebufferHandle NVRHIContext::GetCurrentFramebuffer() const
	{
		if (m_API == RendererAPI::API::NVRHI_Vulkan)
		{
			if (m_VkFramebuffers.empty())
				return nullptr;
			return m_VkFramebuffers[m_CurrentBackBufferIndex];
		}

		if (m_BackBufferFramebuffers.empty())
			return nullptr;
		return m_BackBufferFramebuffers[m_CurrentBackBufferIndex];
	}

	nvrhi::TextureHandle NVRHIContext::GetCurrentBackBuffer() const
	{
		if (m_API == RendererAPI::API::NVRHI_Vulkan)
		{
			if (m_VkBackBuffers.empty())
				return nullptr;
			return m_VkBackBuffers[m_CurrentBackBufferIndex];
		}

		if (m_BackBuffers.empty())
			return nullptr;
		return m_BackBuffers[m_CurrentBackBufferIndex];
	}

	void NVRHIContext::SwapBuffers()
	{
		WL_PROFILE_FUNCTION();

		if (m_API == RendererAPI::API::NVRHI_Vulkan)
		{
			VkDevice device = static_cast<VkDevice>(m_VkDevice);
			VkQueue graphicsQueue = static_cast<VkQueue>(m_VkGraphicsQueue);
			VkQueue presentQueue = static_cast<VkQueue>(m_VkPresentQueue);
			VkSwapchainKHR swapchain = static_cast<VkSwapchainKHR>(m_VkSwapchain);

			if (!device || !swapchain || m_VkFences.empty())
				return;

			VkSemaphore waitSemaphores[] = {static_cast<VkSemaphore>(m_VkImageAvailableSemaphores[m_VkFrameIndex])};
			VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
			VkSemaphore signalSemaphores[] = {static_cast<VkSemaphore>(m_VkRenderFinishedSemaphores[m_VkFrameIndex])};

			VkSubmitInfo submitInfo = {};
			submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			submitInfo.waitSemaphoreCount = 1;
			submitInfo.pWaitSemaphores = waitSemaphores;
			submitInfo.pWaitDstStageMask = waitStages;

			VkCommandBuffer imGuiCmd = VK_NULL_HANDLE;
			if (IsVulkanImGuiInitialized() && !m_VkImGuiCommandBuffers.empty())
			{
				imGuiCmd = static_cast<VkCommandBuffer>(GetCurrentVulkanImGuiCommandBuffer());
				submitInfo.commandBufferCount = 1;
				submitInfo.pCommandBuffers = &imGuiCmd;
			}
			else
			{
				submitInfo.commandBufferCount = 0;
				submitInfo.pCommandBuffers = nullptr;
			}
			submitInfo.signalSemaphoreCount = 1;
			submitInfo.pSignalSemaphores = signalSemaphores;

			VkResult result = vkQueueSubmit(graphicsQueue, 1, &submitInfo, static_cast<VkFence>(m_VkFences[m_VkFrameIndex]));
			if (result != VK_SUCCESS)
			{
				WL_CORE_ERROR("Failed to submit Vulkan queue");
			}

			VkPresentInfoKHR presentInfo = {};
			presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
			presentInfo.waitSemaphoreCount = 1;
			presentInfo.pWaitSemaphores = signalSemaphores;
			presentInfo.swapchainCount = 1;
			presentInfo.pSwapchains = &swapchain;
			presentInfo.pImageIndices = &m_CurrentBackBufferIndex;

			result = vkQueuePresentKHR(presentQueue, &presentInfo);

			m_VkFrameIndex = (m_VkFrameIndex + 1) % m_VkImageCount;

			if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
			{
				DestroyVulkanSwapChain();
				CreateVulkanSwapChain();
				if (IsVulkanImGuiInitialized())
				{
					CreateVulkanImGuiFramebuffers();
					CreateVulkanImGuiCommandPoolAndBuffers();
				}
			}
			else if (result != VK_SUCCESS)
			{
				WL_CORE_ERROR("Failed to present Vulkan swapchain image");
			}
		}
		else
		{
			ExecuteNVRHICommandList();

			if (m_API == RendererAPI::API::NVRHI_DX11 || m_API == RendererAPI::API::NVRHI_DX12)
			{
				IDXGISwapChain3 *swapChain = static_cast<IDXGISwapChain3 *>(m_DXGISwapChain3);
				if (swapChain)
				{
					swapChain->Present(1, 0);
					m_CurrentBackBufferIndex = swapChain->GetCurrentBackBufferIndex();
				}
			}
		}
	}

	void NVRHIContext::Resize(uint32_t width, uint32_t height)
	{
		WL_PROFILE_FUNCTION();

		if (width == m_Width && height == m_Height)
			return;

		m_Width = width;
		m_Height = height;

		if (m_API == RendererAPI::API::NVRHI_Vulkan)
		{
			VkDevice device = static_cast<VkDevice>(m_VkDevice);
			if (device)
				vkDeviceWaitIdle(device);

			DestroyVulkanSwapChain();
			CreateVulkanSwapChain();

			if (IsVulkanImGuiInitialized())
			{
				CreateVulkanImGuiFramebuffers();
				CreateVulkanImGuiCommandPoolAndBuffers();
			}
		}
		else if (m_API == RendererAPI::API::NVRHI_DX11 || m_API == RendererAPI::API::NVRHI_DX12)
		{
			if (m_API == RendererAPI::API::NVRHI_DX12 && m_D3D12CommandQueue && m_D3D12ImGuiFence)
			{
				ID3D12CommandQueue *queue = static_cast<ID3D12CommandQueue *>(m_D3D12CommandQueue);
				ID3D12Fence *fence = static_cast<ID3D12Fence *>(m_D3D12ImGuiFence);
				HANDLE event = m_D3D12ImGuiFenceEvent;
				if (queue && fence && event)
				{
					fence->Signal(m_D3D12ImGuiFenceValue);
					if (fence->GetCompletedValue() < m_D3D12ImGuiFenceValue)
					{
						fence->SetEventOnCompletion(m_D3D12ImGuiFenceValue, event);
						WaitForSingleObject(event, INFINITE);
					}
					m_D3D12ImGuiFenceValue++;
				}
			}

			m_BackBuffers.clear();
			m_BackBufferFramebuffers.clear();

			HRESULT hr = static_cast<IDXGISwapChain3 *>(m_DXGISwapChain3)->ResizeBuffers(2, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
			if (FAILED(hr))
			{
				WL_CORE_ERROR("ResizeBuffers failed: {0:X}", (uint32_t)hr);
			}

			CreateBackBufferFramebuffers();
		}
	}

	void NVRHIContext::BeginFrame()
	{
		WL_PROFILE_FUNCTION();
		if (m_API != RendererAPI::API::NVRHI_Vulkan)
			return;

		VkDevice device = static_cast<VkDevice>(m_VkDevice);
		VkSwapchainKHR swapchain = static_cast<VkSwapchainKHR>(m_VkSwapchain);
		if (!device || !swapchain || m_VkFences.empty() || m_VkImageAvailableSemaphores.empty())
			return;

		if (m_VkFrameIndex < m_VkFences.size() && m_VkFences[m_VkFrameIndex])
		{
			vkWaitForFences(device, 1, reinterpret_cast<VkFence *>(&m_VkFences[m_VkFrameIndex]), VK_TRUE, UINT64_MAX);
			vkResetFences(device, 1, reinterpret_cast<VkFence *>(&m_VkFences[m_VkFrameIndex]));
		}

		uint32_t imageIndex;
		VkResult result = vkAcquireNextImageKHR(
			device,
			swapchain,
			UINT64_MAX,
			static_cast<VkSemaphore>(m_VkImageAvailableSemaphores[m_VkFrameIndex]),
			VK_NULL_HANDLE,
			&imageIndex);

		if (result == VK_ERROR_OUT_OF_DATE_KHR)
		{
			DestroyVulkanSwapChain();
			CreateVulkanSwapChain();
			if (IsVulkanImGuiInitialized())
			{
				CreateVulkanImGuiFramebuffers();
				CreateVulkanImGuiCommandPoolAndBuffers();
			}
			result = vkAcquireNextImageKHR(device, static_cast<VkSwapchainKHR>(m_VkSwapchain), UINT64_MAX,
										   static_cast<VkSemaphore>(m_VkImageAvailableSemaphores[m_VkFrameIndex]), VK_NULL_HANDLE, &imageIndex);
			if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
			{
				WL_CORE_ERROR("Failed to acquire Vulkan image after recreation");
				return;
			}
		}
		else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
		{
			WL_CORE_ERROR("Failed to acquire Vulkan swapchain image");
			return;
		}

		m_CurrentBackBufferIndex = imageIndex;

		if (!m_VkImGuiCommandBuffers.empty())
		{
			uint32_t cmdIndex = m_VkFrameIndex % static_cast<uint32_t>(m_VkImGuiCommandBuffers.size());
			VkCommandBuffer cmd = static_cast<VkCommandBuffer>(m_VkImGuiCommandBuffers[cmdIndex]);
			if (cmd)
				vkResetCommandBuffer(cmd, 0);
		}
	}

	void NVRHIContext::ExecuteNVRHICommandList()
	{
		WL_PROFILE_FUNCTION();
		if (m_Device && m_CommandList)
		{
			m_Device->executeCommandList(m_CommandList);
		}
	}

	VkFramebuffer NVRHIContext::GetVkCurrentImGuiFramebuffer() const
	{
		if (m_VkImGuiFramebuffers.empty())
			return VK_NULL_HANDLE;
		if (m_CurrentBackBufferIndex < m_VkImGuiFramebuffers.size())
			return m_VkImGuiFramebuffers[m_CurrentBackBufferIndex];
		return m_VkImGuiFramebuffers[0];
	}

	VkCommandBuffer NVRHIContext::GetCurrentVulkanImGuiCommandBuffer() const
	{
		if (m_VkImGuiCommandBuffers.empty())
			return VK_NULL_HANDLE;
		uint32_t idx = m_VkFrameIndex % static_cast<uint32_t>(m_VkImGuiCommandBuffers.size());
		return m_VkImGuiCommandBuffers[idx];
	}

	void *NVRHIContext::GetD3D12CurrentBackBufferResource() const
	{
		if (m_API != RendererAPI::API::NVRHI_DX12)
			return nullptr;
		if (m_BackBuffers.empty())
			return nullptr;
		if (m_CurrentBackBufferIndex >= m_BackBuffers.size())
			return nullptr;
		nvrhi::TextureHandle tex = m_BackBuffers[m_CurrentBackBufferIndex];
		if (!tex)
			return nullptr;
		return tex->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);
	}

	void NVRHIContext::CreateVulkanImGuiDescriptorPool()
	{
		WL_PROFILE_FUNCTION();
		if (m_VkImGuiDescriptorPool)
			return;
		VkDevice device = static_cast<VkDevice>(m_VkDevice);
		if (!device)
			return;

		VkDescriptorPoolSize poolSizes[] = {
			{VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
			{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
			{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
			{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
			{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
			{VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
			{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
			{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
			{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
			{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
			{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000}};
		VkDescriptorPoolCreateInfo poolInfo = {};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		poolInfo.maxSets = 1000 * 11;
		poolInfo.poolSizeCount = 11;
		poolInfo.pPoolSizes = poolSizes;

		VkResult result = vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_VkImGuiDescriptorPool);
		if (result != VK_SUCCESS)
		{
			WL_CORE_ERROR("Failed to create Vulkan ImGui descriptor pool");
			m_VkImGuiDescriptorPool = VK_NULL_HANDLE;
		}
		else
		{
			WL_CORE_INFO("Vulkan ImGui descriptor pool created");
		}
	}

	void NVRHIContext::DestroyVulkanImGuiDescriptorPool()
	{
		if (m_VkImGuiDescriptorPool == VK_NULL_HANDLE)
			return;
		VkDevice device = static_cast<VkDevice>(m_VkDevice);
		if (device)
			vkDestroyDescriptorPool(device, m_VkImGuiDescriptorPool, nullptr);
		m_VkImGuiDescriptorPool = VK_NULL_HANDLE;
	}

	void NVRHIContext::CreateVulkanImGuiRenderPass()
	{
		WL_PROFILE_FUNCTION();
		if (m_VkImGuiRenderPass != VK_NULL_HANDLE)
			return;
		VkDevice device = static_cast<VkDevice>(m_VkDevice);
		if (!device)
			return;

		VkFormat format = static_cast<VkFormat>(m_VkSwapchainImageFormat);
		if (format == VK_FORMAT_UNDEFINED)
			format = VK_FORMAT_B8G8R8A8_SRGB;

		VkAttachmentDescription attachment = {};
		attachment.format = format;
		attachment.samples = VK_SAMPLE_COUNT_1_BIT;
		attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkAttachmentReference colorRef = {};
		colorRef.attachment = 0;
		colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass = {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorRef;

		VkSubpassDependency dependency = {};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.srcAccessMask = 0;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		VkRenderPassCreateInfo rpInfo = {};
		rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		rpInfo.attachmentCount = 1;
		rpInfo.pAttachments = &attachment;
		rpInfo.subpassCount = 1;
		rpInfo.pSubpasses = &subpass;
		rpInfo.dependencyCount = 1;
		rpInfo.pDependencies = &dependency;

		VkResult result = vkCreateRenderPass(device, &rpInfo, nullptr, &m_VkImGuiRenderPass);
		if (result != VK_SUCCESS)
		{
			WL_CORE_ERROR("Failed to create Vulkan ImGui render pass");
			m_VkImGuiRenderPass = VK_NULL_HANDLE;
		}
		else
		{
			WL_CORE_INFO("Vulkan ImGui render pass created (format {0})", (int)format);
		}
	}

	void NVRHIContext::DestroyVulkanImGuiRenderPass()
	{
		if (m_VkImGuiRenderPass == VK_NULL_HANDLE)
			return;
		VkDevice device = static_cast<VkDevice>(m_VkDevice);
		if (device)
			vkDestroyRenderPass(device, m_VkImGuiRenderPass, nullptr);
		m_VkImGuiRenderPass = VK_NULL_HANDLE;
	}

	void NVRHIContext::CreateVulkanImGuiFramebuffers()
	{
		WL_PROFILE_FUNCTION();
		DestroyVulkanImGuiFramebuffers();
		if (m_VkImGuiRenderPass == VK_NULL_HANDLE || m_VkSwapchainImageViews.empty())
			return;
		VkDevice device = static_cast<VkDevice>(m_VkDevice);
		if (!device)
			return;

		m_VkImGuiFramebuffers.resize(m_VkSwapchainImageViews.size());
		for (size_t i = 0; i < m_VkSwapchainImageViews.size(); i++)
		{
			VkImageView view = m_VkSwapchainImageViews[i];
			VkFramebufferCreateInfo fbInfo = {};
			fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			fbInfo.renderPass = m_VkImGuiRenderPass;
			fbInfo.attachmentCount = 1;
			fbInfo.pAttachments = &view;
			fbInfo.width = m_Width;
			fbInfo.height = m_Height;
			fbInfo.layers = 1;

			VkFramebuffer framebuffer = VK_NULL_HANDLE;
			VkResult result = vkCreateFramebuffer(device, &fbInfo, nullptr, &framebuffer);
			if (result != VK_SUCCESS)
			{
				WL_CORE_ERROR("Failed to create Vulkan ImGui framebuffer {0}", i);
				m_VkImGuiFramebuffers[i] = VK_NULL_HANDLE;
			}
			else
			{
				m_VkImGuiFramebuffers[i] = framebuffer;
			}
		}
		WL_CORE_INFO("Vulkan ImGui framebuffers created ({0})", m_VkImGuiFramebuffers.size());
	}

	void NVRHIContext::DestroyVulkanImGuiFramebuffers()
	{
		if (m_VkImGuiFramebuffers.empty())
			return;
		VkDevice device = static_cast<VkDevice>(m_VkDevice);
		if (device)
		{
			for (VkFramebuffer fb : m_VkImGuiFramebuffers)
			{
				if (fb != VK_NULL_HANDLE)
					vkDestroyFramebuffer(device, fb, nullptr);
			}
		}
		m_VkImGuiFramebuffers.clear();
	}

	void NVRHIContext::CreateVulkanImGuiCommandPoolAndBuffers()
	{
		WL_PROFILE_FUNCTION();
		DestroyVulkanImGuiCommandPoolAndBuffers();
		VkDevice device = static_cast<VkDevice>(m_VkDevice);
		if (!device)
			return;

		VkCommandPoolCreateInfo poolInfo = {};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.queueFamilyIndex = m_VkGraphicsQueueFamilyIndex;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

		VkResult result = vkCreateCommandPool(device, &poolInfo, nullptr, &m_VkImGuiCommandPool);
		if (result != VK_SUCCESS)
		{
			WL_CORE_ERROR("Failed to create Vulkan ImGui command pool");
			return;
		}

		uint32_t count = m_VkImageCount;
		if (count == 0)
			count = 2;
		m_VkImGuiCommandBuffers.resize(count);
		VkCommandBufferAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = m_VkImGuiCommandPool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = count;

		std::vector<VkCommandBuffer> tmp(count);
		result = vkAllocateCommandBuffers(device, &allocInfo, tmp.data());
		if (result != VK_SUCCESS)
		{
			WL_CORE_ERROR("Failed to allocate Vulkan ImGui command buffers");
			vkDestroyCommandPool(device, m_VkImGuiCommandPool, nullptr);
			m_VkImGuiCommandPool = VK_NULL_HANDLE;
			m_VkImGuiCommandBuffers.clear();
			return;
		}
		for (uint32_t i = 0; i < count; i++)
			m_VkImGuiCommandBuffers[i] = tmp[i];

		WL_CORE_INFO("Vulkan ImGui command buffers created ({0})", count);
	}

	void NVRHIContext::DestroyVulkanImGuiCommandPoolAndBuffers()
	{
		if (m_VkImGuiCommandPool == VK_NULL_HANDLE)
			return;
		VkDevice device = static_cast<VkDevice>(m_VkDevice);
		if (device)
		{
			vkDestroyCommandPool(device, m_VkImGuiCommandPool, nullptr);
		}
		m_VkImGuiCommandPool = VK_NULL_HANDLE;
		m_VkImGuiCommandBuffers.clear();
	}

	bool NVRHIContext::InitVulkanImGuiResources()
	{
		WL_PROFILE_FUNCTION();
		if (IsVulkanImGuiInitialized())
			return true;

		CreateVulkanImGuiDescriptorPool();
		if (!m_VkImGuiDescriptorPool)
			return false;

		CreateVulkanImGuiRenderPass();
		if (!m_VkImGuiRenderPass)
		{
			DestroyVulkanImGuiDescriptorPool();
			return false;
		}

		CreateVulkanImGuiFramebuffers();
		CreateVulkanImGuiCommandPoolAndBuffers();

		WL_CORE_INFO("Vulkan ImGui resources initialized");
		return true;
	}

	void NVRHIContext::ShutdownVulkanImGuiResources()
	{
		WL_PROFILE_FUNCTION();
		VkDevice device = static_cast<VkDevice>(m_VkDevice);
		if (device)
			vkDeviceWaitIdle(device);

		DestroyVulkanImGuiCommandPoolAndBuffers();
		DestroyVulkanImGuiFramebuffers();
		DestroyVulkanImGuiRenderPass();
		DestroyVulkanImGuiDescriptorPool();
	}

}
