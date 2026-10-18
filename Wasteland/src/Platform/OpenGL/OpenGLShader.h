#pragma once

// The backend-agnostic base class. This header is deliberately unaware of
// glad or any GL type beyond the one typedef below, so the interface
        // layer stays free of OpenGL.
#include "Wasteland/Renderer/Shader.h"
// glm core only. mat3/vec4 are used by the uniform-upload declarations,
// and none of them requires an extension header.
#include <glm/glm.hpp>

// DEFECT (type collision hazard, documented not fixed): a file-scope
        // typedef of unsigned int as GLenum, sitting at global scope in a
        // header. If glad/glad.h is ever included in the same translation
        // unit, the two typedefs must agree exactly or the compiler rejects
        // the redefinition. It happens to agree (GL's GLenum is unsigned
        // int) but only by convention, and the TODO acknowledges it.
// TODO: REMOVE
// Not an enum, not uint32_t: matching GL's own typedef is the whole
// point, since PreProcess below keys a map on it. A typedef rather
// than using-declaration because there is no GL namespace to alias.
typedef unsigned int GLenum;

namespace Wasteland
{

	// Public inheritance from an abstract base, with `virtual ... override`
	// on every implementation. DEFECT (missing virtual destructor,
	// documented not fixed): Shader is an abstract base and this class
	// declares ~OpenGLShader() without `virtual`, so deleting a Shader*
	// that points at an OpenGLShader is undefined behaviour -- the GL
	// program would leak. This is safe ONLY as long as every Shader*
	// owner stores a shared_ptr or Ref with a deleter. Nothing in this
	// header enforces that.
	class OpenGLShader : public Shader
	{
	public:
		// File-backed: reads, splits on #type, compiles. m_Name is left empty by
		// this ctor -- see the dead-code note in the .cpp.
		OpenGLShader(const std::string &filepath);
		OpenGLShader(const std::string &name, const std::string &vertexSrc, const std::string &fragmentSrc);
		// Deletes the GL program. Not virtual -- see the base-class note above.
		~OpenGLShader();

		void Bind() const;
		void Unbind() const;

		// The Set* family is the Shader interface; the Upload* family below is the
		// backend-specific implementation that most of these forward to.
		// Redirection matters: a caller holding a Shader* can never reach the
		// Upload* half, so an API added only there would be unreachable
		// through the abstraction.
		virtual void SetInt(const std::string &name, int value) override;
		// A raw int* plus a count, matching the base interface. The caller keeps
		// ownership of the array for the duration of the call -- there is no
		// copy, so the pointer must stay valid and the count must be right.
		virtual void SetIntArray(const std::string &name, int *values, uint32_t count) override;
		virtual void SetFloat(const std::string &name, float value) override;
		virtual void SetFloat2(const std::string &name, const glm::vec2 &value) override;
		virtual void SetFloat3(const std::string &name, const glm::vec3 &value) override;
		virtual void SetFloat4(const std::string &name, const glm::vec4 &value) override;
		virtual void SetMat4(const std::string &name, const glm::mat4 &value) override;
		// uint32_t, not GLuint, at the interface boundary -- the abstraction layer
		// does not use GL types except for the one leaked typedef above.
		// The ID is exposed for interop (binding a program to a pipeline,
		// or handing it to another backend), which is why it is public.
		virtual uint32_t GetRendererID() const override { return m_RendererID; }

		// By reference to a member, so it is valid for the shader's lifetime and
		// costs no copy on what is often a per-draw lookup.
		virtual const std::string &GetName() const override { return m_Name; }

		// NOT virtual and NOT override: backend-specific extras. That asymmetry is
		// deliberate -- these are for code that has an OpenGLShader concretely
		// in hand, and the base class does not promise them.
		// DEFECT (asymmetry, documented not fixed): the base Shader interface
		// has no SetMat3, but UploadUniformMat3 exists below. Mat3 is
		// therefore settable only on the concrete GL type -- any code that
		// wants to be backend-portable cannot set a mat3 uniform at all.
		void UploadUniformInt(const std::string &name, int value);
		void UploadUniformIntArray(const std::string &name, int *values, uint32_t count);

		void UploadUniformFloat(const std::string &name, float value);
		void UploadUniformFloat2(const std::string &name, const glm::vec2 &values);
		void UploadUniformFloat3(const std::string &name, const glm::vec3 &values);
		void UploadUniformFloat4(const std::string &name, const glm::vec4 &values);

		// Present as an implementation with no public interface counterpart. See
		// the note above: reachable only through the concrete class.
		void UploadUniformMat3(const std::string &name, const glm::mat3 &matrix);
		void UploadUniformMat4(const std::string &name, const glm::mat4 &matrix);

	// Two access specifiers in a row: this one guards the file-reading,
	// preprocessing, compiling and uniform-location helpers. They are
	// implementation details of construction and upload, not part of any
	// interface.
	private:
		// Private and non-const: it touches no member state, so const would be
		// more accurate, but it is only ever called from a ctor where `this`
		// is not yet fully initialised.
		std::string ReadFile(const std::string &filepath);
		// Keyed by the leaked GLenum typedef from the top of this file -- the
		// return type is therefore the one place this header exposes a GL
		// type in a signature. Acceptable because the method is private, so
		// no caller outside the class can see it.
		std::unordered_map<GLenum, std::string> PreProcess(const std::string &source);
		void Compile(const std::unordered_map<GLenum, std::string> &shaderSources);
		// Non-const because it MUTATES the cache below. Returns -1 for a uniform
		// that does not exist or was optimised out, which is a legitimate
		// cached value rather than an error.
		int32_t GetUniformLocation(const std::string &name);

	private:
		// No in-class initialiser. It is assigned inside Compile(), which every
		// ctor calls -- so it is never read uninitialised on the success
		// path, but a Compile() that failed before the assignment leaves it
		// holding whatever the ctor's member-init left, which is nothing
		// deterministic. A `= 0` default would close that window.
		uint32_t m_RendererID;
		// Empty by default, and never set by the file-based ctor: the name
		// extraction at the end of that ctor computes a value and discards
		// it (see the .cpp). GetName() on such a shader returns "".
		std::string m_Name;
		// Caches the glGetUniformLocation result per name. Never invalidated: a
		// linked GL program's uniform locations are fixed for its lifetime,
		// so staleness is not possible. The cache dies with the shader.
		std::unordered_map<std::string, int32_t> m_UniformLocationCache;
	};

}