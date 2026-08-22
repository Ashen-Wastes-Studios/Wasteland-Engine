#include "wlpch.h"
#include "NVRHIRendererAPI.h"
#include "NVRHIContext.h"
#include "NVRHIShader.h"
#include "NVRHIVertexArray.h"
#include "NVRHIBuffer.h"
#include "NVRHITexture.h"
#include "Wasteland/Core/Log.h"
namespace Wasteland {

	NVRHIContext* NVRHIRendererAPI::s_CurrentContext = nullptr;

	NVRHIRendererAPI::NVRHIRendererAPI(RendererAPI::API api)
		: m_API(api), m_ClearColor(0.1f, 0.1f, 0.1f, 1.0f)
	{
	}

	NVRHIRendererAPI::~NVRHIRendererAPI()
	{
		m_PipelineCache.clear();
	}

	void NVRHIRendererAPI::Init()
	{
		WL_PROFILE_FUNCTION();
		
		// Context is set externally by the window system
		if (m_Context)
		{
			m_Context->Init();
		}
		
		std::string apiName;
		switch (m_API)
		{
		case RendererAPI::API::NVRHI_DX11: apiName = "DirectX 11"; break;
		case RendererAPI::API::NVRHI_DX12: apiName = "DirectX 12"; break;
		case RendererAPI::API::NVRHI_Vulkan: apiName = "Vulkan"; break;
		default: apiName = "Unknown"; break;
		}
		
		WL_CORE_INFO("NVRHI Renderer API initialized: {0}", apiName);
	}

	void NVRHIRendererAPI::SetContext(NVRHIContext* context)
	{
		m_Context = context;
		s_CurrentContext = context;
	}

	void NVRHIRendererAPI::SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
	{
		WL_PROFILE_FUNCTION();
		
		// Note: In NVRHI, viewport is set as part of GraphicsState before drawing
		// This is handled in the Renderer implementation
		// For now, this is a no-op
		WL_CORE_TRACE("SetViewport({0}, {1}, {2}, {3}) - will be set via GraphicsState", x, y, width, height);
	}

	void NVRHIRendererAPI::SetClearColor(const glm::vec4& color)
	{
		WL_PROFILE_FUNCTION();
		m_ClearColor = color;
	}

	void NVRHIRendererAPI::SetActiveTextures(const std::vector<Ref<Texture2D>>& textures)
	{
		WL_PROFILE_FUNCTION();
		m_ActiveTextures = textures;
		m_BindingSetCache.clear();
	}

	void NVRHIRendererAPI::Clear()
	{
		WL_PROFILE_FUNCTION();
		
		if (!m_Context)
		{
			WL_CORE_ERROR("Clear: No NVRHI context available");
			return;
		}

		nvrhi::ICommandList* cmd = m_Context->GetCommandList();
		if (!cmd)
		{
			WL_CORE_ERROR("Clear: No command list available");
			return;
		}

		nvrhi::TextureHandle backBuffer = m_Context->GetCurrentBackBuffer();
		if (!backBuffer)
		{
			WL_CORE_ERROR("Clear: No backbuffer available");
			return;
		}

		// Open command list, clear, and close
		cmd->open();
		cmd->clearTextureFloat(backBuffer, nvrhi::AllSubresources, 
			nvrhi::Color(m_ClearColor.r, m_ClearColor.g, m_ClearColor.b, m_ClearColor.a));
		cmd->close();
	}

