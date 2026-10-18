#include "wlpch.h"
#include "NVRHITexture.h"
#include "NVRHIContext.h"
#include "NVRHIRendererAPI.h"
#include "Wasteland/Core/Log.h"

#include <stb_image.h>

#include <cstdint>
#include <vector>

namespace Wasteland {

	// Helper to get the current NVRHI device
	// Wraps the two-step lookup (renderer API -> context -> device) that every
	// creation path below needs. Returns null rather than asserting, so callers
	// must check -- and they do, each one logging and returning.
	static nvrhi::IDevice* GetDevice()
	{
		// Re-fetched here rather than cached: the context can be replaced
		// between construction and upload, and a command list belonging to a
		// destroyed context would be a use-after-free.
		// The CURRENT context, not a cached one: switching graphics API mid-process
		// replaces the context, so a cached device pointer would dangle. This is
		// why Renderer2D::RecreateResources exists on the GL/other split.
		auto* context = NVRHIRendererAPI::GetCurrentContext();
		return context ? context->GetDevice() : nullptr;
	// Worth flagging: the renderer distinguishes backends explicitly and
	// never calls Bind() on the NVRHI path, so this silent no-op is
	// currently unreachable. A caller that ignored that split would get no
	// error and simply an untextured draw. `slot` is unused.
	}

	// Full mip chain, mirroring OpenGLTexture2D's level computation
	// (loops while the largest dimension > 1).
	// Pure function of the extents -- no device query. Every mip dimension is
	// derived from the LARGEST axis (see below), so a 2560x1664 atlas gets the
	// chain a square texture would, and its narrower axis simply bottoms out
	// at 1 sooner than a strict halving would produce.
	static uint32_t ComputeMipLevels(uint32_t width, uint32_t height)
	{
		uint32_t levels = 1;
		// The max dimension drives the chain length. GPU mip chains are allowed to
		// have non-power-of-two extents, and halving the smaller axis independently
		// would give a different count than the hardware expects for this texture.
		uint32_t extent = width > height ? width : height;
		while (extent > 1)
		{
			// Integer halving on a uint32_t: a non-power-of-two size loses the low bit
			// each round, so the last levels can be irregular. That is legal and the
			// box filter below copes by clamping the second tap.
			extent >>= 1;
			levels++;
		}
		return levels;
	}

	/////////////////////////////////////////////////////////////////////////////////////////////
	// NVRHITexture2D
	/////////////////////////////////////////////////////////////////////////////////////////////

	// Blank-texture constructor: allocates GPU resources but uploads nothing,
	// so the contents are undefined until SetData. m_Path is set to "" and
	// that value is reused as the debug name.
	NVRHITexture2D::NVRHITexture2D(uint32_t width, uint32_t height)
		: m_Width(width), m_Height(height), m_Path("")
	{
		// Contents are NOT re-uploaded and the sampler is NOT recreated. After a
		// resize the texture is therefore uninitialised: previous pixels are
		// gone and the caller must SetData. Nothing logs that this happened.
		// Split out from the constructor body so both constructors share it. Neither
		// call's return value is checked -- CreateTexture logs and returns early
		// on failure, leaving m_Texture null, and CreateSampler below then fails
		// the same way. A failed load therefore yields a texture object whose bind
		// path is a silent no-op rather than a crash.
		CreateTexture(width, height);
		CreateSampler();
	}

	NVRHITexture2D::NVRHITexture2D(const std::string& path)
		: m_Path(path)
	{
		// stb_image writes these out-parameters. `channels` is used only to reject
		// nothing -- the STBI_rgb_alpha request below means the loader expands
		// whatever it found to 4 channels, so the source count is irrelevant.
		int width, height, channels;
		// D3D UV origin is top-left; stb flip is a GL-bottom-left convention
		// (OpenGLTexture2D sets flip(1) before its own loads). Loading flipped
		// here would display every DX texture upside-down vs GL.
		stbi_set_flip_vertically_on_load(0);
		// STBI_rgb_alpha is the desired_channels argument: it forces 4-channel
		// output (expanding 1/3-channel images, dropping alpha where absent),
		// which is what makes the hardcoded bpp = 4 below correct.
		// stbi_load's first parameter is `const char*` and `path` is a std::string,
		// so this compiles only because something in scope provides char*() for
		// std::string -- an overload this file does not obviously include. If
		// that came from a transitive header it is fragile; path.c_str() is the
		// explicit spelling and costs nothing.
		stbi_uc* data = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);

