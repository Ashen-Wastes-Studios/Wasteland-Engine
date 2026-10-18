// The engine umbrella. This is the editor's entry-point translation unit, so
// it pulls in the application scaffolding wholesale rather than naming
// individual headers.
#include <Wasteland.h>
// Declares CreateApplication(), which must exist with exactly this
// signature and C++ linkage for the engine's own main() to find it. The
// definition at the bottom of this file is the whole reason the file
// exists.
#include "Wasteland/Core/EntryPoint.h"

// The editor's own layer -- panels, menu bar, docking. Not in namespace
// Wasteland (the include path has no namespace prefix in it), which is worth
// noting because everything else here is inside namespace Wasteland.
#include "EditorLayer.h"

// For std::getenv and std::atoi in the AutoSwitchLayer test hook below.
#include <cstdlib>

namespace Wasteland {

	// Test hook: WL_AUTO_SWITCH_TO=N flips the renderer to API N a few
	// frames after startup (2=DX11, 3=DX12, 4=Vulkan), WL_AUTO_SWITCH_QUIT
	// closes the app that many frames later. Hands-free reproduction of
	// Settings-combo backend switches.
	// A test-only Layer that drives renderer-backend switching on a schedule,
	// so the switch path can be exercised hands-free.
	//
	// Defined here, in the entry-point file, rather than in the editor sources:
	// it exists only when the environment variables are set, and keeping it out
	// of the editor's own sources means the shipped editor carries no test
	// hook. The cost is a Layer subclass in a file that otherwise only
	// bootstraps the app.
	class AutoSwitchLayer : public Layer
	{
	public:
		// Takes the target API and when to quit, plus an optional second switch.
		//
		// @param api        API to switch to on frame 5.
		// @param quitFrames Frame to close the app on; 0 means never quit.
		// @param api2       Optional second API; None disables the second switch.
		// @param api2Frame  Frame for that second switch.
		AutoSwitchLayer(RendererAPI::API api, int quitFrames,
			// Defaults for the optional parameters, matching the members' own defaults
			// below. Duplicated rather than relying on the initialisers, because a
			// constructor parameter default has to restate it -- there is no way to
			// say 'same as the member'.
			RendererAPI::API api2 = RendererAPI::API::None, int api2Frame = 60)
			// All four members initialised explicitly, in declaration order, so no
			// in-class default can mask a passed-in value.
			//
			// Empty body: the layer does nothing until OnUpdate starts counting frames.
			: m_API(api), m_QuitFrames(quitFrames), m_API2(api2), m_API2Frame(api2Frame) {}
		// The per-frame hook. The Timestep parameter is unnamed -- this layer has no
		// time-dependent behaviour, only a frame count.
		//
		// DEFECT: called with `==` comparisons below, so it depends on frames
		// arriving in strict sequence with none skipped. Any frame in which this
		// layer is not updated -- a layer removed and re-added, a debugger step over
		// a frame -- means the equality never matches and that switch never fires,
		// silently. `>=` would make the schedule robust to skipped frames.
		void OnUpdate(Timestep) override
		{
			// Frame counter, starting from 0. It is incremented before the comparisons,
			// so the first OnUpdate leaves m_Frames at 1 and frame 1 is never a
			// switch point -- deliberate, since switching on the very first frame would
			// be before anything is rendered.
			m_Frames++;
			// The first switch, hardcoded to frame 5. Early enough that there is
			// something on screen to switch away from, late enough that startup and
			// device creation have completed.
			if (m_Frames == 5)
				// The switch itself -- the same call the Settings combo makes, which is the
				// point: this exercises the real path rather than a test-only one.
				//
				// Return value ignored. If SwitchRendererAPI reports failure, this layer
				// cannot tell and the app carries on with the old backend, which is exactly
				// the case an automated reproduction most needs to notice.
				Application::Get().GetWindow().SwitchRendererAPI(m_API);
			// The optional second switch, for exercising a switch that is not the first
			// one -- the case where old resources still exist and the API is already
			// live, which is a different code path in the window implementation.
			if (m_API2 != RendererAPI::API::None && m_Frames == m_API2Frame)
				Application::Get().GetWindow().SwitchRendererAPI(m_API2);
			// Quit, if a frame was configured. The > 0 test makes 0 mean
			// "run forever", which is what the environment parsing below
			// relies on when WL_AUTO_SWITCH_QUIT is unset.
			//
			// The second comparison is >=, not ==, so closing repeatedly
			// after the threshold is harmless. This layer is also the
			// only place in the file that uses >= for a schedule, where
			// the two switches above use == -- see the note on
			// OnUpdate about skipped frames.
			if (m_QuitFrames > 0 && m_Frames >= m_QuitFrames)
				// Requests shutdown. Called every frame past the threshold, since there is
				// no flag preventing repeats -- Close() must be idempotent for this to be
				// safe, which is an unstated requirement on Application::Close.
				Application::Get().Close();
		}
	private:
		// The first target API. No in-class default, because the constructor always
		// supplies it.
		RendererAPI::API m_API;
		// The optional second target. Defaulted here so the constructor parameter's
		// default and this agree.
		RendererAPI::API m_API2 = RendererAPI::API::None;
		// When to perform that second switch. 60 frames is roughly one second at
		// 60 fps -- long enough that the first switch has settled. A fixed frame
		// count rather than a time means it lands at different wall-clock times on
		// different hardware.
		int m_API2Frame = 60;
		// When to quit; 0 means never. No default, unlike m_API2 -- asymmetric with
		// m_API2 above, though both are always passed explicitly, so the difference
		// is only stylistic.
		int m_QuitFrames;
		// The counter itself, zero-initialised so the first comparison sees a real
		// value rather than garbage.
		int m_Frames = 0;
	};

