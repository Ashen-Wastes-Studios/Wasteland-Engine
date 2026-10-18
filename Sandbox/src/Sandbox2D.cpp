// Own header first, so the class definition and its members are visible
// before anything else. It is quoted, so the compiler looks beside this file
// before falling back to the include path.
#include "Sandbox2D.h"

// ImGui for the debug panel in OnImGuiRender. Quoted with a directory
// segment because the vendored copy lives under the project's include
// root rather than on the system include path.
#include <imgui/imgui.h>

// glm::radians for the per-frame rotation below. Matrix transform is pulled
// in transitively by the engine header, but relying on that would make this
// file's dependency on it invisible.
#include <glm/gtc/matrix_transform.hpp>
// glm::value_ptr, for handing the address of a glm::vec4 to ImGui, which
// takes void*. This is the one place the two libraries meet.
#include <glm/gtc/type_ptr.hpp>

// Map width in TILES.
//
// DEFECT (documented, not fixed): this constant has NO READER AT ALL. A
// search of the whole tree finds one occurrence of `s_MapWidth` here plus
// two more in an unrelated file. The tilemap code at the bottom of this
// file -- the only thing that could want it -- uses `m_MapWidth`, a class
// member, instead. So the value 24 is written out, agrees by coincidence
// with the literal below, and is then never consulted.
static const uint32_t s_MapWidth = 24;
// The tilemap, as one character per tile. Adjacent string literals are
// concatenated at compile time, so this is a single 336-character array
// written as 14 readable rows.
//
// DEFECT (documented, not fixed): the row count and width are implicit.
// Nothing declares either; they are only recoverable by counting the
// characters. Every row is exactly 24 wide and there are 14 of them, which
// happens to match s_MapWidth -- but the #if 0 loop at the bottom of this
// file derives its bounds from m_MapHeight and m_MapWidth, neither of which
// this class has, so the literal cannot be switched back into use without
// adding both members and restating these two numbers as their values.
//
// Only three distinct characters appear: W, D and a single C (row 4). The
// C is unique in the map, so whatever it means is expressed exactly once.
//
// Minor: this line ends in trailing whitespace after the `=`. Harmless to
// the compiler, but it is why tools flag the file and it is inconsistent
// with every other declaration in it.
static const char* s_MapTiles = 
"WWWWWWWWWWWWWWWWWWWWWWWW"
"WWWWWWWDDDDDWWWWWWWWWWWW"
"WWWWWDDDDDDDDDDWWWWWWWWW"
"WWWWDDDDDDDDDDDDCDWWWWWW"
"WWWDDDDDDDDDDDDDDDDDWWWW"
"WWDDDDWWWDDDDDDDDDDDDWWW"
"WDDDDDWWWDDDDDDDDDDDDDWW"
"WWDDDDDDDDDDDDDDDDDDDWWW"
"WWWWDDDDDDDDDDDDDDDDWWWW"
"WWWWWDDDDDDDDDDDDDDWWWWW"
"WWWWWWDDDDDDDDDDDWWWWWWW"
"WWWWWWWDDDDDDDDDWWWWWWWW"
"WWWWWWWWWWDDDDWWWWWWWWWW"
"WWWWWWWWWWWWWWWWWWWWWWWW";

// Default constructor. The camera controller gets its aspect ratio here
// rather than in OnAttach because it is a value member and has no access to
// the window at construction time.
Sandbox2D::Sandbox2D()
		// Hard-coded 16:9. DEFECT (documented, not fixed): the window can be
		// resized -- this class even handles a resize event -- and nothing here
		// re-reads the real aspect. A non-16:9 window therefore renders with a
		// stretched or letterboxed projection until the layer is recreated.
	: Layer("Sandbox2D"), m_CameraController(1280.0f / 720.0f)
{
}

