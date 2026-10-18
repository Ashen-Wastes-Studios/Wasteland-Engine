// PCH first: it supplies the base Shader interface, <cstring> for the strlen
// below, and the engine logging macros this file uses on every failure path.
#include "wlpch.h"
// Its own header: the three nvrhi::ShaderHandle members, the constant buffers
// and the mutable uniform staging vectors this file fills.
#include "NVRHIShader.h"
// Needed for context->GetAPI() in the constructor's .glsl/.hlsl fallback.
#include "NVRHIContext.h"
// For the static s_CurrentContext, which is the only route from here to the
// device, and for NVRHIRendererAPI in Bind().
#include "NVRHIRendererAPI.h"
// WL_CORE_ERROR / WL_CORE_WARN. Every compile and reflection failure is
// reported here rather than thrown: a shader failing to load must not take
// down the renderer.
#include "Wasteland/Core/Log.h"
// RenderCommand::GetRendererAPI(). Bind() reaches back through it to tell the
// renderer which shader is current -- an inversion worth knowing about.
#include "Wasteland/Renderer/RenderCommand.h"

// ReadFile: the whole shader source is slurped into a std::string, then handed
// to the in-process compiler as one blob.
#include <fstream>
// Included but not obviously used in this file; the preprocessing here is
// hand-rolled string scanning, not stream formatting.
#include <sstream>
// D3DCompile and D3DReflect. This is the HLSL compile path -- note it is a
// WINDOWS-ONLY toolchain, and it compiles HLSL regardless of which NVRHI
// backend is active (see the vs_5_0/ps_5_0 profile strings below).
#include <d3dcompiler.h>
// ID3D11ShaderReflection and friends, used to discover constant-buffer layout.
#include <d3d11shader.h>
// Same reflection interfaces, reachable under the D3D12 names. Included so the
// D3D11 spelling compiles whichever SDK headers the project resolved.
#include <d3d12shader.h>

// Links the DirectX shader compiler. MSVC-only: on another toolchain this
// pragma is ignored and the import library must come from the build system.
#pragma comment(lib, "d3dcompiler.lib")

// One namespace for all three NVRHI resource wrappers. Shader, texture and buffer
// share the namespace because they all resolve the device the same way.
namespace Wasteland {

	// File-local static, so it has internal linkage and cannot collide with another
	// translation unit's helper of the same name.
	// Helper to get the current NVRHI device
	// Every shader operation needs a device: compile, create buffers, create
	// layouts. This funnels the static-context lookup into one place.
	static nvrhi::IDevice* GetDevice()
	{
		// The static, NOT a member. A Shader has no renderer back-pointer, so this is
		// how it finds the device at construction time.
		auto* context = NVRHIRendererAPI::GetCurrentContext();
		// Null-safe: a shader loaded before the window exists gets nullptr and every
		// caller below turns that into a logged error rather than a crash.
		return context ? context->GetDevice() : nullptr;
	}

	// Loads from disk and compiles. The other ctor takes source directly, for
	// generated or in-memory shaders.
	NVRHIShader::NVRHIShader(const std::string& filepath)
	{
		// The name is used as NVRHI's debugName and in every log line, so it is derived
		// once here rather than carrying the full path around.
		// Extract name from filepath
		// Both separators: content may arrive with either. This is a Windows-first tool.
		auto lastSlash = filepath.find_last_of("/\\");
		// A path with no directory starts at 0, so the whole filename is the name.
		// Note this REUSES lastSlash as the start offset rather than keeping the
		// separator index -- so lastSlash is no longer a separator position below.
		lastSlash = lastSlash == std::string::npos ? 0 : lastSlash + 1;
		// The extension separator. rfind, not find, so a dotted directory name does not
		// truncate the name early.
		auto lastDot = filepath.rfind('.');
		// DEFECT: if the last dot appears BEFORE the last slash (a directory named
		// "shaders.v2" holding "test.glsl"), lastDot < lastSlash and the subtraction
		// underflows to a huge size_t. substr then clamps to the end of the string, so
		// the name silently becomes "test.glsl" instead of "test" -- wrong, but not
		// a crash.
		auto count = lastDot == std::string::npos ? filepath.size() - lastSlash : lastDot - lastSlash;
		// Stores the stem only. m_Name is the key used in reflection maps and log text.
		m_Name = filepath.substr(lastSlash, count);

		// A mutable copy, because the .glsl -> .hlsl substitution below rewrites it.
		std::string path = filepath;
		// Re-fetched here rather than reusing GetDevice()'s lookup, because this branch
		// needs the API enum and GetDevice() discards it.
		auto* context = NVRHIRendererAPI::GetCurrentContext();
		// Gated to non-Vulkan: on Vulkan the .glsl file is the real source, so swapping
		// it for a same-named .hlsl would be wrong.
		if (context && context->GetAPI() != RendererAPI::API::NVRHI_Vulkan)
		{
			// Second rfind on the (possibly identical) path. Computed even though the name
			// extraction already found one -- the two are independent concerns.
			auto dot = path.rfind('.');
			// Exact lowercase match: ".GLSL" or ".glsl.gz" would not match and would be
			// compiled as GLSL, which the D3DCompile path below cannot do.
			if (dot != std::string::npos && path.substr(dot) == ".glsl")
			{
				// Same stem, different extension. The convention is that a .glsl placeholder
				// sits beside the HLSL a developer actually edits.
				std::string hlslPath = path.substr(0, dot) + ".hlsl";
				// Probed for existence, not read. Opening is the cheapest existence test and
				// the stream is discarded immediately.
				std::ifstream f(hlslPath);
				// Only substitutes when the .hlsl actually exists, so a GLSL-only asset still
				// loads. The failure mode without this check is a confusing "cannot open".
				if (f.good())
				{
					// From here on, `path` is the file that will be read. The original filepath is
					// unchanged and still used for the error message in the failure branch below.
					path = hlslPath;
				}
			}
		}

		// The entire file as one string. Returns empty on failure -- there is no
		// separate success flag, so an unreadable file is detected by the empty result.
		std::string source = ReadFile(path);
		// Splits a multi-stage file on #type directives. Returns a map keyed by stage
		// name; an empty map means the file was malformed (and has already logged).
		auto shaderSources = PreProcess(source);

		// Graph stages require BOTH vertex and pixel. A file with only one of them is
		// treated as unrecognised rather than compiled half-way.
		if (shaderSources.find("vertex") != shaderSources.end() && 
		    shaderSources.find("pixel") != shaderSources.end())
		{
			// operator[] on a key just verified to exist, so this cannot insert a default.
			Compile(shaderSources["vertex"], shaderSources["pixel"]);
		}
		// Compute is checked second and is exclusive: a file with a compute stage goes
		// down the compute path even if it also declared graph stages.
		else if (shaderSources.find("compute") != shaderSources.end())
		{
			// A different function because the pipeline is different: no input layout, no
			// vertex stage, no pixel stage.
			CompileCompute(shaderSources["compute"]);
		}
		else
		{
			// Uses `filepath`, not `path`, so the error names what the caller asked for
			// rather than the substituted .hlsl.
			WL_CORE_ERROR("Shader file '{0}' does not contain valid shader types", filepath);
		}
	}

