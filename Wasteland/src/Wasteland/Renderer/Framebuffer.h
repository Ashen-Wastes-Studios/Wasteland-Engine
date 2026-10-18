// Framebuffer.h -- the engine's offscreen/render-target description and the
// abstract interface both graphics backends implement.
//
// Three layers live here:
//   1. FramebufferTextureFormat   -- which GPU format an attachment uses
//   2. FramebufferSpecification   -- size, attachment list, sample count
//   3. Framebuffer                -- the backend object (Bind/Resize/ReadPixel)
//
// The spec structs are plain data and are shared verbatim with Python:
// Bindings.cpp exposes Width, Height, Attachments, Samples and SwapChainTarget
// as read/write attributes, and Framebuffer's methods as bound functions.
// Renaming a member here breaks a script-facing name.
#pragma once

// For Ref<T>, the engine's intrusive-style refcounted pointer, used by the
// Create() factory's return type below.
#include "Wasteland/Core/Core.h"

#include <glm/glm.hpp>

namespace Wasteland
{

	// Scoped enum so these short names (RGBA8, Depth) do not leak into namespace
	// Wasteland, where Texture2D's own format enum also lives.
	//
	// Every enumerator here must be handled by BOTH backends' format-mapping
	// functions -- FramebufferTextureFormatToNVRHI in NVRHIFramebuffer.cpp and
	// WastelandFBTextureFormatToGL in OpenGLFramebuffer.cpp. Both end with a
	// WL_CORE_ASSERT(false) default, so an unmapped value is a loud debug failure
	// rather than a silent wrong format.
	enum class FramebufferTextureFormat
	{
		None = 0,

		// Grouping comment for the two colour formats below. Attachment order in a
		// FramebufferSpecification's Attachments vector is what assigns slot numbers,
		// so this grouping is documentation, not structure -- the vector is flat.
		// Color
		// Ordinary unorm colour, the editor viewport's default.
		// Maps to nvrhi::Format::SRGBA8_UNORM and GL_RGBA8.
		// Read back as four bytes reassembled little-endian into an int (see
		// ReadPixel), which is how an entity id written as a colour survives the trip.
		RGBA8,
		// Single signed-integer channel: an entity id for GPU picking.
		// Maps to nvrhi::Format::R32_SINT and GL_RED_INTEGER. Both backends force
		// GL_NEAREST filtering on it -- integer textures are not filterable, and
		// ReadPixel's memcpy-based path assumes exact texel fetches.
		// Also the only format ReadPixel can return a full 32-bit value from; RGBA8
		// readback packs its four bytes into one int instead.
		RED_INTEGER,

		// Depth/stencil
		// 24-bit depth plus 8-bit stencil, the single combined depth format the
		// engine supports. Maps to nvrhi::Format::D24S8; OpenGL passes it as a
		// GL_DEPTH24_STENCIL8 internal format with GL_UNSIGNED_INT_24_8 data.
		//
		// Depth is created WITHOUT shader-resource access on purpose: on D3D11/D3D12 a
		// D24S8 texture bound as both DSV and SRV must be typeless, and NVRHI only
		// does that when desc.isTypeless is set. Sampling depth in a shader is
		// therefore not currently possible through this attachment.
		DEPTH24STENCIL8,

		// Defaults
		// An ALIAS, not a distinct format: `Depth` and `DEPTH24STENCIL8` are the same
		// value, so a switch case on either matches both. This is the spelling new
		// specs are expected to use.
		//
		// Deliberately not listed in the "Defaults" group above despite the comment,
		// because it adds no enumerator of its own.
		Depth = DEPTH24STENCIL8
	};

	// One attachment's format. A struct rather than a bare enum so per-attachment
	// properties can be added later without changing every call site -- the
	// TODO below is exactly that intent.
	struct FramebufferTextureSpecification
	{
		// Required because the converting constructor below is user-provided, which
		// would otherwise suppress the implicit default constructor.
		FramebufferTextureSpecification() = default;
		// Implicitly converting from the enum, which is what lets a spec be written
		// as { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::Depth }
		// with no explicit FramebufferTextureSpecification wrapper.
		FramebufferTextureSpecification(FramebufferTextureFormat format)
			// Empty body: every field is either the parameter or has an NSDMI. Colon form
			// rather than a braced init-list because the member declaration comes after
			// the constructors in this struct.
			: TextureFormat(format) {}

