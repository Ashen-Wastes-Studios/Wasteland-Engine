// Precompiled header first: it establishes the GL loader and the logging
// macros before anything else declares a symbol from them.
#include "wlpch.h"
#include "OpenGLShader.h"

// Only standard-library addition. <array>/<vector>/<string> all come via
// wlpch, which is why ShaderTypeFromString below can use std::string
// without a local include.
#include <fstream>

#include <glad/glad.h>

// glm::value_ptr for the matrix uploads at the bottom of this file.
// Header-only; there is no glm library to link.
#include <glm/gtc/type_ptr.hpp>

namespace Wasteland
{

	// Translation of the #type token in a .glsl file to a GL stage enum.
	// Internal linkage: this detail is not part of the Shader interface.
	static GLenum ShaderTypeFromString(const std::string &type)
	{
		if (type == "vertex")
			return GL_VERTEX_SHADER;
		// "pixel" accepted as an alias for "fragment" for HLSL vocabulary.
		// If-chain rather than a map lookup: three comparisons, and it keeps the
		// assert-and-default tail readable.
		if (type == "fragment" || type == "pixel")
			return GL_FRAGMENT_SHADER;
		if (type == "compute")
			return GL_COMPUTE_SHADER;

		// Assert fires only in Debug. In a Release build the next line returns 0,
		// which is not a valid GLenum stage -- the caller below would hand it to
		// glCreateShader. Acceptable because PreProcess already asserts on the
		// same value one step earlier.
		WL_CORE_ASSERT(false, "Unknown shader type!");
		return 0;
	}

	// File-backed ctor: read, split on #type markers, compile. No member
	// initialiser list here -- see the defect noted at the name-extraction
	// block below.
	OpenGLShader::OpenGLShader(const std::string &filepath)
	{
		WL_PROFILE_FUNCTION();

		// By value: ReadFile returns by value anyway and the string is needed as
		// an lvalue for the const& PreProcess parameter.
		std::string source = ReadFile(filepath);
		auto shaderSources = PreProcess(source);
		Compile(shaderSources);

		// Extract name from filepath
		// DEFECT (dead code, documented not fixed): the four statements below
		// compute a name and then DISCARD it -- substr's return value is not
		// assigned to anything, and m_Name is never set in this constructor.
		// The result is a file-loaded shader with an empty m_Name. The fix is to
		// assign the substr to m_Name (or initialise m_Name in a member list);
		// not applied here because this pass is comments-only.
		auto lastSlash = filepath.find_last_of("/\\");
		// npos means "no directory component", so the filename starts at 0.
		// Writing it as `lastSlash + 1` unconditionally would wrap npos to 0
		// anyway (npos + 1 == 0), so the conditional is defensive rather than
		// necessary -- harmless, and clearer about intent.
		lastSlash = lastSlash == std::string::npos ? 0 : lastSlash + 1;
		// rfind, not find: the extension is the LAST dot in the path, since
		// directories and version suffixes like "shader.glsl.bak" both contain
		// earlier dots.
		auto lastDot = filepath.rfind('.');
		// Turns an INDEX into a LENGTH for the substr call, and handles the
		// no-extension case by running to the end of the string. The subtraction
		// is safe because lastSlash is already past the last slash and cannot
		// exceed size().
		auto count = lastDot == std::string::npos ? filepath.size() - lastSlash : lastDot - lastSlash;
		filepath.substr(lastSlash, count);
	}

	// The in-memory ctor, used for built-in shaders compiled from string
	// literals rather than files.
	OpenGLShader::OpenGLShader(const std::string &name, const std::string &vertexSrc, const std::string &fragmentSrc)
		// The member list sets ONLY m_Name. Compare with the file ctor above,
		// which never sets it at all -- so m_RendererID is likewise default-
		// initialised here and assigned inside Compile() rather than at
		// construction.
		: m_Name(name)
	{
		WL_PROFILE_FUNCTION();

		// A map keyed by stage, because the container is filled by callers that
		// do not all supply both stages and Compile() iterates whatever it gets.
		std::unordered_map<GLenum, std::string> sources;
		sources[GL_VERTEX_SHADER] = vertexSrc;
		sources[GL_FRAGMENT_SHADER] = fragmentSrc;
		Compile(sources);
	}

