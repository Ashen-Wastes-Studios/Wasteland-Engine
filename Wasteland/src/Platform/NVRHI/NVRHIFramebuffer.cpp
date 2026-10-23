#include "wlpch.h"
#include "NVRHIFramebuffer.h"
#include "NVRHIContext.h"
#include "NVRHIRendererAPI.h"
#include "Wasteland/Core/Log.h"
#include "Wasteland/Renderer/RendererAPI.h"

// The Vulkan ImGui renderer backend, for ImGui_ImplVulkan_AddTexture alone. Needed
// here rather than behind a forward declaration because the descriptor-set cache
// below is deliberately lazy -- AddTexture is called only when the attachment or
// the descriptor pool changed -- so the header does not have to include it and the
// other two backends of this file do not pay for it. Vendor/imgui is already on
// the include path (premake5.lua IncludeDir["ImGui"]), which is why this is
// "backends/..." and not "imgui/backends/...".
#include "backends/imgui_impl_vulkan.h"

#include <cstring>

// The NVRHI backend of the engine's cross-API Framebuffer interface.
// Every method here is one backend's answer to the pure-virtual
// declared in Wasteland/Renderer/Framebuffer.h; the OpenGL sibling in
// Platform/OpenGL/OpenGLFramebuffer.cpp is the other implementation.
namespace Wasteland
{

	// Every method below needs a device, and the device is not a member:
	// NVRHI objects are owned by a context that the engine can swap at
	// runtime, so the device is re-fetched per call rather than cached.
	// Helper to get the current NVRHI device
	static nvrhi::IDevice *GetDevice()
	{
		auto *context = NVRHIRendererAPI::GetCurrentContext();
		// Guards against a context left over from a DIFFERENT backend. A
		// runtime API switch (see RendererAPI) replaces the context while this
		// framebuffer's texture handles still name objects owned by the old
		// device, so comparing the two APIs is what stops a stale handle being
		// handed to a device that never allocated it.
		// Null and API-mismatch are folded into one test because both have the
		// same consequence: there is no usable device, so callers must bail.
		if (!context || context->GetAPI() != RendererAPI::GetAPI())
		{
			// An error, not a warning: this is a programming error (the context and
			// the active API disagree), not a transient condition, so it should be
			// visible in a log the developer is actually reading.
			WL_CORE_ERROR("NVRHIFramebuffer: renderer API and NVRHI context do not match");
			return nullptr;
		}
		return context->GetDevice();
	}

	// The translation from the engine's format enum to NVRHI's. Both
	// backends must implement this exhaustively; an unmapped value is a
	// loud assert rather than a silent wrong format (see the enum comment in
	// Framebuffer.h, which names this function as one of the two mappers).
	// Helper to convert framebuffer texture format to NVRHI format
	static nvrhi::Format FramebufferTextureFormatToNVRHI(FramebufferTextureFormat format)
	{
		switch (format)
		{
		// RGBA8 maps to the SRGB variant, which is the point of the mapping:
		// the engine's "RGBA8" means "an sRGB-encoded colour attachment", and
		// picking plain UNORM here would make colour management a per-backend
		// decision. The name does not advertise it.
		// NOTE the asymmetry this creates with ReadPixel's DX11 format switch
		// below, which handles RGBA8_UNORM but not SRGBA8_UNORM.
		case FramebufferTextureFormat::RGBA8:
			return nvrhi::Format::SRGBA8_UNORM;
		// RED_INTEGER -> R32_SINT, the entity-id attachment the editor reads
		// back for picking. Signed, because the sentinel that means "nothing
		// here" is -1, which is not representable in an unsigned format.
		case FramebufferTextureFormat::RED_INTEGER:
			// Depth and stencil in one attachment, matching the engine's single
			// FramebufferTextureFormat::Depth enumerator rather than splitting
			// them: the engine never uses the stencil half.
			return nvrhi::Format::R32_SINT;
		case FramebufferTextureFormat::DEPTH24STENCIL8:
			return nvrhi::Format::D24S8;
		default:
			// Release builds fall through to UNKNOWN, and NVRHI rejects an unknown
			// format when the texture is created -- so an unmapped enumerator is a
			// loud failure at createTexture, just later than the assert.
			WL_CORE_ASSERT(false, "Unknown FramebufferTextureFormat!");
			return nvrhi::Format::UNKNOWN;
		}
	}

	// Copies the spec by value and immediately creates GPU resources, so a
	// NVRHIFramebuffer is unusable until Invalidate has succeeded. The spec
	// is the authoritative record of the target size (see Framebuffer.h),
	// which is why it is copied rather than referenced -- Resize mutates it.
	NVRHIFramebuffer::NVRHIFramebuffer(const FramebufferSpecification &spec)
		: m_Specification(spec)
	{
		// The zero-size early-out inside Invalidate is what makes this safe:
		// a default-constructed specification has 0x0 dimensions, which would
		// otherwise ask the device for a zero-extent texture.
		Invalidate();
	}

	// Releases every handle explicitly even though they are RefCountPtrs
	// that would be released by the members' own destructors. The explicit
	// nulling is redundant with the destructor, and its ORDER matters more
	// than its presence: the framebuffer is released first because it holds
	// references to the attachment views.
	NVRHIFramebuffer::~NVRHIFramebuffer()
	{
		// Before the textures, and before anything else: a cached Vulkan descriptor
		// set names this framebuffer's image view, and returning it while that
		// view is still alive is the only order in which the free is well defined.
		ReleaseVulkanImGuiDescriptorSet();

		// Dropping the vector's elements releases the colour textures, but the
		// vector's capacity stays, so a resize does not reallocate the array.
		m_Framebuffer = nullptr;
		m_ColorTextures.clear();
		m_DepthTexture = nullptr;
	}

