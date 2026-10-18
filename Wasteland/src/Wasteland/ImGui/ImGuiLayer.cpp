// Precompiled header, conventionally the first include of every engine
// translation unit. It is what makes WL_CORE_ERROR, WL_PROFILE_FUNCTION
// and the standard containers usable below without a single local
// include; the cost is that this file inherits every header wlpch.h
// pulls in, whether or not ImGui needs it.
#include "wlpch.h"
// The layer's own header. It forward-declares
// `struct ID3D11ShaderResourceView` instead of including d3d11.h, which
// is the only reason the unconditional d3d11.h include further down does
// not infect every file that includes ImGuiLayer.h.
#include "ImGuiLayer.h"

// ImGui's public API: IMGUI_CHECKVERSION, CreateContext, GetIO, Render,
// GetDrawData and the ImGuiConfigFlags used in OnAttach and End all come
// from here. ImGuizmo later re-includes this same header, so the include
// guard is what keeps that from being a second definition.
#include "imgui.h"

// Marks the ImGui SYMBOLS as importable rather than dllexported, so the
// backend .cpp files below link against the engine's ImGui build.
// It has to precede every backend include, which is why it is separated
// from the imgui.h include above by a blank line.
#define IMGUI_IMPL_API
// Platform backend: window + input. Quoted because these sit in an
// imgui/ subdirectory of the third-party tree on the include path.
// Five backends are included unconditionally, so all five must compile
// even when only one is ever used at runtime (see the switch in
// InitBackend).
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
// DEFECT (unconditional DirectX dependency): d3d11.h and d3d12.h are
// included below with no #ifdef around any of it. A build machine
// without the Windows SDK cannot compile this translation unit at all,
// even for an OpenGL-only build. The NVRHI include on line 15 has the
// same problem.
// The standard ImGui guidance is to include only the backend for the
// API actually in use.
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_vulkan.h"

// Application::Get(), the singleton this file dereferences with no null
// check in four places: InitBackend, ShutdownBackend's Vulkan case,
// FallbackToOpenGL and End. Get() returns a reference to a function-local
// static, so all four fail identically if the Application was never
// constructed -- and none of them tests for that.
#include <Wasteland/Core/Application.h>
// RendererAPI::API, plus the static GetAPI() that OnAttach samples to
// decide which backend to build. Including the full header rather than a
// forward declaration is necessary because the enum is used by value in
// every member assignment below.
#include <Wasteland/Renderer/RendererAPI.h>
// The engine-side abstraction over DX11/DX12/Vulkan. Needed for the
// dynamic_cast in every non-OpenGL branch, and for the D3D12/Vulkan
// accessor calls.
#include "Platform/NVRHI/NVRHIContext.h"

// Existing marker. It refers to the GLFW and glad includes below, which
// are platform headers this layer should not know about -- it reaches
// straight into the GL and windowing APIs to query context validity
// and to save/restore the current context around viewport rendering.
// TEMPORARY
#include <GLFW/glfw3.h>
#include <glad/glad.h>

// DirectX headers
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
// The last of the unconditional graphics headers. Every
// VK_STRUCTURE_TYPE_* constant, the VK_SAMPLE_COUNT_* mask and the
// VkCommandBuffer/VkResult types used in InitBackend's Vulkan case and in
// End's Vulkan case come from here rather than from imgui_impl_vulkan.h,
// which only forward-declares the handles it passes through.
#include <vulkan/vulkan.h>

// The only use of this header is ImGuizmo::BeginFrame() in Begin(). It is
// spelled with quotes like the ImGui backends above rather than with
// angle brackets, so it resolves through the include path rather than
// the compiler's system directories.
#include "ImGuizmo.h"

// Opening brace on its own line, which differs from the same namespace
// written inline elsewhere in this codebase. The whole file uses
// brace-on-next-line, so this is internally consistent at least.
namespace Wasteland
{

 // The Layer base takes a name, which is how this layer is identified in
 // a profiler or a layer stack dump.
 // DEFECT: the body is empty, so nothing here initialises the ImGui
 // context -- all of that is deferred to OnAttach, which is the right
 // split (the context needs a graphics device) but means a constructed
 // ImGuiLayer is not usable until it is attached.
 ImGuiLayer::ImGuiLayer()
  : Layer("ImGuiLayer")
 {
 }

 // DEFECT (empty destructor): all teardown is in OnDetach (line 604).
 // An ImGuiLayer destroyed WITHOUT being detached leaks everything
 // OnAttach created -- the ImGui context, the backend user data, and
 // potentially the DX11 white SRV COM reference. Since ~ImGuiLayer()
 // does not call ImGui::DestroyContext(), the context leak is real if
 // anything ever lets a layer die without OnDetach running.
 // Calling ShutdownBackend() and DestroyContext() here would be
 // idempotent given OnDetach already did it.
 ImGuiLayer::~ImGuiLayer()
 {
 }

