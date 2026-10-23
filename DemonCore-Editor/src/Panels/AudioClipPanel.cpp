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
#include "wlpch.h"
#include "AudioClipPanel.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "Wasteland/Audio/AudioSystem.h"
#include "Wasteland/Audio/Mixer.h"
#include "Wasteland/Project/Project.h"
#include "Wasteland/Scene/Scene.h"
#include "Wasteland/Utils/PlatformUtils.h"

namespace Wasteland
{
	namespace
	{
		// Peak to dBFS. Guarded at the bottom: 20*log10(0) is -inf, which ImGui
		// would render as garbage, and digital silence is a real authoring
		// outcome (a wrongly exported file) that deserves a readable number
		// rather than "-inf".
		float PeakToDb(float peak)
		{
			constexpr float kFloor = 0.0000158f; // -96 dBFS
			const float clamped = peak < kFloor ? kFloor : peak;
			return 20.0f * std::log10(clamped);
		}

		// One decimal place, with a trailing ".0" trimmed so a whole second
		// reads as "2 s" rather than "2.0 s".
		std::string FormatSeconds(float seconds)
		{
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "%.1f", seconds);

			std::string out(buffer);
			if (out.size() > 2 && out.compare(out.size() - 2, 2, ".0") == 0)
				out.erase(out.size() - 2);