	// Compiles from source already in memory. No preprocessing: the caller has
	// already split the stages, which is the point of this overload.
	NVRHIShader::NVRHIShader(const std::string& name, const std::string& vertexSrc, const std::string& fragmentSrc)
		// m_Name initialised in the init list rather than assigned, so a compile failure
		// still leaves the shader identifiable in logs.
		: m_Name(name)
	{
		// No null-device check here -- Compile() does its own, so a shader constructed
		// before the window exists logs once and leaves the handles null.
		Compile(vertexSrc, fragmentSrc);
	}

	// Explicitly clears the three shader handles. nvrhi handles are ref-counted, so
	// assigning null releases the shader only when this was the last reference.
	NVRHIShader::~NVRHIShader()
	{
		// Cleared alongside the graph stages even though they are mutually exclusive.
		// The three are separate members, and nothing enforces the exclusivity.
		m_ComputeShader = nullptr;
		// Order is irrelevant -- no destructor below depends on another still holding a
		// value.
		m_PixelShader = nullptr;
		// The constant buffers and m_BindingLayout are NOT cleared here. They are
		// ref-counted too, so they release with the object; doing it explicitly would
		// be redundant.
		m_VertexShader = nullptr;
	}

	// Whole-file read. No size limit and no streaming: shader sources are small and
	// the compiler wants one contiguous buffer anyway.
	std::string NVRHIShader::ReadFile(const std::string& filepath)
	{
		// Returned by value and populated only on success. An empty string is the
		// error signal -- the constructor relies on that to detect failure.
		std::string result;
		// Binary, not text: on Windows a text-mode read would translate CRLF and corrupt
		// preprocessor line structure the hand-rolled scanner below depends on.
		std::ifstream in(filepath, std::ios::in | std::ios::binary);
		// The stream's own bool conversion tests whether the open succeeded.
		if (in)
		{
			// Two seeks rather than a stat(): avoids a second filesystem call and a race
			// between measuring and opening.
			in.seekg(0, std::ios::end);
			// tellg returns a streampos, which converts to the size_type resize wants.
			// A tellg of -1 (error) would wrap to SIZE_MAX and throw -- unreachable here
			// because the stream tested good and the seek succeeded.
			result.resize(in.tellg());
			in.seekg(0, std::ios::beg);
			// &result[0] is the contiguous buffer C++11 guarantees for non-const strings;
			// data() would be more idiomatic but this predates that habit.
			in.read(&result[0], result.size());
			// Explicit, though the destructor would do it. Harmless and conventional here.
			in.close();
		}
		else
		{
			// The only diagnostic for an unreadable shader. The caller then gets an empty
			// string and reports "does not contain valid shader types" as well -- so an
			// unreadable file produces TWO error lines, the first being the accurate one.
			WL_CORE_ERROR("Could not open file '{0}'", filepath);
		}
		// Empty on failure, fully populated on success. No error code.
		return result;
	}

