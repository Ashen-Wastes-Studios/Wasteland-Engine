// NVRHIRendererAPI.h -- the NVRHI backend's implementation of the abstract
// RendererAPI, covering DX11, DX12 and Vulkan behind one class.
//
// The three backends are not separate classes: the API enum is stored in m_API
// and the differences (device creation, readback path, present) are branched on
// inside the .cpp. RenderCommand::SetAPI constructs this same class for all
// three enumerators.
//
// Architecturally the most involved class in the renderer. Where OpenGL's
// implementation is a dozen immediate GL calls, this one maintains three caches
// of GPU objects, builds an nvrhi::GraphicsState per draw, and routes commands
// through the NVRHIContext's command list rather than calling the driver
// directly. The .cpp carries the detailed reasoning; this header is the shape.
#pragma once

// The abstract base, which supplies RendererAPI::API (stored in m_API below) and
// the pure virtuals implemented here.
//
// Note the base's own header forward-declares RenderCommand, which is why this
// header can name RendererAPI::API without a cycle.
#include "Wasteland/Renderer/RendererAPI.h"
// For Ref<Shader>, used by SetCurrentShader, DispatchCompute and the two
// GetOrCreate* helpers. The concrete NVRHIShader is NOT included here -- it is
// forward-declared, because the header only needs the abstract type and each
// .cpp function does its own dynamic_pointer_cast.
#include "Wasteland/Renderer/Shader.h"
// The vendor API. Needed at header scope because the cache members below store
// nvrhi::GraphicsPipelineHandle, ComputePipelineHandle, BindingSetHandle and
// FramebufferInfo -- all nvrhi types.
//
// Unlike RendererAPI.h, which knows nothing of NVRHI, this header is a platform
// header and is expected to.
#include <nvrhi/nvrhi.h>
// The backing store for all three caches below.
#include <unordered_map>

namespace Wasteland {

	// Forward declaration. NVRHIContext owns the nvrhi::IDevice and the command
	// list, and both are needed pervasively here -- but including its header would
	// be a cycle, since it reaches back to this class for GetCurrentContext().
	class NVRHIContext;
	// Also forward-declared. Only ever used as Ref<VertexArray> (a pointer plus a
	// deleter), which does not require the complete type -- the deleter is
	// instantiated at the point of construction, in the .cpp.
	class VertexArray;

	// The concrete backend.
	//
	// DEFECT (missing `virtual`/destructor hygiene, minor): the destructor below
	// IS marked virtual and override, so deletion through RendererAPI* is fine.
	// Worth stating because the base's own dtor is what makes that safe, and this
	// class holds three unordered_maps of GPU handles that must be released before
	// the device dies.
	class NVRHIRendererAPI : public RendererAPI
	{
	public:
		// Takes the API enum rather than a NVRHIContext, because the context does not
		// exist yet at this point -- RenderCommand::SetAPI news the renderer and the
		// context is injected afterwards via SetContext.
		//
		// The constructor is called for all three NVRHI enumerators (see
		// RenderCommand.cpp), so the three collapse into one runtime value carried in
		// m_API and branched on per operation.
		NVRHIRendererAPI(RendererAPI::API api);
		// Non-trivial: clears the graphics pipeline cache so its nvrhi handles release
		// while the device is still alive. A member's destructor would run the same
		// way, but doing it here makes the ordering explicit and lets the compute cache
		// be handled differently -- see the DEFECT noted at that member.
		//
		// `virtual ... override` is redundant (override implies virtual) but harmless.
		virtual ~NVRHIRendererAPI() override;

		virtual void Init() override;
		// A NO-OP that only traces its arguments. This is the single biggest behavioural
		// difference from OpenGL, where the same call does glViewport.
		//
		// NVRHI has no global viewport: it lives in nvrhi::GraphicsState, which
		// DrawIndexed rebuilds for every draw from the BOUND FRAMEBUFFER's real size.
		// So the arguments are ignored entirely, including x and y -- nvrhi::Viewport
		// has no origin at all.
		//
		// Consequence worth knowing: a caller that sets a viewport and expects it to
		// persist gets correct behaviour under OpenGL and silently ignored values here.
		// The engine gets away with it because DrawIndexed derives the viewport from
		// the render target, which is what an editor viewport actually wants.
		//
		// The width/height parameters are read only so the TRACE line is honest about
		// what was asked.
		virtual void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;
		virtual void SetClearColor(const glm::vec4& color) override;
		virtual void Clear() override;