	// The editor application. Trivial apart from pushing its layers in the
	// constructor -- all real behaviour lives in EditorLayer.
	class DemonCoreEditor : public Application
	{
	public:
		// Constructed by CreateApplication() below. The only work is layer setup,
		// so there is no other initialisation to speak of.
		DemonCoreEditor()
			// The window title, and the only argument passed to the base. The base
			// takes no renderer API here, which is why the API switch has to go through
			// SwitchRendererAPI after the fact rather than being chosen at startup.
			: Application("DemonCore Editor")
		{
			// The editor's layer, pushed first so it updates before AutoSwitchLayer --
			// layers update in push order, so the test hook sees the editor's state
			// already advanced.
			PushLayer(new EditorLayer());
			// The test hook's gate: with the variable unset (the normal case) none of
			// the following runs and the editor behaves normally.
			//
			// Declared in the if-init so the getenv result is scoped to the block -- no
			// separate variable, and no null check needed because the condition already
			// established it.
			if (const char *sw = std::getenv("WL_AUTO_SWITCH_TO"))
			{
				// Frame to quit on, defaulting to 0 = never. Chosen over -1 so the AutoSwitchLayer
				// test's `> 0` check reads as 'quit was requested'.
				int quit = 0;
				// Optional second gate. Absent means the switch runs and the app keeps
				// running, which is what makes it useful for watching the switch without
				// the editor vanishing.
				if (const char *q = std::getenv("WL_AUTO_SWITCH_QUIT"))
					// atoi, not strtol. DEFECT: no error checking -- a non-numeric value yields
					// 0, which silently means 'never quit', so a typo in the variable turns
					// into a hung run rather than a reported error.
					quit = std::atoi(q);
				// The optional second API, matching the layer's parameter default.
				RendererAPI::API api2 = RendererAPI::API::None;
				// And its frame, duplicating the layer's own default of 60 -- the same
				// value in two places, so changing one does not change the other.
				int api2Frame = 60;
				// A third and fourth variable for the second switch. Four environment
				// variables for one test hook, each with a default, which is a lot of
				// surface for something the class comment above describes in two lines.
				if (const char *sw2 = std::getenv("WL_AUTO_SWITCH_TO2"))
				{
					// The int-to-enum cast is unchecked. DEFECT: any value outside 0..4
					// produces an enum with no corresponding case in the switches over
					// RendererAPI::API -- an out-of-range value would reach SwitchRendererAPI
					// and hit whatever that function does with an unrecognised API, most
					// plausibly its own assert. The comment at the top of the file lists the
					// valid values (2, 3, 4), but nothing enforces it.
					api2 = (RendererAPI::API)std::atoi(sw2);
					// Overrides when the second switch happens. Read only if TO2 is set, so it
					// cannot be used on its own.
					if (const char *f = std::getenv("WL_AUTO_SWITCH_TO2_FRAME"))
						api2Frame = std::atoi(f);
				}
				// Pushed second, so it updates after the editor -- and being an overlay of
				// the frame counter, it sees the editor's work already done for that frame.
				//
				// The raw-pointer new is handed straight to PushLayer, which owns it. The
				// C-style cast on the first argument has the same unchecked-enum problem as
				// the one above.
				PushLayer(new AutoSwitchLayer((RendererAPI::API)std::atoi(sw), quit, api2, api2Frame));
			}
		}

		// Empty. The base Application's destructor handles shutdown, and the layers
		// pushed above are owned by the base -- so nothing here is needed.

		// The empty body below is the whole destructor: the base
		// Application's destructor handles shutdown and the pushed
		// layers are owned by the base, so nothing is needed here.
		~DemonCoreEditor()
		{

		}

	};

	// The engine's entry point, declared in Core/EntryPoint.h. The engine's own
	// main() calls this once at startup.
	//
	// Returns a raw pointer because that is the signature the engine expects;
	// ownership passes to the caller. Not exception-safe and not a factory in
	// any other sense -- a single hardcoded subclass, so there is no
	// registration mechanism and no way to substitute a different Application.
	Application* CreateApplication()
	{
		// The one and only application type this binary can start.
		//
		// DEFECT: raw new with no matching delete anywhere in this file. It relies
		// on the engine's main() taking ownership and deleting it after the run
		// loop -- which is the conventional contract, but nothing here enforces or
		// even documents it. A leak on any shutdown path that skips the delete.
		return new DemonCoreEditor();
	}

}
