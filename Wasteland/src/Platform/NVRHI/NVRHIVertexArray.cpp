#include "wlpch.h"
#include "NVRHIVertexArray.h"
#include "NVRHIBuffer.h"
#include "Wasteland/Core/Log.h"

namespace Wasteland {

	// Helper to convert ShaderDataType to NVRHI Format
	static nvrhi::Format ShaderDataTypeToNVRHIFormat(ShaderDataType type)
	{
		switch (type)
		{
			case ShaderDataType::Float:  return nvrhi::Format::R32_FLOAT;
			case ShaderDataType::Float2: return nvrhi::Format::RG32_FLOAT;
			case ShaderDataType::Float3: return nvrhi::Format::RGB32_FLOAT;
			case ShaderDataType::Float4: return nvrhi::Format::RGBA32_FLOAT;
			case ShaderDataType::Int:    return nvrhi::Format::R32_SINT;
			case ShaderDataType::Int2:   return nvrhi::Format::RG32_SINT;
			case ShaderDataType::Int3:   return nvrhi::Format::RGB32_SINT;
			case ShaderDataType::Int4:   return nvrhi::Format::RGBA32_SINT;
			case ShaderDataType::Bool:   return nvrhi::Format::R8_UINT;
			default:
				WL_CORE_ASSERT(false, "Unknown ShaderDataType!");
				return nvrhi::Format::UNKNOWN;
		}
	}

	NVRHIVertexArray::NVRHIVertexArray()
	{
	}

	NVRHIVertexArray::~NVRHIVertexArray()
	{
	}

	void NVRHIVertexArray::Bind() const
	{
		// Note: In NVRHI, vertex buffers are bound via GraphicsState
		// This is handled in the Renderer implementation
		// For now, this is a no-op
	}

	void NVRHIVertexArray::Unbind() const
	{
		// No-op in NVRHI
	}

	void NVRHIVertexArray::AddVertexBuffer(const Ref<VertexBuffer>& vertexBuffer)
	{
		WL_CORE_ASSERT(vertexBuffer->GetLayout().GetElements().size() > 0, 
			"Vertex buffer has no layout!");
		
		m_VertexBuffers.push_back(vertexBuffer);
		m_InputLayoutDirty = true;
	}

	void NVRHIVertexArray::SetIndexBuffer(const Ref<IndexBuffer>& indexBuffer)
	{
		m_IndexBuffer = indexBuffer;
	}

	nvrhi::VertexAttributeDesc* NVRHIVertexArray::GetInputLayout(uint32_t& count) const
	{
		if (m_InputLayoutDirty)
		{
			m_InputLayout.clear();
			uint32_t bufferIndex = 0;
			
			for (const auto& vb : m_VertexBuffers)
			{
				const auto& layout = vb->GetLayout();
				uint32_t offset = 0;
				
				for (const auto& element : layout)
				{
					nvrhi::VertexAttributeDesc desc;
					desc.name = element.Name.c_str();
					desc.format = ShaderDataTypeToNVRHIFormat(element.Type);
					desc.offset = element.Offset;
					desc.bufferIndex = bufferIndex;
					desc.arraySize = 1;
					
					m_InputLayout.push_back(desc);
					offset += element.Size;
				}
				
				bufferIndex++;
			}
			
			m_InputLayoutDirty = false;
		}
		
		count = static_cast<uint32_t>(m_InputLayout.size());
		return m_InputLayout.data();
	}

}