		// The workhorse: ~180 lines in the .cpp, and the only path that actually
		// rasterises geometry on this backend.
		//
		// indexCount is in INDICES. The default 0 means "use the whole index buffer",
		// implemented by substituting nvrhiIB->GetCount(). This is the CORRECT reading
		// of the contract -- OpenGLRendererAPI::DrawIndexed inverts the same expression,
		// so partial draws work here and not there.
		//
		// Takes the abstract VertexArray and downcasts to NVRHIVertexArray internally,
		// erroring rather than trusting the caller. Two further downcasts follow, to
		// the concrete vertex and index buffer types.
		//
		// Batched by default: commands are recorded onto the context's open command
		// list rather than submitted immediately. Only when no list is open does it
		// fall back to a transient open/record/close/execute.
		virtual void DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t indexCount = 0) override;
		// UNIMPLEMENTED. The .cpp only logs WL_CORE_WARN and returns, so both
		// parameters are unused and line geometry never appears under DX11/DX12/Vulkan.
		// Only OpenGL draws lines (glDrawArrays).
		//
		// WARN rather than ERROR, deliberately: line drawing is diagnostic, and an
		// error per frame would flood a log the user cannot act on.
		//
		// Kept in the interface rather than removed so a caller written against
		// RendererAPI compiles on both backends and degrades to a log line.
		virtual void DrawLines(const Ref<VertexArray>& vertexArray, uint32_t vertexCount) override;
		// A no-op that traces the value. Real on OpenGL (glLineWidth).
		//
		// Not fixable as written: DirectX has no variable line width at all, and
		// Vulkan requires the wideLines device feature, which this engine does not
		// request. The value is echoed rather than dropped so a caller can confirm the
		// call reached the backend when a line is missing.
		virtual void SetLineWidth(float width) override;
		// The one base-class hook this backend actually implements (the base declares
		// it with an empty body as optional).
		//
		// Loads the engine's 32-slot texture array, and -- this is the important part --
		// CLEARS m_BindingSetCache. The cache is keyed by shader pointer alone, so it
		// cannot detect a texture change itself; without this invalidation a swapped
		// texture would keep drawing the previous one.
		//
		// Copies the vector, so the caller may free its own immediately; the Refs inside
		// keep the textures alive.
		virtual void SetActiveTextures(const std::vector<Ref<Texture2D>>& textures) override;

		// The only compute capability exposed, and it has no counterpart in the abstract
		// RendererAPI -- see the note on DispatchCompute below.
		// Compute dispatch support
		// NOT AN OVERRIDE. RendererAPI declares no compute method, so this exists only on
		// the concrete class and is unreachable through a RendererAPI* or a
		// RenderCommand::DispatchCompute (there is none). A caller must name
		// NVRHIRendererAPI directly and downcast to reach it.
		//
		// The three counts are WORKGROUPS in X/Y/Z, not threads -- the per-group thread
		// count comes from the shader's own [numthreads(numthreads(x,y,z))].
		//
		// Shaders are identified by their compute stage: a shader without one is
		// rejected with an error naming that as the reason.
		//
		// Pipeline creation is cached by shader pointer in m_ComputePipelineCache --
		// keyed on nothing else, because a compute pipeline has no framebuffer or
		// vertex-layout dependency.
		void DispatchCompute(const Ref<Shader>& shader, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ);

		// NVRHI-specific methods
		// The per-instance accessor, returning the member rather than the static.
		// Prefer GetCurrentContext() below unless you specifically need THIS renderer's
		// context -- with more than one renderer alive they can differ.
		//
		// Raw pointer, not Ref: the context belongs to the window system and outlives
		// the renderer. Shared ownership would be a false claim about the lifetime.
		// Inline because it is a bare member read.
		NVRHIContext* GetContext() const { return m_Context; }
		// Injects the context after construction. Exists because the dependency runs
		// both ways: the renderer needs the context's device, and the context (through
		// s_CurrentContext below) is reached from resources that have no back-pointer
		// to the renderer.
		//
		// Sets BOTH the member and the static, LAST-WRITER-WINS. With two renderers
		// alive (the editor may run one per viewport) the second SetContext wins, and
		// resources created afterwards bind to it -- an ordering dependency rather
		// than a bug, but one that must be respected when creating two views.
		//
		// Never clears s_CurrentContext back to nullptr on destruction, so after the
		// last renderer dies the static dangles.
		void SetContext(NVRHIContext* context);

