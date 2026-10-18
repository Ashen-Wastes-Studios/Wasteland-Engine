// Precompiled header. Note the capital W and lowercase lpch -- every other
// engine .cpp in this tree includes "wlpch.h" with a lowercase w, so this
// file's include only resolves on a case-insensitive filesystem. On a Linux
// or other case-sensitive build this is a compile error waiting to happen.
#include "Wlpch.h"
#include "OrthographicCameraController.h"

// The static input facade. Called as Input::IsKeyPressed below, which
// forwards through the platform backend registered in Input::s_Instance.
// Those accessors dereference that pointer with no null check, so this file
// assumes a backend exists by the time OnUpdate first runs.
#include "Wasteland/Core/Input.h"
// For the WL_KEY_* names used in the movement tests.
#include "Wasteland/Core/KeyCodes.h"
// For WL_MOUSE_BUTTON_2, the middle button that gates all panning.
#include "Wasteland/Core/MouseButtonCodes.h"

namespace Wasteland {



	// Constructs the bounds and the camera together.
	//
	// @param aspectRatio  Width / height of the viewport.
	// @param rotation     Whether Q/E rotation is enabled; defaults to false in
	//                      the header. 2D cameras are normally axis-aligned.
	OrthographicCameraController::OrthographicCameraController(float aspectRatio, bool rotation)
		// Four initialisers, and the declaration order matters more than it looks.
		//
		// m_Bounds is initialised using m_AspectRatio, and member initialisers run
		// in DECLARATION order rather than the order written here. m_AspectRatio is
		// declared first in the header, so it is already set when m_Bounds reads
		// it. That works, but only because of that declaration order -- reordering
		// the header's members would silently break this line.
		//
		// m_Camera is initialised FROM m_Bounds, which is likewise only correct
		// because m_Bounds is declared before m_Camera. Same fragility.
		//
		// The bounds are the symmetric ortho box: horizontal extent is the aspect
		// ratio times the zoom level, vertical extent is the zoom level itself. So
		// zoom 1.0 at 16:9 shows 32 x 2 world units.
		//
		// m_CameraTranslationSpeed and m_CameraRotationSpeed are NOT in this list --
		// they have in-class initialisers in the header, which survive because the
		// constructor does not mention them. Note OnUpdate then overwrites the
		// translation speed with m_ZoomLevel every frame, so that 5.0f is dead
		// after the first update.
		: m_AspectRatio(aspectRatio), m_Bounds({ -m_AspectRatio * m_ZoomLevel, m_AspectRatio * m_ZoomLevel, -m_ZoomLevel, m_ZoomLevel }), m_Camera(m_Bounds.Left, m_Bounds.Right, m_Bounds.Bottom, m_Bounds.Top), m_Rotation(rotation)
	// Empty body -- everything happens in the member initialiser list.
	//
	// m_CameraRotation and m_CameraPosition also rely on in-class initialisers
	// rather than being listed, which is why the camera starts at the origin,
	// unrotated.
	{
	}