	// Deletes the program only. The individual stage shaders were detached
	// and deleted at the end of Compile(); GL keeps them alive until the
	// program is deleted, so deleting them again here would be a double free.
	OpenGLShader::~OpenGLShader()
	{
		WL_PROFILE_FUNCTION();

		// Unconditional, with no check for a valid handle. If Compile() failed
		// and asserted in a Debug build, m_RendererID still holds whatever the
		// last compile produced -- see the note in Compile() about the failure
		// path.
		glDeleteProgram(m_RendererID);
	}

	// The seek/size/read dance rather than std::getline: shaders are not
	// line-oriented from this function's point of view, and binary mode
	// avoids any newline translation on Windows.
	std::string OpenGLShader::ReadFile(const std::string &filepath)
	{
		WL_PROFILE_FUNCTION();

		std::string result;
		// The stream is opened in the constructor -- the bool conversion on the
		// next line is the existence check.
		std::ifstream in(filepath, std::ios::in | std::ios::binary);
		if (in)
		{
			// Size discovery by seeking: tellg() on a binary-mode text file is not
			// reliable for counting characters, and resize() needs an exact count.
			in.seekg(0, std::ios::end);
			// tellg() returns a streampos; the implicit conversion to size_type is
			// why resize() is used rather than assigning to tellg directly.
			result.resize(in.tellg());
			in.seekg(0, std::ios::beg);
			// Writing through the first character's address is safe for exactly
			// result.size() bytes because resize() already made the string that
			// long. &result[0] is pre-C++11 style; data() would be the modern form.
			in.read(&result[0], result.size());
			in.close();
		}
		else
		{
			// DEFECT (silent failure, documented not fixed): the error is logged and
			// an EMPTY string is returned. PreProcess() on an empty string finds no
			// #type token and returns an empty map, so Compile() creates and links a
			// program with no stages -- a valid-but-useless GL program rather than a
			// visible failure. Nothing propagates the open error to the caller.
			WL_CORE_ERROR("Could not open file '{0}'", filepath);
		}

		return result;
	}

	// Finds the next "#type" directive that is genuinely a directive: the token
	// must be the first thing on its line, after optional spaces or tabs.
	//
	// A plain find() is not good enough, and the difference is not theoretical.
	// This loader's own header comment quotes the directive while explaining it
	// ("// #type compute -- a GL loader directive..."), and a naive find() locked
	// onto that COMMENT rather than the real directive further down. It derived
	// the stage name from the rest of the comment sentence, which failed the type
	// lookup, and it then treated a second comment mentioning "#type" as the end
	// of the stage -- truncating the source before its main(). The program linked
	// with no usable stage, so every glGetUniformLocation returned -1; glUniform
	// against -1 is a documented no-op, which left u_PassID at its default 0 and
	// made every dispatch run pass 0. The ray-traced viewport stayed black with no
	// shader error reported, because linking a stage-less program SUCCEEDS.
	//
	// Anchoring to the start of the line makes the loader comment-proof, which is
	// what keeps a future explanatory comment from silently disabling the renderer.
	static size_t FindTypeDirective(const std::string &source, size_t from, size_t tokenLength)
	{
		size_t pos = source.find("#type", from);
		while (pos != std::string::npos)
		{
			// Walk back to the first character of this line. find_last_of with a
			// from-index of pos - 1 is undefined at pos == 0, so the start-of-file
			// case is handled explicitly.
			size_t lineStart = 0;
			if (pos > 0)
			{
				size_t nl = source.find_last_of("\r\n", pos - 1);
				lineStart = (nl == std::string::npos) ? 0 : nl + 1;
			}

			// Only whitespace may precede the token on its line.
			bool directiveLike = true;
			for (size_t i = lineStart; i < pos; ++i)
			{
				if (source[i] != ' ' && source[i] != '	')
				{
					directiveLike = false;
					break;
				}
			}
			if (directiveLike)
				return pos;

			// Not a directive -- inside a comment or a string. Keep looking.
			pos = source.find("#type", pos + tokenLength);
		}
		return std::string::npos;
	}

