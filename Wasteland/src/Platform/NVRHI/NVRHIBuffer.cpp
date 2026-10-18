#include "wlpch.h"
#include "NVRHIBuffer.h"
#include "NVRHIContext.h"
#include "NVRHIRendererAPI.h"
#include "Wasteland/Core/Log.h"

namespace Wasteland {

	// Helper to get the current NVRHI device
	// Wraps the two-step lookup (renderer API -> context -> device) every
	// creation path below needs. Returns null instead of asserting, and all
	// four call sites do test it -- which makes the one site that does not
	// stand out.
	static nvrhi::IDevice* GetDevice()
	{
		// The CURRENT context, never a cached device: an API switch replaces the
		// context, so a stored pointer would dangle. Re-fetched in every
		// SetData below for the same reason.
		auto* context = NVRHIRendererAPI::GetCurrentContext();
		return context ? context->GetDevice() : nullptr;
	}

	/////////////////////////////////////////////////////////////////////////////////////////////
	// NVRHIVertexBuffer
	/////////////////////////////////////////////////////////////////////////////////////////////

	// Size-only constructor: allocates the GPU buffer, uploads nothing.
	// The contents are undefined until SetData, and the caller is expected
	// to do that.
	NVRHIVertexBuffer::NVRHIVertexBuffer(uint32_t size)
		// m_Size again takes BYTES: the same number is used for desc.byteSize and
		// is forwarded to SetData below. A vertex COUNT here would size the
		// buffer four times too small and silently truncate the upload.
		// m_Size is set from the ctor argument, which is a BYTE count -- it is
		// forwarded verbatim to desc.byteSize below and to writeBuffer in SetData.
		// The parameter is named `size` rather than `byteSize`, so a caller
		// passing an element count would allocate four times too small.
		: m_Size(size)
	{
		nvrhi::IDevice* device = GetDevice();
		if (!device)
		{
			WL_CORE_ERROR("NVRHIVertexBuffer: No NVRHI device available");
			return;
		}

		nvrhi::BufferDesc desc;
		// byteSize is what NVRHI allocates against; m_Size stores the same
		// number for the CPU side, so a mismatch between them would only show
		// up as a wrong-sized upload later.
		desc.byteSize = size;
		// The vertex flag is what lets the driver pick the right binding path.
		// isIndexBuffer below is left at its default false.
		desc.isVertexBuffer = true;
		// Raw views OFF: this buffer is only ever read through the vertex
		// layout the owning VertexArray installed, so structured access is
		// neither needed nor wanted.
		desc.canHaveRawViews = false;
		// cpuAccess None: the CPU never reads this back, it only writes through
		// a command list. A readable buffer would cost a copy region for
		// nothing.
		desc.cpuAccess = nvrhi::CpuAccessMode::None;
		desc.debugName = "VertexBuffer";
		// initialState plus keepInitialState means NVRHI can skip the first
		// transition: the buffer is born in the state the first draw wants,
		// and stays there until a write explicitly moves it.
		desc.initialState = nvrhi::ResourceStates::VertexBuffer;
		desc.keepInitialState = true;

		// Identical desc to the size-only constructor above -- same state,
		// same flags -- which is why there is no shared helper. Duplicated
		// rather than factored, so the two constructors cannot drift apart
		// silently.
		// Ref assignment releases any previous handle, so re-running this
		// would replace rather than leak.
		m_Buffer = device->createBuffer(desc);
		if (!m_Buffer)
		{
			WL_CORE_ERROR("NVRHIVertexBuffer: Failed to create buffer");
		}
	}

	// Data-taking constructor. DEFECT: the first parameter is `float*`
	// (a non-const pointer to float) while SetData below takes
	// `const void*`. The engine has no API that hands out a mutable
	// float buffer, so every real call site must be passing something
	// that converts -- which does not work for a const or non-float
	// source. The signature should be const void* plus a byte count.
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
		desc.canHaveRawViews = false;
		desc.cpuAccess = nvrhi::CpuAccessMode::None;
		desc.debugName = "VertexBuffer";
		desc.initialState = nvrhi::ResourceStates::VertexBuffer;
		desc.keepInitialState = true;

