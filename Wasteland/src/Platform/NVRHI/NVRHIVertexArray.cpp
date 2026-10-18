// PCH first: it establishes the logging macros and the standard headers this
// file needs before any other include declares a symbol from them.
#include "wlpch.h"
// The class being implemented. Pulls in VertexArray.h and transitively
// Buffer.h, so ShaderDataType and BufferLayout are available from here.
#include "NVRHIVertexArray.h"
// For NVRHIVertexBuffer and NVRHIIndexBuffer.
//
// Notably NOT used in this file: nothing here downcasts to the concrete buffer
// types. The input layout is built from the abstract BufferLayout each buffer
// carries, and the actual nvrhi::BufferHandle is fetched later -- by
// NVRHIRendererAPI::DrawIndexed, which does its own dynamic_pointer_cast.
// Keeping the include here is therefore broader than strictly necessary.
#include "NVRHIBuffer.h"
#include "Wasteland/Core/Log.h"

namespace Wasteland {

	// Free function with internal linkage (`static` at file scope), so it does not
	// collide with the identically-purposed helper in NVRHIFramebuffer.cpp.
	// Not `inline` in a header sense -- there is no header here.
	// Helper to convert ShaderDataType to NVRHI Format
	// The engine's engine-agnostic vertex type enum to NVRHI's format enum. This
	// is the translation layer that lets Renderer2D declare ShaderDataType::Float3
	// and have DX11 receive RGB32_FLOAT.
	//
	// Every case RETURNS, so there is no fallthrough between them.
	static nvrhi::Format ShaderDataTypeToNVRHIFormat(ShaderDataType type)
	{
		switch (type)
		{
			case ShaderDataType::Float:  return nvrhi::Format::R32_FLOAT;
			case ShaderDataType::Float2: return nvrhi::Format::RG32_FLOAT;
			// RGB32_FLOAT is 12 bytes, tightly packed -- matching ShaderDataTypeSize's
			// Float3 = 4*3 with no padding.
			//
			// That consistency is what makes this work: NVRHI/D3D reads three consecutive
			// floats with a 12-byte pitch, which is exactly what the CPU-side layout
			// computed. A std140-style padded vec3 (16 bytes) would NOT match, and this
			// file does not have that padding -- consistent within the engine, but it
			// means a hand-written HLSL shader expecting std140 packing reads these
			// attributes from the wrong offsets.
			//
			// RGB (three-channel) formats are broadly available, unlike RG which NVRHI
			// omits for many usages.
			case ShaderDataType::Float3: return nvrhi::Format::RGB32_FLOAT;
			case ShaderDataType::Float4: return nvrhi::Format::RGBA32_FLOAT;
			case ShaderDataType::Int:    return nvrhi::Format::R32_SINT;
			case ShaderDataType::Int2:   return nvrhi::Format::RG32_SINT;
			case ShaderDataType::Int3:   return nvrhi::Format::RGB32_SINT;
			case ShaderDataType::Int4:   return nvrhi::Format::RGBA32_SINT;
			// An 8-bit unsigned value, NOT a bool format. Consistent with
			// ShaderDataTypeSize's Bool = 1 byte.
			//
			// HLSL `bool` in an input layout reads 4 bytes, so a Bool vertex attribute
			// declared as `bool` in a shader would be fed the wrong stride here. Nothing
			// in the engine currently puts a Bool in a vertex layout, so this is latent.
			case ShaderDataType::Bool:   return nvrhi::Format::R8_UINT;
			// Catches ShaderDataType::None AND -- importantly -- Mat3 and Mat4, which are
			// NOT handled above even though they exist in the enum.
			//
			// DEFECT: a vertex attribute declared Mat3 or Mat4 hits this branch. In a
			// Debug build it asserts; in Release it silently yields nvrhi::Format::UNKNOWN,
			// which createInputLayout will reject or misbehave on. A matrix in a vertex
			// buffer is normally expressed as three or four Float3/Float4 attributes, so
			// this is likely never exercised -- but the mapping is incomplete rather than
			// merely untested.
			default:
				WL_CORE_ASSERT(false, "Unknown ShaderDataType!");
				return nvrhi::Format::UNKNOWN;
		}
	}

