#pragma once

// Pulls in the abstract base classes (VertexBuffer / IndexBuffer) this
// header subclasses, plus BufferLayout. Buffer.h is engine-side and
// knows nothing about NVRHI -- the dependency runs one way only.
#include "Wasteland/Renderer/Buffer.h"
// The vendor header, included bare rather than through a pch, because
// this header is part of the platform layer's public surface.
#include <nvrhi/nvrhi.h>

namespace Wasteland {

	class NVRHIVertexBuffer : public VertexBuffer
	{
	public:
		// Size-only overload: allocates with no contents. The caller is
		// expected to SetLayout then SetData, matching the GL buffer's
		// construction order in VertexArray::Init.
		NVRHIVertexBuffer(uint32_t size);
		// DEFECT (signature): `float*` is a non-const pointer to a specific
		// type, while SetData below takes `const void*`. Nothing in the
		// engine produces a mutable float buffer, so a call with a const or
		// non-float source will not compile at this overload. It should be
		// `const void*, uint32_t size` -- and note `size` must be BYTES,
		// which the name does not convey.
		NVRHIVertexBuffer(float* vertices, uint32_t size);
		// virtual but the class adds no new destructor behaviour, so the
		// override exists purely to complete the base contract. Not
		// `override` on the keyword here, but the virtual is what lets the
		// base's destructor dispatch correctly.
		virtual ~NVRHIVertexBuffer();

		virtual void Bind() const override;
		virtual void Unbind() const override;

		virtual void SetData(const void* data, uint32_t size) override;

		// The layout lives on the CPU side. NVRHI binds vertex layout through
		// the graphics state at draw time, so this is bookkeeping the engine
		// keeps rather than something pushed to the driver here.
		virtual const BufferLayout& GetLayout() const override { return m_Layout; }
		virtual void SetLayout(const BufferLayout& layout) override { m_Layout = layout; }

		// NVRHI-specific accessor
		// The bridge accessor: this is the ONLY way the NVRHI backend gets at
		// the underlying handle. Everything above this layer speaks VertexBuffer.
		nvrhi::BufferHandle GetBuffer() const { return m_Buffer; }

	private:
		// Default-constructed to an empty handle. If CreateBuffer failed in
		// the .cpp this stays null and every later call must cope -- which
		// the .cpp's guards do.
		nvrhi::BufferHandle m_Buffer;
		BufferLayout m_Layout;
		// Byte size of this handle, cached from the constructor's `size`
		// argument. Like m_Size in the vertex buffer, never updated after
		// construction.
		// Cached CPU-side byte size. Mirrors desc.byteSize from the .cpp; it
		// is never written after construction, so a resize is impossible.
		uint32_t m_Size;
	};

	class NVRHIIndexBuffer : public IndexBuffer
	{
	public:
		// No size-only overload here, unlike the vertex buffer: an index
		// buffer is always created populated. `count` is a COUNT, and the
		// .cpp multiplies by 4 -- which is why GetCount below returns
		// elements rather than bytes.
		NVRHIIndexBuffer(uint32_t* indices, uint32_t count);
		virtual ~NVRHIIndexBuffer();

		virtual void Bind() const override;
		virtual void Unbind() const override;

		virtual void SetData(const void* data, uint32_t size) override;
		// ELEMENTS, not bytes -- this is what DrawIndexed needs as its index
		// count. Deliberately the other unit from SetData's `size`, which is
		// why keeping both members (m_Count here, byte size in the .cpp) is
		// necessary rather than redundant.
		virtual uint32_t GetCount() const { return m_Count; }

		// NVRHI-specific accessor
		nvrhi::BufferHandle GetBuffer() const { return m_Buffer; }

	private:
		nvrhi::BufferHandle m_Buffer;
		uint32_t m_Count;
	};

	// Standalone: does NOT derive from the engine's Buffer interface,
	// because there is no abstract StructuredBuffer in Buffer.h. It is
	// therefore usable only where a caller knows this concrete type.
	class NVRHIStructuredBuffer
	{
	public:
		// Both parameters are uint32_t and neither name says which is which:
		// `size` is BYTES and `stride` is bytes PER ELEMENT. Passing them the
		// other way round is a compile-clean runtime disaster -- a stride
		// larger than the allocation is not validated by anything.
		NVRHIStructuredBuffer(uint32_t size, uint32_t stride);
		virtual ~NVRHIStructuredBuffer();

		// Not an override: this type has no base. Consistent with the
		// comment at line 52.
		void SetData(const void* data, uint32_t size);
		nvrhi::BufferHandle GetBuffer() const { return m_Buffer; }

	private:
		nvrhi::BufferHandle m_Buffer;
		uint32_t m_Size;
		// The per-element stride, cached so the .cpp and any future upload
		// agree with what was declared at creation. Not passed to the shader
		// here -- the HLSL StructuredBuffer<T> declaration carries its own
		// stride, and a mismatch is undetected.
		uint32_t m_Stride;
	};

}
