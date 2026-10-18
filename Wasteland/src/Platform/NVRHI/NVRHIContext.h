#pragma once

#include "Wasteland/Renderer/GraphicsContext.h"
#include "Wasteland/Renderer/RendererAPI.h"

#include <vulkan/vulkan.h>
#include <d3d11.h>
// ID3D11/D3D12/Vulkan object headers, plus nvrhi itself. nvrhi is the
// abstraction this class is named for; the three native headers are what
// the void* members below are really pointing at.
#include <d3d12.h>
#include <nvrhi/nvrhi.h>
#include <vector>

// Forward declaration rather than an include: only a GLFWwindow* is stored,
// so this header stays parseable with no windowing library present.
struct GLFWwindow;

namespace Wasteland
{

	class NVRHIContext : public GraphicsContext
	{
	public:
		// The API is chosen by the caller and stored, not inferred: it selects the
		// CreateDevice* path in the .cpp and gates every backend-specific accessor
		// below. Passing an API the build was not configured with is a silent
		// no-op at Init() time rather than a compile error.
		NVRHIContext(GLFWwindow *windowHandle, RendererAPI::API api);
		// Out-of-line and virtual. The .cpp's ordering discipline -- NVRHI objects
		// released first, native device last -- is this destructor's whole reason
		// for existing.
		virtual ~NVRHIContext();

		virtual void Init() override;
		virtual void SwapBuffers() override;

		// NVRHI-specific accessors
		// Raw NVRHI interface pointer borrowed from the DeviceHandle below.
		// Inline rather than a .cpp call because it is on the per-frame path.
		nvrhi::IDevice *GetDevice() const { return m_Device; }
		// Same inline-and-raw-pointer shape as GetDevice. Created during Init(),
		// opened and closed per frame, so this is the most-queried handle.
		nvrhi::ICommandList *GetCommandList() const { return m_CommandList; }
		// NOT inline, unlike its neighbours: it branches on m_API and returns null
		// rather than a handle when the relevant framebuffer vector is empty.
		nvrhi::FramebufferHandle GetCurrentFramebuffer() const;
		// The swapchain IMAGE for the current index. Separate from
		// GetCurrentFramebuffer because that one returns the FBO wrapping it.
		nvrhi::TextureHandle GetCurrentBackBuffer() const;
		// Render-target override set by NVRHIFramebuffer::Bind/Unbind so scene
		// draws (and Clear) go to the editor viewport framebuffer instead of the
		// swapchain backbuffer. Null = swapchain backbuffer.
		// Assignment, not a push or a stack: there is exactly ONE override slot, so
		// a second NVRHIFramebuffer::Bind() while another is still bound silently
		// replaces the first rather than nesting. Unbind() writes nullptr to restore
		// the swapchain default -- see the .cpp destructor, which also clears it
		// before the device goes away.
		void SetRenderTargetFramebuffer(const nvrhi::FramebufferHandle &fb) { m_ActiveRenderTarget = fb; }
		// Reads the override without consuming it. The explicit static_cast is
		// needed because FramebufferHandle's conversion to bool is not implicit.
		bool HasRenderTargetOverride() const { return static_cast<bool>(m_ActiveRenderTarget); }
		uint32_t GetCurrentBackBufferIndex() const { return m_CurrentBackBufferIndex; }
		// Cached window size from glfwGetWindowSize, refreshed by Resize().
		// Read-only so no caller can desynchronise it from the swapchain extent.
		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }

		// Native DirectX object accessors (for ImGui and other integrations)
		// void* rather than a typed pointer so this header would not need the
		// Direct3D headers for the callers that only use Vulkan or OpenGL.
		// The cost is that every consumer must static_cast back, and a wrong cast
		// is a silent type confusion rather than a compile error.
		void *GetD3D11Device() const { return m_D3D11Device; }
		void *GetD3D11Context() const { return m_D3D11Context; }
		void *GetDXGISwapChain() const { return m_DXGISwapChain3; }
		// Bounds-checked: an out-of-range index yields nullptr. That is the opposite
		// of OpenGLFramebuffer::GetColorAttachmentRendererID, which indexes
		// unchecked -- the two backends are inconsistent and this is the safe one.
		// nullptr is also the answer for a valid index before Init() populates it.
		void *GetD3D11BackBufferRTV(uint32_t index) const { return (index < m_D3D11RTVs.size()) ? m_D3D11RTVs[index] : nullptr; }
		void *GetD3D12Device() const { return m_D3D12Device; }
		void *GetD3D12CommandQueue() const { return m_D3D12CommandQueue; }