// Called once when the layer is pushed. Everything that needs the graphics
// device is created here rather than in the constructor, because the
// constructor may run before the device exists.
void Sandbox2D::OnAttach()
{
		// Traces this whole function as one entry in a profiler timeline.
	WL_PROFILE_FUNCTION();

		// Loaded from a path relative to the executable, not to this source file.
		// The asset pipeline has to have copied it there; nothing in this file
		// checks that it did.
	//
		// The return value is assigned straight into the member, so a missing file
		// yields whatever Texture2D::Create returns for failure -- this file does
		// not test for it, and the draw calls below would then pass a null Ref.
	m_CheckerboardTexture = Wasteland::Texture2D::Create("assets/textures/Checkerboard.png");
		// A sprite sheet, loaded but NEVER DRAWN anywhere in the project: this is
		// its only assignment and its only other reference is the declaration in
		// the header. The texture is loaded from disk and uploaded to the GPU every
		// run of this layer, for nothing.
	//
		// DEFECT (documented, not fixed): dead resource. There is also no
		// SubTexture2D built from it, which is what would be needed to slice a
		// sheet -- the class holds only the whole-sheet Ref.
	m_SpriteSheet = Wasteland::Texture2D::Create("assets/game/textures/RPGpack_sheet_2X.png");

		// Particle gradient: warm yellow fading to orange. Components are written
		// as integer/255.0f rather than as literals -- the division is exact for
		// 8-bit values but the source form makes the 0..1 intent explicit.
	m_Particle.ColorBegin = { 254 / 255.0f, 212 / 255.0f, 123 / 255.0f, 1.0f };
	m_Particle.ColorEnd = { 254 / 255.0f, 109 / 255.0f, 41 / 255.0f, 1.0f };
		// Three size fields in one comma expression: every particle starts at 0.5
		// units, varies by +/-0.3, and shrinks to 0 by the end of its life.
	m_Particle.SizeBegin = 0.5f, m_Particle.SizeVariation = 0.3f, m_Particle.SizeEnd = 0.0f;
		// Five seconds of life. Long-lived enough that a burst of five stays
		// visible while the camera is panned around.
	m_Particle.LifeTime = 5.0f;
		// Zero base velocity with +/- (3, 1) variation, so emitted particles spray
		// sideways and drift -- an emitter position with no push of its own.
	m_Particle.Velocity = { 0.0f, 0.0f };
	m_Particle.VelocityVariation = { 3.0f, 1.0f };
	m_Particle.Position = { 0.0f, 0.0f };

		// Zoom 5 at attach time. DEFECT (documented, not fixed): a fixed zoom
		// level means the sandbox looks identical on a 720p laptop and a 4K
		// monitor, and the wheel-zoom the camera controller implements fights it.
	m_CameraController.SetZoomLevel(5.0f);
}

// Teardown. Deliberately empty: the Ref members release on their own, and
// the engine contract is that GPU resources cannot safely be destroyed from
// here once the device is gone.
void Sandbox2D::OnDetach()
{
	WL_PROFILE_FUNCTION();
}

