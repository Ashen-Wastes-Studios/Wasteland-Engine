#pragma once

namespace Wasteland {

	// Scoped enum: the type only exists as ShaderDataType::Float, never as a
	// bare Float, so it cannot collide with anything else in scope.
	// None = 0 makes an uninitialised ShaderDataType visibly wrong rather
	// than silently matching a real type.
	enum class ShaderDataType
	{
		// All on one line. The scalar values are implied by declaration order,
		// so they are only stable as long as nobody inserts a member in the
		// middle. Anything persisting these numbers would break.
		// Only None is explicitly valued, which documents that it is the
		// default/zero; the rest are not named.
		None = 0, Float, Float2, Float3, Float4, Mat3, Mat4, Int, Int2, Int3, Int4, Bool
	};

	// Byte size of one value of the given type, in the std140-like packing
	// this engine uses (see CalculateOffsetsAndStride below).
	// DEFECT: `static` at namespace scope gives this INTERNAL linkage --
	// every translation unit including this header gets its own private
	// copy. That is wasted code and it means a call in one TU cannot be
	// inlined from a call in another. It should be `inline`.
	// The name also collides with the BufferElement member `Size` set from
	// it on line 42, which is what makes the layout calculations work.
	static uint32_t ShaderDataTypeSize(ShaderDataType type)
	{
		// Exhaustive switch over every enumerator. Note ShaderDataType::None is
		// deliberately absent -- it falls through to the assert below, because
		// a None element has no meaningful size.
		switch (type)
		{
			// A float is 4 bytes. Every case below is a compile-time constant, so
			// the whole switch is folded away by any optimising compiler.
			case ShaderDataType::Float: return 4;
			// 8 bytes, and it is written as the multiplication rather than a literal
			// so the "4 bytes per component" rule is visible. Repeated for every
			// vector case below.
			case ShaderDataType::Float2: return 4 * 2;
			// DEFECT (important): 12 bytes with NO padding. In std140 (and in
			// std430 for a vec3 in some layouts) a vec3 is followed by 4 bytes of
			// padding so the next member starts 16-byte aligned. This code packs
			// tightly, so any shader expecting std140 will read every field after
			// the first Float3 from the wrong offset. Whether that actually bites
			// depends on how the concrete vertex buffer class declares its
			// attributes, which is not visible in this header.
			case ShaderDataType::Float3: return 4 * 3;
			// 16 bytes: the first type that happens to land on std140's 16-byte
			// boundary naturally.
			case ShaderDataType::Float4: return 4 * 4;
			// 36 bytes. DEFECT: std140 requires a mat3 to occupy 48 bytes (three
			// 16-byte-aligned columns), so this is short by 12 even ignoring the
			// preceding point.
			case ShaderDataType::Mat3: return 4 * 3 * 3;
			// 64 bytes, and correctly sized for std140.
			case ShaderDataType::Mat4: return 4 * 4 * 4;
			case ShaderDataType::Int: return 4;
			case ShaderDataType::Int2: return 4 * 2;
			case ShaderDataType::Int3: return 4 * 3;
			case ShaderDataType::Int4: return 4 * 4;
			// 1 byte, not 4. In GLSL a bool uniform is a full 4-byte uint, and
			// std140 pads bools to 4 as well. So a Bool element in a vertex layout
			// will not line up with what the shader expects.
			// This is the same class of defect as Float3/Mat3 above.
			case ShaderDataType::Bool: return 1;
		}

		// Reached only for None or a value outside the enumerators.
		// Every case above RETURNS, so there is no fallthrough path into this
		// for a valid type -- the assert is purely a guard.
		WL_CORE_ASSERT(false, "Unknown ShaderDataType!");
		// Required because the switch is not provably exhaustive to the
		// compiler. DEFECT: a 0 size would silently corrupt any offset maths
		// if the assert is compiled out in a release build, where WL_CORE_ASSERT
		// is typically a no-op. A caller has no way to distinguish "bad type"
		// from "zero-sized element".
		return 0;
	}