	nvrhi::GraphicsPipelineHandle NVRHIRendererAPI::GetOrCreatePipeline(const Ref<Shader>& shader, const Ref<VertexArray>& vertexArray)
	{
		PipelineKey key;
		key.shaderPtr = shader.get();
		key.vertexArrayPtr = vertexArray.get();

		auto it = m_PipelineCache.find(key);
		if (it != m_PipelineCache.end())
		{
			return it->second;
		}

		// Create new pipeline
		nvrhi::IDevice* device = m_Context->GetDevice();
		if (!device)
		{
			WL_CORE_ERROR("GetOrCreatePipeline: No NVRHI device available");
			return nullptr;
		}

		auto nvrhiShader = std::dynamic_pointer_cast<NVRHIShader>(shader);
		auto nvrhiVA = std::dynamic_pointer_cast<NVRHIVertexArray>(vertexArray);

		if (!nvrhiShader || !nvrhiVA)
		{
			WL_CORE_ERROR("GetOrCreatePipeline: Invalid shader or vertex array");
			return nullptr;
		}

		// Create input layout
		uint32_t inputLayoutCount = 0;
		nvrhi::VertexAttributeDesc* inputLayoutDescs = nvrhiVA->GetInputLayout(inputLayoutCount);
		nvrhi::InputLayoutHandle inputLayout;
		if (inputLayoutDescs && inputLayoutCount > 0)
		{
			inputLayout = device->createInputLayout(inputLayoutDescs, inputLayoutCount, nvrhiShader->GetVertexShader());
		}

		// Create graphics pipeline
		nvrhi::GraphicsPipelineDesc pipelineDesc;
		pipelineDesc.VS = nvrhiShader->GetVertexShader();
		pipelineDesc.PS = nvrhiShader->GetPixelShader();
		pipelineDesc.inputLayout = inputLayout;

		// Render state
		pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Back;
		pipelineDesc.renderState.rasterState.fillMode = nvrhi::RasterFillMode::Solid;

		// Blend state
		pipelineDesc.renderState.blendState.targets[0].blendEnable = true;
		pipelineDesc.renderState.blendState.targets[0].srcBlend = nvrhi::BlendFactor::SrcAlpha;
		pipelineDesc.renderState.blendState.targets[0].destBlend = nvrhi::BlendFactor::InvSrcAlpha;
		pipelineDesc.renderState.blendState.targets[0].blendOp = nvrhi::BlendOp::Add;
		pipelineDesc.renderState.blendState.targets[0].srcBlendAlpha = nvrhi::BlendFactor::One;
		pipelineDesc.renderState.blendState.targets[0].destBlendAlpha = nvrhi::BlendFactor::InvSrcAlpha;
		pipelineDesc.renderState.blendState.targets[0].blendOpAlpha = nvrhi::BlendOp::Add;
		pipelineDesc.renderState.blendState.targets[0].colorWriteMask = nvrhi::ColorMask::All;

		// Framebuffer info from context
		nvrhi::FramebufferHandle fb = m_Context->GetCurrentFramebuffer();
		if (!fb)
		{
			WL_CORE_ERROR("GetOrCreatePipeline: No framebuffer available");
			return nullptr;
		}

		auto pipeline = device->createGraphicsPipeline(pipelineDesc, fb);
		if (!pipeline)
		{
			WL_CORE_ERROR("GetOrCreatePipeline: Failed to create graphics pipeline");
			return nullptr;
		}

		m_PipelineCache[key] = pipeline;
		return pipeline;
	}

