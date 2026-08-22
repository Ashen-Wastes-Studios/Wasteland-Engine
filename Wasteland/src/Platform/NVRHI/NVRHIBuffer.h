#pragma once

#include "Wasteland/Renderer/Buffer.h"
#include <nvrhi/nvrhi.h>

namespace Wasteland {

	class NVRHIVertexBuffer : public VertexBuffer
	{
	public:
		NVRHIVertexBuffer(uint32_t size);
		NVRHIVertexBuffer(float* vertices, uint32_t size);
		virtual ~NVRHIVertexBuffer();

		virtual void Bind() const override;
		virtual void Unbind() const override;

		virtual void SetData(const void* data, uint32_t size) override;

		virtual const BufferLayout& GetLayout() const override { return m_Layout; }
		virtual void SetLayout(const BufferLayout& layout) override { m_Layout = layout; }

		// NVRHI-specific accessor
		nvrhi::BufferHandle GetBuffer() const { return m_Buffer; }

	private:
		nvrhi::BufferHandle m_Buffer;
		BufferLayout m_Layout;
		uint32_t m_Size;
	};

	class NVRHIIndexBuffer : public IndexBuffer
	{
	public:
		NVRHIIndexBuffer(uint32_t* indices, uint32_t count);
		virtual ~NVRHIIndexBuffer();

		virtual void Bind() const override;
		virtual void Unbind() const override;

		virtual void SetData(const void* data, uint32_t size) override;
		virtual uint32_t GetCount() const { return m_Count; }

		// NVRHI-specific accessor
		nvrhi::BufferHandle GetBuffer() const { return m_Buffer; }

	private:
		nvrhi::BufferHandle m_Buffer;
		uint32_t m_Count;
	};

	class NVRHIStructuredBuffer
	{
	public:
		NVRHIStructuredBuffer(uint32_t size, uint32_t stride);
		virtual ~NVRHIStructuredBuffer();

		void SetData(const void* data, uint32_t size);
		nvrhi::BufferHandle GetBuffer() const { return m_Buffer; }

	private:
		nvrhi::BufferHandle m_Buffer;
		uint32_t m_Size;
		uint32_t m_Stride;
	};

}