	// One field of a vertex: what it is called, what type, and where it
	// sits in the interleaved buffer.
	// A plain aggregate (two user-declared ctors, still an aggregate for
	// member-initialisation purposes) so it stays trivially copyable.
	struct BufferElement
	{
		// The attribute name, matched against the shader's reflection data
		// when the vertex layout is bound.
		std::string Name;
		// Drives both the byte size and the component count. No in-class
		// initialiser, so a default-constructed BufferElement has an
		// indeterminate Type -- which the empty ctor on line 39 preserves.
		ShaderDataType Type;
		// Byte offset within one vertex. Written by
		// BufferLayout::CalculateOffsetsAndStride, not by the constructor --
		// the constructor hardcodes 0 on line 42 because a layout of one
		// element does need offset 0.
		uint32_t Offset;
		// Cached byte size, computed once in the constructor from
		// ShaderDataTypeSize. Caching it means the switch on lines 12-25 does
		// not run per element per frame.
		uint32_t Size;
		// Whether the GPU should scale integer data into [0,1] on read.
		// Stored but, as far as this header shows, never consumed -- the
		// GL/D3D vertex-array classes would have to read it.
		bool Normalized;

		// DEFECT: a user-provided DEFAULT constructor that initialises nothing.
		// It suppresses the implicit default constructor, so every field above
		// stays indeterminate -- Name defaults to empty (std::string), but
		// Type, Offset, Size and Normalized are garbage.
		// It exists presumably to let BufferElement appear in a container that
		// needs default-constructibility, but the parameterised ctor on line
		// 41 already allows that (it has a default for `normalized`, though
		// `type` and `name` are still required).
		BufferElement() {}

		// The real constructor, and the one that actually populates Size
		// from the type. It leaves the body empty -- everything happens in the
		// member-init list below.
		BufferElement(ShaderDataType type, const std::string& name, bool normalized = false)
			// Note the initialiser ORDER does not match declaration order: Size is
			// declared after Type here and initialised after it, so this is fine,
			// but it is close enough to the declaration order that a reorder could
			// silently change behaviour.
			// Offset(0) hardcodes the start of the vertex; CalculateOffsetsAndStride
			// overwrites it when this element is part of a multi-element layout.
			: Name(name), Type(type), Size(ShaderDataTypeSize(type)), Offset(0), Normalized(normalized)
		{
		}

		// Number of components, i.e. Size divided by 4. const, and so is
		// compatible with the const overloads of BufferLayout::begin/end
		// below.
		// DEFECT: it re-runs the same switch as ShaderDataTypeSize even though
		// Size already holds the answer, so the two tables (lines 12-25 and
		// 48-61) can drift apart independently.
		uint32_t GetComponentCount() const
		{
			switch (Type)
			{
				case ShaderDataType::Float: return 1;
				case ShaderDataType::Float2: return 2;
				case ShaderDataType::Float3: return 3;
				case ShaderDataType::Float4: return 4;
				// 9 components, NOT 3. This is the matrix ROW/COLUMN count rather than
				// a component count, which is why Mat3 returns 9 here but 4*3*3 bytes
				// in ShaderDataTypeSize.
				// That makes the name misleading: the count is only "components" for
				// vectors and scalars, and a flat scalar count for matrices.
				case ShaderDataType::Mat3: return 3 * 3;
				// 16 components, i.e. the matrix dimension squared. For Mat4 the
				// arithmetic happens to equal Size/4, but for Mat3 it does not.
				case ShaderDataType::Mat4: return 4 * 4;
				case ShaderDataType::Int: return 1;
				case ShaderDataType::Int2: return 2;
				case ShaderDataType::Int3: return 3;
				case ShaderDataType::Int4: return 4;
				// One component for a bool.
				case ShaderDataType::Bool: return 1;
			}

			// Same unreachable-for-valid-types guard as line 27, duplicated here
			// rather than shared with ShaderDataTypeSize.
			WL_CORE_ASSERT(false, "Unknown ShaderDataType!");
			// Same release-build hazard as line 28: in a build where the assert is
			// compiled out, 0 components is indistinguishable from a real answer.
			return 0;
		}
	};