	void NVRHIFramebuffer::Invalidate()
	{
		// Resolved through GetDevice rather than through the context directly:
		// this method creates resources, so it is one of the paths that needs
		// the backend-match guard.
		nvrhi::IDevice *device = GetDevice();
		if (!device)
		{
			WL_CORE_ERROR("NVRHIFramebuffer: No NVRHI device available");
			return;
		}

		// Zero extent is invalid on every NVRHI backend, so this is a guard
		// against a real call rather than defensive noise: ImGui reports a
		// zero-sized panel for a frame or two during a dock rebuild, and the
		// resize path can hand that straight through.
		if (m_Specification.Width == 0 || m_Specification.Height == 0)
		{
			// A warning, not an error, because the caller has not done anything wrong:
			// the zero came from a transient UI layout state. Returning leaves the
			// PREVIOUS resources bound and intact, so the frame renders to the last
			// good size rather than to nothing.
			WL_CORE_WARN("NVRHIFramebuffer: Skipping Invalidate with zero size ({0}x{1})", m_Specification.Width, m_Specification.Height);
			return;
		}

		// Explicit release-then-recreate rather than mutating the existing
		// textures. NVRHI textures are immutable in their extent -- there is no
		// resize entry point -- so a size change means new objects, and the
		// handles have to be dropped first or the old textures stay alive for
		// the lifetime of the framebuffer.
		// The cached Vulkan descriptor set is released HERE, not merely left to be
		// noticed as stale later. The texture comparison in the cache would catch
		// it either way, but a set that outlives the VkImageView it names stays
		// allocated in ImGui's pool until the next resize or teardown, and a
		// dragged window edge resizes every frame.
		if (!ReleaseVulkanImGuiDescriptorSet())
			return;

		// Clear old resources
		m_Framebuffer = nullptr;
		m_ColorTextures.clear();
		m_DepthTexture = nullptr;

		// Create attachments
		// Walks the SPEC's attachment list, not a fixed count. Slot numbers come
		// from position in this vector, which is why the editor's
		// {RGBA8, RED_INTEGER, Depth} order is what makes ReadPixel(1, ...) the
		// entity-id attachment -- the ordering is a cross-file contract.
		for (const auto &attachmentSpec : m_Specification.Attachments.Attachments)
		{
			// Depth is special-cased rather than handled by format: it is the one
			// attachment type that needs a different set of resource flags, and
			// keeping it out of the colour path is what makes that difference
			// visible instead of hidden behind conditionals.
			if (attachmentSpec.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8)
			{
				// Create depth-stencil texture
				// D3D11/D3D12 note: A depth texture that is both DSV and SRV must be typeless
				// (R24G8_TYPELESS + R24_UNORM_X8_TYPELESS / D24_UNORM_S8_UINT). NVRHI only uses
				// the typeless resource format when desc.isTypeless == true. Here we do NOT
				// need to sample depth in the editor viewport, so keep it DSV-only to avoid
				// the E_INVALIDARG from CreateTexture2D (D24 + BIND_SHADER_RESOURCE without typeless).
				// Depths written one field at a time rather than through a designated
				// initialiser, because NVRHI's TextureDesc has many fields whose
				// defaults matter and a brace-init would silently zero the ones not
				// listed. Leaving mipLevels and arraySize untouched keeps NVRHI's own
				// defaults rather than imposing 0.
				nvrhi::TextureDesc desc;
				desc.width = m_Specification.Width;
				desc.height = m_Specification.Height;
				desc.depth = 1;
				desc.dimension = nvrhi::TextureDimension::Texture2D;
				desc.format = FramebufferTextureFormatToNVRHI(attachmentSpec.TextureFormat);
				desc.isRenderTarget = true;
				// The single most load-bearing line in this block. A D3D11/D3D12 depth
				// texture that is bound as both DSV and SRV must be created TYPELESS
				// (D24_UNORM_S8_UINT with paired RTV/SRV formats); binding SRV on a
				// non-typeless D24S8 texture fails CreateTexture2D with E_INVALIDARG.
				// Nothing samples depth in the editor viewport, so DSV-only is correct
				// here and isTypeless = false below is what keeps it that way.
				desc.isShaderResource = false; // critical: no SRV for depth without typeless
				desc.isUAV = false;
				desc.isTypeless = false;
				// `Samples > 0 ? Samples : 1` rather than Samples directly: the engine's
				// specification defaults Samples to 1 but treats 0 as "unset", and
				// NVRHI rejects a sample count below 1. The same line appears in the
				// colour branch below -- the two must agree, since the framebuffer's
				// attachments and its dimensions have to describe the same sample count.
				desc.sampleCount = m_Specification.Samples > 0 ? m_Specification.Samples : 1;
				desc.debugName = "Framebuffer_Depth";
				// The texture is born in DepthWrite and that state is KEPT, so the first
				// frame of a new framebuffer does not have to transition it. keepInitialState
				// means NVRHI will not silently substitute Unknown and force a barrier.
				desc.initialState = nvrhi::ResourceStates::DepthWrite;
				desc.keepInitialState = true;

				m_DepthTexture = device->createTexture(desc);
				if (!m_DepthTexture)
				{
					// A failed depth create is logged but NOT fatal: execution falls
					// through to build a framebuffer with no depth attachment. That is a
					// deliberate degradation -- a colour-only framebuffer still displays --
					// but depth-tested geometry against it will misbehave, so this is the
					// one log line that explains a visual bug with no crash.
					WL_CORE_ERROR("NVRHIFramebuffer: Failed to create depth texture ({0}x{1} format D24S8)", desc.width, desc.height);
				}
			}
			else
			{
				// Create color texture
				// The colour branch duplicates the desc-building above rather than
				// sharing it. The two differ in exactly two flags (isShaderResource
				// and the format), and keeping them separate is why the depth-only
				// constraint above is easy to see: this one CAN be sampled.
				nvrhi::TextureDesc desc;
				desc.width = m_Specification.Width;
				desc.height = m_Specification.Height;
				desc.depth = 1;
				desc.dimension = nvrhi::TextureDimension::Texture2D;
				desc.format = FramebufferTextureFormatToNVRHI(attachmentSpec.TextureFormat);
				desc.isRenderTarget = true;
				// The one flag that genuinely differs from the depth branch. ImGui
				// samples this texture to draw the viewport, and the editor's entity
				// picking reads it back, so isShaderResource must be true here.
				desc.isShaderResource = true; // needed for ImGui viewport + entity picking readback
				desc.isUAV = false;
				desc.isTypeless = false;
				desc.sampleCount = m_Specification.Samples > 0 ? m_Specification.Samples : 1;
				// The debug name embeds the CURRENT vector size, so it is the index
				// this texture will occupy once pushed -- computed before the push_back
				// below, which is why the two agree.
				desc.debugName = "Framebuffer_Color_" + std::to_string(m_ColorTextures.size());
				desc.initialState = nvrhi::ResourceStates::RenderTarget;
				desc.keepInitialState = true;

				auto texture = device->createTexture(desc);
				if (!texture)
				{
					WL_CORE_ERROR("NVRHIFramebuffer: Failed to create color texture {0} ({1}x{2})", m_ColorTextures.size(), desc.width, desc.height);
					continue;
				// Skipping this attachment on failure keeps the vector contiguous, but
				// it also SHIFTS every later attachment down one slot. Since slot
				// numbers are what ReadPixel indexes by, a failed colour create
				// silently changes what index 1 means. Nothing detects this.
				}
				m_ColorTextures.push_back(texture);
			}
		}

		// Create framebuffer — only add valid attachments
		// Built up from the vectors rather than from the specification, so an
		// attachment that failed to create is simply absent from the framebuffer
		// instead of being referenced as a null handle.
		nvrhi::FramebufferDesc fbDesc;

		for (const auto &colorTexture : m_ColorTextures)
		{
			// Belt-and-braces null check: the vector can only contain textures that
			// were created successfully (see the `continue` above), so this branch
			// should be unreachable. It costs nothing and keeps a null from being
			// handed to the driver.
			if (colorTexture)
				fbDesc.addColorAttachment(colorTexture);
		}
		// Depth is optional. A framebuffer whose depth create failed gets none,
		// which is the degradation the error log above warns about.
		if (m_DepthTexture)
		{
			fbDesc.setDepthAttachment(m_DepthTexture);
		}

		m_Framebuffer = device->createFramebuffer(fbDesc);
		if (!m_Framebuffer)
		{
			// Reports how many colour attachments were actually bound rather than
			// how many were requested -- the difference is the signal that an
			// earlier create failed.
			WL_CORE_ERROR("NVRHIFramebuffer: Failed to create framebuffer ({0} color attachments, depth={1})", m_ColorTextures.size(), m_DepthTexture ? "yes" : "no");
		}
	}

