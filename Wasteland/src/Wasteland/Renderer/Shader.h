// include guard for the Shader interface header

// Re-including is harmless: the guard makes this a no-op, so a header that
// pulls Shader.h in transitively costs nothing to include directly.
#pragma once

// Shader names and uniform names are std::string, not const char*: they are
// looked up in maps and compared, and the engine passes paths through.
#include <string>
// Backing store for ShaderLibrary::m_Shaders -- a name-to-Shader registry
// keyed by string.
#include <unordered_map>

// glm types appear in the uniform setters below (vec2/vec3/vec4/mat4).

// NOTE: this header is NOT self-contained. Ref<T> is declared in
// Wasteland/Core/Core.h and is used below, but Core.h is not included here.
// It compiles today only because every consumer reaches Shader.h after
// including wlpch.h or Core.h first (e.g. Wasteland.h includes Core.h at
// its line 5, Shader.h at line 32). A translation unit that included
// Shader.h first would fail to compile.
#include <glm/glm.hpp>

// Everything renderer-related lives in the single top-level Wasteland
// namespace; there is no nested Renderer namespace here.
namespace Wasteland
{

	// Abstract base for a compiled GLSL program.

	// The interface is deliberately API-agnostic: no GL handles, no NVRHI
	// objects. Concrete subclasses live under Platform/ (OpenGLShader,
	// NVRHIShader) and this header only names what a shader can do.
	class Shader
	{
	public:
		// Virtual, so deleting a Shader through a Shader* runs the concrete
		// destructor and releases any GL/NVRHI program object.
		virtual ~Shader() = default;

		// Makes this program the active one. Const because binding does not change
		// engine-side state -- only GL global state -- so a const shader is still
		// bindable.
		virtual void Bind() const = 0;
		// Unbinds, leaving no program active. Explicit rather than implicit because
		// a leftover binding silently applies to later draws.
		virtual void Unbind() const = 0;

		// Uniform setters, named after the GLSL uniform they address.

		// Each takes the uniform NAME, not a cached location: the concrete shader
		// resolves name -> location on first use. That keeps call sites free of
		// location plumbing at the cost of a lookup per set.
		virtual void SetInt(const std::string &name, int value) = 0;
		// Array-valued uniform. `count` is the element count, NOT a byte size.

		// The pointer is int* rather than const int*: a non-const signature cannot
		// be satisfied by a caller holding a const array, which is the usual case
		// for a static table.
		virtual void SetIntArray(const std::string &name, int *values, uint32_t count) = 0;
		// Scalar float uniform.
		virtual void SetFloat(const std::string &name, float value) = 0;
		// vec2 uniform.
		virtual void SetFloat2(const std::string &name, const glm::vec2 &value) = 0;
		// vec3 uniform -- the common case for colour and direction.
		virtual void SetFloat3(const std::string &name, const glm::vec3 &value) = 0;
		// vec4 uniform -- colour with alpha, or a packed parameter block.
		virtual void SetFloat4(const std::string &name, const glm::vec4 &value) = 0;
		// mat4 uniform: the transform used by every Renderer3D pass.
		virtual void SetMat4(const std::string &name, const glm::mat4 &value) = 0;
		// The backend-native program handle, as an integer.

		// Integer-typed because that is what OpenGL wants (a GLuint name). The
		// DX11 path has no integer program handle, which is why ImGuiLayer carries
		// a separate ID3D11ShaderResourceView route for texture submission.
		virtual uint32_t GetRendererID() const = 0;

		// The shader's name, used as its key in ShaderLibrary.

		// Returns a reference, not a copy: callers bind it to `auto&` rather than
		// taking ownership.
		virtual const std::string &GetName() const = 0;

		// Factories. Both overloads dispatch on Renderer::GetAPI() and construct the
		// backend-appropriate subclass, so callers never name OpenGLShader or
		// NVRHIShader directly.

		// This one compiles from a file on disk.
		static Ref<Shader> Create(const std::string &filepath);
		// This one compiles from in-memory GLSL source. `name` is a label used for
		// logging and library keying, not a path -- there is no file to reload from.

		// The two overloads are distinguishable at the call site by argument count
		// (1 vs 3), so no ambiguity with the filepath version above.
		static Ref<Shader> Create(const std::string &name, const std::string &vertexSrc, const std::string &fragmentSrc);
	};

	// A process-wide-by-convention name -> shader registry.

	// Not a singleton: there is no static instance member, so a library is
	// constructed and owned by whoever needs it.
	class ShaderLibrary
	{
	public:
		// Registers under an explicit key.

		// Asserts (debug builds only) if the name is already taken; in release the
		// assignment silently replaces the previous shader, which is what a content
		// reload wants but is not what a duplicate-load mistake wants.
		void Add(const std::string &name, const Ref<Shader> &shader);
		// Registers using the shader's own GetName() as the key.

		// For the overwhelmingly common case where the name a shader reports is the
		// name it should be filed under.
		void Add(const Ref<Shader> &shader);
		// Compile-and-register in one step, keyed by the shader's own name.

		// Returns the shader as well as storing it, so a caller can bind it without
		// a second lookup.
		Ref<Shader> Load(const std::string &filepath);
		// As above, but filed under an explicit key.

		// Note the Create() used here is the single-argument filepath overload, so
		// the shader is compiled from disk and the `name` argument only chooses the
		// registry key.
		Ref<Shader> Load(const std::string &name, const std::string &filepath);

		// Looks up by name.

		// Returns a Ref BY VALUE, so each call bumps the shared_ptr refcount.
		// Asserts in debug if the name is unknown; in release the underlying
		// operator[] default-constructs a null Ref instead of failing, so a typo
		// becomes a null dereference at the use site rather than an error here.
		Ref<Shader> Get(const std::string &name);

		// Non-mutating presence check. Exists() is the safe way to probe before
		// Get(), since Get() on a missing name has no defined recovery.
		bool Exists(const std::string &name) const;

	private:
		// Private, so registration can only go through Add() and its assert.

		// Shared ownership (Ref, not Scope): a shader is typically held by both the
		// library and whatever object drew with it, and neither should have to own
		// the lifetime.
		std::unordered_map<std::string, Ref<Shader>> m_Shaders;
	};

}