 // Creates the ImGui context and picks a renderer backend.
 // Layer hook, so the base class dispatches this -- unlike most of the
 // rest of this file, which is non-virtual plumbing.
 void ImGuiLayer::OnAttach()
 {
  WL_PROFILE_FUNCTION();

  // Setup Dear ImGui context
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  // Silences an unused-variable warning for the io reference on line 48.
  // DEFECT (vestigial): io IS used on lines 50-51 and again on line 56,
  // so this cast is dead. It is presumably a leftover from a version
  // where io was fetched but not configured.
  (void)io;
  // |= rather than = so flags accumulate.
  // Note this enables KEYBOARD nav but not ImGuiConfigFlags_NavEnableGamepad,
  // so gamepad navigation is off.
  // The flags are read back on line 56 and line 1045, both testing
  // ViewportsEnable -- which is NOT set here, so both of those blocks
  // are currently dead unless something else sets the flag.
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  // Docking without ViewportsEnable: docking works within the main window;
  // dragging a docked panel out into its own OS window does not.
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

  // Setup Dear ImGui style
  // The built-in dark theme. Any project styling would go after this.
  ImGui::StyleColorsDark();
  ImGuiStyle &style = ImGui::GetStyle();
  // Restyles for multi-viewport mode: square window corners and an opaque
  // background, because a detached OS window composites differently from
  // an in-app panel.
  // DEFECT (always false): ViewportsEnable is never set anywhere in this
  // file, so this block cannot execute as written. Same for the identical
  // check in PerformBackendSwitch on line 596 and in End on line 1045.
  // That means the viewport-rendering tail of End() (lines 1046-1051) is
  // also unreachable. Either the flag is supposed to be set here and is
  // missing, or a whole subsystem is switched off.
  if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) && m_CurrentBackend == RendererAPI::API::OpenGL)
  {
   // 0 = square corners.
   style.WindowRounding = 0.0f;
   style.Colors[ImGuiCol_WindowBg].w = 1.0f;
  }

  // Initialize the appropriate backend based on current renderer API
  RendererAPI::API currentAPI = RendererAPI::GetAPI();
  InitBackend(currentAPI);
 }

 // file-static helper, not a member: it needs no layer state.
 // Called from two places -- the OpenGL branch of InitBackend (line 92)
 // and FallbackToOpenGL (line 523). Both need to distinguish "no GL
 // context" from "GL context present", because every failure path in
 // this file falls back to OpenGL and that fallback needs a context.
 static bool HasValidGLContext()
 {
  // The cheap check first: no current context means glGetString on line
  // 71 would be a null-function-pointer call.
  if (!glfwGetCurrentContext())
   return false;
  // Queries the GL version string. The C-style cast is required because
  // glGetString returns GLubyte* for several enums and there is no single
  // overload in glad for GL_VERSION.
  // Note this is called on every frame path that reaches End() with a
  // viewport flag set -- currently dead, per the note on line 56.
  // Only the VERSION string is queried. GL_VENDOR, GL_RENDERER and
  // GL_SHADING_LANGUAGE_VERSION are all available and none are read, so a
  // context that reports a version but no usable profile passes this test
  // and fails later at shader compile time instead.
  const char* glVersion = (const char*)glGetString(GL_VERSION);
  // Null means the context exists but the query failed, which happens
  // when there is no current context OR the driver returns nothing.
  // The check therefore conflates two conditions, but both mean "cannot
  // use GL", which is all the callers need.
  return glVersion != nullptr;
 }

 // The switch that decides which ImGui renderer backend to bring up.
 // This is the largest function in the file and is dominated by
 // repetitive failure handling: every one of the ~14 failure branches
 // below does the identical
 //    if (FallbackToOpenGL()) m_CurrentBackend = OpenGL; else m_CurrentBackend = None;
 // pair. DEFECT (refactor candidate): that could be a single helper, or
 // a goto to one shared epilogue, and its absence is why the function
 // runs 370 lines.
 void ImGuiLayer::InitBackend(RendererAPI::API api)
 {
  WL_PROFILE_FUNCTION();

  // A reference to the singleton. Application::Get() dereferences its
  // static unconditionally, so calling InitBackend before the
  // Application exists is a null dereference -- there is no guard here.
  Application &app = Application::Get();
  // The one native window, fetched as void* and cast.
  // DEFECT (no null check): unlike the graphicsContext checks inside the
  // switch, `window` is used unguarded at line 104, 165, 272 and 370.
  // A null native window -- before the window is created, or after
  // teardown -- is passed straight into ImGui_ImplGlfw_Init*, which will
  // dereference it. FallbackToOpenGL DOES check window (line 531); this
  // function does not, which is an inconsistency within the file.
  GLFWwindow *window = static_cast<GLFWwindow *>(app.GetWindow().GetNativeWindow());

  // If already initialized to same API, nothing to do
  // Idempotence guard: re-initialising an already-initialised backend
  // would leak the previous one, since ImGui backends allocate user data
  // on Init and free it on Shutdown.
  // Note the OpenGL case on line 95 can still set m_CurrentBackend to
  // None and break, so m_CurrentBackend is the real source of truth for
  // "what is running", not the local api.
  if (api == m_CurrentBackend)
   return;

  // Ensure no previous platform backend is active (should be shutdown already)
  // Initialize renderer-specific backend
  // Five cases plus a default. Each NVRHI case repeats the same
  // shape: fetch context, check it, check its API matches, fetch
  // natives, check them, check the device is alive, init GLFW, init the
  // backend, set m_CurrentBackend.
  switch (api)
  {
  // The fallback target, and the only case with no NVRHI involvement.
  // Because every other case falls back to here, OpenGL is effectively
  // mandatory in every build -- which is consistent with the header
  // including GLFW and glad unconditionally.
  case RendererAPI::API::OpenGL:
  {
   if (!HasValidGLContext())
   {
    // The one failure path in InitBackend that does not attempt a fallback,
    // because there is nothing left to fall back to: the requested backend
    // was already OpenGL. Assigning None here also makes it self-amplifying
    // -- Begin() logs an error on every subsequent frame, see the note there.
    WL_CORE_ERROR("ImGui: No valid GL context for OpenGL backend, cannot initialize");
    // None rather than leaving the previous value, so the failure is visible
    // to HasBackend() in the header. Application's frame loop tests that
    // accessor before calling the draw callbacks, so a layer that lands here
    // silently stops drawing UI without the engine reporting anything.
    m_CurrentBackend = RendererAPI::API::None;
    break;
   }
   // Shutdown GLFW if it was initialized (e.g., from DX12 failure path)
   // Reached on a DX11/DX12/Vulkan -> OpenGL transition, when the GLFW
   // platform backend is still up from the NVRHI case. Shutting it down
   // first is what lets ImGui_ImplGlfw_InitForOpenGL below assert-clean:
   // ImGui_ImplGlfw_Init asserts that io.BackendPlatformUserData is null,
   // so a second Init without this would trip an assert in debug builds.
   if (m_GlfwInitialized)
   {
    ImGui_ImplGlfw_Shutdown();
    // Cleared before the re-Init rather than after it, so a failed Init still
    // leaves the flag false and ShutdownBackend will not call
    // ImGui_ImplGlfw_Shutdown for a platform backend that is not there.
    m_GlfwInitialized = false;
   }
   // InitForOpenGL rather than InitForOther, which the three NVRHI cases use.
   // The two differ in exactly one field, bd->ClientApi, and the only
   // consumers of that field are the multi-viewport platform-interface
   // callbacks (viewport window creation, activate and swap buffers).
   // Since ViewportsEnable is never set on this io, that choice currently
   // has no observable effect on any code path in this file.
   // install_callbacks = true: ImGui chain-overwrites the GLFW callbacks
   // for this window and restores them in ImGui_ImplGlfw_Shutdown.
   ImGui_ImplGlfw_InitForOpenGL(window, true);
   // Set unconditionally, so this records "InitForOpenGL was CALLED", not
   // "it succeeded". ShutdownBackend and the four other Init sites all trust
   // this flag to decide whether ImGui_ImplGlfw_Shutdown is legal, and that
   // call asserts on a null BackendPlatformUserData.
   m_GlfwInitialized = true;
   // GLSL 410, hardcoded -- the OpenGL 3.0-era GLSL, which works on any
   // GL 3.3+ context.
   // DEFECT: the return value is IGNORED, unlike line 167 and line 275
   // which both test their ImGui_Impl*_Init result. So if this fails,
   // m_CurrentBackend is set to OpenGL on line 107 and the layer believes
   // it has a working backend when it does not.
   // Note the version string here is lower than the #version 450 in the
   // project's own .glsl files -- which is correct, since the ImGui backend
   // supplies its own vertex/fragment shaders and does not use them.
   ImGui_ImplOpenGL3_Init("#version 410");
   m_CurrentBackend = api;
   WL_CORE_INFO("ImGui: Initialized OpenGL3 backend");
   break;
  }

  // One of three cases with the same five-gate shape. The API name here
  // does not select a backend -- it selects which natives this engine
  // context exposes, and the DX11 case is the only one whose backend
  // (ImGui_ImplDX11) can also be reached without an NVRHI context at all.
  case RendererAPI::API::NVRHI_DX11:
  {
   // dynamic_cast, not static_cast, because GetCurrentContext() returns a
   // GraphicsContext* base pointer that may be the OpenGL context.
   // DEFECT (RTTI dependency): this requires RTTI, which some release
   // builds disable with /GR- or -fno-rtti. Nothing here guards for it.
   // The same cast appears on lines 183, 304, 477, 725, 903 and 990.
   auto *graphicsContext = dynamic_cast<NVRHIContext *>(app.GetWindow().GetCurrentContext());
   // First of four validation gates in this case, each with its own log
   // message naming which backend was wanted.
   // Every gate falls back to OpenGL, so a misconfigured NVRHI context
   // degrades to GL rather than failing the launch.
   if (!graphicsContext)
   {
    WL_CORE_ERROR("ImGui: Failed to get NVRHI graphics context for DX11, falling back to OpenGL");
    // The start of the fallback pair that this file repeats 14 times. Written
    // as a two-armed if/else so m_CurrentBackend lands on either OpenGL or
    // None without a second test; a helper taking m_CurrentSwitchTarget
    // would remove every copy.
    if (FallbackToOpenGL())
     m_CurrentBackend = RendererAPI::API::OpenGL;
    else
     m_CurrentBackend = RendererAPI::API::None;
    break;
   }

   // Second gate: the context is an NVRHIContext but for a different API.
   // This is the check that catches a stale context after a device or
   // backend change -- WindowsWindow swaps m_CurrentContext to the OpenGL
   // context for API::OpenGL, so an NVRHIContext can outlive the API it
   // was built for.
   if (graphicsContext->GetAPI() != RendererAPI::API::NVRHI_DX11)
   {
    // The offending API is logged as an int, not as the enum's name, so the
    // message is unreadable without the RendererAPI::API declaration to hand.
    // Every WL_CORE_* call in this file does the same cast.
    WL_CORE_ERROR("ImGui: Graphics context is not DX11 (API: {0}), falling back to OpenGL", (int)graphicsContext->GetAPI());
    if (FallbackToOpenGL())
     m_CurrentBackend = RendererAPI::API::OpenGL;
    else
     m_CurrentBackend = RendererAPI::API::None;
    break;
   }

   // static_cast, not dynamic_cast: these accessors return void* from the
   // NVRHI context and the DX11 case has already proved the context's API is
   // NVRHI_DX11, so the target type is certain. That is also why the two
   // casts here cannot fail silently the way the context cast above can.
   ID3D11Device *device = static_cast<ID3D11Device *>(graphicsContext->GetD3D11Device());
   ID3D11DeviceContext *context = static_cast<ID3D11DeviceContext *>(graphicsContext->GetD3D11Context());

   // device and context are fetched together because the DX11 backend needs
   // both and neither is useful alone; the combined test keeps the error
   // message singular, at the cost of not distinguishing which one is null.
   if (!device || !context)
   {
    WL_CORE_ERROR("ImGui: DX11 device or context is null, falling back to OpenGL");
    if (FallbackToOpenGL())
     m_CurrentBackend = RendererAPI::API::OpenGL;
    else
     m_CurrentBackend = RendererAPI::API::None;
    break;
   }

   // The device-lost check, done BEFORE initialising the backend.
   // This is a good gate: a device that has been removed (TDR, driver
   // upgrade, GPU reset) cannot be used, and ImGui_ImplDX11_Init would
   // otherwise succeed and produce a black or crashing UI.
   // Queried before the backend is initialised, so a removed device never
   // reaches ImGui_ImplDX11_Init. Cheap relative to the UI: it is a driver
   // call, not a GPU sync.
   HRESULT removedReason = device->GetDeviceRemovedReason();
   if (FAILED(removedReason))
   {
    // The HRESULT is printed in hex with the X format specifier and cast to
    // uint32_t first. DXGI_ERROR_DEVICE_REMOVED (0x887A0005) and
    // DXGI_ERROR_DEVICE_RESET (0x887A0007) are the two that matter in
    // practice; neither is distinguishable from the message text alone.
    WL_CORE_ERROR("ImGui: DX11 device has been removed (HRESULT: {0:X}), falling back to OpenGL", (uint32_t)removedReason);
    if (FallbackToOpenGL())
     m_CurrentBackend = RendererAPI::API::OpenGL;
    else
     m_CurrentBackend = RendererAPI::API::None;
    break;
   }

   // Shutdown GLFW if it was initialized (e.g., from previous backend)
   if (m_GlfwInitialized)
   {
    ImGui_ImplGlfw_Shutdown();
    m_GlfwInitialized = false;
   }
   // InitForOther: the DX11 case shares the window with the engine's own
   // NVRHI submission, so there is no OpenGL client API for ImGui to know
   // about. Same dead-viewport caveat as the InitForOpenGL call above.
   ImGui_ImplGlfw_InitForOther(window, true);
   m_GlfwInitialized = true;
   // Unlike the OpenGL case on line 106, this DOES check the result.
   // So the three NVRHI cases check their init and the OpenGL case does
   // not -- an inconsistency in exactly the file where such an
   // inconsistency has the most effect.
   // Checked, unlike the OpenGL case's ImGui_ImplOpenGL3_Init -- but the
   // check can never fire. In this vendored ImGui, ImGui_ImplDX11_Init has
   // exactly one return statement, `return true`: it asserts on an already
   // initialised renderer backend and on a null device, and otherwise has no
   // failure path at all. ImGui_ImplDX12_Init and ImGui_ImplVulkan_Init are
   // the same shape, so all three "Init failed, falling back to OpenGL"
   // branches below are unreachable.
   //
   // ImGui_ImplOpenGL3_Init is the only one of the four that can actually
   // return false, which inverts the picture: the one Init result this file
   // ignores is the only one that carries information, and the three it
   // checks cannot report anything.
   //
   // What DOES fall back to OpenGL, then, is the validation above this line:
   // a null or mismatched context, null natives, a removed device, and (for
   // DX12) an unusable font atlas. FallbackToOpenGL is reachable from those
   // gates and from nowhere else.
   if (!ImGui_ImplDX11_Init(device, context))
   {
    WL_CORE_ERROR("ImGui: DX11 Init failed, falling back to OpenGL");
    if (FallbackToOpenGL())
     m_CurrentBackend = RendererAPI::API::OpenGL;
    else
     m_CurrentBackend = RendererAPI::API::None;
    break;
   }
   // The single assignment that makes Begin() and End() take the DX11 paths.
   // Until it happens, m_CurrentBackend still holds the value from the
   // idempotence guard near the top of this function, so an exception or a
   // re-entrant call between here and the log line sees the old backend.
   m_CurrentBackend = api;
   // The only success log in the DX11 case, and it is logged before the
   // function returns rather than after -- so it reports that Init and Init
   // of the backend both returned, not that a frame has ever been drawn.
   WL_CORE_INFO("ImGui: Initialized DirectX 11 backend");
   break;
  }

  // The DX12 case is the longest because it is the only one that has to
  // pre-validate the font atlas, tear down a stale renderer backend, and
  // then create device objects explicitly -- work the DX11 backend hides
  // inside its NewFrame and the Vulkan backend hides inside its own init.
  case RendererAPI::API::NVRHI_DX12:
  {
   // The same dynamic_cast as the DX11 case, repeated once per NVRHI case.
   // Three of the four NVRHI branches could hoist it above the switch; the
   // OpenGL branch is the only one that must not, because its context is a
   // different concrete class.
   auto *graphicsContext = dynamic_cast<NVRHIContext *>(app.GetWindow().GetCurrentContext());
   if (!graphicsContext)
   {
    WL_CORE_ERROR("ImGui: Failed to get NVRHI graphics context for DX12, falling back to OpenGL");
    if (FallbackToOpenGL())
     m_CurrentBackend = RendererAPI::API::OpenGL;
    else
     m_CurrentBackend = RendererAPI::API::None;
    break;
   }

   // The API-mismatch gate, identical in shape to DX11's. Note it compares
   // against NVRHI_DX12, not DX12: the engine prefixes every D3D backend
   // with NVRHI_, so a bare DX12 enumerator does not exist.
   if (graphicsContext->GetAPI() != RendererAPI::API::NVRHI_DX12)
   {
    WL_CORE_ERROR("ImGui: Graphics context is not DX12 (API: {0}), falling back to OpenGL", (int)graphicsContext->GetAPI());
    if (FallbackToOpenGL())
     m_CurrentBackend = RendererAPI::API::OpenGL;
    else
     m_CurrentBackend = RendererAPI::API::None;
    break;
   }

   // Six natives fetched and only then checked, as one six-way condition.
   // Nothing between the fetches and the check can fail, so the ordering is
   // purely to keep the failure message to a single line -- at the cost of
   // not saying WHICH native was missing.
   ID3D12Device *device = static_cast<ID3D12Device *>(graphicsContext->GetD3D12Device());
   ID3D12CommandQueue *commandQueue = static_cast<ID3D12CommandQueue *>(graphicsContext->GetD3D12CommandQueue());
   ID3D12DescriptorHeap *srvHeap = static_cast<ID3D12DescriptorHeap *>(graphicsContext->GetD3D12ImGuiSRVHeap());
   // Copied BY VALUE. D3D12_CPU_DESCRIPTOR_HANDLE is a POD wrapper around
   // a UINT64 offset, so copying is free and correct here; the same is true
   // of the GPU handle below. If these were COM pointers the copy would be
   // a reference-count bug -- which is exactly why they are not.
   D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = graphicsContext->GetD3D12ImGuiSRVHeapCPU();
   D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = graphicsContext->GetD3D12ImGuiSRVHeapGPU();

   // Five-way check in one condition.
   // DEFECT (handle comparison): D3D12 CPU/GPU descriptor handles are
   // UINT64 offsets into a heap, and 0 is a legitimate offset (the first
   // descriptor). Testing `== 0` therefore treats a valid heap-start handle
   // as failure. For ImGui's SRV heap the first descriptor is usually used,
   // so in practice this is a false positive rather than a missed one, but
   // the check is not sound.
   // Checked together with the pointer natives so the single log line can
   // cover all six. The handle members are the .ptr inside each struct,
   // hence the explicit field access rather than a bare truth test.
   if (!device || !commandQueue || !srvHeap || cpuHandle.ptr == 0 || gpuHandle.ptr == 0)
   {
    WL_CORE_ERROR("ImGui: Failed to get DX12 device/command queue/descriptor heap, falling back to OpenGL");
    if (FallbackToOpenGL())
     m_CurrentBackend = RendererAPI::API::OpenGL;
    else
     m_CurrentBackend = RendererAPI::API::None;
    break;
   }

   // A previous half-initialized DX12 backend leaves BackendRendererUserData
   // set (ImGui_ImplDX12_Init always succeeds after allocating it). Re-init
   // without shutdown leaks/corrupts it and the next CreateFontsTexture faults
   // on stale state. Tear it down first.
   // Existing comment above explains the stale-state problem this guards.
   // The check reaches into ImGui internals rather than tracking ImGui
   // backends with its own flag, which is fragile but works because the
   // field is where every ImGui backend stores its state.
   // Reached only when a previous InitBackend left a renderer backend
   // registered. Since PerformBackendSwitch calls ShutdownBackend first,
   // the two callers that can actually reach a second init (OnAttach on a
   // reused context, and a fallback path) are the ones where this fires.
   // DEFECT (wrong backend shut down): the field is shared by ALL ImGui
   // renderer backends, so if the live backend is OpenGL or Vulkan, this
   // calls ImGui_ImplDX12_Shutdown() on an ImGui_ImplOpenGL3_Data or
   // ImGui_ImplVulkan_Data and frees it as the wrong type. It is latent
   // today only because both current callers happen to shut down first.
   if (ImGui::GetIO().BackendRendererUserData != nullptr)
   {
    WL_CORE_WARN("ImGui: Stale DX12 renderer backend data found, shutting down before re-init");
    ImGui_ImplDX12_Shutdown();
   }

   // Validate the font atlas BEFORE CreateDeviceObjects: its internal upload
   // does memcpy(pixels) with no null check (faults inside ucrtbase). Rebuild
   // here so the backend only sees a valid atlas.
   {
    unsigned char *fontPixels = nullptr;
    int fontW = 0, fontH = 0;
    // GetTexDataAsRGBA32 builds the atlas if it has not been built, so this
    // call is what forces the CPU-side font upload -- and it is a real CPU
    // cost (a memcpy of width*height*4 bytes) that runs on every DX12 init.
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&fontPixels, &fontW, &fontH);
    if (!fontPixels || fontW <= 0 || fontH <= 0)
    {
     WL_CORE_WARN("ImGui: Font atlas empty before DX12 init, rebuilding default font");
     // Clear() discards the whole atlas, not just its GPU texture: every font
     // added since CreateContext is gone, and TexReady goes false so the next
     // GetTexDataAsRGBA32 rebuilds from an empty atlas. Anything that loaded
     // a custom font before this point silently reverts to the default.
     ImGui::GetIO().Fonts->Clear();
     ImGui::GetIO().Fonts->AddFontDefault();
     ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&fontPixels, &fontW, &fontH);
    }
    // The 16384 bound is the DX12 texture-dimension limit, so this is a
    // genuine device-capability check rather than an arbitrary one.
    // Runs AFTER the rebuild attempt above, so this catches a rebuild that
    // also failed.
    // The 16384 bound is checked AFTER the rebuild above, so it also covers
    // the case where the rebuild produced a valid-but-oversized atlas --
    // which cannot happen from AddFontDefault, so in practice the bound only
    // ever re-tests the first failure.
    if (!fontPixels || fontW <= 0 || fontH <= 0 || fontW > 16384 || fontH > 16384)
    {
     WL_CORE_ERROR("ImGui: Font atlas invalid ({0}x{1}), cannot init DX12 backend", fontW, fontH);
     if (FallbackToOpenGL())
      m_CurrentBackend = RendererAPI::API::OpenGL;
     else
      m_CurrentBackend = RendererAPI::API::None;
     break;
    }
   }

   // Validate device
   // Same device-lost gate as DX11 on line 148, and it comes AFTER the
   // font-atlas work above rather than before.
   // That ordering is a defect: the atlas is uploaded by
   // CreateDeviceObjects later, so the wasted work is bounded, but the
   // stale-backend shutdown on lines 224-228 has already happened by the
   // time this bails -- meaning a removed device leaves the previous
   // backend shut down and m_CurrentBackend None.
   // Deliberately after the font-atlas block rather than before it. The
   // cost is bounded because the atlas work is pure CPU and nothing has been
   // submitted to the GPU yet; the real problem is that the stale-backend
   // shutdown above has already run, so this branch leaves the layer with
   // no renderer backend at all rather than the one it had.
   if (FAILED(device->GetDeviceRemovedReason()))
   {
    WL_CORE_ERROR("ImGui: DX12 device removed, falling back to OpenGL");
    if (FallbackToOpenGL())
     m_CurrentBackend = RendererAPI::API::OpenGL;
    else
     m_CurrentBackend = RendererAPI::API::None;
    break;
   }

   // Shutdown GLFW if it was initialized (e.g., from previous backend)
   if (m_GlfwInitialized)
   {
    ImGui_ImplGlfw_Shutdown();
    m_GlfwInitialized = false;
   }
   // The GLFW half of the backend, brought up before ImGui_ImplDX12_Init
   // below. Both are needed: the platform backend supplies input and window
   // state, the renderer backend supplies the draw calls.
   ImGui_ImplGlfw_InitForOther(window, true);
   m_GlfwInitialized = true;
   // DX12 needs num frames in flight = 2, format = R8G8B8A8_UNORM
   // num_frames_in_flight = 2 and the swapchain format, hardcoded.
   // DEFECT (mismatch risk): the backbuffer format is taken from
   // DXGI_FORMAT_R8G8B8A8_UNORM here, but the render path in End() adapts
   // to whatever the backbuffer actually is (lines 790 and 799). If the
   // swapchain is not R8G8B8A8_UNORM -- B8G8R8A8 is common -- the ImGui
   // shader's output format and the actual RTV disagree and the UI renders
   // with wrong channel order.
   // num_frames_in_flight = 2 is not cosmetic. The DX12 backend keeps an
   // array of 2 frame-render-buffer sets and alternates between them
   // (FrameRenderBuffers[FrameIndex % numFramesInFlight]), so the argument
   // selects between two upload buffers.
   // This layer then resets ONE command allocator every frame with no fence
   // wait (see the note in End), which defeats the two-deep buffering that
   // value implies: the GPU can still be reading the set the CPU is
   // about to overwrite.
   // Unreachable failure branch, for the same reason as the DX11 check
   // above: this vendored ImGui_ImplDX12_Init allocates its backend data,
   // optionally initialises the platform interface, allocates the main
   // viewport's renderer data, and returns true -- with no early return
   // anywhere. So the ShutdownBackend call inside this branch, the
   // fallback, and the None assignment below are all dead.
   //
   // The argument validation the backend does NOT do is done here instead:
   // the null-device and zero-handle gate earlier in this case is what
   // stops a null device reaching a function that would dereference it.
   // That gate, not this check, is what makes the DX12 path survivable.
   if (!ImGui_ImplDX12_Init(device, 2, DXGI_FORMAT_R8G8B8A8_UNORM, srvHeap, cpuHandle, gpuHandle))
   {
    WL_CORE_ERROR("ImGui: DX12 Init failed, falling back to OpenGL");
    // Undo-on-failure, mirroring the guard above. ImGui_ImplDX12_Init has
    // already allocated its backend data by the time it returns false, so
    // without this the next init would allocate a second one and trip the
    // "Already initialized a renderer backend!" assert.
    if (ImGui::GetIO().BackendRendererUserData != nullptr)
     ImGui_ImplDX12_Shutdown();
    if (FallbackToOpenGL())
     m_CurrentBackend = RendererAPI::API::OpenGL;
    else
     m_CurrentBackend = RendererAPI::API::None;
    break;
   }
   // Create device objects (fonts)
   // The font upload, checked.
   // Note DX11 has no equivalent check here: its CreateDeviceObjects
   // path is inside ImGui_ImplDX11_NewFrame, and the comment on line 744
   // explains that NewFrame retries it every frame on failure.
   // The DX11 case has no equivalent: ImGui_ImplDX11_NewFrame calls
   // CreateDeviceObjects itself when pFontSampler is null, so the DX11 path
   // retries a failed font upload on every frame instead of giving up and
   // falling back. Two backends, two different recovery philosophies.
   if (!ImGui_ImplDX12_CreateDeviceObjects())
   {
    WL_CORE_ERROR("ImGui: DX12 CreateDeviceObjects failed, falling back to OpenGL");
    // Shutdown unconditionally, unlike the guarded call above: at this point
    // the backend data is known to exist because Init returned true, so the
    // guard would be noise. The cost is that this is a second place that
    // must be updated if the backend is ever swapped.
    ImGui_ImplDX12_Shutdown();
    if (FallbackToOpenGL())
     m_CurrentBackend = RendererAPI::API::OpenGL;
    else
     m_CurrentBackend = RendererAPI::API::None;
    break;
   }
   // The success assignment. Nothing after it can fail, so there is no
   // path that leaves the DX12 backend initialised but m_CurrentBackend
   // still None -- unlike the earlier branches, which call
   // FallbackToOpenGL and so can reach OpenGL from inside this case.
   m_CurrentBackend = api;
   // Logged before the break rather than after the switch, so the message
   // ordering in the log is per-case rather than per-function.
   WL_CORE_INFO("ImGui: Initialized DirectX 12 backend");
   break;
  }

  // The longest case, and the only one that submits to a queue during
  // Init (the font upload on lines 403-428).
  // The longest case. Two structural differences from DX12: it submits to
  // a queue during Init (the font upload below), and it has no explicit
  // CreateDeviceObjects step because ImGui_ImplVulkan_Init builds the
  // font texture lazily on the first CreateFontsTexture call.
  case RendererAPI::API::NVRHI_Vulkan:
  {
   auto *graphicsContext = dynamic_cast<NVRHIContext *>(app.GetWindow().GetCurrentContext());
   if (!graphicsContext)
   {
    WL_CORE_ERROR("ImGui: Failed to get NVRHI graphics context for Vulkan, falling back to OpenGL");
    if (FallbackToOpenGL())
     m_CurrentBackend = RendererAPI::API::OpenGL;
    else
     m_CurrentBackend = RendererAPI::API::None;
    break;
   }

   // Third and last copy of the API-mismatch gate. Together with the two
   // above it is the whole of this file's defence against a context that
   // outlived the API it was created for.
   if (graphicsContext->GetAPI() != RendererAPI::API::NVRHI_Vulkan)
   {
    WL_CORE_ERROR("ImGui: Graphics context is not Vulkan (API: {0}), falling back to OpenGL", (int)graphicsContext->GetAPI());
    if (FallbackToOpenGL())
     m_CurrentBackend = RendererAPI::API::OpenGL;
    else
     m_CurrentBackend = RendererAPI::API::None;
    break;
   }

   // Four Vulkan handles, all non-owning. The engine's NVRHI context owns
   // them and outlives the ImGui layer, so nothing here releases anything --
   // which is the correct ownership model, and the reason
   // ShutdownVulkanImGuiResources below only touches the descriptor pool and
   // render pass that NVRHIContext itself created.
   VkInstance instance = graphicsContext->GetVkInstance();
   VkPhysicalDevice physicalDevice = graphicsContext->GetVkPhysicalDevice();
   VkDevice device = graphicsContext->GetVkDevice();
   VkQueue queue = graphicsContext->GetVkGraphicsQueue();

   // physicalDevice in particular is redundant: vkQueueSubmit and the
   // device-wait below need only the device. It is required by
   // ImGui_ImplVulkan_InitInfo, so it has to be fetched and checked even
   // though this function never uses it directly.
   if (!instance || !physicalDevice || !device || !queue)
   {
    WL_CORE_ERROR("ImGui: Vulkan native objects are null, falling back to OpenGL");
    if (FallbackToOpenGL())
     m_CurrentBackend = RendererAPI::API::OpenGL;
    else
     m_CurrentBackend = RendererAPI::API::None;
    break;
   }

   // The Vulkan equivalent of the DX12 SRV/RTV heaps: descriptor pool,
   // render pass and framebuffers, created by the context rather than by
   // this layer.
   // Creates the descriptor pool, render pass and framebuffers that
   // ImGui_ImplVulkan_Init needs, and that GetVkImGuiDescriptorPool below
   // then reads back. Doing this through the context rather than here is why
   // this layer contains no vkCreateDescriptorPool call at all.
   if (!graphicsContext->InitVulkanImGuiResources())
   {
    WL_CORE_ERROR("ImGui: Failed to init Vulkan ImGui resources, falling back to OpenGL");
    if (FallbackToOpenGL())
     m_CurrentBackend = RendererAPI::API::OpenGL;
    else
     m_CurrentBackend = RendererAPI::API::None;
    break;
   }

   VkDescriptorPool descriptorPool = graphicsContext->GetVkImGuiDescriptorPool();
   VkRenderPass renderPass = graphicsContext->GetVkImGuiRenderPass();

   // Checked before the backend init so the failure is attributed to the
   // context rather than to ImGui, whose error message would say only that
   // Init returned false.
   if (!descriptorPool || !renderPass)
   {
    WL_CORE_ERROR("ImGui: Vulkan descriptor pool or render pass is null, falling back to OpenGL");
    // Undo for the init above. Not mirrored on the DX11 or DX12 paths, which
    // have no equivalent engine-owned resources to release -- another way in
    // which these four cases are not symmetrical.
    graphicsContext->ShutdownVulkanImGuiResources();
    if (FallbackToOpenGL())
     m_CurrentBackend = RendererAPI::API::OpenGL;
    else
     m_CurrentBackend = RendererAPI::API::None;
    break;
   }

   // Shutdown GLFW if it was initialized (e.g., from previous backend)
   if (m_GlfwInitialized)
   {
    ImGui_ImplGlfw_Shutdown();
    m_GlfwInitialized = false;
   }
   // InitForOther again, but here with a Vulkan caveat the backend does not
   // model: ImGui_ImplGlfw_InitForVulkan exists and would set
   // ClientApi to Vulkan, and the only three consumers of that field are
   // multi-viewport paths. With viewports disabled the three choices are
   // indistinguishable, so InitForOther is as correct as the Vulkan variant.
   ImGui_ImplGlfw_InitForOther(window, true);
   m_GlfwInitialized = true;

   // Value-initialised, which is the required zero for every field the
   // backend does not set -- notably DynamicRendering and
   // PipelineCache on old SDKs.
   // Zero-initialising a struct this large matters more than usual: ImGui
   // reads fields conditionally on the SDK version, so a field left
   // uninitialised is a read of stack garbage, not merely a wrong value.
   ImGui_ImplVulkan_InitInfo initInfo = {};
   initInfo.Instance = instance;
   initInfo.PhysicalDevice = physicalDevice;
   // The three natives the backend uses for submission, grouped because
   // ImGui_ImplVulkan_Init treats them as a set: the graphics queue must
   // belong to QueueFamily, and this layer never verifies that itself -- it
   // trusts GetVkGraphicsQueueFamilyIndex to agree with GetVkGraphicsQueue.
   initInfo.Device = device;
   // Asked of the context rather than derived from the queue. Vulkan permits
   // several queues per family; picking the wrong index here produces a
   // validation error at the first vkQueueSubmit, which lands in
   // CheckVkResultFn -- and that callback is null here, so it produces
   // nothing at all.
   initInfo.QueueFamily = graphicsContext->GetVkGraphicsQueueFamilyIndex();
   initInfo.Queue = queue;
   // No pipeline cache: ImGui recompiles its pipelines on every init.
   // Acceptable for a UI backend (a handful of pipelines) but it is a
   // deliberate perf cost, and it means a backend switch recompiles them.
   initInfo.PipelineCache = VK_NULL_HANDLE;
   initInfo.DescriptorPool = descriptorPool;
   // Render pass 0, because this engine does not use subpasses.
   initInfo.Subpass = 0;
   initInfo.MinImageCount = graphicsContext->GetVkImageCount();
   initInfo.ImageCount = graphicsContext->GetVkImageCount();
   // No MSAA. DEFECT (unverified against the render pass): the render pass
   // on line 351 is created by NVRHIContext, and if that render pass was
   // created with a multisample attachment while ImGui is told 1 sample,
   // the framebuffer and the pipeline disagree and the UI fails to render.
   // Nothing in this file checks, and the assumption is implicit.
   initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
   // No custom Vulkan allocator, so the backend uses default malloc.
   // That means ImGui's Vulkan allocations are invisible to whatever
   // allocator tracking the engine uses for the rest of the frame.
   initInfo.Allocator = nullptr;
   // DEFECT (lost diagnostics): a null check callback means every
   // ImGui_ImplVulkan vkXxx call that fails is SILENT. Vulkan failures in
   // this file are otherwise reported (lines 409, 1010, 1030), so this
   // is the one path where a bad descriptor allocation or pipeline
   // creation produces a silently broken UI.
   initInfo.CheckVkResultFn = nullptr;

   // Also unreachable. ImGui_ImplVulkan_Init in this vendored tree has a
   // single `return true`; everything it rejects, it rejects with
   // IM_ASSERT -- Instance, PhysicalDevice, Device, Queue,
   // DescriptorPool, MinImageCount >= 2 and ImageCount >= MinImageCount.
   //
   // DEFECT (release builds lose the validation entirely): IM_ASSERT
   // compiles out unless NDEBUG is undefined, so in a shipping build all
   // seven checks vanish and Init proceeds with whatever initInfo holds.
   // The checks this file performs before the call -- non-null instance,
   // physical device, device and queue, non-null descriptor pool and
   // render pass -- are therefore doing the backend's job for it, which
   // is the one thing in this case that is genuinely load-bearing.
   if (!ImGui_ImplVulkan_Init(&initInfo, renderPass))
   // The braces are redundant around a single if/else/break, unlike every
   // other case in this switch. Left as written; it is a readability wart,
   // not a defect.
   {
    WL_CORE_ERROR("ImGui: Vulkan Init failed, falling back to OpenGL");
    // Order matters in this teardown pair: the platform backend is shut down
    // BEFORE the renderer backend, which is the opposite of ShutdownBackend's
    // renderer-then-platform order. Harmless here because nothing below
    // reaches for GLFW state, but it is the kind of inversion that breaks
    // when someone adds cleanup that does.
    ImGui_ImplGlfw_Shutdown();
    m_GlfwInitialized = false;
    // Safe to call on a backend whose Init returned false, because
    // ImGui_ImplVulkan_Shutdown nulls io.BackendRendererUserData and returns
    // early when it finds null -- and ImGui_ImplVulkan_Init itself calls
    // Shutdown internally on its own failure paths.
    ImGui_ImplVulkan_Shutdown();
    graphicsContext->ShutdownVulkanImGuiResources();
    if (FallbackToOpenGL())
     m_CurrentBackend = RendererAPI::API::OpenGL;
    else
     m_CurrentBackend = RendererAPI::API::None;
    break;
   }

   // Upload fonts
   // The command buffer is owned by the context, so this layer borrows
   // it and resets it again on line 422.
   VkCommandBuffer cmd = graphicsContext->GetCurrentVulkanImGuiCommandBuffer();
   if (cmd)
   {
    // sType is mandatory in every Vulkan struct: the loader validates it
    // against the struct size, and a zero sType is rejected. The rest of the
    // struct is value-initialised, which leaves pInheritanceInfo null.
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    // |= rather than = because flags is a bitmask; here there is only one
    // flag, so the two are equivalent. ONE_TIME_SUBMIT tells the driver this
    // buffer is recorded once and submitted once, which lets it choose
    // better placement for the upload.
    beginInfo.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult res = vkBeginCommandBuffer(cmd, &beginInfo);
    if (res == VK_SUCCESS)
    {
     // Return value IGNORED, unlike the CreateDeviceObjects checks in the
     // DX12 case. If the font texture fails to build, the layer still logs
     // "Initialized Vulkan backend" below and Begin/End will happily submit
     // draw commands that sample a descriptor pointing at nothing.
     ImGui_ImplVulkan_CreateFontsTexture(cmd);
     // Also unchecked. If this fails the submit below pushes a command buffer
     // that was never ended, which is a validation error rather than a crash
     // -- and, per the note at initInfo.CheckVkResultFn, it is silent.
     vkEndCommandBuffer(cmd);
     // Three fields set and the rest zero. The zero fields matter: pWaitSemaphores
     // and pWaitDstStageMask are null, so this submit cannot wait for the
     // swapchain and cannot be made to wait later -- which is why the
     // vkDeviceWaitIdle below is used instead of a semaphore.
     VkSubmitInfo submitInfo = {};
     submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
     submitInfo.commandBufferCount = 1;
     submitInfo.pCommandBuffers = &cmd;
     // The fence is VK_NULL_HANDLE, so nothing downstream can wait on this
     // submission except a full device wait. pCommandBuffers points at the
     // stack local `cmd`, which is why the submit must complete before this
     // scope ends -- it does not, because the wait below is synchronous.
     vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
     // DEFECT (device-wide stall): this blocks the ENTIRE device until every
     // queue is idle, not just the graphics queue the submit went to. It is
     // done once during Init, which bounds the cost, but it is a full GPU
     // sync in the middle of startup.
     // The narrower alternative is a fence on `queue` plus a host wait.
     vkDeviceWaitIdle(device);
     ImGui_ImplVulkan_DestroyFontUploadObjects();
     // Reset for next frame use
     // Returns the borrowed command buffer to a clean state so the next
     // frame can use it. Flag 0 means no release flags.
     // DEFECT (cleanup ordering): this runs BEFORE the success path ends,
     // so it happens even if ImGui_ImplVulkan_CreateFontsTexture left the
     // buffer in an error state. And if vkEndCommandBuffer on line 413
     // failed, this reset would mask it.
     vkResetCommandBuffer(cmd, 0);
    }
    else
    // The failure branch of the font upload. Note it does NOT undo anything:
    // the command buffer is left begun and the backend is still initialised,
    // so the layer proceeds to log success. The upload is simply skipped.
    {
     // WARN, not ERROR: the layer treats a failed font upload as survivable
     // and carries on, which is defensible for a UI atlas but leaves the
     // caller with no way to know the UI is about to sample a stale TexID.
     WL_CORE_WARN("ImGui: Failed to begin command buffer for font upload");
    }
   }

   // Same shape as the DX11 and DX12 success assignments. This one is
   // reachable even when the font upload above failed, because that failure
   // only warns -- so Vulkan can reach m_CurrentBackend = api with a
   // backend whose font texture was never uploaded.
   m_CurrentBackend = api;
   WL_CORE_INFO("ImGui: Initialized Vulkan backend");
   break;
  }

  // Any other API value, including RendererAPI::API::None.
  // DEFECT (no-op on None): calling InitBackend(None) logs "Unsupported
  // renderer API" as an ERROR and then tries to fall back to OpenGL.
  // None is a legitimate value meaning "no backend", not a user error, so
  // a caller resetting the layer would get a spurious error.
  // Reachable only from OnAttach with RendererAPI::GetAPI() == None, or
  // from PerformBackendSwitch, which also refuses to switch to the API
  // that is already current. So in practice None never arrives here and
  // the spurious ERROR is unreachable today -- but InitBackend(None) is
  // still a legal call that logs an error and falls back to OpenGL,
  // which is the opposite of what "switch to none" should mean.
  // End of the switch, and end of InitBackend. Nothing reports success
  // or failure upward: the only signal is m_CurrentBackend, which every
  // caller must re-read. That is why the fourteen fallback pairs assign
  // to the member instead of returning a status code.
  default:
   WL_CORE_ERROR("ImGui: Unsupported renderer API {0}, falling back to OpenGL", (int)api);
   if (FallbackToOpenGL())
    m_CurrentBackend = RendererAPI::API::OpenGL;
   else
    m_CurrentBackend = RendererAPI::API::None;
   break;
  }
 }

 // Mirrors InitBackend, and must stay in step with it: an API added to
 // the init switch but not the shutdown switch leaks its backend state.
 // Must stay in step with the switch in InitBackend: an API added there
 // and not here leaks its backend state, because nothing else in the
 // engine tears ImGui backends down.
 void ImGuiLayer::ShutdownBackend()
 {
  WL_PROFILE_FUNCTION();

  // Nothing to tear down. The comment says "handles None check
  // internally", which is this line -- so callers need not check.
  if (m_CurrentBackend == RendererAPI::API::None)
   return;

  // Shutdown renderer-specific backend
  switch (m_CurrentBackend)
  {
  // ShutdownBackend() is called BEFORE ImGui::DestroyContext() in OnDetach,
  // which is required: every ImGui_Impl*_Shutdown asserts on its own
  // backend data being non-null, so the context must still exist here.
  case RendererAPI::API::OpenGL:
   ImGui_ImplOpenGL3_Shutdown();
   break;

  // The white SRV is released here and in FallbackToOpenGL, and nowhere
  // else. It is created lazily inside End's DX11 render path, so a layer
  // that attached and detached without ever rendering keeps nothing -- but
  // a layer that rendered once and then switched away from DX11 through
  // any path other than these two would leak the COM reference.
  case RendererAPI::API::NVRHI_DX11:
   ImGui_ImplDX11_Shutdown();
   // Null-checked rather than assumed, because m_D3D11WhiteSRV is only
   // created on the first DX11 frame that actually needs it. A DX11 session
   // that never rendered has a null here and Release would fault.
   if (m_D3D11WhiteSRV)
   {
    m_D3D11WhiteSRV->Release();
    // Nulled after the release, not before, so that a fault inside Release
    // leaves a double-release rather than a leak. Either order is a bug;
    // this one at least fails loudly.
    m_D3D11WhiteSRV = nullptr;
   }
   break;

  // The DX12 case releases nothing beyond the backend itself: the SRV and
  // GPU descriptor heaps were created by NVRHIContext and belong to it.
  // Only the Vulkan case below calls back into the context, which is the
  // asymmetry to watch when a new backend is added.
  case RendererAPI::API::NVRHI_DX12:
   // Note the DX12 case does NOT do the stale-data check that InitBackend
   // line 224 does. That asymmetry is fine here (shutdown is the end, not a
   // re-init) but it means the two functions are not symmetric.
   ImGui_ImplDX12_Shutdown();
   break;

  case RendererAPI::API::NVRHI_Vulkan:
   // The renderer backend goes first, then the engine-owned Vulkan resources
   // it was rendering into. Reversing these two would leave the render pass
   // and framebuffers alive while ImGui still references them.
   ImGui_ImplVulkan_Shutdown();
   // Shutdown NVRHI Vulkan ImGui resources
   {
    // Application::Get() again, unguarded. This runs during teardown, where
    // the Application is normally still alive because OnDetach is driven by
    // the layer stack the Application owns.
    Application &app = Application::Get();
    auto *ctx = dynamic_cast<NVRHIContext *>(app.GetWindow().GetCurrentContext());
    if (ctx)
     // ctx is re-fetched rather than cached from InitBackend, which is correct
     // in principle -- the window and its context can have been recreated
     // since -- but means this teardown targets whatever context exists NOW,
     // not necessarily the one the Vulkan backend was initialised against.
     ctx->ShutdownVulkanImGuiResources();
   }
   break;

  // Silent. DEFECT: unlike InitBackend's default on line 435, an
  // unrecognised backend is torn down with no log at all, so a corrupt
  // m_CurrentBackend leaks whatever it named with no diagnostic.
  default:
   break;
  }

  // Shutdown platform bindings
  // The platform backend is torn down after the renderer backend, which
  // is the correct order: the renderer backend may still reference
  // GLFW-owned state.
  // Guarded by m_GlfwInitialized rather than by BackendPlatformUserData, so
  // it depends on this class's own bookkeeping staying in step with the
  // backend's. Every place that calls ImGui_ImplGlfw_Init without checking
  // its result sets the flag anyway, which is the one way to make the two
  // disagree.
  if (m_GlfwInitialized)
  {
   ImGui_ImplGlfw_Shutdown();
   m_GlfwInitialized = false;
  }
  // Reset last, after everything has been released, so a failure part
  // way through leaves m_CurrentBackend claiming a backend that is half
  // shut down.
  // Reset to None LAST, after both backends are down. If a shutdown step
  // returned early or threw, the field would still name a half-torn-down
  // backend and Begin would try to use it next frame.
  m_CurrentBackend = RendererAPI::API::None;
 }

 // Called when the window is recreated (device removal, fullscreen
 // toggle), where the native window handle changes and every backend
 // holding it is invalid.
 // The log line below is this function's only real contribution; the
 // teardown itself is ShutdownBackend's. It exists so the window-
 // recreation path reads as an intentional call at the call site rather
 // than reaching into ShutdownBackend and re-deriving its preconditions.
 void ImGuiLayer::ShutdownBackendForWindowRecreate()
 {
  WL_PROFILE_FUNCTION();
  // The guard ShutdownBackend applies internally, duplicated here only so
  // the log line is not emitted for an already-torn-down layer. Harmless,
  // but it means two places to change if the None sentinel ever moves.
  if (m_CurrentBackend == RendererAPI::API::None)
   return;
  // The only trace the recreation path leaves. Nothing here records
  // whether a replacement backend is coming, so a caller that recreates
  // the window and never follows up with SwitchBackend leaves the layer
  // at None and Begin() logging an error on every frame.
  WL_CORE_INFO("ImGui: Shutting down backend for window recreate ({0})", (int)m_CurrentBackend);
  // Pure delegation: the log line is the only thing this adds.
  ShutdownBackend();
 }

 // Records the request only -- the switch happens in PerformBackendSwitch
 // at the start of the next frame (line 648).
 // That deferral is necessary: swapping a renderer backend mid-frame
 // would invalidate the draw data already built.
 // The only public way to change backends; WindowsWindow calls it after
 // a device or fullscreen transition. It records intent and returns --
 // the teardown happens on the next Begin(), so a call made from inside
 // the draw pass cannot pull a GL context out from under a live frame.
 void ImGuiLayer::SwitchBackend(RendererAPI::API api)
 {
  WL_PROFILE_FUNCTION();

  // The same idempotence test InitBackend opens with, duplicated so an
  // already-current request does not even set the pending flag. Without
  // it, a redundant request would cost a full teardown and rebuild on the
  // next frame -- a real cost, since switching recompiles GPU shaders.
  if (api == m_CurrentBackend)
   return;

  // "(deferred)" because the switch has NOT happened yet: at this point
  // the layer is still running m_CurrentBackend and the old backend's
  // device objects are still live. Logging it as done would be the easy
  // mistake here.
  WL_CORE_INFO("ImGui: Backend switch requested from {0} to {1} (deferred)", (int)m_CurrentBackend, (int)api);

  // Mark switch as pending - will be performed at the start of next frame
  // Two members rather than one: the API to switch to, plus a flag
  // saying a switch is wanted. A flag is needed because the requested
  // API can legitimately equal the current one, and
  // "pending == current" would otherwise be indistinguishable from
  // "nothing pending".
  // Stored even when the value equals the current backend, because the
  // flag is what separates "switch to what is already running" from
  // "nothing pending". See the note on the two members in the header.
  m_PendingBackendSwitch = api;
  m_BackendSwitchPending = true;
 }

 // The single recovery path for all ~14 failure branches in
 // InitBackend. Every one of them calls this and then sets
 // m_CurrentBackend from its result.
 bool ImGuiLayer::FallbackToOpenGL()
 {
  // If there's no GL context, we can't fall back to OpenGL.
  // glGetString would return null and crash in ImGui_ImplOpenGL3_Init.
  // Reached most often when the failure that triggered the fallback also
  // took the GL context with it, which is the normal case: WindowsWindow
  // tears the OpenGL context down when switching away from OpenGL, so by
  // the time any NVRHI backend fails there is no context left to fall
  // back to and every one of the fourteen callers ends up at None.
  if (!HasValidGLContext())
  {
   WL_CORE_ERROR("ImGui: No OpenGL context available for fallback (context was destroyed)");
   // One of only two exits from this function, and both are logged. That is
   // the sole reason this function's failure modes are as traceable as they
   // are -- the callers themselves log nothing about the result.
   return false;
  }

  // The third unguarded Application::Get() in this file. On the fallback
  // path it runs while something has already gone wrong, which is the
  // worst possible moment for a static that may not exist yet.
  Application &app = Application::Get();
  GLFWwindow *window = static_cast<GLFWwindow *>(app.GetWindow().GetNativeWindow());
  // The null check InitBackend omits, repeated here. Its presence in one
  // function and absence in the other is the clearest evidence in this
  // file that the missing check is an oversight and not a decision: the
  // author knew the window could be null and wrote the test here.
  if (!window)
  {
   WL_CORE_ERROR("ImGui: No window available for OpenGL fallback");
   return false;
  }

  // A SECOND call to glGetString. HasValidGLContext() on line 523
  // already made this exact call and discarded the result; doing it again
  // to log it is redundant but harmless.
  // Recomputed rather than returned by HasValidGLContext, which performs
  // the identical call and throws the result away. Cheaper to fix by
  // giving HasValidGLContext an out-parameter than by querying GL twice
  // on a failure path.
  const char* glVersion = (const char*)glGetString(GL_VERSION);
  // DEFECT (format mismatch): this uses `{}` while every other WL_CORE_*
  // call in this file uses {0}/{1}. The two placeholders are not
  // interchangeable in fmt-style formatters, so with {} the argument may
  // be passed positionally -- which happens to be right here by luck,
  // since glVersion is the only argument. It is inconsistent with the
  // surrounding code and would break if a second argument were added.
  // The only {} placeholder in the file; every other WL_CORE_* call here
  // uses {0}. With a single argument the two spellings happen to agree,
  // so the log is correct today -- adding a second argument here is the
  // moment it stops being.
  WL_CORE_INFO("ImGui: Falling back to OpenGL backend (GL_VERSION: {})", glVersion);

  // Shutdown GLFW if it was initialized (e.g., from DX12 failure path)
  // The same three-line guard as the four other sites in InitBackend and
  // ShutdownBackend. Its purpose is to satisfy ImGui_ImplGlfw_Init's
  // "Already initialized a platform backend!" assert, so it is
  // load-bearing rather than merely tidy.
  if (m_GlfwInitialized)
  {
   ImGui_ImplGlfw_Shutdown();
   m_GlfwInitialized = false;
  }

  // Release D3D11 white SRV if we were using DX11
  // Released here as well as in ShutdownBackend, because this function is
  // reachable from a DX11 failure branch where End() already created the
  // SRV. The nullptr assignment below is what stops the second release.
  if (m_D3D11WhiteSRV)
  {
   m_D3D11WhiteSRV->Release();
   m_D3D11WhiteSRV = nullptr;
  }

  // Re-init GLFW for OpenGL
  // The true is install_callbacks.
  // DEFECT (return ignored): ImGui_ImplGlfw_InitForOpenGL returns bool
  // and its result is discarded here and at lines 104, 165, 272 and 370.
  // A failed GLFW-platform init still sets m_GlfwInitialized = true on the
  // next line, so ShutdownBackend will later call ImGui_ImplGlfw_Shutdown
  // for a backend that never initialised.
  // The return value is dropped, exactly as at the four other Init sites,
  // and the flag on the next line is set regardless -- so a failed
  // platform init leaves the layer believing GLFW is up, and
  // ShutdownBackend will later shut down a backend that never started.
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  m_GlfwInitialized = true;

  // Checked here, unlike the identical call in InitBackend's OpenGL case.
  // So the same operation is validated on the fallback path and ignored
  // on the direct path. The asymmetry matters because the fallback is the
  // path a user reaches when something has already gone wrong.
  if (!ImGui_ImplOpenGL3_Init("#version 410"))
  {
   WL_CORE_ERROR("ImGui: OpenGL3 init failed");
   // Note what is NOT undone before returning false: GLFW was initialised
   // two statements ago and stays initialised, and m_GlfwInitialized is
   // left true. A caller treating false as "nothing happened" would be
   // wrong -- and the callers here do exactly that.
   return false;
  }

  // True means "the GL backend is up", not "a backend exists":
  // assigning m_CurrentBackend is the caller's job. That is precisely
  // why all fourteen call sites repeat the same if/else pair afterwards.
  return true;
 }

 // The deferred half of SwitchBackend.
 // The only writer of m_CurrentBackend outside InitBackend and
 // ShutdownBackend, and it writes it only indirectly by calling those
 // two. Every state change therefore funnels through here, which is what
 // makes a single deferred-switch point possible at all.
 void ImGuiLayer::PerformBackendSwitch()
 {
  WL_PROFILE_FUNCTION();

  // Tested twice per switch: once in Begin and once here. The redundancy
  // is what makes this function safe to call directly, at the cost of one
  // branch per frame.
  if (!m_BackendSwitchPending)
   return;

  // Read before the flag is cleared and before ShutdownBackend resets
  // m_CurrentBackend -- that ordering is what keeps a request for the
  // CURRENT backend from being mistaken for a no-op.
  RendererAPI::API api = m_PendingBackendSwitch;
  // Cleared BEFORE the work, deliberately: if InitBackend on line 593
  // throws or the backend switch fails, this is not retried forever.
  // The cost is that a failed switch is silently lost -- m_CurrentBackend
  // ends up None and Begin() skips frames.
  m_BackendSwitchPending = false;

  if (api == m_CurrentBackend)
   return;

  // "Performing", not "performed": InitBackend below may still fall back
  // to OpenGL, so the requested API is not necessarily what is running by
  // the time this function returns.
  WL_CORE_INFO("ImGui: Performing backend switch to {0}", (int)api);

  // Shutdown current backend fully (handles None check internally)
  ShutdownBackend();

  // Clear font atlas for new backend and re-add default font
  // Fonts must be rebuilt after Clear() - add default font back
  // Fetched before the atlas is cleared so the reference stays valid;
  // the IO object outlives the atlas.
  // Used twice below, the second time after InitBackend has run -- so the
  // reference has to survive the backend teardown inside ShutdownBackend.
  // It does: ImGuiIO lives in the context, which is not destroyed until
  // OnDetach, not by a backend shutdown.
  ImGuiIO &io = ImGui::GetIO();
  // DEFECT (loses authored fonts): Clear() discards the font ATLAS, not
  // just its GPU texture. Any custom font added in OnAttach or by a panel
  // is gone, and AddFontDefault on the next line replaces it with the
  // built-in. A backend switch therefore silently reverts the application
  // to the default font.
  // The correct sequence is to release only the GPU-side TexID and let
  // the new backend rebuild it.
  io.Fonts->Clear();
  io.Fonts->AddFontDefault();
  // TexID will be recreated by new backend's CreateDeviceObjects / CreateFontsTexture

  // Initialize new backend
  InitBackend(api);

  // Ensure style is correct after switch
  // Re-applies the two style values OnAttach sets, because InitBackend
  // touches no style at all. The duplication exists so a switch away and
  // back lands on the same appearance without a full re-attach.
  if (m_CurrentBackend == RendererAPI::API::OpenGL && (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
  {
   ImGuiStyle &style = ImGui::GetStyle();
   // Identical to the two assignments in OnAttach, and keyed on the same
   // never-set flag, so this block cannot run today either. If
   // ViewportsEnable is ever enabled, both sites become live and must stay
   // in step -- which is the whole argument for hoisting them into one
   // helper.
   style.WindowRounding = 0.0f;
   style.Colors[ImGuiCol_WindowBg].w = 1.0f;
  }
 }

 // Layer teardown hook. Called in the reverse order of OnAttach.
 void ImGuiLayer::OnDetach()
 {
  WL_PROFILE_FUNCTION();

  // Backend before context, which is required rather than stylistic:
  // every ImGui_Impl*_Shutdown asserts that its own backend data is still
  // registered in the IO, and DestroyContext would clear that first.
  ShutdownBackend();
  // Destroys the context created in OnAttach.
  // DEFECT (order): ShutdownBackend() runs first, which is correct, but
  // nothing destroys the multi-viewport platform windows if they were
  // ever created -- no ImGui::DestroyPlatformWindows() call.
  // That is currently harmless because ViewportsEnable is never set
  // (see the note on line 56).
  // io.BackendPlatformUserData and BackendRendererUserData are cleared by
  // the ShutdownBackend call above, not by this. So any backend that was
  // never shut down leaves a dangling pointer inside a context that no
  // longer exists -- harmless in practice, because nothing reads it
  // afterwards, but it is why a second OnAttach on the same layer would
  // trip ImGui_ImplGlfw_Init's "already initialized" assert.
  ImGui::DestroyContext();
 }

 // The only method here that is not a Layer lifecycle hook: it runs for
 // every engine event, once per layer, every time. Note the absence of
 // WL_PROFILE_FUNCTION, unlike Begin, End and the four backend
 // functions -- presumably because per-event profiling would dominate
 // the per-frame scopes it shares a trace with.
 void ImGuiLayer::OnEvent(Event &event)
 {
  // The master switch for input capture. It gates ONLY the capture block
  // below; the resize dispatcher under it runs unconditionally, because
  // keeping the backend's swapchain-sized resources correct is not
  // optional when input blocking is off.
  if (m_BlockEvents)
  {
   // Fetched inside the guard so a layer with blocking disabled does not
   // touch the ImGui context at all. That is a genuine saving on the hot
   // path, since OnEvent runs once per event and events are pumped every
   // frame whether or not anything was drawn.
   ImGuiIO &io = ImGui::GetIO();
   // Swallows mouse events when ImGui wants them, so clicking a UI panel
   // does not also click through to the game.
   // DEFECT (bitwise &, not &&): io.WantCaptureMouse is a bool, so &
   // between bools yields an int. It happens to give the same truth table as
   // && here (both operands are 0/1 and neither short-circuits, and neither
   // side has side effects), so it is not a live bug -- but it is the kind
   // of expression that becomes one when a side gains a side effect.
   // ||= on line 618 would likewise be simpler, and |=/&= together read as
   // an attempt to force evaluation that the compiler already does.
   // DEFECT (the only handled-flag write in the engine that works): Event
   // carries TWO flags for one piece of state -- the public Handled read
   // here, and a protected m_Handled. EventDispatcher::Dispatch writes
   // only the protected one, and its one public-flag write sits behind a
   // duplicated, unreachable type check. Application::OnEvent tests the
   // PUBLIC flag when deciding whether to stop walking the layer stack.
   // So these two lines are the entire input-consumption mechanism that
   // has any effect; every handler returning true from a Dispatch has
   // none.
   event.Handled |= event.IsInCategory(EventCategoryMouse) & io.WantCaptureMouse;
   // Keyboard counterpart. WantCaptureKeyboard is set only while a text
   // field or a navigation-active widget wants keys, so this is the line
   // that stops a keystroke typed into the UI from also reaching gameplay
   // code further down the layer stack.
   event.Handled |= event.IsInCategory(EventCategoryKeyboard) & io.WantCaptureKeyboard;
  }

  // Handle window resize for Vulkan/DX12 to recreate ImGui resources
  // A stack-local dispatcher wrapping this single event by reference, so
  // the lambda on line 623 can run synchronously.
  // Constructing one per event is slightly wasteful, but events are
  // dispatched once per event per layer, not in a loop.
  // Constructed even when the event is not a resize. The constructor only
  // stores a reference, so the cost is two words; the real cost is that
  // Dispatch below has to test the type itself before anything runs.
  EventDispatcher dispatcher(event);
  // DEFECT (unused parameter `e`): the lambda ignores the event entirely
  // and reads the size from ctx instead (line 1022 on the Vulkan path).
  // More significantly, for Vulkan the handler does NOT re-read the new
  // render-area size -- it only calls SetMinImageCount(2), so a resized
  // window keeps rendering into the old framebuffer extent until
  // NVRHIContext rebuilds it.
  // Typed dispatch, so the lambda runs only for a WindowResizeEvent and
  // no downcast is needed to reach the resize payload. Its return value
  // becomes the event's handled flag -- see the DEFECT two blocks above.
  dispatcher.Dispatch<WindowResizeEvent>([this](WindowResizeEvent &e)
         {
   // First of three else-if backend branches. There is deliberately no
   // OpenGL branch: the GL backend holds no swapchain-sized resource of its
   // own, so a GL resize legitimately falls through all three and does
   // nothing.
   if (m_CurrentBackend == RendererAPI::API::NVRHI_Vulkan)
   {
    // Notify ImGui Vulkan about image count change
    // Hardcoded 2, while InitBackend on line 382 asks the context for its
    // actual image count. DEFECT (inconsistency): if the swapchain has more
    // than two images, this reduces what ImGui thinks it has, and the
    // mismatch with the init value means the backend may have to re-create
    // its resources on the next NewFrame.
    // DEFECT (fires an assert in debug builds): the vendored backend
    // implements SetMinImageCount as an early return when the value is
    // unchanged, followed immediately by IM_ASSERT(0) -- "FIXME-VIEWPORT:
    // Unsupported. Need to recreate all swap chains!" -- when it differs.
    // So if InitBackend gave the backend a MinImageCount other than 2, the
    // first window resize after that trips the assert. In a release build
    // the assert vanishes and the function instead vkDeviceWaitIdles and
    // destroys every viewport render buffer.
    ImGui_ImplVulkan_SetMinImageCount(2);
   }
   else if (m_CurrentBackend == RendererAPI::API::NVRHI_DX12)
   {
    // Invalidate then recreate: the documented way to force a font texture
    // rebuild after a swapchain change. DEFECT (return ignored):
    // CreateDeviceObjects returns bool and its result is discarded, so a
    // failed rebuild leaves Fonts->TexID pointing at a destroyed SRV and
    // the next frame's draw dereferences it. The DX11 backend retries from
    // NewFrame; this path has no retry at all.
    ImGui_ImplDX12_InvalidateDeviceObjects();
    // Unchecked, and this is the call that actually rebuilds. Note the
    // backend's frame-render-buffer array is sized by num_frames_in_flight
    // and is NOT touched by InvalidateDeviceObjects, so what is recreated
    // here is the font texture alone -- which is all a resize needs.
    ImGui_ImplDX12_CreateDeviceObjects();
   }
   else if (m_CurrentBackend == RendererAPI::API::NVRHI_DX11)
   {
    // The DX11 equivalent. The missing check is less harmful here, because
    // ImGui_ImplDX11_NewFrame calls CreateDeviceObjects itself whenever
    // pFontSampler is null -- so a failure is retried every frame instead
    // of left dangling. The DX12 case above has no such retry.
    ImGui_ImplDX11_InvalidateDeviceObjects();
    // Same unchecked result, same per-frame retry, so the two branches
    // differ in consequence only because the backend underneath them
    // differs.
    ImGui_ImplDX11_CreateDeviceObjects();
   }
   // The handler returns false, so the event is NOT marked handled --
   // other layers still see the resize. Correct: a resize is a global
   // concern, not something ImGui should consume.
   return false; });
 }

 // Frame start. Pairs with End(); m_FrameStarted is the flag that keeps
 // the two balanced when a backend fails mid-frame.
 // Not a Layer hook: Application calls it directly, wrapping the layer
 // stack's OnImGuiRender loop. That is what makes the Begin/End pairing
 // a caller obligation, and why m_FrameStarted exists -- nothing in the
 // layer stack enforces it.
 void ImGuiLayer::Begin()
 {
  WL_PROFILE_FUNCTION();

  // Perform deferred backend switch if pending
  // The deferred switch is executed HERE rather than in SwitchBackend,
  // so it lands at a frame boundary.
  // Note PerformBackendSwitch itself re-checks the flag (line 571), so
  // this outer test is redundant.
  // The braces around a single statement are inconsistent with the two
  // braceless early-return guards further down. Cosmetic only, but it is
  // the kind of drift that makes a reader second-guess whether something
  // was omitted inside.
  if (m_BackendSwitchPending)
  {
   PerformBackendSwitch();
  }

  // Validate current backend
  // The second gate of the same frame: a switch that failed above lands
  // here immediately rather than on the next frame, so the failure is
  // reported once per frame from here on.
  if (m_CurrentBackend == RendererAPI::API::None)
  {
   // DEFECT (per-frame error): this fires EVERY frame once the backend is
   // None. After a failed switch or a device removal, the log fills with
   // identical errors at frame rate, which is a real operational problem
   // for diagnosis because it drowns everything else.
   // A once-only warning would be the right behaviour.
   WL_CORE_ERROR("ImGui: No backend initialized, skipping frame");
   m_FrameStarted = false;
   return;
  }

  // Call the appropriate NewFrame function based on current backend
  // Four cases plus a default, each a one-line NewFrame call. The switch
  // exists only to map an enum onto a function name; a function-pointer
  // table keyed by API, built once at init, would remove it along with
  // the matching switch in End.
  switch (m_CurrentBackend)
  {
  case RendererAPI::API::OpenGL:
   // The backend's own NewFrame does almost nothing: the vendored
   // ImGui_ImplOpenGL3_NewFrame only calls CreateDeviceObjects when
   // bd->ShaderHandle is null. All the real per-frame work is in
   // ImGui_ImplGlfw_NewFrame two statements below.
   ImGui_ImplOpenGL3_NewFrame();
   break;

  case RendererAPI::API::NVRHI_DX11:
   // The DX11 backend's NewFrame does exactly one thing -- if
   // pFontSampler is null, call CreateDeviceObjects. That is the retry
   // loop the DX11 branch of InitBackend relies on when it declines to
   // validate the font atlas explicitly.
   ImGui_ImplDX11_NewFrame();
   break;

  case RendererAPI::API::NVRHI_DX12:
   // Also a no-op beyond the font texture, but with NO lazy-creation
   // fallback. That asymmetry is why the DX12 branch of InitBackend
   // checks CreateDeviceObjects explicitly and DX11 does not.
   ImGui_ImplDX12_NewFrame();
   break;

  case RendererAPI::API::NVRHI_Vulkan:
   // Literally an empty function in the vendored backend: an assert on the
   // backend data, then IM_UNUSED(bd). Every bit of Vulkan per-frame state
   // is established in End instead, which is why this file's Vulkan branch
   // is three times the length of this one.
   ImGui_ImplVulkan_NewFrame();
   break;

  default:
   // Unreachable in practice. m_CurrentBackend is only ever assigned one of
   // the five enumerators InitBackend knows about, so this default exists
   // to satisfy the compiler rather than to handle a real state. It is also
   // the wrong fallback: calling ImGui_ImplOpenGL3_NewFrame with no GL
   // backend initialised asserts in the vendored backend.
   WL_CORE_WARN("ImGui: Unknown backend {0}, using OpenGL", (int)m_CurrentBackend);
   ImGui_ImplOpenGL3_NewFrame();
   break;
  }

  // Platform frame, then core frame, then ImGuizmo. That ORDER is
  // required by both backends: ImGui_ImplOpenGL3_NewFrame reads the
  // display size and input state that the GLFW backend produced.
  // This is where io.DisplaySize, io.DisplayFramebufferScale and
  // io.DeltaTime actually come from: the GLFW backend queries the window
  // size and the framebuffer size and derives the scale from the ratio.
  // End() then overwrites DisplaySize with the engine's own size -- see
  // the inconsistency noted there.
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
  // Must come after ImGui::NewFrame because ImGuizmo reads the IO state
  // (delta time, mouse position) that ImGui just updated.
  // The only use of ImGuizmo in this file. It records the current time so
  // gizmo input can be interpolated, which is why it has to run once per
  // frame AND after the mouse state is current -- hence after both
  // NewFrame calls rather than before them.
  ImGuizmo::BeginFrame();
  // Set last, after ImGuizmo::BeginFrame, so any early return above
  // leaves the flag false and End() skips cleanly. Because BeginFrame
  // cannot fail, setting it before would be equivalent -- but last is
  // the order that survives someone adding a fallible call here.
  m_FrameStarted = true;
 }

 // Frame end: render the draw data through the active backend.
 // This is the largest function in the file, with per-backend branches
 // ranging from 3 lines (OpenGL) to 175 (DX11).
 // Frame end: closes the frame Begin opened and routes the finished draw
 // data to whichever backend is live. Nothing here decides layout -- that
 // happened between Begin and this call, in the layer stack's own pass.
 //
 // Three early exits before any drawing: no frame was started, no backend
 // is live, or an unknown backend fell through. All three skip the switch
 // entirely, so the viewport tail at the bottom is skipped with them.
 void ImGuiLayer::End()
 {
  WL_PROFILE_FUNCTION();

  // Skip rendering if no frame was started (e.g., backend switch failed)
  // The Begin/End balance guard set by Begin on line 657.
  // This is the pairing that makes the m_FrameStarted flag worth its
  // cost: without it, an early return in Begin would leave End calling
  // ImGui_ImplOpenGL3_RenderDrawData on a frame that never started.
  if (!m_FrameStarted)
  {
   return;
  }
  // Cleared before the None check below, so the flag is already false by
  // the time that early return runs. Setting it first rather than last is
  // what makes the two exits at the bottom of this function symmetric.
  m_FrameStarted = false;

  // Validate current backend
  if (m_CurrentBackend == RendererAPI::API::None)
  {
   return; // Skip rendering if no backend
  }

  // Fetched unconditionally and used by the viewport block at the bottom
  // and, indirectly, by every branch. Fetching it here rather than inside
  // the OpenGL branch is what lets the multi-viewport tail run regardless
  // of which backend drew the frame.
  ImGuiIO &io = ImGui::GetIO();
  // The fourth unguarded Application::Get(). In End it runs once per
  // frame on every frame that gets this far, so unlike the three in the
  // init path it is on the hot path rather than the cold one.
  Application &app = Application::Get();
  // Set from the engine window, not from the backend.
  // Note this happens AFTER ImGui::Render() would normally be called --
  // it is set on line 711, before Render on line 714, so the ordering is
  // correct.
  // DEFECT (hardcoded here, ignored for Vulkan): the Vulkan path uses
  // ctx->GetWidth()/GetHeight() on line 1022 instead, so two different
  // size sources are in play. If they disagree, the viewport windows
  // render at one size while the draw data was laid out for another.
  io.DisplaySize = ImVec2((float)app.GetWindow().GetWidth(), (float)app.GetWindow().GetHeight());

  // Rendering
  ImGui::Render();

  // Call the appropriate RenderDrawData function based on current backend
  switch (m_CurrentBackend)
  {
  case RendererAPI::API::OpenGL:
   // The simplest branch: the GL backend sets its own state and issues the
   // draw. Every other case here does more work because those APIs
   // require explicit render-target binding.
   ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
   break;

  // By far the longest branch: the DX11 backend will not bind a render
  // target for itself, so this layer has to obtain the backbuffer, build
  // an RTV over it, bind that, sanitise every texture handle, and unbind
  // it again -- all of which the other three backends do internally.
  case RendererAPI::API::NVRHI_DX11:
  {
   // The same dynamic_cast as in InitBackend, and the same RTTI
   // dependency. Note the asymmetry: the null case here only breaks out of
   // the switch, whereas in InitBackend the same null triggers a fallback
   // to OpenGL. A DX11 context vanishing mid-frame degrades to no UI
   // rather than to GL UI.
   auto *ctx = dynamic_cast<NVRHIContext *>(app.GetWindow().GetCurrentContext());
   if (!ctx)
   {
    WL_CORE_ERROR("ImGui: DX11 context is null");
    break;
   }
   // static_cast for the same reason as in InitBackend: the accessors
   // return void* and the branch has already proved the context is a DX11
   // NVRHI context. d3dCtx is named differently from the `context` local
   // in InitBackend for no reason -- same type, same lifetime.
   ID3D11Device *device = static_cast<ID3D11Device *>(ctx->GetD3D11Device());
   ID3D11DeviceContext *d3dCtx = static_cast<ID3D11DeviceContext *>(ctx->GetD3D11Context());
   if (!device || !d3dCtx)
   {
    // WARN, not ERROR, unlike the ctx-null case above. The distinction
    // appears to mean "the backend is fine, the device is not ready this
    // frame" versus "we do not know what we are talking to", but it is
    // not written down anywhere, so it reads as inconsistency.
    WL_CORE_WARN("ImGui: DX11 device/context not ready, skipping render");
    break;
   }

   // Device objects are created lazily by ImGui_ImplDX11_NewFrame().
   // Do NOT call CreateDeviceObjects here: it invalidates the font atlas
   // (Fonts->SetTexID(0)) and recreates the font SRV AFTER the draw data
   // was built — every command then holds a released SRV (use-after-free
   // in DrawIndexed) and no longer matches the current TexID.
   // If D3DCompile failed at init (missing d3dcompiler_47.dll), NewFrame
   // retries creation every frame until it succeeds.

   ImDrawData *draw_data = ImGui::GetDrawData();
   // Five-way guard before any DX11 work.
   // DEFECT: cmdList->CmdBuffer.Size is NOT checked, only the aggregate
   // counts. A draw list with a non-empty buffer but zero total vertices
   // would pass -- though that combination is not reachable in practice.
   if (!draw_data || draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f || draw_data->CmdListsCount == 0 || draw_data->TotalVtxCount == 0)
    break;

   // Font atlas must have a valid TexID before drawing. Right after a
   // backend switch the first NewFrame recreates it, so draw nothing
   // this frame rather than sampling a released/absent texture.
   // ImGui stores the backend texture handle in Fonts->TexID as a void*,
   // so the DX11 backend reinterprets it as a COM SRV pointer.
   // That reinterpretation is why the sanitizer loop below exists: under
   // the GL backend the same field holds a small integer GL name.
   ID3D11ShaderResourceView *fontSrv = (ID3D11ShaderResourceView *)ImGui::GetIO().Fonts->TexID;
   if (!fontSrv)
    break;

   // Device removed / TDR check
   HRESULT removedReason = device->GetDeviceRemovedReason();
   if (FAILED(removedReason))
   {
    // A device-removed HRESULT here means End is skipping every frame from
    // now on, and it will log once per frame doing it -- the same log flood
    // the None guard in Begin produces. Application's frame loop has its
    // own device-lost recovery, but it checks NVRHIContext::IsDeviceLost,
    // which this does not feed.
    WL_CORE_ERROR("ImGui: DX11 device removed (HRESULT: {0:X}), skipping render", (uint32_t)removedReason);
    break;
   }

   // DX11 backend does NOT set RTV - caller must bind it.
   // NVRHI may leave the immediate context without a guaranteed RTV after ExecuteNVRHICommandList().
   // Explicitly bind current backbuffer RTV to avoid driver crash inside DrawIndexed.
   void *swapPtr = ctx->GetDXGISwapChain();
   if (!swapPtr)
   {
    // The swapchain is fetched through a void* accessor that can return null
    // while a device is being reset. Skipping is the right call: there is
    // no backbuffer to draw into, and continuing would mean drawing into
    // whatever the previous bind left bound.
    WL_CORE_WARN("ImGui: DX11 swapchain is null, skipping render");
    break;
   }
   // static_cast, not QueryInterface. The accessor returns IDXGISwapChain*
   // semantics via void*, and the context is trusted to have been created
   // with BufferCount > 1 -- GetCurrentBackBufferIndex below is only
   // meaningful on a flip-model swapchain. If it were not, the cast would
   // succeed and GetBuffer would return DXGI_ERROR_INVALID_CALL.
   IDXGISwapChain3 *swapChain = static_cast<IDXGISwapChain3 *>(swapPtr);
   // The index into the swapchain's buffer array. The DX11 backend then
   // draws one frame per Present; nothing here waits for this buffer to be
   // free, so correctness depends on the engine's own frame pacing rather
   // than on anything in this file.
   uint32_t idx = ctx->GetCurrentBackBufferIndex();
   // Initialised to null so the FAILED(hr) test below cannot be confused by
   // an uninitialised read. GetBuffer on the success path overwrites it
   // with a reference this function owns and must release -- which it does
   // on three separate paths.
   ID3D11Texture2D *backBuffer = nullptr;
   // GetBuffer ADDS a reference, so backBuffer->Release() on lines 806
   // and 811 is required and is present in both paths.
   // IID_PPV_ARGS both selects the right IID for the interface version and
   // supplies the void** that COM wants -- the modern alternative to a
   // separate QueryInterface plus cast.
   HRESULT hr = swapChain->GetBuffer(idx, IID_PPV_ARGS(&backBuffer));
   if (FAILED(hr) || !backBuffer)
   {
    // Both conditions logged together: a failed HRESULT and a null out-param
    // are indistinguishable to a caller, and D3D can legitimately leave the
    // out-param untouched on failure. Backbuffer index is in the message,
    // which is what makes this the most useful of the DX11 error paths.
    WL_CORE_ERROR("ImGui: Failed to get DX11 backbuffer {0} (HRESULT: {1:X})", idx, (uint32_t)hr);
    break;
   }
   // The RTV lives to the end of the switch case and is released at the
   // bottom. It is declared out here rather than inside the inner scope so
   // that OMSetRenderTargets below can see it after the scope closes.
   ID3D11RenderTargetView *rtv = nullptr;
   {
    // Not zero-initialised before GetDesc, unlike every other descriptor in
    // this file. GetDesc overwrites the whole struct, so this is safe -- but
    // it is the one place in the file that relies on that rather than on
    // the explicit `= {}` pattern used elsewhere.
    D3D11_TEXTURE2D_DESC bbDesc;
    backBuffer->GetDesc(&bbDesc);

    // rtTexture is the texture the RTV will be made from, which is either
    // the backbuffer itself or the copy made below. The backbuffer reference
    // is held by rtTexture from here on, so the release at the bottom of
    // the block is what balances GetBuffer.
    ID3D11Texture2D *rtTexture = backBuffer;
    // Starts as whatever the backbuffer actually is and is only rewritten
    // in the two branches below. That is the right default -- the typeless
    // mapping in the else branch preserves B8G8R8A8 and R10G10B10A2, which a
    // hardcoded R8G8B8A8 would destroy.
    DXGI_FORMAT rtvFormat = bbDesc.Format;
    // The decision this whole block exists for: can the swapchain back
    // buffer be used as a render target directly?
    // DXGI_USAGE_RENDER_TARGET_OUTPUT should guarantee it, so this is a
    // defensive check for a swapchain created with a non-default usage.
    bool needCopy = (bbDesc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0;
    if (needCopy)
    {
     // Backbuffer not render-target bindable: copy into a compatible
     // texture and draw against that instead of the raw buffer.
     D3D11_TEXTURE2D_DESC copyDesc = bbDesc;
     copyDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
     copyDesc.MiscFlags = 0;
     // DEFECT (format assumption): the copy destination is hardcoded to
     // R8G8B8A8_UNORM regardless of what the backbuffer actually is. If the
     // swapchain is B8G8R8A8, CopyResource between different formats is
     // undefined behaviour -- CopySubresource would at least fail loudly.
     // This is the same R8G8B8A8 assumption InitBackend makes on line 275.
     copyDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
     ID3D11Texture2D *copyTex = nullptr;
     if (SUCCEEDED(device->CreateTexture2D(&copyDesc, nullptr, &copyTex)) && copyTex)
     {
      // Full-resource copy, so the copy texture must be the same size and
      // mip count -- which it is, since copyDesc is derived from bbDesc on
      // line 796.
      d3dCtx->CopyResource(copyTex, backBuffer);
      rtTexture = copyTex;
      rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
      // The backbuffer reference is dropped here because rtTexture now points
      // at copyTex instead. One of the two paths in this block releases it and
      // the other does not, which is the single most error-prone thing about
      // this function: every exit must release exactly the right one of the
      // two textures it is holding.
      backBuffer->Release();
     }
     else
     {
      // The copy failed, so there is no valid render target this frame. Note
      // this can only be reached when needCopy was true, i.e. when the
      // backbuffer was not render-target bindable at all -- a configuration
      // no normal swapchain uses, which is why this path is rarely exercised.
      WL_CORE_ERROR("ImGui: Failed to create DX11 RT copy texture, skipping render");
      backBuffer->Release();
      break;
     }
    }
    else
    {
     // A typeless resource cannot be viewed with its own typeless format —
     // map to the standard typed variant so the RTV creation cannot fail.
     // The fast path: the backbuffer was directly render-target bindable,// so only the typeless-format mapping below is needed (the copy path
     // above handles the other case).
     // Maps the four typeless formats to their typed variants.
     // DEFECT (incomplete): only these four cases are handled. A backbuffer
     // in any other typeless format falls through the default on line 825 and
     // CreateRenderTargetView on line 833 then fails -- handled, but the error
     // message on line 837 does not say the format was the problem.
     switch (rtvFormat)
     {
     case DXGI_FORMAT_R8G8B8A8_TYPELESS:    rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;    break;
     // One line per format rather than a table or a helper, so adding a
     // format means adding a line here. All three map to the same
     // bit-depth-typed variant the typeless form implies; the mapping is
     // total only over the formats a swapchain plausibly uses.
     case DXGI_FORMAT_B8G8R8A8_TYPELESS:    rtvFormat = DXGI_FORMAT_B8G8R8A8_UNORM;    break;
     case DXGI_FORMAT_R10G10B10A2_TYPELESS: rtvFormat = DXGI_FORMAT_R10G10B10A2_UNORM; break;
     case DXGI_FORMAT_R16G16B16A16_TYPELESS: rtvFormat = DXGI_FORMAT_R16G16B16A16_UNORM; break;
     // A typeless format with no case here falls through unchanged, and
     // CreateRenderTargetView below then fails on it. The failure IS handled
     // -- the error is just misleading, naming a generic HRESULT rather than
     // the format that caused it.
     default: break;
     }
    }

    // Value-initialised, so ArraySize, Texture2D.PlaneSlice and the rest
    // are zero. Needed here for the same reason as elsewhere: a
    // D3D11_RENDER_TARGET_VIEW_DESC read from stack garbage is a crash in
    // the driver, not a validation error.
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = rtvFormat;
    // TEXTURE2D, never TEXTURE2DMS. The render target is a swapchain
    // backbuffer or a copy of one, both of which are single-sampled; the
    // white SRV below is created with SampleDesc.Count = 1 for the same
    // reason.
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice = 0;
    // An explicit description is required whenever the resource format is
    // typeless, which is exactly the case the switch above exists to fix up.
    // For a non-typeless format this could have been nullptr, at the cost of
    // accepting the resource's own format rather than a computed one.
    HRESULT rtvHr = device->CreateRenderTargetView(rtTexture, &rtvDesc, &rtv);
    // Released before the HRESULT is tested, so the texture does not outlive
    // the scope that created the RTV over it. An RTV holds its own reference
    // to the resource, so releasing here is safe on the success path too.
    rtTexture->Release();
    if (FAILED(rtvHr) || !rtv)
    {
     WL_CORE_ERROR("ImGui: Failed to create DX11 RTV (HRESULT: {0:X})", (uint32_t)rtvHr);
     break;
    }
   }

   // (Font SRV null-check happens before RTV creation above.)

   // Count 1, one RTV, null depth stencil. The comment above explains why
   // this must be explicit: the DX11 ImGui backend does not set it and
   // NVRHI may leave the context without a guaranteed RTV.
   d3dCtx->OMSetRenderTargets(1, &rtv, nullptr);

   // Sanitize ImTextureIDs for the DX11 backend: OpenGL texture IDs are
   // small integers (GL names < 65536) that the DX11 backend would
   // dereference as bogus COM pointers and fault on DrawIndexed. Real
   // D3D11 SRV pointers (passed by NVRHI-mode panels via native SRV
   // accessors) are large addresses and pass through untouched.
   // Unknown small/odd IDs get the neutral 1x1 white SRV.
   if (!m_D3D11WhiteSRV)
   {
    D3D11_TEXTURE2D_DESC wDesc = {};
    // The 1x1 descriptor, field by field. Nothing here is defaulted by the
    // D3D11 API: an unset Width or Height in a texture description is a
    // creation failure, not a zero-sized texture.
    wDesc.Width = 1;
    wDesc.Height = 1;
    wDesc.MipLevels = 1;
    wDesc.ArraySize = 1;
    // R8G8B8A8 to match the copy path's assumption and the DX12 init's.
    // All-white so a substituted texture shows as white rather than as
    // undefined content, which is what makes the sanitiser's failure mode
    // a visible blank rather than a visible artefact.
    wDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    wDesc.SampleDesc.Count = 1;
    // IMMUTABLE, which permits the initial-data pointer below and forbids
    // any later write or CPU read. Combined with D3D11_USAGE_IMMUTABLE this
    // is the cheapest possible way to hold one constant texel.
    wDesc.Usage = D3D11_USAGE_IMMUTABLE;
    wDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // The one texel, as a packed RGBA value in the byte order D3D expects
    // for R8G8B8A8. Every component is 0xFF, so alpha is 1.0 and the pixel
    // is opaque white -- the neutral substitute for a missing texture.
    const uint32_t white = 0xFFFFFFFFu;
    // pSysMem = &white, SysmemPitch = 4 bytes (one RGBA texel),
    // SysmemSlicePitch = 0.
    // DEFECT (stricter than C requires): the first member is a
    // pointer to a local that must outlive CreateTexture2D. It does --
    // the call on line 866 is before this scope ends -- but taking the
    // address of a stack local for a COM call is the kind of thing that
    // becomes a dangling pointer when someone hoists the call.
    D3D11_SUBRESOURCE_DATA initData = { &white, 4, 0 };
    ID3D11Texture2D *wTex = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&wDesc, &initData, &wTex)) && wTex)
    // Created at most once and cached in the member. The SRV is released
    // in ShutdownBackend's DX11 case and in FallbackToOpenGL, so it
    // survives across frames and across backend switches within DX11 --
    // which is why the creation test is a plain null check rather than a
    // per-frame call.
    {
     // nullptr description means "default SRV for this resource".
     // DEFECT (return ignored): if this fails, m_D3D11WhiteSRV is left
     // undefined by CreateShaderResourceView on some failure paths rather
     // than guaranteed null, and wTex is released on the next line leaving
     // m_D3D11WhiteSRV possibly non-null and dangling -- which the sanitizer
     // on line 874 would then substitute into every draw command.
     device->CreateShaderResourceView(wTex, nullptr, &m_D3D11WhiteSRV);
     wTex->Release();
    }
   }
   {
    // Read once and reused for both the substitution target and the
    // identity comparison below, so the loop cannot disagree with itself
    // about which handle is the font atlas. Both are void* because ImGui
    // stores the backend texture handle untyped -- an SRV pointer under
    // DX11, a small integer GL name under OpenGL.
    void *fontTexId = ImGui::GetIO().Fonts->TexID;
    // Falls back to the raw font handle when the white SRV does not exist,
    // which happens only if CreateShaderResourceView failed on the first
    // frame. In that case the sanitiser becomes a no-op for small IDs -- it
    // substitutes the font texture itself -- so the loop still runs and
    // still does nothing useful.
    void *safeTexId = m_D3D11WhiteSRV ? (void *)m_D3D11WhiteSRV : fontTexId;
    // Walks every command list, then every command, rewriting TextureId in
    // place -- it mutates the ImDrawCmd structs, which is why this runs
    // before RenderDrawData on line 892.
    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
     ImDrawList *cmdList = draw_data->CmdLists[n];
     for (int i = 0; i < cmdList->CmdBuffer.Size; i++)
     {
      // Non-const, because the loop below rewrites TextureId in place. These
      // are the draw commands ImGui produced during the frame, not copies --
      // which is why sanitising here is safe only because it happens before
      // the backend reads them, and unsafe the moment that order changes.
      ImDrawCmd &cmd = cmdList->CmdBuffer[i];
      if (cmd.UserCallback != nullptr)
       continue;
      // A callback command carries no texture, so substituting one would be
      // meaningless. It is skipped rather than rewritten so ImGui's own
      // ResetRenderState callback still reaches the backend intact.
      // The numeric view, used only by the threshold test two lines below.
      // GetTexID returns the same value already typed; the reinterpret is
      // what makes an address comparable against a magnitude rather than
      // against another pointer.
      uintptr_t id = reinterpret_cast<uintptr_t>(cmd.GetTexID());
      // GL texture names and null pointers are invalid as COM
      // pointers — substitute. Anything pointer-shaped passes.
      // The heuristic: anything below 65536 that is not the font texture is
      // treated as a GL name and replaced.
      // DEFECT (heuristic on an address space): 0x10000 is chosen because
      // GL names are small, but a DX11 SRV allocated in the low 64 KB of the
      // address space would be misidentified and replaced by white. That is
      // possible in principle -- and on a 32-bit process it is likely, since
      // the low address range is heavily used.
      if (cmd.GetTexID() != fontTexId && id < 0x10000)
       cmd.TextureId = (ImTextureID)safeTexId;
     }
    }
   }

   // Uses the local draw_data from line 747 rather than a second
   // ImGui::GetDrawData() call -- consistent with the other branches,
   // which do call GetDrawData() inline.
   ImGui_ImplDX11_RenderDrawData(draw_data);

   // Release transient RTV after draw.
   // Unbinding the transient RTV after the draw.
   // Good hygiene: leaving it bound would let a later DrawIndexed by the
   // engine render into a released RTV.
   ID3D11RenderTargetView *nullRTV = nullptr;
   d3dCtx->OMSetRenderTargets(1, &nullRTV, nullptr);
   // The RTV is released on the last line of the DX11 branch. Every exit
   // above that creates one also releases it, so the count is balanced --
   // but the pairing is manual, with no RAII, across seven exit paths in
   // a 175-line branch. A unique ID3D11RenderTargetView* owner would make
   // that structural instead.
   rtv->Release();
   break;
  }

  // Command-list recording, the opposite model from DX11 above. Here the
  // layer OWNS a command list and allocator it resets and submits itself;
  // the DX11 branch instead issues immediate-mode draws on the engine's
  // context. The two branches share no code, which is why this function
  // cannot be factored without a backend interface.
  case RendererAPI::API::NVRHI_DX12:
  {
   // Fifth dynamic_cast. This one is not merely an RTTI dependency: it is
   // a per-frame cost, since End runs every frame and a dynamic_cast walks
   // the RTTI table where a static_cast would not. The context cannot
   // change type within a frame, so the cast is pure overhead here.
   auto *ctx = dynamic_cast<NVRHIContext *>(app.GetWindow().GetCurrentContext());
   if (!ctx)
   {
    WL_CORE_ERROR("ImGui: DX12 context is null");
    break;
   }

   // Six natives borrowed from the context, none owned. The command list
   // and allocator are the engine's ImGui-specific ones -- created once by
   // NVRHIContext -- which is why this layer resets rather than creates
   // them, and why there is no matching release anywhere in this file.
   ID3D12GraphicsCommandList *cmdList = static_cast<ID3D12GraphicsCommandList *>(ctx->GetD3D12ImGuiCommandList());
   ID3D12CommandAllocator *allocator = static_cast<ID3D12CommandAllocator *>(ctx->GetD3D12ImGuiCommandAllocator());
   ID3D12DescriptorHeap *srvHeap = static_cast<ID3D12DescriptorHeap *>(ctx->GetD3D12ImGuiSRVHeap());
   ID3D12DescriptorHeap *rtvHeap = static_cast<ID3D12DescriptorHeap *>(ctx->GetD3D12ImGuiRTVHeap());
   ID3D12CommandQueue *queue = static_cast<ID3D12CommandQueue *>(ctx->GetD3D12CommandQueue());
   // Fetched as void* while its five siblings are typed, then cast on the
   // next statement. No functional difference, but it is the reason the
   // six-way check below has to include a resource handle at all.
   void *backBufferResource = ctx->GetD3D12CurrentBackBufferResource();

   // Six-way null test before any DX12 call. Note it does NOT test the
   // backbuffer's current state or whether the device has been removed --
   // unlike the DX11 branch, which checks GetDeviceRemovedReason. A
   // removed device therefore reaches allocator->Reset() below and fails
   // there instead, with a less informative message.
   if (!cmdList || !allocator || !srvHeap || !rtvHeap || !queue || !backBufferResource)
   {
    WL_CORE_WARN("ImGui: DX12 ImGui resources not ready, skipping render");
    break;
   }

   // The cast that pairs with the void* fetch above. Sound only because
   // the context's API was established at init; nothing re-verifies it
   // per frame, so a context swapped underneath a live frame would be
   // reinterpreted as the wrong resource type.
   ID3D12Resource *backBuffer = static_cast<ID3D12Resource *>(backBufferResource);
   // The RTV descriptor stride, needed because the next statement does
   // pointer arithmetic into a descriptor heap rather than using an
   // offset-based API. Retrieved from the context because the driver
   // decides the size and it differs between feature levels.
   uint32_t rtvSize = ctx->GetD3D12RTVDescriptorSize();
   uint32_t idx = ctx->GetCurrentBackBufferIndex();
   // Heap start plus index times stride: the CPU descriptor for THIS
   // frame's backbuffer. DEFECT (unbounded index): the heap must have at
   // least idx+1 descriptors, and nothing here compares idx against the
   // heap's descriptor count. If the swapchain has more buffers than the
   // heap was sized for, this computes an offset past the end of the
   // heap and the subsequent OMSetRenderTargets reads garbage.
   D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
   rtvHandle.ptr += idx * rtvSize;

   // Wait for previous frame if needed (simple fence check)
   // Reset allocator and command list
   // Frees all GPU memory associated with the allocator -- which is only
   // legal once the GPU has finished with those allocations.
   // DEFECT (no fence wait): the comment on line 929 says "Wait for
   // previous frame if needed (simple fence check)", but no fence is
   // actually waited on here. The fence is only SIGNALLED at line 981 and
   // never waited. So a GPU still reading last frame's allocations can
   // have allocator->Reset() free memory out from under it.
   // That is a real use-after-free-shaped hazard, and the comment describes
   // the fix that was not applied.
   HRESULT hr = allocator->Reset();
   if (FAILED(hr))
   {
    // A failed allocator reset leaves the command list still recording the
    // PREVIOUS frame's commands, and the break above skips the submit -- so
    // the list is neither closed nor resubmitted, and the next frame's
    // Reset will fail the same way. This is a sticky failure.
    WL_CORE_ERROR("ImGui: Failed to reset DX12 allocator");
    break;
   }
   // The list must be reset against its allocator, which is why the
   // allocator had to be reset first. Passing nullptr for the initial
   // state is correct: D3D12 requires the initial state only when a list
   // is recorded on a queue that may still be executing it.
   hr = cmdList->Reset(allocator, nullptr);
   if (FAILED(hr))
   {
    // Second sticky failure, same shape as the one above. Both `break`
    // out of the switch rather than out of End, so the viewport tail below
    // still runs -- harmless only because that tail is currently dead.
    WL_CORE_ERROR("ImGui: Failed to reset DX12 command list");
    break;
   }

   // Transition backbuffer to render target
   D3D12_RESOURCE_BARRIER barrier = {};
   barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
   barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
   // The barrier's payload. pResource is a raw resource pointer with no
   // reference taken, which is correct: a barrier does not keep the
   // resource alive, and the backbuffer outlives this command list by
   // virtue of belonging to the swapchain.
   barrier.Transition.pResource = backBuffer;
   // ALL_SUBRESOURCES, which is correct for a backbuffer (it has one)
   // and would be wrong for a mip chain. It is also the value most likely
   // to be copy-pasted into a branch that does have mips.
   barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
   barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
   barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
   // One barrier, recorded rather than submitted. The barrier is part of
   // the command list, so it executes only when ExecuteCommandLists runs
   // near the bottom of this branch -- not here. Reading this code as if
   // the state changed immediately is the easy mistake.
   cmdList->ResourceBarrier(1, &barrier);

   // One RTV from the CPU handle computed on lines 926-927, FALSE for
   // the per-RTV clear flag (no clear), null depth stencil.
   cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
   ID3D12DescriptorHeap *heaps[] = {srvHeap};
   cmdList->SetDescriptorHeaps(1, heaps);

   // GetDrawData() is called again here rather than reusing a local as
   // the DX11 branch does. The pointer is stable within a frame, so the
   // two styles are equivalent; the inconsistency is cosmetic.
   // //
   // DEFECT (no texture sanitising): the DX11 branch above rewrites
   // small ImTextureIDs because a GL texture name is not a valid DX11
   // SRV. There is no equivalent here, and none is needed for the font,
   // but a DX11 SRV pointer reinterpreted as a DX12 descriptor handle is
   // an arbitrary descriptor offset that reads whatever descriptor it
   // lands on. The failure is garbage pixels rather than a crash, which
   // makes it considerably harder to trace back to this line.
   ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList);

   // Transition back to present
   barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
   barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
   cmdList->ResourceBarrier(1, &barrier);

   // A command list must be closed before submission; this is not
   // optional and the API enforces it. Checked, unlike the
   // vkEndCommandBuffer below which only warns.
   hr = cmdList->Close();
   if (FAILED(hr))
   {
    WL_CORE_ERROR("ImGui: Failed to close DX12 command list");
    break;
   }

   // A one-element array where the API wants a pointer-to-pointer. The
   // array is a local, so it must outlive the call -- ExecuteCommandLists
   // copies what it needs before returning, so this is safe.
   ID3D12CommandList *lists[] = {cmdList};
   // Submitting directly to the queue rather than handing the work back
   // to NVRHI. This is the same queue the engine renders the scene on, so
   // ImGui and the scene are ordered by submission order alone -- with
   // no fence between them, which is why the UI can only be assumed to
   // composite on top if the engine's own submission happened first.
   queue->ExecuteCommandLists(1, lists);

   // Existing comment, and it is accurate: the signal exists but no
   // corresponding wait is performed by this layer.
   // Signal fence for next frame's wait (optional, SwapBuffers handles present fence)
   // Increment fence value for tracking
   if (ctx->GetD3D12ImGuiFence())
   {
    ID3D12Fence *fence = static_cast<ID3D12Fence *>(ctx->GetD3D12ImGuiFence());
    // The value is read once and signalled, then incremented below. Reading
    // it again at signal time instead would be the bug; reading once and
    // reusing is correct, because IncrementD3D12FenceValue below is what
    // advances the counter for the next frame.
    uint64_t fenceValue = ctx->GetD3D12ImGuiFenceValue();
    // Signal return value DISCARDED. A failed signal is silent here, and
    // the next frame's counter has already advanced, so the fence can
    // never reach the value it is waiting for -- a wait that never
    // completes rather than one that fails cleanly.
    // //
    // Nothing ever waits on this fence in this file, so the failure has no
    // consequence today. It is documented here because the moment someone
    // adds the matching wait, this becomes the line that breaks it.
    queue->Signal(fence, fenceValue);
    // DEFECT (const_cast on a non-const pointer): ctx is
    // NVRHIContext* -- a pointer to non-const -- so this const_cast is
    // unnecessary. The compiler accepts it and warns on clang
    // (-Wcast-qual) but it signals the author believed ctx was const.
    // The increment is needed because the value was already read on line
    // 980 and each frame must signal a distinct value.
    const_cast<NVRHIContext *>(ctx)->IncrementD3D12FenceValue();
   }

   break;
  }

  // The fourth backend and the shortest non-GL branch. Everything is
  // recorded into a command buffer the engine owns and submits later in
  // SwapBuffers, so this function ends without reaching the GPU at all --
  // the only branch of the four that does not submit.
  case RendererAPI::API::NVRHI_Vulkan:
  {
   auto *ctx = dynamic_cast<NVRHIContext *>(app.GetWindow().GetCurrentContext());
   // Two conditions in one test: the context must exist AND the Vulkan
   // ImGui resources must have been created. IsVulkanImGuiInitialized
   // checks the descriptor pool and render pass for null internally, so
   // this is a single call standing in for two checks.
   if (!ctx || !ctx->IsVulkanImGuiInitialized())
   {
    WL_CORE_WARN("ImGui: Vulkan ImGui not initialized, skipping render");
    break;
   }

   // All three are borrowed and all three can be null independently: the
   // command buffer is per-frame, the render pass is persistent, and the
   // framebuffer depends on the current swapchain image. The combined
   // check below cannot say which one was missing.
   VkCommandBuffer cmd = ctx->GetCurrentVulkanImGuiCommandBuffer();
   VkRenderPass rp = ctx->GetVkImGuiRenderPass();
   VkFramebuffer fb = ctx->GetVkCurrentImGuiFramebuffer();

   if (!cmd || !rp || !fb)
   {
    WL_CORE_WARN("ImGui: Vulkan ImGui command buffer / render pass / framebuffer is null");
    break;
   }

   // The same begin pattern as the font upload in InitBackend, duplicated
   // because the command buffer is different. sType is mandatory; a
   // zeroed sType is rejected by the loader rather than defaulted.
   VkCommandBufferBeginInfo beginInfo = {};
   beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   beginInfo.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   // Checked, and unlike the DX11/DX12 branches this failure is reported
   // rather than silently skipped -- so the Vulkan path is the best
   // instrumented of the three NVRHI branches. It still writes nothing
   // through CheckVkResultFn, which is null.
   VkResult res = vkBeginCommandBuffer(cmd, &beginInfo);
   if (res != VK_SUCCESS)
   {
    WL_CORE_ERROR("ImGui: Failed to begin Vulkan command buffer");
    break;
   }

   // Move the viewport colour attachment into the layout its ImGui descriptor set
   // names. It has to happen HERE, on a command buffer that is open and before the
   // render pass begins, because the scene NVRHI command list was already closed and
   // executed earlier in Application::Run -- there is no NVRHI command list left this
   // frame to record it on. The attachment itself was named by
   // NVRHIFramebuffer::GetColorAttachmentImGuiTexID during OnImGuiRender; this is the
   // only place that knows both the image and the open command buffer.
   //
   // No-ops on every frame where the viewport image is not drawn, which is whenever
   // the panel is closed.
   ctx->RecordVulkanImGuiAttachmentTransition();

   // Value-initialised, which leaves finalLayout, loadOp, storeOp and
   // the rest at zero. The two that matter are below: clearValueCount = 0
   // and pClearValues = nullptr, so the render pass must not have been
   // created with a loadOp that needs clear values.
   VkRenderPassBeginInfo rpBegin = {};
   rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
   rpBegin.renderPass = rp;
   rpBegin.framebuffer = fb;
   // Offset zero, which is correct only because this framebuffer's origin
   // is the framebuffer's own. Unlike the OpenGL and DX11 branches there
   // is no concept of a viewport rectangle here, so the whole render area
   // is covered every frame.
   rpBegin.renderArea.offset = {0, 0};
   // The render area, from the NVRHI context rather than from
   // io.DisplaySize set on line 711. See the inconsistency noted there.
   // DEFECT (zero extent): no check that width and height are non-zero. A
   // minimised window gives a 0x0 extent, and vkCmdBeginRenderPass with a
   // zero area is invalid -- Vulkan will report it through
   // CheckVkResultFn, which is null on line 386, so it is silent.
   rpBegin.renderArea.extent = {ctx->GetWidth(), ctx->GetHeight()};
   rpBegin.clearValueCount = 0;
   rpBegin.pClearValues = nullptr;

   // INLINE subpass contents: the draw commands are recorded into this
   // same command buffer rather than a secondary one. Correct, and it
   // means no subpass dependency has to be declared for the UI pass.
   vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
   // The second and last GetDrawData() call in this branch region, each
   // to a different backend's entry point. Consistent within the file's
   // style even where it is not consistent with itself.
   ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
   vkCmdEndRenderPass(cmd);
   ctx->RecordVulkanImGuiComputeOutputReturnTransition();

   // Ending the command buffer is what makes it submittable. Note the
   // failure here only WARNS and falls through to the break below -- the
   // buffer is left in whatever state vkEndCommandBuffer failed in, and
   // SwapBuffers will still be handed it.
   res = vkEndCommandBuffer(cmd);
   if (res != VK_SUCCESS)
   {
    WL_CORE_ERROR("ImGui: Failed to end Vulkan command buffer");
   }
   // The command buffer is recorded but NOT submitted here.
   // DEFECT (ordering assumption): this assumes SwapBuffers runs after
   // End() and always submits. If ImGui is disabled for a frame, or End()
   // returns early on line 700 or 706, this command buffer is recorded
   // into and never submitted or reset.
   // Submission will be done in NVRHIContext::SwapBuffers
   break;
  }

  // Unreachable, for the same reason Begin's default is:
  // m_CurrentBackend is only ever assigned an enumerator InitBackend
  // knows. Note it is also a real hazard if it were reached, because
  // ImGui_ImplOpenGL3_RenderDrawData would run with no GL backend and
  // no GL context -- and this file includes no GL headers check that
  // could catch it.
  default:
   WL_CORE_WARN("ImGui: Unknown backend {0}, using OpenGL", (int)m_CurrentBackend);
   ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
   break;
  }

  // The viewport tail: UpdatePlatformWindows creates or destroys the
  // detached OS windows, RenderPlatformWindowsDefault draws each
  // viewport's draw data, and the two context calls bracket the pair.
  // //
  // Dead code today, because ViewportsEnable is never set on io. That
  // is the single highest-value thing to fix in this file: the flag is
  // one line in OnAttach and it would activate this block plus the
  // style block in OnAttach, the one in PerformBackendSwitch, and the
  // platform-interface setup in three backend Init paths.
  if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) && m_CurrentBackend == RendererAPI::API::OpenGL)
  {
   // Saved, not assumed. RenderPlatformWindowsDefault makes a different
   // platform window current for each viewport it draws, so the main
   // window's context has to be restored afterwards or the engine's next
   // GL call goes to the wrong context. This is the whole reason the two
   // glfw context calls bracket the pair.
   GLFWwindow *backup_current_context = glfwGetCurrentContext();
   ImGui::UpdatePlatformWindows();
   ImGui::RenderPlatformWindowsDefault();
   // Restored unconditionally rather than null-checked, because
   // glfwGetCurrentContext() above cannot have failed -- and restoring
   // null would itself be legal GLFW. Correct as written; worth stating
   // because the save/restore pairing is easy to break in edit.
   glfwMakeContextCurrent(backup_current_context);
  }
 }

 // The Layer UI hook, and it is deliberately EMPTY -- everything
 // commented out below.
 // So this layer draws nothing of its own: its whole job is context
 // creation, backend management and event capture. Actual ImGui
 // widgets are pushed by other layers between Begin() and End().
 void ImGuiLayer::OnImGuiRender()
 {
  // The commented-out demo window. A static local would have been
  // thread-safe under C++11 initialisation, but as dead code it documents
  // that ImGui::ShowDemoWindow is the quick way to confirm the backend
  // actually renders.
  // static bool show = false;
  // ImGui::ShowDemoWindow(&show);
 }

}
