// AudioMixerPanel -- the live voice and device view.
//
// The mixer is the one audio system with state that changes every frame, so
// this panel is a snapshot view: Capture() copies what it needs under the
// mixer's own lock, and every control below reads the copy. Nothing here reads
// the live mixer except Capture().
#include "wlpch.h"
#include "AudioMixerPanel.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "Wasteland/Audio/AudioSystem.h"
#include "Wasteland/Audio/Mixer.h"
#include "Wasteland/Scene/Scene.h"

namespace Wasteland
{
	namespace
	{
		// 0..1 gain to dB. Floored rather than allowed to reach -inf, for the
		// same reason as the clip panel: ImGui renders -inf as garbage, and a
		// muted channel is a real state that deserves a number.
		float GainToDb(float gain)
		{
			constexpr float kFloor = 0.0000158f; // -96 dB
			const float clamped = gain < kFloor ? kFloor : gain;
			return 20.0f * std::log10(clamped);
		}

		// Level bar colour. Green through amber to red, on a fixed ramp rather
		// than ImGui's accent, so the colour reads as a LEVEL and not as
		// selection. The ramp is deliberately conservative: only the top of the
		// scale goes red, because the mixer clamps every sample to +/-1 and a mix
		// sitting near full scale is the warning, not the norm.
		ImU32 LevelColour(float level)
		{
			const float t = level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);

			if (t < 0.6f)
			{
				// Green, with the green channel easing down as level rises.
				const float g = 190.0f - 70.0f * (t / 0.6f);
				return IM_COL32(90, static_cast<int>(g), 110, 255);
			}
			if (t < 0.85f)
			{
				// Amber.
				return IM_COL32(210, 170, 80, 255);
			}
			return IM_COL32(215, 80, 70, 255);
		}