	// Hands a cached ImGui Vulkan descriptor set back to the pool it came from.
	//
	// DELIBERATELY GUARDED ON THE CURRENT CONTEXT STILL BEING VULKAN AND STILL
	// OWNING A POOL. ImGui_ImplVulkan_RemoveTexture dereferences ImGui's backend data
	// without a null check, so calling it after ImGui_ImplVulkan_Shutdown -- which
	// nulls io.BackendRendererUserData -- is a crash rather than a failed free. The
	// window shuts the renderer backend down before it deletes the graphics context,
	// so there is a real window in which the old context still exists but the pool
	// it owned is already gone; IsVulkanImGuiInitialized() closes exactly that gap,
	// because it reads the pool handle.
	//
	// A pool mismatch means its owner will reclaim the set, so it is forgotten
	// without RemoveTexture. For a live matching pool, device idle must succeed
	// before RemoveTexture; on failure the cached set is retained for a later retry.
	bool NVRHIFramebuffer::ReleaseVulkanImGuiDescriptorSet() const
	{
		if (!m_VkImGuiCacheDescriptorSet)
		{
			m_VkImGuiCacheTexture = nullptr;
			m_VkImGuiCachePool = nullptr;
			return true;
		}

		auto *context = NVRHIRendererAPI::GetCurrentContext();
		if (context && context->GetAPI() == RendererAPI::API::NVRHI_Vulkan &&
			context->IsVulkanImGuiInitialized() &&
			static_cast<void *>(context->GetVkImGuiDescriptorPool()) == m_VkImGuiCachePool)
		{
			if (!context->GetVkDevice())
			{
				WL_CORE_ERROR("NVRHIFramebuffer: cannot safely release Vulkan ImGui texture without a VkDevice");
				return false;
			}

			VkResult waitResult = vkDeviceWaitIdle(context->GetVkDevice());
			if (waitResult != VK_SUCCESS)
			{
				WL_CORE_ERROR("NVRHIFramebuffer: vkDeviceWaitIdle failed ({0}); keeping Vulkan ImGui descriptor set allocated", static_cast<int>(waitResult));
				return false;
			}

			ImGui_ImplVulkan_RemoveTexture(static_cast<VkDescriptorSet>(m_VkImGuiCacheDescriptorSet));
		}
		m_VkImGuiCacheDescriptorSet = nullptr;
		m_VkImGuiCacheTexture = nullptr;
		m_VkImGuiCachePool = nullptr;
		return true;
	}