		// DX12 ImGui descriptor heap
		// The SRV heap, its CPU/GPU starts and its capacity below are one unit:
		// descriptor [0] is ImGui's font atlas, and the engine copies every other
		// SRV above it. Split three ways because Direct3D defines the handle types.
		void *GetD3D12ImGuiSRVHeap() const { return m_D3D12ImGuiSRVHeap; }
		D3D12_CPU_DESCRIPTOR_HANDLE GetD3D12ImGuiSRVHeapCPU() const { return m_D3D12ImGuiSRVHeapCPU; }
		D3D12_GPU_DESCRIPTOR_HANDLE GetD3D12ImGuiSRVHeapGPU() const { return m_D3D12ImGuiSRVHeapGPU; }

	// Copies an SRV for `resource` into a slot of the ImGui SRV heap and returns
	// the GPU descriptor handle ImGui's DX12 backend expects in TexID.
	//
	// WHY THIS EXISTS: ImGui's DX12 backend does
	//     texture_handle.ptr = (UINT64)pcmd->GetTexID();
	//     ctx->SetGraphicsRootDescriptorTable(1, texture_handle);
	// so TexID must be a GPU DESCRIPTOR HANDLE VALUE, not a resource pointer or
	// a view pointer. Passing anything else binds a garbage descriptor and the
	// draw silently samples nothing -- which is why the DX12 viewport showed no
	// scene at all while DX11 and OpenGL were fine.
	//
	// `slot` must be a stable per-resource index (0 is reserved for the font
	// atlas, which ImGui_ImplDX12_Init writes itself). Returns a null handle if
	// the slot is out of range or the resource is null.
	// `const` but it MUTATES: it writes an SRV into the heap. That is sound
	// because the heap and the D3D12 device are logically const from this
	// class's point of view -- the cache is a side table, not object state.
	// Slot 0 is rejected by the .cpp, not here, so the exclusion is enforced
	// at the definition rather than visible in the declaration.
	D3D12_GPU_DESCRIPTOR_HANDLE GetOrCreateImGuiDescriptor(UINT slot, void *resource) const;

		// DX12 ImGui command objects
		void *GetD3D12ImGuiCommandAllocator() const { return m_D3D12ImGuiCommandAllocator; }
		void *GetD3D12ImGuiCommandList() const { return m_D3D12ImGuiCommandList; }
		void *GetD3D12ImGuiFence() const { return m_D3D12ImGuiFence; }
		void *GetD3D12ImGuiFenceEvent() const { return m_D3D12ImGuiFenceEvent; }
		uint64_t GetD3D12ImGuiFenceValue() const { return m_D3D12ImGuiFenceValue; }
		// The one accessor here that is not read-only: the .cpp advances the value
		// after signalling the fence, so the counter and the GPU signal stay
		// paired. Marking it const would only push a cast into the caller.
		void IncrementD3D12FenceValue() { m_D3D12ImGuiFenceValue++; }
		void *GetD3D12ImGuiRTVHeap() const { return m_D3D12ImGuiRTVHeap; }
		uint32_t GetD3D12RTVDescriptorSize() const { return m_D3D12RTVDescriptorSize; }
		// Out-of-line because it does real work: it checks m_API is DX12, checks
		// the backbuffer vector is non-empty, checks the index is in range, then
		// unwraps the NVRHI texture to its native ID3D12Resource. Three early
		// returns rather than one combined condition, so each failure mode is
		// separately readable.
		void *GetD3D12CurrentBackBufferResource() const;

