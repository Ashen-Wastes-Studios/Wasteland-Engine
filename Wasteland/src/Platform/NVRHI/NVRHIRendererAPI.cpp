// PCH first: it defines WL_PROFILE_FUNCTION, the WL_CORE_* logging macros and
// the <memory>/<glm> headers every other include here depends on.
// Including anything before it in this translation unit would be an ODR hazard.
#include "wlpch.h"
// This file's own header: the class declaration and the PipelineKey type.
#include "NVRHIRendererAPI.h"
// NVRHIContext is the only owner of the nvrhi::IDevice, so it is what every
// resource wrapper in this directory reaches the device through.
#include "NVRHIContext.h"
// Three NVRHI resource headers follow because this file downcasts Ref<Shader>,
// Ref<VertexArray> and Ref<Texture2D> to their concrete NVRHI types to reach
// the nvrhi:: handles the engine-agnostic interfaces do not expose.
#include "NVRHIShader.h"
#include "NVRHIVertexArray.h"
// NVRHIIndexBuffer / NVRHIVertexBuffer, whose GetBuffer()/GetCount() are used
// to assemble the draw's index and vertex bindings.
#include "NVRHIBuffer.h"
// NVRHITexture2D, the only way to reach a nvrhi::TextureHandle and its sampler
// from the Ref<Texture2D> the renderer is given.
#include "NVRHITexture.h"
// The WL_CORE_* family is the engine-wide logger. Every failure path in this
// file logs and returns rather than throwing: a renderer that throws mid-frame
// would unwind through the graphics driver.
#include "Wasteland/Core/Log.h"
// Every NVRHI wrapper lives in the Wasteland namespace but shares ONE global
// piece of state -- NVRHIRendererAPI::s_CurrentContext -- because the nvrhi
// device is created once by the window system and referenced from resource
// objects that have no back-pointer to the renderer.
namespace Wasteland
{

	// Null until SetContext runs. Shaders and buffers read this to find the device
	// during construction, so a resource loaded before the window exists silently
	// fails to compile rather than crashing -- see NVRHIShader::GetDevice().
	NVRHIContext *NVRHIRendererAPI::s_CurrentContext = nullptr;

	// m_ClearColor is initialised here, not left default: the first Clear() may run
	// before any SetClearColor() call, and an uninitialised vec4 would clear the
	// framebuffer with whatever was on the stack.
	NVRHIRendererAPI::NVRHIRendererAPI(RendererAPI::API api)
		// The 0.1 grey is a deliberate "nothing has drawn yet" colour: pure black reads
		// as a crashed render, dark grey reads as an empty viewport.
		: m_API(api), m_ClearColor(0.1f, 0.1f, 0.1f, 1.0f)
	{
	}

	// Caches hold nvrhi::GraphicsPipelineHandle / BindingSetHandle, which are
	// reference-counted on the NVRHI side. Dropping the maps here releases those
	// references while the device is still alive.
	NVRHIRendererAPI::~NVRHIRendererAPI()
	{
		// Only the GRAPHICS pipeline cache is cleared here. m_ComputePipelineCache is not,
		// which is an inconsistency against ClearCachedResources() below, which clears
		// all three. The omission is benign only because the handles are ref-counted.
		m_PipelineCache.clear();
		// Self-check rather than a blind null: a second NVRHIRendererAPI can exist (the
		// editor may run one per window/viewport), and tearing down the wrong one would
		// leave live renderers unable to find the device.
		if (s_CurrentContext == m_Context)
			// Cleared on teardown, but only if this renderer still owns the static slot --
			// see the guard above.
			s_CurrentContext = nullptr;
	}

	// Idempotent from the caller's perspective: it does not create the device, only
	// hands the externally-owned context its Init() and logs which backend is live.
	void NVRHIRendererAPI::Init()
	{
		// Scoped timer. Every public entry point carries one so a profile shows where
		// frame time goes without needing a sampling profiler attached.
		WL_PROFILE_FUNCTION();

		// The context is injected (see SetContext) rather than created here, which is
		// what lets one nvrhi::IDevice be shared by the game view and the editor view.
		// Context is set externally by the window system
		// Null-tolerant on purpose: Init() can be called on a headless/test instance
		// that never had a context, and that should not be a hard failure.
		if (m_Context)
		{
			// Hands control to the context, which is what actually creates the nvrhi device,
			// swapchain and command lists. This renderer never creates them itself.
			m_Context->Init();
		}

		// Plain std::string, not a fmt/log type: the switch below only needs a human
		// label, and the value is consumed immediately by the log call below.
		std::string apiName;
		// The switch is exhaustive over RendererAPI::API's NVRHI values, with `default`
		// as a catch-all so adding a backend later cannot silently log a wrong name.
		switch (m_API)
		{
		// The API enum records which backend was REQUESTED; NVRHI itself picks the
		// concrete device implementation from that, so this function never queries it.
		// DX11 is listed first and is the lowest enum value here; the order of the
		// cases is presentation only and has no effect on behaviour.
		case RendererAPI::API::NVRHI_DX11:
			apiName = "DirectX 11";
			break;
		// DX12 differs from DX11 in NVRHI only internally -- from this file's point of
		// view it is the same RendererAPI::API value and the same call sequence.
		case RendererAPI::API::NVRHI_DX12:
			apiName = "DirectX 12";
			break;
		// Vulkan is reached through the same nvrhi facade, which is the whole point of
		// routing every backend through NVRHI rather than three native renderers.
		case RendererAPI::API::NVRHI_Vulkan:
			apiName = "Vulkan";
			break;
		// Reached only if an API value outside this switch is constructed. Logging
		// "Unknown" beats logging a wrong name -- the caller sees the mismatch.
		default:
			// Only reachable for an API value this switch does not handle -- including a
			// non-NVRHI backend, if one were ever passed to this constructor.
			apiName = "Unknown";
			break;
		}

		// INFO, not WARN: an unknown backend is still a working renderer, just one the
		// user did not ask for. This line is the first thing to check in a bug report.
		WL_CORE_INFO("NVRHI Renderer API initialized: {0}", apiName);
	}

