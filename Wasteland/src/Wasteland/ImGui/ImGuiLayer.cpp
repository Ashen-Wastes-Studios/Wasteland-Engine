#include "wlpch.h"
#include "ImGuiLayer.h"

#include "imgui.h"

#define IMGUI_IMPL_API
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_dx12.h"

#include <Wasteland/Core/Application.h>
#include <Wasteland/Renderer/RendererAPI.h>
#include "Platform/NVRHI/NVRHIContext.h"

// TEMPORATY
#include <GLFW/glfw3.h>
#include <glad/glad.h>

// DirectX headers
#include <d3d11.h>
#include <d3d12.h>

#include "ImGuizmo.h"

namespace Wasteland {

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
		ImGuiIO& io = ImGui::GetIO(); (void)io;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

		// Setup Dear ImGui style
		ImGui::StyleColorsDark();

		ImGuiStyle& style = ImGui::GetStyle();
		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
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

		Application& app = Application::Get();
		GLFWwindow* window = static_cast<GLFWwindow*>(app.GetWindow().GetNativeWindow());

		// Initialize platform bindings (GLFW works for all backends)
		ImGui_ImplGlfw_InitForOpenGL(window, true);

		// Initialize renderer-specific backend
		switch (api)
		{
		case RendererAPI::API::OpenGL:
			ImGui_ImplOpenGL3_Init("#version 410");
			WL_CORE_INFO("ImGui: Initialized OpenGL3 backend");
			break;

		case RendererAPI::API::NVRHI_DX11:
		{
			// Get NVRHI context and native DX11 objects
			auto* graphicsContext = dynamic_cast<NVRHIContext*>(app.GetWindow().GetCurrentContext());
			if (graphicsContext)
			{
				ID3D11Device* device = static_cast<ID3D11Device*>(graphicsContext->GetD3D11Device());
				ID3D11DeviceContext* context = static_cast<ID3D11DeviceContext*>(graphicsContext->GetD3D11Context());
				
				// Validate pointers before using them
				if (device && context)
				{
					// Check if device is still valid by querying interface
					ID3D11Device* testDevice = nullptr;
					HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&testDevice));
					if (SUCCEEDED(hr) && testDevice)
					{
						testDevice->Release();
						ImGui_ImplDX11_Init(device, context);
						WL_CORE_INFO("ImGui: Initialized DirectX 11 backend");
					}
					else
					{
						WL_CORE_ERROR("ImGui: DX11 device is invalid or has been released");
						// Fall back to OpenGL
						ImGui_ImplOpenGL3_Init("#version 410");
						m_CurrentBackend = RendererAPI::API::OpenGL;
						WL_CORE_INFO("ImGui: Fell back to OpenGL3 backend");
					}
				}
				else
				{
					WL_CORE_ERROR("ImGui: Failed to get DX11 device/context (null pointers)");
					// Fall back to OpenGL
					ImGui_ImplOpenGL3_Init("#version 410");
					m_CurrentBackend = RendererAPI::API::OpenGL;
					WL_CORE_INFO("ImGui: Fell back to OpenGL3 backend");
				}
			}
			else
			{
				WL_CORE_ERROR("ImGui: Failed to get NVRHI graphics context");
			}
			break;
		}

		case RendererAPI::API::NVRHI_DX12:
		{
			// Get NVRHI context and native DX12 objects
			auto* graphicsContext = dynamic_cast<NVRHIContext*>(app.GetWindow().GetCurrentContext());
			if (graphicsContext)
			{
				ID3D12Device* device = static_cast<ID3D12Device*>(graphicsContext->GetD3D12Device());
				ID3D12CommandQueue* commandQueue = static_cast<ID3D12CommandQueue*>(graphicsContext->GetD3D12CommandQueue());
				ID3D12DescriptorHeap* srvHeap = static_cast<ID3D12DescriptorHeap*>(graphicsContext->GetD3D12ImGuiSRVHeap());
				D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle = static_cast<D3D12_CPU_DESCRIPTOR_HANDLE*>(graphicsContext->GetD3D12ImGuiSRVHeapCPU());
				D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle = static_cast<D3D12_GPU_DESCRIPTOR_HANDLE*>(graphicsContext->GetD3D12ImGuiSRVHeapGPU());
				
				if (device && commandQueue && srvHeap && cpuHandle && gpuHandle)
				{
					// DX12 needs the number of frames in flight and descriptor heap for font
					ImGui_ImplDX12_Init(device, 2, DXGI_FORMAT_R8G8B8A8_UNORM, srvHeap, 
						*cpuHandle, *gpuHandle);
					WL_CORE_INFO("ImGui: Initialized DirectX 12 backend");
				}
				else
				{
					WL_CORE_ERROR("ImGui: Failed to get DX12 device/command queue/descriptor heap");
				}
			}
			break;
		}

		default:
			WL_CORE_ERROR("ImGui: Unsupported renderer API");
			break;
		}

		m_CurrentBackend = api;
	}

	void ImGuiLayer::ShutdownBackend()
	{
		WL_PROFILE_FUNCTION();

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

		default:
			break;
		}

		// Shutdown platform bindings
		ImGui_ImplGlfw_Shutdown();

		m_CurrentBackend = RendererAPI::API::None;
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

		WL_CORE_INFO("ImGui: Performing backend switch to {0}", (int)api);

		// Shutdown current backend
		ShutdownBackend();

		// Clear and rebuild font atlas for the new backend
		ImGuiIO& io = ImGui::GetIO();
		
		// Clear font textures but keep font data
		io.Fonts->ClearTexData();
		
		// Initialize new backend (this will create new font texture)
		InitBackend(api);
	}

	void ImGuiLayer::OnDetach()
	{
		WL_PROFILE_FUNCTION();

		ShutdownBackend();
		ImGui::DestroyContext();
	}

	void ImGuiLayer::OnEvent(Event& event)
	{
		if (m_BlockEvents)
		{
			ImGuiIO& io = ImGui::GetIO();
			event.Handled |= event.IsInCategory(EventCategoryMouse) & io.WantCaptureMouse;
			event.Handled |= event.IsInCategory(EventCategoryKeyboard) & io.WantCaptureKeyboard;
		}
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

		ImGuiIO& io = ImGui::GetIO();
		Application& app = Application::Get();
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
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
			break;

		case RendererAPI::API::NVRHI_DX12:
			// TODO: DX12 RenderDrawData requires native command list
			// For now, skip DX12 ImGui rendering
			WL_CORE_WARN("ImGui: DX12 rendering not yet fully implemented");
			break;

		default:
			WL_CORE_WARN("ImGui: Unknown backend {0}, using OpenGL", (int)m_CurrentBackend);
			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
			break;
		}

		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			GLFWwindow* backup_current_context = glfwGetCurrentContext();
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