	// Per-frame input integration. Not virtual, not an override: this is a plain
	// mixin, and whoever owns the controller calls this directly rather than
	// through a Layer base.
	//
	// @param ts  Frame delta. Taking it is what makes the pan and rotation
	//            speeds frame-rate independent; without it the camera would
	//            move a fixed step per CALL, so speed would depend on frame
	//            rate.
	void OrthographicCameraController::OnUpdate(Timestep ts)
	{
		// Names the enclosing scope for the profiler, so the frame-time samples
		// group under this function rather than under its caller.
		WL_PROFILE_FUNCTION();

		// Pan left. Two conditions ANDed -- the key AND the middle mouse button --
		// so the middle button is a modifier that must be held for panning at all.
		//
		// IsKeyPressed is LEVEL-triggered, not edge-triggered: it stays true for as
		// long as the key is down, so this branch runs every frame the combination
		// is held. That is what makes the per-frame accumulation below work.
		if (Input::IsKeyPressed(WL_KEY_A) && Input::IsMouseButtonPressed(WL_MOUSE_BUTTON_2))
			// Multiplied by ts, so movement is in world units per second rather than
			// per frame.
			//
			// Moves the controller's own copy of the position, not the camera's -- the
			// camera is only told at the end of the function. See the duplicated-state
			// note on m_CameraPosition in the header.
			m_CameraPosition.x -= m_CameraTranslationSpeed * ts;
		// Pan right.
		//
		// This is else-if, not a second if. That is a real constraint rather than
		// just style: on a keyboard reporting more than one key at once (an
		// on-screen keyboard, a macro pad, a remapped pair) holding A and D together
		// moves left and never right, because the first branch wins and the second
		// is never evaluated. Two independent ifs would give the more forgiving
		// behaviour, and the two branches are otherwise symmetric.
		else if (Input::IsKeyPressed(WL_KEY_D) && Input::IsMouseButtonPressed(WL_MOUSE_BUTTON_2))
			m_CameraPosition.x += m_CameraTranslationSpeed * ts;

		// Pan up, on the same modifier pattern. A separate if-chain rather than an
		// extension of the horizontal one, so vertical and horizontal panning are
		// independent -- which is what lets diagonal panning work.
		if (Input::IsKeyPressed(WL_KEY_W) && Input::IsMouseButtonPressed(WL_MOUSE_BUTTON_2))
			m_CameraPosition.y += m_CameraTranslationSpeed * ts;
		else if (Input::IsKeyPressed(WL_KEY_S) && Input::IsMouseButtonPressed(WL_MOUSE_BUTTON_2))
			m_CameraPosition.y -= m_CameraTranslationSpeed * ts;

		// The whole rotation block is gated on the constructor flag, so a non
		// rotating camera ignores Q and E entirely instead of the user having to
		// avoid those keys.
		//
		// DEFECT: no else branch, so SetRotation is never called for a
		// non-rotating controller. Harmless today, because the camera starts at
		// zero rotation and nothing else here can change it -- but anything
		// reaching the camera through GetCamera().SetRotation() would have that
		// change overwritten every frame in the rotating case and never in the
		// non-rotating one. The asymmetry is a trap.
		if (m_Rotation)
		{
			// Rotate one way. No mouse-button modifier here, unlike panning --
			// rotation is unguarded, so Q and E rotate the camera whether or not the
			// middle button is held.
			//
			// Separate ifs, not an if/else: holding Q and E together applies both
			// deltas and cancels, which is the correct behaviour here and the opposite
			// of the A/D case above.
			if (Input::IsKeyPressed(WL_KEY_Q))
				// Degrees per second times the frame delta -- the same frame-rate
				// independence argument as the pan speed.
				m_CameraRotation += m_CameraRotationSpeed * ts;
			if (Input::IsKeyPressed(WL_KEY_E))
				m_CameraRotation -= m_CameraRotationSpeed * ts;

			// Wraps the angle into (-180, 180] so it cannot drift without bound.
			//
			// A single-step correction rather than a loop, which is safe because one
			// frame cannot add more than m_CameraRotationSpeed * ts degrees -- at
			// 180 deg/s that needs a frame delta over two seconds to overshoot by
			// enough to need a second subtraction. A tab resumed from the background
			// can deliver exactly such a delta, in which case the angle stays out of
			// range until the next frame corrects it. Cosmetic, not a correctness bug.
			if (m_CameraRotation > 180.0f)
				m_CameraRotation -= 360.0f;
			// The lower-bound mirror. Written <= rather than < so exactly -180 stays
			// put, making the retained range symmetric: (-180, 180].
			else if (m_CameraRotation <= -180.0f)
				m_CameraRotation += 360.0f;

			// Pushed to the camera once per frame, inside the rotation gate. Kept out of
			// the unconditional section below because it is the only per-frame state
			// this class owns that the camera cannot infer for itself.
			m_Camera.SetRotation(m_CameraRotation);
		}

		// The controller's mirrored position pushed to the camera every frame,
		// whether or not it changed.
		//
		// This is the line that reconciles the duplicated state the header warns
		// about: a caller who set a position through GetCamera().SetPosition() has
		// it overwritten here on the very next frame, because m_CameraPosition is
		// the controller's own copy and knows nothing of that change.
		m_Camera.SetPosition(m_CameraPosition);

		// DEFECT: overwrites the tunability every frame, and does so AFTER the
		// moves above it have already used this frame's value.
		//
		// Two problems. (1) It makes m_CameraTranslationSpeed a misnomer: the 5.0f
		// in-class initialiser is dead after the first frame, and pan speed is
		// from then on exactly the zoom level in world units per second -- an
		// undocumented coupling between two settings a user would reasonably expect
		// to be independent. (2) It is assigned after the pan branches read it, so
		// the speed used on any given frame derives from the PREVIOUS frame's
		// zoom. On the frame the wheel zooms, panning uses the old speed: a
		// one-frame lag, invisible at 60 fps and the difference between
		// frame-rate-independent movement and not.
		//
		// Presumably intended to keep panning matched to how much is on screen, so
		// a zoomed-in view pans slower. Defensible as a design, but it belongs in
		// CalculateView next to the zoom change, and it should not be expressed by
		// clobbering a member whose name claims to be a translation speed.
		m_CameraTranslationSpeed = m_ZoomLevel;
	}

	// Single funnel for every event, with the per-type handlers below doing the
	// filtering. The alternative -- registering each handler at the call site --
	// would let a caller add an event without adding its handler here.
	//
	// @param e  The event, by non-const reference because the handlers may mark
	//            it handled.
	void OrthographicCameraController::OnEvent(Event& e)
	{
		WL_PROFILE_FUNCTION();

		// A temporary over one event, constructed per call rather than held as a
		// member. Dispatch is immediate and synchronous, so there is nothing to
		// carry between calls.
		EventDispatcher dispatcher(e);
		// Zoom. Dispatch matches on the event's dynamic type, so this fires for any
		// MouseScrolledEvent regardless of which device scrolled.
		dispatcher.Dispatch<MouseScrolledEvent>(WL_BIND_EVENT_FN(OrthographicCameraController::OnMouseScrolled));
		// Resize, which re-derives the aspect ratio and the bounds.
		//
		// These two are the complete event set. DEFECT: neither handler returns
		// true, so neither marks its event handled, and the dispatcher's return
		// value -- which reports whether any handler consumed the event -- is
		// discarded. Consistent between the two, so it works today, but the
		// controller silently competes with every other listener for the same events
		// instead of claiming them.
		dispatcher.Dispatch<WindowResizeEvent>(WL_BIND_EVENT_FN(OrthographicCameraController::OnWindowResized));
	}

