// Precompiled header first, by engine convention: it carries the standard
// library, the core types (Ref, WL_CORE_ASSERT), and the platform macros.
// Every .cpp in this module starts this way.
#include "wlpch.h"
#include "Shader.h"

// Needed for Renderer::GetAPI(), the dispatch key in both Create overloads.
// Renderer.h is a heavier include than the enum alone, which is why
// RendererAPI.h (which declares API) is pulled in via it rather than directly.
#include "Renderer.h"
// Concrete backend implementations. Included here (rather than forward-
// declared) because both are constructed by value into a std::make_shared,
// which requires the complete type.
#include "Platform/OpenGL/OpenGLShader.h"
#include "Platform/NVRHI/NVRHIShader.h"

// Single top-level namespace, brace on the same line as the declaration.
namespace Wasteland {

	// Factory #1: compile a shader from a file on disk.

	// Note the parameter is `const std::string&` here while the declaration in
	// Shader.h spells the same parameter `const std::string &filepath`. Both
	// forms denote the identical signature, so this is a definition and not an
	// overload.
	Ref<Shader> Shader::Create(const std::string& filepath)
	{
		// One switch picks the backend. Every other renderer factory in the codebase
		// (RendererAPI.cpp, Framebuffer.cpp, Shader.cpp) is written this way, so
		// backend choice lives in exactly one place per resource type.

		// There is deliberately NO default: label. A new RendererAPI::API value
		// added to the enum will silently fall out of the switch and reach the
		// assert below rather than producing a -Wswitch warning at build time.
		switch (Renderer::GetAPI())
		{
			// None means no backend has been selected. Returning nullptr lets the caller
			// decide; the assert makes the mistake visible while developing.

			// IMPORTANT: WL_CORE_ASSERT expands to nothing in release builds (see
			// Core.h). So in a release build this arm is just `return nullptr;` and the
			// diagnostic is gone. Callers that dereference the result without a null
			// check will fault rather than log.
			case RendererAPI::API::None: WL_CORE_ASSERT(false, "RendererApi::None is currently not supported!"); return nullptr;
			// OpenGL backend. make_shared rather than the engine's CreateRef helper:
			// same result (Ref is std::shared_ptr), one fewer name to look up.
			case RendererAPI::API::OpenGL: return std::make_shared<OpenGLShader>(filepath);
			// The three NVRHI backends share one implementation and differ only in which
			// RHI was initialised underneath; the shader code is identical, so they fall
			// through as a group.
			case RendererAPI::API::NVRHI_DX11:
			case RendererAPI::API::NVRHI_DX12:
			case RendererAPI::API::NVRHI_Vulkan:
				// Single shared return for all three NVRHI cases, via fall-through.
				return std::make_shared<NVRHIShader>(filepath);
		}

		// Reached only if Renderer::GetAPI() returns a value not handled above.
		// In debug this breaks into the debugger; in release it vanishes and control
		// falls to the return below.
		WL_CORE_ASSERT(false, "Unknown RendererAPI!");
		// The null return is unconditional and unconditional-in-release, so this
		// function never returns a partially-constructed Shader.
		return nullptr;
	}

	// Factory #2: compile from in-memory GLSL source rather than a path.

	// `name` is a label for logging and library keying; there is no file to
	// reload from, so no hot-reload path exists for a shader built this way.
	Ref<Shader> Shader::Create(const std::string& name, const std::string& vertexSrc, const std::string& fragmentSrc)
	{
		switch (Renderer::GetAPI())
		{
			case RendererAPI::API::None: WL_CORE_ASSERT(false, "RendererApi::None is currently not supported!"); return nullptr;
			// Source-string overload of the OpenGL constructor; the name is forwarded so
			// the shader reports itself under that key from its own GetName().
			case RendererAPI::API::OpenGL: return std::make_shared<OpenGLShader>(name, vertexSrc, fragmentSrc);
			case RendererAPI::API::NVRHI_DX11:
			case RendererAPI::API::NVRHI_DX12:
			case RendererAPI::API::NVRHI_Vulkan:
				// Source-string overload for the NVRHI backend, same fall-through shape as
				// factory #1 above.
				return std::make_shared<NVRHIShader>(name, vertexSrc, fragmentSrc);
		}

		WL_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}

	// Registers a shader under an explicit key.

	// The assert documents the intent -- duplicate keys are a mistake -- but it
	// is compiled out in release, where the assignment on the next line silently
	// overwrites. A content reload relies on that overwrite; a load-twice bug
	// gets it for free.
	void ShaderLibrary::Add(const std::string& name, const Ref<Shader>& shader)
	{
		WL_CORE_ASSERT(!Exists(name), "Shader already exists!");
		// operator[], so this inserts or replaces.

		// The map now holds a second reference to the shader. The caller's Ref is
		// unaffected -- shared_ptr, not a move.
		m_Shaders[name] = shader;
	}

	// Convenience overload: file the shader under its own reported name.

	// Requires GetName() to be non-empty and stable for the lifetime of the
	// object. If a backend returns a reference to a temporary or an unset field,
	// this keys the map on garbage.
	void ShaderLibrary::Add(const Ref<Shader>& shader)
	{
		// Binds by reference, so no string copy is made for the lookup.

		// auto& deduces `const std::string&` here because GetName() returns a const
		// reference -- so `name` is a const reference despite the unadorned auto&.
		auto& name = shader->GetName();
		Add(name, shader);
	}

	// Compile and register in one call, keyed by the shader's own name.
	Ref<Shader> ShaderLibrary::Load(const std::string& filepath)
	{
		// May return nullptr if the backend is None or the API is unknown. Nothing
		// here checks, so a failed compile registers nothing and returns null.

		// Add() on the next line then dereferences that null through GetName() --
		// that is the failure mode, not a silent no-op.
		auto shader = Shader::Create(filepath);
		Add(shader);
		// Returns the same Ref the library now holds, so the caller can bind
		// immediately without a second Get().
		return shader;
	}

	Ref<Shader> ShaderLibrary::Load(const std::string& name, const std::string& filepath)
	{
		auto shader = Shader::Create(filepath);
		Add(name, shader);
		return shader;
	}

	// The main read path.

	// Returns BY VALUE, so every call increments the shared_ptr refcount and
	// every temporary drops it. For a shader fetched once per frame per pass
	// that is measurable churn; returning const Ref& would avoid it at the cost
	// of handing out a reference into the map.
	Ref<Shader> ShaderLibrary::Get(const std::string& name)
	{
		// Existence check, debug only -- see the note on the None case above.

		// DEFECT: this assert is the ONLY guard, and it does not survive a release
		// build. The next line uses operator[], which INSERTS a default-constructed
		// null Ref for a missing key rather than reporting failure. So in release a
		// lookup of an unregistered name silently mutates the library: it now
		// contains a null entry, and a later Exists(name) call returns TRUE for it.
		// The failure is therefore not merely a null dereference at the call site --
		// it corrupts the library state as a side effect of asking.
		WL_CORE_ASSERT(Exists(name), "Shader not found!");
		// operator[] on a std::unordered_map: non-const access, inserts on miss.
		// find()/at() would keep the lookup non-mutating.
		return m_Shaders[name];
	}

	// The non-mutating probe, and the only safe way to test before Get().
	bool ShaderLibrary::Exists(const std::string& name) const
	{
		// find() rather than operator[], so this cannot insert -- const-correct and
		// side-effect free.

		// Also the reason the Get() defect above is worth fixing here rather than at
		// the call sites: once a null entry exists, this reports true for it.
		return m_Shaders.find(name) != m_Shaders.end();
	}

}
