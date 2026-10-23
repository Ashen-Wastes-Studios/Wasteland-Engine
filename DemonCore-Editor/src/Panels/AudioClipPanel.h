// AudioClipPanel -- the clip library and waveform.
//
// The only panel in the editor that touches AudioClip directly, because a clip
// is the one audio object with real samples behind it: the mixer's voice table
// is live DSP state, but a clip's buffer is already a const vector<float> and
// can be drawn.
//
// The panel is deliberately read-only about samples. It draws a MIN/MAX envelope
// -- one bar per bucket -- rather than every sample, which is what makes a
// multi-minute clip drawable at all.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "Wasteland/Audio/AudioClip.h"

namespace Wasteland
{

	// The library and waveform. Owns no AudioSystem: clips are loaded through
	// the scene's own AudioSystem (Scene::GetAudioSystem), so there is exactly
	// one registry and the editor cannot disagree with the runtime about which
	// clips exist.
	class AudioClipPanel
	{
	public:
		AudioClipPanel() = default;

		// The draw entry, matching every other editor panel. Takes the scene
		// rather than storing it: the panel is owned by the layer, which owns
		// the scene, and a stored Ref would pin the scene past a project swap.
		void OnImGuiRender(class Scene &scene);

		bool IsVisible() const { return m_Visible; }
		void SetVisible(bool visible) { m_Visible = visible; }

		// The clip the waveform is showing, or empty. Set by clicking a row in
		// the library, and by OpenClip. Empty means "nothing selected", which is
		// a valid authoring state rather than an error.
		const std::string &GetSelectedClip() const { return m_SelectedClip; }

		// Load a clip from a path into the scene's mixer and make it the
		// selected clip. Returns false with a reason when the file cannot be
		// read -- the caller shows it, the panel does not pop dialogs.
		bool OpenClip(Scene &scene, const std::filesystem::path &path);

	private:
		// Envelope buckets for the waveform. Rebuilt when the selection changes
		// rather than per frame: a clip's samples do not change under us, so a
		// per-frame rebuild would redo the same O(samples) work forever.
		void RebuildEnvelope(const Audio::AudioClip &clip);

		bool m_Visible = false;

		// One-shot, not a per-frame SetNextWindowSize: the panel is re-docked on
		// every layout switch, and an unconditional size would fight the dock node
		// for space on every frame after the first.
		bool m_SizedOnce = false;

		// Names only, not Refs: the mixer's registry already owns the clips and
		// this list must not keep a clip alive after it is re-registered. Sorted
		// for a stable, scan-friendly list.
		std::vector<std::string> m_ClipNames;
		std::string m_SelectedClip;

		// The waveform: kEnvelopeBuckets MIN/MAX pairs over the clip, as
		// fractions of the clip's own peak. Normalising to the peak is what
		// makes a quiet clip visible at all -- it means a clip peaking at 0.02
		// draws as loudly as one at 0.9, and the row's peak readout is the only
		// place the real level shows. An empty envelope means no clip selected,
		// which is distinct from a real all-zero (silent) clip.
		static constexpr int kEnvelopeBuckets = 512;
		std::vector<float> m_Envelope;
		float m_Peak = 0.0f;
		int m_SampleRate = 0;
		float m_Duration = 0.0f;
		// Frame count, kept so the header can report a length without the
		// caller re-deriving it from the rate and the duration.
		std::size_t m_Frames = 0;

		// Status line from the last action: a failed load must say why, and a
		// status that only cleared on success would go stale.
		std::string m_Status;
	};

}