	// Sets BOTH the member and the static. The member is what this instance uses;
	// the static is how NVRHIShader/NVRHITexture reach the device without a back-
	// pointer to the renderer.
	void NVRHIRendererAPI::SetContext(NVRHIContext *context)
	{
		// Raw pointer, not Ref: the context outlives the renderer (it belongs to the
		// window system), so shared ownership would be a false claim.
		m_Context = context;
		// Last-writer-wins. With two renderers alive the second SetContext wins, and
		// resources built afterwards bind to it -- an ordering dependency, not a bug,
		// but it means SetContext order matters when creating two views.
		s_CurrentContext = context;
	}

	// Deliberately a no-op -- see the body. The base-class signature exists so a
	// caller can be backend-agnostic; on NVRHI the viewport is per-draw state.
	void NVRHIRendererAPI::SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
	{
		WL_PROFILE_FUNCTION();

		// NVRHI has no global viewport: it is baked into nvrhi::GraphicsState, which
		// DrawIndexed() builds per draw call from the framebuffer's real size.
		// Note: In NVRHI, viewport is set as part of GraphicsState before drawing
		// DrawIndexed() reads the size off the bound framebuffer, so an editor
		// framebuffer and the swapchain each get the correct viewport automatically.
		// This is handled in the Renderer implementation
		// The parameters are read only to keep the trace honest about what was asked.
		// For now, this is a no-op
		// TRACE compiles out in release, so this costs nothing in a shipping build
		// while remaining greppable in a debug log.
		WL_CORE_TRACE("SetViewport({0}, {1}, {2}, {3}) - will be set via GraphicsState", x, y, width, height);
	}

	// Stores only. The colour is read in Clear(), so setting it mid-frame does not
	// retroactively change clears already recorded on the open command list.
	void NVRHIRendererAPI::SetClearColor(const glm::vec4 &color)
	{
		WL_PROFILE_FUNCTION();
		// Copied by value: the caller may pass a temporary or a reference to something
		// it mutates next frame, and the clear must keep using what was set.
		m_ClearColor = color;
	}

	// The texture list is renderer state consumed by GetOrCreateBindingSet(), which
	// packs them into the shared t0[32] array.
	void NVRHIRendererAPI::SetActiveTextures(const std::vector<Ref<Texture2D>> &textures)
	{
		WL_PROFILE_FUNCTION();
		// Copied so the caller can free its vector immediately; the Ref inside keeps
		// each texture alive for as long as the list holds it.
		m_ActiveTextures = textures;
		// Mandatory: the cached binding set baked in the PREVIOUS textures. Without
		// this, swapping a texture would keep drawing the old one -- the cache is keyed
		// by shader pointer alone and cannot detect the texture change itself.
		m_BindingSetCache.clear();
	}

	// Records clears onto the command list; it does not submit them. In the batched
	// path (BeginFrame already called) ExecuteNVRHICommandList() closes and runs the
	// list, so the clear lands in the same submission as this frame's draws.
	void NVRHIRendererAPI::Clear()
	{
		WL_PROFILE_FUNCTION();

		// Every accessor below dereferences m_Context, so this guard is what makes the
		// function safe on a renderer that was never given one (headless, or post-teardown).
		if (!m_Context)
		{
			// Early-out rather than a silent no-op: a clear that silently does nothing
			// looks exactly like a frozen frame, which is far harder to diagnose than
			// an error in the log.
			WL_CORE_ERROR("Clear: No NVRHI context available");
			return;
		}

		// Borrowed, not owned: the context owns the list and recycles it per frame.
		nvrhi::ICommandList *cmd = m_Context->GetCommandList();
		// Second guard on the same principle: a context with no command list this frame
		// cannot record anything, and the caller expects a cleared target.
		if (!cmd)
		{
			WL_CORE_ERROR("Clear: No command list available");
			return;
		}

		// Default target is the swapchain's backbuffer, which is correct for a game view.
		nvrhi::TextureHandle backBuffer = m_Context->GetCurrentBackBuffer();
		// When an editor framebuffer is bound, clear its color attachment 0
		// instead of the swapchain backbuffer (that's the render target now).
		// An editor framebuffer bound as an override becomes the real render target,
		// so clearing the swapchain instead would clear a texture nobody is presenting.
		if (m_Context->HasRenderTargetOverride())
		{
			auto fb = m_Context->GetCurrentFramebuffer();
			// Both conditions needed: a null framebuffer, or one with zero colour
			// attachments (a depth-only target), leaves backBuffer as the swapchain so
			// the function still has something valid to clear.
			if (fb && !fb->getDesc().colorAttachments.empty())
				// Attachment 0 only. An MRT editor target's attachments 1..N are left stale,
				// which is fine for the current panels -- nothing here samples them.
				backBuffer = fb->getDesc().colorAttachments[0].texture;
		}
		// A framebuffer with zero colour attachments clears nothing. Bailing here is
		// better than calling clearTextureFloat on a null handle.
		if (!backBuffer)
		{
			WL_CORE_ERROR("Clear: No backbuffer available");
			return;
		}

	// Batched frame: BeginFrame() already opened the command list, ExecuteNVRHICommandList() will close+execute.
		// Do NOT open/close here — per-draw open/close overwrites m_ActiveCommandList and loses previous draws.
		// Depth is cleared separately because a framebuffer may have a depth attachment
		// while the colour target came from elsewhere. The lambda keeps that optional
		// step from being duplicated in both the batched and transient branches below.
		auto clearDepth = [&]()
		{
			// Re-fetched inside the lambda rather than captured, because the framebuffer can
			// be swapped between the lambda's definition and its two call sites.
			auto fb = m_Context->GetCurrentFramebuffer();
			if (fb && fb->getDesc().depthAttachment.texture)
				// reverse=false, stencil=0: this clears depth to the far plane with the compare
				// order left in the default LessOrEqual, matching pipelineDesc's depthFunc.
				cmd->clearDepthStencilTexture(fb->getDesc().depthAttachment.texture, nvrhi::AllSubresources, true, 1.0f, false, 0);
		};
		// Outside a batched frame (init-time clears, or a stray call from an editor
		// path). The fallback below opens/closes/executes immediately at the cost of
		// one extra submission.
		if (!m_Context->IsCommandListOpen())
		{
			// DEVICE-LOST GUARD. On a lost device IsCommandListOpen() is false
			// (BeginFrame skipped open()), so this transient path would otherwise
			// call open()/close()/executeCommandList on a REMOVED device -- the same
			// crash the device-lost guards in BeginFrame prevent. The transient path
			// exists for init-time and out-of-frame calls on a LIVE device, so the
			// right answer here is to skip the clear entirely.
			if (m_Context->IsDeviceLost())
			{
				WL_CORE_ERROR("Clear: Device lost, skipping clear");
				return;
			}
			WL_CORE_WARN("Clear: Command list not open (missing BeginFrame), recording with transient open/close");
			// Only reached on the transient path. Opening inside a batched frame would
			// reset the command list and discard every draw already recorded on it.
			cmd->open();
			clearDepth();
		// AllSubresources rather than a single mip: the backbuffer is mip 0 only, but
		// the same call form is reused for editor targets, which can be mip-chained.
		cmd->clearTextureFloat(backBuffer, nvrhi::AllSubresources,
								   // The components are listed explicitly rather than passing the vec4 straight
								   // through: nvrhi::Color is a distinct 4-float type, so this is an explicit
								   // conversion at the API boundary.
								   nvrhi::Color(m_ClearColor.r, m_ClearColor.g, m_ClearColor.b, m_ClearColor.a));
			// Closes the list opened two statements above. executeCommandList below
			// submits it immediately.
			cmd->close();
			// Transient: execute immediately since we are outside batched frame
			if (auto *dev = m_Context->GetDevice())
				// Transient path: submit now, because nothing else will close this list. The
				// null check on dev mirrors every other device use in this file.
				dev->executeCommandList(cmd);
			return;
		}
		// Batched twin of the depth clear in the transient branch. Called before the
		// colour clear so a failed colour clear cannot leave stale depth behind.
		clearDepth();
		// Batched twin of the transient clear: identical calls, but recorded onto the
		// list BeginFrame already opened, so ExecuteNVRHICommandList() submits them.
		cmd->clearTextureFloat(backBuffer, nvrhi::AllSubresources,
							   // Same explicit vec4 -> nvrhi::Color conversion as the transient branch.
							   nvrhi::Color(m_ClearColor.r, m_ClearColor.g, m_ClearColor.b, m_ClearColor.a));
	}