	// An ordered set of vertex attributes plus the total stride between
	// consecutive vertices.
	// Holds the elements by value so a layout is self-contained and the
	// offsets it computed cannot be invalidated by a caller mutating the
	// vector it was built from.
	class BufferLayout
	{
	public:
		// Empty layout: no elements, stride 0. legal and useful as a "not yet
		// configured" state, though nothing in this header checks for it.
		BufferLayout() {}

		// The braced form: BufferLayout({ {"u_Position", ShaderDataType::Float3}, ... })
		// so callers can write a layout inline at the call site.
		// DEFECT: takes std::initializer_list BY CONST REFERENCE. An
		// initializer_list is only valid for the duration of the full-expression
		// that created it, and the compiler materialises a backing array -- the
		// reference dangles immediately after the ctor returns. It works only
		// because m_Elements(elements) on line 74 copies it within the same
		// full-expression. Copying the parameter by value would be correct.
		BufferLayout(const std::initializer_list<BufferElement>& elements)
			// The copy from the (dangling-by-contract) initializer_list above,
			// performed inside the mem-init list where it is still valid.
			: m_Elements(elements)
		{
			// Must run in the constructor body, after the elements exist. Every
			// constructor that can populate m_Elements calls it.
			CalculateOffsetsAndStride();
		}

		// The std::vector overload. DEFECT: it is not marked explicit, so a
		// vector<BufferElement> will implicitly convert to a BufferLayout.
		// Harmless in practice but it makes BufferLayout an overly-eager
		// conversion target in overload resolution.
		// This overload has no aliasing problem, unlike the initializer_list
		// one above.
		BufferLayout(const std::vector<BufferElement>& elements)
			: m_Elements(elements)
		{
			CalculateOffsetsAndStride();
		}

		// Bytes per vertex, cached by CalculateOffsetsAndStride. The concrete
		// vertex buffer uses this as its GL_ARRAY_STRIDE.
		// Returns 0 for an empty layout, which a caller passing to a GPU
		// would read as "stride of nothing".
		inline uint32_t GetStride() const { return m_Stride; }
		// const REFERENCE to the internal vector: no copy on each call.
		// Safe because BufferElement is held by value inside, so a caller
		// cannot invalidate the vector through the reference.
		inline const std::vector<BufferElement>& GetElements() const { return m_Elements; }

		// The first of four begin/end overloads. These two non-const ones give
		// mutable iteration, letting a caller rewrite Offset/Size and thereby
		// silently invalidate the computed stride -- the class does not
		// recompute it if that happens.
		std::vector<BufferElement>::iterator begin() { return m_Elements.begin(); }
		std::vector<BufferElement>::iterator end() { return m_Elements.end(); }
		// Const overload, selected for a const BufferLayout. Both overload sets
		// exist so a layout can be range-for-ed either way.
		std::vector<BufferElement>::const_iterator begin() const { return m_Elements.begin(); }
		std::vector<BufferElement>::const_iterator end() const { return m_Elements.end(); }
	private:
		// The single place offsets and the stride are derived.
		// private, so the invariant "members == what CalculateOffsetsAndStride
		// wrote" cannot be broken from outside -- except through the mutable
		// iterators on lines 88-89, which is the hole noted there.
		void CalculateOffsetsAndStride()
		{
			// Running byte cursor. Starts at 0, i.e. the first attribute sits at
			// the start of each vertex.
			uint32_t offset = 0;
			// Explicit reset rather than accumulating onto the previous value, so
			// calling this twice (or on a reused layout) cannot double the stride.
			m_Stride = 0;
			// Non-const reference: line 99 writes into each element.
			for (auto& element : m_Elements)
			{
				// Assign BEFORE advancing, so the first element gets 0 and each
				// subsequent one the total size of everything before it.
				// DEFECT: no alignment padding between elements. This is a purely
				// tightly-packed interleaved layout; any type that needs std140
				// padding (see the Float3/Mat3 notes above) is silently mis-strided.
				element.Offset = offset;
				// Advances by the element's own cached Size, which came from
				// ShaderDataTypeSize -- so this function inherits that table's padding
				// assumptions rather than making its own.
				offset += element.Size;
				// The stride ends up equal to the final offset value, because both
				// accumulate the same sizes from the same starting point. The two
				// accumulators are redundant: m_Stride could be assigned offset after
				// the loop. Kept separate for readability.
				m_Stride += element.Size;
			}
		}
	// Second consecutive private: label, directly after line 92.
	// Redundant, same as elsewhere in this codebase.
	private:
		// The attributes in declaration order. Order IS the memory layout,
		// so it must match the order the vertex attributes were declared in
		// the shader.
		std::vector<BufferElement> m_Elements;
		// In-class initialiser, unlike BufferLayout::m_Elements. The default
		// BufferLayout on line 71 therefore has a defined stride of 0 rather
		// than garbage.
		uint32_t m_Stride = 0;
	};