		// Native Vulkan object accessors
		VkInstance GetVkInstance() const { return m_VkInstance; }
		VkPhysicalDevice GetVkPhysicalDevice() const { return m_VkPhysicalDevice; }
		VkDevice GetVkDevice() const { return m_VkDevice; }
		VkQueue GetVkGraphicsQueue() const { return m_VkGraphicsQueue; }
		VkSurfaceKHR GetVkSurface() const { return m_VkSurface; }
		VkSwapchainKHR GetVkSwapchain() const { return m_VkSwapchain; }

		// Vulkan queue family / swapchain info (needed for ImGui)
		// Compared against each other in the .cpp to decide whether swapchain
		// images need VK_SHARING_MODE_CONCURRENT. GetVkPresentQueueFamilyIndex
		// falls back to the graphics index, so the two are equal in the common case.
		uint32_t GetVkGraphicsQueueFamilyIndex() const { return m_VkGraphicsQueueFamilyIndex; }
		uint32_t GetVkPresentQueueFamilyIndex() const { return m_VkPresentQueueFamilyIndex; }
		uint32_t GetVkImageCount() const { return m_VkImageCount; }
		uint32_t GetVkSwapchainImageFormat() const { return m_VkSwapchainImageFormat; }
		uint32_t GetVkFrameIndex() const { return m_VkFrameIndex; }

		// Vulkan ImGui resources
		// The two resources whose absence the .cpp treats as 'Vulkan ImGui is not
		// initialised'; exposed separately because ImGui's backend binds the pool
		// and begins the render pass itself.
		VkDescriptorPool GetVkImGuiDescriptorPool() const { return m_VkImGuiDescriptorPool; }
		VkRenderPass GetVkImGuiRenderPass() const { return m_VkImGuiRenderPass; }
		// The sampler every ImGui-bound texture is filtered with, created here
		// alongside the descriptor pool because that is where the rest of the
		// Vulkan ImGui resources live.
		//
		// WHY THE ENGINE OWNS ONE RATHER THAN BORROWING ImGui's: in this vendored
		// ImGui the font sampler is bd->FontSampler, created inside
		// ImGui_ImplVulkan_CreateDeviceObjects and reachable only through
		// backend-private data. ImGui_ImplVulkan_InitInfo in this version has no
		// Sampler field either (that arrived in a later release), so there is no
		// supported way to hand one in or to read one out. ImGui_ImplVulkan_AddTexture
		// takes the sampler as a parameter, which is the only reason one is needed.
		//
		// Clamp-to-edge rather than ImGui's own repeat: the one texture bound
		// through this sampler is the viewport colour attachment, whose UVs run
		// exactly 0..1, and bilinear filtering with repeat addressing at the
		// 1.0 edge blends in the opposite edge of the image.
		VkSampler GetVkImGuiSampler() const { return m_VkImGuiSampler; }
		// Out-of-line and index-checked: unlike GetCurrentFramebuffer above, this
		// one bounds-checks and falls back to element 0 when the current index is
		// out of range. The .cpp documents the inconsistency between the two.
		VkFramebuffer GetVkCurrentImGuiFramebuffer() const;
		VkCommandPool GetVkImGuiCommandPool() const { return m_VkImGuiCommandPool; }
		VkCommandBuffer GetCurrentVulkanImGuiCommandBuffer() const;
		// Names the attachment the current frame's ImGui pass is about to sample,
		// so the layout transition that makes sampling legal can be recorded at
		// the only point where a command buffer is open. Called by
		// NVRHIFramebuffer::GetColorAttachmentImGuiTexID, consumed by
		// ImGuiLayer::End.
		//
		// ONE SLOT, NOT A REGISTRY, and that is the whole design: the editor
		// presents exactly one framebuffer image, so a single texture handle is
		// the entire amount of state this needs. Storing a handle rather than a
		// raw pointer means a stale slot cannot dangle even if the framebuffer
		// is destroyed before the slot is overwritten.
		void SetVulkanImGuiAttachment(nvrhi::TextureHandle texture) { m_VkImGuiAttachment = texture; }
		// Registers this frame's compute-written ImGui texture. A new registration
		// also discards any reverse-transition handle left over from an aborted frame.
		void SetVulkanImGuiComputeOutput(nvrhi::TextureHandle texture)
		{
			m_VkImGuiComputeOutput = texture;
			m_VkImGuiComputeOutputReturn = nullptr;
		}
		// Records the eColorAttachmentOptimal -> eShaderReadOnlyOptimal
		// transition on the current ImGui command buffer, so the layout named by
		// the descriptor set NVRHIFramebuffer built is the layout the image is
		// actually in. A compute-output registration instead uses GENERAL as its
		// old layout and retains the handle for the post-pass return transition.
		// Records nothing when neither kind of attachment was registered.
		//
		// WHY IT CANNOT LIVE IN NVRHI: the transition has to sit between the
		// scene's NVRHI submission and the ImGui submission, and NVRHI's command
		// list is closed and executed before ImGui draws (Application::Run calls
		// ExecuteNVRHICommandList before m_ImGuiLayer->Begin). Anything recorded
		// on that list afterwards would only be flushed a frame late, which is
		// one frame of sampling a texture whose layout does not match its
		// descriptor. The ImGui command buffer is a raw Vulkan one, so the
		// barrier is recorded directly on it.
		void RecordVulkanImGuiAttachmentTransition();
		// Returns a compute output to GENERAL after the ImGui render pass so the
		// next compute dispatch can read and write it again.
		void RecordVulkanImGuiComputeOutputReturnTransition();
		// The 'is Vulkan ImGui up?' predicate, and it checks only TWO of the four
		// resources creation produces -- framebuffers and the command pool are not
		// tested. A partial InitVulkanImGuiResources() therefore reports true.
		// Inline because it is on the per-frame Vulkan submit path.
		bool IsVulkanImGuiInitialized() const { return m_VkImGuiDescriptorPool != nullptr && m_VkImGuiRenderPass != nullptr; }

