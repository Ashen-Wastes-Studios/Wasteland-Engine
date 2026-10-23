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

// NVRHI message callback: routes backend errors (failed Map, invalid params...)
// into the engine log. Without this the device's messageCallback is null and
// every backend error path dereferences it and crashes.
namespace
{
	class NVRHILogCallback : public nvrhi::IMessageCallback
	{
	public:
		void *context = nullptr; // NVRHIContext* (void* to avoid header dependency in anonymous namespace)

		void message(nvrhi::MessageSeverity severity, const char *messageText) override
		{
			switch (severity)
			{
			case nvrhi::MessageSeverity::Info:
				WL_CORE_TRACE("NVRHI: {0}", messageText);
				break;
			case nvrhi::MessageSeverity::Warning:
				WL_CORE_WARN("NVRHI: {0}", messageText);
				break;
			case nvrhi::MessageSeverity::Error:
			case nvrhi::MessageSeverity::Fatal:
			default:
				WL_CORE_ERROR("NVRHI: {0}", messageText);
				// NVRHI catches vk::DeviceLostError / DX12 GetDeviceRemovedReason
				// internally and routes them here as Error severity. Mark the
				// owning context as lost so the frame loop can skip rendering
				// and trigger recovery instead of cascading into backend init
				// failures ("No backend initialized, skipping frame").
				// DEVICE LOSS IS DETECTED BY MATCHING AN EXACT NVRHI ERROR STRING.
				//
				// There is no structured device-lost signal exposed by NVRHI, so this hooks the
				// message callback and pattern-matches the text. The literal is not invented -- it
				// appears verbatim in exactly two places in the vendored backend:
				//
				//     vendor/nvrhi/src/d3d12/d3d12-device.cpp:648
				//     vendor/nvrhi/src/vulkan/vulkan-queue.cpp:200
				//
				// both as MessageSeverity::Error. So the match is currently correct for DX12 and
				// Vulkan. The fragility is that a vendored NVRHI upgrade that reworded either site
				// would silently disable device-loss detection -- no compile error, and the only
				// symptom is the old cascading init failures this code exists to prevent.
				//
				// The context pointer is null-checked because the callback is a namespace-scope
				// singleton (s_NVRHILogCallback) shared across contexts; it is null until a context
				// assigns itself in Init. A backend error during early init would otherwise
				// dereference it.
				//
				// The static_cast names the class explicitly because the callback lives in an
				// anonymous namespace above the Wasteland namespace, and the class is only
				// forward-declarable there.
				if (context && strstr(messageText, "Device Removed!"))
				{
					static_cast<class Wasteland::NVRHIContext*>(context)->SetDeviceLost();
				}
				break;
			}
		}
	};
	NVRHILogCallback s_NVRHILogCallback;
}

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
			// THE SRV HEAP IS RELEASED HERE RATHER THAN IN DestroyD3D12ImGuiCommandObjects, WHICH
			// HANDLES THE RTV HEAP.
			//
			// Every other DX12 ImGui resource -- fence event, fence, command list, command
			// allocator, RTV heap -- is torn down in DestroyD3D12ImGuiCommandObjects. Only the
			// SRV heap is handled inline here. Nothing is leaked; the split is just inconsistent.
			//
			// The CPU and GPU handle copies are zeroed above the release rather than after it,
			// which also protects against a stale handle being used if the release path is
			// interrupted.
			//
			// WHY IT MATTERS ENOUGH TO NOTE: DestroyD3D12ImGuiCommandObjects is callable on its
			// own -- it is called from the destructor AND from the backend-switch path -- so a
			// caller expecting it to fully release DX12 ImGui resources would leave this heap
			// alive. For a static-library engine torn down at exit that is harmless; for a
			// mid-process backend switch it would leak one descriptor heap per switch.
			//
			// Both heaps are ID3D12DescriptorHeap but are not interchangeable: the RTV heap is
			// written by CreateRenderTargetView during backbuffer setup and read by ImGuiLayer's
			// `rtvHandle.ptr += idx * rtvSize`, while the SRV heap serves ImGui font textures.
			if (m_D3D12ImGuiSRVHeap)
			{
				static_cast<ID3D12DescriptorHeap *>(m_D3D12ImGuiSRVHeap)->Release();
				m_D3D12ImGuiSRVHeap = nullptr;
			}
		}

				// Release D3D11 RTVs cached for ImGui
	for (auto* rtv : m_D3D11RTVs) if (rtv) rtv->Release();
	m_D3D11RTVs.clear();

		// Release NVRHI objects — order matters: every handle that keeps the
		// NVRHI Device alive (backbuffers, framebuffers, render-target
		// override, command list) must be released BEFORE the native device
		// is destroyed below. Otherwise the NVRHI Device object (with its
		// Queue timeline semaphores) outlives vkDestroyDevice / the D3D
		// Release calls: the driver then destroys a device with live
		// children (validation errors + hard AV in nvoglv64, and the
		// corrupted driver state poisons the NEXT device creation, which
		// crashes in nvrhi::vulkan::Queue::Queue).
		m_ActiveRenderTarget = nullptr;
		m_BackBufferFramebuffers.clear();
		m_BackBuffers.clear();
		m_VkFramebuffers.clear();
		m_VkBackBuffers.clear();
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
			DestroyVulkan();
		}
	}

		void NVRHIContext::DestroyVulkan()
		{
		// Tear down all Vulkan natives. Safe on partial state (failed init) and
		// resets every handle so a retry starts clean. NVRHI handles must already
		// be released by the caller (see destructor ordering).
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

			if (m_VkCommandPool != VK_NULL_HANDLE)
				vkDestroyCommandPool(device, m_VkCommandPool, nullptr);

			for (size_t i = 0; i < m_VkSwapchainImageViews.size(); i++)
			{
				if (m_VkSwapchainImageViews[i] != VK_NULL_HANDLE)
					vkDestroyImageView(device, m_VkSwapchainImageViews[i], nullptr);
			}

			if (m_VkSwapchain != VK_NULL_HANDLE)
				vkDestroySwapchainKHR(device, m_VkSwapchain, nullptr);
		}

		m_VkFences.clear();
		m_VkImageAvailableSemaphores.clear();
		m_VkRenderFinishedSemaphores.clear();
		m_VkSwapchainImageViews.clear();
		m_VkSwapchainImages.clear();
		m_VkSwapchain = VK_NULL_HANDLE;
		m_VkCommandPool = VK_NULL_HANDLE;

		if (m_VkDevice != VK_NULL_HANDLE)
		{
			vkDeviceWaitIdle(m_VkDevice);
			vkDestroyDevice(m_VkDevice, nullptr);
			m_VkDevice = VK_NULL_HANDLE;
		}
		m_VkGraphicsQueue = VK_NULL_HANDLE;
		m_VkPresentQueue = VK_NULL_HANDLE;
		m_VkPhysicalDevice = VK_NULL_HANDLE;

		if (m_VkSurface != VK_NULL_HANDLE)
		{
			if (m_VkInstance != VK_NULL_HANDLE)
				vkDestroySurfaceKHR(m_VkInstance, m_VkSurface, nullptr);
			m_VkSurface = VK_NULL_HANDLE;
		}

		if (m_VkInstance != VK_NULL_HANDLE)
		{
			vkDestroyInstance(m_VkInstance, nullptr);
			m_VkInstance = VK_NULL_HANDLE;
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
			CreateDeviceVulkanWithRetry();
			break;
		default:
			WL_CORE_ASSERT(false, "Unsupported NVRHI API");
			break;
		}

		// THE SWAPCHAIN IS CREATED BEFORE THE NULL-DEVICE CHECK THAT FOLLOWS IT.
		//
		// The comment two lines below claims "Any of the Create* paths above can fail
		// gracefully (logged); never dereference a null device here" -- but that guarantee
		// only holds for this line if CreateSwapChain itself is null-safe. It is called
		// unconditionally, while the `if (!m_Device)` bail-out comes after it.
		//
		// So the ordering is: create device -> CreateSwapChain -> check device. If device
		// creation failed, the swapchain path has already run against a null or broken
		// device.
		//
		// Note the Vulkan path is handled separately and correctly: the swapchain is created
		// INSIDE CreateDeviceVulkan, and this block is guarded by `m_API == Vulkan &&
		// m_Device`, so it only runs once a device exists. The unguarded CreateSwapChain above
		// is the DX11/DX12 path.
		CreateSwapChain();
		// VULKAN IS THE ONLY BACKEND WHOSE SWAPCHAIN IS BUILT INSIDE DEVICE CREATION.
		//
		// CreateVulkanSwapChain uses native Vulkan APIs rather than DXGI, so it cannot be
		// reached through the shared CreateSwapChain path. The m_Device check makes this
		// conditional on device creation having actually succeeded, which is the ordering
		// the DX11/DX12 path above lacks.
		//
		// The comment below notes CreateSwapChain already calls CreateBackBufferFramebuffers
		// for the D3D backends, so there is deliberately no second call here -- a previous
		// double-initialization was causing needless Release/GetBuffer pairs.
		if (m_API == RendererAPI::API::NVRHI_Vulkan && m_Device)
		{
			// Vulkan swapchain is created inside CreateDeviceVulkan after device creation
			// and uses native Vulkan APIs rather than DXGI
			CreateVulkanSwapChain();
		}
		// Note: CreateSwapChain() already calls CreateBackBufferFramebuffers() for DX11/DX12
		// so no second call here (was double-init causing needless Release/GetBuffer)

		// Any of the Create* paths above can fail gracefully (logged); never
		// dereference a null device here.
		if (!m_Device)
		{
			WL_CORE_ERROR("NVRHI Context initialization failed: no device (see errors above)");
			return;
		}

		// Create command list
		m_CommandList = m_Device->createCommandList();

		// Route NVRHI device-removal messages to m_IsDeviceLost
		s_NVRHILogCallback.context = this;

		WL_CORE_INFO("NVRHI Context initialized successfully");
	}

	void NVRHIContext::CreateDeviceDX11()
	{
		WL_PROFILE_FUNCTION();

		// Create D3D11 device
		ID3D11Device *device = nullptr;
		ID3D11DeviceContext *context = nullptr;

		UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
		// Debug layer only on explicit opt-in: the D3D11 debug layer itself crashes
		// in d3d11.dll on driver 591.59 with FLIP_DISCARD swapchains (faults inside
		// DrawIndexed validation), so WL_DEBUG no longer enables it automatically.
		char d3dDebugEnv[8] = {};
		if (GetEnvironmentVariableA("WL_D3D_DEBUG", d3dDebugEnv, sizeof(d3dDebugEnv)) && d3dDebugEnv[0] == '1')
			creationFlags |= D3D11_CREATE_DEVICE_DEBUG;

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

		// IN RELEASE THIS ASSERT COMPILES AWAY, AND THE NULL HANDLES ARE STORED ANYWAY.
		//
		// WL_CORE_ASSERT expands to nothing outside debug builds -- not merely to a discarded
		// branch. So in Release a failed D3D11CreateDevice leaves device and context null,
		// and the next three lines store those nulls into m_D3D11Device and m_D3D11Context.
		//
		// The NVRHI device is then created from a null context:
		//     deviceDesc.context = context;
		//     m_Device = nvrhi::d3d11::createDevice(deviceDesc);
		// which either returns null or produces a device whose backing context is unusable.
		// The function then unconditionally logs "D3D11 device created successfully", so the
		// log claims success on a failure.
		//
		// THE MITIGATION IS DOWNSTREAM BUT ONLY PARTIAL. Init does check `if (!m_Device)` and
		// logs "no device (see errors above)" -- but that check happens AFTER CreateSwapChain,
		// so the swapchain is already being built against the broken device. See the Init
		// entry.
		//
		// A `if (FAILED(hr)) { WL_CORE_ERROR(...); return; }` here would make the DX11 path
		// behave like the Vulkan one. The retry before this line already covers the common
		// debug-layer failure, so reaching here means hardware creation genuinely failed.
		WL_CORE_ASSERT(SUCCEEDED(hr), "Failed to create D3D11 device");

		m_D3D11Device = device;
		m_D3D11Context = context;

		// Create NVRHI device
		nvrhi::d3d11::DeviceDesc deviceDesc;
		deviceDesc.context = context;
		deviceDesc.messageCallback = &s_NVRHILogCallback;
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

		if (FAILED(hr) || !device)
		{
			WL_CORE_ERROR("Failed to create D3D12 device (HRESULT 0x{0:X})", (uint32_t)hr);
			return;
		}

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
		deviceDesc.errorCB = &s_NVRHILogCallback;
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
			m_D3D12ImGuiSRVCapacity = heapDesc.NumDescriptors;

			WL_CORE_INFO("D3D12 ImGui SRV descriptor heap created (64 descriptors)");
		}
		else
		{
			WL_CORE_ERROR("Failed to create D3D12 ImGui SRV descriptor heap");
		}

		CreateD3D12ImGuiCommandObjects();

		WL_CORE_INFO("D3D12 device created successfully");
	}

	D3D12_GPU_DESCRIPTOR_HANDLE NVRHIContext::GetOrCreateImGuiDescriptor(UINT slot, void *resource) const
	{
		D3D12_GPU_DESCRIPTOR_HANDLE none = {};
		if (!resource || !m_D3D12ImGuiSRVHeap || slot == 0)
			return none;

		// Slot 0 is the font atlas: ImGui_ImplDX12_Init writes it directly, so
		// overwriting it would break every glyph.
		ID3D12Device *device = static_cast<ID3D12Device *>(m_D3D12Device);
		if (!device)
			return none;

		UINT srvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		// ID3D12DescriptorHeap exposes no total byte size, so the capacity is
		// the count recorded when the heap was created.
		ID3D12DescriptorHeap *heap = static_cast<ID3D12DescriptorHeap *>(m_D3D12ImGuiSRVHeap);
		UINT capacity = m_D3D12ImGuiSRVCapacity;
		if (slot >= capacity)
		{
			WL_CORE_WARN("ImGui descriptor slot {0} exceeds heap capacity {1}", slot, capacity);
			return none;
		}

		UINT offset = slot * srvSize;
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += offset;

		// Take the format from the resource itself. Hardcoding
		// R8G8B8A8_UNORM here faulted the GPU ("NVRHI: Device Removed!") against
		// the Nova output, which is R32G32B32A32_FLOAT -- a format that does not
		// match the view is undefined behaviour, not a warning.
		ID3D12Resource *res = static_cast<ID3D12Resource *>(resource);
		D3D12_RESOURCE_DESC resDesc = res->GetDesc();
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = resDesc.Format;
		srvDesc.ViewDimension = (resDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
			? D3D12_SRV_DIMENSION_TEXTURE3D
			: D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		// Whole mip range: Nova writes one mip, but a swap-chain texture may have
		// several, and asking for fewer than exist is fine while asking for more
		// than exist is invalid.
		srvDesc.Texture2D.MipLevels = resDesc.MipLevels;
		srvDesc.Texture2D.MostDetailedMip = 0;
		device->CreateShaderResourceView(res, &srvDesc, cpu);

		D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += offset;
		return gpu;
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
		// THE COMMAND LIST IS CLOSED IMMEDIATELY, BECAUSE D3D12 CREATES IT IN THE OPEN STATE.
		//
		// CreateCommandList returns an OPEN list. Every draw call in ImGuiLayer assumes a
		// closed list that can be reset and recorded into per frame, so it is closed here at
		// creation.
		//
		// Note the failure cleanup is correct and asymmetric by design: if the list creation
		// fails, the allocator is released AND nulled before returning, so there is no
		// dangling pointer for the destructor to double-release. The fence and heap failures
		// below only WARN -- they are treated as non-fatal, leaving the ImGui path without
		// synchronisation rather than aborting device setup.
		//
		// A failure creating the allocator is fatal and returns early, so the later code
		// never sees a null device.
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
			// TWO RTV DESCRIPTORS, MATCHING THE SWAPCHAIN'S BUFFER COUNT OF 2.
			//
			// The swapchain is created with BufferCount = 2 in CreateSwapChain, so one RTV per
			// backbuffer is exactly right -- no spare descriptor and no shortfall.
			//
			// There IS a guard for a driver that ignores the request: CreateSwapChain checks
			// actual.BufferCount != 2 and warns that it "will cause GetBuffer(1) INVALID_CALL".
			// That is the same driver-591.59 behaviour documented on the swap effect choice, and
			// it is worth knowing the two are coupled -- if a driver ever clamped DOWN to 1 the
			// heap would be oversized rather than wrong, which is the harmless direction.
			//
			// The increment size is queried rather than assumed:
			//     m_D3D12RTVDescriptorSize = device->GetDescriptorHandleIncrementSize(...)
			// Descriptor handles are NOT fixed-width byte offsets, and the increment size varies
			// by GPU, so a hardcoded stride would write RTVs into the wrong slots.
			//
			// Creation is guarded on !m_D3D12ImGuiRTVHeap, so this is idempotent and a second
			// call neither leaks nor duplicates the heap.
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
		// TEARDOWN ORDER IS INVERTED DEPENDENCY ORDER, AND IT MATTERS FOR THE FENCE PAIR.
		//
		// The event handle is closed BEFORE the fence is released. Both are nulled after
		// their own release, so a second call is a no-op and this function is idempotent.
		//
		// Closing the event first is the safe order: nothing can be waiting on it once the
		// handle is gone, so releasing the fence cannot leave a waiter stranded on a dead
		// handle. Reversing it would leave a window where a waiter holds the event while the
		// fence it refers to is gone.
		//
		// NOTE WHAT THIS FUNCTION DOES NOT RELEASE: m_D3D12ImGuiSRVHeap. That heap is
		// released in the destructor instead, three statements after this function returns.
		// So the two ImGui heaps are torn down in two different places --
		// DestroyD3D12ImGuiCommandObjects handles the RTV heap, the destructor handles the SRV
		// heap and nulls both CPU/GPU handle copies. Nothing leaks, but the split means
		// calling this function alone leaves the SRV heap alive.
		//
		// m_D3D12ImGuiFenceValue is deliberately not reset. It is the fence's current
		// completed value, and it is meaningless without the fence itself.
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
		VkSurfaceKHR surface = static_cast<VkSurfaceKHR>(m_VkSurface);

		// Enumerate physical devices
		uint32_t deviceCount = 0;
		vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
		if (deviceCount == 0)
		{
			WL_CORE_ERROR("Failed to find GPUs with Vulkan support");
			return false;
		}

		// VERIFIED AGAINST THE VENDORED NVRHI SOURCE, because the claim about it throwing is
		// not supported by that source.
		//
		// The comment above this block says NVRHI's Queue constructor "throws (fatal) if the
		// timeline feature wasn't enabled on the device". Searching the vendored backend:
		//
		//   * TimelineSemaphore appears in exactly ONE file under
		//     vendor/nvrhi/src/vulkan/ -- vulkan-queue.cpp.
		//   * That file has no throw in the submit path. It has a try/catch that catches
		//     vk::DeviceLostError and routes it to the message callback as
		//     MessageSeverity::Error with the literal "Device Removed!" -- which is where
		//     the device-loss string this file matches on actually originates.
		//
		// So the requirement is still real -- NVRHI records vk::TimelineSemaphoreSubmitInfo
		// unconditionally, so timeline semaphores must be enabled -- but the failure mode as
		// described is not what the vendored code does. Enabling them is still correct and
		// necessary; only the stated consequence is unverified.
		//
		// The same search is what establishes the "Device Removed!" literal has exactly two
		// origins in the vendored tree -- vulkan-queue.cpp:200 here, and
		// d3d12-device.cpp:648 -- which is what makes the strstr match in the log callback
		// safe today.
		//
		// Timeline and synchronization2 are both checked here because NVRHI records
		// vkCmdPipelineBarrier2 unconditionally as well.
		std::vector<VkPhysicalDevice> devices(deviceCount);
		vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

		// Suitability check: the chosen GPU must have a graphics queue, a queue
		// that can present to our surface, the swapchain extension, and timeline
		// semaphores (core in 1.2+ / KHR on 1.1) which NVRHI unconditionally
		// requires — its Queue ctor throws (fatal, see CreateDeviceVulkan) if the
		// timeline feature wasn't enabled on the device.
		// SURFACE SUPPORT IS ONLY CHECKED WHEN A SURFACE EXISTS, so headless or pre-surface
		// device selection still works.
		//
		// Every present-support test is guarded by `surface != VK_NULL_HANDLE`. With no
		// surface -- an offscreen or headless context, or selection running before the surface
		// is created -- hasPresent is simply never set and is not required, so a GPU with no
		// present capability is still considered suitable.
		//
		// That is the right behaviour, but it means suitability is STRICTLY STRICTER once a
		// window exists. A GPU that would be accepted headless can be rejected when the
		// surface appears -- the check is not merely skipped, it changes the answer.
		//
		// The two-pass loop below is what makes this tolerable in practice: a rejected discrete
		// GPU does not end selection, it falls back to a suitable integrated one.
		auto IsDeviceSuitable = [&](VkPhysicalDevice dev, const char *name) -> bool {
			uint32_t qCount = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, nullptr);
			if (qCount == 0)
			{
				WL_CORE_WARN("  Vulkan GPU '{0}' rejected: no queue families", name);
				return false;
			}
			std::vector<VkQueueFamilyProperties> qProps(qCount);
			vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, qProps.data());

			bool hasGraphics = false, hasPresent = false;
			for (uint32_t i = 0; i < qCount; i++)
			{
				if (qProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
					hasGraphics = true;
				if (surface != VK_NULL_HANDLE)
				{
					VkBool32 presentSupport = VK_FALSE;
					vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &presentSupport);
					if (presentSupport)
						hasPresent = true;
				}
			}
			if (!hasGraphics)
			{
				WL_CORE_WARN("  Vulkan GPU '{0}' rejected: no graphics queue family", name);
				return false;
			}
			if (surface != VK_NULL_HANDLE && !hasPresent)
			{
				WL_CORE_WARN("  Vulkan GPU '{0}' rejected: no present support for window surface", name);
				return false;
			}

			uint32_t extCount = 0;
			vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
			std::vector<VkExtensionProperties> exts(extCount);
			vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, exts.data());
			bool hasSwapchain = false;
			for (auto &e : exts)
			{
				if (strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
				{
					hasSwapchain = true;
					break;
				}
			}
			if (!hasSwapchain)
			{
				WL_CORE_WARN("  Vulkan GPU '{0}' rejected: missing VK_KHR_swapchain", name);
				return false;
			}

			VkPhysicalDeviceVulkan12Features f12 = {};
			f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
			VkPhysicalDeviceTimelineSemaphoreFeatures fKHR = {};
			fKHR.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
			VkPhysicalDeviceVulkan13Features f13 = {};
			f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
			f12.pNext = &fKHR;
			fKHR.pNext = &f13;
			VkPhysicalDeviceFeatures2 f2 = {};
			f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
			f2.pNext = &f12;
			vkGetPhysicalDeviceFeatures2(dev, &f2);
			if (!f12.timelineSemaphore && !fKHR.timelineSemaphore)
			{
				WL_CORE_WARN("  Vulkan GPU '{0}' rejected: no timeline semaphore support (NVRHI requires it)", name);
				return false;
			}
			if (!f13.synchronization2)
			{
				WL_CORE_WARN("  Vulkan GPU '{0}' rejected: no synchronization2 support (NVRHI uses vkCmdPipelineBarrier2)", name);
				return false;
			}
			return true;
		};

		// Two passes: prefer a suitable discrete GPU, fall back to any suitable GPU.
		VkPhysicalDevice chosen = VK_NULL_HANDLE;
		for (int pass = 0; pass < 2 && chosen == VK_NULL_HANDLE; pass++)
		{
			for (const auto &dev : devices)
			{
				VkPhysicalDeviceProperties properties;
				vkGetPhysicalDeviceProperties(dev, &properties);
				// TWO PASSES: DISCRETE FIRST, THEN ANYTHING ELSE. The XOR SKIPS THE WRONG PASS.
				//
				//     if ((pass == 0) != isDiscrete) continue;
				//
				// On pass 0 a discrete GPU is kept and an integrated one skipped; on pass 1 the
				// reverse. Expressed as a boolean inequality rather than an if/else so one line covers
				// both directions.
				//
				// WITHIN A PASS, THE FIRST SUITABLE DEVICE WINS -- there is no ranking by memory, type,
				// or vendor. On a multi-GPU machine that means enumeration order decides, which the
				// Vulkan spec leaves to the driver.
				//
				// A discrete GPU with no timeline semaphores is therefore skipped in pass 0 and an
				// integrated one is chosen, rather than the discrete one being used with a fallback
				// synchronisation path. Given NVRHI throws from its Queue constructor without timeline
				// semaphores, that is the correct trade: a working integrated device beats a crashing
				// discrete one.
				//
				// The outer loop condition also stops as soon as a device is chosen, so pass 1 never
				// runs if pass 0 succeeded.
				bool isDiscrete = (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
				if ((pass == 0) != isDiscrete)
					continue;
				if (IsDeviceSuitable(dev, properties.deviceName))
				{
					chosen = dev;
					break;
				}
			}
		}

		if (chosen == VK_NULL_HANDLE)
		{
			WL_CORE_ERROR("Failed to find a suitable Vulkan GPU (need graphics+present queues, swapchain + timeline semaphore support)");
			return false;
		}

		m_VkPhysicalDevice = chosen;

		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties(chosen, &properties);
		WL_CORE_INFO("Selected Vulkan GPU: {0}", properties.deviceName);

		return true;
	}

	void NVRHIContext::GetVulkanQueueFamilyIndices()
	{
		VkPhysicalDevice physicalDevice = static_cast<VkPhysicalDevice>(m_VkPhysicalDevice);
		VkSurfaceKHR surface = static_cast<VkSurfaceKHR>(m_VkSurface);

		// THE COUNT-THEN-FILL PATTERN IS THE ONLY CORRECT WAY TO SIZE THIS.
		//
		// vkGetPhysicalDeviceQueueFamilyProperties takes a capacity in/out: called with a null
		// array it writes the required count and nothing else, and called again with a
		// sized array it fills that many entries.
		//
		// m_VkSurface IS NULL-SAFE HERE BECAUSE THE PRESENCE CHECK IS NOT GUARDED. Unlike
		// IsDeviceSuitable above, which skips every present test when surface ==
		// VK_NULL_HANDLE, this calls vkGetPhysicalDeviceSurfaceSupportKHR unconditionally.
		// That means this function REQUIRES a surface -- a pre-surface or headless init that
		// reached here would pass VK_NULL_HANDLE into a query that expects a valid
		// surface handle.
		//
		// The practical consequence of the fallbacks below is that such a call still produces
		// usable indices rather than crashing, which is likely why it has gone unnoticed.
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

		// THE FALLBACKS MASK FAILURE RATHER THAN REPORTING IT -- AND THE PRESENT FALLBACK IS
		// THE MORE DANGEROUS OF THE TWO.
		//
		// Graphics falls back to family 0. On almost every discrete GPU family 0 IS graphics,
		// so this fallback is usually harmless.
		//
		// Present falls back to m_VkGraphicsQueueFamilyIndex, i.e. it ASSUMES graphics can
		// present. That assumption is false on the two configurations it is most likely to
		// encounter:
		//
		//   * a system without a display, where the surface is null or support is absent,
		//   * a GPU where presentation lives on a separate family.
		//
		// A wrong present family does not fail at creation -- vkQueuePresentKHR accepts it and
		// returns an error only at present time, per frame. So this degrades into a stream of
		// failed presents instead of one clear diagnostic.
		//
		// Logging the failure here would cost one line and turn a per-frame symptom into an
		// init-time one. The "should not happen on valid device" comment is accurate but has
		// no corresponding check, so the condition it describes is unobserved.
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
		// A FAILED SUBMIT IS LOGGED BUT THE PRESENT STILL GOES AHEAD.
		//
		// The error branch only logs; execution falls through to vkQueuePresentKHR with the
		// same signal semaphore the submit was supposed to signal. That semaphore will never
		// be signalled, so the present blocks indefinitely on pWaitSemaphores -- a hang
		// rather than a crash, which is a harder failure to diagnose.
		//
		// An early return here would leave the swapchain unpresented, which at least surfaces
		// as a visibly frozen window rather than a stalled queue.
		//
		// The three Vulkan failures are distinguished rather than lumped together, which is
		// correct:
		//   * VK_ERROR_OUT_OF_DATE_KHR and VK_SUBOPTIMAL_KHR are RECOVERED from by destroying
		//     and recreating the swapchain, plus the ImGui framebuffers and command buffers
		//     that depend on it. This is the resize-minimise path, not an error.
		//   * anything else is logged as a genuine presentation failure.
		//
		// Note the frame index advances BEFORE the result is inspected, so recovery happens
		// with the index already stepped. Since CreateVulkanSwapChain re-queries the image
		// count without resetting it, the index stays within range -- but if a driver ever
		// returned fewer images than the index, that would not hold.
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
		// Scene samplers request 16x anisotropy (GL parity). The feature must
		// be enabled or vkCreateSampler fails on aniso samplers. Enable only
		// when supported so device creation never regresses.
		{
			VkPhysicalDeviceFeatures supported = {};
			vkGetPhysicalDeviceFeatures(physicalDevice, &supported);
			if (supported.samplerAnisotropy)
				deviceFeatures.samplerAnisotropy = VK_TRUE;
		}
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

		// Prepare pNext to enable — Vulkan 1.2+ core path uses ONLY VkPhysicalDeviceVulkan12Features.
		// Chaining both Vulkan12 and VkPhysicalDeviceTimelineSemaphoreFeatures violates
		// VUID-VkDeviceCreateInfo-pNext-02830 and triggers validation error (seen on 591.59).
		VkPhysicalDeviceVulkan12Features enabledVulkan12Features = {};
		VkPhysicalDeviceTimelineSemaphoreFeatures enabledTimelineFeaturesKHR = {};
		VkPhysicalDeviceVulkan13Features enabledVulkan13Features = {};
		enabledVulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		enabledVulkan13Features.synchronization2 = VK_TRUE;
		void *deviceCreatePNext = nullptr;
		// Remember which path provided the feature so we enable the matching struct only
		bool timelineViaVulkan12 = vulkan12FeaturesQuery.timelineSemaphore == VK_TRUE;
		bool timelineViaKHR = timelineFeaturesQueryKHR.timelineSemaphore == VK_TRUE;

		if (timelineSemaphoreSupported)
		{
			if (timelineViaVulkan12) {
				// Core 1.2 path: only Vulkan12 struct. Do NOT push KHR extension nor chain KHR struct.
				enabledVulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
				enabledVulkan12Features.timelineSemaphore = VK_TRUE;
				deviceCreatePNext = &enabledVulkan12Features;
				WL_CORE_INFO("Enabling timeline semaphores via VkPhysicalDeviceVulkan12Features (core 1.2)");
			} else if (timelineViaKHR || hasTimelineSemaphoreExtension) {
				// 1.1 fallback: KHR extension path
				enabledTimelineFeaturesKHR.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
				enabledTimelineFeaturesKHR.timelineSemaphore = VK_TRUE;
				deviceExtensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
				deviceCreatePNext = &enabledTimelineFeaturesKHR;
				WL_CORE_INFO("Enabling timeline semaphores via VkPhysicalDeviceTimelineSemaphoreFeatures + KHR extension (1.1 fallback)");
			} else {
				// Should not happen, fallback to Vulkan12
				enabledVulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
				enabledVulkan12Features.timelineSemaphore = VK_TRUE;
				deviceCreatePNext = &enabledVulkan12Features;
				WL_CORE_INFO("Enabling timeline semaphores via VkPhysicalDeviceVulkan12Features (fallback)");
			}
		}
		else
		{
			WL_CORE_ERROR("Vulkan device does not support timeline semaphores — NVRHI Vulkan backend requires it. Device creation will proceed but NVRHI Queue creation will crash.");
		}
		// NVRHI unconditionally records vkCmdPipelineBarrier2 (synchronization2,
		// core in 1.3): chain the 1.3 enable struct onto whichever timeline
		// head was selected above. Chaining distinct core structs is legal;
		// only duplicating the timeline structs violates VUID-02830.
		// THE SYNCHRONIZATION2 ENABLE STRUCT IS CHAINED ONTO WHICHEVER TIMELINE HEAD WON.
		//
		// NVRHI unconditionally records vkCmdPipelineBarrier2, so synchronization2 (core in
		// 1.3) must be enabled regardless of which timeline semaphore path was taken. The
		// two cases are compared by POINTER IDENTITY against deviceCreatePNext, not by a
		// separate flag -- which is why the assignment above must have happened first.
		//
		// THE CONSTRAINT IS SPECIFIC: chaining distinct core structs is legal, but
		// DUPLICATING the timeline structs -- chaining both VkPhysicalDeviceVulkan12Features
		// and VkPhysicalDeviceTimelineSemaphoreFeatures -- violates VUID-VkDeviceCreateInfo-
		//pNext-02830. The comment above the query chain says the same thing, and the two are
		// deliberately asymmetric:
		//
		//   * timeline: EITHER the 1.2 core struct OR the KHR one, never both. The selection
		//     is made earlier, once, from which feature the device actually reported.
		//   * synchronization2: ONE struct, chained onto whichever timeline head won, because
		//     they are different features and do not conflict.
		//
		// THE ENABLE CHAIN MIRRORS THE QUERY CHAIN from SelectVulkanPhysicalDevice, which
		// probed 1.2 and KHR timeline plus 1.3 synchronization2. Vulkan does not let you
		// enable a feature you did not query, so the two chains must stay in step -- if a
		// query struct is added there, the matching enable struct has to be added here.
		if (deviceCreatePNext == &enabledVulkan12Features)
			enabledVulkan12Features.pNext = &enabledVulkan13Features;
		else if (deviceCreatePNext == &enabledTimelineFeaturesKHR)
			enabledTimelineFeaturesKHR.pNext = &enabledVulkan13Features;
		if (deviceCreatePNext)
			WL_CORE_INFO("Enabling synchronization2 via VkPhysicalDeviceVulkan13Features (core 1.3)");

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
		// VALIDATION LAYERS ARE REQUESTED AT BOTH INSTANCE AND DEVICE LEVEL, gated on a
		// member set during instance creation.
		//
		// m_VkValidationLayersEnabled is set only inside the WL_DEBUG block in the instance
		// path, and only if VK_LAYER_KHRONOS_validation was actually found among the
		// available layers. So a Release build always requests zero layers here, and a
		// Debug build on a machine without the SDK requests zero too -- in both cases
		// enabledLayerCount is explicitly 0 rather than left at whatever the zero-init gave.
		//
		// The layer is requested twice, at instance creation and again here at device
		// creation. That is redundant for the Khronos validation layer, which is an instance
		// layer -- requesting it again as a device layer is accepted by some implementations
		// and ignored by others, which is what the "legacy, but supported by some
		// implementations" comment is acknowledging.
		//
		// Note this block is NOT inside an #ifdef, unlike the detection logic that sets the
		// flag. That is correct: the flag is already false in Release, and re-testing it here
		// keeps the two paths from diverging.
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

	void NVRHIContext::CreateDeviceVulkanWithRetry()
	{
		// A freshly created VkDevice can come up lost (post-TDR recovery,
		// driver hiccup under load): NVRHI's first semaphore creation
		// then throws ErrorDeviceLost. Tear everything down and try
		// once more from scratch before giving up.
		for (int attempt = 0; attempt < 2; ++attempt)
		{
			if (attempt > 0)
			{
				WL_CORE_WARN("Vulkan device creation failed, retrying from scratch (attempt {0})", attempt + 1);
				m_ActiveRenderTarget = nullptr;
				m_BackBufferFramebuffers.clear();
				m_BackBuffers.clear();
				m_VkFramebuffers.clear();
				m_VkBackBuffers.clear();
				m_CommandList = nullptr;
				m_Device = nullptr;
				DestroyVulkan();
			}
			CreateDeviceVulkan();
			if (m_Device)
				return;
		}
		WL_CORE_ERROR("Vulkan device creation failed after retry; backend unavailable");
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
		appInfo.apiVersion = VK_API_VERSION_1_3;

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
			deviceDesc.errorCB = &s_NVRHILogCallback;
			deviceDesc.instance = static_cast<VkInstance>(m_VkInstance);
			deviceDesc.physicalDevice = static_cast<VkPhysicalDevice>(m_VkPhysicalDevice);
			deviceDesc.device = static_cast<VkDevice>(m_VkDevice);
			deviceDesc.graphicsQueue = static_cast<VkQueue>(m_VkGraphicsQueue);
			deviceDesc.graphicsQueueIndex = m_VkGraphicsQueueFamilyIndex;
			deviceDesc.transferQueue = static_cast<VkQueue>(m_VkGraphicsQueue);
			deviceDesc.transferQueueIndex = m_VkGraphicsQueueFamilyIndex;
			deviceDesc.computeQueue = static_cast<VkQueue>(m_VkGraphicsQueue);
			deviceDesc.computeQueueIndex = m_VkGraphicsQueueFamilyIndex;

			if (m_VkGraphicsQueue == VK_NULL_HANDLE)
			{
				WL_CORE_ERROR("Cannot create NVRHI Vulkan device: graphics queue is null (queue family {0})", m_VkGraphicsQueueFamilyIndex);
				return;
			}

			// NVRHI's Queue ctor creates an internal timeline semaphore and lets the
			// vk:: exception escape on failure — without this guard that is an
			// unhandled crash in vk::detail::throwResultException (the stack in the
			// bug report). Log it and fail gracefully instead.
			try
			{
				m_Device = nvrhi::vulkan::createDevice(deviceDesc);
			}
			catch (const std::exception &e)
			{
				WL_CORE_ERROR("nvrhi::vulkan::createDevice threw: {0} (timeline semaphore creation failed — device lacks the enabled feature?)", e.what());
				m_Device = nullptr;
				return;
			}
			catch (...)
			{
				WL_CORE_ERROR("nvrhi::vulkan::createDevice threw an unknown exception");
				m_Device = nullptr;
				return;
			}

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

		// Create DXGI factory - use CreateDXGIFactory2 so FLIP model is guaranteed (Factory2+)
		IDXGIFactory4 *factory = nullptr;
		UINT factoryFlags = 0;
#ifdef WL_DEBUG
		factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
		HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory));
		if (FAILED(hr) || !factory)
		{
			// Fallback to CreateDXGIFactory1 for older SDK
			hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
		}
		if (FAILED(hr) || !factory)
		{
			WL_CORE_ERROR("CreateSwapChain: Failed to create DXGI factory HRESULT 0x{0:X} ({0})", (uint32_t)hr);
			return;
		}

		// Describe swap chain.
		// DX11 uses the legacy bitblt model (DXGI_SWAP_EFFECT_DISCARD): the FLIP
		// model is broken on driver 591.59 — a freshly created FLIP_DISCARD
		// swapchain fails GetBuffer(1) with DXGI_ERROR_INVALID_CALL, and ImGui's
		// first DrawIndexed faults inside d3d11.dll. Legacy bitblt is the most
		// battle-tested D3D11 configuration. DX12 keeps FLIP_DISCARD (required for
		// its present model and unaffected by the issue).
		DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
		swapChainDesc.BufferCount = 2;
		swapChainDesc.Width = m_Width;
		swapChainDesc.Height = m_Height;
		// THE SWAPCHAIN FORMAT IS THE NON-sRGB VARIANT, WHILE THE VULKAN PATH NEGOTIATES sRGB.
		//
		// This is fixed rather than enumerated from IDXGIFactory::EnumOutputModes, so it is not
		// the display's preferred format. It is a plain BGRA8 UNORM surface.
		//
		// THE BACKENDS DIFFER. The Vulkan path searches available formats for
		// VK_FORMAT_B8G8R8A8_SRGB, and the ImGui render pass falls back to
		// VK_FORMAT_B8G8R8A8_SRGB when the swapchain format is undefined. So Vulkan presents
		// sRGB and D3D presents linear UNORM.
		//
		// That difference has to be absorbed somewhere: either the D3D backend does its own
		// gamma encode in the present path, or the two backends produce visibly different
		// output brightness for identical scene data. Worth confirming which, because the
		// comment on the NVRHI texture creation a few hundred lines below explicitly notes
		// "swapchain is B8G8R8A8_UNORM" -- so the non-sRGB choice is deliberate and at least
		// acknowledged where it matters.
		//
		// SampleDesc.Count = 1 disables MSAA, so there is no resolve step and no multisampled
		// backbuffer to account for.
		swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapChainDesc.SampleDesc.Count = 1;
		swapChainDesc.SampleDesc.Quality = 0;
		swapChainDesc.Stereo = FALSE;
		// THE D3D BACKENDS DIVERGE ON SWAP EFFECT, AND THE SPLIT IS DRIVER-DRIVEN.
		//
		// DX12 gets FLIP_DISCARD, which its present model requires; DX11 gets the legacy
		// DISCARD bitblt. The reason is not a preference -- FLIP_DISCARD is broken on driver
		// 591.59, where a freshly created FLIP_DISCARD swapchain fails GetBuffer(1) with
		// DXGI_ERROR_INVALID_CALL and ImGui's first DrawIndexed then faults inside d3d11.dll.
		//
		// This single choice explains three other pieces of code that look unrelated:
		//
		//   * CreateBackBufferFramebuffers forcing bufferCount = 1 on bitblt chains, because
		//     DISCARD exposes only buffer 0.
		//   * the D3D11 debug layer being opt-in via WL_D3D_DEBUG, since it faults inside
		//     DrawIndexed validation on the same driver.
		//   * the BufferCount != 2 warning after creation, guarding a driver that clamps.
		//
		// ALPHA MODE AND SCALING ARE SET ONLY FOR DX12. Leaving them at the zero-init default
		// for DX11 is correct -- STRETCH and IGNORE are the DX11 defaults too -- but it means
		// the two paths do not describe the same thing, and adding a DX11 override later would
		// need to be conditional for the same reason.
		if (m_API == RendererAPI::API::NVRHI_DX12)
		{
			swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
			swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
		}
		else // DX11 legacy bitblt
		{
			swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
			swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
			swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
		}

		WL_CORE_INFO("CreateSwapChain: Requesting {0}x{1} BufferCount={2} SwapEffect={3}", m_Width, m_Height, swapChainDesc.BufferCount, (int)swapChainDesc.SwapEffect);

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
		{
			DXGI_SWAP_CHAIN_DESC1 actual{}; swapChain3->GetDesc1(&actual);
			WL_CORE_INFO("SwapChain created: BufferCount={0} {1}x{2} Format={3} SwapEffect={4} Scaling={5} Alpha={6}", actual.BufferCount, actual.Width, actual.Height, (int)actual.Format, (int)actual.SwapEffect, (int)actual.Scaling, (int)actual.AlphaMode);
			if (actual.BufferCount != 2)
				WL_CORE_WARN("SwapChain BufferCount !=2 - driver clamped! This will cause GetBuffer(1) INVALID_CALL");
		}
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

		// Clear old backbuffers - release D3D11 RTVs first (they hold refs to backbuffers)
		for (auto* rtv : m_D3D11RTVs) if (rtv) rtv->Release();
	m_D3D11RTVs.clear();
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

		// Get backbuffers from swapchain.
		// BufferCount from the desc is NOT the number of GetBuffer-accessible
		// buffers: legacy bitblt chains (DISCARD/SEQUENTIAL) only expose buffer 0
		// — GetBuffer(1) returns DXGI_ERROR_INVALID_CALL by spec. FLIP chains
		// expose all of them.
		uint32_t bufferCount = 2;
		{
			DXGI_SWAP_CHAIN_DESC1 scDesc{}; static_cast<IDXGISwapChain3*>(m_DXGISwapChain3)->GetDesc1(&scDesc);
			if (scDesc.SwapEffect == DXGI_SWAP_EFFECT_DISCARD || scDesc.SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
			{
				// BITBLT SWAPCHAINS EXPOSE ONLY BUFFER 0, SO THIS PATH USES ONE BACKBUFFER.
				//
				// This is the exact workaround for the driver-591.59 failure described on the swap
				// effect choice above. A legacy bitblt chain (DISCARD or SEQUENTIAL) exposes only
				// buffer 0: GetBuffer(1) returns DXGI_ERROR_INVALID_CALL by spec, which is where the
				// original crash came from -- ImGui's first DrawIndexed faulted inside d3d11.dll
				// against a swapchain it could not actually address.
				//
				// So DX11 requests two buffers but addresses one. That is not a leak -- the
				// unaddressed buffer is owned by the swapchain, which releases it on destruction.
				//
				// NOTE THE INTERACTION WITH THE IMGUI RTV HEAP, which allocates NumDescriptors = 2
				// unconditionally. On the DX11 bitblt path only descriptor 0 is ever written, so the
				// heap is oversized rather than undersized -- the safe direction, and consistent with
				// the BufferCount != 2 warning a driver clamp produces.
				//
				// The FLIP branch below uses the real descriptor count, which is correct for DX12 and
				// for any FLIP-based DX11 chain.
				bufferCount = 1;
				WL_CORE_INFO("CreateBackBufferFramebuffers: bitblt swapchain - using 1 accessible backbuffer (BufferCount={0})", scDesc.BufferCount);
			}
			else if (scDesc.BufferCount > 0)
			{
				bufferCount = scDesc.BufferCount;
			}
		}
		// BOTH VECTORS ARE SIZED TOGETHER so index i pairs a backbuffer with its
		// framebuffer.
		//
		// Every later access is parallel by index, so keeping them the same length is the
		// invariant that matters. On Vulkan this function returns early above without sizing
		// either, because Vulkan backbuffers are created in CreateVulkanSwapChain.
		//
		// The clearing before this is ordered deliberately: the D3D11 RTVs hold references to
		// the backbuffers, so they are released before the backbuffer wrappers are dropped,
		// and the framebuffer views -- which reference the RTVs -- are cleared last of the
		// three.
		//
		// Note the release loop and the three clear() calls here are byte-identical to the
		// ones in the resize path and in device destruction; this is the third occurrence.
		m_BackBuffers.resize(bufferCount);
		m_BackBufferFramebuffers.resize(bufferCount);

		if (m_API == RendererAPI::API::NVRHI_DX11)
		{
			IDXGISwapChain3 *swapChain = static_cast<IDXGISwapChain3 *>(m_DXGISwapChain3);

			// Get all backbuffers first
			std::vector<ID3D11Texture2D *> nativeBuffers(bufferCount, nullptr);
			for (uint32_t i = 0; i < bufferCount; i++)
			{
				// ALL BACKBUFFERS ARE FETCHED BEFORE ANY RTV IS CREATED, and a failure dumps
				// diagnostic state rather than just logging.
				//
				// BufferCount from the descriptor is not the number of addressable buffers -- on the
				// bitblt path that is 1 by the correction above. GetBuffer(i) is called for each, and
				// the failure branch is unusually thorough for an init path:
				//
				//   * the swapchain descriptor at the moment of failure,
				//   * GetDeviceRemovedReason, which distinguishes "the driver took the device away"
				//     from "you asked for the wrong index",
				//   * an IDXGISwapChain2 query to confirm BufferCount through a second interface,
				//     in case the DXGI 1.3 accessor was the thing lying.
				//
				// That is deliberate and was almost certainly added while chasing the driver-591.59
				// GetBuffer(1) INVALID_CALL fault, where the descriptor said 2 buffers but only 1 was
				// addressable. The multi-interface confirmation is exactly the check that situation
				// calls for.
				//
				// The native buffers are collected into a temporary vector first so that a failure
				// partway through does not leave a half-populated m_BackBuffers.
				HRESULT hr = swapChain->GetBuffer(i, IID_PPV_ARGS(&nativeBuffers[i]));
				if (FAILED(hr) || !nativeBuffers[i])
				{
					WL_CORE_ERROR("CreateBackBufferFramebuffers: Failed to get DX11 backbuffer {0} HRESULT 0x{1:X} ({1}) - INVALID_CALL(0x887A0001)=out-of-range or ResizeBuffers with live refs; DEVICE_REMOVED=0x887A0005", i, (uint32_t)hr);
					{
						DXGI_SWAP_CHAIN_DESC1 dbg{}; swapChain->GetDesc1(&dbg);
					WL_CORE_ERROR("  SwapChain desc at failure: BufferCount={0} Width={1} Height={2} Format={3} SwapEffect={4} Scaling={5} Alpha={6} Flags=0x{7:X}", dbg.BufferCount, dbg.Width, dbg.Height, (int)dbg.Format, (int)dbg.SwapEffect, (int)dbg.Scaling, (int)dbg.AlphaMode, dbg.Flags);
						if (m_D3D11Device)
						{
							HRESULT removed = static_cast<ID3D11Device*>(m_D3D11Device)->GetDeviceRemovedReason();
							if (FAILED(removed))
								WL_CORE_ERROR("  D3D11 GetDeviceRemovedReason: 0x{0:X}", (uint32_t)removed);
						}
						// Try IDXGISwapChain2 path for diagnosis
						IDXGISwapChain2* sc2 = nullptr;
						if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&sc2))))
						{
							DXGI_SWAP_CHAIN_DESC1 d2{}; sc2->GetDesc1(&d2);
							WL_CORE_ERROR("  IDXGISwapChain2::GetDesc1 confirms BufferCount={0}", d2.BufferCount);
							sc2->Release();
						}
					}
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
				// Create NVRHI texture from D3D11 texture (swapchain is B8G8R8A8_UNORM)
				nvrhi::TextureDesc desc;
				desc.width = m_Width;
				desc.height = m_Height;
				desc.format = nvrhi::Format::BGRA8_UNORM;
				desc.isRenderTarget = true;
				desc.initialState = nvrhi::ResourceStates::RenderTarget;
				desc.keepInitialState = true;

				// NVRHI RefCountPtr will AddRef internally, so just hand off
				// and Release our GetBuffer() reference
				m_BackBuffers[i] = m_Device->createHandleForNativeTexture(
					nvrhi::ObjectTypes::D3D11_Resource,
					nativeBuffers[i],
					desc);
				// Create cached RTV for ImGui (owned here, one per backbuffer)
				ID3D11RenderTargetView* rtv = nullptr;
				HRESULT rtvHr = static_cast<ID3D11Device*>(m_D3D11Device)->CreateRenderTargetView(nativeBuffers[i], nullptr, &rtv);
				if (SUCCEEDED(rtvHr) && rtv)
					m_D3D11RTVs.push_back(rtv);
				else
				{
					WL_CORE_ERROR("CreateBackBufferFramebuffers: Failed to create D3D11 RTV {0} HRESULT 0x{1:X}", i, (uint32_t)rtvHr);
					m_D3D11RTVs.push_back(nullptr);
				}
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
					WL_CORE_ERROR("CreateBackBufferFramebuffers: Failed to get DX12 backbuffer {0} HRESULT 0x{1:X} ({1})", i, (uint32_t)hr);
					{
						DXGI_SWAP_CHAIN_DESC1 dbg{}; swapChain->GetDesc1(&dbg);
					WL_CORE_ERROR("  SwapChain desc at failure: BufferCount={0}", dbg.BufferCount);
					}
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
				desc.format = nvrhi::Format::BGRA8_UNORM;
				desc.isRenderTarget = true;
				desc.initialState = nvrhi::ResourceStates::RenderTarget;
				desc.keepInitialState = true;

				// NVRHI RefCountPtr will AddRef internally, so just hand off
				// and Release our GetBuffer() reference
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
						// THE OFFSET IS COMPUTED PER-ITERATION RATHER THAN INCREMENTED, and both this loop
						// and its consumer must use the same stride.
						//
						//     D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHandle;
						//     h.ptr += i * m_D3D12RTVDescriptorSize;
						//     device->CreateRenderTargetView(nativeBuffers[i], nullptr, h);
						//
						// Recomputing from i rather than doing h.ptr += stride each pass means the result
						// does not depend on iteration order and stays correct if a buffer is skipped by the
						// `if (nativeBuffers[i])` guard below.
						//
						// m_D3D12RTVDescriptorSize IS THE QUERIED INCREMENT, not a hardcoded 32:
						// GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV). RTV descriptors are
						// 32 bytes in practice but that is not contractual, and hardcoding it is the classic
						// source of silently overlapping descriptors.
						//
						// THE CONSUMER APPLIES THE IDENTICAL ARITHMETIC. ImGuiLayer.cpp:927 does
						//     rtvHandle.ptr += idx * rtvSize;
						// against the heap start, where idx is GetCurrentBackBufferIndex. So descriptor i
						// here is the descriptor ImGui reads for backbuffer i.
						//
						// TWO COUPLINGS THAT ARE NOT ENFORCED BY ANYTHING:
						//
						//  * The loop writes `bufferCount` descriptors, and the heap must have been created
						//    with at least that many. On a bitblt chain bufferCount was forced to 1 while the
						//    heap is sized differently, so the heap is deliberately oversized here -- safe
						//    in the direction of writing fewer than allocated.
						//  * The heap itself must be alive. The `if (m_D3D12ImGuiRTVHeap && m_D3D12Device)`
						//    guard skips the whole block otherwise, which means the ImGui pass later finds a
						//    heap with stale descriptors and reports "resources not ready".
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

		// THE ACTUAL IMAGE COUNT FROM THE DRIVER OVERWRITES THE REQUESTED ONE, AND
		// m_VkFrameIndex MUST NOT BE RESET.
		//
		// imageCount goes in as minImageCount -- the Vulkan minimum, typically surface
		// capabilities.minImageCount or +1 for the mailbox/FIFO present mode. What the driver
		// actually allocates can be higher, so it is re-queried here with the two-call
		// count-then-fill pattern and this assignment makes the real number authoritative.
		//
		// THAT MATTERS BECAUSE m_VkFrameIndex IS ADVANCED AS
		//     (m_VkFrameIndex + 1) % m_VkImageCount
		// during presentation. Every vector sized below uses the corrected count, so they
		// stay consistent. The frame index itself is deliberately left alone across a
		// swapchain recreation: resetting it to zero would be harmless but unnecessary, and
		// leaving it preserves the presentation cadence across a resize.
		//
		// This is the same "descriptor said one thing, driver gave another" situation the
		// DXGI path warns about with BufferCount != 2, handled differently: there it warns,
		// here it corrects. Both are needed -- Vulkan's count is authoritative by design
		// here because all the dependent allocations are sized from it.
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
		// COLOR_ATTACHMENT ONLY, AND oldSwapchain IS EXPLICITLY NULL.
		//
		// Requesting only COLOR_ATTACHMENT_BIT means swapchain images cannot be transitioned
		// for transfer. That is fine for rendering, but it means any future feature needing
		// to blit TO the swapchain -- a post-process resolve, or a UI scaled composited from
		// a texture -- would need this bit added along with a surface-capability check.
		//
		// oldSwapchain = VK_NULL_HANDLE means this is always a fresh creation rather than a
		// recreation pass. Vulkan lets you pass the old swapchain so the driver can reuse its
		// resources atomically; not doing so means a resize destroys and recreates every
		// image, image view and framebuffer -- more expensive, and it can produce a visible
		// hitch. The recreation path here goes through DestroyVulkanSwapChain and then
		// CreateVulkanSwapChain instead, which achieves the same thing non-atomically.
		//
		// clipped = VK_TRUE means pixels outside the surface are discarded rather than
		// read, which is both faster and the correct choice for a windowed swapchain.
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
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;

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
		// A FAILED COMMAND POOL DOES NOT RETURN -- EXECUTION FALLS THROUGH TO THE SUCCESS LOG.
		//
		// The failure branch only logs. m_VkCommandPool is then assigned unconditionally, and
		// because commandPool was initialised to VK_NULL_HANDLE, that assignment stores a null
		// handle -- safe, but the function then logs "Vulkan backbuffer framebuffers created"
		// having created no pool.
		//
		// So a command-pool failure is reported to the log as success. Nothing dereferences
		// the null here; the damage surfaces later at submit time, when the missing pool
		// produces a Vulkan error far from its cause. An early return after the log would
		// make this the same fail-loudly pattern the neighbouring code uses.
		//
		// Contrast the ImGui command pool creation above, which DOES return on failure and
		// cleans up the pool it just created. Two different conventions for the same failure
		// class in one file.
		//
		// The commandPoolInfo is otherwise conventional: the graphics queue family, with
		// RESET_COMMAND_BUFFER_BIT so per-frame command buffers can be reset individually
		// instead of the whole pool.
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
		// TEARDOWN ORDER IS LOAD-BEARING: idle, then ImGui framebuffers, then sync objects,
		// then image views, then the swapchain.
		//
		// vkDeviceWaitIdle first is mandatory rather than defensive -- destroying a fence or
		// semaphore the GPU still references is undefined behaviour, and during a resize the
		// swapchain images are very much still in flight.
		//
		// THE IMGUI FRAMEBUFFERS GO FIRST because they reference the swapchain image views.
		// Destroying the views first would leave the framebuffers pointing at dead views.
		//
		// The NVRHI framebuffers and backbuffers are only CLEARED, not destroyed, with the
		// comment that NVRHI owns them. That is the right call only if NVRHI is told to
		// forget them -- they are NVRHI objects wrapping the swapchain images, so the engine
		// cannot destroy them itself and must not.
		//
		// Every destroy loop null-checks its handle, so a partially failed swapchain creation
		// cannot make this crash during teardown.
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
		// Editor framebuffer Bind() override wins over the swapchain backbuffer
		if (m_ActiveRenderTarget)
			return m_ActiveRenderTarget;

		if (m_API == RendererAPI::API::NVRHI_Vulkan)
		{
			if (m_VkFramebuffers.empty())
				return nullptr;
			// EMPTY-CHECKED BUT NOT INDEX-CHECKED, unlike GetVkCurrentImGuiFramebuffer nearby.
			//
			// This accessor guards against an empty vector and then indexes it unconditionally.
			// The sibling Vulkan ImGui accessor does both:
			//
			//     if (m_VkImGuiFramebuffers.empty()) return VK_NULL_HANDLE;
			//     if (m_CurrentBackBufferIndex < m_VkImGuiFramebuffers.size())
			//         return m_VkImGuiFramebuffers[m_CurrentBackBufferIndex];
			//     return m_VkImGuiFramebuffers[0];        // <- also has a fallback
			//
			// So the two differ in two ways: no index bound, and no fallback.
			//
			// Whether that is reachable depends on whether the two vectors can ever differ in
			// size. m_VkFramebuffers is rebuilt in CreateVulkanSwapChain from the image views;
			// m_CurrentBackBufferIndex comes from vkAcquireNextImageKHR, which returns an index
			// < the image count. On a correctly built swapchain they agree.
			//
			// THE CASE THAT CAN DIVERGE IS THE BITBLT-STYLE MISMATCH, not a Vulkan one: a driver
			// that clamps the requested image count upward while returning an acquire index for
			// its own larger allocation, or a frame arriving between the rebuild and the next
			// acquire when the old index is still live. GetBuffer(1) on driver 591.59 is the
			// precedent for a driver not honouring a swapchain count.
			//
			// m_VkBackBuffers and m_BackBuffers/m_BackBufferFramebuffers below have the same
			// shape, so the pattern is consistent -- consistently unguarded.
			//
			// Adding the same bound-and-fallback the ImGui accessor uses would make the three
			// accessors agree.
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
			// A SINGLE SEMAPHORE PAIR PER FRAME, WITH THE WAIT STAGE PINNED TO
			// COLOR_ATTACHMENT_OUTPUT.
			//
			// image-available is waited on before colour output, and render-finished is signalled
			// so the present waits for the submit. Pinning the destination stage to
			// COLOR_ATTACHMENT_OUTPUT is the standard idiom and is deliberately narrower than
			// TOP_OF_PIPE -- it means the GPU does not stall the whole pipeline behind the
			// acquire.
			//
			// NOTE THE SEMAPHORES ARE INDEXED BY THE FRAME INDEX, not by the backbuffer index.
			// The two advance independently: m_VkFrameIndex steps modulo m_VkImageCount here,
			// while m_CurrentBackBufferIndex is what is presented. That is why there is a fence
			// and a semaphore set PER FRAME rather than per image -- indexing these by backbuffer
			// index would be a bug whenever the two counters diverged.
			//
			// The ImGui command buffer is appended to the same submit, so the UI is recorded and
			// presented in one submission. When ImGui is not initialised the submit carries zero
			// command buffers -- legal, and it avoids submitting a null handle.
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
				// THE EARLY RETURN TRUSTS A TWO-OF-FOUR PREDICATE, so a partial init is treated as
				// complete.
				//
				// IsVulkanImGuiInitialized() checks only m_VkImGuiDescriptorPool and
				// m_VkImGuiRenderPass. If a previous call created those two and then failed on the
				// framebuffers or command pool, this returns true and SKIPS the two creation calls
				// below -- leaving the missing resources missing permanently, with no retry.
				//
				// That matters because the checks after each of the first two creations treat a null
				// handle as failure and roll back, but the last two calls have no such check. So the
				// only states reachable here on entry are "nothing created" or "first two created",
				// and only the first is repaired.
				//
				// Checking the framebuffer vector and command pool here -- or simply testing those
				// two members instead of the predicate -- would make this a genuine retry rather
				// than a one-shot.
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
			// Scene NVRHI work was already close()+execute() in Application::Run before ImGui
			// (scene must be behind UI for DX12 where ImGui uses its own native command list).
			// Do NOT ExecuteNVRHICommandList() again here — second execute dereferences null
			// m_ActiveCommandList and is the crash at Device::executeCommandLists / getD3D12CommandList().
			// SwapBuffers for DX11/DX12 only needs to Present; Vulkan path above already handled via vkQueueSubmit.
			if (m_API == RendererAPI::API::NVRHI_DX11 || m_API == RendererAPI::API::NVRHI_DX12)
			{
				// Safety: if for any reason Execute was skipped (e.g. no BeginFrame), flush now once.
				if (m_CommandListOpen)
					ExecuteNVRHICommandList();
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

		// THE ORDER OF THE TWO GUARDS IS WHAT MAKES MINIMISE SAFE.
		//
		// The no-op check comes first, so an unchanged size never reaches the backend. The
		// zero check comes second and is the one that matters: a minimised window reports
		// 0x0, and passing that to ResizeBuffers or recreating a swapchain with it is
		// undefined-adjacent at best -- DXGI rejects it outright, and the Vulkan path would
		// build a swapchain with a zero extent.
		//
		// THIS GUARD ALSO PREVENTS m_Width/m_Height FROM BEING SET TO ZERO, because the
		// assignment below is after both early returns. A later frame still sees the last
		// good dimensions, so nothing downstream has to handle a degenerate size.
		//
		// The cost is that a resize is lost entirely while minimised rather than deferred --
		// the restore path relies on the OS delivering a fresh non-zero size, which GLFW
		// does.
		//
		// This is the shared D3D/Vulkan guard. CreateVulkanSwapChain has its own, and the two
		// are separate rather than shared, so the Vulkan path is reached only because the
		// guard above returns first for both APIs.
		if (width == 0 || height == 0)
		{
			WL_CORE_WARN("Resize: ignoring 0-sized request ({0}x{1}) - window minimized", width, height);
			return;
		}

		// If a NVRHI command list is still open (mid-frame resize), close+execute before touching swapchain
		if (m_CommandListOpen)
		{
			WL_CORE_WARN("Resize: closing open NVRHI command list before ResizeBuffers");
			ExecuteNVRHICommandList();
		}

		m_Width = width;
		m_Height = height;

		if (m_API == RendererAPI::API::NVRHI_Vulkan)
		{
			// TEARDOWN ORDER IS EXACTLY INVERSE OF CREATION, AND vkDeviceWaitIdle COMES FIRST
			// BECAUSE IT IS LOAD-BEARING.
			//
			//     vkDeviceWaitIdle(device);                              // all work finished
			//     DestroyVulkanImGuiCommandPoolAndBuffers();
			//     DestroyVulkanImGuiFramebuffers();
			//     DestroyVulkanImGuiRenderPass();
			//     DestroyVulkanImGuiDescriptorPool();
			//
			// Creation was pool -> render pass -> framebuffers -> command pool and buffers, so
			// teardown unwinds that: the command pool first, then the framebuffers that
			// referenced the render pass, then the render pass, then the descriptor pool.
			//
			// Destroying the render pass while framebuffers still exist would leave those
			// framebuffers referencing a dead render pass -- and GetVkCurrentImGuiFramebuffer
			// would happily return one of them, since it only bounds-checks the vector.
			//
			// THE IDLE WAIT IS NOT DEFENSIVE POLITENESS. Each of these destroys an object the
			// GPU may still be reading: the command pool owns in-flight command buffers, the
			// framebuffers were render-pass targets. Destroying them mid-flight is undefined
			// behaviour, not merely wasteful.
			//
			// NOTE THE MISSING DEVICE GUARD ON THE IDLE CALL itself -- there is none, which is
			// correct: the other four destroys null-check internally, so this is safe on partial
			// state. But it means vkDeviceWaitIdle is reached with a null device if teardown runs
			// after a failed init, relying on Vulkan to treat it as a no-op rather than crashing.
			// That is the standard leniency, not something the spec guarantees.
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
			if (m_Device)
				m_Device->waitForIdle();

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
						// THE FENCE IS SIGNALLED SYNCHRONOUSLY FROM THE CPU, NOT BY THE QUEUE.
						//
						// This is fence->Signal(value) on the ID3D12Fence object itself, not
						// ID3D12CommandQueue::Signal. It marks the fence complete immediately rather than
						// enqueuing a GPU-side signal behind pending work.
						//
						// It therefore does NOT wait for the GPU. What it actually achieves is that any
						// SetEventOnCompletion registered earlier for THIS value would now fire -- and the
						// GetCompletedValue check below usually short-circuits, since the signal has already
						// completed the fence.
						//
						// The surrounding m_Device->waitForIdle() is what does the real GPU synchronisation;
						// this sequence exists to release the wait handle registered by whoever was consuming
						// the fence (the ImGui command objects), so a thread blocked on it can wake.
						//
						// Note it is guarded on DX12 AND on all three handles being present, and the value
						// is incremented only inside that guard -- so a missing fence stops the counter
						// advancing rather than skipping a number.
						fence->SetEventOnCompletion(m_D3D12ImGuiFenceValue, event);
						WaitForSingleObject(event, INFINITE);
					}
					m_D3D12ImGuiFenceValue++;
				}
			}

			else if (m_API == RendererAPI::API::NVRHI_DX11 && m_D3D11Context)
			{
				ID3D11DeviceContext *ctx = static_cast<ID3D11DeviceContext *>(m_D3D11Context);
				// DX11 RESIZE CLEARS THE IMMUTABLE CONTEXT STATE, WHICH DISCARDS NVRHI'S BINDINGS.
				//
				// ClearState resets the D3D11 immediate context to defaults: unbound render targets,
				// null input layout, null shaders, unbound SRVs and samplers. That is the documented
				// way to drop references to resources about to be released, so it is correct here.
				//
				// THE CONSEQUENCE IS THAT EVERYTHING BOUND THROUGH NVRHI IS NOW GONE. NVRHI caches
				// the pipeline state, shader bindings and descriptor state it believes is current,
				// and the immediate-context reset invalidates that cache without NVRHI being told.
				// Anything drawn after this point without a full state rebind could read stale or
				// null bindings.
				//
				// In practice the window is mid-resize and the next frame re-establishes state, so
				// this is survivable -- but it is a real cross-boundary assumption, and the DX12 path
				// below does not have the equivalent because its state lives in descriptor heaps
				// rather than the immediate context.
				//
				// OMSetRenderTargets(nullptr) plus Flush before it ensures queued work referencing the
				// old RTVs has been submitted before ClearState drops them.
				ctx->OMSetRenderTargets(0, nullptr, nullptr);
				ctx->Flush();
				// BACKBUFFER RESOURCES ARE RELEASED BEFORE BEING RECREATED, FOR ALL D3D BACKENDS.
				//
				// This loop is not inside the DX11-only branch above, so on a DX12 resize it also
				// releases m_D3D11RTVs -- which a DX12 context never populated, so the vector is
				// empty and the loop does nothing. Harmless, and it means one release path serves
				// both backends.
				//
				// The null check inside the loop is needed because Release on a null COM pointer is
				// not valid, and a partially failed swapchain creation can leave holes.
				//
				// All three vectors are then cleared: the RTVs themselves, the NVRHI back buffer
				// wrappers, and the framebuffer views that reference both. Order matters -- the views
				// hold references to the RTVs, so releasing the RTVs first is safe only because each
				// is separately reference counted and the views are released in the same block.
				ctx->ClearState();
			}

			for (auto* rtv : m_D3D11RTVs) if (rtv) rtv->Release();
			m_D3D11RTVs.clear();
			m_BackBuffers.clear();
			m_BackBufferFramebuffers.clear();

			if (m_Device)
				m_Device->waitForIdle();

			// Preserve the created swapchain's parameters: BufferCount=0 and
			// DXGI_FORMAT_UNKNOWN mean "keep current" — correct for both legacy
			// bitblt (DX11) and FLIP_DISCARD (DX12) chains regardless of what the
			// driver actually created.
			IDXGISwapChain3 *sc = static_cast<IDXGISwapChain3 *>(m_DXGISwapChain3);
			DXGI_SWAP_CHAIN_DESC1 cur{};
			sc->GetDesc1(&cur);
			HRESULT hr = sc->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, cur.Flags);
			if (FAILED(hr))
				{
				WL_CORE_ERROR("ResizeBuffers({0}x{1}) failed: HRESULT 0x{2:X} ({2}) - likely leaked backbuffer refs or minimized window", width, height, (uint32_t)hr);
					DXGI_SWAP_CHAIN_DESC1 dbg{}; static_cast<IDXGISwapChain3*>(m_DXGISwapChain3)->GetDesc1(&dbg);
				WL_CORE_ERROR("  SwapChain desc: BufferCount={0} Format={1} SwapEffect={2}", dbg.BufferCount, (int)dbg.Format, (int)dbg.SwapEffect);
			}

			CreateBackBufferFramebuffers();
			}
			}

			void NVRHIContext::BeginFrame()
	{
		WL_PROFILE_FUNCTION();
		// Compute/ImGui registrations belong to one frame only. A begin, transition,
		// or ImGui-end failure must not replay either handle on a later frame.
		m_VkImGuiComputeOutput = nullptr;
		m_VkImGuiComputeOutputReturn = nullptr;
		if (m_API != RendererAPI::API::NVRHI_Vulkan)
		{
			// Open NVRHI command list for batched frame recording (DX11/DX12/Vulkan via NVRHI)
			// All writeBuffer / clear / draw calls between BeginFrame and ExecuteNVRHICommandList
			// must be inside a single open/close pair — per-draw open/close overwrites previous chunks
			// and writeBuffer outside open is illegal. See NVRHIRendererAPI.
			//
			// m_IsDeviceLost must be checked here: after a TDR the device pointer is still
			// non-null but the underlying D3D12 device is removed. Calling open() on a lost
			// device reaches createInternalCommandList -> CreateCommandAllocator/CreateCommandList
			// on a removed device, which crashes. ExecuteNVRHICommandList already guards this;
			// BeginFrame must too.
			if (m_Device && m_CommandList && !m_CommandListOpen && !m_IsDeviceLost)
			{
				m_CommandList->open();
				m_CommandListOpen = true;
			}
			return;
		}
		VkDevice device = static_cast<VkDevice>(m_VkDevice);
		VkSwapchainKHR swapchain = static_cast<VkSwapchainKHR>(m_VkSwapchain);
		if (!device || !swapchain || m_VkFences.empty() || m_VkImageAvailableSemaphores.empty())
			return;

		if (m_VkFrameIndex < m_VkFences.size() && m_VkFences[m_VkFrameIndex])
		{
			// THE ACQUIRE SEQUENCE IS WAIT, THEN RESET, THEN ACQUIRE -- AND THE FENCE WAITED IS
			// FROM N FRAMES AGO.
			//
			// VK_TRUE is the wait-all flag. Because m_VkFrameIndex is advanced in
			// SubmitAndPresent AFTER the submit, the fence indexed here belongs to a frame
			// submitted several frames ago -- waiting on it is what stops the GPU still reading
			// the backbuffer that is about to become the acquire target.
			//
			// The reset follows immediately, before the acquire, so the fence is armed for this
			// frame's submit.
			//
			// THE INDEX IS BOUNDS-CHECKED at the enclosing `if` even though the emptiness test
			// earlier already implies a non-empty vector. m_VkFrameIndex could exceed the size if
			// a swapchain recreation shrank the fence set -- the guard makes that recoverable
			// rather than an out-of-bounds read.
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
			// THE OUT-OF-DATE BRANCH RECREATES AND RETRIES ONCE, RECOVERING IN-PLACE.
			//
			// Rather than returning and waiting for the next frame, the swapchain is destroyed
			// and rebuilt and the acquire retried immediately, so a resize does not drop a frame.
			//
			// The ImGui resources are rebuilt inside the `IsVulkanImGuiInitialized()` branch --
			// correct, because the framebuffers reference the image views that
			// DestroyVulkanSwapChain just destroyed, and the command buffers are sized from the
			// image count.
			//
			// TWO CONCERNS WITH THE RETRY:
			//
			//  1. The fence waited above belonged to the OLD swapchain. DestroyVulkanSwapChain
			//     destroys the old fences, so the wait and reset done this frame are discarded
			//     and the new fence set starts unsignalled. Harmless to wait on (an unsignalled
			//     fence returns immediately) but the per-frame pacing guarantee does not hold
			//     across a resize.
			//  2. Only ONE retry is attempted. A second VK_ERROR_OUT_OF_DATE_KHR after recreation
			//     falls into the error branch below and logs, so a persistently out-of-date
			//     swapchain drops the frame rather than looping.
			//
			// VK_SUBOPTIMAL_KHR is accepted alongside VK_SUCCESS in both checks, which is right:
			// suboptimal means the swapchain still works but no longer matches the surface, so
			// the frame is valid and recreation can happen on a later attempt.
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

		// Open the NVRHI command list exactly like the DX path above: every
		// writeBuffer/clear/draw must be inside open/close. Vulkan previously
		// returned before opening it, so the first constant-buffer upload on a
		// real scene aborted in NVRHI's writeBuffer (assert(m_CurrentCmdBuf)).
		if (m_Device && m_CommandList && !m_CommandListOpen && !m_IsDeviceLost)
		{
			m_CommandList->open();
			m_CommandListOpen = true;
		}
		}

	void NVRHIContext::ExecuteNVRHICommandList()
	{
		WL_PROFILE_FUNCTION();
		// EXECUTION IS SKIPPED WHILE THE DEVICE IS LOST, AND THE OPEN FLAG IS CLEARED
		// WITHOUT CLOSING THE LIST.
		//
		// Setting m_CommandListOpen = false here rather than closing the command list means
		// the list is abandoned rather than submitted. That is deliberate: the underlying
		// device is already gone, so a close or execute would touch invalid objects.
		//
		// Without this early return the frame loop would keep submitting work to a removed
		// device and cascade into backend init failures.
		//
		// Note the guard order: device-lost is checked BEFORE the null device and null
		// command list checks, so a lost device short-circuits regardless of what state the
		// handles are in.
		if (m_IsDeviceLost)
		{
			m_CommandListOpen = false;
			return;
		}
		if (!m_Device || !m_CommandList)
			return;
		// THIS GUARD PREVENTS A CRASH, NOT JUST WASTED WORK.
		//
		// After NVRHI executes a command list it nulls m_ActiveCommandList, so a second
		// execute would dereference null inside the vendor code. The comment names the
		// concrete crash site -- getD3D12Com -- reached from Application::Run plus
		// SwapBuffers double-executing, or from a frame that recorded no work.
		//
		// So this covers two distinct cases with one flag: an empty flush, and an already
		// executed list. Both leave m_CommandListOpen false and are skipped.
		if (!m_CommandListOpen)
		{
			// No work recorded this frame (empty flush) or already executed — prevents
			// Device::executeCommandLists dereferencing null m_ActiveCommandList (crash at getD3D12CommandList())
			// which happens on double-execute (Application::Run + SwapBuffers) or empty frame.
			return;
		}
		// Guard against executing an empty list via native check — extra safety before vendor deref
		// After executed(), m_ActiveCommandList is null, so getNativeObject would return null.
		m_CommandList->close();
		m_CommandListOpen = false;
		m_Device->executeCommandList(m_CommandList);
	}



	void NVRHIContext::RecoverDevice()
	{
		WL_PROFILE_FUNCTION();
		if (!m_IsDeviceLost)
			return;

		if (m_API == RendererAPI::API::NVRHI_Vulkan)
		{
			WL_CORE_INFO("NVRHI: Attempting Vulkan device recovery...");
			DestroyVulkan();
			m_IsDeviceLost = false;
			CreateDeviceVulkanWithRetry();

			if (!m_Device)
			{
				WL_CORE_ERROR("NVRHI: Vulkan device recovery failed; backend remains lost");
				m_IsDeviceLost = true;
				return;
			}

			CreateVulkanSwapChain();
			if (IsVulkanImGuiInitialized())
			{
				CreateVulkanImGuiFramebuffers();
				CreateVulkanImGuiCommandPoolAndBuffers();
			}

			m_CommandList = m_Device->createCommandList();
			s_NVRHILogCallback.context = this;
			WL_CORE_INFO("NVRHI: Vulkan device recovery complete");
		}
		else
		{
			// ONLY VULKAN RECOVERS. DX11 AND DX12 ARE LEFT PERMANENTLY LOST.
			//
			// The Vulkan branch tears down and rebuilds the whole device, then recreates the
			// swapchain, the ImGui framebuffers and command pool if those were initialized, and
			// a fresh command list. It also re-points the log callback at this context, which is
			// necessary because DestroyVulkan released the device the old callback was bound to.
			//
			// The else branch covers BOTH D3D backends and only logs. m_IsDeviceLost stays true,
			// so ExecuteNVRHICommandList keeps skipping and every frame renders nothing.
			//
			// That is a real asymmetry rather than an oversight in the log line -- recreating a
			// DX12 or DX11 device means rebuilding the swapchain, the ImGui heaps and command
			// objects, and every resource the renderer created through NVRHI, which the engine
			// owns. Vulkan can be torn down here because this class owns its surface and device;
			// the D3D resources are shared more widely.
			//
			// The message says so explicitly and asks the user to recreate the device, so the
			// practical outcome is that a TDR on a D3D backend requires restarting the process.
			WL_CORE_ERROR("NVRHI: Device recovery for {0} requires engine-level device recreation. Please restart.",
				m_API == RendererAPI::API::NVRHI_DX11 ? "DX11" : "DX12");
		}
	}

	VkFramebuffer NVRHIContext::GetVkCurrentImGuiFramebuffer() const
	{
		if (m_VkImGuiFramebuffers.empty())
			return VK_NULL_HANDLE;
		// THIS IS THE DEFENSIVE PATTERN THE OTHER THREE ACCESSORS LACK: BOUND, THEN FALL BACK
		// TO INDEX 0.
		//
		// The fallback is safe because the emptiness test above already proved at least one
		// element exists -- index 0 cannot be out of bounds here.
		//
		// Returning the FIRST framebuffer rather than VK_NULL_HANDLE on a bad index means a
		// caller that ignores the failure still renders, into the wrong backbuffer, rather
		// than passing a null framebuffer to vkCmdBeginRenderPass. That is a deliberate
		// degrade-don't-crash choice; it does mean the resulting image is wrong, so it is
		// still worth logging if it ever fires.
		//
		// This is what InitVulkanImGuiResources's unchecked return leaves exposed: the
		// predicate callers trust tests neither this vector nor the command buffers, so a
		// partial init reaches here and is absorbed by exactly this guard.
		if (m_CurrentBackBufferIndex < m_VkImGuiFramebuffers.size())
			return m_VkImGuiFramebuffers[m_CurrentBackBufferIndex];
		return m_VkImGuiFramebuffers[0];
	}

	VkCommandBuffer NVRHIContext::GetCurrentVulkanImGuiCommandBuffer() const
	{
		if (m_VkImGuiCommandBuffers.empty())
			return VK_NULL_HANDLE;
		// THIS ACCESSOR INDEXES BY FRAME INDEX WHILE GetVkCurrentImGuiFramebuffer INDEXES BY
		// BACKBUFFER INDEX. The two differ, and nothing enforces that they stay compatible.
		//
		//     GetVkCurrentImGuiFramebuffer:  m_CurrentBackBufferIndex
		//     GetCurrentVulkanImGuiCommandBuffer: m_VkFrameIndex % size
		//
		// m_CurrentBackBufferIndex is what vkAcquireNextImageKHR returned -- the driver's
		// choice of image. m_VkFrameIndex is the engine's own counter, advanced once per
		// SubmitAndPresent as `(m_VkFrameIndex + 1) % m_VkImageCount`. These are independent
		// and generally NOT equal: the frame index walks 0,1,2,... deterministically while the
		// acquire index depends on which images the driver considers available.
		//
		// So the ImGui pass can record into command buffer N while targeting framebuffer M,
		// with N != M. That is acceptable ONLY because each command buffer is submitted with
		// the framebuffer's own attachment and the two vectors are independently complete --
		// there is no requirement that a given command buffer belongs to a given framebuffer.
		//
		// WHY IT IS SAFE TODAY: both vectors are sized from the same source.
		//
		//     m_VkImGuiFramebuffers.resize(m_VkSwapchainImageViews.size());
		//     m_VkImGuiCommandBuffers.resize(count);        // count == m_VkImageCount
		//
		// so the modulo is a no-op in the normal case and the two stay in range. It is also
		// the SAME INDEXING RULE AS m_VkFences, m_VkImageAvailableSemaphores and
		// m_VkRenderFinishedSemaphores, all of which are per-frame -- command buffers are
		// per-frame in the same way. The framebuffer is the odd one out.
		//
		// WHAT WOULD BREAK IT: a driver clamping the image count upward would make `count`
		// larger than the framebuffer vector, leaving the modulo wrapping command buffers
		// against fewer framebuffers. Both loops are individually bounds-safe, so this would
		// not crash -- it would render the UI through the wrong framebuffer.
		uint32_t idx = m_VkFrameIndex % static_cast<uint32_t>(m_VkImGuiCommandBuffers.size());
		return m_VkImGuiCommandBuffers[idx];
	}

	// Records the layout transition that makes the viewport attachment legal to
	// sample, on the command buffer the ImGui pass is about to be recorded into.
	//
	// WHY A RAW BARRIER RATHER THAN AN NVRHI STATE CHANGE: the ImGui pass does not
	// go through NVRHI at all. It is recorded into a VkCommandBuffer this class
	// owns and submitted by SwapBuffers, and it runs AFTER the scene's NVRHI
	// command list has already been closed and executed -- Application::Run calls
	// ExecuteNVRHICommandList before m_ImGuiLayer->Begin. A state change requested
	// on that NVRHI list at this point would sit in the tracker until the next
	// close(), which is one frame too late: a frame of sampling an image whose
	// layout disagrees with the descriptor naming it.
	//
	// THE LAYOUTS. NVRHI's Vulkan backend renders through dynamic rendering
	// (vkCmdBeginRendering), whose implicit transitions leave a colour attachment
	// in eColorAttachmentOptimal. That is the layout the descriptor set built by
	// NVRHIFramebuffer has to agree with, and it is what this barrier moves the
	// image away from. The move is NOT undone afterwards, and does not need to be:
	// next frame's vkCmdBeginRendering transitions the image back to
	// eColorAttachmentOptimal implicitly, and NVRHI's tracker still believes this
	// texture is a render target, so it never emits a conflicting explicit
	// barrier for it.
	void NVRHIContext::RecordVulkanImGuiAttachmentTransition()
	{
		// ONE-SHOT: registrations selected below are drained before command-buffer
		// or image validation. Compute registrations from a frame that never reaches
		// this call (for example, a failed vkBeginCommandBuffer) are cleared at the
		// next BeginFrame instead of being replayed against an image the UI no longer samples.
		//
		// THE OLD LAYOUT IS SELECTED BY THE CURRENT FRAME'S REGISTRATION. The
		// raster path is in eColorAttachmentOptimal after NVRHI dynamic rendering;
		// the compute-output path remains in GENERAL after its UAV dispatch.
		// BeginFrame discards stale compute registrations, so neither hardcoded
		// old layout can be replayed against a later frame's image state.
		// Drain both registrations up front. Compute output takes precedence if
		// a frame registered both kinds, because this is the one texture whose
		// incoming layout/access differs from the existing raster attachment.
		nvrhi::TextureHandle computeOutput = m_VkImGuiComputeOutput;
		m_VkImGuiComputeOutput = nullptr;
		m_VkImGuiComputeOutputReturn = nullptr;
		nvrhi::TextureHandle rasterAttachment = m_VkImGuiAttachment;
		m_VkImGuiAttachment = nullptr;
		const bool isComputeOutput = static_cast<bool>(computeOutput);
		nvrhi::TextureHandle texture = isComputeOutput ? computeOutput : rasterAttachment;
		if (!texture)
			return;

		VkCommandBuffer cmd = GetCurrentVulkanImGuiCommandBuffer();
		VkDevice device = static_cast<VkDevice>(m_VkDevice);
		if (!cmd || !device)
			return;

		VkImage image = static_cast<VkImage>(texture->getNativeObject(nvrhi::ObjectTypes::VK_Image));
		if (!image)
		{
			WL_CORE_WARN("Vulkan: viewport attachment has no VkImage, skipping the ImGui layout transition");
			return;
		}

		// The whole colour range, taken from the texture's own descriptor rather
		// than assumed: an NVRHI framebuffer colour attachment is a single-mip,
		// single-layer 2D image created with isShaderResource = true, so the aspect
		// is colour and the subresource range is the entire image.
		const nvrhi::TextureDesc &desc = texture->getDesc();
		VkImageSubresourceRange range = {};
		range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		range.baseMipLevel = 0;
		range.levelCount = desc.mipLevels;
		range.baseArrayLayer = 0;
		range.layerCount = desc.arraySize;

		// SRC STAGE IS ALL_COMMANDS, NOT COLOR_ATTACHMENT_OUTPUT, and that is the
		// load-bearing choice here. The writes being made visible were submitted to
		// this queue in an EARLIER submission -- the scene's NVRHI command list --
		// so a stage mask naming only the attachment stage asks the driver to wait
		// for writes in THIS submission and closes nothing.
		//
		// RASTER CASE: no SHADER_WRITE bit, because this attachment is created
		// with isUAV = false and its writes report as COLOR_ATTACHMENT_WRITE.
		// The compute-output case below selects SHADER_WRITE instead.
		VkImageMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = isComputeOutput
			? VK_IMAGE_LAYOUT_GENERAL
			: VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcAccessMask = isComputeOutput
			? VK_ACCESS_SHADER_WRITE_BIT
			: VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange = range;

		// The legacy barrier rather than vkCmdPipelineBarrier2: the two have the
		// same effect here, and the legacy form needs no synchronization2 feature
		// check. Nothing in this class tests for that feature, while the NVRHI
		// Vulkan backend -- which does -- is not the code recording here.
		vkCmdPipelineBarrier(cmd,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0,
			0, nullptr,
			0, nullptr,
			1, &barrier);

		if (isComputeOutput)
			m_VkImGuiComputeOutputReturn = texture;
	}

	void NVRHIContext::RecordVulkanImGuiComputeOutputReturnTransition()
	{
		// Drain the retained handle before validating anything so a failed attempt
		// cannot be replayed against a later command buffer.
		nvrhi::TextureHandle texture = m_VkImGuiComputeOutputReturn;
		m_VkImGuiComputeOutputReturn = nullptr;
		if (!texture)
			return;

		VkCommandBuffer cmd = GetCurrentVulkanImGuiCommandBuffer();
		VkDevice device = static_cast<VkDevice>(m_VkDevice);
		if (!cmd || !device)
			return;

		VkImage image = static_cast<VkImage>(texture->getNativeObject(nvrhi::ObjectTypes::VK_Image));
		if (!image)
		{
			WL_CORE_WARN("Vulkan: compute output has no VkImage, skipping its return transition");
			return;
		}

		const nvrhi::TextureDesc &desc = texture->getDesc();
		VkImageSubresourceRange range = {};
		range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		range.baseMipLevel = 0;
		range.levelCount = desc.mipLevels;
		range.baseArrayLayer = 0;
		range.layerCount = desc.arraySize;

		VkImageMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange = range;

		vkCmdPipelineBarrier(cmd,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			0,
			0, nullptr,
			0, nullptr,
			1, &barrier);
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

	// The sampler ImGui filters every bound texture with, which is the third
	// input to ImGui_ImplVulkan_AddTexture (the other two are a VkImageView and
	// a layout). ImGui's own font sampler cannot serve here: in this vendored
	// backend it lives in private backend data as bd->FontSampler, and
	// ImGui_ImplVulkan_InitInfo in this version carries no Sampler field, so
	// there is no supported way to hand one in or read one out.
	//
	// CREATED BEFORE THE DESCRIPTOR POOL, DESTROYED AFTER IT. The order is the
	// whole point: a descriptor set holds a reference to the sampler it was
	// written with, and vkDestroyDescriptorPool is what ends the sets, so
	// destroying the sampler first would leave every set in the pool pointing
	// at a dead sampler until the pool itself goes.
	void NVRHIContext::CreateVulkanImGuiSampler()
	{
		WL_PROFILE_FUNCTION();
		if (m_VkImGuiSampler != VK_NULL_HANDLE)
			return;
		VkDevice device = static_cast<VkDevice>(m_VkDevice);
		if (!device)
			return;

		// Linear filtering, matching the default ImGui font sampler, because the
		// viewport image is displayed magnified. Mipmap mode is nearest and LOD is
		// clamped to zero: an NVRHI framebuffer colour attachment has exactly one
		// mip, and a sampler with a nonzero max LOD sampling a single-level image is
		// legal but invites the driver to sample an unfiltered level.
		//
		// CLAMP TO EDGE rather than ImGui's own repeat. The only texture bound
		// through this sampler is the viewport colour attachment and its UVs run
		// exactly 0..1, so repeat addressing buys nothing and bilinear filtering
		// at the 1.0 edge would blend in the opposite edge of the image.
		VkSamplerCreateInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		info.magFilter = VK_FILTER_LINEAR;
		info.minFilter = VK_FILTER_LINEAR;
		info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.minLod = 0.0f;
		info.maxLod = 0.0f;
		info.maxAnisotropy = 1.0f;
		// borderColor and unnormalizedCoordinates are left at the zero the
		// `= {}` above gave them: transparent-black, and false. The border colour
		// is only read for a coordinate outside 0..1, which clamp-to-edge makes
		// unreachable, so naming it explicitly would only restate the zero.
		VkResult result = vkCreateSampler(device, &info, nullptr, &m_VkImGuiSampler);
		if (result != VK_SUCCESS)
		{
			// Reported rather than asserted: a missing sampler costs the viewport
			// image on Vulkan and nothing else, and the caller already handles a null
			// TexID by skipping the draw.
			WL_CORE_ERROR("Failed to create Vulkan ImGui sampler");
			m_VkImGuiSampler = VK_NULL_HANDLE;
		}
		else
		{
			WL_CORE_INFO("Vulkan ImGui sampler created");
		}
	}

	void NVRHIContext::DestroyVulkanImGuiSampler()
	{
		if (m_VkImGuiSampler == VK_NULL_HANDLE)
			return;
		VkDevice device = static_cast<VkDevice>(m_VkDevice);
		if (device)
			vkDestroySampler(device, m_VkImGuiSampler, nullptr);
		m_VkImGuiSampler = VK_NULL_HANDLE;
	}

	void NVRHIContext::CreateVulkanImGuiDescriptorPool()
	{
		WL_PROFILE_FUNCTION();
		// CREATION IS IDEMPOTENT: AN EXISTING POOL IS LEFT ALONE.
		//
		// Recreating the descriptor pool while ImGui still holds sets allocated from it
		// would invalidate those handles, so the early return protects against a caller
		// running this during a backend switch or window resize without a full teardown.
		//
		// Contrast DestroyVulkanImGuiDescriptorPool, which nulls the handle after destroying
		// it, so a subsequent create does allocate fresh.
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
		// THE POOL IS SIZED AT 1000 DESCRIPTORS OF EACH OF 11 TYPES, AND maxSets IS THE
		// SAME NUMBER TIMES THE TYPE COUNT.
		//
		// maxSets bounds descriptor SETS, not descriptors, so 11000 sets is enormously more
		// than ImGui needs -- the real limit is the per-type descriptor counts. The
		// multiplication keeps maxSets and the total consistent if a type is ever added or
		// removed, though poolSizeCount is still hardcoded to 11 separately.
		//
		// ALL ELEVEN DESCRIPTOR TYPES ARE DECLARED EVEN THOUGH ICON-ONLY UI USES A FRACTION.
		// STORAGE_BUFFER, STORAGE_IMAGE, INPUT_ATTACHMENT and both texel-buffer types are
		// listed with 1000 each but are not needed for a 2D UI. Harmless, though it does make
		// the pool larger than necessary.
		//
		// THIS FILE NEVER ALLOCATES FROM THE POOL -- there are no vkAllocateDescriptorSets or
		// vkResetDescriptorPool calls here. The pool is handed to ImGui_ImplVulkan_Init as
		// initInfo.DescriptorPool and the ImGui Vulkan backend allocates from it, so these
		// numbers are effectively that backend's budget. Running out would surface as a
		// descriptor-pool-exhausted error from inside imgui_impl_vulkan rather than here.
		//
		// FREE_DESCRIPTOR_SET_BIT is set, allowing individual returns, though nothing in this
		// file frees a set either.
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

		// THE RENDER PASS FORMAT FALLS BACK TO B8G8R8A8_SRGB WHEN THE SWAPCHAIN FORMAT IS
		// UNDEFINED.
		//
		// A render pass attachment format must match the swapchain images it will render
		// into. m_VkSwapchainImageFormat is VK_FORMAT_UNDEFINED before the swapchain is
		// created, so this path can be reached during early init or after a failed swapchain
		// creation.
		//
		// The fallback assumes an 8-bit BGRA sRGB surface, which is what the Vulkan path
		// requests. Choosing it unconditionally means a different negotiated format -- say
		// R8G8B8A8_UNORM on a surface that prefers it -- would produce a render pass whose
		// attachment format mismatches the swapchain, which Vulkan treats as undefined
		// behaviour rather than an error.
		//
		// LOAD_OP is LOAD rather than CLEAR, so the render pass preserves what is already in
		// the attachment. That is what lets ImGui composite over the frame the renderer drew;
		// the underlying clear happens in the renderer's own pass.
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
		// STORE_OP IS STORE AND THE FINAL LAYOUT IS PRESENT_SRC, because THIS PASS ONLY
		// COMPOSITES ImGui ON TOP OF AN ALREADY-RENDERED FRAME.
		//
		// There is no clear anywhere in this render pass. The swapchain image arrives with
		// the scene already in it, LOAD_OP preserves that, ImGui blends over it, and
		// STORE_OP writes the result back for presentation.
		//
		// So the scene is drawn by the renderer's own pass(es) into the same swapchain image
		// beforehand. This is why the ImGui pass depends on COLOR_ATTACHMENT_WRITE rather
		// than a discard-and-clear, and why the format fallback to B8G8R8A8_SRGB above
		// matters: it has to match what the renderer wrote.
		//
		// Samples is 1_BIT, so MSAA is not enabled on this pass. A multisampled swapchain
		// would need a resolve attachment, which is not present.
		attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkAttachmentReference colorRef = {};
		colorRef.attachment = 0;
		colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass = {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorRef;

		VkSubpassDependency dependency = {};
		// ONLY THE ACQUIRE-SIDE DEPENDENCY IS DECLARED; THE PRESENT-SIDE ONE IS MISSING.
		//
		// The single dependency runs EXTERNAL -> subpass 0, with COLOR_ATTACHMENT_OUTPUT on
		// both stage masks and COLOR_ATTACHMENT_WRITE as the destination access. That covers
		// the transition INTO the pass, from whatever layout the swapchain image was left in.
		//
		// WHAT IS NOT DECLARED IS THE MATCHING subpass 0 -> EXPASS. The attachment declares
		// finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, so a layout transition is required
		// at the end of the pass -- and layout transitions in the legacy render pass API must
		// be covered by a subpass dependency with the corresponding
		// src/dstStageMask and src/dstAccessMask.
		//
		// In practice most drivers perform the final transition anyway, so this commonly
		// works. It is not guaranteed, and it is the kind of omission that surfaces as
		// validation layer warnings rather than a hard failure -- so it can sit undetected
		// for a long time.
		//
		// A second VkSubpassDependency from subpass 0 to VK_SUBPASS_EXTERNAL, with
		// COLOR_ATTACHMENT_OUTPUT stages and COLOR_ATTACHMENT_WRITE -> 0 access, is what
		// the spec expects.
		//
		// Note also that initialLayout is COLOR_ATTACHMENT_OPTIMAL rather than
		// UNDEFINED. That is intentional here -- LOAD_OP is LOAD, so the contents are
		// expected to be preserved for ImGui to composite over -- but it means the image
		// must ALREADY be in COLOR_ATTACHMENT_OPTIMAL when this render pass begins, which
		// is the acquisition side's responsibility.
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
			// pAttachments POINTS AT A LOOP-LOCAL VARIABLE. This is legal Vulkan, and worth
			// being precise about why.
			//
			// `view` is a VkImageView handle copied out of m_VkSwapchainImageViews[i]. The
			// framebuffer creation call is synchronous and copies what it needs before
			// returning, so the address being of a soon-to-die local is fine -- Vulkan does not
			// retain the pointer.
			//
			// It reads as a mistake because taking the address of a local to fill a pNext-style
			// array is exactly the pattern that IS a bug when the callee retains it. Here the
			// variable also outlives the assignment statement, so even a hypothetical retained
			// pointer would still be valid until the end of the iteration.
			//
			// The width and height come from m_Width/m_Height rather than querying the image
			// view. They must match the swapchain image dimensions exactly -- a mismatch is a
			// framebuffer-compatibility failure at creation.
			//
			// attachmentCount is 1 and layers is 1, matching the single-colour-attachment render
			// pass above.
			fbInfo.pAttachments = &view;
			fbInfo.width = m_Width;
			fbInfo.height = m_Height;
			fbInfo.layers = 1;

			VkFramebuffer framebuffer = VK_NULL_HANDLE;
			VkResult result = vkCreateFramebuffer(device, &fbInfo, nullptr, &framebuffer);
			if (result != VK_SUCCESS)
			{
				WL_CORE_ERROR("Failed to create Vulkan ImGui framebuffer {0}", i);
				// THE FAILURE BRANCH WRITES VK_NULL_HANDLE, WHICH IS SAFE ONLY BECAUSE resize()
				// VALUE-INITIALISED THE VECTOR.
				//
				// m_VkImGuiFramebuffers.resize(m_VkSwapchainImageViews.size()) is called just above,
				// and resize on a vector of a handle type zero-initialises any new elements. So the
				// slot is already VK_NULL_HANDLE when this branch overwrites it with the same value.
				//
				// The write is therefore redundant but harmless, and it documents intent. What it
				// relies on is that the resize is the immediately preceding statement -- a future
				// edit that pushed allocation to a separate step would leave this writing to a slot
				// that may not exist.
				//
				// DestroyVulkanImGuiFramebuffers checks fb != VK_NULL_HANDLE before destroying, so
				// these nulls are correctly skipped at teardown rather than passed to
				// vkDestroyFramebuffer.
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
		// A ZERO IMAGE COUNT FALLS BACK TO 2, so the vector is never sized zero.
		//
		// m_VkImageCount is a uint32_t member defaulted to 2 and assigned from the swapchain
		// at creation, so it is normally already 2 or more. The guard covers the window
		// between construction and swapchain creation, or a swapchain creation that failed
		// and left the field at zero.
		//
		// RESIZING TO 2 RATHER THAN BAILING is the safe choice: vkAllocateCommandBuffers with
		// commandBufferCount = 0 is legal but leaves nothing to record into, and the ImGui
		// layer would then have no command buffer to submit.
		//
		// Note the allocation failure path is thorough -- it destroys the pool it just
		// created, nulls the handle, and clears the vector, so a partial failure leaves no
		// orphaned Vulkan object behind. That matters because the next call to this function
		// begins by destroying whatever is there.
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

	// THE LAST TWO CREATION CALLS HAVE THEIR RESULTS IGNORED, and the function returns
	// true regardless.
	//
	// Both of the earlier steps are checked and roll back on failure -- the render pass
	// failure destroys the descriptor pool it just created before returning false. These
	// two do not. CreateVulkanImGuiFramebuffers and CreateVulkanImGuiCommandPoolAndBuffers
	// return void and log their own failures.
	//
	// SO A PARTIAL FAILURE LEAVES A STATE THE PREDICATE BELOW CALLS INITIALIZED. Two of
	// the four gates on IsVulkanImGuiInitialized -- descriptor pool and render pass --
	// would be non-null while the framebuffers and command pool are absent or null.
	// Every caller that asks IsVulkanImGuiInitialized() (six sites, including the
	// swapchain recreation paths and the ImGui submit) would then take the Vulkan branch
	// against resources that were never created.
	//
	// The framebuffer case is the more likely of the two to fail in practice: it also
	// returns early if m_VkSwapchainImageViews is empty, so calling this before the
	// swapchain exists leaves it with nothing and no error.
	//
	// Checking the framebuffer vector and command pool after these two calls, and
	// rolling back like the render pass branch does, would close the gap.
	bool NVRHIContext::InitVulkanImGuiResources()
	{
		WL_PROFILE_FUNCTION();
		if (IsVulkanImGuiInitialized())
			return true;

		// Sampler first: it is the cheaper of the two and a Vulkan session with no
		// sampler cannot show the viewport image at all, so there is no point
		// building a descriptor pool for a backend that will bind nothing.
		CreateVulkanImGuiSampler();
		if (!m_VkImGuiSampler)
			return false;

		CreateVulkanImGuiDescriptorPool();
		if (!m_VkImGuiDescriptorPool)
		{
			DestroyVulkanImGuiSampler();
			return false;
		}

		CreateVulkanImGuiRenderPass();
		if (!m_VkImGuiRenderPass)
		{
			DestroyVulkanImGuiDescriptorPool();
			DestroyVulkanImGuiSampler();
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
		// The pool goes first. vkDestroyDescriptorPool frees every set allocated
		// from it, including ImGui's font set and the viewport set NVRHIFramebuffer
		// built, and only once none of those can still name the sampler does
		// destroying the sampler stop being a use-after-free.
		DestroyVulkanImGuiDescriptorPool();
		DestroyVulkanImGuiSampler();
	}

}
