#include "wlpch.h"
#include "NVRHIBuffer.h"
#include "NVRHIContext.h"
#include "NVRHIRendererAPI.h"
#include "Wasteland/Core/Log.h"

namespace Wasteland {

	// Helper to get the current NVRHI device
	static nvrhi::IDevice* GetDevice()
	{
		auto* context = NVRHIRendererAPI::GetCurrentContext();
		return context ? context->GetDevice() : nullptr;
	}

	/////////////////////////////////////////////////////////////////////////////////////////////
	// NVRHIVertexBuffer
	/////////////////////////////////////////////////////////////////////////////////////////////

	NVRHIVertexBuffer::NVRHIVertexBuffer(uint32_t size)
		: m_Size(size)
	{
		nvrhi::IDevice* device = GetDevice();
		if (!device)
		{
			WL_CORE_ERROR("NVRHIVertexBuffer: No NVRHI device available");
			return;
		}

		nvrhi::BufferDesc desc;
		desc.byteSize = size;
		desc.isVertexBuffer = true;
		desc.canHaveRawViews = true;
		desc.cpuAccess = nvrhi::CpuAccessMode::Write;
		desc.debugName = "VertexBuffer";

		m_Buffer = device->createBuffer(desc);
		if (!m_Buffer)
		{
			WL_CORE_ERROR("NVRHIVertexBuffer: Failed to create buffer");
		}
	}

	NVRHIVertexBuffer::NVRHIVertexBuffer(float* vertices, uint32_t size)
		: m_Size(size)
	{
		nvrhi::IDevice* device = GetDevice();
		if (!device)
		{
			WL_CORE_ERROR("NVRHIVertexBuffer: No NVRHI device available");
			return;
		}

		nvrhi::BufferDesc desc;
		desc.byteSize = size;
		desc.isVertexBuffer = true;
		desc.canHaveRawViews = true;
		desc.cpuAccess = nvrhi::CpuAccessMode::Write;
		desc.debugName = "VertexBuffer";

		m_Buffer = device->createBuffer(desc);
		if (!m_Buffer)
		{
			WL_CORE_ERROR("NVRHIVertexBuffer: Failed to create buffer");
			return;
		}

		// Upload initial data
		if (vertices && size > 0)
		{
			SetData(vertices, size);
		}
	}

	NVRHIVertexBuffer::~NVRHIVertexBuffer()
	{
		m_Buffer = nullptr;
	}

	void NVRHIVertexBuffer::Bind() const
	{
		// Note: In NVRHI, vertex buffers are bound via GraphicsState, not directly
		// This is handled in the draw call implementation
		// For now, this is a no-op
	}

	void NVRHIVertexBuffer::Unbind() const
	{
		// No-op in NVRHI
	}

	void NVRHIVertexBuffer::SetData(const void* data, uint32_t size)
	{
		if (!m_Buffer || !data || size == 0)
			return;

		auto* context = NVRHIRendererAPI::GetCurrentContext();
		if (!context)
		{
			WL_CORE_ERROR("NVRHIVertexBuffer::SetData: No NVRHI context available");
			return;
		}

		nvrhi::ICommandList* cmd = context->GetCommandList();
		if (!cmd)
		{
			WL_CORE_ERROR("NVRHIVertexBuffer::SetData: No command list available");
			return;
		}

		cmd->writeBuffer(m_Buffer, data, size);
	}

	/////////////////////////////////////////////////////////////////////////////////////////////
	// NVRHIIndexBuffer
	/////////////////////////////////////////////////////////////////////////////////////////////

	NVRHIIndexBuffer::NVRHIIndexBuffer(uint32_t* indices, uint32_t count)
		: m_Count(count)
	{
		nvrhi::IDevice* device = GetDevice();
		if (!device)
		{
			WL_CORE_ERROR("NVRHIIndexBuffer: No NVRHI device available");
			return;
		}

		uint32_t size = count * sizeof(uint32_t);

		nvrhi::BufferDesc desc;
		desc.byteSize = size;
		desc.isIndexBuffer = true;
		desc.canHaveRawViews = true;
		desc.cpuAccess = nvrhi::CpuAccessMode::Write;
		desc.debugName = "IndexBuffer";

		m_Buffer = device->createBuffer(desc);
		if (!m_Buffer)
		{
			WL_CORE_ERROR("NVRHIIndexBuffer: Failed to create buffer");
			return;
		}

		// Upload initial data
		if (indices && count > 0)
		{
			SetData(indices, size);
		}
	}

	NVRHIIndexBuffer::~NVRHIIndexBuffer()
	{
		m_Buffer = nullptr;
	}

	void NVRHIIndexBuffer::Bind() const
	{
		// Note: In NVRHI, index buffers are bound via GraphicsState, not directly
		// This is handled in the draw call implementation
		// For now, this is a no-op
	}

	void NVRHIIndexBuffer::Unbind() const
	{
		// No-op in NVRHI
	}

	void NVRHIIndexBuffer::SetData(const void* data, uint32_t size)
	{
		if (!m_Buffer || !data || size == 0)
			return;

		auto* context = NVRHIRendererAPI::GetCurrentContext();
		if (!context)
		{
			WL_CORE_ERROR("NVRHIIndexBuffer::SetData: No NVRHI context available");
			return;
		}

		nvrhi::ICommandList* cmd = context->GetCommandList();
		if (!cmd)
		{
			WL_CORE_ERROR("NVRHIIndexBuffer::SetData: No command list available");
			return;
		}

		cmd->writeBuffer(m_Buffer, data, size);
	}

}
