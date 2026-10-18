// Include guard for the Texture interface hierarchy.
#pragma once

// For the path-based Create() factory on Texture2D.
#include <string>

// The umbrella core header, and the reason Ref<uint32_t>-style names and
// uint32_t resolve here. Unlike Shader.h and VertexArray.h, this header
// DOES include Core.h, so it is self-contained for those names.
#include "Wasteland/Core/Core.h"

namespace Wasteland {

	// Backend-agnostic texture interface: dimensions, a native handle, and the
	// ability to upload pixels.

	// Abstract base for Texture2D, which adds resizing and native-resource
	// accessors. The name is deliberately not suffixed 2D here, though
	// the derived class is -- so `Texture` is the common ancestor for any
	// dimension of texture.
	class Texture
	{
	public:
		// Virtual so a Texture* can delete a concrete GL or NVRHI texture and have
		// its GPU resource released.
		virtual ~Texture() = default;

		// Width in texels. Sampled from the driver, so after a driver-side format
		// change this may differ from what was requested at Create() time.
		virtual uint32_t GetWidth() const = 0;
		// Height in texels, same caveat as GetWidth().
		virtual uint32_t GetHeight() const = 0;
		// The backend-native texture handle as an integer.

		// Sized and typed for OpenGL, where a texture is a GLuint name. Under NVRHI a
		// handle is a pointer-sized ID, which does not fit uint32_t -- that is why
		// Texture2D also offers GetNativeSRV() below, and why ImGuiLayer carries a
		// separate DX11 white-SRV fallback for non-font submissions. Any NVRHI
		// implementation truncating its handle into this uint32_t is lossy.
		virtual uint32_t GetRendererID() const = 0;

		// Uploads pixel data.

		// `data` is void* rather than const void* although the pixels are only read,
		// so a caller holding a const pixel array cannot use this directly.

		// `size` is in BYTES, and is the engine's responsibility to keep consistent
		// with the width/height/format the texture was created with -- nothing checks
		// it, so an undersized upload is a partial, undefined-content texture.
		virtual void SetData(void* data, uint32_t size) = 0;

		// Binds to a texture unit. Const: GL/NVRHI binding state is global, not
		// per-object.

		// Defaults to unit 0. A multi-texture material binds each of its maps to
		// distinct slots and relies on the shader's sampler bindings lining up;
		// forgetting to increment the slot silently overwrites unit 0's binding.
		virtual void Bind(uint32_t slot = 0) const = 0;

		// Identity comparison, declared as an operator rather than a named method.

		// NOT defined here -- each backend decides what equality means for it.
		// Without a definition, a `Texture == Texture` comparison will not compile
		// on an abstract type, so callers must go through a concrete pointer or a
		// Ref, whose comparison compares pointers rather than calling this. This
		// operator therefore appears effectively unused: std::shared_ptr's operator==
		// does not dispatch to it.
		virtual bool operator==(const Texture& other) const = 0;
	};
	
	// The concrete 2D texture used by all 2D and 3D rendering.
	class Texture2D : public Texture
	{
	public:
		// Allocate a blank texture of the given pixel dimensions.

		// No format, no mip flag, no data: the two-argument Create always produces a
		// fixed default format (RGBA, single level), which is why there is no way
		// to request a different format through this factory.
		static Ref<Texture2D> Create(uint32_t width, uint32_t height);
		// Load from an image file, decoding and uploading it.

		// Overload rather than a defaulted trailing parameter, so the two call
		// forms are distinguished by argument count and cannot be confused.
		static Ref<Texture2D> Create(const std::string& path);

		// Change the texture's dimensions in place, discarding contents.

		// Under OpenGL the conventional implementation is glTexImage2D with a null
		// pointer, which reallocates; a DSA-only backend would use glTextureStorage2D
		// plus glTextureSubImage2D. Either way the previous pixels are gone, so a
		// caller expecting to preserve content across a resize gets garbage.
		virtual void Resize(uint32_t width, uint32_t height) = 0;

		// Native GPU view of this texture for ImGui texture submission under
		// NVRHI APIs (returns an ID3D11ShaderResourceView* on DX11; nullptr
		// elsewhere, where integer GetRendererID() is used instead).
		// Defaulted to nullptr, so this is opt-in per backend.

		// Returning a bare void* sidesteps the header having to name ID3D11Shader-
		// ResourceView, which would drag d3d11.h into every consumer. The concrete
		// DX11 texture overrides it with a correctly-typed return.

		// The void* also means the CALLER must know which backend it is holding --
		// there is no way to tell from the return whether it is an SRV or something
		// else. ImGuiLayer::m_D3D11WhiteSRV is the known consumer.
		virtual void *GetNativeSRV() const { return nullptr; }

		// Raw backend resource for APIs that build their own descriptors (DX12 Nova).
		// This is distinct from an SRV; most backends do not expose one here.
		virtual void *GetNativeTextureResource() const { return nullptr; }
	};

}