		m_Buffer = device->createBuffer(desc);
		if (!m_Buffer)
		{
			WL_CORE_ERROR("NVRHIVertexBuffer: Failed to create buffer");
			return;
		}

		// Upload initial data
		// Guarded upload: null data or a zero size is a legitimate no-op
		// (an empty vertex buffer), distinct from the failure at line 68.
		if (vertices && size > 0)
		{
			// Forwards `size` as bytes, matching the desc.byteSize set above.
			SetData(vertices, size);
		}
	}

	// Non-virtual; holders keep the concrete type so this is safe.
	NVRHIVertexBuffer::~NVRHIVertexBuffer()
	{
		// Dropping the last Ref is the release. Nothing to unbind first --
		// NVRHI resources are reference-tracked, and a buffer still in use by
		// a submitted command list is kept alive by that list.
		m_Buffer = nullptr;
	}

	// Interface-satisfying no-op. On this path vertex buffers are attached
	// through the graphics state at draw time, not bound imperatively.
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

	// Unlike NVRHITexture2D::SetData there is no open/close/execute
	// bracket around the write: the caller's command list is assumed open.
	// Consequence -- a write issued outside a frame is recorded on a
	// list nobody submits, and the upload silently never happens.
	void NVRHIVertexBuffer::SetData(const void* data, uint32_t size)
	{
		// Three-way guard. Unlike the texture path, a null m_Buffer here means
		// construction failed and the caller is left with no error -- SetData
		// returns silently in both the failure case and the legitimate
		// empty-upload case.
		if (!m_Buffer || !data || size == 0)
			return;

		auto* context = NVRHIRendererAPI::GetCurrentContext();
		if (!context)
		{
			WL_CORE_ERROR("NVRHIVertexBuffer::SetData: No NVRHI context available");
			return;
		}

		// Borrowed, not owned. The list belongs to the context and is reused
		// every frame, so the write is recorded rather than submitted.
		nvrhi::ICommandList* cmd = context->GetCommandList();
		if (!cmd)
		{
			WL_CORE_ERROR("NVRHIVertexBuffer::SetData: No command list available");
			return;
		}

		// No size check against m_Size, in any of the three buffers. A caller
		// passing a buffer larger than the allocation gets an
		// out-of-bounds device write with no diagnostic on this side.
		// `size` is bytes, not elements -- writeBuffer's third argument is a
		// byte count. No check that it fits byteSize either: a caller passing
		// more than the buffer holds gets an out-of-bounds write handed
		// straight to the driver.
		cmd->writeBuffer(m_Buffer, data, size);
	}

	/////////////////////////////////////////////////////////////////////////////////////////////
	// NVRHIIndexBuffer
	/////////////////////////////////////////////////////////////////////////////////////////////

	// Index buffer constructor. Unlike the vertex buffer there is no
	// data-free overload, so an index buffer is always created populated.
	NVRHIIndexBuffer::NVRHIIndexBuffer(uint32_t* indices, uint32_t count)
		// m_Count is the ELEMENT count, which is what DrawIndexed wants, whereas
		// `size` below is bytes. Keeping both members is why the draw path has
		// to remember which one a given API wants.
		: m_Count(count)
	{
		nvrhi::IDevice* device = GetDevice();
		if (!device)
		{
			WL_CORE_ERROR("NVRHIIndexBuffer: No NVRHI device available");
			return;
		}

		// count * 4. DEFECT (overflow): the multiply is done in uint32_t, so
		// a count above 2^30 wraps to a small value and the buffer is
		// allocated far too small for the indices that follow.
		uint32_t size = count * sizeof(uint32_t);

		nvrhi::BufferDesc desc;
		desc.byteSize = size;
		// The mirror of isVertexBuffer above; NVRHI uses one flag per role
		// rather than a bitfield.
		desc.isIndexBuffer = true;
		desc.canHaveRawViews = false;
		desc.cpuAccess = nvrhi::CpuAccessMode::None;
		desc.debugName = "IndexBuffer";
		// Born in the index-buffer state so the first indexed draw needs no
		// transition.
		desc.initialState = nvrhi::ResourceStates::IndexBuffer;
		desc.keepInitialState = true;

		m_Buffer = device->createBuffer(desc);
		if (!m_Buffer)
		{
			WL_CORE_ERROR("NVRHIIndexBuffer: Failed to create buffer");
			return;
		}

		// Upload initial data
		// Same null/zero guard as the vertex path. The `count > 0` half is
		// what keeps the multiply at line 132 from being reached with a
		// degenerate size.
		if (indices && count > 0)
		{
			// Passes BYTES (`size`), matching the ctor parameter that was
			// already converted from count at line 132.
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

	// Byte-for-byte the same body as the vertex-buffer SetData above,
	// down to the guard order. Copy-paste rather than a shared template,
	// which is why any fix to one has to be made in the other.
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

	/////////////////////////////////////////////////////////////////////////////////////////////
	// NVRHIStructuredBuffer
	/////////////////////////////////////////////////////////////////////////////////////////////

	// Structured buffer: a different resource shape from the two above --
	// shader-side random access rather than vertex/index fetch, so its
	// desc is genuinely different rather than merely renamed.
	NVRHIStructuredBuffer::NVRHIStructuredBuffer(uint32_t size, uint32_t stride)
		// m_Size is BYTES (see desc.byteSize below) while m_Stride is the
		// per-element byte distance. Conflating them is the easy mistake here,
		// since both parameters arrive as plain uint32_t named `size`/`stride`.
		: m_Size(size), m_Stride(stride)
	{
		nvrhi::IDevice* device = GetDevice();
		if (!device)
		{
			WL_CORE_ERROR("NVRHIStructuredBuffer: No NVRHI device available");
			return;
		}

		nvrhi::BufferDesc desc;
		desc.byteSize = size;
		// The defining field of a structured buffer: how far apart elements
		// sit. The shader's StructuredBuffer<T> declaration must agree, and
		// nothing here checks that they do.
		desc.structStride = stride;
		// UAV-capable, unlike the other two buffer types. That is the whole
		// reason this type exists -- it lets a shader write back results.
		desc.canHaveUAVs = true;
		desc.canHaveRawViews = true;
		// cpuAccess Write, unlike None above: the CPU writes this buffer
		// every frame, and declaring the access mode is what allows the
		// driver to pick a staging path.
		desc.cpuAccess = nvrhi::CpuAccessMode::Write;
		desc.debugName = "StructuredBuffer";
		// Born in ShaderResource because that is the state the consuming
		// compute/graphics pass expects to read it in.
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;

		m_Buffer = device->createBuffer(desc);
		if (!m_Buffer)
		{
			WL_CORE_ERROR("NVRHIStructuredBuffer: Failed to create buffer");
		}
	}

	NVRHIStructuredBuffer::~NVRHIStructuredBuffer()
	{
		m_Buffer = nullptr;
	}

	// Third copy of the same SetData body. Same consequence as the vertex
	// and index versions: no open/close/execute, so the write only lands
	// if the caller is inside a recorded command list.
	void NVRHIStructuredBuffer::SetData(const void* data, uint32_t size)
	{
		if (!m_Buffer || !data || size == 0)
			return;

		auto* context = NVRHIRendererAPI::GetCurrentContext();
		if (!context)
		{
			WL_CORE_ERROR("NVRHIStructuredBuffer::SetData: No NVRHI context available");
			return;
		}

		nvrhi::ICommandList* cmd = context->GetCommandList();
		if (!cmd)
		{
			WL_CORE_ERROR("NVRHIStructuredBuffer::SetData: No command list available");
			return;
		}

		cmd->writeBuffer(m_Buffer, data, size);
	}

}
