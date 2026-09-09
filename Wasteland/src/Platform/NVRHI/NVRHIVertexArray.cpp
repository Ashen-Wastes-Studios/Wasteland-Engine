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

	// HLSL input semantics required by D3D11 input layouts. NVRHI passes the
	// attribute name straight through as SemanticName, so engine-side names
	// (a_Position, a_TexCoord, ...) must be translated to match the .hlsl
	// shaders' declared semantics (POSITION, TEXCOORD, ...) or
	// CreateInputLayout fails with E_INVALIDARG.
	static const char *ToHLSLSemantic(const std::string &name)
	{
		if (name == "a_Position")      return "POSITION";
		if (name == "a_Normal")        return "NORMAL";
		if (name == "a_Tangent")       return "TANGENT";
		if (name == "a_Bitangent")     return "BINORMAL";
		if (name == "a_TexCoord")      return "TEXCOORD";
		if (name == "a_Color")         return "COLOR";
		if (name == "a_TexIndex")      return "TEXINDEX";
		if (name == "a_TilingFactor")  return "TILINGFACTOR";
		if (name == "a_EntityID")      return "ENTITYID";
		if (name == "a_LocalPosition") return "LOCALPOSITION";
		if (name == "a_WorldPosition") return "POSITION";
		if (name == "a_Thickness")     return "THICKNESS";
		if (name == "a_Fade")          return "FADE";
		if (name == "a_Metallic")      return "METALLIC";
		if (name == "a_Roughness")     return "ROUGHNESS";
		if (name == "a_Displacement")  return "DISPLACEMENT";
		if (name == "a_BumpStrength")  return "BUMPSTRENGTH";
		// Fall back to the name itself (shader may use it verbatim)
		return name.c_str();
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
					// ToHLSLSemantic returns either a literal or element.Name's own
					// storage — both outlive the layout (VB owns the layout strings)
					desc.name = ToHLSLSemantic(element.Name);
					desc.format = ShaderDataTypeToNVRHIFormat(element.Type);
					desc.offset = element.Offset;
					desc.bufferIndex = bufferIndex;
					desc.elementStride = layout.GetStride();
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