	// Splits a multi-stage .glsl file on its #type markers into one source
	// string per GL stage. This is why a shader file can carry a vertex and
	// a fragment section in a single asset.
	std::unordered_map<GLenum, std::string> OpenGLShader::PreProcess(const std::string &source)
	{
		WL_PROFILE_FUNCTION();

		std::unordered_map<GLenum, std::string> shaderSources;

		// A raw C string rather than std::string because the next line uses
		// strlen on it and find() would take it either way.
		const char *typeToken = "#type";
		size_t typeTokenLength = strlen(typeToken);
		// The explicit 0 is the search start and matches the from-position
		// overload; the loop below advances it. Anchored, so a "#type" quoted
		// inside a comment is skipped rather than mistaken for the directive.
		size_t pos = FindTypeDirective(source, 0, typeTokenLength);
		while (pos != std::string::npos)
		{
			// The #type token must be followed by a line ending. Asserted below --
			// a #type with nothing after it is a syntax error worth failing on.
			size_t eol = source.find_first_of("\r\n", pos);
			WL_CORE_ASSERT(eol != std::string::npos, "Syntax error");
			// The +1 skips exactly one character past the token -- the space or tab
			// that is required between "#type" and the stage name. It is a hardcoded
			// single character: "#type  vertex" (two spaces) would leave " vertex" as
			// the type string and fail the lookup below.
			size_t begin = pos + typeTokenLength + 1;
			std::string type = source.substr(begin, eol - begin);
			// Validation only. Note it does not break or return -- in a Release build
			// the assert vanishes and ShaderTypeFromString is called a second time at
			// line ~101, returning 0 for a bad type. The map then gains a key of 0 and
			// glCreateShader(0) fails, so the error surfaces as a confusing GL failure
			// rather than a clear message.
			WL_CORE_ASSERT(ShaderTypeFromString(type), "Invalid shader type specified!");

			// Skips the whole run of line endings, not just one, so a CRLF file with a
			// stray blank line does not produce a source string starting mid-break.
			size_t nextLinePos = source.find_first_not_of("\r\n", eol);
			// Anchored, for the same reason as the initial search: the stage body
			// ends at the NEXT real directive, not at the first mention of the
			// token anywhere in the text. A plain find() here matched a comment
			// that referenced "#type" and cut the stage short before main().
			pos = FindTypeDirective(source, nextLinePos, typeTokenLength);
			// The substring runs from the first character after the #type line to the
			// NEXT #type token -- i.e. the whole body of the current stage. That is
			// why the search for the next token happens before the substr.
			//
			// DEFECT (off-by-one on the last stage, documented not fixed): when the
			// next token does not exist, pos is npos and the length is computed as
			// source.size() - 1, silently dropping the final character of the last
			// stage. In GLSL a dropped trailing character is usually harmless
			// whitespace, but if the file has no trailing newline the last token of
			// the last stage is lost.
			shaderSources[ShaderTypeFromString(type)] = source.substr(nextLinePos, pos - (nextLinePos == std::string::npos ? source.size() - 1 : nextLinePos));
		}

		return shaderSources;
	}