		// Static accessor for current context (used by NVRHI resource implementations)
		// THE accessor for everything else in the platform layer. NVRHIFramebuffer,
		// NVRHIBuffer, NVRHIShader and NVRHITexture all call this to reach the device,
		// because they are constructed by engine-side factories (Framebuffer::Create,
		// Shader::Create) that know nothing about which renderer instance is live.
		//
		// Static and inline: called from many translation units, and the body is a
		// single load.
		//
		// Returns null before any SetContext, which callers handle -- NVRHIFramebuffer
		// logs an error and bails; NVRHIShader logs "No NVRHI device available" and
		// leaves its handles null. So the null case is a supported state, not a crash.
		//
		// DEFECT: no way to query validity without dereferencing, and no reset on
		// teardown, so a use after the last renderer is destroyed reads a dangling
		// pointer rather than returning null.
		static NVRHIContext* GetCurrentContext() { return s_CurrentContext; }

		// Track current shader for pipeline creation
		// Records the shader subsequent draws should use. This backend has no equivalent
		// of glUseProgram: the shader's NVRHIShader::Bind() stores the Ref, and
		// DrawIndexed reads m_CurrentShader to fetch or build a pipeline and a binding
		// set.
		//
		// Assigning a Ref, not a raw pointer -- so the shader cannot be destroyed while
		// it is current. ClearCachedResources() resets it to nullptr.
		//
		// `shader` is a const Ref& and copied in; passing a temporary is fine.
		void SetCurrentShader(const Ref<Shader>& shader) { m_CurrentShader = shader; }

		// Resource cleanup
		// Drops all three caches so they can be rebuilt: after a resize, a device loss,
		// or a backend switch. The nvrhi handles release their references as the maps
		// drop, which must happen while the device is alive.
		//
		// Also resets m_CurrentShader to nullptr, so a draw cannot reach a pipeline built
		// against a shader whose binding layout has gone.
		//
		// More complete than the destructor, which clears only the graphics pipeline
		// cache -- see the DEFECT at that member.
		void ClearCachedResources();

	private:
		// Three-part key for the graphics pipeline cache, and the reasoning is worth
		// reading before changing it.
		//
		// The key is deliberately made of POINTERS, not content: creating a graphics
		// pipeline is a driver call plus shader-variant work, far too expensive to
		// repeat per draw, and the shader and vertex array objects are long-lived, so
		// address identity is both cheap and sufficient within one session.
		// Pipeline cache key (use pointer addresses)
		// A plain aggregate -- no user-declared constructors -- so it is usable as an
		// unordered_map key and can be brace-initialised. All three members are
		// set explicitly at the call site in GetOrCreatePipeline.
		struct PipelineKey
		{
			const void* shaderPtr;
			const void* vertexArrayPtr;
			// The third key component, and the one that is easy to miss: the same shader
			// needs a DIFFERENT pipeline for an MSAA editor target than for the swapchain,
			// because blend and depth state are written unconditionally at creation time.
			//
			// FramebufferInfo is a value type (format + sample count), so this compares by
			// content -- unlike the two pointers above. Two distinct framebuffer OBJECTS
			// with identical info therefore share one cached pipeline, which is correct
			// and desirable.
			//
			// DEFECT (partial initialisation): in GetOrCreatePipeline this member is only
			// assigned when a framebuffer is bound. With no framebuffer the key is left
			// INDETERMINATE, and it is then used in a lookup. In practice the function
			// errors out on the null-framebuffer path before the lookup matters, but the
			// ordering makes that the only thing preventing a garbage comparison.
			nvrhi::FramebufferInfo framebufferInfo;
			
