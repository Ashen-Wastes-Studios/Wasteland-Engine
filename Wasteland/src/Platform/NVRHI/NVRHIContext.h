#pragma once

#include "Wasteland/Renderer/GraphicsContext.h"
#include "Wasteland/Renderer/RendererAPI.h"

#include <nvrhi/nvrhi.h>
#include <vector>

struct GLFWwindow;

namespace Wasteland {

	class NVRHIContext : public GraphicsContext
	{
	public:
		NVRHIContext(GLFWwindow* windowHandle, RendererAPI::API api);
		virtual ~NVRHIContext();

		virtual void Init() override;
		virtual void SwapBuffers() override;

		// NVRHI-specific accessors
		nvrhi::IDevice* GetDevice() const { return m_Device; }
		nvrhi::ICommandList* GetCommandList() const { return m_CommandList; }
		nvrhi::FramebufferHandle GetCurrentFramebuffer() const;
		nvrhi::TextureHandle GetCurrentBackBuffer() const;
		uint32_t GetCurrentBackBufferIndex() const { return m_CurrentBackBufferIndex; }
		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }

		// Native DirectX object accessors (for ImGui and other integrations)
		void* GetD3D11Device() const { return m_D3D11Device; }
		void* GetD3D11Context() const { return m_D3D11Context; }
		void* GetD3D12Device() const { return m_D3D12Device; }
		void* GetD3D12CommandQueue() const { return m_D3D12CommandQueue; }
		
		// DX12 ImGui descriptor heap
		void* GetD3D12ImGuiSRVHeap() const { return m_D3D12ImGuiSRVHeap; }
		void* GetD3D12ImGuiSRVHeapCPU() const { return m_D3D12ImGuiSRVHeapCPU; }
		void* GetD3D12ImGuiSRVHeapGPU() const { return m_D3D12ImGuiSRVHeapGPU; }

		// Get the renderer API type
		RendererAPI::API GetAPI() const { return m_API; }

		void Resize(uint32_t width, uint32_t height);

	private:
		void CreateDeviceDX11();
		void CreateDeviceDX12();
		void CreateSwapChain();
		void CreateBackBufferFramebuffers();

		GLFWwindow* m_WindowHandle;
		RendererAPI::API m_API;

		// NVRHI objects
		nvrhi::DeviceHandle m_Device;
		nvrhi::CommandListHandle m_CommandList;
		
		// Backbuffer management
		std::vector<nvrhi::TextureHandle> m_BackBuffers;
		std::vector<nvrhi::FramebufferHandle> m_BackBufferFramebuffers;
		uint32_t m_CurrentBackBufferIndex = 0;

		// DX11 native objects (when using DX11)
		void* m_D3D11Device = nullptr;
		void* m_D3D11Context = nullptr;
		void* m_DXGISwapChain = nullptr;

		// DX12 native objects (when using DX12)
		void* m_D3D12Device = nullptr;
		void* m_D3D12CommandQueue = nullptr;
		void* m_DXGISwapChain3 = nullptr;
		
		// DX12 ImGui SRV descriptor heap
		void* m_D3D12ImGuiSRVHeap = nullptr;
		void* m_D3D12ImGuiSRVHeapCPU = nullptr;
		void* m_D3D12ImGuiSRVHeapGPU = nullptr;

		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
	};

}