		// NSDMI to None, so a default-constructed attachment is inert: None matches
		// no case in either backend's mapping switch and trips their assert rather
		// than silently allocating an uninitialised texture.
		FramebufferTextureFormat TextureFormat = FramebufferTextureFormat::None;
		// Real gap, not just a note: sampler state for an attachment cannot be
		// requested here. The OpenGL backend hardcodes it instead -- GL_NEAREST for
		// GL_R32I, GL_LINEAR otherwise, with CLAMP_TO_EDGE on all three axes -- so
		// REPEAT or NEAREST on an RGBA8 attachment cannot be expressed.
		// On NVRHI there is no equivalent sampler selection for a render target at
		// all; the texture is created with isShaderResource and default params.
		// TODO: filtering/wrap
	};

	// Groups the attachments of one framebuffer. The name suggests a single
	// attachment but the member is a vector -- it is the framebuffer's whole
	// attachment list, kept behind its own spec type so a future per-group
	// property (like mip levels) has somewhere to live.
	struct FramebufferAttachmentSpecification
	{
		// Present because the two converting constructors below would otherwise
		// delete it. Yields an empty attachment list, which both backends accept:
		// the framebuffer is created with no color and no depth attachment.
		FramebufferAttachmentSpecification() = default;
		// This is the overload that makes brace-init lists work in specs:
		// `Attachments{ {RGBA8}, {Depth} }` needs a constructor taking an
		// initializer_list, since a braced list cannot deduce the vector's template
		// parameter. Each element converts via the single-format constructor above.
		//
		// Passing BY VALUE and then copy-constructing into Attachments is one extra
		// copy of a vector of 4-byte enums. Harmless, and it matches the shape
		// produced automatically when no such constructor exists.
		FramebufferAttachmentSpecification(std::initializer_list<FramebufferTextureSpecification> attachments)
			// Direct-initialises the vector from the initializer_list, so the parameter's
			// copy is elided into the member.
			: Attachments(attachments) {}
		// The by-reference overload. It is needed alongside the initializer_list one
		// because a braced list prefers the initializer_list ctor while a named
		// vector argument needs this -- without it, passing a vector would fail.
		//
		// Copies rather than moves: the parameter is a const reference, so the data
		// cannot be stolen from the caller's vector.
		FramebufferAttachmentSpecification(const std::vector<FramebufferTextureSpecification>& attachments)
			: Attachments(attachments) {}

		// No NSDMI -- unlike the format field above. It is therefore
		// value-initialised to empty by the defaulted constructor anyway, so the two
		// members differ in style but not in outcome.
		//
		// ORDER IS SEMANTIC. Both backends walk this vector in order: depth textures
		// go to the depth slot regardless of position, while colour textures are
		// numbered by the order they were pushed (m_ColorTextures.size() becomes the
		// debug name and the slot index in NVRHIFramebuffer::Invalidate). Reading
		// attachment N therefore depends on how many colour attachments preceded it.
		std::vector<FramebufferTextureSpecification> Attachments;
	};

	// Everything needed to create or recreate a framebuffer. Held by value inside
	// each backend (NVRHIFramebuffer::m_Specification) and mutated in place by
	// Resize, so it is the authoritative record of the target's current size.
	struct FramebufferSpecification
	{
		// NO initialisers, and they are first -- so a FramebufferSpecification built
		// with `{}` or default-constructed has uninitialised dimensions.
		// DEFECT: this is the one place in this header where a default is genuinely
		// unsafe. NVRHIFramebuffer::Invalidate does test for 0 and warns "Skipping
		// Invalidate with zero size", but an uninitialised uint32_t is only 0 by
		// luck, so a spec that sets only Attachments creates a framebuffer of
		// garbage size. The Python binding exposes both fields read/write, and a
		// script constructing FramebufferSpecification() without assigning them
		// hits this directly.
		uint32_t Width, Height;
		// By value: cheap in practice (a vector header plus up to one heap block),
		// and copying it into the backend is what makes the spec self-contained.
		FramebufferAttachmentSpecification Attachments;
		// Default 1 = no MSAA. Both backends clamp it on use rather than trusting it:
		// `Samples > 0 ? Samples : 1` in NVRHIFramebuffer::Invalidate, and
		// `samples > 1` to choose GL_TEXTURE_2D_MULTISAMPLE in OpenGLFramebuffer.
		// So 0 means 1, and a multisample target needs a manual resolve before its
		// texture can be sampled -- which nothing in this header exposes.
		uint32_t Samples = 1;

