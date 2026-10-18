// The engine's single umbrella header. Applications include this one file and
// get the public API; nothing else in the engine is included directly.

// That is the whole design intent: Wasteland.h is the public surface, so
// adding a line here widens what studios can depend on.
#pragma once

// Present-tense statement of the file's purpose, sitting above the includes.
// For use in Wasteland applications

// FIRST include deliberately, and not alphabetical: Core.h defines WL_API,
// the platform macros (WL_PLATFORM_LINUX and friends), and the Ref/Scope
// aliases that every header below uses in its own declarations. Anything
// included before it would fail to compile.

// This ordering is load-bearing. It is also why headers deeper in the tree
// can get away with using Ref<T> without including Core.h themselves --
// see the self-containment notes in Renderer/Shader.h and
// Renderer/VertexArray.h.
#include "Wasteland/Core/Core.h"

// The application object: owns the window, the layer stack, and the main
// loop. Applications subclass or instantiate this.
#include "Wasteland/Core/Application.h"
// The attachable-unit interface every subsystem layer derives from.
#include "Wasteland/Core/Layer.h"
// The logging macros (WL_CORE_INFO and friends) used across the public API.
#include "Wasteland/Core/Log.h"

// Per-frame delta time, as passed to Update().
#include "Wasteland/Core/Timestep.h"

// The polling input API: IsKeyPressed, GetMousePosition, and friends.
#include "Wasteland/Core/Input.h"
// Key code constants for the input API above.

// These mirror GLFW's values -- that is what lets
// Platform/Linux/LinuxInput.cpp pass a keycode straight to glfwGetKey without
// a translation table.
#include "Wasteland/Core/KeyCodes.h"
// Mouse button constants, same GLFW-mirroring arrangement as the key codes.
#include "Wasteland/Core/MouseButtonCodes.h"
// 2D camera controller. Pulled in at umbrella level because a 2D game needs
// it immediately and it is not a heavier dependency than the renderer
// headers beside it.
#include "Wasteland/Renderer/OrthographicCameraController.h"

// The ImGui integration layer.

// Exposed here so applications can drive ImGui without a second include.
#include "Wasteland/ImGui/ImGuiLayer.h"

// The scene: entities, components, and the registry.
#include "Wasteland/Scene/Scene.h"
// The entity handle. Separate from Scene.h because Entity is the type
// application code holds far more often than it constructs a Scene.
#include "Wasteland/Scene/Entity.h"
// Entity plus scripting conveniences, for code that binds entities to scripts.
#include "Wasteland/Scene/ScriptableEntity.h"
// Every built-in component: Transform, ID, Tag, Camera, and the rest, in one
// header rather than one header each.

// Consolidating them is a compile-time trade -- editing any component forces
// a recompile of everything that includes this -- but it keeps the umbrella
// header short and the component list discoverable in one place.
#include "Wasteland/Scene/Components.h"

// ASCII rule marking the renderer block below.

// DEFECT (stale comment, cosmetic): the dashes are uneven -- 5 before
// the word, 36 after. Compare the closing rule at line 39, which is 50. These
// are hand-drawn separators, not generated, and they have drifted.
// ---Renderer------------------------------------
// The render pipeline: submission, sorting, and backend selection.
#include "Wasteland/Renderer/Renderer.h"
// 2D renderer: sprites, text, and a 2D camera.
#include "Wasteland/Renderer/Renderer2D.h"
// 3D renderer: PBR shading, the material model, and the neural/uniform
// plumbing the Renderer3D_Basic.glsl shader reads.
#include "Wasteland/Renderer/Renderer3D.h"
// The command-buffer type the renderers record into.

// Note this header is what grants RenderCommand friendship to RendererAPI
// (RendererAPI.h declares `friend class RenderCommand`), which is how the
// one place that assigns RendererAPI::s_API -- RendererAPI.cpp -- is reached.
#include "Wasteland/Renderer/RenderCommand.h"

// BufferLayout, VertexBuffer and IndexBuffer interfaces. Included before the
// concrete headers below because those derive from these.
#include "Wasteland/Renderer/Buffer.h"
// Shader interface and ShaderLibrary.

// This header is NOT self-contained -- it uses Ref<T> without including
// Core.h -- and compiles only because line 5 above did that first. In this
// umbrella that ordering is exactly what makes it work.
#include "Wasteland/Renderer/Shader.h"
// Off-screen render targets and their specification.
#include "Wasteland/Renderer/Framebuffer.h"
// Texture interfaces. Unlike Shader.h and VertexArray.h this one includes
// Core.h itself, so it is self-contained -- which is why the comment above
// about ordering applies to its neighbours but not to it.
#include "Wasteland/Renderer/Texture.h"
// Sprite-sheet sub-regions of a Texture2D.
#include "Wasteland/Renderer/SubTexture2D.h"
// The vertex array interface.

// Also not self-contained (uses Ref<T> with no Core.h include), same as
// Shader.h; the umbrella's line 5 is what lets it compile.
#include "Wasteland/Renderer/VertexArray.h"

// The 2D camera itself, as distinct from the controller included at line 16.
#include "Wasteland/Renderer/OrthographicCamera.h"
// Closing rule for the renderer block.

// 50 dashes against the 36 on the opening rule at line 25 -- see the note
// there. Purely cosmetic, but it is the kind of drift that makes a file look
// hand-maintained when it is.
// --------------------------------------------------