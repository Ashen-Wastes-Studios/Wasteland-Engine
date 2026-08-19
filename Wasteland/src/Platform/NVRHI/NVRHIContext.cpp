#include "wlpch.h"
#include "NVRHIContext.h"

#include "Wasteland/Core/Log.h"

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
#include <nvrhi/utils.h>

namespace Wasteland {

	NVRHIContext::NVRHIContext(GLFWwindow* windowHandle, RendererAPI::API api)
		: m_WindowHandle(windowHandle), m_API(api)
	{
	}

	NVRHIContext::~NVRHIContext()
	{
		WL_PROFILE_FUNCTION();

		// Release NVRHI objects
		m_CommandList = nullptr;
		m_Device = nullptr;

		// Release native DX objects
		if (m_API == RendererAPI::API::NVRHI_DX11)
		{
			if (m_DXGISwapChain) static_cast<IDXGISwapChain*>(m_DXGISwapChain)->Release();
			if (m_D3D11Context) static_cast<ID3D11DeviceContext*>(m_D3D11Context)->Release();
			if (m_D3D11Device) static_cast<ID3D11Device*>(m_D3D11Device)->Release();
		}
		else if (m_API == RendererAPI::API::NVRHI_DX12)
		{
			if (m_DXGISwapChain3) static_cast<IDXGISwapChain3*>(m_DXGISwapChain3)->Release();
			if (m_D3D12CommandQueue) static_cast<ID3D12CommandQueue*>(m_D3D12CommandQueue)->Release();
			if (m_D3D12Device) static_cast<ID3D12Device*>(m_D3D12Device)->Release();
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
		default:
			WL_CORE_ASSERT(false, "Unsupported NVRHI API");
			break;
		}

		CreateSwapChain();

		// Create command list
		m_CommandList = m_Device->createCommandList();

		WL_CORE_INFO("NVRHI Context initialized successfully");
	}

	void NVRHIContext::CreateDeviceDX11()
	{
		WL_PROFILE_FUNCTION();

		// Create D3D11 device
		ID3D11Device* device = nullptr;
		ID3D11DeviceContext* context = nullptr;

		UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef WL_DEBUG
		creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

		D3D_FEATURE_LEVEL featureLevels[] = {
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0
		};

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
			&context
		);

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
				&context
			);
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
		ID3D12Device* device = nullptr;
		ID3D12CommandQueue* commandQueue = nullptr;

		UINT creationFlags = 0;
#ifdef WL_DEBUG
		creationFlags |= D3D12_CREATE_DEVICE_DEBUG;
