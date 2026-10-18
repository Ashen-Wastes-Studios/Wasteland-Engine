#pragma once

// Quoted: resolved relative to this file first, then the include path.
// Brings in the Layer base class plus the engine entry points; this is
// the umbrella header, so nothing else from the engine core is needed.
#include "Wasteland.h"
// Angle brackets: include-path lookup only, never this directory.
// Needed for Wasteland::Texture2D, which the Ref members below hold.
#include <Wasteland/Renderer/Texture.h>

// Sandbox-local header, kept last because ParticleSystem.h itself leans
// on the engine types from lines 3-4.
#include "ParticleSystem.h"

// A Layer is the engine's per-scene unit: it gets pushed onto an
// application layer stack and receives OnAttach/OnUpdate/OnEvent ticks.
// Public inheritance is required -- the application stores Layers and
// calls through base pointers, so a private base would be unusable.
class Sandbox2D : public Wasteland::Layer
{
public:
	// Out-of-line definition lives in Sandbox2D.cpp: the texture handles and
	// particle props are loaded there because they need the graphics device.
	Sandbox2D();
	// The refcounted Texture2D Refs release themselves, so there is nothing
	// bespoke to clean up. virtual because the base Layer is polymorphic and
	// instances are deleted through a Layer pointer.
	virtual ~Sandbox2D() = default;

	// Called once when the layer becomes active: create/queue the graphics
	// resources the render loop will later use.
	// override is a compile-time check that the base signature still exists --
	// if Layer renames or re-params it, this becomes a hard error.
	virtual void OnAttach() override;
	// Teardown counterpart to OnAttach. Note the engine contract matters
	// here: GPU resources generally may not be released from here if the
	// graphics device is already gone, so this is a natural place for leaks.
	virtual void OnDetach() override;

	// Per-frame tick. Timestep carries the frame delta so simulation is
	// frame-rate independent rather than assuming a fixed 60 Hz.
	// Style inconsistency worth noting: this and OnEvent omit the explicit
	// 'virtual' the three handlers above carry. It is harmless -- 'override'
	// already implies virtual -- but the file is not internally consistent.
	void OnUpdate(Wasteland::Timestep ts) override;
	// ImGui passthrough so editor/debug overlays draw on top of this layer;
	// the base Layer presumably declares a no-op default for it.
	virtual void OnImGuiRender() override;
	// Non-const reference because handlers are expected to consume events
	// (mark handled, mutate state) rather than observe them.
	void OnEvent(Wasteland::Event& e) override;
// Only the layer itself touches these; there is no public accessor for
// the textures or the camera controller.
private:
	// Owns pan/zoom input handling and the camera itself. Held by value (no
	// pointer) so there is no allocation and no null state at construction.
	Wasteland::OrthographicCameraController m_CameraController;

	// Ref<> is a shared handle: copying is cheap, and the texture is kept
	// alive as long as this layer or any copy of the handle exists.
	Wasteland::Ref<Wasteland::Texture2D> m_CheckerboardTexture;
	// Second texture; both are Refs so upload timing can be deferred to
	// OnAttach while the layer is already safely keeping them alive.
	Wasteland::Ref<Wasteland::Texture2D> m_SpriteSheet;

	// Default blue, non-premultiplied-looking alpha of 1.0 (fully opaque).
	// In-class initialiser means it is valid before OnAttach runs, so no
	// separate constructor initialiser list entry is needed.
	glm::vec4 m_SquareColor = { 0.2f, 0.3f, 0.8f, 1.0f };

	// Particle storage and update logic, by value. Simulation state lives
	// here rather than in the render layer so it can tick independently of
	// whether anything is drawn this frame.
	ParticleSystem m_ParticleSystem;
	// The single template describing every spawned particle (lifetime,
	// gravity, colours). One instance implies one uniform look for all
	// particles -- varying per-particle props would need an instanced buffer.
	ParticleProps m_Particle;
};