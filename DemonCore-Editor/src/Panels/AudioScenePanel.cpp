// AudioScenePanel -- every audio component in the open scene.
//
// See the header for why this exists separately from the hierarchy and the
// properties view. The short version: neither of those answers "where is the
// sound in this level", and an audio workflow is only useful if it can.
//
// The capture is done once per frame over the registry, not cached: a scene's
// component set changes as scripts and tools add entities, and a cached list
// would be a list that disagrees with the scene it claims to describe.
#include "wlpch.h"
#include "AudioScenePanel.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "Wasteland/Audio/AudioSystem.h"
#include "Wasteland/Audio/Mixer.h"
#include "Wasteland/Scene/Components.h"
#include "Wasteland/Scene/Scene.h"

namespace Wasteland
{
	namespace
	{
		// An entity's display name. TagComponent when it has one, otherwise a
		// short UUID fragment -- stable across frames, unlike the entity's index,
		// so a row does not change label when another entity is destroyed.
		std::string EntityLabel(Entity entity)
		{
			if (!entity)
				return "(invalid)";

			if (entity.HasComponent<TagComponent>())
			{
				const std::string &tag = entity.GetComponent<TagComponent>().Tag;
				if (!tag.empty())
					return tag;
			}

			char buffer[40];
			std::snprintf(buffer, sizeof(buffer), "entity %llu",
				static_cast<unsigned long long>(static_cast<uint64_t>(entity.GetUUID())));
			return std::string(buffer);
		}

		// The flags column, compact. Kept in one helper because it is built three
		// ways (listener / clean source / warning source) and the buffer sizing is
		// the part that is easy to get wrong.
		//
		// Hand-rolled rather than strncat: strncat's count argument is the number
		// of SOURCE characters, not the remaining DESTINATION space, so the
		// obvious call is an off-by-one that only bites when the buffer is full --
		// and the flags column is exactly the place a long entity name ends up.
		void AppendFlag(char *buffer, std::size_t capacity, const char *text)
		{
			const std::size_t used = std::strlen(buffer);
			const std::size_t room = capacity - used - 1;
			if (room == 0)
				return;

			char *out = buffer + used;
			std::size_t written = 0;

			// A separating slash when the buffer already holds a flag.
			if (used > 0 && written < room)
				out[written++] = '/';

			for (const char *c = text; *c != '\0' && written < room; ++c)
				out[written++] = *c;

			out[written] = '\0';
		}
	}

	void AudioScenePanel::Capture(Scene &scene)
	{
		m_Rows.clear();
		m_SourceCount = 0;
		m_ListenerCount = 0;
		m_MissingClipCount = 0;

		// The clip registry, read once for the whole pass. A per-row lookup would
		// be the same cost, but a single snapshot also means every row in one
		// frame agrees about which clips exist.
		const auto clips = scene.GetAudioSystem()->GetMixer().GetClipsSnapshot();

		// Sources first, listeners second. Two passes rather than one pass
		// appending both: the order is what makes the panel readable, and a
		// single pass would interleave them by registry order.
		// .each() rather than the bare view: it is the adaptor that yields a tuple
		// of (entity, component&), which is what makes the structured binding
		// below work. The bare view iterates entity handles only.
		for (auto [entityHandle, source] : scene.GetAllEntitiesWith<AudioSourceComponent>().each())
		{
			AudioSourceRow row;
			row.IsListener = false;
			row.Handle = Entity{ entityHandle, &scene };
			row.EntityName = EntityLabel(row.Handle);
			row.ClipName = source.ClipName;
			row.Volume = source.Volume;
			row.Pitch = source.Pitch;
			row.Loop = source.Loop;
			row.Priority = source.Priority;
			row.Spatial = source.Spatial;
			row.MinDistance = source.MinDistance;
			row.MaxDistance = source.MaxDistance;
			row.PlayOnStart = source.PlayOnStart;

			// MaxDistance <= MinDistance is the inverted window. The mixer
			// special-cases it as "no falloff" rather than dividing by a negative
			// width, so it is not a crash -- but it is also not what an author who
			// typed two numbers almost always meant, so it is flagged rather than
			// corrected.
			row.HasDistanceWarning = source.Spatial && source.MaxDistance <= source.MinDistance;

			// A source with a clip name the mixer has never registered. This is
			// the "why is there no sound" case, and it is checkable without
			// playing anything -- which is the entire reason the panel asks the
			// mixer rather than trusting the component.
			row.ClipMissing = !source.ClipName.empty() &&
				clips.find(source.ClipName) == clips.end();

			++m_SourceCount;
			if (row.ClipMissing)
				++m_MissingClipCount;

			m_Rows.push_back(row);
		}

		for (auto [entityHandle, listener] : scene.GetAllEntitiesWith<AudioListenerComponent>().each())
		{
			++m_ListenerCount;

			if (m_HideListeners)
				continue;

			AudioSourceRow row;
			row.IsListener = true;
			row.Handle = Entity{ entityHandle, &scene };
			row.EntityName = EntityLabel(row.Handle);
			// The listener's Enabled, shown in the flags column. A disabled
			// listener is still a listener: it is what the mixer's fallback to
			// the primary camera means in practice.
			row.ListenerEnabled = listener.Enabled;

			m_Rows.push_back(row);
		}

		// Sorted by name so the list is stable frame to frame. Registry order is
		// creation order, which moves whenever an entity is destroyed -- a list
		// that re-sorts itself under the cursor is a list that cannot be read.
		std::stable_sort(m_Rows.begin(), m_Rows.end(),
			[](const AudioSourceRow &a, const AudioSourceRow &b)
			{
				if (a.IsListener != b.IsListener)
					return !a.IsListener;
				return a.EntityName < b.EntityName;
			});
	}