#endif

		HRESULT hr = D3D12CreateDevice(
			nullptr,
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&device)
		);

		if (FAILED(hr))
		{
			// Try without debug layer
			hr = D3D12CreateDevice(
				nullptr,
				D3D_FEATURE_LEVEL_11_0,
				IID_PPV_ARGS(&device)
			);
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
		heapDesc.NumDescriptors = 1; // Only need 1 for font texture
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		heapDesc.NodeMask = 0;
		
		ID3D12DescriptorHeap* srvHeap = nullptr;
		hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap));
		if (SUCCEEDED(hr) && srvHeap)
		{
			m_D3D12ImGuiSRVHeap = srvHeap;
			
			// Get CPU and GPU handles
			D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();
			D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = srvHeap->GetGPUDescriptorHandleForHeapStart();
			
			// Store handles (we'll use void* to avoid exposing D3D12 types in header)
			m_D3D12ImGuiSRVHeapCPU = new D3D12_CPU_DESCRIPTOR_HANDLE(cpuHandle);
			m_D3D12ImGuiSRVHeapGPU = new D3D12_GPU_DESCRIPTOR_HANDLE(gpuHandle);
			
			WL_CORE_INFO("D3D12 ImGui SRV descriptor heap created");
		}
		else
		{
			WL_CORE_ERROR("Failed to create D3D12 ImGui SRV descriptor heap");
		}

		WL_CORE_INFO("D3D12 device created successfully");
	}

	void NVRHIContext::CreateSwapChain()
	{
		WL_PROFILE_FUNCTION();

		HWND hwnd = glfwGetWin32Window(m_WindowHandle);
		if (!hwnd)
		{
			WL_CORE_ERROR("CreateSwapChain: Failed to get HWND from GLFW window");
			return;
		}

		// Create DXGI factory
		IDXGIFactory4* factory = nullptr;
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

		IDXGISwapChain1* swapChain1 = nullptr;
		
		if (m_API == RendererAPI::API::NVRHI_DX12)
		{
			hr = factory->CreateSwapChainForHwnd(
				static_cast<IUnknown*>(m_D3D12CommandQueue),
				hwnd,
				&swapChainDesc,
				nullptr,
				nullptr,
				&swapChain1
			);
		}
		else if (m_API == RendererAPI::API::NVRHI_DX11)
		{
			hr = factory->CreateSwapChainForHwnd(
				static_cast<IUnknown*>(m_D3D11Device),  // Pass device, not context
				hwnd,
				&swapChainDesc,
				nullptr,
				nullptr,
				&swapChain1
			);
		}
		
		if (FAILED(hr) || !swapChain1)
		{
			WL_CORE_ERROR("CreateSwapChain: Failed to create swap chain (HRESULT: {0})", hr);
			factory->Release();
			return;
		}

		if (m_API == RendererAPI::API::NVRHI_DX12)
		{
			IDXGISwapChain3* swapChain3 = nullptr;
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
			WL_CORE_INFO("Created DX12 swap chain");
		}
		else if (m_API == RendererAPI::API::NVRHI_DX11)
		{
			m_DXGISwapChain = swapChain1;
			WL_CORE_INFO("Created DX11 swap chain");
		}

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

		// Check if swap chain exists
		if (m_API == RendererAPI::API::NVRHI_DX11 && !m_DXGISwapChain)
		{
			WL_CORE_ERROR("CreateBackBufferFramebuffers: DX11 swap chain is null");
			return;
		}
		else if (m_API == RendererAPI::API::NVRHI_DX12 && !m_DXGISwapChain3)
		{
			WL_CORE_ERROR("CreateBackBufferFramebuffers: DX12 swap chain is null");
			return;
		}

		// Get backbuffers from swapchain
		const uint32_t bufferCount = 2;
		m_BackBuffers.resize(bufferCount);
		m_BackBufferFramebuffers.resize(bufferCount);

		if (m_API == RendererAPI::API::NVRHI_DX11)
		{
			IDXGISwapChain* swapChain = static_cast<IDXGISwapChain*>(m_DXGISwapChain);
			
			// Get all backbuffers first
			std::vector<ID3D11Texture2D*> nativeBuffers(bufferCount, nullptr);
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
		}
		else if (m_API == RendererAPI::API::NVRHI_DX12)
		{
			IDXGISwapChain3* swapChain = static_cast<IDXGISwapChain3*>(m_DXGISwapChain3);
			
			// Get all backbuffers first
			std::vector<ID3D12Resource*> nativeBuffers(bufferCount, nullptr);
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
		}

		WL_CORE_INFO("Created {0} backbuffer framebuffers", bufferCount);
	}

	nvrhi::FramebufferHandle NVRHIContext::GetCurrentFramebuffer() const
	{
		if (m_BackBufferFramebuffers.empty())
			return nullptr;
		return m_BackBufferFramebuffers[m_CurrentBackBufferIndex];
	}

	nvrhi::TextureHandle NVRHIContext::GetCurrentBackBuffer() const
	{
		if (m_BackBuffers.empty())
			return nullptr;
		return m_BackBuffers[m_CurrentBackBufferIndex];
	}

	void NVRHIContext::SwapBuffers()
	{
		WL_PROFILE_FUNCTION();

		if (m_API == RendererAPI::API::NVRHI_DX11)
		{
			static_cast<IDXGISwapChain*>(m_DXGISwapChain)->Present(1, 0);
		}
		else if (m_API == RendererAPI::API::NVRHI_DX12)
		{
			IDXGISwapChain3* swapChain = static_cast<IDXGISwapChain3*>(m_DXGISwapChain3);
			swapChain->Present(1, 0);
			m_CurrentBackBufferIndex = swapChain->GetCurrentBackBufferIndex();
		}
	}

	void NVRHIContext::Resize(uint32_t width, uint32_t height)
	{
		WL_PROFILE_FUNCTION();

		if (width == m_Width && height == m_Height)
			return;

		m_Width = width;
		m_Height = height;

		// Release old backbuffers
		m_BackBuffers.clear();
		m_BackBufferFramebuffers.clear();

		// Resize swap chain
		if (m_API == RendererAPI::API::NVRHI_DX11)
		{
			static_cast<IDXGISwapChain*>(m_DXGISwapChain)->ResizeBuffers(
				2, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
		}
		else if (m_API == RendererAPI::API::NVRHI_DX12)
		{
			static_cast<IDXGISwapChain3*>(m_DXGISwapChain3)->ResizeBuffers(
				2, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
		}

		// Recreate backbuffer framebuffers
		CreateBackBufferFramebuffers();
	}

}
