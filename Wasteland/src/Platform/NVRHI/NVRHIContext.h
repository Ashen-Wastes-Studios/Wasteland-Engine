#pragma once

#include "Wasteland/Renderer/GraphicsContext.h"
#include "Wasteland/Renderer/RendererAPI.h"

#include <vulkan/vulkan.h>
#include <d3d11.h>
#include <d3d12.h>
#include <nvrhi/nvrhi.h>
#include <vector>

struct GLFWwindow;

namespace Wasteland
{

	class NVRHIContext : public GraphicsContext
	{
	public:
		NVRHIContext(GLFWwindow *windowHandle, RendererAPI::API api);
		virtual ~NVRHIContext();

		virtual void Init() override;
		virtual void SwapBuffers() override;

		// NVRHI-specific accessors
		nvrhi::IDevice *GetDevice() const { return m_Device; }
		nvrhi::ICommandList *GetCommandList() const { return m_CommandList; }
		nvrhi::FramebufferHandle GetCurrentFramebuffer() const;
		nvrhi::TextureHandle GetCurrentBackBuffer() const;
		uint32_t GetCurrentBackBufferIndex() const { return m_CurrentBackBufferIndex; }
		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }

		// Native DirectX object accessors (for ImGui and other integrations)
		void *GetD3D11Device() const { return m_D3D11Device; }
		void *GetD3D11Context() const { return m_D3D11Context; }
		void *GetD3D12Device() const { return m_D3D12Device; }
		void *GetD3D12CommandQueue() const { return m_D3D12CommandQueue; }

		// DX12 ImGui descriptor heap
		void *GetD3D12ImGuiSRVHeap() const { return m_D3D12ImGuiSRVHeap; }
		D3D12_CPU_DESCRIPTOR_HANDLE GetD3D12ImGuiSRVHeapCPU() const { return m_D3D12ImGuiSRVHeapCPU; }
		D3D12_GPU_DESCRIPTOR_HANDLE GetD3D12ImGuiSRVHeapGPU() const { return m_D3D12ImGuiSRVHeapGPU; }

		// DX12 ImGui command objects
		void *GetD3D12ImGuiCommandAllocator() const { return m_D3D12ImGuiCommandAllocator; }
		void *GetD3D12ImGuiCommandList() const { return m_D3D12ImGuiCommandList; }
		void *GetD3D12ImGuiFence() const { return m_D3D12ImGuiFence; }
		void *GetD3D12ImGuiFenceEvent() const { return m_D3D12ImGuiFenceEvent; }
		uint64_t GetD3D12ImGuiFenceValue() const { return m_D3D12ImGuiFenceValue; }
		void IncrementD3D12FenceValue() { m_D3D12ImGuiFenceValue++; }
		void *GetD3D12ImGuiRTVHeap() const { return m_D3D12ImGuiRTVHeap; }
		uint32_t GetD3D12RTVDescriptorSize() const { return m_D3D12RTVDescriptorSize; }
		void *GetD3D12CurrentBackBufferResource() const;

		// Native Vulkan object accessors
		VkInstance GetVkInstance() const { return m_VkInstance; }
		VkPhysicalDevice GetVkPhysicalDevice() const { return m_VkPhysicalDevice; }
		VkDevice GetVkDevice() const { return m_VkDevice; }
		VkQueue GetVkGraphicsQueue() const { return m_VkGraphicsQueue; }
		VkSurfaceKHR GetVkSurface() const { return m_VkSurface; }
		VkSwapchainKHR GetVkSwapchain() const { return m_VkSwapchain; }

		// Vulkan queue family / swapchain info (needed for ImGui)
		uint32_t GetVkGraphicsQueueFamilyIndex() const { return m_VkGraphicsQueueFamilyIndex; }
		uint32_t GetVkPresentQueueFamilyIndex() const { return m_VkPresentQueueFamilyIndex; }
		uint32_t GetVkImageCount() const { return m_VkImageCount; }
		uint32_t GetVkSwapchainImageFormat() const { return m_VkSwapchainImageFormat; }
		uint32_t GetVkFrameIndex() const { return m_VkFrameIndex; }

		// Vulkan ImGui resources
		VkDescriptorPool GetVkImGuiDescriptorPool() const { return m_VkImGuiDescriptorPool; }
		VkRenderPass GetVkImGuiRenderPass() const { return m_VkImGuiRenderPass; }
		VkFramebuffer GetVkCurrentImGuiFramebuffer() const;
		VkCommandPool GetVkImGuiCommandPool() const { return m_VkImGuiCommandPool; }
		VkCommandBuffer GetCurrentVulkanImGuiCommandBuffer() const;
		bool IsVulkanImGuiInitialized() const { return m_VkImGuiDescriptorPool != nullptr && m_VkImGuiRenderPass != nullptr; }

		// Get the renderer API type
		RendererAPI::API GetAPI() const { return m_API; }

		void Resize(uint32_t width, uint32_t height);
		void BeginFrame() override;
		void ExecuteNVRHICommandList();