		// Get the renderer API type
		RendererAPI::API GetAPI() const { return m_API; }

		// Resize rebuilds the swapchain rather than resizing it in place -- the
		// same whole-object-rebuild approach the OpenGL framebuffer takes.
		// Not virtual and not an override, unlike most of this interface: only
		// this backend has a reason to be resized by the window.
		void Resize(uint32_t width, uint32_t height);
		// The base declares BeginFrame() virtual with an empty body as a
		// deliberate no-op, so overriding it is what makes the DX11/DX12/Vulkan
		// acquire-present step happen at all. See GraphicsContext.h.
		void BeginFrame() override;
		void ExecuteNVRHICommandList();
		// Whether a command list is open. Pair with IsDeviceLost(): after a loss
		// a list may be left open and never submitted.
		bool IsCommandListOpen() const { return m_CommandListOpen; }

		// Device loss detection (TDR, driver crash, GPU reset). When true the
		// native device is dead and all GPU operations must be skipped until
		// RecoverDevice() re-creates the device and swapchain.
		bool IsDeviceLost() const { return m_IsDeviceLost; }
		// Rebuilds the device and swapchain after a loss. NOT inline and not
		// idempotent by construction -- the .cpp is where the ordering discipline
		// for a partial teardown lives.
		void RecoverDevice();
		void SetDeviceLost() { m_IsDeviceLost = true; }

		// Vulkan ImGui lifecycle (called by ImGuiLayer)
		// Declared, not defined -- the failure handling lives in the .cpp. Public
		// because ImGuiLayer calls them, not because they are renderer interface.
		bool InitVulkanImGuiResources();
		void ShutdownVulkanImGuiResources();

	private:
		void CreateDeviceDX11();
		void CreateDeviceDX12();
		void CreateDeviceVulkan();
	// DEFECT (cosmetic, but it hides intent): this declaration is indented one
	// tab less than the two above it. It compiles and behaves identically, but
	// it reads as belonging to the enclosing scope rather than to the private
	// helper block, which is the opposite of what it is.
	void CreateDeviceVulkanWithRetry();
		void CreateSwapChain();
		void CreateBackBufferFramebuffers();
		void CreateVulkanSwapChain();
		void DestroyVulkanSwapChain();
		// DestroyVulkan() has no DX11/DX12 counterpart by design: the DX objects
		// are released through NVRHI's device teardown plus explicit Release()
		// calls, because NVRHI owns the COM references and a double release
		// is a use-after-free.
		void DestroyVulkan();