	// Splits one file into per-stage sources. The format is a sequence of
	// `#type <name>` lines, each followed by that stage's body, with no nested
	// `#type` allowed inside a body.
	std::unordered_map<std::string, std::string> NVRHIShader::PreProcess(const std::string& source)
	{
		// Key is the stage name as written in the file. The constructor then looks up
		// the literal strings "vertex", "pixel" and "compute".
		std::unordered_map<std::string, std::string> shaderSources;

		// The directive that separates stages. Hard-coded, so a shader written with
		// `#pragma` or a different spelling is not recognised.
		const char* typeToken = "#type";
		// strlen rather than sizeof-1: the token is a char*, so its length is not
		// known at compile time.
		size_t typeTokenLength = strlen(typeToken);
		// Starting the scan at 0 means a file with NO #type at all yields npos, an empty
		// map, and the constructor's "no valid shader types" error.
		size_t pos = source.find(typeToken, 0);
		// One iteration per stage. The loop variable `pos` is reused as both the search
		// origin and the body boundary, which is why the ordering below matters.
		while (pos != std::string::npos)
		{
			// The end of the directive line. Both CR and LF are accepted so the scanner is
			// line-ending agnostic.
			size_t eol = source.find_first_of("\r\n", pos);
			// A #type on the last line with no newline after it. This is a malformed file,
			// so bailing with an empty map is right -- there is no body to capture.
			if (eol == std::string::npos)
			{
				WL_CORE_ERROR("Shader type directive has no body in {0}", m_Name);
				return {};
			}
			// The +1 skips exactly ONE character after #type, assuming a single space.
			// DEFECT: `#type` followed by a tab, or by several spaces, makes `type` include
			// the leading whitespace and every lookup in the constructor misses.
			size_t begin = pos + typeTokenLength + 1;
			// The stage name, untrimmed and un-lowercased -- hence the sensitivity above.
			// This is the string that becomes the map key and the constructor's lookup key.
			std::string type = source.substr(begin, eol - begin);

			// Skips the blank lines between the directive and its body, so a #type followed
			// by an empty line still finds its stage.
			size_t nextLinePos = source.find_first_not_of("\r\n", eol);
			// Directive at end of file with only whitespace after it. Malformed, and again
			// reported as an empty map.
			if (nextLinePos == std::string::npos)
			{
				WL_CORE_ERROR("Empty shader stage in {0}", m_Name);
				return {};
			}
			// Advances to the NEXT stage before capturing this one, so `end` below is the
			// start of the following stage rather than the end of the file.
			pos = source.find(typeToken, nextLinePos);
			// The last stage runs to end-of-file. Subtract is safe because pos >= nextLinePos.
			// This is what lets a single-stage file parse without a trailing directive.
			const size_t end = pos == std::string::npos ? source.size() : pos;
			// DEFECT: assignment, not insert. A file with two `#type vertex` blocks keeps
			// only the SECOND body -- silently discarding the first rather than reporting
			// a duplicate. Using a different key per stage would make the collision visible.
			// Also note the body retains its trailing newline(s) up to the next #type.
			shaderSources[type] = source.substr(nextLinePos, end - nextLinePos);
		}

		// Empty on any malformed input, and every failure path has already logged.
		return shaderSources;
	}

	// Two-stage compile. Each stage is compiled, created and reflected separately,
	// then the constant buffers and binding layout are built from what reflection
	// found.
	void NVRHIShader::Compile(const std::string& vertexSrc, const std::string& fragmentSrc)
	{
		// The device is needed for createShader and createBuffer, but NOT for D3DCompile
		// -- the D3D compiler is a CPU-side library. So the check below would also
		// reject compilation on a machine with no GPU, which is arguably wrong.
		nvrhi::IDevice* device = GetDevice();
		// Logs and returns, leaving all three handles null. The shader object still
		// exists and is safe to hold; any Bind/Draw against it fails downstream.
		if (!device)
		{
			WL_CORE_ERROR("NVRHIShader: No NVRHI device available");
			return;
		}

		// Stage order is fixed: vertex, then pixel. The order matters only for which
		// error is reported first when both are broken.
		// Compile vertex shader
		// Zero-initialised so the FAILED(hr) path can test errorBlob without it being
		// set by D3DCompile.
		ID3DBlob* vertexBlob = nullptr;
		// On success this may STILL be non-null: the compiler emits warnings through the
		// same channel. That is why the success path checks it too.
		ID3DBlob* errorBlob = nullptr;
		// In-process compilation. No include paths are passed (nullptr), so #include in
		// a shader would fail unless the source is already self-contained.
		HRESULT hr = D3DCompile(
			// The LENGTH is passed, not just the pointer, so an embedded NUL would be
			// handled correctly. c_str() also guarantees the null terminator D3DCompile wants.
			vertexSrc.c_str(), vertexSrc.length(),
			// Source name for diagnostics is the shader's stem, and the two nulls are the
			// include/environment path and the pPreprocessorArguments -- so no includes.
			m_Name.c_str(), nullptr, nullptr,
			// Entry point and profile are both hard-coded. `main` is the convention this
			// engine's HLSL uses. `vs_5_0` is Shader Model 5.0 (Direct3D 11).
			"main", "vs_5_0",
			// Strictness on, optimisation level 0 = DEFAULT. Neither flag is configurable,
			// so a shader needing higher optimisation cannot ask for it.
			D3DCOMPILE_ENABLE_STRICTNESS, 0,
			&vertexBlob, &errorBlob
		);

		// HRESULT check, not an exception: D3DCompile reports through hr and errorBlob.
		// There is no throw path anywhere in this function.
		if (FAILED(hr))
		{
			if (errorBlob)
			{
				// Logs the compiler's own message, which includes the line and column. Far more
				// useful than "compile failed".
				WL_CORE_ERROR("Vertex shader compilation failed:\n{0}",
					(char*)errorBlob->GetBufferPointer());
				// COM reference released. Leaking this would keep the error text alive for the
				// process lifetime -- harmless but wrong.
				errorBlob->Release();
			}
			return;
		}

		// SUCCESS with diagnostics: the shader compiled, but the compiler had something
		// to say. Failing here instead would reject working shaders.
		if (errorBlob)
		{
			// WARN because the bytecode is usable. The distinction between this and the
			// ERROR above is the whole reason both branches exist.
			WL_CORE_WARN("Vertex shader compiler diagnostics for {0}:\n{1}", m_Name, (char*)errorBlob->GetBufferPointer());
			errorBlob->Release();
			// Nulled after release so a later Release cannot double-free. Not strictly
			// needed -- errorBlob is reassigned before the pixel compile -- but it makes
			// the state explicit.
			errorBlob = nullptr;
		}

		// Two fields set; the rest take NVRHI defaults. debugName below is what shows
		// up in a GPU debugger.
		nvrhi::ShaderDesc vsDesc;
		// Required. There is no inference from the bytecode.
		vsDesc.shaderType = nvrhi::ShaderType::Vertex;
		// Suffix distinguishes the two stages of one shader in a capture.
		vsDesc.debugName = m_Name + "_VS";
		// NVRHI takes the DXBC blob directly on the D3D backends. DEFECT: on Vulkan the
		// backend expects SPIR-V, so this path cannot produce a usable shader there --
		// which the constructor's .glsl/.hlsl switch implicitly acknowledges.
		m_VertexShader = device->createShader(vsDesc, vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize());
		
		// Reflection runs BEFORE the blob is released, which is the whole reason the
		// comment at the top of ReflectShaderConstants exists.
		// Reflect vertex shader constants
		// `true` selects the VS constant buffer. Reflection fills m_UniformLocations,
		// which is what every SetUniform call looks names up in.
		ReflectShaderConstants(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), true);