	// Re-derives bounds from a new pixel size, keeping the world-space height
	// and zoom fixed and solving for the width via the aspect ratio.
	//
	// @param width   New viewport width in pixels.
	// @param height  New viewport height in pixels.
	//
	// DEFECT: no divide-by-zero guard. A WindowResizeEvent carrying height == 0
	// -- which a minimise-to-zero on some window managers produces -- makes
	// m_AspectRatio an infinity or a NaN, and that propagates into the bounds
	// and the projection matrix on the next CalculateView().
	void OrthographicCameraController::OnResize(float width, float height)
	{
		// Cached so a later resize need not be handed the ratio again. Written
		// before the CalculateView() below, which reads it -- the order matters
		// here and is correct.
		m_AspectRatio = width / height;
		CalculateView();
	}

	// The single place bounds become a projection. Funnelling every change
	// through one function is what keeps the camera consistent: SetZoomLevel
	// and OnResize both call it rather than touching the projection directly.
	void OrthographicCameraController::CalculateView()
	{
		// Brace-initialises the aggregate in declaration order: Left, Right,
		// Bottom, Top. Aggregates initialise positionally, so this depends on the
		// field order in OrthographicCameraBounds -- swapping two fields there
		// would silently transpose the box rather than fail to compile.
		//
		// This expression is duplicated verbatim in the constructor's initialiser
		// list. That is a second copy of the box formula to keep in step; a helper
		// or a bounds-from-zoom constructor would give it one home.
		m_Bounds = { -m_AspectRatio * m_ZoomLevel, m_AspectRatio * m_ZoomLevel, -m_ZoomLevel, m_ZoomLevel };
		// Pushes the box into the camera.
		//
		// DEFECT: the header's own note on CalculateView records that
		// OrthographicCamera::SetProjection does NOT refresh that class's cached
		// m_ViewProjectionMatrix. So after any zoom or resize,
		// GetViewProjectionMatrix() keeps returning the OLD matrix until something
		// else rebuilds it -- meaning a zoom does not take effect for code reading
		// that cached matrix. This is the stale-cache defect the header refers to;
		// the bug is in the camera rather than here, and whether SetPosition in
		// OnUpdate papers over it depends on that class's internals.
		m_Camera.SetProjection(m_Bounds.Left, m_Bounds.Right, m_Bounds.Bottom, m_Bounds.Top);
	}

	// The wheel-zoom handler. Returns bool, matching the dispatcher's
	// convention for 'handled'.
	//
	// @param e  The scroll event; only GetYOffset is used.
	bool OrthographicCameraController::OnMouseScrolled(MouseScrolledEvent& e)
	{
		WL_PROFILE_FUNCTION();

		// The zoom rate. Subtracting a positive scroll (wheel up, GetYOffset > 0)
		// DECREASES the zoom level, and a smaller level means less world space in
		// view -- so wheel up zooms IN. The sign works out correctly, but it reads
		// backwards: the branch that looks like zooming out is the zoom-in path.
		//
		// The 0.25f is a magic scale factor converting wheel clicks to zoom units.
		m_ZoomLevel -= e.GetYOffset() * 0.25f;
		// Clamps the lower bound only. DEFECT: nothing stops the level reaching 0 or
		// negative through the header's SetZoomLevel, which performs no clamping at
		// all -- as its own comment notes. A non-positive level collapses or inverts
		// the ortho box; this line happens to prevent reaching that state by wheel,
		// and SetZoomLevel is the hole in it.
		m_ZoomLevel = std::max(m_ZoomLevel, 0.25f);
		// Applied immediately rather than deferred to the next OnUpdate, so the new
		// zoom is live before the next frame's draw. SetZoomLevel in the header does
		// the same.
		CalculateView();
		// Does not mark the event handled, consistent with the resize handler --
		// see the note in OnEvent. A scroll left unhandled goes on to other
		// listeners, which is how ImGui and the engine controller can both respond
		// to the same wheel event.
		return false;
	}

	// The resize handler, a thin adapter from the event to OnResize.
	//
	// @param e  The resize event; width and height are cast from int to float
	//            below.
	bool OrthographicCameraController::OnWindowResized(WindowResizeEvent& e)
	{
		WL_PROFILE_FUNCTION();

		// Delegates rather than inlining the two lines above. Explicit casts from
		// int are needed because OnResize takes floats.
		//
		// Inherits OnResize's missing divide-by-zero guard: a zero-height resize
		// corrupts m_AspectRatio here.
		OnResize((float)e.GetWidth(), (float)e.GetHeight());
		// Not marked handled, for the same reason as the scroll handler.
		return false;
	}

}