			// The equality predicate unordered_map requires. All three components must
			// match -- and the && short-circuit means framebufferInfo is only compared when
			// both pointers already agree.
			//
			// `const` and by const reference, per the unordered_map contract.
			//
			// Note there is no operator!= -- C++20 would generate it from this, but the
			// project is presumably on C++17, so any code needing != must write !(a == b).
			bool operator==(const PipelineKey& other) const
			{
				return shaderPtr == other.shaderPtr && vertexArrayPtr == other.vertexArrayPtr
					&& framebufferInfo == other.framebufferInfo;
			}
		};

		// The hasher, as a separate functor rather than a member of PipelineKey. That
		// keeps the key a POD aggregate and follows the usual unordered_map practice.
		struct PipelineKeyHash
		{
			size_t operator()(const PipelineKey& key) const
			{
				// Combines two pointer hashes with XOR after shifting one left by one bit.
				// The shift is what keeps the two from cancelling: hashing (A,B) and (B,A)
				// would collide under a plain XOR, and the shift makes them differ.
				//
				// DEFECT: framebufferInfo is NOT hashed. Two keys differing only in framebuffer
				// -- the same shader and vertex array, one MSAA target and one not -- produce
				// the SAME hash but are NOT equal. That is legal (a hash collision) and
				// unordered_map handles it by comparing keys with the == above, so lookups stay
				// correct; the cost is a bucket with several entries where one would do. With
				// a single framebuffer in play it never bites.
				//
				// The XOR combination is also weak by construction: nothing here mixes in a
				// third independent value, so pointer alignment patterns could cluster. Not a
				// correctness problem, only a distribution one.
				return std::hash<const void*>()(key.shaderPtr) ^ (std::hash<const void*>()(key.vertexArrayPtr) << 1);
			}
		};

		// The cache lookup-or-build for graphics pipelines. Returns a HANDLE, never
		// null on the success path: every failure returns early, and DrawIndexed
		// checks and logs.
		//
		// Takes the ABSTRACT Shader and VertexArray, then dynamic_pointer_casts both to
		// NVRHIShader and NVRHIVertexArray. The input layout comes from the latter's
		// GetInputLayout(), which is why the vertex array pointer is part of the key.
		//
		// Private: nothing outside this class builds pipelines, which is what keeps the
		// cache coherent.
		nvrhi::GraphicsPipelineHandle GetOrCreatePipeline(const Ref<Shader>& shader, const Ref<VertexArray>& vertexArray);
		// The binding-set equivalent, but keyed on the SHADER POINTER ALONE. That is
		// only correct because SetActiveTextures() clears the whole cache whenever the
		// texture list changes -- the shader cannot detect it.
		//
		// Returns nullptr quietly on a failed cast (there is nothing to report from a
		// setter-like accessor), and DrawIndexed's `if (bindingSet)` then leaves
		// state.bindings empty. That is legal NVRHI but means "bind nothing", which
		// for a shader expecting t0 is undefined output -- a shader with no textures
		// draws unshaded rather than erroring. That is a real, documented behaviour of
		// this path.
		//
		// DEFECT: a FAILED creation is also cached. The handle is inserted into
		// m_BindingSetCache whether or not createBindingSet succeeded, so a null set
		// is returned from the cache forever after with no retry and no second log.
		nvrhi::BindingSetHandle GetOrCreateBindingSet(const Ref<Shader>& shader);

		// Which of the three NVRHI backends this instance is. Set from the constructor
		// and read per operation to pick a code path -- DX11 versus DX12 versus Vulkan
		// for device creation, readback and present.
		//
		// No initialiser: the constructor's member-init list sets it. A default would
		// be API::None, which is not a valid NVRHI backend, so leaving it unset would
		// be silently wrong rather than obviously wrong.
		RendererAPI::API m_API;
		// Stored, not applied. Read in Clear(), so setting it mid-frame does not
		// retroactively change clears already recorded on the open command list.
		//
		// Initialised in the constructor's member-init list to a dark grey rather than
		// left default: the first Clear() may run before any SetClearColor(), and an
		// uninitialised vec4 would clear with whatever was on the stack.
		glm::vec4 m_ClearColor;
		// This renderer's own context, distinct from the static below. Null until
		// SetContext runs, which is why so many methods begin with a null check and log
		// an error rather than crashing.
		//
		// NSDMI to nullptr even though SetContext is the only way to set it -- cheap
		// insurance, and it makes the destructor safe if construction threw.
		//
		// Raw pointer: the context belongs to the window system and outlives the
		// renderer.
		NVRHIContext* m_Context = nullptr;
		// The shader in effect for subsequent draws, owned via Ref so it cannot be
		// destroyed mid-frame.
		//
		// Read by DrawIndexed to fetch the pipeline and binding set. A null here makes
		// DrawIndexed error out ("No shader bound"), so it is checked rather than
		// assumed.
		//
		// Reset to nullptr by ClearCachedResources().
		Ref<Shader> m_CurrentShader = nullptr;
		