		// Vulkan-specific helpers
		// The Vulkan path is the only one with a selection step, because it is the
		// only one that has to CHOOSE a physical device. The DX backends bind to
		// whatever adapter the runtime hands back.
		bool SelectVulkanPhysicalDevice();
		void CreateVulkanDevice();
		void CreateVulkanSurface();
		void GetVulkanQueueFamilyIndices();

		// DX12 ImGui helpers
		void CreateD3D12ImGuiCommandObjects();
		void DestroyD3D12ImGuiCommandObjects();

		// Vulkan ImGui helpers
		// Sampler for ImGui_ImplVulkan_AddTexture. Its own create/destroy pair
		// rather than a member of CreateVulkanImGuiDescriptorPool, because the
		// sampler must outlive the sets allocated from that pool and die after it.
		void CreateVulkanImGuiSampler();
		void DestroyVulkanImGuiSampler();
		void CreateVulkanImGuiDescriptorPool();
		void DestroyVulkanImGuiDescriptorPool();
		void CreateVulkanImGuiRenderPass();
		void DestroyVulkanImGuiRenderPass();
		void CreateVulkanImGuiFramebuffers();
		void DestroyVulkanImGuiFramebuffers();
		void CreateVulkanImGuiCommandPoolAndBuffers();
		void DestroyVulkanImGuiCommandPoolAndBuffers();

		// Stored raw. The window outlives the context in every platform window, and
		// GLFWwindow is only forward-declared here, so this is a non-owning
		// observer -- nothing in the destructor may touch it.
		GLFWwindow *m_WindowHandle;
		// NOT initialised here: there is no default member initializer, so this is
		// whatever the ctor's member-init list set it to. m_API is therefore only
		// meaningful after the ctor body has run, which every accessor above relies
		// on.
		RendererAPI::API m_API;
		// NVRHI objects
		// NVRHI handles, not raw pointers: a DeviceHandle is a refcounted smart
		// handle that keeps the underlying object alive. The raw IDevice* that
		// GetDevice() returns is the borrowed pointer view of this same object.
		// Neither has an initializer, so both are empty handles until Init().
		nvrhi::DeviceHandle m_Device;
		nvrhi::CommandListHandle m_CommandList;

		// Backbuffer management
		// The DX11/DX12 backbuffers. Distinct from m_VkBackBuffers below, which
		// serves the Vulkan path, and from m_VkSwapchainImages below that, which
		// holds the raw VkImage rather than an NVRHI handle.
		// The framebuffer vector is parallel to this one and index-aligned by
		// convention only -- nothing in the type system ties the two lengths.
		std::vector<nvrhi::TextureHandle> m_BackBuffers;
		std::vector<nvrhi::FramebufferHandle> m_BackBufferFramebuffers;
		// Starts at 0 and is set by the present/acquire step each frame. Every
		// accessor that indexes a parallel vector uses it, which is why the
		// .cpp's bounds checks on those reads matter.
		uint32_t m_CurrentBackBufferIndex = 0;
		nvrhi::FramebufferHandle m_ActiveRenderTarget;

		// DX11 / DX12 native objects
		// Null-initialised, unlike the NVRHI handles above: an empty NVRHI handle
		// is a valid 'nothing yet', whereas these must be a real null for the
		// destructor's Release() calls to be safe.
		// There is no DX12-specific handle set -- DX12 goes through NVRHI for
		// resources and needs only the native device and queue here.
		void *m_D3D11Device = nullptr;
		void *m_D3D11Context = nullptr;
		void *m_D3D12Device = nullptr;
		void *m_D3D12CommandQueue = nullptr;
		void *m_DXGISwapChain3 = nullptr;