			return out;
		}
	}

	bool AudioClipPanel::OpenClip(Scene &scene, const std::filesystem::path &path)
	{
		// LoadClip registers under the file's STEM (AudioClip::LoadWav calls
		// SetName with the stem, and AudioSystem::LoadClip registers under
		// GetName), so two files with the same name in different directories
		// collide. That is the mixer's documented behaviour -- a re-register
		// REPLACES the clip -- so a collision is a replace, not an error, and the
		// panel reports the name it got.
		if (!scene.GetAudioSystem()->LoadClip(path.string()))
		{
			m_Status = "Could not load '" + path.filename().string() + "'";
			return false;
		}

		m_SelectedClip = path.stem().string();
		RebuildEnvelope(*scene.GetAudioSystem()->GetMixer().GetClip(m_SelectedClip));
		m_Status = "Loaded '" + m_SelectedClip + "'";
		return true;
	}

	void AudioClipPanel::RebuildEnvelope(const Audio::AudioClip &clip)
	{
		// Cleared first, before any early return: the panel must not keep its
		// last envelope when the clip it belongs to is gone, or a failed load
		// would keep drawing the previous clip's shape.
		m_Envelope.clear();
		m_Peak = 0.0f;
		m_SampleRate = 0;
		m_Duration = 0.0f;
		m_Frames = 0;

		if (!clip.IsValid())
			return;

		const std::vector<float> &samples = clip.GetSamples();
		const std::size_t frameCount = samples.size();

		m_Peak = clip.GetPeak();
		m_SampleRate = clip.GetSampleRate();
		m_Duration = clip.GetDuration();
		m_Frames = frameCount;

		if (frameCount == 0)
			return;

		// Normalise by the clip's own peak when it has one. A peak of exactly 0
		// (digital silence, or a clip of all zeros) is divided around rather
		// than by: the envelope is all zeros either way, and dividing by zero
		// would turn that into NaNs that propagate into every bar's height.
		const float normaliser = m_Peak > 0.0f ? 1.0f / m_Peak : 0.0f;

		m_Envelope.resize(static_cast<std::size_t>(kEnvelopeBuckets) * 2, 0.0f);

		for (int bucket = 0; bucket < kEnvelopeBuckets; ++bucket)
		{
			// Inclusive start, exclusive end, so the buckets tile the clip
			// exactly once -- no frame counted twice at a boundary and none
			// skipped.
			const std::size_t first = static_cast<std::size_t>(
				(static_cast<double>(bucket) * frameCount) / kEnvelopeBuckets);
			const std::size_t last = static_cast<std::size_t>(
				(static_cast<double>(bucket + 1) * frameCount) / kEnvelopeBuckets);

			float minimum = 1.0f;
			float maximum = -1.0f;

			for (std::size_t frame = first; frame < last && frame < frameCount; ++frame)
			{
				const float value = samples[frame] * normaliser;
				if (value < minimum)
					minimum = value;
				if (value > maximum)
					maximum = value;
			}

			// An empty bucket (possible when the clip is shorter than the bucket
			// count) reads as silence at the centre line rather than as a gap:
			// the 1/-1 initialisers above would otherwise draw a full-height
			// bar for a bucket holding no frames at all.
			if (minimum > maximum)
			{
				minimum = 0.0f;
				maximum = 0.0f;
			}

			m_Envelope[bucket * 2] = minimum;
			m_Envelope[bucket * 2 + 1] = maximum;
		}
	}

	void AudioClipPanel::OnImGuiRender(Scene &scene)
	{
		if (!m_Visible)
			return;

		if (!m_SizedOnce)
		{
			m_SizedOnce = true;
			ImGui::SetNextWindowSize(ImVec2(360.0f, 420.0f), ImGuiCond_FirstUseEver);
		}

		// Begin's return value is deliberately ignored and End() is always reached.
		// The `if (!Begin(...)) return;` form skips End() when the window is closed
		// or collapsed, and ImGui asserts on the next panel's End() --
		// "Mismatched Begin/BeginChild vs End/EndChild calls". The guard above
		// already early-outs on a closed panel; AvbGraphPanel is the reference
		// for this shape.
		(void)ImGui::Begin("Audio Clips", &m_Visible);

		const Audio::Mixer &mixer = scene.GetAudioSystem()->GetMixer();

		// ---- Library -------------------------------------------------------

		// Rebuilt from the registry every frame. A small map, and it keeps the
		// list correct after a script or a tool call loads a clip from another
		// thread -- the alternative is a list that disagrees with the mixer.
		m_ClipNames.clear();
		for (const auto &entry : mixer.GetClipsSnapshot())
			m_ClipNames.push_back(entry.first);
		std::sort(m_ClipNames.begin(), m_ClipNames.end());

		if (ImGui::Button("Load WAV"))
		{
			const std::string path = FileDialogs::OpenFile("Wave Audio (*.wav)\0*.wav\0");
			if (!path.empty())
			{
				// Project-relative resolution, matching the content browser: a
				// project can be opened from any directory, so a bare relative
				// path would resolve against the process working directory.
				std::filesystem::path resolved = path;
				if (resolved.is_relative())
					resolved = Project::Get().GetAssetRoot() / resolved;
				OpenClip(scene, resolved);
			}
		}

		// The selected clip is read back from the mixer rather than trusted from
		// m_SelectedClip: the registry is the authority, and a clip re-registered
		// under a different name makes the stored string stale in a way the panel
		// cannot otherwise notice.
		const Audio::AudioClip *selected = nullptr;
		if (!m_SelectedClip.empty())
			selected = mixer.GetClip(m_SelectedClip).get();

		// Name and peak beside the button, so the clip in the waveform is
		// identified without a second header block.
		if (selected)
		{
			// SameLine before the text, or the readout starts a new row. Text
			// rather than TextDisabled: this is live data about a real clip.
			// The name is the mixer's registry key, which is what an
			// AudioSourceComponent's ClipName has to match to play this clip.
			char header[160];
			std::snprintf(header, sizeof(header), "%s  %.1f dBFS",
				m_SelectedClip.c_str(), PeakToDb(selected->GetPeak()));
			// SameLine, so the button and the readout share the toolbar row.
			ImGui::SameLine();
			ImGui::Text("%s", header);
		}
		else
		{
			// Disabled is the right call: there is no clip, so the panel is
			// saying what it is waiting for rather than reporting a value.
			// SameLine keeps the row layout identical with and without a
			// selection, so the panel does not reflow when a clip is chosen.
			ImGui::SameLine();
			ImGui::TextDisabled("no clip selected");
		}

		// ---- Clip list -----------------------------------------------------

		const float listHeight = 5.0f * ImGui::GetTextLineHeightWithSpacing();
		if (ImGui::BeginListBox("##audio_clip_list", ImVec2(-1.0f, listHeight)))
		{
			if (m_ClipNames.empty())
			{
				// A disabled row rather than empty space: an empty list box reads
				// as a panel that has not loaded yet.
				ImGui::Selectable("No clips loaded", false, ImGuiSelectableFlags_Disabled);
			}
			else
			{
				for (const std::string &name : m_ClipNames)
				{
					const bool isSelected = (name == m_SelectedClip);
					if (ImGui::Selectable(name.c_str(), isSelected))
					{
						m_SelectedClip = name;
						RebuildEnvelope(*mixer.GetClip(name));
					}
				}
			}

			// EndListBox is required by BeginListBox on every path, including the
			// empty one.
			ImGui::EndListBox();
		}

		// ---- Waveform ------------------------------------------------------

		// A child rather than a bare cursor offset: it gives the bars a clipping
		// rectangle and advances the cursor past the graph, so the readout below
		// sits under it instead of on top of it. No border and no scrollbar, so
		// the graph runs edge to edge.
		const float graphHeight = std::max(48.0f, ImGui::GetContentRegionAvail().y);

		if (ImGui::BeginChild("##audio_waveform", ImVec2(-1.0f, graphHeight), false, ImGuiWindowFlags_NoScrollbar))
		{
			// Drawn first, so the bars below cannot cover it. Only reached when
			// there is nothing to draw, so the two never overlap.
			if (!selected || m_Envelope.empty())
				ImGui::TextDisabled("Load a .wav to see its waveform");

			ImDrawList *drawList = ImGui::GetWindowDrawList();
			const ImVec2 topLeft = ImGui::GetWindowPos();
			const ImVec2 size = ImGui::GetWindowSize();
			const float mid = topLeft.y + size.y * 0.5f;

			if (selected && !m_Envelope.empty())
			{
				const float barWidth = size.x / static_cast<float>(kEnvelopeBuckets);
				const float halfHeight = size.y * 0.5f;

				for (int bucket = 0; bucket < kEnvelopeBuckets; ++bucket)
				{
					const float minimum = m_Envelope[bucket * 2];
					const float maximum = m_Envelope[bucket * 2 + 1];

					const float left = topLeft.x + bucket * barWidth;
					const float right = left + std::max(1.0f, barWidth - 1.0f);
					const float top = mid - maximum * halfHeight;
					const float bottom = mid - minimum * halfHeight;

					// A fixed neutral grey rather than the accent: the waveform
					// is data, and the accent is reserved for selection. A bar a
					// hair narrower than its slot leaves a gap so adjacent bars
					// stay separable at small widths.
					drawList->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom),
						IM_COL32(160, 170, 185, 255));
				}
			}

			// The zero axis, drawn over the bars so it stays visible when the
			// waveform is dense. In the empty case it is the only mark on the
			// graph, which is the honest picture: there is no envelope to show.
			drawList->AddRectFilled(ImVec2(topLeft.x, mid),
				ImVec2(topLeft.x + size.x, mid + 1.0f),
				IM_COL32(70, 75, 85, 255));

			ImGui::EndChild();
		}

		// ---- Readout -------------------------------------------------------

		if (selected)
		{
			// Frames, rate and duration are what a sound designer needs before
			// placing a clip on a timeline. Peak is on the toolbar row above,
			// next to the name.
			ImGui::Text("%llu frames  %.0f Hz  %s s",
				static_cast<unsigned long long>(m_Frames),
				static_cast<float>(m_SampleRate), FormatSeconds(m_Duration).c_str());
		}

		if (!m_Status.empty())
			ImGui::TextDisabled("%s", m_Status.c_str());

		// End on the only path that opened a window.
		ImGui::End();
	}
}