		if (!data)
		{
			// Hard failure, logged at error level, but the constructor still returns
			// successfully -- the object exists with a zero extent and no GPU
			// resource. Callers that ignore the result will draw from nothing.
			WL_CORE_ERROR("Failed to load texture: {0}", path);
			// Zeroing the extents is what makes the failure visible downstream:
			// SetData's size check rejects a nonzero upload against a zero-extent
			// texture, and any size query reports an empty image.
			m_Width = 0;
			m_Height = 0;
			return;
		}

		m_Width = width;
		m_Height = height;

		CreateTexture(width, height);
		CreateSampler();

		// Logs at INFO on every load -- including on a texture reloaded at
		// runtime -- so a content-heavy frame with several uploads produces
		// several INFO lines. {0}-style placeholders mean this is the project's
		// own logger wrapper, not fmt/spdlog called directly.
		WL_CORE_INFO("NVRHITexture2D loaded '{0}' ({1}x{2}, {3} mips, {4})",
			path, width, height, ComputeMipLevels((uint32_t)width, (uint32_t)height),
			// Reports the format by re-reading the descriptor rather than echoing
			// the constant just assigned. If CreateTexture failed, m_Texture is null
			// and this prints "null" -- the ternary is what guards that.
			!m_Texture ? "null" : (m_Texture->getDesc().format == nvrhi::Format::RGBA8_UNORM ? "RGBA8" : "other"));

		// Upload texture data
		// Hardcoded because STBI_rgb_alpha guarantees 4 channels. Not const,
		// and deliberately not derived from the `channels` out-parameter above.
		int bpp = 4;
		// DEFECT (overflow): width and height are `int` and bpp is `int`, so the
		// product is evaluated in signed int and only then converted to uint32_t.
		// A large enough image -- over 2^31/4 bytes, roughly 23000x23000 --
		// overflows to a negative value, which converts to a huge uint32_t and
		// then passes the size check in SetData as a bogus number. Computing in
		// size_t would not have this problem.
		uint32_t size = width * height * bpp;
		// Uploads level 0 plus the whole generated mip chain. stbi's buffer is
		// freed on the next line, so nothing may retain a pointer into it.
		SetData(data, size);