		// DX11 cached RTVs for ImGui (owned here, one per backbuffer)
		// Owned: the .cpp destructor Releases each and clears the vector. Exposed
		// one at a time through GetD3D11BackBufferRTV.
		// DEFECT: 'owned here' is a comment, not a type. Nothing stops another
		// translation unit releasing these through the raw pointer the accessor
		// hands back, which would double-free on teardown.
		std::vector<ID3D11RenderTargetView*> m_D3D11RTVs;

		// DX12 ImGui SRV descriptor heap
		void *m_D3D12ImGuiSRVHeap = nullptr;
		// The CPU and GPU descriptor heap STARTS, zero-initialised. D3D12 handles
		// are opaque integers rather than pointers, so a valid handle can be all
		// zeroes -- these are only meaningful once the heap exists.
		D3D12_CPU_DESCRIPTOR_HANDLE m_D3D12ImGuiSRVHeapCPU = {};
		D3D12_GPU_DESCRIPTOR_HANDLE m_D3D12ImGuiSRVHeapGPU = {};

		// Descriptor count of the ImGui SRV heap. ID3D12DescriptorHeap exposes no
		// total byte size, so the capacity is recorded at creation for bounds checks.
		// Recorded at creation precisely because the heap object cannot report its
		// own size: ID3D12DescriptorHeap exposes no total-byte-size accessor. The
		// .cpp bounds-checks every slot against this number before writing.
		// DEFECT: it is a UINT next to uint32_t members in the same block -- same
		// width, so harmless, but the inconsistency invites a format-string
		// mismatch in the warn call that reads it.
		UINT m_D3D12ImGuiSRVCapacity = 0;
		void *m_D3D12ImGuiRTVHeap = nullptr;
		uint32_t m_D3D12RTVDescriptorSize = 0;

		// DX12 ImGui command objects
		// The DX12 ImGui command objects, all null and all owned. Released by the
		// .cpp's DestroyD3D12ImGuiCommandObjects.
		// The fence value is a counter, not a handle, so it starts at 0 rather
		// than at a sentinel.
		void *m_D3D12ImGuiCommandAllocator = nullptr;
		void *m_D3D12ImGuiCommandList = nullptr;
		void *m_D3D12ImGuiFence = nullptr;
		uint64_t m_D3D12ImGuiFenceValue = 0;
		void *m_D3D12ImGuiFenceEvent = nullptr;

		// Vulkan native objects
		// Initialised to VK_NULL_HANDLE rather than left zeroed: that is the
		// idiomatic Vulkan 'no handle' value and it happens to equal null, but
		// writing it explicitly means the intent survives a reader who does not
		// know that.
		VkInstance m_VkInstance = VK_NULL_HANDLE;
		VkPhysicalDevice m_VkPhysicalDevice = VK_NULL_HANDLE;
		VkDevice m_VkDevice = VK_NULL_HANDLE;
		VkQueue m_VkGraphicsQueue = VK_NULL_HANDLE;
		VkQueue m_VkPresentQueue = VK_NULL_HANDLE;
		VkSurfaceKHR m_VkSurface = VK_NULL_HANDLE;
		VkSwapchainKHR m_VkSwapchain = VK_NULL_HANDLE;
		// Zero-init, NOT VK_NULL_HANDLE: these are indices, not handles. A default
		// of 0 is a real family index, so it is indistinguishable from 'found
		// family 0' until GetVulkanQueueFamilyIndices() has run. The .cpp uses
		// UINT32_MAX as its not-found sentinel and falls back, which is why the
		// accessors above are safe before the device exists.
		uint32_t m_VkGraphicsQueueFamilyIndex = 0;
		uint32_t m_VkPresentQueueFamilyIndex = 0;

		// Vulkan swapchain image management
		// Three parallel views of the same swapchain images: raw VkImage,
		// VkImageView, and NVRHI texture handle. All three are needed because
		// the Vulkan ImGui pass and the renderer take different ones.
		std::vector<VkImage> m_VkSwapchainImages;
		std::vector<VkImageView> m_VkSwapchainImageViews;
		std::vector<nvrhi::TextureHandle> m_VkBackBuffers;
		std::vector<nvrhi::FramebufferHandle> m_VkFramebuffers;

