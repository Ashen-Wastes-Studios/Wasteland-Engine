// The translation unit that exists only to compile ImGui's platform and
// renderer backends into the engine.

// ImGui ships its backends as .cpp files with external symbols, so the usual
// approach is to add them to the build as separate translation units. This
// file instead #includes them, which keeps the set of backends visible in one
// place and guarantees they see wlpch.h (and therefore the engine's GL
// typedefs) first.
#include "wlpch.h"

// Intended to tell imgui_impl_opengl3.cpp to use the engine's glad loader
// instead of ImGui's bundled one.

// DEFECT -- this macro is inert in this vendored version. The only loader
// selector imgui_impl_opengl3.cpp actually tests is
// IMGUI_IMPL_OPENGL_LOADER_CUSTOM (backends/imgui_impl_opengl3.cpp:143 and
// :263). There is no IMGUI_IMPL_OPENGL_LOADER_GLAD branch anywhere under
// vendor/imgui -- a repo-wide search finds that identifier only here and in
// the unrelated Box2D testbed copy. imgui_impl_opengl3_loader.h spells out
// the consequence at lines 17-18: a third-party loader in the same
// translation unit needs IMGUI_IMPL_OPENGL_LOADER_CUSTOM, not this.

// So what actually happens: imgui_impl_opengl3.cpp falls into the
// `!defined(IMGUI_IMPL_OPENGL_LOADER_CUSTOM)` arm, defines IMGL3W_IMPL, and
// compiles ImGui's own stripped gl3w-derived loader (opengl3.cpp:151-152).
// Two GL loaders therefore coexist in this translation unit -- glad via
// wlpch.h and imgl3w -- and the define that was supposed to prevent that is
// not what prevents it. It happens to build because the two expose disjoint
// symbol sets, but that is luck rather than intent, and the loader header
// warns at line 14 that this exact configuration produces "macro
// redefinitions or function redefinitions" build errors.

// The fix is to define IMGUI_IMPL_OPENGL_LOADER_CUSTOM instead (and keep
// glad available to the TU, which wlpch.h already does). Left as-is and
// documented, not changed.
#define IMGUI_IMPL_OPENGL_LOADER_GLAD
// Renderer backend: uploads ImGui draw data to OpenGL.

// This is the only renderer backend compiled here. ImGuiLayer.cpp includes
// backends/imgui_impl_dx11.h, _dx12.h and _vulkan.h (its lines 9-11) and
// calls ImGui_ImplDX11_NewFrame and friends (its lines 668-678), so the
// NVRHI backends are expected to be linked from somewhere else in the build;
// this file is not where they come from.
#include "backends/imgui_impl_opengl3.cpp"
// Platform backend: pumps GLFW input and maintains io.DisplaySize /
// io.DeltaTime for ImGui.

// Included after the renderer backend because the renderer backend's headers
// are the ones that need the GL loader macro above, and that macro must be
// defined before either include is seen.
#include "backends/imgui_impl_glfw.cpp"