	// Cached on (shader, vertex array, framebuffer). The framebuffer is part of the
	// key because the blend/depth state below is written unconditionally here, so
	// the pipeline is specific to the target it was built against.
	nvrhi::GraphicsPipelineHandle NVRHIRendererAPI::GetOrCreatePipeline(const Ref<Shader> &shader, const Ref<VertexArray> &vertexArray)
	{
		// Value key, not a pointer: two Shader objects compiling to the same blob still
		// need different pipelines if their input layouts differ.
		PipelineKey key;
		// Raw identity, not a shader hash: the cache is per-renderer and the shader
		// objects are long-lived, so pointer identity is both cheap and sufficient.
		key.shaderPtr = shader.get();
		// Part of the key because the input layout comes from the vertex array; two
		// arrays sharing a shader still need two pipelines.
		key.vertexArrayPtr = vertexArray.get();
		// Uninitialised PipelineKey has an indeterminate framebufferInfo, so it is only
		// filled when a framebuffer exists. A null framebuffer falls through to the
		// explicit error below rather than being used as a cache key.
		if (auto fb = m_Context->GetCurrentFramebuffer())
			// The third key component. FramebufferInfo carries format and sample count, so
			// the same shader needs a different pipeline for an MSAA editor target.
			key.framebufferInfo = fb->getFramebufferInfo();

		// The hit path returns immediately -- creating a graphics pipeline is a driver
		// call plus shader-variant work, far too expensive to repeat per draw.
		auto it = m_PipelineCache.find(key);
		// Cache hit. The common case by far: a steady-state frame hits this for every
		// draw, which is why the miss path below can afford to be verbose.
		if (it != m_PipelineCache.end())
		{
			return it->second;
		}

		// Create new pipeline
		nvrhi::IDevice *device = m_Context->GetDevice();
		// The device is owned by the context. A null here means the context was never
		// initialised, which is a lifecycle bug rather than a transient failure.
		if (!device)
		{
			WL_CORE_ERROR("GetOrCreatePipeline: No NVRHI device available");
			return nullptr;
		}

		// dynamic_pointer_cast, not static: the Ref<Shader> could hold a non-NVRHI
		// backend shader if the renderer was swapped, and a bad cast here would be UB.
		auto nvrhiShader = std::dynamic_pointer_cast<NVRHIShader>(shader);
		// Same reason as the shader cast: cross-backend safety. A non-NVRHI array must
		// fail here rather than at a reinterpret_cast deeper in the function.
		auto nvrhiVA = std::dynamic_pointer_cast<NVRHIVertexArray>(vertexArray);

		// Both must be NVRHI-backed. Returning nullptr rather than throwing keeps the
		// failure on the DrawIndexed error path instead of unwinding through ImGui.
		if (!nvrhiShader || !nvrhiVA)
		{
			WL_CORE_ERROR("GetOrCreatePipeline: Invalid shader or vertex array");
			return nullptr;
		}

		// Create input layout
		// Out-parameter for GetInputLayout, which fills it with the attribute count.
		// Zero-initialised so a failed call leaves the count at 0 rather than garbage.
		uint32_t inputLayoutCount = 0;
		// Borrowed pointer into the vertex array's storage, valid only for this call.
		// NVRHI copies it inside createInputLayout, so no lifetime extension is needed.
		nvrhi::VertexAttributeDesc *inputLayoutDescs = nvrhiVA->GetInputLayout(inputLayoutCount);
		// Default-constructed (null) and left null if the layout above is empty.
		// Assigning a null handle to pipelineDesc.inputLayout is legal in NVRHI.
		nvrhi::InputLayoutHandle inputLayout;
		// An empty layout would create an invalid input layout, so the handle is left
		// default-constructed (null) and assigned to pipelineDesc.inputLayout as-is.
		if (inputLayoutDescs && inputLayoutCount > 0)
		{
			inputLayout = device->createInputLayout(inputLayoutDescs, inputLayoutCount, nvrhiShader->GetVertexShader());
		}

		// Create graphics pipeline
		nvrhi::GraphicsPipelineDesc pipelineDesc;
		// VS and PS are set unconditionally. A shader with only one stage leaves the
		// other handle null, which NVRHI accepts for a PSO missing that stage.
		pipelineDesc.VS = nvrhiShader->GetVertexShader();
		pipelineDesc.PS = nvrhiShader->GetPixelShader();
		// May be null (see above). The layout must be built against the SAME vertex
		// shader byte code passed here, or the attribute/entry-point pairing is invalid.
		pipelineDesc.inputLayout = inputLayout;
		// Optional: a compute-style or layout-less shader still gets a pipeline, with
		// pipelineDesc.bindings left empty and state.bindings empty in DrawIndexed.
		if (nvrhiShader->GetBindingLayout())
			pipelineDesc.addBindingLayout(nvrhiShader->GetBindingLayout());

		// The state below is FIXED, not derived from material data. That is why the
		// framebuffer is part of the cache key and why a blend-mode change would need a
		// new key. Documented as-is; deriving state from the material (and hashing it
		// into the key) is a larger change than a comments-only pass may make.
		// Render state
		// No culling: the engine draws 2D quads whose winding is not guaranteed, and
		// getting this wrong shows up as invisible geometry rather than an error.
		pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
		// Solid, not wireframe: no debug-wireframe mode exists in this file.
		pipelineDesc.renderState.rasterState.fillMode = nvrhi::RasterFillMode::Solid;
		// Depth test ON for every pipeline this file creates, including 2D. 2D content
		// relies on draw order plus LessOrEqual, not on disabling depth.
		pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
		// Depth write ON, which is what lets later geometry depth-test against earlier
		// geometry in the same frame.
		pipelineDesc.renderState.depthStencilState.depthWriteEnable = true;
		// LessOrEqual, not Less: equal depth must pass so coplanar decals and UI drawn
		// at the same z do not z-fight themselves away.
		pipelineDesc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;

		// SrcAlpha / InvSrcAlpha is standard source-over alpha. Alpha-channel factors
		// are One / InvSrcAlpha, i.e. alpha is NOT squared the way the colour channels are.
		// Blend state
		pipelineDesc.renderState.blendState.targets[0].blendEnable = true;
		// srcBlend/srcBlendAlpha pair below forms standard source-over: out = src*a + dst*(1-a).
		pipelineDesc.renderState.blendState.targets[0].srcBlend = nvrhi::BlendFactor::SrcAlpha;
		// InvSrcAlpha, not OneMinusSrcAlpha-as-a-factor-name: NVRHI factors are applied
		// per channel, so the alpha channel would otherwise be squared.
		pipelineDesc.renderState.blendState.targets[0].destBlend = nvrhi::BlendFactor::InvSrcAlpha;
		// Additive combination of the two weighted terms. Only targets[0] is configured,
		// so a multi-render-target framebuffer would get undefined blending on [1]..[N].
		pipelineDesc.renderState.blendState.targets[0].blendOp = nvrhi::BlendOp::Add;
		// Alpha channel: src is taken at full weight, dst at InvSrcAlpha. This is what
		// makes the result's alpha equal src.a + dst.a*(1-src.a).
		pipelineDesc.renderState.blendState.targets[0].srcBlendAlpha = nvrhi::BlendFactor::One;
		pipelineDesc.renderState.blendState.targets[0].destBlendAlpha = nvrhi::BlendFactor::InvSrcAlpha;
		pipelineDesc.renderState.blendState.targets[0].blendOpAlpha = nvrhi::BlendOp::Add;
		// All four channels: the engine renders to a backbuffer where the alpha channel
		// is meaningful to the compositor, so masking it off would break window
		// blending on some platforms.
		pipelineDesc.renderState.blendState.targets[0].colorWriteMask = nvrhi::ColorMask::All;

		// Framebuffer info from context
		// Fetched a SECOND time: the one at the top of the function was inside an `if`
		// and is out of scope here. Two calls, same frame, same answer in practice.
		nvrhi::FramebufferHandle fb = m_Context->GetCurrentFramebuffer();
		// createGraphicsPipeline needs the framebuffer for render-target format and MSAA
		// sample count; without it there is nothing sensible to build against.
		if (!fb)
		{
			WL_CORE_ERROR("GetOrCreatePipeline: No framebuffer available");
			return nullptr;
		}

		// The one genuinely expensive call on the miss path. It can fail on a bad desc,
		// an unsupported format combination, or shader/entry-point mismatch.
		auto pipeline = device->createGraphicsPipeline(pipelineDesc, fb);
		// createGraphicsPipeline reports failure by returning a null handle rather than
		// throwing, so it has to be checked explicitly.
		if (!pipeline)
		{
			WL_CORE_ERROR("GetOrCreatePipeline: Failed to create graphics pipeline");
			return nullptr;
		}

		// Inserted only on success. A failed create is NOT cached, so the next frame
		// retries -- which is what you want after a transient device loss.
		m_PipelineCache[key] = pipeline;
		// Returns the handle rather than the cache entry; equivalent, but keeps the
		// ownership story in one place.
		return pipeline;
	}