	// Takes the stage->source map and produces a linked program, assigning
	// m_RendererID. Shared by both constructors.
	void OpenGLShader::Compile(const std::unordered_map<GLenum, std::string> &shaderSources)
	{
		WL_PROFILE_FUNCTION();

		// An empty map means PreProcess found no usable #type directive -- usually
		// because the file could not be read, or because its directive was quoted
		// inside a comment. Linking the resulting program SUCCEEDS, because GL
		// permits a program with no stages, and the only symptom is that every
		// glGetUniformLocation returns -1 afterwards. glUniform against -1 is a
		// documented no-op, so uniforms silently keep their defaults and the
		// renderer produces a black viewport with no error anywhere.
		//
		// That is exactly how a stray comment mentioning "#type" blacked out the
		// ray-traced viewport, so this now fails loudly at the point of the mistake
		// rather than silently downstream.
		if (shaderSources.empty())
		{
			WL_CORE_ERROR("OpenGLShader: no shader stages were found; the source had no usable "
						  "'#type' directive at the start of a line (a '#type' quoted inside a "
						  "comment does not count). The program would link with no stages and "
						  "every uniform lookup would return -1.");
			WL_CORE_ASSERT(false, "Shader has no stages!");
			m_RendererID = 0;
			return;
		}

		// Created up front, before any stage compiles, so the attach below has
		// somewhere to attach to.
		GLuint program = glCreateProgram();
		// DEFECT (fixed-size container vs variable stages, documented not fixed):
		// a std::array of 2 assumes a vertex+fragment pair, but the container
		// iterated below is a map of arbitrary size and the type token
		// supports "compute". A third stage writes glShaderIDs[2] -- out of
		// bounds -- which is undefined behaviour and, with the default
		// iterator-debug level in MSVC, an assert. A compute shader loaded
		// from a file would trip this. The array is also never zero-initialised,
		// so the detach loop at the end iterates uninitialised values when a
		// file contains fewer than two stages.
		std::array<GLenum, 2> glShaderIDs;
		// Manual cursor into that array, because operator[] on an array cannot
		// append.
		int glShaderIDIndex = 0;
		// Non-const reference because GLenum type = kv.first is copied out below;
		// the map itself is not modified, so a const ref would do.
		for (auto &kv : shaderSources)
		{
			GLenum type = kv.first;
			const std::string &source = kv.second;

			GLuint shader = glCreateShader(type);

			const GLchar *sourceCStr = source.c_str();
			// One string, no null-terminator length (the 0), so GL relies on c_str()'s
			// NUL. Fine for std::string, would truncate embedded NULs.
			glShaderSource(shader, 1, &sourceCStr, 0);

			glCompileShader(shader);

			GLint isCompiled = 0;
			glGetShaderiv(shader, GL_COMPILE_STATUS, &isCompiled);
			// Failure is checked per stage so the info log can name the offending
			// stage, rather than only being discovered at link time.
			if (isCompiled == GL_FALSE)
			{
				// Zero-initialised before the query. GL_INFO_LOG_LENGTH includes the
				// terminator, which is why the vector is sized to maxLength rather than
				// maxLength - 1.
				GLint maxLength = 0;
				glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &maxLength);

				// Sized to the query result rather than a fixed buffer: driver log lengths
				// vary and are often longer than the obvious default.
				std::vector<GLchar> infoLog(maxLength);
				glGetShaderInfoLog(shader, maxLength, &maxLength, &infoLog[0]);

				// Deleted before the assert so a Debug build that trips on the line below
				// has not leaked a GL object. The stage was never attached, so there is
				// nothing to detach.
				glDeleteShader(shader);

				WL_CORE_ERROR("{0}", infoLog.data());
				// Asserts, then breaks out of the loop below. In Release both are skipped:
				// the break is INSIDE this if-block, so a Release build falls through,
				// attaches a shader that failed to compile, and continues to
				// glLinkProgram on a program with a broken stage. The link will
				// fail and be reported, but the original compile error is already
				// gone by then.
				WL_CORE_ASSERT(false, "Shader compilation failure!");
				break;
			}

			// Reached only when the stage compiled -- the break above skips the rest
			// of the loop body for a failed stage.
			glAttachShader(program, shader);
			glShaderIDs[glShaderIDIndex++] = shader;
		}