// One frame of the sandbox: update the camera, queue draws, then handle
// input and particles.
void Sandbox2D::OnUpdate(Wasteland::Timestep ts)
{
	WL_PROFILE_FUNCTION();

	// Update
		// Camera first, because every draw below needs its view-projection.
	m_CameraController.OnUpdate(ts);

	// Render
		// Stats are reset at the START of the frame, so the panel in OnImGuiRender
		// reports the frame just queued rather than a running total across the
		// session.
	Wasteland::Renderer2D::ResetStats();
		// Scoped purely so the profiler attributes the clear to "Renderer Prep"
		// separately from the draw phase below.
	{
		WL_PROFILE_SCOPE("Renderer Prep");
				// Dark grey clear, matching the editor palette. Set every frame rather than
				// once, because BeginScene/EndScene pairs do not imply a clear.
		Wasteland::RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1.0f });
		Wasteland::RenderCommand::Clear();
	}

	{
				// Function-local static, so the accumulated rotation survives the frame
				// without adding a member. DEFECT (documented, not fixed): it is not reset
				// by OnAttach, so the spin resumes from wherever it was if the layer is
				// detached and re-attached.
		static float rotation = 0.0f;
				// Degrees per SECOND, converted by glm::radians at the draw call below.
				// Using a frame delta here is what keeps the spin frame-rate independent.
		rotation += ts * 50.0f;

		WL_PROFILE_SCOPE("Renderer Draw");
				// BeginScene uploads the camera's view-projection to the renderer before
				// the first vertex, so every quad that follows is placed in camera space.
		Wasteland::Renderer2D::BeginScene(m_CameraController.GetCamera());
				// -45 degrees, converted from degrees. Note this one does NOT rotate over
				// time: it is the static counter-rotation that makes the rotated-quad path
				// visibly different from the plain quad below.
		Wasteland::Renderer2D::DrawRotatedQuad({ 1.0f, 0.0f }, { 0.8f, 0.8f }, glm::radians(-45.0f), {0.8f, 0.2f, 0.3f, 1.0f});
		Wasteland::Renderer2D::DrawQuad({ -1.0f, 0.0f }, { 0.8f, 0.8f }, { 0.8f, 0.2f, 0.3f, 1.0f });
		Wasteland::Renderer2D::DrawQuad({ 0.5f, -0.5f }, { 0.5f, 0.75f }, m_SquareColor);
				// A 20x20 checkerboard tiled 10 times, pushed to z = -0.1 so it sits behind
				// the other quads. Tiling is a shader-side multiply, so the single quad
				// costs the same as an untextured one.
		Wasteland::Renderer2D::DrawQuad({ 0.0f, 0.0f, -0.1f }, { 20.0f, 20.0f }, m_CheckerboardTexture, 10.0f);
				// The one quad that animates: rotated by the accumulated value above and
				// tiled 20 times. Deliberately at z = 0 so it composites with the checkerboard
				// rather than being occluded by it.
		Wasteland::Renderer2D::DrawRotatedQuad({ -2.0f, 0.0f, 0.0f }, { 1.0f, 1.0f }, glm::radians(rotation), m_CheckerboardTexture, 20.0f);
		Wasteland::Renderer2D::EndScene();

				// A SECOND BeginScene/EndScene pair in the same frame. Legal -- the pairs
				// bracket batches -- but it means the vertex buffer is flushed and rebound
				// twice per frame, and any RenderCommand state set between the two pairs
				// would not carry across.
		Wasteland::Renderer2D::BeginScene(m_CameraController.GetCamera());
				// 20x20 grid at 0.5 spacing. Built by nested float loops rather than by
				// integer iteration, which is why the bounds are written as -5.0f and
				// 5.0f and the step as 0.5f.
		//
				// DEFECT (documented, not fixed): the loop runs 20 iterations per axis
				// (from -5.0 in steps of 0.5 up to but excluding 5.0), so 400 quads every
				// frame with no frustum check at all -- every grid square is submitted even
				// when it is entirely off-screen. The cost scales with the grid size, not
				// with what is visible.
		for (float y = -5.0f; y < 5.0f; y += 0.5f)
		{ 
			for (float x = -5.0f; x < 5.0f; x += 0.5f)
			{
								// Alpha 0.7, so the grid squares blend with whatever is behind them and
								// the overlap of the two scenes reads as transparency.
				//
								// The red channel is derived from x and the green is constant: the gradient
								// runs left-to-right only. The alpha does the same with y, so the grid is
								// a smooth ramp in both axes with no visible cell edges.
				glm::vec4 color = { (x + 5.0f) / 10.0f, 0.4f, (y + 5.0f) / 10.0f, 0.7f };
				Wasteland::Renderer2D::DrawQuad({ x, y }, { 0.45f, 0.45f }, color);
			}
		}
		Wasteland::Renderer2D::EndScene();
	}

		// Poll, not event: this reads the button state for the frame rather than
		// reacting to a press event, so holding the button emits continuously.
	if (Wasteland::Input::IsMouseButtonPressed(WL_MOUSE_BUTTON_LEFT))
	{
				// Screen pixels, straight from the input layer.
		auto [x, y] = Wasteland::Input::GetMousePosition();
				// DEFECT (documented, not fixed): unsigned integer window dimensions divided
				// into a float expression. If the window were ever 0x0 -- a minimized
				// window can report that, and WindowResizeEvent does not validate against
				// zero -- both divisors are zero and the two lines below produce inf or NaN
				// coordinates, which propagate into the particle system.
		auto width = Wasteland::Application::Get().GetWindow().GetWidth();
		auto height = Wasteland::Application::Get().GetWindow().GetHeight();

				// The camera's visible world-space rectangle. Converting a screen point to
				// world space is a matter of mapping [0,1] onto that rectangle.
		auto bounds = m_CameraController.GetBounds();
		auto pos = m_CameraController.GetCamera().GetPosition();
				// x is NOT flipped: screen x grows rightward and world x does too. The y
				// line below inverts, because screen y grows DOWNWARD while world y grows
				// upward.
		x = (x / width) * bounds.GetWidth() - bounds.GetWidth() * 0.5f;
				// The y flip, as noted above. Written as 0.5 - t rather than 1 - t so the
				// maths reads as "reflect about the centre line".
				// DEFECT (documented, not fixed): if this ever ran with a negative height
				// the result would be inverted twice; with a zero height, as above, the
				// whole expression is NaN.
		y = bounds.GetHeight() * 0.5f - (y / height) * bounds.GetHeight();
				// Camera position added because the bounds rectangle is centred on the
				// ORIGIN, not on the camera. Without this the emitter would be pinned to
				// the world centre while the view moved.
		m_Particle.Position = { x + pos.x, y + pos.y };
				// Five particles per click, not per frame. With the poll above, holding the
				// button for one second at 60 fps emits about 300 -- there is no rate limit
				// on the emit call, only on the click.
		for (int i = 0; i < 5; i++)
			m_ParticleSystem.Emit(m_Particle);
	}

		// Simulation then rendering, both after the manual draw calls. Particle
		// state is updated before it is submitted, so what is drawn this frame is
		// the post-step state.
	m_ParticleSystem.OnUpdate(ts);
		// Rendered with the same camera, and OUTSIDE any BeginScene/EndScene pair --
		// the particle system opens and closes its own scene.
	m_ParticleSystem.OnRender(m_CameraController.GetCamera());