		// Appears to be DEAD in the engine's C++ path. Both backends' Invalidate
		// build their own framebuffer from Width/Height and never read this flag;
		// the swapchain is reached instead by unbinding (NVRHIFramebuffer::Unbind
		// calls SetRenderTargetFramebuffer(nullptr)).
		// DEFECT: it is bound to Python as a read/write attribute
		// (Bindings.cpp:1835-1836), so a script can set it and see no effect -- a
		// flag that looks meaningful and is not read is worse than no flag.
		bool SwapChainTarget = false;
	};

	// Abstract, all-virtual except for GetColorAttachmentNativeSRV below, which
	// has a default body so a backend with no native view need not override it.
	// Created exclusively through the Create() factory at the bottom -- the
	// constructors of both backends take a FramebufferSpecification, not a spec
	// plus a device.
	class Framebuffer
	{
	public:
		// Virtual and defaulted, so `delete` through Ref<Framebuffer> runs the derived
		// destructor. That matters for NVRHIFramebuffer, whose handles must be
		// released while the nvrhi::IDevice is still alive.
		virtual ~Framebuffer() = default;

		// Not a real binding call on NVRHI: it stores the framebuffer on the context
		// (SetRenderTargetFramebuffer) so the next DrawIndexed picks it up via
		// GraphicsState. On OpenGL it is a real glBindFramebuffer.
		//
		// Asymmetric with Unbind(): NVRHI's Unbind unconditionally passes nullptr,
		// i.e. back to the swapchain, so a framebuffer cannot be restored to a
		// previously bound one without re-binding it.
		virtual void Bind() = 0;
		// Backends differ in cost: OpenGL binds framebuffer 0, NVRHI clears the
		// context's target pointer. Neither is paired with a save, so the previous
		// target is not remembered.
		virtual void Unbind() = 0;

		// Destructive on both backends: NVRHIFramebuffer::Resize updates
		// m_Specification and calls Invalidate(), which drops and recreates every
		// attachment texture. Any nvrhi handle or ImGui descriptor cached against
		// the old attachments is stale afterwards -- which is why
		// GetColorAttachmentRendererID must be re-read after a resize rather than
		// cached by the caller.
		//
		// No-ops when the size is unchanged or either dimension is 0, so a minimised
		// window does not destroy the attachments.
		virtual void Resize(uint32_t width, uint32_t height) = 0;
		// Synchronous GPU readback of ONE texel, used for entity picking.
		// Expensive by design: it creates a staging texture and a command list (NVRHI)
		// or a D3D11 staging resource, copies, maps, and reads a single int. It is not
		// meant to be called per pixel.
		//
		// Returns -1 for every failure mode -- out-of-range attachment, out-of-range
		// coordinates, no device, failed staging creation -- so a caller cannot
		// distinguish "nothing there" from "readback failed". An entity handle of
		// -1 is not a valid one, which is what keeps that ambiguity harmless.
		//
		// x/y are ints and validated against the texture's own descriptor, not the
		// framebuffer's Width/Height.
		virtual int ReadPixel(uint32_t attachmentIndex, int x, int y) = 0;

		// Fills an attachment with a CONSTANT value, splatted across all channels.
		// On NVRHI it issues clearTextureFloat with the int cast to float, so clearing
		// an R32_SINT picking attachment with a large entity id loses precision above
		// 2^24 and any value is stored as a float. The integer clear path
		// (clearTextureUint) is not used.
		//
		// Silently does nothing when attachmentIndex is out of range on NVRHI -- there
		// is no else branch, and no error is logged.
		virtual void ClearAttachment(uint32_t attachmentIndex, int value) = 0;