		// Assigned BEFORE the link is checked, so a failed link still leaves a
		// valid handle behind (the program was deleted further down, so it is a
		// deleted name -- m_RendererID then refers to a destroyed object and any
		// later Bind() is a use-after-delete). Only reachable in Release, since
		// the Debug path asserts above.
		m_RendererID = program;

		// Link our program
		glLinkProgram(program);

		// Note the different functions here: glGetProgram* instead of glGetShader*.
		GLint isLinked = 0;
		// The C-style cast here is wrong in form but harmless in layout: GLint is
		// a 32-bit signed int on every GL implementation, so *(int*)&isLinked
		// aliases the same four bytes as the *(GLint*)& the signature wants.
		// A reinterpret_cast would be the correct spelling.
		glGetProgramiv(program, GL_LINK_STATUS, (int *)&isLinked);
		if (isLinked == GL_FALSE)
		{
			GLint maxLength = 0;
			glGetProgramiv(program, GL_INFO_LOG_LENGTH, &maxLength);

			// The maxLength includes the NULL character
			// Same "maxLength includes the NULL" sizing as the compile path above.
			std::vector<GLchar> infoLog(maxLength);
			glGetProgramInfoLog(program, maxLength, &maxLength, &infoLog[0]);

			// We don't need the program anymore.
			// The program is destroyed, but m_RendererID still names it -- see the
			// note at that assignment.
			glDeleteProgram(program);

			// DEFECT (deletes uninitialised values, documented not fixed): this loop
			// always visits exactly two elements. If compilation broke early or the
			// file had fewer than two stages, the untouched slots are
			// uninitialised GLenum values passed to glDeleteShader -- a GL error
			// and, depending on the driver, a deletion of an arbitrary name. The
			// loop should be bounded by glShaderIDIndex instead.
			// Detach after a successful link: the program holds everything it needs,
			// so the stage objects can be released immediately. Same
			// uninitialised-slot issue as the loop above applies here on the
			// success path when fewer than two stages were compiled.
			for (auto id : glShaderIDs)
				glDeleteShader(id);

			WL_CORE_ERROR("{0}", infoLog.data());
			WL_CORE_ASSERT(false, "Shader link failure!");
			return;
		}

