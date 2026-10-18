#pragma once

#include "Wasteland/Renderer/Shader.h"
#include <nvrhi/nvrhi.h>
#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

namespace Wasteland {

	class NVRHIShader : public Shader
	{
	public:
		// File constructor: reads and compiles from disk. The name is derived
		// from the path by the .cpp, so GetName() below returns something
		// usable in a debug label without a second lookup.
		NVRHIShader(const std::string& filepath);
		// Source constructor, used by tests and by any caller that already
		// has the shader text. Two separate constructors rather than one with
		// defaulted arguments, because "no fragment source" is a real case
		// (a vertex-only shader) and an empty string cannot express it.
		NVRHIShader(const std::string& name, const std::string& vertexSrc, const std::string& fragmentSrc);
		virtual ~NVRHIShader();

		virtual void Bind() const override;
		virtual void Unbind() const override;

		virtual void SetInt(const std::string& name, int value) override;
		// `int*` non-const even though the array is only read -- this is the
		// signature the abstract Shader base declares, so the mismatch
		// propagates. Renderer2D::Init passes a local int32_t[] at line 209.
		virtual void SetIntArray(const std::string& name, int* values, uint32_t count) override;
		virtual void SetFloat(const std::string& name, float value) override;
		virtual void SetFloat2(const std::string& name, const glm::vec2& value) override;
		virtual void SetFloat3(const std::string& name, const glm::vec3& value) override;
		virtual void SetFloat4(const std::string& name, const glm::vec4& value) override;
		virtual void SetMat4(const std::string& name, const glm::mat4& value) override;

		virtual uint32_t GetRendererID() const override;
		// Returns a reference to a member, so it is invalidated by anything
		// that reassigns m_Name. Cheap, but the caller must keep the Shader
		// alive to read it.
		virtual const std::string& GetName() const override { return m_Name; }

		// NVRHI-specific accessors
		// Three separate handles rather than one combined program: NVRHI
		// keeps vertex and pixel stages as distinct objects, and this is the
		// accessor that lets the draw path build its graphics state.
		nvrhi::ShaderHandle GetVertexShader() const { return m_VertexShader; }
		nvrhi::ShaderHandle GetPixelShader() const { return m_PixelShader; }
		// The compute stage. Reached through a different constructor in the
		// .cpp (CompileCompute below), so a shader object is either
		// graphics or compute, never both -- nothing here enforces that.
		nvrhi::ShaderHandle GetComputeShader() const { return m_ComputeShader; }
		
		// The push side of the constant path: SetInt/SetMat4/... write into
		// the CPU-side m_VSConstantData buffers and set a dirty flag, and
		// THIS is what actually uploads them. It is const because recording
		// the upload mutates only the mutable members -- which is also why
		// the const-ness is a slight lie (see the mutable fields at 70).
		void UploadConstants(nvrhi::ICommandList* cmd) const;

		// Constant buffer accessors
		// The GPU-side destination. Separate buffers per stage because each
		// stage gets its own binding, and one buffer bound twice would need
		// two bindings to the same resource.
		nvrhi::BufferHandle GetVSConstantBuffer() const { return m_VSConstantBuffer; }
		nvrhi::BufferHandle GetPSConstantBuffer() const { return m_PSConstantBuffer; }
		
		// Binding layout accessors
		// The binding layout describing this shader's descriptor set. The
		// draw path needs this to build a matching BindingSet, and the two
		// must agree on every binding index -- which is where a shader
		// change that alters bindings tends to break silently.
		nvrhi::BindingLayoutHandle GetBindingLayout() const { return m_BindingLayout; }