		// Returns the raw nvrhi::TextureHandle pointer value, truncated to uint32_t --
		// NVRHIFramebuffer casts the handle's address through uintptr_t, so this is a
		// pointer, not a GPU-visible id. Under ASLR the low 32 bits are not stable
		// across runs, which is fine for an opaque ImGui TexID but means it must
		// never be persisted.
		//
		// Returns 0 when index is out of range, and 0 is indistinguishable from a
		// null texture. Also truncating to 32 bits on a 64-bit build can collide
		// between two live attachments in principle.
		//
		// Preferred over GetColorAttachmentNativeSRV below where the backend needs
		// only an opaque texture identifier.
		virtual uint32_t GetColorAttachmentRendererID(uint32_t index = 0) const = 0;

		// Native GPU view of a color attachment for immediate ImGui texture use
		// (D3D11 SRV pointer under NVRHI; nullptr for OpenGL / unsupported).
		// The ONE virtual with a body, and it is not pure: a backend that has no
		// native SRV inherits a nullptr answer instead of being forced to override.
		//
		// Existing comment above it says D3D11 only. That is accurate but incomplete,
		// and the backend's own comments are blunter about why: on DX12 the
		// D3D11_ShaderResourceView object type does not exist, so asking for it
		// produced a descriptor built from the wrong object type and ImGui sampled
		// garbage. NVRHIFramebuffer returns nullptr there, forcing the DX12 caller to
		// build a GPU descriptor handle via NVRHIContext::GetOrCreateImGuiDescriptor
		// instead. OpenGL inherits the base nullptr.
		virtual void *GetColorAttachmentNativeSRV(uint32_t index = 0) const { return nullptr; }

		// The value ImGui's ACTIVE renderer backend expects in ImDrawCmd::TexID
		// for this attachment, in whatever form that backend needs it.
		//
		// WHY THIS IS NOT GetColorAttachmentNativeSRV: TexID is not a texture, it
		// is a backend-specific binding token, and one void* cannot be all four
		// of the things it has to be. imgui_impl_dx11 casts it to
		// ID3D11ShaderResourceView*, imgui_impl_dx12 binds it as a root GPU
		// DESCRIPTOR HANDLE, imgui_impl_vulkan binds it as a VkDescriptorSet and
		// imgui_impl_opengl3 reads it as a GLuint name. A caller that falls back
		// to "whatever handle I have" therefore binds garbage on three of them --
		// which is precisely what happened on DX12, where a truncated
		// nvrhi::TextureHandle pointer was bound as a descriptor and the draw
		// sampled nothing.
		//
		// nullptr is a real answer, not a failure to be worked around: it means
		// this backend has no such representation (Vulkan today, where
		// ImGui_ImplVulkan_AddTexture is the only producer of a descriptor set
		// and the engine does not expose one), and the caller must SKIP the draw
		// rather than substitute something else.
		//
		// Defaulted, like GetColorAttachmentNativeSRV above: a backend with
		// nothing to contribute inherits nullptr instead of being forced to
		// override. OpenGL inherits it and keeps using the integer
		// GetColorAttachmentRendererID, whose value IS a valid GL name.
		virtual void *GetColorAttachmentImGuiTexID(uint32_t index = 0) const { return nullptr; }

		// By const reference: the backend owns the spec and mutates it in Resize, so
		// returning a reference keeps callers in step with a resize instead of
		// handing them a stale copy. The reference is invalidated by the destruction
		// of the framebuffer, and by nothing else.
		virtual const FramebufferSpecification &GetSpecification() const = 0;

		// The single construction path, defined in Framebuffer.cpp: it switches on
		// Renderer::GetAPI() and returns CreateRef<OpenGLFramebuffer> or
		// CreateRef<NVRHIFramebuffer>, collapsing all three NVRHI enumerators onto the
		// latter.
		//
		// Returns nullptr when the API is None or unrecognised (both asserted in a
		// debug build), so callers in a release build get a null Ref with no
		// diagnostic. Every subsequent call on the result would be a null deref.
		//
		// Ref<> rather than a raw pointer: the framebuffer is shared between the
		// editor, the renderers and the ImGui layer.
		static Ref<Framebuffer> Create(const FramebufferSpecification &spec);
	};

}