		// The process-wide context, defined in the .cpp as nullptr.
		//
		// This is the one piece of genuinely global state in the renderer, and it
		// exists to break a dependency cycle: engine-side factories construct resources
		// without knowing which renderer is live, so the resources reach the device
		// through here.
		//
		// DEFECT: never reset. The destructor's cleanup is conditional on
		// `s_CurrentContext == m_Context`, and even that only nulls it if this instance
		// was the last writer. After the final renderer is destroyed the static still
		// holds a pointer to a context the window system is about to destroy, so a
		// resource constructed during teardown would dereference freed memory.
		// GetCurrentContext() has no validity check to catch it.
		static NVRHIContext* s_CurrentContext;

		// Of the three caches, the expensive one: a miss costs a driver call plus
		// shader-variant work, which is why it gets the three-part key.
		//
		// Value type is a HANDLE, so dropping the map releases one reference per entry
		// rather than destroying the pipeline -- other holders can keep using it.
		// Pipeline cache
		// The custom hasher is passed explicitly -- the standard one cannot hash a
		// PipelineKey, since it has no std::hash specialisation.
		//
		// No reserve(): a game session's pipeline count is small and bounded by
		// (shaders x vertex arrays x framebuffer formats), so rehashing during the
		// first frames is cheaper than pre-sizing for a guess.
		std::unordered_map<PipelineKey, nvrhi::GraphicsPipelineHandle, PipelineKeyHash> m_PipelineCache;

		// Compute pipeline cache
		// The compute equivalent, keyed on the bare shader pointer -- no struct needed,
		// because a compute pipeline depends on neither a framebuffer nor an input
		// layout.
		//
		// DEFECT: NOT cleared by the destructor, which clears only m_PipelineCache. The
		// inconsistency against ClearCachedResources() -- which clears all three -- is
		// benign only because the handles are ref-counted and the device releases
		// whatever the renderer drops. But it means the destructor's cleanup is
		// incomplete by inspection, and if NVRHI ever stopped ref-counting, this would
		// leak.
		//
		// Shader destruction is the other hazard: a destroyed shader's address can be
		// reused by a new one, and this cache -- keyed on address -- would return the
		// OLD pipeline. The same ABA hazard applies to the other two caches; long-lived
		// shaders make it unlikely in practice.
		std::unordered_map<const void*, nvrhi::ComputePipelineHandle> m_ComputePipelineCache;

		// Binding set cache
		// The third cache, keyed on shader pointer alone.
		//
		// Its correctness DEPENDS on an invariant maintained elsewhere:
		// SetActiveTextures() clears this map whenever the texture list changes, since
		// the key cannot express that. Miss that call site and a texture swap silently
		// keeps drawing the old texture -- no error, just wrong pixels.
		//
		// That coupling is invisible from this declaration; it is documented at both
		// SetActiveTextures and GetOrCreateBindingSet.
		std::unordered_map<const void*, nvrhi::BindingSetHandle> m_BindingSetCache;

		// The bound texture list, copied from SetActiveTextures. Read only by
		// GetOrCreateBindingSet, which packs the textures into the shader's shared
		// 32-element array.
		//
		// Holding Refs here keeps those textures alive between draws -- necessary,
		// because a caller may drop its own references after binding.
		//
		// Never cleared except by assignment in SetActiveTextures, so the last set stays
		// bound for the rest of the frame. That is the intent: Renderer2D and Renderer3D
		// set once and then draw many batches.
		std::vector<Ref<Texture2D>> m_ActiveTextures;
	};

}