		// Vulkan synchronization
		// One entry per swapchain image. The frame index mods against these
		// counts, so a count that disagrees with the real image count silently
		// reuses an in-flight signal -- the hazard the .cpp documents on
		// GetCurrentVulkanImGuiCommandBuffer.
		std::vector<VkFence> m_VkFences;
		std::vector<VkSemaphore> m_VkImageAvailableSemaphores;
		std::vector<VkSemaphore> m_VkRenderFinishedSemaphores;

		// Vulkan format and capabilities
		// Recorded from the surface rather than hardcoded. Shading rate depends
		// on it, which is why it is exposed for ImGui: the Vulkan ImGui render
		// pass must be created with a compatible format.
		uint32_t m_VkSwapchainImageFormat = 0;
		// Vulkan command pool (for NVRHI / general)
		VkCommandPool m_VkCommandPool = VK_NULL_HANDLE;

		// Vulkan ImGui resources
		VkDescriptorPool m_VkImGuiDescriptorPool = VK_NULL_HANDLE;
		VkRenderPass m_VkImGuiRenderPass = VK_NULL_HANDLE;
		std::vector<VkFramebuffer> m_VkImGuiFramebuffers;
		VkCommandPool m_VkImGuiCommandPool = VK_NULL_HANDLE;
		std::vector<VkCommandBuffer> m_VkImGuiCommandBuffers;
		// The sampler handed to ImGui_ImplVulkan_AddTexture. Created with the
		// pool and destroyed after it, because a descriptor set still refers to a
		// sampler for as long as it exists and vkDestroyDescriptorPool is what
		// ends those sets.
		VkSampler m_VkImGuiSampler = VK_NULL_HANDLE;
		// The single attachment the current frame's ImGui pass samples, filled by
		// NVRHIFramebuffer and drained by ImGuiLayer::End. A handle, not a raw
		// pointer, so a frame that registers an attachment and then fails before
		// recording the transition still leaves a live object in the slot.
		nvrhi::TextureHandle m_VkImGuiAttachment;
		// Compute output awaiting the pre-ImGui transition, plus the handle retained
		// until its reverse transition is recorded after the render pass.
		nvrhi::TextureHandle m_VkImGuiComputeOutput;
		nvrhi::TextureHandle m_VkImGuiComputeOutputReturn;

		// Frame index for sync objects
		uint32_t m_VkFrameIndex = 0;
		// Initialised to 2, the Vulkan minimum for a non-zero present mode, so an
		// early reader gets a plausible count instead of 0 (which would make a
		// mod-by-zero). The real count is overwritten during swapchain creation.
		// DEFECT: this default is invisible to callers. A caller that reads
		// GetVkImageCount() before CreateVulkanSwapChain() sees 2 and cannot tell
		// it from a real two-image swapchain.
		uint32_t m_VkImageCount = 2;

		// NVRHI command list frame state (DX11/DX12/Vulkan)
		// Frame-state flags grouped by per-frame read path rather than next to the
		// handles they describe. m_IsDeviceLost has a comment and an accessor;
		// the other two have neither, which is why m_VkValidationLayersEnabled
		// below is the least discoverable member in this class.
		bool m_CommandListOpen = false;
		bool m_IsDeviceLost = false;
		// The only member with no accessor and no reader outside the .cpp's
		// device-creation path, where it gates whether
		// VK_LAYER_KHRONOS_validation is requested at instance AND device level.
		bool m_VkValidationLayersEnabled = false;

		// DEFECT: this comment is orphaned. It describes the anonymous log
		// callback's ability to call SetDeviceLost(), but sits directly above
		// m_Width and m_Height, which have nothing to do with device loss.
		// Reading the members in order it looks like a description of the viewport
		// size. It belongs above m_IsDeviceLost.
		// Allow the anonymous log callback to mark device loss
		// Cached window size, read by GetWidth/GetHeight. Written from
		// glfwGetWindowSize at Init() and from the new dimensions in Resize(),
		// so it tracks the swapchain rather than the OS window -- a minimised
		// window can report 0 without the window being 0x0.
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
	};

}