		// Gain to a readable dB string. -inf is spelled out rather than left as
		// a float: a muted master is a normal state, and "-inf dB" is how a
		// mixer is expected to report it.
		std::string GainToDbString(float gain)
		{
			const float db = GainToDb(gain);
			if (db <= -96.0f)
				return "-inf dB";

			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%.1f dB", db);
			return std::string(buffer);
		}
	}

	void AudioMixerPanel::Capture(Scene &scene)
	{
		Audio::AudioSystem *system = scene.GetAudioSystem().get();
		if (!system)
		{
			// Defensive rather than expected: Scene::GetAudioSystem() lazily
			// creates the system, so this is unreachable in practice. Cleared
			// rather than left stale, so the panel cannot show last frame's
			// numbers over a missing system.
			m_Voices.clear();
			m_ActiveVoices = 0;
			m_MaxVoices = 0;
			m_MasterVolume = 1.0f;
			m_DeviceRunning = false;
			m_EndpointRate = 0;
			m_HasListener = false;
			return;
		}

		const Audio::Mixer &mixer = system->GetMixer();

		m_DeviceRunning = system->IsOutputRunning();
		m_EndpointRate = system->GetOutputEndpointSampleRate();

		m_ActiveVoices = mixer.GetActiveVoiceCount();
		m_MaxVoices = mixer.GetMaxVoices();
		m_Dropped = mixer.GetDroppedVoiceCount();
		m_Stolen = mixer.GetStealedVoiceCount();
		m_MasterVolume = mixer.GetMasterVolume();

		m_HasListener = true;
		const Audio::Mixer::Listener listener = mixer.GetListenerSnapshot();
		m_ListenerPosition[0] = listener.Position.x;
		m_ListenerPosition[1] = listener.Position.y;
		m_ListenerPosition[2] = listener.Position.z;

		m_Voices.clear();
		for (const Audio::Voice &voice : mixer.GetVoicesSnapshot())
		{
			// Retired slots are dropped here unless asked for. The mixer keeps
			// them so Play can reuse the slot, which is a memory win and a
			// display trap: without this filter the table would grow to the
			// loudest moment ever heard and never shrink.
			if (!voice.IsPlaying() && !m_ShowRetired)
				continue;

			VoiceRow row;
			row.Id = voice.VoiceId;
			row.Volume = voice.Volume;
			row.Pitch = voice.Pitch;
			row.Priority = voice.Priority;
			row.Spatial = voice.Spatial;
			row.Fading = voice.FadeRemaining > 0.0f;

			// The clip name, and the progress through it. Both read the clip,
			// which the snapshot copied by Ref -- so the clip outlives the row
			// even if it is re-registered a frame later.
			if (voice.Clip)
			{
				row.Clip = voice.Clip->GetName();

				const std::size_t frames = voice.Clip->GetFrameCount();
				row.Progress = frames > 0
					? static_cast<float>(voice.FramePosition / static_cast<double>(frames))
					: 0.0f;
			}
			else
			{
				// A playing voice with no clip is not a state the mixer should
				// produce, but the snapshot is a copy taken while the audio
				// thread may be retiring a voice, so it is handled rather than
				// assumed away. The row is labelled "(no clip)" instead of
				// silently showing an empty name.
				row.Clip = "(no clip)";
			}

			m_Voices.push_back(row);
		}

		// Highest priority first, then highest id, so the most important voice
		// in the scene is the first row. std::stable_sort so two voices at the
		// same priority keep the mixer's own order.
		std::stable_sort(m_Voices.begin(), m_Voices.end(),
			[](const VoiceRow &a, const VoiceRow &b)
			{
				if (a.Priority != b.Priority)
					return a.Priority > b.Priority;
				return a.Id > b.Id;
			});
	}

	void AudioMixerPanel::OnImGuiRender(Scene &scene)
	{
		if (!m_Visible)
			return;

		// Capture BEFORE the controls, so every number below is this frame's.
		// Done even when the panel is collapsed: it is one small map copy and
		// keeps the first expanded frame from showing stale state.
		Capture(scene);

		if (!m_SizedOnce)
		{
			m_SizedOnce = true;
			ImGui::SetNextWindowSize(ImVec2(420.0f, 460.0f), ImGuiCond_FirstUseEver);
		}

		// Begin's return value is deliberately ignored and End() is always reached.
		// The `if (!Begin(...)) return;` form skips End() when the window is closed
		// or collapsed, and ImGui asserts on the next panel's End() --
		// "Mismatched Begin/BeginChild vs End/EndChild calls". The guard above
		// already early-outs on a closed panel; AvbGraphPanel is the reference
		// for this shape.
		(void)ImGui::Begin("Audio Mixer", &m_Visible);

		// ---- Device -------------------------------------------------------

		// Running state and endpoint rate are the first thing to check when
		// audio is reported as missing, so they lead the panel.
		ImGui::TextDisabled("device: %s   endpoint: %u Hz   mix rate: %d Hz",
			m_DeviceRunning ? "running" : "stopped",
			m_EndpointRate, Audio::Mixer::kEngineMixRate);
		if (!m_DeviceRunning)
			ImGui::TextDisabled("output is stopped: the mixer still mixes, but nothing is heard");

		// ---- Master --------------------------------------------------------

		// The one gain control. SetMasterVolume is internally locked, so this
		// is safe from the UI thread -- unlike anything touching the voice
		// table, which the audio thread advances without a lock held for us.
		ImGui::Text("Master");
		ImGui::SameLine();
		ImGui::PushItemWidth(180.0f);
		float master = m_MasterVolume;
		if (ImGui::SliderFloat("##audio_master", &master, 0.0f, 1.0f, "%.2f"))
			scene.GetAudioSystem()->SetMasterVolume(master);
		ImGui::PopItemWidth();
		ImGui::SameLine();
		ImGui::TextDisabled("%s", GainToDbString(m_MasterVolume).c_str());

		// ---- Counters ------------------------------------------------------

		// Active vs cap, and the two lifetime counters. Dropped and stolen are
		// the numbers to watch: a non-zero dropped count means the voice cap is
		// being hit, which is exactly the case the priority field exists for.
		ImGui::Text("voices %d / %d   dropped %d   stolen %d",
			m_ActiveVoices, m_MaxVoices, m_Dropped, m_Stolen);
		if (m_Dropped > 0)
			ImGui::TextDisabled("dropped voices: raise Priority on sounds that must be heard");
		if (m_HasListener)
			ImGui::TextDisabled("listener %.1f %.1f %.1f",
				m_ListenerPosition[0], m_ListenerPosition[1], m_ListenerPosition[2]);

		// ---- Voice table ---------------------------------------------------

		ImGui::Separator();
		ImGui::Text("Voices");
		ImGui::SameLine();
		ImGui::Checkbox("show retired slots", &m_ShowRetired);
		ImGui::SameLine();
		// StopAll is internally locked, so it is safe here. A 0.15 s fade
		// rather than a hard cut: cutting every voice dead on a click is the
		// audible version of a bug, and a short fade is what a sound designer
		// means by "stop".
		if (ImGui::Button("Stop all"))
			scene.GetAudioSystem()->StopAll(0.15f);

		if (ImGui::BeginTable("##audio_voices", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("id");
			ImGui::TableSetupColumn("clip");
			ImGui::TableSetupColumn("vol");
			ImGui::TableSetupColumn("pitch");
			ImGui::TableSetupColumn("prog");
			ImGui::TableSetupColumn("pri");
			ImGui::TableSetupColumn("flags");
			ImGui::TableHeadersRow();

			if (m_Voices.empty())
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				// Spanning one column rather than a merged header: 1.89 has no
				// merge helper, and a single-column empty message is enough.
				ImGui::TextDisabled("nothing playing");
			}
			else
			{
				for (const VoiceRow &voice : m_Voices)
				{
					ImGui::TableNextRow();

					ImGui::TableNextColumn();
					ImGui::TextDisabled("%d", voice.Id);

					ImGui::TableNextColumn();
					ImGui::Text("%s", voice.Clip.c_str());

					ImGui::TableNextColumn();
					// Level bar behind the number, so a row can be read by scanning
					// the column as well as by reading it.
					{
						const float gain = voice.Volume;
						const ImVec2 barPos = ImGui::GetCursorScreenPos();
						const float rowHeight = ImGui::GetTextLineHeight();
						ImGui::GetWindowDrawList()->AddRectFilled(
							ImVec2(barPos.x, barPos.y),
							ImVec2(barPos.x + (ImGui::GetColumnWidth() - 4.0f) * std::min(1.0f, gain), barPos.y + rowHeight),
							LevelColour(gain));
						ImGui::Text("%.2f", gain);
					}

					ImGui::TableNextColumn();
					ImGui::Text("%.2f", voice.Pitch);

					ImGui::TableNextColumn();
					// Progress as a bar rather than a bare fraction: the shape of a
					// voice's lifetime is the thing worth seeing at a glance.
					{
						const float t = voice.Progress < 0.0f ? 0.0f : (voice.Progress > 1.0f ? 1.0f : voice.Progress);
						ImGui::ProgressBar(t, ImVec2(-1.0f, 0.0f), "");
					}

					ImGui::TableNextColumn();
					ImGui::TextDisabled("%d", voice.Priority);

					ImGui::TableNextColumn();
					// Flags, compact: 3D sources and fading voices are the two states
					// that change how a voice is expected to behave.
					{
						char flags[8] = {0};
						if (voice.Spatial) std::snprintf(flags, sizeof(flags), "3D");
						// Bounded appends rather than strncat: the count argument is the
						// number of SOURCE characters, not remaining DESTINATION space,
						// so the obvious call overflows by one when the buffer is full.
						if (voice.Fading)
						{
							const std::size_t used = std::strlen(flags);
							std::snprintf(flags + used, sizeof(flags) - used, "%sfade",
								voice.Spatial ? "/" : "");
						}
						ImGui::TextDisabled("%s", flags);
					}
				}
			}

			ImGui::EndTable();
		}

		// End on the only path that opened a window.
		ImGui::End();
	}
}