// A disabled tilemap renderer, kept for reference. Everything from here to
// the #endif is not compiled.
#if 0
	Wasteland::Renderer2D::BeginScene(m_CameraController.GetCamera());

		// DEFECT (documented, not fixed): this #if 0 block CANNOT be re-enabled as
		// written. It refers to m_MapHeight, s_TextureMap and m_TextureStars, none
		// of which exist -- m_MapHeight and m_TextureStairs are neither members of
		// Sandbox2D nor file statics in the header, and s_TextureMap is not declared
		// anywhere. Three separate names would have to be supplied before the block
		// would compile, so "just flip the #if to 0" is not a one-character change.
	for (uint32_t y = 0; y < m_MapHeight; y++)
	{
		for (uint32_t x = 0; x < m_MapWidth; x++)
		{
						// Row-major indexing: each row is s_MapWidth characters wide, so the same
						// index also walks the string off its end if the rows are not all exactly
						// that long.
			char tileType = s_MapTiles[x + y * m_MapWidth];
			Wasteland::Ref<Wasteland::SubTexture2D> texture;
			if (s_TextureMap.find(tileType) != s_TextureMap.end())
				texture = s_TextureMap[tileType];
			else
				texture = m_TextureStairs;

						// Integer division promoted to float for the centring offset. Note the y
						// term is `m_MapHeight - y`, which flips the row order so the first row of
						// the literal ends up at the TOP of the map.
			Wasteland::Renderer2D::DrawQuad({ x - m_MapWidth / 2.0f, m_MapHeight - y - m_MapHeight / 2.0f, 0.5f }, { 1.0f, 1.0f }, texture);
		}
	}

	// Wasteland::Renderer2D::DrawQuad({ 0.0f, 0.0f, 0.5f }, { 1.0f, 1.0f }, m_TextureStairs);
	// Wasteland::Renderer2D::DrawQuad({ 1.0f, 0.0f, 0.5f }, { 1.0f, 1.0f }, m_TextureBarrel);
	// Wasteland::Renderer2D::DrawQuad({ -1.0f, 0.0f, 0.5f }, { 1.0f, 2.0f }, m_TextureTree);
	Wasteland::Renderer2D::EndScene();
#endif
}

// The ImGui passthrough. Runs after the scene draws, so the panel appears
// on top of whatever the frame rendered.
void Sandbox2D::OnImGuiRender()
{
	WL_PROFILE_FUNCTION();

	ImGui::Begin("Settings");

		// By value: Statistics is four uint32_t, cheaper to copy than to reference,
		// and the snapshot is exactly the frame just queued -- a later draw in the
		// same frame would not be reflected in it.
	auto stats = Wasteland::Renderer2D::GetStats();
	ImGui::Text("Renderer2D Stats:");
		// The two counters the panel shows directly. Both are reset by the
		// ResetStats call in OnUpdate.
	ImGui::Text("Draw Calls: %d", stats.DrawCalls);
	ImGui::Text("Quads: %d", stats.QuadCount);
		// Derived from QuadCount rather than counted separately, so it cannot
		// disagree with the quad count it describes.
	ImGui::Text("Vertices: %d", stats.GetTotalVertexCount());
	ImGui::Text("Indices: %d", stats.GetTotalIndexCount());

		// glm::value_ptr rather than &m_SquareColor: ImGui needs a float*, and the
		// vec4 is four contiguous floats, so the reinterpretation is well-defined
		// here. Edits land directly in the member, which is why the draw above and
		// this widget stay in sync.
	ImGui::ColorEdit4("Square Color", glm::value_ptr(m_SquareColor));

		// Disabled ImGui image preview. The texture ID it would need is fetched
		// through the renderer abstraction, and the commented call takes a
		// reinterpret_cast-style C-style cast of an integer to void* -- it would
		// have to be cast to the GLuint-sized type ImGui expects.
	// uint32_t textureID = m_CheckerboardTexture->GetRendererID();
	// ImGui::Image((void*)textureID, ImVec2{ 1280.0f, 720.0f });
	ImGui::End();
}

// Event fan-out. The camera controller needs the resize and mouse events,
// so everything is forwarded rather than filtered here.
void Sandbox2D::OnEvent(Wasteland::Event& e)
{
		// Forwarded unfiltered, and the layer does not mark any event handled. So
		// every event also continues to the layers below this one in the stack.
	m_CameraController.OnEvent(e);
}