	// Pre-existing comment. Read together with the DEFECT noted at ToHLSLSemantic's
	// body below: the translation is required, and it is also lossy.
	// HLSL input semantics required by D3D11 input layouts. NVRHI passes the
	// attribute name straight through as SemanticName, so engine-side names
	// (a_Position, a_TexCoord, ...) must be translated to match the .hlsl
	// shaders' declared semantics (POSITION, TEXCOORD, ...) or
	// CreateInputLayout fails with E_INVALIDARG.
	// Translates an engine attribute name to the semantic string NVRHI will use as
	// the D3D11 SemanticName. The engine's GLSL-style names (a_Position) and the
	// HLSL shaders' declared semantics (POSITION) are different vocabularies, and
	// D3D matches on the semantic name.
	//
	// A chain of if-comparisons rather than a static map. Cheap enough (called once
	// per attribute, on layout build) and it keeps the table visible at a glance
	// in the order attributes are declared.
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
		// DEFECT (a real one): a_WorldPosition maps to the SAME "POSITION" semantic as
		// a_Position above. In a D3D11 input layout, SemanticName is only unique
		// together with SemanticIndex -- two attributes both named POSITION with index
		// 0 is a duplicate, and createInputLayout fails (or binds the wrong data).
		//
		// Because desc.arraySize is hard-coded to 1 and SemanticIndex is never set,
		// nothing here disambiguates them. A vertex buffer containing both a_Position
		// and a_WorldPosition therefore cannot be described to NVRHI.
		//
		// No shader in the engine currently declares both, so it is latent rather than
		// active -- but the two names are exactly the kind of pair a 3D mesh format
		// would carry.
		if (name == "a_WorldPosition") return "POSITION";
		if (name == "a_Thickness")     return "THICKNESS";
		if (name == "a_Fade")          return "FADE";
		if (name == "a_Metallic")      return "METALLIC";
		if (name == "a_Roughness")     return "ROUGHNESS";
		if (name == "a_Displacement")  return "DISPLACEMENT";
		if (name == "a_BumpStrength")  return "BUMPSTRENGTH";
		// An unmapped name passes through unchanged, which is the escape hatch for
		// attributes whose semantic matches their name.
		//
		// The pre-existing comment above is slightly optimistic: on Vulkan there is no
		// semantic at all (NVRHI derives location from arrayIndex), so a pass-through
		// there is harmless. On D3D, an unmapped name reaches CreateInputLayout as a
		// SemanticName the shader does not declare, and the attribute silently reads
		// as zero rather than failing loudly.
		//
		// The list above therefore has to be extended whenever a shader adds an
		// attribute -- there is no assert to catch a miss.
		// Fall back to the name itself (shader may use it verbatim)
		// DANGEROUS RETURN, and the lifetime argument is subtle: this returns a
		// pointer into the `name` PARAMETER, a const std::string&. If the caller
		// passed a temporary, that string dies when this function returns and the
		// pointer dangles.
		//
		// In the one call site (GetInputLayout below) the argument is
		// element.Name, a member of a BufferElement owned by the vertex buffer's
		// BufferLayout -- which outlives the cached input layout, so the pointer
		// stays valid. That is the invariant the call site's own comment records.
		//
		// The literal returns above are static storage and always safe. So the safety
		// of the whole table depends on the caller -- nothing in the signature
		// expresses that.
		return name.c_str();
	}

	// Empty, and could be `= default`. Every member has an initialiser in the
	// header, so there is genuinely nothing to do here.
	NVRHIVertexArray::NVRHIVertexArray()
	{
	}

	// Empty for a reason worth stating: this class owns no nvrhi resource.
	// m_VertexBuffers and m_IndexBuffer are Ref<>s to NVRHIVertexBuffer /
	// NVRHIIndexBuffer, and dropping them here releases one reference each.
	// The nvrhi::BufferHandle inside those buffers is released when the LAST
	// reference goes -- so if Renderer2D or another holder still owns a buffer,
	// the GPU resource survives this destructor.
	//
	// That is also the ordering hazard: nvrhi handles must be released before
	// the nvrhi::IDevice is shut down, or the release touches a dead device.
	NVRHIVertexArray::~NVRHIVertexArray()
	{
	}

	// const because the base declares it const, and because binding is a GPU-state
	// change, not a change to this C++ object -- the convention throughout this
	// renderer.
	void NVRHIVertexArray::Bind() const
	{
		// The pre-existing comment above states the reason correctly. Expanded: NVRHI
		// has no glBindVertexArray equivalent because a pipeline object already fixes
		// the input layout, and the buffers themselves are supplied per draw through
		// nvrhi::GraphicsState.vertexBuffers.
		// Note: In NVRHI, vertex buffers are bound via GraphicsState
		// This is handled in the Renderer implementation
		// For now, this is a no-op
	}

	// Symmetric no-op with Bind. There is nothing to unbind because nothing was
	// ever bound here -- a caller written against the OpenGL VertexArray
	// interface gets correct-looking code that does nothing on this backend.
	void NVRHIVertexArray::Unbind() const
	{
		// No-op in NVRHI
	}

	// Takes a Ref to the ABSTRACT VertexBuffer, not to NVRHIVertexBuffer. So this
	// cannot see the nvrhi::BufferHandle and does not try to; the handle is
	// fetched at draw time by the renderer, which downcasts then.
	//
	// Appends rather than replaces -- the resulting order is the NVRHI slot order
	// (see the header's note on GetVertexBuffers).
	void NVRHIVertexArray::AddVertexBuffer(const Ref<VertexBuffer>& vertexBuffer)
	{
		// Guards the one precondition this method actually has: a buffer with no
		// layout contributes no attributes, so the cached input layout would silently
		// be shorter than the shader expects and attributes would read from wrong
		// offsets.
		//
		// ASSERT ONLY -- in a Release build a layout-less buffer is accepted and
		// produces a quietly wrong vertex layout rather than an error. There is no
		// runtime check, no log and no skip.
		//
		// The trailing space after the comma is pre-existing.
		WL_CORE_ASSERT(vertexBuffer->GetLayout().GetElements().size() > 0, 
			"Vertex buffer has no layout!");
		
		// May reallocate. That is why GetInputLayout's raw-pointer contract is only
		// valid until the next AddVertexBuffer -- a caller holding the pointer across
		// this call reads freed memory.
		//
		// Copies the Ref, so the buffer's lifetime extends to at least this array's.
		m_VertexBuffers.push_back(vertexBuffer);
		// Invalidate the cache. Setting it rather than rebuilding eagerly is the point
		// of the lazy design: Renderer2D adds all buffers at Init time and then the
		// layout is built once, on the first draw, and reused for every draw after.
		m_InputLayoutDirty = true;
	}

	// REPLACES rather than appends -- there is exactly one index buffer per vertex
	// array, which is why the member is a single Ref rather than a vector.
	//
	// Does NOT touch m_InputLayoutDirty, correctly: the index buffer plays no part
	// in the vertex input layout, so rebuilding it would be wasted work.
	//
	// No null check and no assert: passing a null Ref is accepted, and the failure
	// surfaces later in the renderer's DrawIndexed ("No index buffer" error).
	void NVRHIVertexArray::SetIndexBuffer(const Ref<IndexBuffer>& indexBuffer)
	{
		m_IndexBuffer = indexBuffer;
	}

	// The lazily-built cache accessor. `count` is a C-style out-parameter because
	// NVRHI's createInputLayout takes (desc*, count) -- there is no span overload,
	// so the length cannot travel with the pointer.
	//
	// const with `mutable` members doing the writing; see the header's note on why
	// that trade was made. Not thread-safe as a result.
	nvrhi::VertexAttributeDesc* NVRHIVertexArray::GetInputLayout(uint32_t& count) const
	{
		// True on first call (the header initialises it true) and after every
		// AddVertexBuffer. False thereafter, so the rebuild cost is paid once per
		// change rather than once per draw.
		//
		// Note the flag is cleared at the END of the block, not the start: a rebuild
		// that threw or returned early would leave it dirty, which is the safer of the
		// two failure modes.
		if (m_InputLayoutDirty)
		{
			// Clear rather than assign a fresh vector, so the existing capacity is reused
			// across rebuilds -- important because a pipeline cache rebuild after a buffer
			// is added would otherwise reallocate every time.
			//
			// Also invalidates any pointer previously returned by this function, which is
			// the documented lifetime rule.
			m_InputLayout.clear();
			uint32_t bufferIndex = 0;
			
			// By const reference: avoids copying each Ref (and bumping its refcount) per
			// buffer.
			//
			// The loop index is not used; a separate counter below supplies the NVRHI
			// buffer index instead.
			for (const auto& vb : m_VertexBuffers)
			{
				const auto& layout = vb->GetLayout();
				// A running byte cursor that is written but NEVER READ.
				// DEFECT (dead code): the loop below uses element.Offset -- the value
				// BufferLayout::CalculateOffsetsAndStride already computed on the CPU -- and
				// advances this local offset by element.Size without ever consulting it.
				// The accumulated total is discarded when the iteration ends.
				//
				// It reads as if it were computing offsets here, which is actively misleading:
				// a reader could conclude the layout is rebuilt per buffer rather than read
				// from the BufferLayout. It also shadows nothing, so the compiler may warn
				// about it being set-but-unused in a strict build.
				//
				// Per-BUFFER (reset each outer iteration), which is the right granularity if
				// it were used at all -- offsets restart at each vertex buffer's base.
				uint32_t offset = 0;
				
				for (const auto& element : layout)
				{
					// Default-constructed, so every field NVRHI does not assign below takes
					// VertexAttributeDesc's default. Notably desc.arraySize is set explicitly, but
					// desc.ARRAYINDEX (the NVRHI field name) is left at its default of 0 for every
					// attribute -- which is correct only because NVRHI/D3D derives the slot from
					// bufferIndex, not from arrayIndex.
					//
					// Constructed inside the loop and copied into the vector by push_back below,
					// rather than emplaced. A 40-odd-byte POD copy per attribute, once per layout
					// rebuild -- not worth optimising.
					nvrhi::VertexAttributeDesc desc;
					// ToHLSLSemantic returns either a literal or element.Name's own
					// storage — both outlive the layout (VB owns the layout strings)
					// The semantic-name translation, and the lifetime-critical line: desc.name is a
					// `const char*` that may point either at a string literal (from the table in
					// ToHLSLSemantic) or at element.Name's own storage. Both outlive this vector
					// as long as the vertex buffer does, because BufferLayout holds its elements
					// by value.
					//
					// That is the whole safety argument for ToHLSLSemantic returning name.c_str():
					// the vector is a member of THIS object, and the strings it points into are
					// members of buffers THIS object also holds a Ref to. Destroy the vertex array
					// and both die together.
					//
					// If a caller ever handed GetInputLayout a layout from a temporary buffer, this
					// would dangle. Nothing enforces that ordering.
					desc.name = ToHLSLSemantic(element.Name);
					// The other half of the translation layer. A Mat3/Mat4 element lands on that
					// function's default branch and becomes Format::UNKNOWN -- see the defect noted
					// there.
					//
					// element.Normalized is IGNORED. BufferLayout::BufferElement carries a
					// Normalized flag that some vertex formats need (integer data scaled into
					// [0,1]), and no NVRHI VertexAttributeDesc field is set from it here. On
					// OpenGL that flag maps to glVertexAttribPointer's normalized parameter, so
					// this is a backend divergence: the same layout normalises under GL and not
					// under NVRHI. Nothing in the engine sets Normalized to true today.
					desc.format = ShaderDataTypeToNVRHIFormat(element.Type);
					// Read from the CPU-computed layout, NOT from the dead local offset above.
					// That is the correct source: BufferLayout::CalculateOffsetsAndStride already
					// packed these with no alignment padding, and element.Offset is relative to
					// the start of THIS vertex buffer -- which is what NVRHI wants alongside
					// bufferIndex.
					//
					// Note this is the tight-packing assumption discussed at the Float3 case
					// above: consistent within the engine, divergent from std140.
					desc.offset = element.Offset;
					// Which vertex BUFFER, from the counter incremented at the end of the outer
					// loop -- not the inner one. So a mesh split across several buffers gets
					// bufferIndex 0, 1, 2, ... and every attribute within a buffer shares that
					// index.
					//
					// This must agree with the slot NVRHIRendererAPI::DrawIndexed assigns, which it
					// derives from the same m_VertexBuffers position via its own loop index. The
					// two agree because both walk the same vector in the same order.
					desc.bufferIndex = bufferIndex;
					// Bytes between consecutive vertices in this buffer, cached by
					// BufferLayout::CalculateOffsetsAndStride. Read once per attribute even though
					// it is the same for all of them in a buffer -- a property of the loop shape,
					// not a bug.
					//
					// 0 for an empty layout, which the AddVertexBuffer assert is there to prevent.
					desc.elementStride = layout.GetStride();
					// Hard-coded scalar. A vertex attribute array (e.g. a 4-element tangent where
					// each component is a separate attribute) would need this above 1, and NVRHI
					// requires arraySize and arrayStride to agree with the shader's declaration.
					//
					// Nothing in the engine needs it; every attribute is a scalar per vertex.
					desc.arraySize = 1;

					// Copies the assembled desc. Reallocation here is safe -- the vector owns its
					// contents -- but it is why the pointer returned at the end of this function is
					// only good until the NEXT rebuild.
					//
					// The desc's `name` pointer is copied by value, not deep-copied; the target
					// string lives in the vertex buffer as noted above.
					m_InputLayout.push_back(desc);
					offset += element.Size;
				}
				
				// Advances to the next vertex buffer. Declared outside the inner loop so it
				// accumulates across buffers; it is deliberately NOT reset per buffer.
				//
				// Overflow after 4 billion buffers is not a practical concern.
				bufferIndex++;
			}
			
			// Only after a complete rebuild, so a partial build cannot mark the cache
			// valid. Set at the end of the dirty block, outside both loops.
			m_InputLayoutDirty = false;
		}
		
		// The out-parameter. Always assigned, on both the cached and the freshly-built
		// path, so the caller always receives a length -- including 0 for a vertex
		// array with no buffers added.
		//
		// static_cast rather than an implicit conversion: size() is size_t (64-bit) and
		// NVRHI's parameter is uint32_t, so this is a narrowing conversion made
		// explicit. Unrealistic to overflow, but it is now visible rather than silent.
		count = static_cast<uint32_t>(m_InputLayout.size());
		// A pointer into this object's member vector -- valid until the next
		// AddVertexBuffer or until this object is destroyed.
		//
		// data() on an empty vector returns a valid (possibly null) pointer rather than
		// UB, with count 0 alongside it, so a zero-attribute vertex array is handled
		// without a special case by the caller.
		return m_InputLayout.data();
	}

}
