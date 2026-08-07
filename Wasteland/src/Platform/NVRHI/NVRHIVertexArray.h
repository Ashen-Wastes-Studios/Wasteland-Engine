#pragma once

#include "Wasteland/Renderer/VertexArray.h"
#include <nvrhi/nvrhi.h>

namespace Wasteland {

	class NVRHIVertexArray : public VertexArray
	{
	public:
		NVRHIVertexArray();
		virtual ~NVRHIVertexArray();

		virtual void Bind() const override;
		virtual void Unbind() const override;

		virtual void AddVertexBuffer(const Ref<VertexBuffer>& vertexBuffer) override;
		virtual void SetIndexBuffer(const Ref<IndexBuffer>& indexBuffer) override;

		virtual const std::vector<Ref<VertexBuffer>>& GetVertexBuffers() const override { return m_VertexBuffers; }
		virtual const Ref<IndexBuffer>& GetIndexBuffer() const override { return m_IndexBuffer; }

		// NVRHI-specific: create input layout from vertex buffer layouts
		nvrhi::VertexAttributeDesc* GetInputLayout(uint32_t& count) const;

	private:
		std::vector<Ref<VertexBuffer>> m_VertexBuffers;
		Ref<IndexBuffer> m_IndexBuffer;
		
		// Cached input layout (mutable to allow lazy generation in const method)
		mutable std::vector<nvrhi::VertexAttributeDesc> m_InputLayout;
		mutable bool m_InputLayoutDirty = true;
	};

}
