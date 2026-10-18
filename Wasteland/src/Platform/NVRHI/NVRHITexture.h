// Header guard. This is one of the few headers in the tree that names a
        // third-party SDK in an include, so anything including it inherits a
        // dependency on nvrhi being on the include path.
#pragma once

// The backend-agnostic base. Including the base here rather than
        // forward-declaring is required: the class derives from Texture2D and
        // overrides virtuals declared there.
#include "Wasteland/Renderer/Texture.h"
// The whole point of this backend layer. NVRHI is an abstraction over D3D11,
        // D3D12, Vulkan and GL, which is why one implementation of this class
        // covers four graphics APIs.
#include <nvrhi/nvrhi.h>

namespace Wasteland {

	// The name says 2D; the class is still called Texture2D in the base, and
	// it creates a 2D texture with a mip chain. Neither the depth nor the
	// array layer of D3D is exposed, so this is the flat case only.
	class NVRHITexture2D : public Texture2D
	{
	public:
		// Empty-texture ctor: allocates but does not upload. SetData is then the
		// caller's responsibility, and nothing tracks whether it was called --
		// an uninitialised texture is representable.
		NVRHITexture2D(uint32_t width, uint32_t height);
		// File-backed ctor. Overload resolution is unambiguous with the (uint32_t,
		// uint32_t) pair above unless someone passes two integers convertible
		// from a string, which is not possible.
		NVRHITexture2D(const std::string& path);
		// DEFECT (non-virtual destructor through a base pointer, documented not
		// fixed -- and this is the same defect class as OpenGLShader's): the
		// class derives from Texture2D, declares a destructor, and does NOT
		// mark it virtual. Deleting through a Texture2D* would skip this
		// destructor and leak the nvrhi texture handle. It is safe only because
		// every owner appears to hold a Ref/shared_ptr.
		virtual ~NVRHITexture2D();

		// Trivial accessors, inline. The values are CPU-side copies of what the GPU
		// texture was created with, so after a Resize they report the NEW size
		// only if the implementation updates these members -- a requirement the
		// interface does not state.
		virtual uint32_t GetWidth() const override { return m_Width; }
		virtual uint32_t GetHeight() const override { return m_Height; }
		// Not inline, so it lives in the .cpp: converting an nvrhi handle to a
		// uint32_t id for the interface layer is the one place needing backend
		// knowledge. Returning a raw integer rather than the handle is what
		// keeps nvrhi types out of the abstraction.
		virtual uint32_t GetRendererID() const override;

		// A raw void* plus a byte size, matching the base interface. DEFECT (no
		// null check, documented not fixed): nothing here validates that data is
		// non-null or that size matches width*height*4, so a mismatch is a
		// silent out-of-bounds upload. The size parameter is the only thing
		// standing between the caller and a GPU-side overrun.
		virtual void SetData(void* data, uint32_t size) override;

		// const because binding is device state, not a change to this texture. The
		// default slot 0 means a single-texture shader needs no argument.
		virtual void Bind(uint32_t slot = 0) const override;

		virtual void Resize(uint32_t width, uint32_t height) override;

		// Comparing by native handle rather than by path or dimensions: two
		// distinct uploads of the same file are different textures, which is
		// what a resource cache wants.
		virtual bool operator==(const Texture& other) const override
		{
			// dynamic_cast, so comparing two textures of DIFFERENT backends is
			// well-defined -- it yields null and the comparison returns false
			// rather than being undefined. The cost is an RTTI lookup per
			// comparison, which is why this is not called in a per-frame path.
			const NVRHITexture2D* otherNVRHI = dynamic_cast<const NVRHITexture2D*>(&other);
			// The short-circuit also protects against a null m_Texture: comparing two
			// null handles returns true, so two failed textures would compare
			// EQUAL and a cache would treat them as the same resource.
			return otherNVRHI && m_Texture == otherNVRHI->m_Texture;
		}

		// NVRHI-specific accessors
		// NVRHI-specific escape hatches, public so NVRHI-aware callers can build
		// their own descriptors. These are the ONLY members that expose nvrhi
		// types in their signature, which is the seam: code using them is not
		// backend-portable.
		nvrhi::TextureHandle GetTexture() const { return m_Texture; }
		nvrhi::SamplerHandle GetSampler() const { return m_Sampler; }

		// Native D3D11 SRV for ImGui texture submission (DX11 mode)
		// Overridden in the HEADER, inline, because the body needs the nvrhi type.
		// Returning void* is the same untyped escape hatch as
		// Renderer3D::GetRayTraceTargetNativeSRV: the caller must know which
		// backend it is on to interpret the pointer.
		// Available only when the NVRHI device is running on DX11.
		virtual void *GetNativeSRV() const override
		{
			if (!m_Texture)
				return nullptr;
			return m_Texture->getNativeView(nvrhi::ObjectTypes::D3D11_ShaderResourceView).pointer;
		}

		// DX12 Nova creates SRVs in its own descriptor heap, so it needs the
		// underlying ID3D12Resource rather than a D3D11 SRV or NVRHI view.
		virtual void *GetNativeTextureResource() const override
		{
			if (!m_Texture)
				return nullptr;
			return m_Texture->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource).pointer;
		}

	private:
		// Private because constructing the handle is the ctor's job; a caller
		// cannot ask for a differently-sized texture without going through
		// Resize.
		void CreateTexture(uint32_t width, uint32_t height);
		// Separate from CreateTexture: a sampler is an independent object with its
		// own filter state, and nothing in this interface exposes the filter
		// parameters, so the configuration is fixed here.
		void CreateSampler();
		// Takes a command list rather than submitting itself. That is the NVRHI
		// idiom: resource uploads are recorded into a command list so they
		// are ordered against the draw calls rather than racing them.
		// The raw cmd pointer means the caller must supply a list recorded on
		// a context that knows this texture exists.
		void UploadMipChain(nvrhi::ICommandList* cmd, const void* baseData);

		// CONST, and initialised in the member list: the source path is immutable
		// for the texture's lifetime, so Resize and SetData cannot change it.
		// Empty for the (width, height) ctor, which means nothing records where
		// a procedurally-created texture came from.
		// DEFECT (uninitialised, documented not fixed): a std::string member is
		// initialised correctly by default construction, but the width and
		// height below are NOT -- see the next entry.
		const std::string m_Path;
		// DEFECT (uninitialised members, documented not fixed): no in-class
		// initialisers and no mention in a member list, so these hold
		// indeterminate values until CreateTexture runs. GetWidth() called
		// before construction finishes, or on an object whose ctor threw
		// between these two members and m_Texture, would return garbage
		// rather than 0. `= 0` on both would close this at zero cost.
		uint32_t m_Width, m_Height;
		// An nvrhi handle, not a pointer: it is a small POD wrapper around a driver
		// object with its own lifetime rules, and its default value is the null
		// handle. That default is what the GetNativeSRV null check relies on.
		nvrhi::TextureHandle m_Texture;
		// Held alongside the texture so it survives with it. Sampler state is
		// immutable here -- there is no setter -- so the filter configuration
		// baked into CreateSampler applies for the texture's whole lifetime.
		nvrhi::SamplerHandle m_Sampler;
	};

}