	private:
		// The graphics pipeline build. Private because it mutates three
		// handles at once and has no meaningful partial-success state.
		void Compile(const std::string& vertexSrc, const std::string& fragmentSrc);
		// Compute counterpart. Separate from Compile because a compute shader
		// takes one source, not a vertex/fragment pair.
		void CompileCompute(const std::string& computeSrc);
		void CreateConstantBuffers();
		void CreateBindingLayout();
		// Takes RAW BYTECODE, not source: reflection must happen on the
		// compiled binary, because that is where the compiler has recorded
		// the final offsets. `isVertexShader` selects which CPU buffer the
		// discovered offsets are attributed to.
		void ReflectShaderConstants(const void* bytecode, size_t bytecodeSize, bool isVertexShader);
		// const because it writes into the mutable constant vectors rather
		// than into any NVRHI handle -- the actual upload is deferred to
		// UploadConstants. A missing uniform name therefore does not reach
		// the driver at this point; it is a silent no-op here.
		void SetUniformData(const std::string& name, const void* data, size_t size) const;
		
		std::string ReadFile(const std::string& filepath);
		// Returns by value a fresh string each call. A #include-style
		// preprocessor pass -- this is what lets the GLSL assets use includes
		// that the runtime file system would otherwise have to resolve.
		std::unordered_map<std::string, std::string> PreProcess(const std::string& source);

		std::string m_Name;
		// Handles, not raw pointers: nvrhi handles are ref-counted smart
		// wrappers, so assigning or clearing one is the create/destroy.
		nvrhi::ShaderHandle m_VertexShader;
		nvrhi::ShaderHandle m_PixelShader;
		nvrhi::ShaderHandle m_ComputeShader;

		// Constant buffers
		nvrhi::BufferHandle m_VSConstantBuffer;
		nvrhi::BufferHandle m_PSConstantBuffer;

		// Binding layout
		nvrhi::BindingLayoutHandle m_BindingLayout;

		// Uniform data storage (mutable for const methods)
		// mutable, so a const SetInt can stage a value into them. This is
		// why const-correctness on this class is only nominal: calling
		// SetFloat on a const NVRHIShader& still changes observable state
		// (the staged bytes and the dirty flag), it just does not touch a
		// device resource until UploadConstants runs.
		mutable std::vector<uint8_t> m_VSConstantData;
		mutable std::vector<uint8_t> m_PSConstantData;

		// Uniform name to offset mapping
		struct UniformInfo
		{
			// Byte offset into the CPU-side buffer. NOT a hardware register
			// index: the upload path memcpy's into m_VSConstantData at this
			// offset and the whole block goes to the GPU as one buffer.
			uint32_t offset;
			// Size in bytes, which together with `offset` is what lets
			// SetUniformData know how many bytes to copy from the caller's
			// value -- this is how one generic setter serves SetInt, SetFloat4
			// and SetMat4 alike.
			uint32_t size;
			// Which stage's buffer this uniform belongs to. Without it a name
			// that exists in both stages would be ambiguous -- HLSL allows
			// `float4x4 u_ViewProjection;` in both, and they are different
			// uniforms with different values.
			bool isVS; // true if in vertex shader, false if in pixel shader
		};
		// Name -> every stage that declares that name. A VECTOR, not a single
		// entry, because one name may legitimately exist in both the VS and PS
		// constant buffers and they are independent uniforms at independent
		// offsets. The previous name-only map kept just whichever stage
		// reflected last (the PS pass always ran second), so a SetFloat on a
		// dual-declared name silently updated only the PS buffer while the VS
		// copy stayed zero -- which is how Renderer3D_Basic lost its neural
		// flags in the vertex stage and lost per-vertex neural displacement.
		//
		// SetUniformData writes EVERY entry for a name, so a caller sets a
		// uniform once and both stages receive it. A name present in only one
		// stage still has exactly one entry, so single-stage shaders behave
		// exactly as before.
		std::unordered_map<std::string, std::vector<UniformInfo>> m_UniformLocations;

		// mutable + dirty flags: the deferred-upload protocol. A setter marks
		// dirty; UploadConstants uploads and clears. So a setter alone does
		// NOT reach the GPU, and a caller who never calls UploadConstants
		// gets silently stale constants rather than an error.
		mutable bool m_VSConstantsDirty = false;
		mutable bool m_PSConstantsDirty = false;
	};

}