		// Compile pixel shader
		// Second blob, separate from the vertex one. Both are still alive: the vertex
		// blob must survive until after the layout is built.
		ID3DBlob* pixelBlob = nullptr;
		errorBlob = nullptr;
		// Reuses the HRESULT variable rather than declaring a new one.
		hr = D3DCompile(
			// The pixel stage's source, passed in the same way as the vertex stage's.
			fragmentSrc.c_str(), fragmentSrc.length(),
			m_Name.c_str(), nullptr, nullptr,
			// ps_5_0, matching vs_5_0. Both are Shader Model 5.0, so a shader cannot mix
			// a VS4/PS4 pair or target SM6 features.
			"main", "ps_5_0",
			D3DCOMPILE_ENABLE_STRICTNESS, 0,
			&pixelBlob, &errorBlob
		);

		if (FAILED(hr))
		{
			if (errorBlob)
			{
				WL_CORE_ERROR("Pixel shader compilation failed:\n{0}",
					(char*)errorBlob->GetBufferPointer());
				errorBlob->Release();
			}
			// DEFECT (asymmetric cleanup): released on the pixel-failure path and at the end,
			// but NOT if `device->createShader` for the vertex stage failed -- there is no
			// check on that call's result at all, so a failed VS creation falls through and
			// this Release still happens, leaking nothing but leaving m_VertexShader null.
			vertexBlob->Release();
			return;
		}

		if (errorBlob)
		{
			WL_CORE_WARN("Pixel shader compiler diagnostics for {0}:\n{1}", m_Name, (char*)errorBlob->GetBufferPointer());
			errorBlob->Release();
			errorBlob = nullptr;
		}

		// Same two-field desc as the vertex stage.
		nvrhi::ShaderDesc psDesc;
		// Fragment stage in NVRHI's vocabulary.
		psDesc.shaderType = nvrhi::ShaderType::Pixel;
		// "_PS" suffix, matching the "_VS" above.
		psDesc.debugName = m_Name + "_PS";
		// Second createShader. Like the VS one, its result is never null-checked, so a
		// driver rejection leaves m_PixelShader null with no diagnostic at all.
		m_PixelShader = device->createShader(psDesc, pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize());
		
