#include "wlpch.h"
#include "ImGuiLayer.h"

#include "imgui.h"

#define IMGUI_IMPL_API
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_vulkan.h"

#include <Wasteland/Core/Application.h>
#include <Wasteland/Renderer/RendererAPI.h>
#include "Platform/NVRHI/NVRHIContext.h"

// TEMPORARY
#include <GLFW/glfw3.h>
#include <glad/glad.h>

// DirectX headers
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vulkan/vulkan.h>

#include "ImGuizmo.h"

namespace Wasteland
{

	ImGuiLayer::ImGuiLayer()
		: Layer("ImGuiLayer")
	{
	}

	ImGuiLayer::~ImGuiLayer()
	{
	}

	void ImGuiLayer::OnAttach()
	{
		WL_PROFILE_FUNCTION();

		// Setup Dear ImGui context
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO &io = ImGui::GetIO();
		(void)io;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

		// Setup Dear ImGui style
		ImGui::StyleColorsDark();
		ImGuiStyle &style = ImGui::GetStyle();
		if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) && m_CurrentBackend == RendererAPI::API::OpenGL)
		{
			style.WindowRounding = 0.0f;
			style.Colors[ImGuiCol_WindowBg].w = 1.0f;
		}

		// Initialize the appropriate backend based on current renderer API
		RendererAPI::API currentAPI = RendererAPI::GetAPI();
		InitBackend(currentAPI);
	}

	void ImGuiLayer::InitBackend(RendererAPI::API api)
	{
		WL_PROFILE_FUNCTION();

		Application &app = Application::Get();
		GLFWwindow *window = static_cast<GLFWwindow *>(app.GetWindow().GetNativeWindow());

		// If already initialized to same API, nothing to do
		if (api == m_CurrentBackend)
			return;

		// Ensure no previous platform backend is active (should be shutdown already)
		// Initialize renderer-specific backend
		switch (api)
		{
		case RendererAPI::API::OpenGL:
			ImGui_ImplGlfw_InitForOpenGL(window, true);
			ImGui_ImplOpenGL3_Init("#version 410");
			m_CurrentBackend = api;
			WL_CORE_INFO("ImGui: Initialized OpenGL3 backend");
			break;

		case RendererAPI::API::NVRHI_DX11:
		{
			auto *graphicsContext = dynamic_cast<NVRHIContext *>(app.GetWindow().GetCurrentContext());
			if (!graphicsContext)
			{
				WL_CORE_ERROR("ImGui: Failed to get NVRHI graphics context for DX11, falling back to OpenGL");
				ImGui_ImplGlfw_InitForOpenGL(window, true);
				ImGui_ImplOpenGL3_Init("#version 410");
				m_CurrentBackend = RendererAPI::API::OpenGL;
				break;
			}

			if (graphicsContext->GetAPI() != RendererAPI::API::NVRHI_DX11)
			{
				WL_CORE_ERROR("ImGui: Graphics context is not DX11 (API: {0}), falling back to OpenGL", (int)graphicsContext->GetAPI());
				ImGui_ImplGlfw_InitForOpenGL(window, true);
				ImGui_ImplOpenGL3_Init("#version 410");
				m_CurrentBackend = RendererAPI::API::OpenGL;
				break;
			}

			ID3D11Device *device = static_cast<ID3D11Device *>(graphicsContext->GetD3D11Device());
			ID3D11DeviceContext *context = static_cast<ID3D11DeviceContext *>(graphicsContext->GetD3D11Context());

			if (!device || !context)
			{
				WL_CORE_ERROR("ImGui: DX11 device or context is null, falling back to OpenGL");
				ImGui_ImplGlfw_InitForOpenGL(window, true);
				ImGui_ImplOpenGL3_Init("#version 410");
				m_CurrentBackend = RendererAPI::API::OpenGL;
				break;
			}

			HRESULT removedReason = device->GetDeviceRemovedReason();
			if (FAILED(removedReason))
			{
				WL_CORE_ERROR("ImGui: DX11 device has been removed (HRESULT: {0:X}), falling back to OpenGL", (uint32_t)removedReason);
				ImGui_ImplGlfw_InitForOpenGL(window, true);
				ImGui_ImplOpenGL3_Init("#version 410");
				m_CurrentBackend = RendererAPI::API::OpenGL;
				break;
			}

			ImGui_ImplGlfw_InitForOther(window, true);
			if (!ImGui_ImplDX11_Init(device, context))
			{
				WL_CORE_ERROR("ImGui: DX11 Init failed, falling back to OpenGL");
				ImGui_ImplGlfw_Shutdown();
				ImGui_ImplGlfw_InitForOpenGL(window, true);
				ImGui_ImplOpenGL3_Init("#version 410");
				m_CurrentBackend = RendererAPI::API::OpenGL;
				break;
			}
			m_CurrentBackend = api;
			WL_CORE_INFO("ImGui: Initialized DirectX 11 backend");
			break;
		}

		case RendererAPI::API::NVRHI_DX12:
		{
			auto *graphicsContext = dynamic_cast<NVRHIContext *>(app.GetWindow().GetCurrentContext());
			if (!graphicsContext)
			{
				WL_CORE_ERROR("ImGui: Failed to get NVRHI graphics context for DX12, falling back to OpenGL");
				ImGui_ImplGlfw_InitForOpenGL(window, true);
				ImGui_ImplOpenGL3_Init("#version 410");
				m_CurrentBackend = RendererAPI::API::OpenGL;
				break;
			}

			if (graphicsContext->GetAPI() != RendererAPI::API::NVRHI_DX12)
			{
				WL_CORE_ERROR("ImGui: Graphics context is not DX12 (API: {0}), falling back to OpenGL", (int)graphicsContext->GetAPI());
				ImGui_ImplGlfw_InitForOpenGL(window, true);
				ImGui_ImplOpenGL3_Init("#version 410");
				m_CurrentBackend = RendererAPI::API::OpenGL;
				break;
			}

			ID3D12Device *device = static_cast<ID3D12Device *>(graphicsContext->GetD3D12Device());
			ID3D12CommandQueue *commandQueue = static_cast<ID3D12CommandQueue *>(graphicsContext->GetD3D12CommandQueue());
			ID3D12DescriptorHeap *srvHeap = static_cast<ID3D12DescriptorHeap *>(graphicsContext->GetD3D12ImGuiSRVHeap());
			D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = graphicsContext->GetD3D12ImGuiSRVHeapCPU();
			D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = graphicsContext->GetD3D12ImGuiSRVHeapGPU();

			if (!device || !commandQueue || !srvHeap)
			{
				WL_CORE_ERROR("ImGui: Failed to get DX12 device/command queue/descriptor heap, falling back to OpenGL");
				ImGui_ImplGlfw_InitForOpenGL(window, true);
				ImGui_ImplOpenGL3_Init("#version 410");
				m_CurrentBackend = RendererAPI::API::OpenGL;
				break;
			}

			// Validate device
			if (FAILED(device->GetDeviceRemovedReason()))
			{
				WL_CORE_ERROR("ImGui: DX12 device removed, falling back to OpenGL");
				ImGui_ImplGlfw_InitForOpenGL(window, true);
				ImGui_ImplOpenGL3_Init("#version 410");
				m_CurrentBackend = RendererAPI::API::OpenGL;
				break;
			}

			ImGui_ImplGlfw_InitForOther(window, true);
			// DX12 needs num frames in flight = 2, format = R8G8B8A8_UNORM
			if (!ImGui_ImplDX12_Init(device, 2, DXGI_FORMAT_R8G8B8A8_UNORM, srvHeap, cpuHandle, gpuHandle))
			{
				WL_CORE_ERROR("ImGui: DX12 Init failed, falling back to OpenGL");
				ImGui_ImplGlfw_Shutdown();
				ImGui_ImplGlfw_InitForOpenGL(window, true);
				ImGui_ImplOpenGL3_Init("#version 410");
				m_CurrentBackend = RendererAPI::API::OpenGL;
				break;
			}
			// Create device objects (fonts)
			ImGui_ImplDX12_CreateDeviceObjects();
			m_CurrentBackend = api;
			WL_CORE_INFO("ImGui: Initialized DirectX 12 backend");
			break;
		}

		case RendererAPI::API::NVRHI_Vulkan:
		{
			auto *graphicsContext = dynamic_cast<NVRHIContext *>(app.GetWindow().GetCurrentContext());
			if (!graphicsContext)
			{
				WL_CORE_ERROR("ImGui: Failed to get NVRHI graphics context for Vulkan, falling back to OpenGL");
				ImGui_ImplGlfw_InitForOpenGL(window, true);
				ImGui_ImplOpenGL3_Init("#version 410");
				m_CurrentBackend = RendererAPI::API::OpenGL;
				break;
			}

			if (graphicsContext->GetAPI() != RendererAPI::API::NVRHI_Vulkan)
			{
				WL_CORE_ERROR("ImGui: Graphics context is not Vulkan (API: {0}), falling back to OpenGL", (int)graphicsContext->GetAPI());
				ImGui_ImplGlfw_InitForOpenGL(window, true);
				ImGui_ImplOpenGL3_Init("#version 410");
				m_CurrentBackend = RendererAPI::API::OpenGL;
				break;
			}

			VkInstance instance = graphicsContext->GetVkInstance();
			VkPhysicalDevice physicalDevice = graphicsContext->GetVkPhysicalDevice();
			VkDevice device = graphicsContext->GetVkDevice();
			VkQueue queue = graphicsContext->GetVkGraphicsQueue();

			if (!instance || !physicalDevice || !device || !queue)
			{
				WL_CORE_ERROR("ImGui: Vulkan native objects are null, falling back to OpenGL");
				ImGui_ImplGlfw_InitForOpenGL(window, true);
				ImGui_ImplOpenGL3_Init("#version 410");
				m_CurrentBackend = RendererAPI::API::OpenGL;
				break;
			}

			if (!graphicsContext->InitVulkanImGuiResources())
			{
				WL_CORE_ERROR("ImGui: Failed to init Vulkan ImGui resources, falling back to OpenGL");
				ImGui_ImplGlfw_InitForOpenGL(window, true);
				ImGui_ImplOpenGL3_Init("#version 410");
				m_CurrentBackend = RendererAPI::API::OpenGL;
				break;
			}

			VkDescriptorPool descriptorPool = graphicsContext->GetVkImGuiDescriptorPool();
			VkRenderPass renderPass = graphicsContext->GetVkImGuiRenderPass();

			if (!descriptorPool || !renderPass)
			{
				WL_CORE_ERROR("ImGui: Vulkan descriptor pool or render pass is null, falling back to OpenGL");
				graphicsContext->ShutdownVulkanImGuiResources();
				ImGui_ImplGlfw_InitForOpenGL(window, true);
				ImGui_ImplOpenGL3_Init("#version 410");
				m_CurrentBackend = RendererAPI::API::OpenGL;
				break;
			}

			ImGui_ImplGlfw_InitForOther(window, true);

			ImGui_ImplVulkan_InitInfo initInfo = {};
			initInfo.Instance = instance;
			initInfo.PhysicalDevice = physicalDevice;
			initInfo.Device = device;
			initInfo.QueueFamily = graphicsContext->GetVkGraphicsQueueFamilyIndex();
			initInfo.Queue = queue;
			initInfo.PipelineCache = VK_NULL_HANDLE;
			initInfo.DescriptorPool = descriptorPool;
			initInfo.Subpass = 0;
			initInfo.MinImageCount = graphicsContext->GetVkImageCount();
			initInfo.ImageCount = graphicsContext->GetVkImageCount();
			initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
			initInfo.Allocator = nullptr;
			initInfo.CheckVkResultFn = nullptr;

			if (!ImGui_ImplVulkan_Init(&initInfo, renderPass))
			{
				WL_CORE_ERROR("ImGui: Vulkan Init failed, falling back to OpenGL");
				ImGui_ImplGlfw_Shutdown();
				ImGui_ImplVulkan_Shutdown();
				graphicsContext->ShutdownVulkanImGuiResources();
				ImGui_ImplGlfw_InitForOpenGL(window, true);
				ImGui_ImplOpenGL3_Init("#version 410");
				m_CurrentBackend = RendererAPI::API::OpenGL;
				break;
			}

			// Upload fonts
			VkCommandBuffer cmd = graphicsContext->GetCurrentVulkanImGuiCommandBuffer();
			if (cmd)
			{
				VkCommandBufferBeginInfo beginInfo = {};
				beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
				beginInfo.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
				VkResult res = vkBeginCommandBuffer(cmd, &beginInfo);
				if (res == VK_SUCCESS)
				{
					ImGui_ImplVulkan_CreateFontsTexture(cmd);
					vkEndCommandBuffer(cmd);
					VkSubmitInfo submitInfo = {};
					submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
					submitInfo.commandBufferCount = 1;
					submitInfo.pCommandBuffers = &cmd;
					vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
					vkDeviceWaitIdle(device);
					ImGui_ImplVulkan_DestroyFontUploadObjects();
					// Reset for next frame use
					vkResetCommandBuffer(cmd, 0);
				}
				else
				{
					WL_CORE_WARN("ImGui: Failed to begin command buffer for font upload");
				}
			}

			m_CurrentBackend = api;
			WL_CORE_INFO("ImGui: Initialized Vulkan backend");
			break;
		}

		default:
			WL_CORE_ERROR("ImGui: Unsupported renderer API {0}, falling back to OpenGL", (int)api);
			ImGui_ImplGlfw_InitForOpenGL(window, true);
			ImGui_ImplOpenGL3_Init("#version 410");
			m_CurrentBackend = RendererAPI::API::OpenGL;
			break;
		}
	}

	void ImGuiLayer::ShutdownBackend()
	{
		WL_PROFILE_FUNCTION();

		if (m_CurrentBackend == RendererAPI::API::None)
			return;

		// Shutdown renderer-specific backend
		switch (m_CurrentBackend)
		{
		case RendererAPI::API::OpenGL:
			ImGui_ImplOpenGL3_Shutdown();
			break;

		case RendererAPI::API::NVRHI_DX11:
			ImGui_ImplDX11_Shutdown();
			break;

		case RendererAPI::API::NVRHI_DX12:
			ImGui_ImplDX12_Shutdown();
			break;

		case RendererAPI::API::NVRHI_Vulkan:
		{
			ImGui_ImplVulkan_Shutdown();
			// Shutdown NVRHI Vulkan ImGui resources
			Application &app = Application::Get();
			auto *ctx = dynamic_cast<NVRHIContext *>(app.GetWindow().GetCurrentContext());
			if (ctx)
				ctx->ShutdownVulkanImGuiResources();
			break;
		}

		default:
			break;
		}

		// Shutdown platform bindings
		ImGui_ImplGlfw_Shutdown();
		m_CurrentBackend = RendererAPI::API::None;
	}

	void ImGuiLayer::ShutdownBackendForWindowRecreate()
	{
		WL_PROFILE_FUNCTION();
		if (m_CurrentBackend == RendererAPI::API::None)
			return;
		WL_CORE_INFO("ImGui: Shutting down backend for window recreate ({0})", (int)m_CurrentBackend);
		ShutdownBackend();
	}

	void ImGuiLayer::SwitchBackend(RendererAPI::API api)
	{
		WL_PROFILE_FUNCTION();

		if (api == m_CurrentBackend)
			return;

		WL_CORE_INFO("ImGui: Backend switch requested from {0} to {1} (deferred)", (int)m_CurrentBackend, (int)api);

		// Mark switch as pending - will be performed at the start of next frame
		m_PendingBackendSwitch = api;
		m_BackendSwitchPending = true;
	}

	void ImGuiLayer::PerformBackendSwitch()
	{
		WL_PROFILE_FUNCTION();

		if (!m_BackendSwitchPending)
			return;

		RendererAPI::API api = m_PendingBackendSwitch;
		m_BackendSwitchPending = false;

		if (api == m_CurrentBackend)
			return;

		WL_CORE_INFO("ImGui: Performing backend switch to {0}", (int)api);

		// Shutdown current backend fully (handles None check internally)
		ShutdownBackend();

		// Clear font atlas for new backend and re-add default font
		// Fonts must be rebuilt after Clear() - add default font back
		ImGuiIO &io = ImGui::GetIO();
		io.Fonts->Clear();
		io.Fonts->AddFontDefault();
		// TexID will be recreated by new backend's CreateDeviceObjects / CreateFontsTexture

		// Initialize new backend
		InitBackend(api);

		// Ensure style is correct after switch
		if (m_CurrentBackend == RendererAPI::API::OpenGL && (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
		{
			ImGuiStyle &style = ImGui::GetStyle();
			style.WindowRounding = 0.0f;
			style.Colors[ImGuiCol_WindowBg].w = 1.0f;
		}
	}

	void ImGuiLayer::OnDetach()
	{
		WL_PROFILE_FUNCTION();

		ShutdownBackend();
		ImGui::DestroyContext();
	}

	void ImGuiLayer::OnEvent(Event &event)
	{
		if (m_BlockEvents)
		{
			ImGuiIO &io = ImGui::GetIO();
			event.Handled |= event.IsInCategory(EventCategoryMouse) & io.WantCaptureMouse;
			event.Handled |= event.IsInCategory(EventCategoryKeyboard) & io.WantCaptureKeyboard;
		}

		// Handle window resize for Vulkan/DX12 to recreate ImGui resources
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<WindowResizeEvent>([this](WindowResizeEvent &e)
											   {
			if (m_CurrentBackend == RendererAPI::API::NVRHI_Vulkan)
			{
				// Notify ImGui Vulkan about image count change
				ImGui_ImplVulkan_SetMinImageCount(2);
			}
			else if (m_CurrentBackend == RendererAPI::API::NVRHI_DX12)
			{
				ImGui_ImplDX12_InvalidateDeviceObjects();
				ImGui_ImplDX12_CreateDeviceObjects();
			}
			else if (m_CurrentBackend == RendererAPI::API::NVRHI_DX11)
			{
				ImGui_ImplDX11_InvalidateDeviceObjects();
				ImGui_ImplDX11_CreateDeviceObjects();
			}
			return false; });
	}

	void ImGuiLayer::Begin()
	{
		WL_PROFILE_FUNCTION();

		// Perform deferred backend switch if pending
		if (m_BackendSwitchPending)
		{
			PerformBackendSwitch();
		}

		// Validate current backend
		if (m_CurrentBackend == RendererAPI::API::None)
		{
			WL_CORE_ERROR("ImGui: No backend initialized, skipping frame");
			return;
		}

		// Call the appropriate NewFrame function based on current backend
		switch (m_CurrentBackend)
		{
		case RendererAPI::API::OpenGL:
			ImGui_ImplOpenGL3_NewFrame();
			break;

		case RendererAPI::API::NVRHI_DX11:
			ImGui_ImplDX11_NewFrame();
			break;

		case RendererAPI::API::NVRHI_DX12:
			ImGui_ImplDX12_NewFrame();
			break;

		case RendererAPI::API::NVRHI_Vulkan:
			ImGui_ImplVulkan_NewFrame();
			break;

		default:
			WL_CORE_WARN("ImGui: Unknown backend {0}, using OpenGL", (int)m_CurrentBackend);
			ImGui_ImplOpenGL3_NewFrame();
			break;
		}

		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
		ImGuizmo::BeginFrame();
	}

	void ImGuiLayer::End()
	{
		WL_PROFILE_FUNCTION();

		// Validate current backend
		if (m_CurrentBackend == RendererAPI::API::None)
		{
			return; // Skip rendering if no backend
		}

		ImGuiIO &io = ImGui::GetIO();
		Application &app = Application::Get();
		io.DisplaySize = ImVec2((float)app.GetWindow().GetWidth(), (float)app.GetWindow().GetHeight());

		// Rendering
		ImGui::Render();

		// Call the appropriate RenderDrawData function based on current backend
		switch (m_CurrentBackend)
		{
		case RendererAPI::API::OpenGL:
			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
			break;

		case RendererAPI::API::NVRHI_DX11:
		{
			auto *ctx = dynamic_cast<NVRHIContext *>(app.GetWindow().GetCurrentContext());
			if (!ctx)
			{
				WL_CORE_ERROR("ImGui: DX11 context is null");
				break;
			}
			ID3D11Device *device = static_cast<ID3D11Device *>(ctx->GetD3D11Device());
			ID3D11DeviceContext *d3dCtx = static_cast<ID3D11DeviceContext *>(ctx->GetD3D11Context());
			if (!device || !d3dCtx)
			{
				WL_CORE_WARN("ImGui: DX11 device/context not ready, skipping render");
				break;
			}

			// Ensure ImGui DX11 device objects exist.
			// D3DCompile can fail at init if d3dcompiler_47.dll is missing, leaving backend objects null.
			if (!ImGui_ImplDX11_CreateDeviceObjects())
			{
				WL_CORE_ERROR("ImGui: DX11 CreateDeviceObjects failed (check d3dcompiler_47.dll), skipping render");
				break;
			}

			ImDrawData *draw_data = ImGui::GetDrawData();
			if (!draw_data || draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f || draw_data->CmdListsCount == 0 || draw_data->TotalVtxCount == 0)
				break;

			// Device removed / TDR check
			HRESULT removedReason = device->GetDeviceRemovedReason();
			if (FAILED(removedReason))
			{
				WL_CORE_ERROR("ImGui: DX11 device removed (HRESULT: {0:X}), skipping render", (uint32_t)removedReason);
				break;
			}

			// DX11 backend does NOT set RTV - caller must bind it.
			// NVRHI may leave the immediate context without a guaranteed RTV after ExecuteNVRHICommandList().
			// Explicitly bind current backbuffer RTV to avoid driver crash inside DrawIndexed.
			void *swapPtr = ctx->GetDXGISwapChain();
			if (!swapPtr)
			{
				WL_CORE_WARN("ImGui: DX11 swapchain is null, skipping render");
				break;
			}
			IDXGISwapChain3 *swapChain = static_cast<IDXGISwapChain3 *>(swapPtr);
			uint32_t idx = ctx->GetCurrentBackBufferIndex();
			ID3D11Texture2D *backBuffer = nullptr;
			HRESULT hr = swapChain->GetBuffer(idx, IID_PPV_ARGS(&backBuffer));
			if (FAILED(hr) || !backBuffer)
			{
				WL_CORE_ERROR("ImGui: Failed to get DX11 backbuffer {0} (HRESULT: {1:X})", idx, (uint32_t)hr);
				break;
			}
			ID3D11RenderTargetView *rtv = nullptr;
			hr = device->CreateRenderTargetView(backBuffer, nullptr, &rtv);
			backBuffer->Release();
			if (FAILED(hr) || !rtv)
			{
				WL_CORE_ERROR("ImGui: Failed to create DX11 RTV (HRESULT: {0:X})", (uint32_t)hr);
				break;
			}

			d3dCtx->OMSetRenderTargets(1, &rtv, nullptr);
			ImGui_ImplDX11_RenderDrawData(draw_data);

			// Release transient RTV after draw.
			ID3D11RenderTargetView *nullRTV = nullptr;
			d3dCtx->OMSetRenderTargets(1, &nullRTV, nullptr);
			rtv->Release();
			break;
		}

		case RendererAPI::API::NVRHI_DX12:
		{
			auto *ctx = dynamic_cast<NVRHIContext *>(app.GetWindow().GetCurrentContext());
			if (!ctx)
			{
				WL_CORE_ERROR("ImGui: DX12 context is null");
				break;
			}

			ID3D12GraphicsCommandList *cmdList = static_cast<ID3D12GraphicsCommandList *>(ctx->GetD3D12ImGuiCommandList());
			ID3D12CommandAllocator *allocator = static_cast<ID3D12CommandAllocator *>(ctx->GetD3D12ImGuiCommandAllocator());
			ID3D12DescriptorHeap *srvHeap = static_cast<ID3D12DescriptorHeap *>(ctx->GetD3D12ImGuiSRVHeap());
			ID3D12DescriptorHeap *rtvHeap = static_cast<ID3D12DescriptorHeap *>(ctx->GetD3D12ImGuiRTVHeap());
			ID3D12CommandQueue *queue = static_cast<ID3D12CommandQueue *>(ctx->GetD3D12CommandQueue());
			void *backBufferResource = ctx->GetD3D12CurrentBackBufferResource();

			if (!cmdList || !allocator || !srvHeap || !rtvHeap || !queue || !backBufferResource)
			{
				WL_CORE_WARN("ImGui: DX12 ImGui resources not ready, skipping render");
				break;
			}

			ID3D12Resource *backBuffer = static_cast<ID3D12Resource *>(backBufferResource);
			uint32_t rtvSize = ctx->GetD3D12RTVDescriptorSize();
			uint32_t idx = ctx->GetCurrentBackBufferIndex();
			D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
			rtvHandle.ptr += idx * rtvSize;

			// Wait for previous frame if needed (simple fence check)
			// Reset allocator and command list
			HRESULT hr = allocator->Reset();
			if (FAILED(hr))
			{
				WL_CORE_ERROR("ImGui: Failed to reset DX12 allocator");
				break;
			}
			hr = cmdList->Reset(allocator, nullptr);
			if (FAILED(hr))
			{
				WL_CORE_ERROR("ImGui: Failed to reset DX12 command list");
				break;
			}

			// Transition backbuffer to render target
			D3D12_RESOURCE_BARRIER barrier = {};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
			barrier.Transition.pResource = backBuffer;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			cmdList->ResourceBarrier(1, &barrier);

			cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
			ID3D12DescriptorHeap *heaps[] = {srvHeap};
			cmdList->SetDescriptorHeaps(1, heaps);

			ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList);

			// Transition back to present
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
			cmdList->ResourceBarrier(1, &barrier);

			hr = cmdList->Close();
			if (FAILED(hr))
			{
				WL_CORE_ERROR("ImGui: Failed to close DX12 command list");
				break;
			}

			ID3D12CommandList *lists[] = {cmdList};
			queue->ExecuteCommandLists(1, lists);

			// Signal fence for next frame's wait (optional, SwapBuffers handles present fence)
			// Increment fence value for tracking
			if (ctx->GetD3D12ImGuiFence())
			{
				ID3D12Fence *fence = static_cast<ID3D12Fence *>(ctx->GetD3D12ImGuiFence());
				uint64_t fenceValue = ctx->GetD3D12ImGuiFenceValue();
				queue->Signal(fence, fenceValue);
				const_cast<NVRHIContext *>(ctx)->IncrementD3D12FenceValue();
			}

			break;
		}

		case RendererAPI::API::NVRHI_Vulkan:
		{
			auto *ctx = dynamic_cast<NVRHIContext *>(app.GetWindow().GetCurrentContext());
			if (!ctx || !ctx->IsVulkanImGuiInitialized())
			{
				WL_CORE_WARN("ImGui: Vulkan ImGui not initialized, skipping render");
				break;
			}

			VkCommandBuffer cmd = ctx->GetCurrentVulkanImGuiCommandBuffer();
			VkRenderPass rp = ctx->GetVkImGuiRenderPass();
			VkFramebuffer fb = ctx->GetVkCurrentImGuiFramebuffer();

			if (!cmd || !rp || !fb)
			{
				WL_CORE_WARN("ImGui: Vulkan ImGui command buffer / render pass / framebuffer is null");
				break;
			}

			VkCommandBufferBeginInfo beginInfo = {};
			beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			beginInfo.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			VkResult res = vkBeginCommandBuffer(cmd, &beginInfo);
			if (res != VK_SUCCESS)
			{
				WL_CORE_ERROR("ImGui: Failed to begin Vulkan command buffer");
				break;
			}

			VkRenderPassBeginInfo rpBegin = {};
			rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			rpBegin.renderPass = rp;
			rpBegin.framebuffer = fb;
			rpBegin.renderArea.offset = {0, 0};
			rpBegin.renderArea.extent = {ctx->GetWidth(), ctx->GetHeight()};
			rpBegin.clearValueCount = 0;
			rpBegin.pClearValues = nullptr;

			vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
			ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
			vkCmdEndRenderPass(cmd);

			res = vkEndCommandBuffer(cmd);
			if (res != VK_SUCCESS)
			{
				WL_CORE_ERROR("ImGui: Failed to end Vulkan command buffer");
			}
			// Submission will be done in NVRHIContext::SwapBuffers
			break;
		}

		default:
			WL_CORE_WARN("ImGui: Unknown backend {0}, using OpenGL", (int)m_CurrentBackend);
			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
			break;
		}

		if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) && m_CurrentBackend == RendererAPI::API::OpenGL)
		{
			GLFWwindow *backup_current_context = glfwGetCurrentContext();
			ImGui::UpdatePlatformWindows();
			ImGui::RenderPlatformWindowsDefault();
			glfwMakeContextCurrent(backup_current_context);
		}
	}

	void ImGuiLayer::OnImGuiRender()
	{
		// static bool show = false;
		// ImGui::ShowDemoWindow(&show);
	}

}