	// Shared Vulkan descriptor path for framebuffer attachments and external NVRHI textures.
	void *NVRHIFramebuffer::GetVulkanImGuiTexID(nvrhi::TextureHandle texture) const
	{
		if (!texture)
			return nullptr;

		auto *context = NVRHIRendererAPI::GetCurrentContext();
		if (!context || context->GetAPI() != RendererAPI::API::NVRHI_Vulkan)
			return nullptr;

		VkSampler sampler = context->GetVkImGuiSampler();
		VkDescriptorPool poolHandle = context->GetVkImGuiDescriptorPool();
		void *pool = static_cast<void *>(poolHandle);
		if (!sampler || !pool || !context->IsVulkanImGuiInitialized())
			return nullptr;

		nvrhi::ITexture *textureObject = texture.Get();
		if (!m_VkImGuiCacheDescriptorSet || textureObject != m_VkImGuiCacheTexture.Get() ||
			pool != m_VkImGuiCachePool)
		{
			if (!ReleaseVulkanImGuiDescriptorSet())
				return nullptr;

			VkImageView view = static_cast<VkImageView>(
				textureObject->getNativeView(nvrhi::ObjectTypes::VK_ImageView));
			if (!view)
			{
				WL_CORE_ERROR("NVRHIFramebuffer: NVRHI texture has no VkImageView for ImGui");
				return nullptr;
			}

			VkDescriptorSet descriptorSet = ImGui_ImplVulkan_AddTexture(
				sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			if (!descriptorSet)
			{
				WL_CORE_ERROR("NVRHIFramebuffer: ImGui_ImplVulkan_AddTexture failed for NVRHI texture");
				return nullptr;
			}

			m_VkImGuiCacheTexture = texture;
			m_VkImGuiCachePool = pool;
			m_VkImGuiCacheDescriptorSet = static_cast<void *>(descriptorSet);
		}

		return m_VkImGuiCacheDescriptorSet;
	}

	// Binding is indirect: there is no NVRHI framebuffer-to-context binding,
	// because a framebuffer is not render state. Binding here means
	// publishing it so GetCurrentFramebuffer() returns it and every
	// subsequent scene draw and clear targets it instead of the swapchain.
	void NVRHIFramebuffer::Bind()
	{
		// In NVRHI, framebuffers are set via GraphicsState. Route that through
		// the context: while bound, GetCurrentFramebuffer() returns this
		// framebuffer and scene draws/clears target it instead of the swapchain.
		// The context is re-fetched rather than cached for the same reason as
		// the device: a runtime API switch replaces it.
		auto *context = NVRHIRendererAPI::GetCurrentContext();
		if (context)
			// A null m_Framebuffer here means "bind the swapchain", which is the
			// same as Unbind. Nothing checks it, so calling Bind on a framebuffer
			// whose Invalidate failed silently renders to the swapchain instead --
			// the scene appears to work, in the wrong place.
			context->SetRenderTargetFramebuffer(m_Framebuffer);
	}

	// The counterpart to Bind. Restoring the swapchain is the same call with
	// a null argument, so there is no separate "reset" path to keep in step.
	void NVRHIFramebuffer::Unbind()
	{
		auto *context = NVRHIRendererAPI::GetCurrentContext();
		if (context)
			context->SetRenderTargetFramebuffer(nullptr); // back to swapchain
	}

	// Resize is a spec update plus a full rebuild, because -- as noted in
	// Invalidate -- NVRHI textures cannot change extent. It does not touch
	// the readback staging texture or readback command list; those are
	// size-checked lazily at their next use (see ReadPixel).
	void NVRHIFramebuffer::Resize(uint32_t width, uint32_t height)
	{
		// Precedence note: && binds tighter than ||, so this reads as
		// `(w==0 || h==0) || (w==W && h==H)`, which is the intended test.
		// Written without parentheses it still means the right thing, but it
		// reads as if the size comparison were unconditional.
		if (width == 0 || height == 0 ||
			width == m_Specification.Width && height == m_Specification.Height)
			return;

		m_Specification.Width = width;
		m_Specification.Height = height;

		Invalidate();
	}

	// Blocking GPU->CPU readback of one texel. This is a synchronisation
	// point on every call: it stalls until the GPU has finished the copy.
	// Callers must keep it off the per-frame path -- the editor picks at
	// most every 15 frames for exactly this reason.
	int NVRHIFramebuffer::ReadPixel(uint32_t attachmentIndex, int x, int y)
	{
		// Two guards in one condition: an out-of-range index, and a null entry
		// left by a partial resize failure. -1 is the documented "no pixel",
		// and callers rely on it -- the editor treats a -1 from the
		// entity-id attachment as "nothing under the cursor".
		if (attachmentIndex >= m_ColorTextures.size() || !m_ColorTextures[attachmentIndex])
			return -1;

		auto *context = NVRHIRendererAPI::GetCurrentContext();
		nvrhi::IDevice *device = context ? context->GetDevice() : nullptr;
		if (!device)
			return -1;

		const nvrhi::TextureDesc &texDesc = m_ColorTextures[attachmentIndex]->getDesc();
		// Bounds-checked against the TEXTURE's own descriptor rather than the
		// framebuffer specification, which is the one that is actually correct
		// after a resize the specification would disagree with. The cast to int
		// keeps the comparison signed, so a negative y is caught here instead
		// of wrapping to a huge unsigned offset.
		if (x < 0 || y < 0 || x >= (int)texDesc.width || y >= (int)texDesc.height)
			return -1;

		// DX11 and DX12/Vulkan read the same pixel by completely different
		// mechanisms, because NVRHI's D3D11 backend cannot create a deferred
		// command list (createCommandList errors and returns null) while the
		// DX12 and Vulkan backends can. The split is a property of NVRHI, not
		// a choice made here.
		if (context->GetAPI() == RendererAPI::API::NVRHI_DX11)
		{
			// D3D11: NVRHI's backend does not support deferred command lists
			// (createCommandList errors and returns null), so do the readback on
			// the raw immediate context instead. On DX11 the immediate context
			// executes at record time, so ordering with the batched NVRHI list
			// is preserved without any command-list juggling.
			auto *d3dDevice = static_cast<ID3D11Device *>(context->GetD3D11Device());
			auto *d3dCtx = static_cast<ID3D11DeviceContext *>(context->GetD3D11Context());
			// getNativeObject with the D3D11 object type on a DX11-only path.
			// On DX12 this type would not resolve -- the guard at the top of the
			// branch is what keeps it unreachable there.
			ID3D11Resource *srcResource = static_cast<ID3D11Resource *>(
				m_ColorTextures[attachmentIndex]->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource));
			if (!d3dDevice || !d3dCtx || !srcResource)
				return -1;

			DXGI_FORMAT dxgiFormat;
			switch (texDesc.format)
			{
			// DEFECT (latent): this switch handles RGBA8_UNORM, but a colour
			// attachment created from FramebufferTextureFormat::RGBA8 gets
			// SRGBA8_UNORM (see the mapper above), which is NOT in this list and
			// falls to `default: return -1`. So on DX11, reading a colour
			// attachment always reports "no pixel".
			// Not live today because the editor's only per-frame read is
			// ReadPixel(1, ...) -- the R32_SINT entity-id attachment, which IS
			// handled. It becomes a bug the moment anything reads attachment 0.
			case nvrhi::Format::RGBA8_UNORM:  dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;     break;
			case nvrhi::Format::R32_SINT:     dxgiFormat = DXGI_FORMAT_R32_SINT;           break;
			case nvrhi::Format::RGBA16_FLOAT: dxgiFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
			default:
				return -1;
			}

			// Staging texture descriptor for the DX11 copy. MipLevels and ArraySize
			// are pinned to 1 because exactly one texel is being read, and SampleDesc
			// is pinned to 1 for the same reason -- a non-multisampled staging
			// target is required when CopySubresourceRegion reads a single texel.
			D3D11_TEXTURE2D_DESC sDesc = {};
			sDesc.Width = texDesc.width;
			sDesc.Height = texDesc.height;
			sDesc.MipLevels = 1;
			sDesc.ArraySize = 1;
			sDesc.Format = dxgiFormat;
			// SampleDesc.Count = 1 unconditionally. If the framebuffer itself were
			// ever multisampled (Samples > 1), CopySubresourceRegion below could
			// not copy from a multisampled source to a single-sampled staging
			// texture -- that requires a resolve first. Latent only: nothing in the
			// editor sets Samples, so it is always 1.
			sDesc.SampleDesc.Count = 1;
			sDesc.Usage = D3D11_USAGE_STAGING;
			sDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ID3D11Texture2D *staging = nullptr;
			// FAILED() and a null pointer are checked together. Checking only the
			// HRESULT would dereference a null staging texture in the copy below
			// if the API reported failure without leaving the out-parameter null.
			if (FAILED(d3dDevice->CreateTexture2D(&sDesc, nullptr, &staging)) || !staging)
				return -1;

			// Copies the WHOLE source region into the staging texture, not the one
			// texel. The single texel is picked out on the CPU afterwards. On DX11
			// this is correct without any ordering concern, because the immediate
			// context executes at record time.
			d3dCtx->CopySubresourceRegion(staging, 0, 0, 0, 0, srcResource, 0, nullptr);

			D3D11_MAPPED_SUBRESOURCE mapped = {};
			int pixel = -1;
			// Map is the blocking read. On DX11 the immediate context has already
			// executed the copy above, so there is nothing to wait for.
			if (SUCCEEDED(d3dCtx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData)
			{
				// RowPitch, not width*4, is the stride. RowPitch is padded by the
				// driver to its own alignment, so computing the offset from the
				// width would read the wrong texel on every row past the first.
				const uint8_t *row = reinterpret_cast<const uint8_t *>(mapped.pData) + size_t(y) * mapped.RowPitch;
				if (texDesc.format == nvrhi::Format::R32_SINT)
				{
					// The integer path: memcpy of the whole int. A single aligned 4-byte
					// load, which is why it needs no branch on the individual bytes.
					memcpy(&pixel, row + size_t(x) * 4, sizeof(int));
				}
				else
				{
					// RGBA8: reconstruct the entity id written as bytes
					const uint8_t *px = row + size_t(x) * 4;
					// The colour path: the four bytes are reassembled little-endian by hand.
					// This reconstructs the uint32 the shader wrote across R,G,B,A rather
					// than reading one component, so it works for any entity-id encoding
					// that fits in four bytes.
					pixel = int(px[0]) | (int(px[1]) << 8) | (int(px[2]) << 16) | (int(px[3]) << 24);
				}
				d3dCtx->Unmap(staging, 0);
			}
			staging->Release();
			return pixel;
		}

		// The DX12/Vulkan path. Same result as the DX11 branch above, reached
		// through NVRHI staging instead of raw D3D11, which is possible
		// because these backends support non-immediate command lists.
		// DX12 / Vulkan: NVRHI staging readback with a deferred command list
		// (both backends support non-immediate lists).
		// The staging texture is REUSED across calls and only rebuilt when the
		// dimensions change -- creating one per readback would be a per-call
		// GPU allocation on a path that is already a stall.
		// The size check does not compare FORMAT: an attachment whose format
		// changed but whose dimensions did not would keep a staging texture
		// of the old format. Not reachable today (the format comes from the
		// specification and does not change without a new framebuffer), but the
		// check is narrower than the condition it is standing in for.
		if (!m_ReadbackStagingTexture || m_ReadbackStagingTexture->getDesc().width != texDesc.width ||
			m_ReadbackStagingTexture->getDesc().height != texDesc.height)
		{
			nvrhi::TextureDesc stagingDesc;
			stagingDesc.width = texDesc.width;
			stagingDesc.height = texDesc.height;
			stagingDesc.depth = 1;
			stagingDesc.arraySize = 1;
			stagingDesc.mipLevels = 1;
			// sampleCount 1 and mipLevels 1: a readback landing zone holds exactly
			// one texel of one mip of one slice.
			stagingDesc.sampleCount = 1;
			stagingDesc.format = texDesc.format;
			stagingDesc.dimension = nvrhi::TextureDimension::Texture2D;
			stagingDesc.debugName = "Framebuffer_ReadbackStaging";
			// Born in CopyDest, because the only thing that ever writes it is the
			// copyTexture call below -- and kept there, so the copy does not have
			// to transition it first.
			stagingDesc.initialState = nvrhi::ResourceStates::CopyDest;
			stagingDesc.keepInitialState = true;
			m_ReadbackStagingTexture = device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
		}
		if (!m_ReadbackStagingTexture)
			return -1;

		if (!m_ReadbackCommandList)
		{
			nvrhi::CommandListParameters params;
			// The reason this backend needs its OWN command list at all: the
			// engine's batched immediate list has already been opened this frame,
			// and a second open on the same list is invalid. Immediate execution
			// is disabled so NVRHI lets this list be built and submitted
			// independently of the frame's batched list.
			params.setEnableImmediateExecution(false);
			m_ReadbackCommandList = device->createCommandList(params);
		}
		if (!m_ReadbackCommandList)
			return -1;

		// open/close pairs recorded into the dedicated list. There is no Begin
		// marker here because NVRHI supplies one on open().
		m_ReadbackCommandList->open();
		m_ReadbackCommandList->copyTexture(m_ReadbackStagingTexture, nvrhi::TextureSlice(),
										   m_ColorTextures[attachmentIndex], nvrhi::TextureSlice());
		m_ReadbackCommandList->close();
		// Submitted on its own. The result is still asynchronous on DX12 --
		// the copy is queued, not finished -- which is why mapStagingTexture
		// below is the call that blocks.
		device->executeCommandList(m_ReadbackCommandList);

		size_t rowPitch = 0;
		void *mapped = device->mapStagingTexture(m_ReadbackStagingTexture,
												 nvrhi::TextureSlice(),
												 nvrhi::CpuAccessMode::Read,
												 &rowPitch);
		if (!mapped)
			return -1;

		int pixel = -1;
		// bytesPerBlock, not a hardcoded 4, so the byte-reassembly branch below
		// is correct for any colour format. Note the DX11 branch above
		// hardcodes 4 in both places instead.
		const uint32_t bytesPerPixel = nvrhi::getFormatInfo(texDesc.format).bytesPerBlock;
		const uint8_t *row = reinterpret_cast<const uint8_t *>(mapped) + size_t(y) * rowPitch;
		if (texDesc.format == nvrhi::Format::R32_SINT)
		{
			memcpy(&pixel, row + size_t(x) * 4, sizeof(int));
		}
		else
		{
			const uint8_t *px = row + size_t(x) * bytesPerPixel;
			// Guards the reassembly: it reads four bytes, so a format narrower
			// than 4 bytes per block would read past the texel. For such a format
			// the function returns the initial -1 rather than a garbage pixel,
			// which is why `pixel` is initialised and not assigned in that path.
			if (bytesPerPixel >= 4)
				pixel = int(px[0]) | (int(px[1]) << 8) | (int(px[2]) << 16) | (int(px[3]) << 24);
		}

		// Unmapped unconditionally after a successful map, and skipped on the
		// early-return above where nothing was mapped. Leaving a staging
		// texture mapped would pin its memory on every subsequent call.
		device->unmapStagingTexture(m_ReadbackStagingTexture);
		return pixel;
	}

	// Clears ONE colour attachment to a constant, for the entity-id
	// buffer rather than the colour one: the editor clears attachment 1
	// to -1 every frame so a stale id from the previous frame can never
	// be picked as a live entity.
	void NVRHIFramebuffer::ClearAttachment(uint32_t attachmentIndex, int value)
	{
		auto *context = NVRHIRendererAPI::GetCurrentContext();
		if (!context)
		{
			WL_CORE_ERROR("NVRHIFramebuffer::ClearAttachment: No NVRHI context available");
			return;
		}

		nvrhi::ICommandList *cmd = context->GetCommandList();
		if (!cmd || !context->IsCommandListOpen())
		{
			WL_CORE_ERROR("NVRHIFramebuffer::ClearAttachment: Command list not open");
			return;
		}

		// Bounds check with no else branch, and no log: an out-of-range
		// attachment index is silently ignored. Framebuffer.h documents this
		// asymmetry against the GL backend, which asserts instead.
		if (attachmentIndex < m_ColorTextures.size())
		{
			nvrhi::Color clearColor;
			clearColor.r = clearColor.g = clearColor.b = clearColor.a = static_cast<float>(value);
			// R32_SINT is an integer format — clearTextureFloat on an integer attachment
			// is undefined behavior on D3D12 (spec requires component type match).
			// Use the integer clear path; -1 as uint32_t is 0xFFFFFFFF which the
			// shader reads back as -1 in the signed interpretation.
			cmd->clearTextureUInt(m_ColorTextures[attachmentIndex], nvrhi::TextureSubresourceSet(), static_cast<uint32_t>(value));
		}
	}

	// Returns the texture HANDLE's pointer value truncated to 32 bits.
	// A 64-bit pointer does not survive that, so the returned value is
	// only usable as an identity token for comparison, never as an address.
	// Returning 0 for out-of-range makes "no attachment" distinguishable
	// from a real handle only by accident, since 0 is not a valid texture.
	uint32_t NVRHIFramebuffer::GetColorAttachmentRendererID(uint32_t index) const
	{
		if (index < m_ColorTextures.size())
		{
			return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m_ColorTextures[index].Get()));
		}
		return 0;
	}

	// Exposes the native SRV for ImGui, and the reason it is this narrow:
	// only D3D11 has a cached native view. On DX12 the same request
	// returned a descriptor built from the wrong object type, which ImGui
	// then bound as a GPU descriptor handle and sampled garbage.
	// The caller must branch on the backend and use a descriptor handle
	// on DX12 (see NVRHIContext::GetOrCreateImGuiDescriptor).
	void *NVRHIFramebuffer::GetColorAttachmentNativeSRV(uint32_t index) const
	{
		// NVRHI's D3D11 backend caches the SRV inside the texture (freed with it),
		// so requesting the native view is cheap and needs no local lifetime
		// management. Attachments were created with isShaderResource = true.
		//
		// BACKEND MATTERS: the requested ObjectType is not interchangeable across
		// D3D11/D3D12, and hardcoding D3D11 here made this return garbage on DX12.
		// D3D12 has no cached native view at all, so the caller must instead get a
		// GPU descriptor handle (see NVRHIContext::GetOrCreateImGuiDescriptor),
		// because ImGui's DX12 backend binds TexID as a descriptor handle value.
		if (index >= m_ColorTextures.size() || !m_ColorTextures[index])
			return nullptr;

		// Only D3D11 exposes a cached native SRV view. D3D12 has no equivalent
		// object, and asking for the D3D11 one produced a descriptor built from
		// the wrong object type -- which ImGui then bound as a GPU descriptor,
		// sampling garbage. Report "no native view" honestly so the caller can
		// build a descriptor handle instead.
		// Asks for the D3D11 view type unconditionally and reports "no native
		// view" when it comes back null. On DX12 the request simply fails,
		// which is the correct outcome -- there is no such object to return.
		// The asymmetry is deliberate and is what stops the DX12 path from
		// receiving a descriptor built against the wrong object type.
		if (auto *view = m_ColorTextures[index]->getNativeView(nvrhi::ObjectTypes::D3D11_ShaderResourceView).pointer)
			return view;
		return nullptr;
	}

	// Returns the attachment in whatever shape the ACTIVE ImGui backend binds as
	// TexID, which is the whole point: TexID is a per-backend token, so this has
	// to branch where the API is known rather than leaving the caller to guess.
	void *NVRHIFramebuffer::GetColorAttachmentImGuiTexID(uint32_t index) const
	{
		if (index >= m_ColorTextures.size() || !m_ColorTextures[index])
			return nullptr;

		// Same reach-the-context rule as every other method here: the device and
		// the API both live on the context, which a runtime API switch replaces.
		auto *context = NVRHIRendererAPI::GetCurrentContext();
		if (!context)
			return nullptr;

		// DX11: the cached native SRV IS the TexID. imgui_impl_dx11 does
		//     ID3D11ShaderResourceView* texture_srv = (ID3D11ShaderResourceView*)pcmd->GetTexID();
		// so the value that used to reach the editor unchanged is already correct,
		// and returning it here keeps one accessor for all three backends.
		if (context->GetAPI() == RendererAPI::API::NVRHI_DX11)
			return GetColorAttachmentNativeSRV(index);

		// DX12: ImGui binds TexID as a GPU DESCRIPTOR HANDLE
		//     texture_handle.ptr = (UINT64)pcmd->GetTexID();
		//     ctx->SetGraphicsRootDescriptorTable(1, texture_handle);
		// so the attachment must be turned into a descriptor in the ImGui SRV heap
		// before it can be shown. This is the fix for the blank DX12 raster
		// viewport: the editor used to pass the truncated nvrhi handle pointer,
		// which landed on an arbitrary descriptor and sampled nothing.
		if (context->GetAPI() == RendererAPI::API::NVRHI_DX12)
		{
			// The unwrap NVRHI offers for a DX12 texture, matching what
			// GetD3D12CurrentBackBufferResource does for the swap chain. It
			// returns null for a texture that belongs to a different backend,
			// which the null-handle branch below then reports as "no descriptor".
			void *resource = m_ColorTextures[index]->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);
			if (!resource)
				return nullptr;

			// Slot 2, because slot 0 is the font atlas (ImGui_ImplDX12_Init writes
			// it) and slot 1 is the DX12 Nova output (see
			// Renderer3D::GetRayTraceTargetNativeSRV). A FIXED slot means this
			// rewrites the same descriptor each frame rather than allocating a new
			// one per frame and exhausting the 64-descriptor heap. The raster and
			// ray-traced viewports are mutually exclusive per frame, but sharing
			// slot 1 would be a latent aliasing bug the moment that stopped being
			// true, and there are 62 free slots.
			D3D12_GPU_DESCRIPTOR_HANDLE gpu = context->GetOrCreateImGuiDescriptor(2, resource);
			if (gpu.ptr == 0)
			{
				// Returning the resource pointer here would be WORSE than nullptr:
				// ImGui would bind it as a descriptor and sample garbage, whereas a
				// null TexID lets the caller skip the draw entirely.
				WL_CORE_ERROR("NVRHIFramebuffer: failed to build an ImGui descriptor for colour attachment {0} on DX12", index);
				return nullptr;
			}
			// The handle's VALUE, not the struct: ImGui stores TexID as an opaque
			// pointer and its DX12 backend reinterprets it as a UINT64 address.
			return (void *)(size_t)gpu.ptr;
		}

		// Vulkan: imgui_impl_vulkan binds TexID as a VkDescriptorSet --
		//     VkDescriptorSet desc_set[1] = { (VkDescriptorSet)pcmd->TextureId };
		//     vkCmdBindDescriptorSets(...);
		// -- so the attachment has to be turned into a descriptor before ImGui can
		// show it. ImGui_ImplVulkan_AddTexture is the only thing that produces one,
		// and all three of its arguments are things the engine already owns: the
		// image view is the attachment's own (NVRHI caches it on the texture, so the
		// view dies with the texture and not before), the sampler is NVRHIContext's,
		// and the descriptor set LAYOUT is created by ImGui itself in
		// ImGui_ImplVulkan_CreateDeviceObjects. The set is allocated from the engine's
		// Vulkan ImGui descriptor pool, which carries
		// VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT.
		//
		// THIS IS WHAT MAKES THE VULKAN VIEWPORT NON-BLANK. It used to return
		// nullptr here, so the editor skipped the draw entirely.
		if (context->GetAPI() == RendererAPI::API::NVRHI_Vulkan)
		{
			// AddTexture dereferences ImGui's backend data with no null check --
			// ImGui_ImplVulkan_GetBackendData asserts on it -- so calling it before
			// ImGui_ImplVulkan_Init, or after its Shutdown, is a hard crash rather
			// than a failed lookup. IsVulkanImGuiInitialized is the only readiness
			// signal the context publishes, and InitVulkanImGuiResources has already
			// returned by the time a frame can reach here, which is exactly when
			// ImGuiLayer has run ImGui_ImplVulkan_Init.
			//
			// CACHED, because AddTexture ALLOCATES a set every call and a viewport
			// that never resizes would otherwise leak one descriptor per frame out
			// of a 1000-slot pool until the UI failed to bind anything.
			//
			// THE VIEW IS NOT PART OF THE KEY ON PURPOSE. getNativeView takes the
			// texture's lock and can create a VkImageView on first use, which is not
			// free, so keying on it would put that cost back on the per-frame path.
			// The texture handle is the same identity in every frame, and NVRHI maps
			// both to the same cached view, so keying on the texture is sufficient.
			// The shared helper retires the previous cached set before replacing it.
			//
			// THE LAYOUT ARGUMENT IS NOT A FREE PARAMETER, AND IT IS THE REASON
			// NOTHING ELSE HERE TOUCHES RESOURCE STATE. NVRHI's Vulkan backend
			// renders through dynamic rendering, whose implicit transitions leave
			// a colour attachment in eColorAttachmentOptimal, so the descriptor has
			// to name eShaderReadOnlyOptimal and something has to move the image
			// there. It cannot be an NVRHI state change: the engine never calls
			// setPermanentTextureState, so this texture has NO permanent state and
			// NVRHI tracks it normally -- but the scene's command list is closed and
			// executed before ImGui draws, so a request recorded on it now would be
			// flushed a frame late. Nor can initialState be widened to
			// RenderTarget | ShaderResource as VulkanNova does for its history
			// texture: those two bits map to eColorAttachmentOptimal and
			// eShaderReadOnlyOptimal, which are different layouts, and
			// convertResourceState asserts on exactly that and then silently picks
			// the wrong one in release. The transition is recorded instead, by
			// NVRHIContext::RecordVulkanImGuiAttachmentTransition, once this
			// accessor has told it which image the pass will sample.
			//
			// REGISTERED EVERY FRAME, not only when the set is rebuilt: the transition
			// has to be recorded again every frame, because NVRHI's dynamic rendering
			// moves the image back to eColorAttachmentOptimal at the start of each
			// frame's scene pass.
			//
			// The DESCRIPTOR SET VALUE, not a texture or view pointer: imgui_impl_vulkan
			// casts TexID straight back to VkDescriptorSet, and handing it anything
			// else binds an arbitrary descriptor set and samples nothing.
			void *texID = GetVulkanImGuiTexID(m_ColorTextures[index]);
			if (texID)
				context->SetVulkanImGuiAttachment(m_ColorTextures[index]);
			return texID;
		}

		// No backend matched, which means the context reported an API this accessor
		// does not implement. Unreachable through the two branches above plus OpenGL
		// (which never constructs an NVRHIFramebuffer), but returning null is the
		// only safe answer if it ever is reached.
		return nullptr;
	}

	// Returns a Vulkan ImGui TexID for an arbitrary NVRHI texture and registers it as a compute output.
	void *NVRHIFramebuffer::GetExternalTextureImGuiTexID(nvrhi::TextureHandle texture) const
	{
		auto *context = NVRHIRendererAPI::GetCurrentContext();
		if (!context || context->GetAPI() != RendererAPI::API::NVRHI_Vulkan)
			return nullptr;

		void *texID = GetVulkanImGuiTexID(texture);
		if (texID)
			context->SetVulkanImGuiComputeOutput(texture);
		return texID;
	}

	// Handle accessor returning the NVRHI handle itself rather than a raw
	// pointer, so the caller's reference keeps the texture alive.
	nvrhi::TextureHandle NVRHIFramebuffer::GetColorAttachment(uint32_t index) const
	{
		if (index < m_ColorTextures.size())
		{
			return m_ColorTextures[index];
		}
		return nullptr;
	}

}