	void AudioScenePanel::OnImGuiRender(Scene &scene)
	{
		if (!m_Visible)
			return;

		Capture(scene);

		if (!m_SizedOnce)
		{
			m_SizedOnce = true;
			ImGui::SetNextWindowSize(ImVec2(440.0f, 420.0f), ImGuiCond_FirstUseEver);
		}

		// Begin's return value is deliberately ignored and End() is always reached.
		// The `if (!Begin(...)) return;` form skips End() when the window is closed
		// or collapsed, and ImGui asserts on the next panel's End() --
		// "Mismatched Begin/BeginChild vs End/EndChild calls". The guard above
		// already early-outs on a closed panel; AvbGraphPanel is the reference
		// for this shape.
		(void)ImGui::Begin("Audio Sources", &m_Visible);

		// ---- Summary -------------------------------------------------------

		// The counts lead, because "how many sources, and how many of them are
		// wired to a clip that does not exist" is the question this panel exists
		// to answer.
		ImGui::Text("sources %d   listeners %d", m_SourceCount, m_ListenerCount);
		if (m_MissingClipCount > 0)
			// Coloured rather than plain text: a source pointing at a clip the
			// mixer has never heard of is the failure this panel is for, and it
			// should not read as a statistic.
			ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f),
				"%d source(s) reference a clip that is not loaded", m_MissingClipCount);
		if (m_SourceCount == 0 && m_ListenerCount == 0)
			ImGui::TextDisabled("no audio components in this scene");

		// ---- List ----------------------------------------------------------

		ImGui::SameLine();
		ImGui::Checkbox("hide listeners", &m_HideListeners);

		if (ImGui::BeginTable("##audio_sources", 6,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("entity");
			ImGui::TableSetupColumn("clip");
			ImGui::TableSetupColumn("vol");
			ImGui::TableSetupColumn("pitch");
			ImGui::TableSetupColumn("pri");
			ImGui::TableSetupColumn("flags");
			ImGui::TableHeadersRow();

			if (m_Rows.empty())
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextDisabled("no audio components in this scene");
			}
			else
			{
				for (const AudioSourceRow &row : m_Rows)
				{
					ImGui::TableNextRow();

					// The entity cell is selectable, and selecting it focuses the
					// entity in the hierarchy. That is the only action offered, and it
					// is the one that belongs here: the component is edited in the
					// properties view, so the panel must not grow a second editor for
					// it. No SpanAllColumns -- the remaining cells of the row are still
					// drawn, and a spanning selectable would consume them.
					ImGui::TableNextColumn();
					{
						const bool selected = m_PendingFocus && m_PendingFocus == row.Handle;
						if (ImGui::Selectable(row.EntityName.c_str(), selected))
						{
							m_PendingFocus = row.Handle;
							m_Status = "focused '" + row.EntityName + "'";
						}
					}

					ImGui::TableNextColumn();
					// A source with no clip is inert by design, which is a valid
					// authoring state -- so it is labelled as such rather than shown as
					// an error. A clip that is NAMED but MISSING is the error.
					if (row.IsListener)
						ImGui::TextDisabled("listener");
					else if (row.ClipName.empty())
						ImGui::TextDisabled("(none)");
					else
						ImGui::Text("%s", row.ClipName.c_str());

					ImGui::TableNextColumn();
					// Listeners carry no source parameters. TableNextColumn rather than
					// an empty cell, so the columns after it stay aligned.
					if (row.IsListener)
						ImGui::TextDisabled("-");
					else
						ImGui::Text("%.2f", row.Volume);

					ImGui::TableNextColumn();
					if (row.IsListener)
						ImGui::TextDisabled("-");
					else
						ImGui::Text("%.2f", row.Pitch);

					ImGui::TableNextColumn();
					if (row.IsListener)
						ImGui::TextDisabled("-");
					else
						ImGui::TextDisabled("%d", row.Priority);

					ImGui::TableNextColumn();
					{
						char flags[48] = {0};
						if (row.IsListener)
						{
							AppendFlag(flags, sizeof(flags), row.ListenerEnabled ? "enabled" : "disabled");
						}
						else
						{
							if (row.Spatial) AppendFlag(flags, sizeof(flags), "3D");
							if (row.Loop) AppendFlag(flags, sizeof(flags), "loop");
							if (row.PlayOnStart) AppendFlag(flags, sizeof(flags), "start");
							if (row.ClipMissing) AppendFlag(flags, sizeof(flags), "MISSING");
							if (row.HasDistanceWarning) AppendFlag(flags, sizeof(flags), "dist");
						}

						// The two warnings are coloured so they read as problems rather
						// than as flags. An empty flags cell is left blank, not "-": a
						// source with no flags has nothing to say.
						if (row.ClipMissing || row.HasDistanceWarning)
							ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "%s", flags);
						else
							ImGui::TextDisabled("%s", flags);
					}
				}
			}

			ImGui::EndTable();
		}

		if (!m_Status.empty())
			ImGui::TextDisabled("%s", m_Status.c_str());

		// End on the only path that opened a window.
		ImGui::End();
	}
}