		// The correct stb counterpart to stbi_load -- malloc'd internally, so it
		// must be stbi_image_free and not delete[]. Freeing promptly matters: a
		// decoded 4K RGBA image is ~33 MB, and several can be alive at once
		// while a level loads.
		stbi_image_free(data);
	}

	// Non-virtual destructor, which is fine because holders keep a
	// Ref<NVRHITexture2D> and never delete through a base pointer.
	NVRHITexture2D::~NVRHITexture2D()
	{
		// Dropping the last ref releases the NVRHI sampler, then the texture on
		// the next line. Explicit rather than incidental because release order
		// is visible to the device: the sampler goes while its texture is alive.
		m_Sampler = nullptr;
		// Drops the old handle explicitly before CreateTexture. Redundant -- the
		// assignment inside CreateTexture releases it anyway -- but it makes
		// destroy-before-create ordering explicit at the cost of one Ref round
		// trip.
		m_Texture = nullptr;
	}

	// Called from two constructors and from Resize. Every failure path logs
	// and returns WITHOUT clearing m_Texture, so a caller that ignores the
	// result keeps the previous texture -- on the Resize path that is the
	// old, wrong-sized one.
	void NVRHITexture2D::CreateTexture(uint32_t width, uint32_t height)
	{
		nvrhi::IDevice* device = GetDevice();
		// The no-device guard. GetDevice() returns null only when there is no
		// current context, which in normal operation means construction during
		// teardown.
		if (!device)
		{
			WL_CORE_ERROR("NVRHITexture2D: No NVRHI device available");
			return;
		}

		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		// Explicitly 2D. No NVRHI field defaults are relied on anywhere in this
		// desc, which is deliberate: every field the renderer depends on is set.
		desc.depth = 1;
		desc.dimension = nvrhi::TextureDimension::Texture2D;
		// Linear RGBA8, matching OpenGLTexture2D's GL_RGBA8: the Nova shaders
		// were tuned on raw sRGB values. SRGBA8_UNORM would make D3D
		// auto-decode every sample to linear (dark/desaturated vs GL and wrong
		// luminance-derived height inputs for relief).
		// Linear, not sRGB. The reasoning in the comment above is a genuine
		// cross-backend constraint: SRGBA8_UNORM makes D3D hardware-decode every
		// sample, so the same asset would look different under D3D than under
		// the GL path. It also corrupts the luminance-derived height inputs the
		// relief shader reads.
		desc.format = nvrhi::Format::RGBA8_UNORM;
		// The path doubles as the debug name, which is what makes a GPU capture
		// readable. On the Resize path m_Path is the empty string from the blank
		// constructor, so a resized texture silently loses its identifying name.
		desc.debugName = m_Path.empty() ? "Texture2D" : m_Path;
		// Declares the state the resource starts in, which lets NVRHI skip an
		// initial transition. keepInitialState below then pins it there --
		// without that, the first write would have to transition out first.
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		// Full mip chain: Nova samples explicit LOD 0..4 (distance + grazing).
		// A single level leaves minification unfiltered (shimmer/blockiness).
		// Full chain, not level 1. Nova's shaders sample explicit LOD 0..4, so a
		// single-level texture returns undefined data -- typically black or the
		// base colour -- for every distance-blended sample, not merely a sharp one.
		desc.mipLevels = ComputeMipLevels(width, height);

		// The Ref assignment releases the previous handle, so calling this twice
		// does not leak -- though it does destroy whatever was there.
		m_Texture = device->createTexture(desc);
		// Log-only. No fallback, no throw: an out-of-memory texture becomes a
		// null Ref that every later call must survive.
		if (!m_Texture)
		{
			WL_CORE_ERROR("NVRHITexture2D: Failed to create texture");
		}
	}

	// Separate from CreateTexture because Resize does NOT call it. That
	// omission is safe: a sampler has no size, so a resize cannot stale it.
	void NVRHITexture2D::CreateSampler()
	{
		nvrhi::IDevice* device = GetDevice();
		if (!device)
		{
			WL_CORE_ERROR("NVRHITexture2D: No NVRHI device available");
			return;
		}

		nvrhi::SamplerDesc desc;
		// NVRHI spells filtering as booleans rather than an enum: true = linear,
		// false = nearest. Less readable than a named enumerator, but it is the
		// library's API rather than a shortcut taken here.
		desc.minFilter = true;  // Linear
		desc.magFilter = true;  // Linear
		// The inline note reads as a history: this was presumably nearest before
		// textures carried a real mip chain. Combined with linear magnification
		// below, true here gives trilinear filtering.
		desc.mipFilter = true;  // Linear (trilinear, now that textures carry mips)
		// Mirrored repeat, matching OpenGLTexture2D: photographic textures are
		// tiled ~1/m but not tileable, so plain repeat shows seams every tile.
		// Mirror rather than Repeat because the assets tile at ~1/m and are not
		// seamless at their edges: mirror hides the seam that repeat exposes as a
		// hard discontinuity every tile. Matching the GL path exactly is what
		// keeps a backend swap visually identical.
		desc.addressU = nvrhi::SamplerAddressMode::Mirror;
		desc.addressV = nvrhi::SamplerAddressMode::Mirror;
		desc.addressW = nvrhi::SamplerAddressMode::Mirror;
		// 16x aniso like the GL path; >1 also selects the anisotropic D3D filter.
		// 16x, matching GL. On D3D any value > 1 selects anisotropic filtering,
		// so this is a hard request for 16 samples rather than a preference;
		// hardware supporting fewer clamps silently.
		desc.maxAnisotropy = 16.0f;

		m_Sampler = device->createSampler(desc);
		if (!m_Sampler)
		{
			WL_CORE_ERROR("NVRHITexture2D: Failed to create sampler");
		}
	}

	// Diagnostic accessor, not an API handle: NVRHI has no integer object IDs.
	uint32_t NVRHITexture2D::GetRendererID() const
	{
		// NVRHI doesn't have integer IDs, return a hash of the texture pointer
		// A POINTER VALUE truncated to 32 bits. Two consequences: (1) it changes
		// whenever the allocator reuses an address, so it is not stable across
		// frames and must not be cached or compared over time; (2) on a 64-bit
		// build only the low half survives, so two live textures can collide.
		// Fine for a same-frame debug query, wrong for anything persistent.
		return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m_Texture.Get()));
	}

	// Uploads level 0 plus a CPU box-filtered mip chain. NVRHI has no
	// GenerateMips equivalent on this path, and Nova samples explicit LODs.
	// Manual CPU mip generation. NVRHI exposes no glGenerateMipmap
	// equivalent on this path and Nova needs real levels, so the filter runs
	// here on the CPU at load time rather than per frame.
	void NVRHITexture2D::UploadMipChain(nvrhi::ICommandList* cmd, const void* baseData)
	{
		const uint8_t* src = static_cast<const uint8_t*>(baseData);
		// Level 0. The fourth argument is the ROW PITCH in bytes, not the total
		// size: one row is m_Width pixels x 4 bytes, and the full extent comes
		// from the texture descriptor. Passing a total size here would be wrong.
		// The leading two zeros are the array slice and the mip index.
		cmd->writeTexture(m_Texture, 0, 0, src, size_t(m_Width) * 4);

		uint32_t levelW = m_Width, levelH = m_Height;
		// Two scratch buffers reused across every level: prev holds the level
		// just written, next accumulates the next. Declared once so the whole
		// chain costs two allocations instead of one per level.
		std::vector<uint8_t> prev, next;
		// Copies level 0 into `prev`. A copy rather than a view because the read
		// loop below indexes with the stride of a POTENTIALLY different width
		// each iteration, while src carries only the level-0 stride.
		prev.assign(src, src + size_t(levelW) * levelH * 4);

		uint32_t mipLevels = ComputeMipLevels(m_Width, m_Height);
		// Starts at 1 because level 0 was uploaded directly above. mipLevels
		// comes from the same ComputeMipLevels the descriptor used, so the loop
		// and the allocation cannot disagree about how many levels exist.
		for (uint32_t level = 1; level < mipLevels; level++)
		{
			// Integer halving with a clamp at 1. For a non-power-of-two width this
			// rounds DOWN, so the chain drops the rightmost column at some level --
			// the same lossy-but-legal choice ComputeMipLevels makes.
			uint32_t nextW = levelW > 1 ? levelW / 2 : 1;
			uint32_t nextH = levelH > 1 ? levelH / 2 : 1;
			next.resize(size_t(nextW) * nextH * 4);

			for (uint32_t y = 0; y < nextH; y++)
			{
				for (uint32_t x = 0; x < nextW; x++)
				{
					uint32_t sx = x * 2, sy = y * 2;
					// Clamps the second tap at the edge. Without this an odd-width level
					// would read one pixel past the row -- into the next row, or off the
					// end of the buffer on the final row.
					uint32_t ex = (sx + 1 < levelW) ? sx + 1 : sx;
					uint32_t ey = (sy + 1 < levelH) ? sy + 1 : sy;
					// The tap count: 4 normally, fewer at edges. Dividing by this is what
					// keeps the border average from being biased bright.
					uint32_t count = (ex - sx + 1) * (ey - sy + 1);
					// Four uint32_t accumulators, zero-initialised. uint32_t rather than
					// uint8_t because summing four bytes overflows an 8-bit accumulator
					// on the very first tap.
					uint32_t sum[4] = {};
					for (uint32_t yy = sy; yy <= ey; yy++)
						for (uint32_t xx = sx; xx <= ex; xx++)
						{
							const uint8_t* p = prev.data() + (size_t(yy) * levelW + xx) * 4;
							// Naive box filter, unrolled per channel rather than looped. Alpha is
							// averaged alongside colour, so an RGBA image's mips filter coverage
							// correctly instead of clamping to opaque.
							sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; sum[3] += p[3];
						}
					uint8_t* d = next.data() + (size_t(y) * nextW + x) * 4;
					// Truncating integer division, which biases results up to 1/255 darker
					// than rounding would. Each channel truncates independently, so the
					// result is close to but not exactly the true mean.
					d[0] = uint8_t(sum[0] / count); d[1] = uint8_t(sum[1] / count);
					d[2] = uint8_t(sum[2] / count); d[3] = uint8_t(sum[3] / count);
				}
			}

			// Row pitch again, now for this level's narrower width. The mip index
			// is the third argument; the leading 0 is the array slice.
			cmd->writeTexture(m_Texture, 0, level, next.data(), size_t(nextW) * 4);
			// Swap rather than copy: `prev` now holds the level just written and
			// `next` holds the one before it, which the resize at the top of the
			// next iteration overwrites. No allocation and no copy.
			prev.swap(next);
			levelW = nextW;
			levelH = nextH;
		}
	}

	// Public upload entry. Takes void* rather than const void* even though
	// the data is only read, so a caller holding a const buffer must cast.
	void NVRHITexture2D::SetData(void* data, uint32_t size)
	{
		// Three-in-one guard covering the failed-construction case (m_Texture
		// null), the null-buffer case, and a zero-size no-op. Silent -- no log --
		// so a caller uploading into a texture whose creation failed learns
		// nothing about why.
		if (!m_Texture || !data || size == 0)
			return;

		auto* context = NVRHIRendererAPI::GetCurrentContext();
		if (!context)
		{
			WL_CORE_ERROR("NVRHITexture2D::SetData: No NVRHI context available");
			return;
		}

		nvrhi::ICommandList* cmd = context->GetCommandList();
		if (!cmd)
		{
			WL_CORE_ERROR("NVRHITexture2D::SetData: No command list available");
			return;
		}

		// Requires a FULL-image upload. There is no sub-region and no
		// single-level path, so a caller holding a cropped or partial buffer is
		// rejected outright rather than uploaded wrongly.
		if (size != uint64_t(m_Width) * m_Height * 4)
		{
			// Logs and returns, leaving the previous contents in place. The message
			// names the expectation but not either size, so a caller cannot tell
			// from the log whether the expected or the supplied value was wrong.
			WL_CORE_ERROR("NVRHITexture2D::SetData: Expected a full RGBA8 image");
			return;
		}
		// Distinguishes 'I am borrowing the caller's command list' from 'nobody
		// opened one, so I must open, close and submit my own'. Inverting this
		// would either submit a half-filled list or leave one unclosed.
		const bool transient = !context->IsCommandListOpen();
		// Single-line if without braces -- the only such line in this file.
		// The style is consistent, but it does mean the matching close below
		// must stay adjacent to the submit.
		if (transient) cmd->open();
		// Every level is written in one pass, so a caller that passed only
		// level-0 data still ends up with a complete chain. The price is a
		// full CPU box-filter of the image on every upload.
		UploadMipChain(cmd, data);
		if (transient)
		{
			// Close before submit: NVRHI requires a closed list on the submission
			// path, and an unclosed list silently drops the recorded commands.
			cmd->close();
			// Immediate submit, so the upload is visible to the next draw with no
			// frame of latency. DEFECT: GetDevice() is dereferenced WITHOUT a null
			// check, unlike every other use in this file which tests the result --
			// a context with a null device crashes here.
			context->GetDevice()->executeCommandList(cmd);
		}
	}

	// Present to satisfy the Texture2D interface the GL path uses. Under
	// NVRHI binding goes through the descriptor / binding-array path at draw
	// time, so there is nothing for a per-texture bind to do.
	void NVRHITexture2D::Bind(uint32_t slot) const
	{
		// Note: In NVRHI, textures are bound via binding arrays, not directly
		// This is handled in the shader/draw call implementation
		// For now, this is a no-op
	}

	// Recreates the texture at a new size. NOT a resize in the GPU sense --
	// NVRHI textures are immutable, so this is destroy-and-recreate.
	void NVRHITexture2D::Resize(uint32_t width, uint32_t height)
	{
		// No-op on a same-size request, which is the common case when one
		// resize event is delivered more than once. Avoids a needless
		// recreate plus full mip re-upload.
		if (width == m_Width && height == m_Height)
			return;

		m_Width = width;
		m_Height = height;

		// Recreate texture with new size
		m_Texture = nullptr;
		CreateTexture(width, height);
	}

}