	// Abstract GPU vertex buffer. The four pure virtuals below are the
	// whole contract; the concrete implementation lives in the OpenGL
	// backend.
	class VertexBuffer
	{
	public:
		// virtual + defaulted: the vtable slot is needed for deletion through
		// the base pointer that Ref<VertexBuffer> hands back.
		virtual ~VertexBuffer() = default;

		// const because binding does not change the C++ object -- it changes
		// GPU state. `const` on a mutating-looking operation is the convention
		// used throughout this renderer.
		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;

		// void* rather than a typed pointer: the caller passes bytes.
		// size is in BYTES, not elements -- the same unit ShaderDataTypeSize
		// reports, though nothing in this header enforces that.
		// Returns void, so an implementation cannot report an upload failure
		// to the caller.
		virtual void SetData(const void* data, uint32_t size) = 0;

		// Tells the backend how the interleaved bytes are to be interpreted.
		// const ref because the layout is copied/stored, not adopted.
		virtual void SetLayout(const BufferLayout& layout) = 0;
		// Returning a reference to the stored layout means the implementer
		// must keep one alive; a reference to a temporary would dangle.
		virtual const BufferLayout& GetLayout() const = 0;

		// Factory pair: a size-only overload that pre-allocates an uninitialised
		// buffer for the caller to SetData into later, and the data overload
		// below.
		// They differ only in the first parameter type (uint32_t vs float*),
		// so calling Create(0) is ambiguous-looking but legal -- it picks the
		// size overload.
		static Ref<VertexBuffer> Create(uint32_t size);
		// Takes float* rather than const float*: the buffer takes ownership of
		// a copy of the data, but the signature still forbids a caller from
		// passing a const array, which is the common case for literal geometry.
		// Returning Ref<> (a shared handle) rather than a raw pointer is why
		// the base destructor has to be virtual.
		static Ref<VertexBuffer> Create(float* vertices, uint32_t size);
	};

	// Existing comment: the 16-bit alternative is not implemented, so
	// the Create signature below hardcodes uint32_t* rather than an enum
	// or an overload.
	// Currently Wasteland only supports 32-bit index buffers
	class IndexBuffer
	{
	public:
		// Same virtual-and-defaulted rationale as VertexBuffer.
		virtual ~IndexBuffer() = default;

		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;

		// void* and size-in-bytes, same contract as the vertex version.
		virtual void SetData(const void* data, uint32_t size) = 0;
		// Element count, not byte count -- needed for the draw call's
		// index count. This is the value Renderer::Submit leaves defaulted to 0
		// (RenderCommand::DrawIndexed), where 0 means "use all of them".
		virtual uint32_t GetCount() const = 0;

		// Single factory -- there is no size-only overload here, so an index
		// buffer must be created with its data already available.
		// uint32_t* (not const) for the same reason as VertexBuffer::Create:
		// literal const index arrays cannot be passed.
		// count is the ELEMENT count, matching GetCount, not the byte size
		// that SetData takes. Two different units for the same quantity in
		// one class.
		static Ref<IndexBuffer> Create(uint32_t* indices, uint32_t count);
	};

}