		// Second reflection pass, this time into the PS staging buffer.
		// Reflect pixel shader constants
		// `false` selects m_PSConstantData. Both passes must complete before
		// CreateConstantBuffers below, which sizes the GPU buffers from what they found.
		ReflectShaderConstants(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), false);

		// Only reached when BOTH stages compiled. A half-compiled shader therefore has
		// no constant buffers, which the binding-set builder accounts for.
		// Create constant buffers
		// Also calls CreateBindingLayout, so the whole GPU-side setup is one hop from here.
		CreateConstantBuffers();

		vertexBlob->Release();
		// Both blobs released before returning. Safe now because reflection has already
		// copied out everything it needs.
		pixelBlob->Release();
	}

	// Single-stage compile. Structurally parallel to Compile() but without input
	// layouts, raster state or a pixel stage.
	void NVRHIShader::CompileCompute(const std::string& computeSrc)
	{
		// Same lookup as Compile(). Computeshaders need a device to create the stage,
		// not to compile.
		nvrhi::IDevice* device = GetDevice();
		// Identical guard and message as the graph path. A compute shader loaded too
		// early fails the same way.
		if (!device)
		{
			WL_CORE_ERROR("NVRHIShader: No NVRHI device available");
			return;
		}

		// One blob, since there is only one stage.
		ID3DBlob* computeBlob = nullptr;
		ID3DBlob* errorBlob = nullptr;
		// Same call shape as the graph stages, differing only in the profile string.
		HRESULT hr = D3DCompile(
			computeSrc.c_str(), computeSrc.length(),
			m_Name.c_str(), nullptr, nullptr,
			// cs_5_0: the compute profile at Shader Model 5.0. NVRHI's ComputePipeline
			// consumes the resulting DXBC on the D3D backends.
			"main", "cs_5_0",
			D3DCOMPILE_ENABLE_STRICTNESS, 0,
			&computeBlob, &errorBlob
		);

		if (FAILED(hr))
		{
			if (errorBlob)
			{
				// Trailing space after the comma is cosmetic. The message itself matches the
				// graph-stage errors so a log reader sees one consistent format.
				WL_CORE_ERROR("Compute shader compilation failed:\n{0}", 
					(char*)errorBlob->GetBufferPointer());
				errorBlob->Release();
			}
			return;
		}

		if (errorBlob)
		{
			// Same warn-on-success-diagnostics handling as the graph stages.
			WL_CORE_WARN("Compute shader compiler diagnostics for {0}:\n{1}", m_Name, (char*)errorBlob->GetBufferPointer());
			errorBlob->Release();
			errorBlob = nullptr;
		}

		// Single desc for the one stage.
		nvrhi::ShaderDesc csDesc;
		// The third nvrhi::ShaderType. Distinguishes this from the graph stages at the
		// NVRHI boundary.
		csDesc.shaderType = nvrhi::ShaderType::Compute;
		// "_CS" suffix, consistent with _VS and _PS.
		csDesc.debugName = m_Name + "_CS";
		// DEFECT: no reflection follows, and no CreateConstantBuffers call. A compute
		// shader therefore gets NO constant buffer, NO uniform locations and NO binding
		// layout -- so every SetFloat on it logs "Uniform not found" and any dispatch
		// that expects bindings has nothing to bind.
		m_ComputeShader = device->createShader(csDesc, computeBlob->GetBufferPointer(), computeBlob->GetBufferSize());
		// Released immediately, with nothing deferred that needs it.
		computeBlob->Release();
	}

	// Sizes two GPU buffers from what reflection discovered. Called once, right
	// after Compile() has reflected both stages.
	void NVRHIShader::CreateConstantBuffers()
	{
		nvrhi::IDevice* device = GetDevice();
		// Silent return, unlike Compile's error. By the time this runs the device was
		// already validated, so this is defensive only.
		if (!device)
			return;

		// Create vertex shader constant buffer
		// Conditional: a stage that declared no constants produces an empty vector, and
		// creating a zero-byte constant buffer is not valid.
		if (!m_VSConstantData.empty())
		{
			// Fresh desc per stage; only three fields are set.
			nvrhi::BufferDesc desc;
			// Sized to the reflected buffer's declared size, not the sum of the variables'
			// sizes -- which is what preserves cbuffer padding.
			desc.byteSize = m_VSConstantData.size();
			// Tells the driver this is a cbuffer, which affects binding and visibility rules.
			desc.isConstantBuffer = true;
			// "_VS_CB" distinguishes it in a capture from the "_PS_CB" sibling.
			desc.debugName = m_Name + "_VS_CB";
			// NVRHI buffer, ref-counted, owned by this shader until it is destroyed.
			m_VSConstantBuffer = device->createBuffer(desc);
			
			// DEFECT: marks the buffer dirty with NO DATA WRITTEN. The upload in
			// UploadConstants will push whatever is in m_VSConstantData -- which at this
			// point is zero-filled, because CreateConstantBuffers never copies reflection's
			// layout into it. Harmless today only because every shader sets its uniforms
			// before the first draw.
			m_VSConstantsDirty = true;
		}

		// Create pixel shader constant buffer
		// Same conditional for the pixel stage, which may legitimately have no
		// constants where the vertex stage does.
		if (!m_PSConstantData.empty())
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = m_PSConstantData.size();
			desc.isConstantBuffer = true;
			// The same desc object is reused; only debugName differs between the two stages.
			desc.debugName = m_Name + "_PS_CB";
			m_PSConstantBuffer = device->createBuffer(desc);
			
			// Same deliberate initial dirty flag, with the same caveat as the VS one above.
			m_PSConstantsDirty = true;
		}

		// The layout is derived from the buffers just created, so it must follow them.
		// Create binding layout
		// Not inlined here because the layout is also the thing the renderer reads via
		// GetBindingLayout() when it builds a binding set.
		CreateBindingLayout();
	}

	// Describes the binding slots this shader's shaders expect. Must match what
	// NVRHIRendererAPI::GetOrCreateBindingSet fills in, or createBindingSet fails.
	void NVRHIShader::CreateBindingLayout()
	{
		nvrhi::IDevice* device = GetDevice();
		if (!device)
			return;

		// Accumulator for the three item types below: two cbuffers, one texture array,
		// one sampler.
		nvrhi::BindingLayoutDesc layoutDesc;
		// DEFECT: Compute is NOT in the visibility mask, and the texture/sampler items
		// added unconditionally below are declared Vertex|Pixel only. A compute shader
		// built from this layout could not legally read those bindings.
		layoutDesc.setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel);

		// Section marker kept from the original: the next two blocks are the cbuffers.
		// Add constant buffer bindings
		// Only declare a slot for a buffer that exists, so the layout matches the
		// binding set the renderer will build from the same condition.
		if (m_VSConstantBuffer)
		{
			// Slot 0 is a CONTRACT: NVRHIRendererAPI::GetOrCreateBindingSet hard-codes
			// BindingSetItem::ConstantBuffer(0, vsCB) to match.
			// VS constant buffer at slot 0
			layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
		}

		if (m_PSConstantBuffer)
		{
			// Slot 1 likewise hard-coded on the renderer side as ConstantBuffer(1, psCB).
			// PS constant buffer at slot 1
			layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1));
		}

		// Renderer2D and Renderer3D_Basic share a 32-entry texture array at t0
		// and one sampler at s0. Keeping this in the common NVRHI layout makes
		// both paths use the same binding contract.
		// A 32-element descriptor array at t0, matching the SetActiveTextures loop's
		// `setArrayElement(i)`. Unconditional, so the layout is identical for every
		// material -- which is what lets the renderer cache binding sets by shader alone.
		layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0).setSize(32));
		// Exactly ONE sampler, matching the renderer's `if (i == 0)` rule that binds
		// only element 0's sampler.
		layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));

		// The desc is complete; NVRHI turns it into a device object.
		// Create binding layout
		// Not null-checked. A layout the driver rejects leaves m_BindingLayout null, and
		// GetOrCreateBindingSet then returns nullptr for every call -- a null that is
		// indistinguishable from the legitimate "no layout" case.
		m_BindingLayout = device->createBindingLayout(layoutDesc);
	}

	// Discovers constant-buffer layout from compiled bytecode. Called once per
	// stage, BEFORE the blob is released.
	void NVRHIShader::ReflectShaderConstants(const void* bytecode, size_t bytecodeSize, bool isVertexShader)
	{
		// D3D11 NVRHI retains bytecode only for VS input layouts, not PS.
		// Reflect the compiler blob before releasing it, for every stage.

		// Guards the D3DReflect call below, which would fail on a null pointer anyway.
		if (!bytecode || bytecodeSize == 0)
			return;

		// Create reflection interface
		// COM interface from D3DReflect. Must be Released at the end -- the early return
		// above and the one below both handle that.
		ID3D11ShaderReflection* reflection = nullptr;
		// IID-based QueryInterface-style creation. The (void**) cast is the COM idiom;
		// it is safe because reflection was initialised to null.
		HRESULT hr = D3DReflect(bytecode, bytecodeSize, IID_ID3D11ShaderReflection, (void**)&reflection);
		// Both conditions: the call can fail, and can theoretically succeed while
		// leaving the pointer null.
		if (FAILED(hr) || !reflection)
		{
			// WARN, not ERROR. The shader BYTECODE is still usable -- only constant discovery
			// failed -- so failing the whole load here would be too aggressive.
			WL_CORE_WARN("Failed to create shader reflection for {0}", m_Name);
			return;
		}

		// Get constant buffer description
		// Uninitialised struct filled by GetDesc. No default member initialisers exist
		// on these D3D types, so reading an unset field would be undefined.
		D3D11_SHADER_DESC shaderDesc;
		// Fills the aggregate counts below. The HRESULT is discarded, which is the
		// documented pattern for these accessors.
		reflection->GetDesc(&shaderDesc);

		// Iterate through constant buffers
		// One iteration per cbuffer. Almost always exactly one -- see the resize defect
		// noted at the body below.
		for (UINT i = 0; i < shaderDesc.ConstantBuffers; i++)
		{
			// Borrowed pointer owned by `reflection`; valid only until the Release at the end.
			ID3D11ShaderReflectionConstantBuffer* cb = reflection->GetConstantBufferByIndex(i);
			D3D11_SHADER_BUFFER_DESC cbDesc;
			cb->GetDesc(&cbDesc);

			// Determine which constant buffer this is (we assume one CB per shader stage)
			// A REFERENCE to one of the two mutable staging vectors, chosen by the flag.
			// Everything below writes through it, so neither stage can mix up the other.
			std::vector<uint8_t>& constantData = isVertexShader ? m_VSConstantData : m_PSConstantData;
			// DEFECT: inside the loop, so a shader with TWO constant buffers resizes twice --
			// the second resize to a smaller Size would DISCARD the first buffer's data.
			// Harmless only because this engine's HLSL uses a single cbuffer per stage, as
			// the comment above states. Hoisting the resize out of the loop would be the fix.
			constantData.resize(cbDesc.Size);

			// Iterate through variables in the constant buffer
			// One iteration per uniform. The loop body records name -> offset/size only; it
			// writes NO initial values, which is why the buffers stay zero-filled.
			for (UINT j = 0; j < cbDesc.Variables; j++)
			{
				// Borrowed, same lifetime as `cb`.
				ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(j);
				D3D11_SHADER_VARIABLE_DESC varDesc;
				var->GetDesc(&varDesc);

				// Store uniform location
				// Three uint32/bool fields, all set below. No default initialisers in the struct,
				// so an unset field here would be indeterminate.
				UniformInfo info;
				// Byte offset within the constant buffer, INCLUDING compiler padding. memcpy in
				// SetUniformData writes to this exact offset, which is why it must be the
				// reflected offset and not a packed one.
				info.offset = varDesc.StartOffset;
				// The variable's size. SetUniformData refuses a write larger than this, which is
				// what catches a caller passing a vec4 for a float uniform.
				info.size = varDesc.Size;
				// Which staging buffer this name lives in. Stored per entry so
				// SetUniformData can pick the right vector without re-reflecting.
				info.isVS = isVertexShader;
				// APPEND rather than assign. A name declared in BOTH stage
				// cbuffers yields two UniformInfo records with different
				// offsets; push_back keeps both. The old
				// `m_UniformLocations[name] = info` overwrote the vertex entry
				// with the pixel one (the PS pass always reflects second), so
				// the VS copy of a dual-declared uniform was unreachable and
				// SetFloat on it updated only the PS buffer.
				m_UniformLocations[varDesc.Name].push_back(info);
			}
		}

		// COM release. Every path that acquired `reflection` reaches this or an early
		// return before it, so there is no leak.
		reflection->Release();
	}

	// const because the base-class Shader interface declares it const. The
	// const_cast below is the price of that -- see the note there.
	void NVRHIShader::Bind() const
	{
		// NVRHI has no global current-shader binding; the pipeline is built from
		// whatever this tells the renderer.
		// Track this shader as the current shader for NVRHI pipeline creation
		// The global renderer, reached through a static. A layering inversion -- the
		// shader reaches up to the renderer rather than the renderer pulling the shader.
		RendererAPI* api = RenderCommand::GetRendererAPI();
		// dynamic_cast, not static: the renderer may be a different backend, in which
		// case there is nothing to tell and Bind silently does nothing.
		auto* nvrhiAPI = dynamic_cast<NVRHIRendererAPI*>(api);
		// Null is the normal case on other backends, so this is a capability check
		// rather than an error path.
		if (nvrhiAPI)
		{
			// and will remain valid during the draw call
			// The comment asserts a lifetime property the code cannot enforce: nothing here
			// prevents the caller destroying the Shader before the next DrawIndexed.
			// Create a Ref from 'this' - safe because the shader is owned by the caller
			// and will remain valid during the draw call
			// A NON-OWNING Ref: the no-op deleter means the renderer's m_CurrentShader
			// holds a reference that will never free anything and never keeps the shader
			// alive. If the shader is destroyed between Bind and Draw, m_CurrentShader
			// dangles and DrawIndexed dynamic_casts it -- a use-after-free the comment above
			// dismisses as "safe".
			nvrhiAPI->SetCurrentShader(Ref<Shader>(const_cast<NVRHIShader*>(this), [](NVRHIShader*){}));
		}

	}

	// Pushes dirty constant buffers into the command stream. Called from the
	// renderer immediately before each draw, not from Bind.
	void NVRHIShader::UploadConstants(nvrhi::ICommandList* cmd) const
	{
		// Explains the ordering: NVRHI has no state block, so a uniform set after Bind
		// must still reach the GPU before the draw that uses it.
		// Setters normally run AFTER Bind; upload immediately before drawing.
		// Null-tolerant so a caller can invoke this before a command list exists.
		// The upload is simply skipped rather than erroring.
		if (cmd)
		{
			
			// Three conditions, all needed: the flag says something changed, the buffer must
			// exist to write to, and there must be data. Together they make the common
			// unchanged case a single bool test.
			if (m_VSConstantsDirty && m_VSConstantBuffer && !m_VSConstantData.empty())
			{
				// The WHOLE buffer, not just the changed uniform. Correct but not incremental:
				// a shader with a large cbuffer re-uploads all of it whenever one float changes.
				cmd->writeBuffer(m_VSConstantBuffer, m_VSConstantData.data(), m_VSConstantData.size());
				// Cleared AFTER the write is recorded, not before. If the write were to fail the
				// flag would already be false and the data would never be retried.
				m_VSConstantsDirty = false;
			}

			// The pixel stage's twin of the condition above.
			if (m_PSConstantsDirty && m_PSConstantBuffer && !m_PSConstantData.empty())
			{
				cmd->writeBuffer(m_PSConstantBuffer, m_PSConstantData.data(), m_PSConstantData.size());
				m_PSConstantsDirty = false;
			}
		}
	}

	void NVRHIShader::Unbind() const
	{
		// Unbind needs no work: the state lives in GraphicsState, which is rebuilt per
		// draw. Leaving m_CurrentShader set is harmless for the same reason.
		// No-op in NVRHI
	}

	// The single path every typed setter (SetInt, SetFloat4, SetMat4, ...) funnels
	// through. const because the staging vectors are `mutable`.
	void NVRHIShader::SetUniformData(const std::string& name, const void* data, size_t size) const
	{
		// Lookup by the exact reflected name. There is no aliasing, prefix matching or
		// case folding, so a typo produces a warning rather than a silent no-op.
		auto it = m_UniformLocations.find(name);
		if (it == m_UniformLocations.end())
		{
			// WARN and return. Not an error: a caller may set uniforms conditionally, and a
			// shader optimised away a uniform that reflection never saw.
			WL_CORE_WARN("Uniform '{0}' not found in shader '{1}'", name, m_Name);
			return;
		}

		// One write PER STAGE that declares this name. A name present in only
		// one stage has a single entry and behaves exactly as before; a name
		// declared in both cbuffers now reaches both, which is what lets a
		// shader read the same uniform from either stage off one SetFloat call.
		// Each entry is bounds-checked on its own buffer, so a size mismatch in
		// one stage is reported and skipped without preventing the other stage
		// from receiving the value.
		for (const UniformInfo& info : it->second)
		{
			std::vector<uint8_t>& constantData = info.isVS ? m_VSConstantData : m_PSConstantData;
			// A reference to one of the two mutable flags, so the write below sets the
			// right one without a second branch.
			bool& dirtyFlag = info.isVS ? m_VSConstantsDirty : m_PSConstantsDirty;

			// Two bounds checks. The first rejects a caller writing more than the uniform
			// holds (a vec4 into a float). The second rejects an offset+size past the end.
			// DEFECT: the setters do not CHECK their return value, so these warnings are
			// easy to miss and a mistyped uniform silently does nothing.
			if (size > info.size || info.offset + size > constantData.size())
			{
				// ERROR rather than WARN, because reaching here means the caller passed a
				// mismatched size, which is a programming error rather than a shader quirk.
				WL_CORE_ERROR("Uniform '{0}' data exceeds constant buffer size", name);
				continue;
			}

			// Writes at the REFLECTED offset, so compiler-inserted padding is respected.
			// Not vector::assign, because the target region is an interior slice.
			memcpy(constantData.data() + info.offset, data, size);
			// Marks the buffer for upload on the next draw. This is the whole mechanism:
			// setters only touch CPU memory, and nothing reaches the GPU without this.
			dirtyFlag = true;
		}
	}

	// Trampoline. Every typed setter below is two lines for exactly this reason:
	// one place does the lookup, the bounds check and the memcpy.
	void NVRHIShader::SetInt(const std::string& name, int value)
	{
		// &value is the address of the parameter, valid for the call's duration. The
		// memcpy copies out before SetUniformData returns.
		SetUniformData(name, &value, sizeof(int));
	}

	// The one setter that can legitimately exceed a single uniform's size.
	void NVRHIShader::SetIntArray(const std::string& name, int* values, uint32_t count)
	{
		// DEFECT: count * sizeof(int) will exceed `info.size` for any array uniform
		// declared as e.g. `int values[4]`, because D3D reflection reports Size as the
		// WHOLE array there but this check compares against the same value -- so an
		// array setter works only when reflection happens to report the full size.
		SetUniformData(name, values, count * sizeof(int));
	}

	// Same trampoline shape. The float overloads below differ only in sizeof.
	void NVRHIShader::SetFloat(const std::string& name, float value)
	{
		// 4 bytes into the reflected offset of the named float.
		SetUniformData(name, &value, sizeof(float));
	}

	// glm::vec2 is passed by const reference, so no copy at the call boundary.
	void NVRHIShader::SetFloat2(const std::string& name, const glm::vec2& value)
	{
		// 8 bytes. glm's default layout is tightly packed, so this matches HLSL float2.
		SetUniformData(name, &value, sizeof(glm::vec2));
	}

	// A vec3 in HLSL is 12 bytes but frequently occupies a 16-byte slot; reflection
	// reports the true offset, so the memcpy lands correctly regardless.
	void NVRHIShader::SetFloat3(const std::string& name, const glm::vec3& value)
	{
		// 12 bytes, matching HLSL's float3. GLM's padding is disabled by default.
		SetUniformData(name, &value, sizeof(glm::vec3));
	}

	// The most common setter in practice -- colours and transforms.
	void NVRHIShader::SetFloat4(const std::string& name, const glm::vec4& value)
	{
		// 16 bytes, matching HLSL float4 exactly.
		SetUniformData(name, &value, sizeof(glm::vec4));
	}

	// A matrix is passed column-major by GLM, which is what HLSL expects with its
	// default row_major setting, so no transpose is needed.
	void NVRHIShader::SetMat4(const std::string& name, const glm::mat4& value)
	{
		// 64 bytes: four columns of four floats. The largest single write the setters
		// can make, and the one most likely to trip the bounds check above.
		SetUniformData(name, &value, sizeof(glm::mat4));
	}

	// A backend-specific identity used as a cache key by the engine's resource
	// system. This implementation returns a truncated pointer.
	uint32_t NVRHIShader::GetRendererID() const
	{
		// Return hash of vertex shader pointer (or pixel if no vertex)
		// Prefers VS, falls back to PS, then CS. The first non-null handle identifies
		// the shader well enough for cache-keying.
		void* ptr = m_VertexShader ? m_VertexShader.Get() : 
		            (m_PixelShader ? m_PixelShader.Get() : 
		            (m_ComputeShader ? m_ComputeShader.Get() : nullptr));
		// DEFECT: truncates a 64-bit address to 32 bits. On a 64-bit process two shaders
		// whose handles differ only in the upper 32 bits collide, so a cache keyed on
		// this could return the wrong shader's data. A hash of the full pointer, or the
		// name, would not.
		return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr));
	}

}