		for (auto id : glShaderIDs)
			glDetachShader(program, id);
	}

	// Const: binding is a global GL state change, not a change to this shader,
	// so the method cannot mutate it.
	void OpenGLShader::Bind() const
	{
		WL_PROFILE_FUNCTION();

		glUseProgram(m_RendererID);
	}

	// Unbind is glUseProgram(0) -- there is no "unbind this one" in GL, so
	// the zero is a reset rather than a symmetric operation.
	void OpenGLShader::Unbind() const
	{
		WL_PROFILE_FUNCTION();

		glUseProgram(0);
	}

	// The Set*/UploadUniform* split: Set* are the Shader interface, the
	// Upload* are the backend-specific implementations. Each Set* forwards
	// unchanged, which is why they exist at all -- so a caller works against one
	// interface regardless of backend.
	void OpenGLShader::SetInt(const std::string &name, int value)
	{
		WL_PROFILE_FUNCTION();

		UploadUniformInt(name, value);
	}

	// The one Set* that takes no profile macro and a raw int* rather than a
	// std::vector: the array length is passed separately as count, so the caller
	// owns the storage for exactly the call.
	void OpenGLShader::SetIntArray(const std::string &name, int *values, uint32_t count)
	{
		UploadUniformIntArray(name, values, count);
	}

	void OpenGLShader::SetFloat(const std::string &name, float value)
	{
		WL_PROFILE_FUNCTION();

		UploadUniformFloat(name, value);
	}

	void OpenGLShader::SetFloat2(const std::string &name, const glm::vec2 &value)
	{
		WL_PROFILE_FUNCTION();

		UploadUniformFloat2(name, value);
	}

	void OpenGLShader::SetFloat3(const std::string &name, const glm::vec3 &value)
	{
		WL_PROFILE_FUNCTION();

		UploadUniformFloat3(name, value);
	}

	void OpenGLShader::SetFloat4(const std::string &name, const glm::vec4 &value)
	{
		WL_PROFILE_FUNCTION();

		UploadUniformFloat4(name, value);
	}

	// No SetMat3 public counterpart above even though UploadUniformMat3
	// exists below -- appears to be an omission in the interface rather than a
	// deliberate choice; mat3 uploads are unreachable from Shader's public API.
	void OpenGLShader::SetMat4(const std::string &name, const glm::mat4 &value)
	{
		WL_PROFILE_FUNCTION();

		UploadUniformMat4(name, value);
	}

	// Caches lookups because glGetUniformLocation is a string compare inside
	// the driver and this is called for every uniform of every draw call.
	int32_t OpenGLShader::GetUniformLocation(const std::string &name)
	{
		// Linear hash lookup by string first, driver call only on a miss.
		auto it = m_UniformLocationCache.find(name);
		// Note that a location of -1 (uniform not found / optimised out) is a
		// legitimate cached value and is returned as-is. That is correct: passing
		// -1 to glUniform* is a documented no-op, so an inactive uniform costs
		// nothing.
		if (it != m_UniformLocationCache.end())
			return it->second;

		GLint location = glGetUniformLocation(m_RendererID, name.c_str());
		// operator[] both tests and inserts. Also the cache's lifetime: the map
		// is a member, so it dies with the shader -- and it is never invalidated
		// because a GL program's uniform locations are fixed once linked.
		m_UniformLocationCache[name] = location;
		return location;
	}

	void OpenGLShader::UploadUniformInt(const std::string &name, int value)
	{
		glUniform1i(GetUniformLocation(name), value);
	}

	// count is a uint32_t passed to a GLsizei parameter -- a narrowing
	// conversion that is harmless for any real uniform array.
	void OpenGLShader::UploadUniformIntArray(const std::string &name, int *values, uint32_t count)
	{
		glUniform1iv(GetUniformLocation(name), count, values);
	}

	void OpenGLShader::UploadUniformFloat(const std::string &name, float value)
	{
		glUniform1f(GetUniformLocation(name), value);
	}

	void OpenGLShader::UploadUniformFloat2(const std::string &name, const glm::vec2 &values)
	{
		glUniform2f(GetUniformLocation(name), values.x, values.y);
	}

	void OpenGLShader::UploadUniformFloat3(const std::string &name, const glm::vec3 &values)
	{
		glUniform3f(GetUniformLocation(name), values.x, values.y, values.z);
	}

	void OpenGLShader::UploadUniformFloat4(const std::string &name, const glm::vec4 &values)
	{
		glUniform4f(GetUniformLocation(name), values.x, values.y, values.z, values.w);
	}

	void OpenGLShader::UploadUniformMat3(const std::string &name, const glm::mat3 &matrix)
	{
		// count=1, transpose=GL_FALSE. GL_FALSE means the matrix is supplied in
		// column-major order, which is what glm stores -- so no transpose is needed
		// and passing GL_TRUE here (the OpenGL-tutorial default when coming from
		// row-major math) would silently produce a wrong transform.
		glUniformMatrix3fv(GetUniformLocation(name), 1, GL_FALSE, glm::value_ptr(matrix));
	}

	void OpenGLShader::UploadUniformMat4(const std::string &name, const glm::mat4 &matrix)
	{
		glUniformMatrix4fv(GetUniformLocation(name), 1, GL_FALSE, glm::value_ptr(matrix));
	}

}