	nvrhi::BindingSetHandle NVRHIRendererAPI::GetOrCreateBindingSet(const Ref<Shader>& shader)
	{
		auto nvrhiShader = std::dynamic_pointer_cast<NVRHIShader>(shader);
		if (!nvrhiShader)
			return nullptr;

		const void* shaderPtr = shader.get();
		auto it = m_BindingSetCache.find(shaderPtr);
		if (it != m_BindingSetCache.end())
		{
			return it->second;
		}

		nvrhi::IDevice* device = m_Context->GetDevice();
		if (!device)
			return nullptr;

		nvrhi::BindingLayoutHandle layout = nvrhiShader->GetBindingLayout();
		if (!layout)
			return nullptr;

		nvrhi::BindingSetDesc setDesc;

		// Bind vertex shader constant buffer
		nvrhi::BufferHandle vsCB = nvrhiShader->GetVSConstantBuffer();
		if (vsCB)
		{
			setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, vsCB));
		}

		// Bind pixel shader constant buffer
		nvrhi::BufferHandle psCB = nvrhiShader->GetPSConstantBuffer();
		if (psCB)
		{
			setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(1, psCB));
		}

		// Bind active textures (e.g. for Renderer2D texture slots)
		for (size_t i = 0; i < m_ActiveTextures.size(); i++)
		{
			if (m_ActiveTextures[i])
			{
				auto nvrhiTex = std::dynamic_pointer_cast<NVRHITexture2D>(m_ActiveTextures[i]);
				if (nvrhiTex && nvrhiTex->GetTexture())
				{
					setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, nvrhiTex->GetTexture()).setArrayElement(static_cast<uint32_t>(i)));
					if (i == 0 && nvrhiTex->GetSampler())
					{
						setDesc.addItem(nvrhi::BindingSetItem::Sampler(0, nvrhiTex->GetSampler()));
					}
				}
			}
		}

		// Create binding set
		nvrhi::BindingSetHandle bindingSet = device->createBindingSet(setDesc, layout);
		m_BindingSetCache[shaderPtr] = bindingSet;
		return bindingSet;
	}

	void NVRHIRendererAPI::DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t indexCount)
	{
		WL_PROFILE_FUNCTION();

		if (!m_Context || !m_CurrentShader || !vertexArray)
		{
			WL_CORE_ERROR("DrawIndexed: Missing context, shader, or vertex array");
			return;
		}

		nvrhi::ICommandList* cmd = m_Context->GetCommandList();
		if (!cmd)
		{
			WL_CORE_ERROR("DrawIndexed: No command list available");
			return;
		}

		// Get or create pipeline
		auto pipeline = GetOrCreatePipeline(m_CurrentShader, vertexArray);
		if (!pipeline)
		{
			WL_CORE_ERROR("DrawIndexed: Failed to get/create pipeline");
			return;
		}

		// Get or create binding set
		auto bindingSet = GetOrCreateBindingSet(m_CurrentShader);

		auto nvrhiVA = std::dynamic_pointer_cast<NVRHIVertexArray>(vertexArray);
		if (!nvrhiVA)
		{
			WL_CORE_ERROR("DrawIndexed: Invalid vertex array");
			return;
		}

		// Get index buffer to determine count
		auto indexBuffer = nvrhiVA->GetIndexBuffer();
		if (!indexBuffer)
		{
			WL_CORE_ERROR("DrawIndexed: No index buffer");
			return;
		}

		auto nvrhiIB = std::dynamic_pointer_cast<NVRHIIndexBuffer>(indexBuffer);
		if (!nvrhiIB)
		{
			WL_CORE_ERROR("DrawIndexed: Invalid index buffer");
			return;
		}

		// Set up graphics state
		nvrhi::GraphicsState state;
		state.pipeline = pipeline;
		state.framebuffer = m_Context->GetCurrentFramebuffer();
		
		// Add binding set if available
		if (bindingSet)
		{
			state.bindings.push_back(bindingSet);
		}

		// Bind vertex buffers
		const auto& vertexBuffers = nvrhiVA->GetVertexBuffers();
		for (size_t i = 0; i < vertexBuffers.size(); i++)
		{
			auto nvrhiVB = std::dynamic_pointer_cast<NVRHIVertexBuffer>(vertexBuffers[i]);
			if (nvrhiVB)
			{
				state.vertexBuffers.push_back(nvrhi::VertexBufferBinding()
					.setBuffer(nvrhiVB->GetBuffer())
					.setOffset(0));
			}
		}

		// Bind index buffer
		state.indexBuffer = nvrhi::IndexBufferBinding()
			.setBuffer(nvrhiIB->GetBuffer())
			.setOffset(0)
			.setFormat(nvrhi::Format::R32_UINT);

		// Set viewport
		state.viewport.addViewport(nvrhi::Viewport(static_cast<float>(m_Context->GetWidth()), static_cast<float>(m_Context->GetHeight())));
		state.viewport.addScissorRect(nvrhi::Rect(m_Context->GetWidth(), m_Context->GetHeight()));

		// Open command list if not already open
		cmd->open();
		cmd->setGraphicsState(state);
		
		uint32_t actualIndexCount = (indexCount > 0) ? indexCount : nvrhiIB->GetCount();
		cmd->drawIndexed(nvrhi::DrawArguments().setVertexCount(actualIndexCount));
		cmd->close();
	}

	void NVRHIRendererAPI::DrawLines(const Ref<VertexArray>& vertexArray, uint32_t vertexCount)
	{
		WL_PROFILE_FUNCTION();
		
		// TODO: Implement NVRHI line drawing
		// Similar to DrawIndexed but with primitive topology = LineList
		WL_CORE_WARN("NVRHIRendererAPI::DrawLines not yet implemented");
	}

	void NVRHIRendererAPI::SetLineWidth(float width)
	{
		WL_PROFILE_FUNCTION();

		// Note: DirectX does not support variable line width
		// This is a no-op for DX11/DX12
		WL_CORE_TRACE("SetLineWidth({0}) - not supported on DirectX", width);
	}

	void NVRHIRendererAPI::DispatchCompute(const Ref<Shader>& shader, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
	{
		WL_PROFILE_FUNCTION();

		if (!m_Context || !shader)
		{
			WL_CORE_ERROR("DispatchCompute: Missing context or shader");
			return;
		}

		nvrhi::ICommandList* cmd = m_Context->GetCommandList();
		if (!cmd)
		{
			WL_CORE_ERROR("DispatchCompute: No command list available");
			return;
		}

		auto nvrhiShader = std::dynamic_pointer_cast<NVRHIShader>(shader);
		if (!nvrhiShader || !nvrhiShader->GetComputeShader())
		{
			WL_CORE_ERROR("DispatchCompute: Invalid shader or missing compute shader");
			return;
		}

		nvrhi::IDevice* device = m_Context->GetDevice();
		if (!device)
			return;

		const void* shaderPtr = shader.get();
		auto it = m_ComputePipelineCache.find(shaderPtr);
		nvrhi::ComputePipelineHandle pipeline;
		if (it != m_ComputePipelineCache.end())
		{
			pipeline = it->second;
		}
		else
		{
			nvrhi::ComputePipelineDesc desc;
			desc.CS = nvrhiShader->GetComputeShader();
			if (nvrhiShader->GetBindingLayout())
			{
				desc.addBindingLayout(nvrhiShader->GetBindingLayout());
			}
			pipeline = device->createComputePipeline(desc);
			if (pipeline)
			{
				m_ComputePipelineCache[shaderPtr] = pipeline;
			}
		}

		if (!pipeline)
		{
			WL_CORE_ERROR("DispatchCompute: Failed to create compute pipeline");
			return;
		}

		auto bindingSet = GetOrCreateBindingSet(shader);

		nvrhi::ComputeState state;
		state.pipeline = pipeline;
		if (bindingSet)
		{
			state.bindings.push_back(bindingSet);
		}

		cmd->open();
		cmd->setComputeState(state);
		cmd->dispatch(groupCountX, groupCountY, groupCountZ);
		cmd->close();
	}

	void NVRHIRendererAPI::ClearCachedResources()
	{
		WL_PROFILE_FUNCTION();

		WL_CORE_INFO("Clearing cached NVRHI resources");

		// Clear pipeline caches
		m_PipelineCache.clear();
		m_ComputePipelineCache.clear();

		// Clear binding set cache
		m_BindingSetCache.clear();

		// Reset current shader
		m_CurrentShader = nullptr;

		WL_CORE_INFO("Cleared {0} pipelines, {1} compute pipelines, and {2} binding sets",
			m_PipelineCache.size(), m_ComputePipelineCache.size(), m_BindingSetCache.size());
	}

}
