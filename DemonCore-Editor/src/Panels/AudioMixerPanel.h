// AudioMixerPanel -- the live voice and device view.
//
// This is the only view of what the mixer is ACTUALLY doing: the voice table
// (what is playing right now, how far through, at what gain) and the device
// state (running or not, and at what endpoint rate). The scene hierarchy shows
// sources as components, but a component is an INTENTION to play -- this panel
// shows whether that intention became a voice.
//
// Control surface is deliberately narrow. The mixer is advanced by the device
// callback on the audio thread, so the panel only ever calls the mixer's
// INTERNALLY-LOCKED control methods (StopAll, SetMasterVolume) and never the
// voice table: a write to a voice's gain from the UI thread would race the
// callback. Stop All and the master fader are the two controls that are safe,
// and they are the two this panel offers.
//
// NOTE ON THE AVB BUS GRAPH: it is deliberately NOT shown here. AudioBusGraph
// has no owner in the production scene -- AvbProjectRuntime::RenderBlock takes
// it as a parameter, and the Recorder constructs one for offline renders. A bus
// table drawn here would therefore be a table the panel invented, and a bus
// list that does not match what the runtime is mixing is worse than no list.
// The AVB Graph panel owns that surface; this one covers the device path.
#pragma once

#include <string>
#include <vector>

namespace Wasteland
{

	// The read-only voice and device console, with the two safe controls.
	class AudioMixerPanel
	{
	public:
		AudioMixerPanel() = default;

		void OnImGuiRender(class Scene &scene);

		bool IsVisible() const { return m_Visible; }
		void SetVisible(bool visible) { m_Visible = visible; }

	private:
		// Refresh the snapshot. Called once per frame, before the controls are
		// drawn, so a row reads this frame's numbers rather than last frame's --
		// which matters because the voice table changes on every Play, and a row
		// drawn from a stale snapshot says a finished sound is still playing.
		void Capture(Scene &scene);

		bool m_Visible = false;

		// One-shot, so the panel's first size is applied once rather than every
		// frame: it is re-docked on every layout switch.
		bool m_SizedOnce = false;

		// Retired slots are HIDDEN by default. Mixer::GetVoicesSnapshot returns
		// the whole slot vector, which keeps finished voices so Play can reuse
		// the slot without reallocating -- so the table grows to the loudest
		// moment ever heard and then keeps showing dead rows forever. The toggle
		// exists to inspect that growth, not for day-to-day use.
		bool m_ShowRetired = false;

		// Voice rows are flat rather than a copy of Mixer::Voice: the panel needs
		// seven of Voice's fields, and copying the whole struct would drag Clip
		// (a Ref), Position, distances and fade state along for nothing.
		struct VoiceRow
		{
			int Id = -1;
			std::string Clip;
			float Volume = 0.0f;
			float Pitch = 0.0f;
			float Progress = 0.0f;
			int Priority = 0;
			bool Spatial = false;
			bool Fading = false;
		};
		std::vector<VoiceRow> m_Voices;

		int m_ActiveVoices = 0;
		int m_MaxVoices = 0;
		int m_Dropped = 0;
		int m_Stolen = 0;
		float m_MasterVolume = 1.0f;

		// Device, reported separately from the mixer because a running device
		// with a stopped mixer is the state a "no audio" bug report actually
		// describes. 0 rate means the device is not open -- the endpoint rate is
		// only meaningful once Start() succeeded.
		bool m_DeviceRunning = false;
		unsigned int m_EndpointRate = 0;

		// Listener, for the one spatial fact a mixer view can show: where the
		// ears are. Read from the snapshot accessor, which is locked.
		bool m_HasListener = false;
		float m_ListenerPosition[3] = {0.0f, 0.0f, 0.0f};
	};

}