	// Keyed by shader pointer ONLY. That is correct solely because SetActiveTextures()
	// clears the cache whenever the texture list changes -- see the note there.
	nvrhi::BindingSetHandle NVRHIRendererAPI::GetOrCreateBindingSet(const Ref<Shader> &shader)
	{
		// Cast repeated from GetOrCreatePipeline rather than threaded through: the two
		// functions are independent cache lookups and each validates for itself.
		auto nvrhiShader = std::dynamic_pointer_cast<NVRHIShader>(shader);
		// Unlike the pipeline path, this one returns quietly -- there is no error to
		// report from a setter-like accessor, and DrawIndexed tolerates a null set.
		if (!nvrhiShader)
			return nullptr;

		// Raw address as the key. A shader destroyed and a new one allocated at the same
		// address would collide, which is the classic ABA hazard; in practice Shader
		// objects live for the whole resource lifetime.
		const void *shaderPtr = shader.get();
		auto it = m_BindingSetCache.find(shaderPtr);
		// Cache hit. Cleared by SetActiveTextures() whenever the bound textures change,
		// which is the only other input to the set's contents.
		if (it != m_BindingSetCache.end())
		{
			return it->second;
		}

		nvrhi::IDevice *device = m_Context->GetDevice();
		// Silent nullptr, not an error: unlike pipeline creation this has no caller
		// that logs, and DrawIndexed's `if (bindingSet)` already handles the null case.
		if (!device)
			return nullptr;

		// The layout is NOT cached with the shader; it is read fresh each time so a
		// layout rebuilt after a device reset is picked up here.
		nvrhi::BindingLayoutHandle layout = nvrhiShader->GetBindingLayout();
		// No layout means nothing to bind against, so createBindingSet would fail.
		// Returning early avoids calling the driver with a null layout.
		if (!layout)
			return nullptr;

		// Default-constructed and empty: every item below is added conditionally, so
		// the desc mirrors exactly the items the layout declared.
		nvrhi::BindingSetDesc setDesc;

		// Bind vertex shader constant buffer
		// Slots 0 and 1 are fixed here and hard-coded in NVRHIShader::CreateBindingLayout,
		// which uses ConstantBuffer(0) and ConstantBuffer(1). The two must stay in sync.
		nvrhi::BufferHandle vsCB = nvrhiShader->GetVSConstantBuffer();
		// Conditional because CreateConstantBuffers only makes the buffer when the
		// reflected constant data is non-empty (NVRHIShader.cpp).
		if (vsCB)
		{
			// Only added when the shader actually HAS that buffer, so the binding set matches
			// a layout that was built conditionally too.
			setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, vsCB));
		}

		// Bind pixel shader constant buffer
		nvrhi::BufferHandle psCB = nvrhiShader->GetPSConstantBuffer();
		// Same conditional pairing for the pixel shader's buffer at slot 1.
		if (psCB)
		{
			// Slot 1, matching ConstantBuffer(1) in NVRHIShader::CreateBindingLayout. If the
			// layout only declared slot 0, adding slot 1 here would make the set invalid.
			setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(1, psCB));
		}

		// Bind all active textures to the shared t0[32] array. Always provide
		// slot 0's sampler, even if the caller has no non-empty texture list.
		// Index == array element in t0. The layout declares setSize(32) on that binding,
		// so a 33rd texture would exceed it -- there is no bounds check here.
		for (size_t i = 0; i < m_ActiveTextures.size(); i++)
		{
			// A null entry is skipped, leaving that array element unbound. NVRHI tolerates
			// gaps in a descriptor array only if the shader never reads that element.
			if (!m_ActiveTextures[i])
				continue;
			// Same cross-backend guard as the shader/array casts above.
			auto nvrhiTex = std::dynamic_pointer_cast<NVRHITexture2D>(m_ActiveTextures[i]);
			if (nvrhiTex && nvrhiTex->GetTexture())
			{
				// All textures share binding 0 and differ only by array element -- a single
				// descriptor array rather than N separate bindings, which is what lets the
				// layout be identical across every material.
				setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, nvrhiTex->GetTexture()).setArrayElement(static_cast<uint32_t>(i)));
				// The sampler is bound once, at slot 0, and shared by every array element. The
				// layout declares exactly one sampler (NVRHIShader::CreateBindingLayout), so
				// binding it per-element would exceed the layout.
				if (i == 0 && nvrhiTex->GetSampler())
					// Bound once from element 0's texture. A shader that samples t0[5] uses the
					// filter mode of texture 0 -- a real, if deliberate, simplification.
					setDesc.addItem(nvrhi::BindingSetItem::Sampler(0, nvrhiTex->GetSampler()));
			}
		}

		// Create binding set
		// DEFECT: the result is cached even if creation FAILED (handle null). A null
		// handle is then returned on every later call for this shader, and DrawIndexed's
		// `if (bindingSet)` skips the push -- so a shader with no textures silently
		// draws with no bindings rather than erroring.
		nvrhi::BindingSetHandle bindingSet = device->createBindingSet(setDesc, layout);
		// Unconditional store, unlike the pipeline cache. See the DEFECT note at the
		// createBindingSet call above: a failed create caches a null handle.
		m_BindingSetCache[shaderPtr] = bindingSet;
		return bindingSet;
	}

	// The main draw path. Everything it needs is either cached or rebuilt per call;
	// the pipeline and binding set come from the caches above.
	void NVRHIRendererAPI::DrawIndexed(const Ref<VertexArray> &vertexArray, uint32_t indexCount)
	{
		WL_PROFILE_FUNCTION();

		// A shader is mandatory here (unlike DrawLines), because the pipeline is built
		// from it. m_CurrentShader is set by NVRHIShader::Bind().
		// All three checked together because any one of them missing makes the draw
		// unconstructable, and one error message is more useful than three.
		if (!m_Context || !m_CurrentShader || !vertexArray)
		{
			WL_CORE_ERROR("DrawIndexed: Missing context, shader, or vertex array");
			return;
		}

		nvrhi::ICommandList *cmd = m_Context->GetCommandList();
		if (!cmd)
		{
			WL_CORE_ERROR("DrawIndexed: No command list available");
			return;
		}

		// Get or create pipeline
		auto pipeline = GetOrCreatePipeline(m_CurrentShader, vertexArray);
		// GetOrCreatePipeline already logged the specific reason; this message names the
		// consequence so a log filtered to DrawIndexed still shows the failure.
		if (!pipeline)
		{
			WL_CORE_ERROR("DrawIndexed: Failed to get/create pipeline");
			return;
		}

		// Get or create binding set
		// Deliberately NOT checked for null here. The `if (bindingSet)` push below makes
		// a null set equivalent to no bindings, so the draw proceeds unshaded.
		auto bindingSet = GetOrCreateBindingSet(m_CurrentShader);

		auto nvrhiVA = std::dynamic_pointer_cast<NVRHIVertexArray>(vertexArray);
		if (!nvrhiVA)
		{
			WL_CORE_ERROR("DrawIndexed: Invalid vertex array");
			return;
		}

		// Get index buffer to determine count
		// Base-class accessor: returns IndexBuffer, not NVRHIIndexBuffer, so a second
		// downcast is needed below to reach GetBuffer().
		auto indexBuffer = nvrhiVA->GetIndexBuffer();
		// A vertex array with vertices but no indices cannot be drawn with drawIndexed,
		// which is the only draw path this file has.
		if (!indexBuffer)
		{
			WL_CORE_ERROR("DrawIndexed: No index buffer");
			return;
		}

		// Third cross-backend downcast in this function, for the same safety reason.
		auto nvrhiIB = std::dynamic_pointer_cast<NVRHIIndexBuffer>(indexBuffer);
		// Without the concrete buffer there is no nvrhi::BufferHandle to bind, so the
		// draw is abandoned rather than issued with a null index buffer.
		if (!nvrhiIB)
		{
			WL_CORE_ERROR("DrawIndexed: Invalid index buffer");
			return;
		}

		// nvrhi::GraphicsState is a VALUE, not a handle: it is rebuilt every draw and
		// consumed by setGraphicsState, which copies it into the command list.
		// Set up graphics state
		nvrhi::GraphicsState state;
		// The pipeline must be set before setGraphicsState is called below; NVRHI
		// snapshots the whole GraphicsState at that point.
		state.pipeline = pipeline;
		// May be null if the context has no target. Unlike the pipeline path this is not
		// checked -- DrawIndexed would then render with a null framebuffer.
		state.framebuffer = m_Context->GetCurrentFramebuffer();

		// Add binding set if available
		if (bindingSet)
		{
			// Skipped when the set is null, leaving state.bindings empty. That is legal and
			// means "bind nothing", which for a shader expecting t0 is undefined output.
			state.bindings.push_back(bindingSet);
		}

		// Slot indices come from the loop position, so the vertex buffer order in the
		// VertexArray must match the input layout's slot numbers exactly.
		// Bind vertex buffers
		const auto &vertexBuffers = nvrhiVA->GetVertexBuffers();
		// Binds every slot the vertex array declares. A gap in slot numbering would
		// silently shift attributes, so the array order must match the input layout.
		for (size_t i = 0; i < vertexBuffers.size(); i++)
		{
			auto nvrhiVB = std::dynamic_pointer_cast<NVRHIVertexBuffer>(vertexBuffers[i]);
			// Non-NVRHI vertex buffers are skipped rather than erroring, so a partially
			// migrated mesh draws its NVRHI slots and silently omits the rest.
			if (nvrhiVB)
			{
				// Built by chaining builder calls: a fresh default binding is configured and
				// appended, rather than mutating one in place.
				state.vertexBuffers.push_back(nvrhi::VertexBufferBinding()
												  .setBuffer(nvrhiVB->GetBuffer())
													  .setSlot(static_cast<uint32_t>(i))
												  // Byte offset 0 -- the whole buffer from its start. No sub-range support here,
												  // so a mesh that wants to draw a slice cannot express it.
												  .setOffset(0));
			}
		}

		// Bind index buffer
		state.indexBuffer = nvrhi::IndexBufferBinding()
								// The nvrhi buffer behind the engine's IndexBuffer wrapper. Continuation line of
								// the fluent chain above, so it has no terminating semicolon of its own.
								.setBuffer(nvrhiIB->GetBuffer())
								// Same zero offset for the index buffer. The binding is a single struct member
								// of state, not a push_back, because there is exactly one index buffer.
								.setOffset(0)
								// R32_UINT is hard-coded: NVRHIIndexBuffer stores uint32_t* (see NVRHIBuffer.h),
								// so the format is implied by the buffer type and cannot disagree.
								.setFormat(nvrhi::Format::R32_UINT);

		// Derived from the framebuffer, not from GetWidth()/GetHeight(), because an
		// editor framebuffer is rarely the window size. Without this the viewport
		// would cover the window while rendering into a smaller offscreen target.
		// Set viewport from the actual render target (editor framebuffer when
		// bound; otherwise the swapchain), not always the window size
		uint32_t vpWidth = m_Context->GetWidth();
		// Window/frame size from the context, used as the fallback when the framebuffer
		// reports no usable dimensions.
		uint32_t vpHeight = m_Context->GetHeight();
		// Prefer the render target's own size over the window's. Skipped entirely when
		// there is no framebuffer, leaving the context size above.
		if (state.framebuffer)
		{
			const auto &fbInfo = state.framebuffer->getFramebufferInfo();
			// Zero-size info is treated as "no information" and the context size is kept,
			// so an uninitialised framebuffer falls back instead of setting a 0x0 viewport.
			if (fbInfo.width > 0 && fbInfo.height > 0)
			{
				vpWidth = fbInfo.width;
				// Width and height are assigned together; a half-updated pair would produce an
				// aspect ratio the user could see as stretched geometry.
				vpHeight = fbInfo.height;
			}
		}
		// NVRHI takes floats, hence the casts. Origin defaults to (0,0), which is why
		// there is no X/Y here even though SetViewport accepted them.
		state.viewport.addViewport(nvrhi::Viewport(static_cast<float>(vpWidth), static_cast<float>(vpHeight)));
		// Scissor matching the viewport. NVRHI does not implicitly clear the scissor, so
		// without this a stale rect from a previous target would clip this draw.
		state.viewport.addScissorRect(nvrhi::Rect(vpWidth, vpHeight));

		// Mirrors Clear()'s structure: same commands, wrapped in an immediate
		// open/close/execute when no frame is in progress.
		// Batched frame: BeginFrame already opened, Execute will close+execute.
		// Fallback for outside-frame calls (e.g. init) uses transient open/close+execute.
		if (!m_Context->IsCommandListOpen())
		{
			// DEVICE-LOST GUARD. Same hazard as Clear(): a lost device makes
			// IsCommandListOpen() false, so the transient branch would open/close/
			// execute on a removed device. Skip the draw instead.
			if (m_Context->IsDeviceLost())
			{
				WL_CORE_ERROR("DrawIndexed: Device lost, skipping draw");
				return;
			}
			WL_CORE_WARN("DrawIndexed: Command list not open, using transient open/close");
			// Transient branch only, for the same reason as in Clear(): opening here during
			// a batched frame would discard the frame's already-recorded draws.
			cmd->open();
			// static_pointer_cast is safe here -- GetOrCreatePipeline above already proved
			// m_CurrentShader is an NVRHIShader (it dynamic_cast it and errored out if not).
			// Uploading per draw rather than in Bind() is what makes a uniform set AFTER
			// Bind() actually take effect.
			std::static_pointer_cast<NVRHIShader>(m_CurrentShader)->UploadConstants(cmd);
		// Applies pipeline, framebuffer, bindings, vertex/index buffers, viewport and
		// scissor in one call -- the whole per-draw state bundle.
		cmd->setGraphicsState(state);
			// indexCount == 0 means "draw the whole buffer", which is the header's default.
			// The draw is expressed in INDICES, not vertices: drawIndexed takes a vertex
			// count and interprets it through the bound index buffer.
			uint32_t actualIndexCount = (indexCount > 0) ? indexCount : nvrhiIB->GetCount();
			// Default DrawArguments: no vertex offset, no instance count, no index offset,
			// so this is a single non-instanced draw.
			cmd->drawIndexed(nvrhi::DrawArguments().setVertexCount(actualIndexCount));
			// Ends the transient list. Nothing else will close it, so this is mandatory
			// before the executeCommandList below.
			cmd->close();
			if (auto *dev = m_Context->GetDevice())
				dev->executeCommandList(cmd);
			return;
		}
		// Batched twin of the line above. The two branches are duplicated rather than
		// shared because open/close must straddle them, not sit outside.
		std::static_pointer_cast<NVRHIShader>(m_CurrentShader)->UploadConstants(cmd);
		// Batched twin of the setGraphicsState above, recorded onto the frame's list.
		cmd->setGraphicsState(state);
		// Same 0-means-whole-buffer fallback as the transient branch. No cmd->close()
		// here: the frame's ExecuteNVRHICommandList() owns that.
		uint32_t actualIndexCount = (indexCount > 0) ? indexCount : nvrhiIB->GetCount();
		// Batched twin of the draw above. This is the line that actually rasterises in
		// normal frame flow; the transient branch is only for out-of-frame calls.
		cmd->drawIndexed(nvrhi::DrawArguments().setVertexCount(actualIndexCount));
	}

	// Unimplemented. Both parameters are unused; the WARN below is the only output,
	// so a caller that draws lines sees repeated warnings instead of geometry.
	void NVRHIRendererAPI::DrawLines(const Ref<VertexArray> &vertexArray, uint32_t vertexCount)
	{
		WL_PROFILE_FUNCTION();

		// TODO: Implement line drawing for all NVRHI backends
		// For Vulkan, primitive topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST
		// For DirectX, primitive topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST
		// WARN, not ERROR: line drawing is optional on some backends, and a per-frame
		// warn would otherwise spam a log the user cannot act on.
		WL_CORE_WARN("NVRHIRendererAPI::DrawLines not yet implemented");
	}

	// No-op for every backend, unlike DrawLines which is merely unimplemented.
	// width is read only for the trace.
	void NVRHIRendererAPI::SetLineWidth(float width)
	{
		WL_PROFILE_FUNCTION();

		// Vulkan supports variable line width (with wideLines feature), but it's not universally available
		// DirectX does not support variable line width
		// For now, this is a no-op for all APIs
		// The value is echoed rather than dropped so a caller can confirm the call
		// reached the backend at all when debugging a missing line.
		WL_CORE_TRACE("SetLineWidth({0}) - not supported on current API", width);
	}

	// Not a RendererAPI override -- it is NVRHI-only, declared in this class's own
	// header. Group counts are in groups, not threads: each group is a workgroup
	// sized by the shader's own [numthreads] declaration.
	void NVRHIRendererAPI::DispatchCompute(const Ref<Shader> &shader, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
	{
		WL_PROFILE_FUNCTION();

		// A dispatch with no target device would have nowhere to record, so it is
		// rejected here rather than at the first device call.
		if (!m_Context || !shader)
		{
			WL_CORE_ERROR("DispatchCompute: Missing context or shader");
			return;
		}

		nvrhi::ICommandList *cmd = m_Context->GetCommandList();
		// Same rationale as the draw path: recording needs an open-able command list.
		if (!cmd)
		{
			WL_CORE_ERROR("DispatchCompute: No command list available");
			return;
		}

		// Third occurrence of this cast in the file; each function validates its own
		// argument rather than trusting a caller's cast.
		auto nvrhiShader = std::dynamic_pointer_cast<NVRHIShader>(shader);
		// A shader without a compute stage is rejected here rather than at pipeline
		// creation, so the error names the actual problem.
		if (!nvrhiShader || !nvrhiShader->GetComputeShader())
		{
			WL_CORE_ERROR("DispatchCompute: Invalid shader or missing compute shader");
			return;
		}

		// Fetched separately from the command list: pipelines are device objects, not
		// command-list objects, and must outlive the recording.
		nvrhi::IDevice *device = m_Context->GetDevice();
		// Silent return, unlike the draw path's error. DispatchCompute has no caller
		// in this file, and the null-device case only occurs during teardown.
		if (!device)
			return;

		// Same pointer-identity key the graphics path uses, for the same reason.
		const void *shaderPtr = shader.get();
		// Keyed by shader pointer only -- compute pipelines do not depend on the bound
		// framebuffer, so unlike GraphicsPipelineHandle there is nothing else to key on.
		auto it = m_ComputePipelineCache.find(shaderPtr);
		// Left default-constructed (null) so a create failure below leaves it null
		// rather than holding an uninitialised handle.
		nvrhi::ComputePipelineHandle pipeline;
		// Cache hit. Only the pipeline is reused -- a compute dispatch still re-records
		// state every call, because there is no draw state to amortise.
		if (it != m_ComputePipelineCache.end())
		{
			// A cached null is possible only if a create succeeded and returned null, which
			// the guard at the insert below prevents.
			pipeline = it->second;
		}
		else
		{
			// Minimal desc: just the compute stage and, optionally, the binding layout.
			nvrhi::ComputePipelineDesc desc;
			// Already validated non-null by the check at the top of this function.
			desc.CS = nvrhiShader->GetComputeShader();
			if (nvrhiShader->GetBindingLayout())
			{
				desc.addBindingLayout(nvrhiShader->GetBindingLayout());
			}
			pipeline = device->createComputePipeline(desc);
			// Only successful creations are cached. On failure `pipeline` stays null and
			// the explicit error below fires, rather than caching a null handle that would
			// suppress the error on every later dispatch.
			if (pipeline)
			{
				// Inside `if (pipeline)`, unlike the binding-set cache. This one correctly
				// avoids caching a failed creation.
				m_ComputePipelineCache[shaderPtr] = pipeline;
			}
		}

		// Both the cache-miss failure and a hypothetically cached null land here, so the
		// message is the single place a dispatch failure is reported.
		if (!pipeline)
		{
			WL_CORE_ERROR("DispatchCompute: Failed to create compute pipeline");
			return;
		}

		// Reuses the SAME binding set as the graphics path, whose layout is built with
		// visibility Vertex|Pixel only. A compute shader needing those bindings would
		// need a compute-visible layout, which CreateBindingLayout does not produce.
		auto bindingSet = GetOrCreateBindingSet(shader);

		// The compute equivalent of GraphicsState: pipeline plus optional bindings.
		nvrhi::ComputeState state;
		// Non-null here: the explicit error above returns if creation failed.
		state.pipeline = pipeline;
		if (bindingSet)
		{
			state.bindings.push_back(bindingSet);
		}

		// Same transient-vs-batched split as DrawIndexed and Clear: recording into a
		// closed list is invalid, and per-draw open/close inside a batched frame would
		// clobber the frame's active list.
		if (!m_Context->IsCommandListOpen())
		{
			// DEVICE-LOST GUARD. Same hazard as Clear() and DrawIndexed(): a lost
			// device makes IsCommandListOpen() false, so the transient branch would
			// open/close/execute on a removed device. Skip the dispatch instead.
			if (m_Context->IsDeviceLost())
			{
				WL_CORE_ERROR("DispatchCompute: Device lost, skipping dispatch");
				return;
			}
			WL_CORE_WARN("DispatchCompute: Command list not open, using transient");
			// Transient branch only, mirroring DrawIndexed's split. Same hazard: opening
			// mid-frame would reset the active command list.
			cmd->open();
			cmd->setComputeState(state);
			// Raw group counts, no bounds check. A caller passing an absurd count gets
			// whatever the driver does with it.
			// Batched twin: no open/close, because the frame's command list is already live
			// and closing it here would submit the frame's draws early.
			cmd->dispatch(groupCountX, groupCountY, groupCountZ);
			// Closes the list opened above so it can be submitted immediately.
			cmd->close();
			if (auto *dev = m_Context->GetDevice())
				dev->executeCommandList(cmd);
			return;
		}
		cmd->setComputeState(state);
		cmd->dispatch(groupCountX, groupCountY, groupCountZ);
	}

	// Drops every cached GPU object so a resize, a device loss or an API switch can
	// rebuild them. The nvrhi handles release their references when the maps drop.
	void NVRHIRendererAPI::ClearCachedResources()
	{
		WL_PROFILE_FUNCTION();

		// Logged before the work so a hang inside a clear() is attributable.
		WL_CORE_INFO("Clearing cached NVRHI resources");

		// Clear pipeline caches
		m_PipelineCache.clear();
		// Unlike the destructor, this clears the compute cache too -- see the DEFECT
		// note on the destructor's graphics-only clear.
		m_ComputePipelineCache.clear();

		// Clear binding set cache
		// Third cache. Cleared last because the pipeline caches are the expensive ones
		// to rebuild, so the log order below reflects rebuild cost, not importance.
		m_BindingSetCache.clear();

		// Cleared alongside the caches: the shader owns the binding LAYOUT the binding
		// sets were built against, so leaving it set would let a draw reach a stale
		// pipeline whose layout no longer matches.
		// Reset current shader
		// Ref assignment, not .reset(): drops the shader's reference count. The Ref in
		// NVRHIShader::Bind() is a non-owning alias, so this does not destroy it.
		m_CurrentShader = nullptr;

		// DEFECT: this reports the sizes AFTER the clear() calls above, so it always
		// prints zero for all three. The counts are unreachable as written -- capturing
		// the sizes before clearing would make this a useful diagnostic.
		WL_CORE_INFO("Cleared {0} pipelines, {1} compute pipelines, and {2} binding sets",
					 m_PipelineCache.size(), m_ComputePipelineCache.size(), m_BindingSetCache.size());
	}

}