		// Vulkan ImGui lifecycle (called by ImGuiLayer)
		bool InitVulkanImGuiResources();
		void ShutdownVulkanImGuiResources();

	private:
		void CreateDeviceDX11();
		void CreateDeviceDX12();
		void CreateDeviceVulkan();
		void CreateSwapChain();
		void CreateBackBufferFramebuffers();
		void CreateVulkanSwapChain();
		void DestroyVulkanSwapChain();

		// Vulkan-specific helpers
		bool SelectVulkanPhysicalDevice();
		void CreateVulkanDevice();
		void CreateVulkanSurface();
		void GetVulkanQueueFamilyIndices();

		// DX12 ImGui helpers
		void CreateD3D12ImGuiCommandObjects();
		void DestroyD3D12ImGuiCommandObjects();

		// Vulkan ImGui helpers
		void CreateVulkanImGuiDescriptorPool();
		void DestroyVulkanImGuiDescriptorPool();
		void CreateVulkanImGuiRenderPass();
		void DestroyVulkanImGuiRenderPass();
		void CreateVulkanImGuiFramebuffers();
		void DestroyVulkanImGuiFramebuffers();
		void CreateVulkanImGuiCommandPoolAndBuffers();
		void DestroyVulkanImGuiCommandPoolAndBuffers();

		GLFWwindow *m_WindowHandle;
		RendererAPI::API m_API;
		// NVRHI objects
		nvrhi::DeviceHandle m_Device;
		nvrhi::CommandListHandle m_CommandList;

		// Backbuffer management
		std::vector<nvrhi::TextureHandle> m_BackBuffers;
		std::vector<nvrhi::FramebufferHandle> m_BackBufferFramebuffers;
		uint32_t m_CurrentBackBufferIndex = 0;

		// DX11 / DX12 native objects
		void *m_D3D11Device = nullptr;
		void *m_D3D11Context = nullptr;
		void *m_D3D12Device = nullptr;
		void *m_D3D12CommandQueue = nullptr;
		void *m_DXGISwapChain3 = nullptr;

		// DX12 ImGui SRV descriptor heap
		void *m_D3D12ImGuiSRVHeap = nullptr;
		D3D12_CPU_DESCRIPTOR_HANDLE m_D3D12ImGuiSRVHeapCPU = {};
		D3D12_GPU_DESCRIPTOR_HANDLE m_D3D12ImGuiSRVHeapGPU = {};
		void *m_D3D12ImGuiRTVHeap = nullptr;
		uint32_t m_D3D12RTVDescriptorSize = 0;

		// DX12 ImGui command objects
		void *m_D3D12ImGuiCommandAllocator = nullptr;
		void *m_D3D12ImGuiCommandList = nullptr;
		void *m_D3D12ImGuiFence = nullptr;
		uint64_t m_D3D12ImGuiFenceValue = 0;
		void *m_D3D12ImGuiFenceEvent = nullptr;

		// Vulkan native objects
		VkInstance m_VkInstance = VK_NULL_HANDLE;
		VkPhysicalDevice m_VkPhysicalDevice = VK_NULL_HANDLE;
		VkDevice m_VkDevice = VK_NULL_HANDLE;
		VkQueue m_VkGraphicsQueue = VK_NULL_HANDLE;
		VkQueue m_VkPresentQueue = VK_NULL_HANDLE;
		VkSurfaceKHR m_VkSurface = VK_NULL_HANDLE;
		VkSwapchainKHR m_VkSwapchain = VK_NULL_HANDLE;
		uint32_t m_VkGraphicsQueueFamilyIndex = 0;
		uint32_t m_VkPresentQueueFamilyIndex = 0;

		// Vulkan swapchain image management
		std::vector<VkImage> m_VkSwapchainImages;
		std::vector<VkImageView> m_VkSwapchainImageViews;
		std::vector<nvrhi::TextureHandle> m_VkBackBuffers;
		std::vector<nvrhi::FramebufferHandle> m_VkFramebuffers;

		// Vulkan synchronization
		std::vector<VkFence> m_VkFences;
		std::vector<VkSemaphore> m_VkImageAvailableSemaphores;
		std::vector<VkSemaphore> m_VkRenderFinishedSemaphores;

		// Vulkan format and capabilities
		uint32_t m_VkSwapchainImageFormat = 0;
		// Vulkan command pool (for NVRHI / general)
		VkCommandPool m_VkCommandPool = VK_NULL_HANDLE;

		// Vulkan ImGui resources
		VkDescriptorPool m_VkImGuiDescriptorPool = VK_NULL_HANDLE;
		VkRenderPass m_VkImGuiRenderPass = VK_NULL_HANDLE;
		std::vector<VkFramebuffer> m_VkImGuiFramebuffers;
		VkCommandPool m_VkImGuiCommandPool = VK_NULL_HANDLE;
		std::vector<VkCommandBuffer> m_VkImGuiCommandBuffers;

		// Frame index for sync objects
		uint32_t m_VkFrameIndex = 0;
		uint32_t m_VkImageCount = 2;

		// Vulkan validation layers enabled in debug builds
		bool m_VkValidationLayersEnabled = false;
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
	};

}
