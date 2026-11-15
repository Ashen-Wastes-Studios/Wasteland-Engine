#include "AnimatorPanel.h"

#include "wlpch.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Wasteland/Animation/AnimationClip.h"
#include "Wasteland/Animation/AnimationStateMachine.h"

namespace Wasteland
{
	// -- field helpers -----------------------------------------------------

	std::vector<char> &AnimatorPanel::TextFieldBuffer(const std::string &key,
													  const std::string &seed)
	{
		auto it = m_TextBuffers.find(key);
		if (it != m_TextBuffers.end())
			return it->second;

		std::vector<char> buffer(seed.begin(), seed.end());
		// ImGui writes a null terminator; two spare slots mean a resize during
		// typing does not invalidate the pointer ImGui is holding.
		buffer.push_back('\0');
		buffer.push_back('\0');
		return m_TextBuffers.emplace(key, std::move(buffer)).first->second;
	}

	bool AnimatorPanel::DrawFloatField(const char *label, float &value)
	{
		char buffer[64];
		std::snprintf(buffer, sizeof(buffer), "%.4f", value);

		// A local buffer is safe: ImGui keeps the pointer only until the widget
		// returns, and no map entry is needed for a value type.
		bool committed = ImGui::InputText(label, buffer, sizeof(buffer),
										 ImGuiInputTextFlags_EnterReturnsTrue);
		// Focus loss is a commit too, otherwise clicking away from a field that
		// was typed but never confirmed would silently discard the edit.
		if (!committed && ImGui::IsItemDeactivatedAfterEdit())
			committed = true;

		if (!committed)
			return false;

		char *end = nullptr;
		const float parsed = std::strtof(buffer, &end);
		if (end == buffer)
		{
			ApplyStatus(std::string("Could not read a number from '") + buffer + "'.");
			return false;
		}
		while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end)))
			++end;
		if (*end != '\0')
		{
			ApplyStatus(std::string("Could not read a number from '") + buffer + "'.");
			return false;
		}

		// An unchanged value is not an edit. Returning true for one would mark the
		// asset dirty on an unrelated click.
		if (parsed == value)
			return false;

		value = parsed;
		return true;
	}

	// -- construction / asset lifecycle ------------------------------------

	void AnimatorPanel::Reset()
	{
		m_HasAsset = false;
		m_Asset = Animation::AnimationAsset{};
		m_AssetPath.clear();
		m_SelectedState.clear();
		m_SelectedTransition = -1;
		m_TextBuffers.clear();
		m_Status.clear();
		m_StatusFrames = 0;
		m_FramedOnce = false;
	}

	bool AnimatorPanel::OpenAsset(const std::filesystem::path &path,
								  const std::filesystem::path &assetRoot)
	{
		Animation::AnimationError error;
		Animation::AnimationAsset asset;
		if (!Animation::AnimationSerializer::Load(path.string(), asset, error))
		{
			// Failure leaves the PREVIOUS asset on screen. Clearing it would make
			// the panel look empty while the file it was showing is still open,
			// and the user would have no way to tell "load failed" from "this
			// clip has no state machine".
			m_Status = "Load failed: " + error.Message;
			return false;
		}

		m_Asset = std::move(asset);
		m_AssetPath = path;
		m_AssetRoot = assetRoot;
		m_HasAsset = true;
		m_SelectedState.clear();
		m_SelectedTransition = -1;
		m_TextBuffers.clear();
		m_FramedOnce = false;
		m_Status = "Loaded " + path.filename().string();
		return true;
	}

	void AnimatorPanel::SetAssetRoot(const std::filesystem::path &root)
	{
		m_AssetRoot = root;
	}

	bool AnimatorPanel::SaveAsset()
	{
		if (!m_HasAsset)
		{
			m_Status = "Nothing to save: open a .wlanim first.";
			return false;
		}
		Animation::AnimationError error;
		if (!Animation::AnimationSerializer::Save(m_AssetPath.string(), m_Asset, error))
		{
			m_Status = "Save failed: " + error.Message;
			return false;
		}
		m_Status = "Saved " + m_AssetPath.filename().string();
		return true;
	}

	void AnimatorPanel::ApplyStatus(const std::string &message)
	{
		m_Status = message;
		// ~5 seconds at 60 fps. Long enough to read a validation message, short
		// enough that the strip gives the canvas its line back.
		m_StatusFrames = 300;
	}

	// -- presentation ------------------------------------------------------

	void AnimatorPanel::RebuildPresentation()
	{
		m_Presentation.Nodes.clear();

		if (!m_HasAsset || !m_Asset.HasStateMachine)
			return;

		const Animation::AnimationStateMachine &machine = m_Asset.StateMachine;

		// State order is sorted by name. m_States is an unordered_map, so the
		// order would otherwise change between frames and the canvas would
		// reshuffle its nodes while the user is dragging one.
		std::vector<std::string> names;
		names.reserve(machine.GetStates().size());
		for (const auto &entry : machine.GetStates())
			names.push_back(entry.first);
		std::sort(names.begin(), names.end());

		for (std::size_t index = 0; index < names.size(); ++index)
		{
			const std::string &name = names[index];
			const auto stateIt = machine.GetStates().find(name);
			if (stateIt == machine.GetStates().end())
				continue;
			const Animation::AnimationState *state = &stateIt->second;
			if (!state)
				continue;

			NodeCanvas::Node node;
			node.Id = name;
			node.Label = name;
			// TypeKnown=false means "unknown node type" in NodeCanvas's vocabulary,
			// which renders as broken. A state with no clip is a valid authoring
			// state that simply will not play, so it is annotated in the label
			// instead of being marked unknown.
			node.TypeKnown = true;

			// A missing entry means "never positioned": lay it out on a loose grid
			// rather than at the origin, where it would sit behind every other
			// node. The index is the sorted position, so two never-positioned
			// states never overlap.
			const auto positioned = m_Asset.StatePositions.find(name);
			if (positioned != m_Asset.StatePositions.end())
			{
				node.Position = { positioned->second.X, positioned->second.Y };
			}
			else
			{
				node.Position = { 40.0f + (index % 4) * 240.0f,
								  40.0f + (index / 4) * 120.0f };
			}

			node.Selected = (name == m_SelectedState);
			node.Pins = {
				{ "in", "In", NodeCanvas::PinDirection::Input, NodeCanvas::PinStyle::Flow },
				{ "out", "Out", NodeCanvas::PinDirection::Output, NodeCanvas::PinStyle::Flow },
			};
			// The body line is part of the label rather than a TypeKnown=false
			// marker: a state with no clip is a valid authoring state, it just
			// will not play, and TypeKnown=false renders it as an unknown node
			// type, which reads as "broken" rather than "empty".
			if (!state->Clip)
				node.Label += " (no clip)";
			m_Presentation.Nodes.push_back(std::move(node));
		}

		// Wires are attached to their SOURCE node, matching NodeCanvas::Wire's
		// endpoint-relative shape.
		const std::vector<Animation::AnimationTransition> &transitions =
			machine.GetTransitions();
		for (const Animation::AnimationTransition &transition : transitions)
		{
			NodeCanvas::Node *from = m_Presentation.FindNode(transition.FromState);
			if (!from)
				continue;
			// A dangling FromState cannot be drawn; Validate() reports it. Skipping
			// the wire rather than drawing it from nothing is the honest choice.
			if (!m_Presentation.FindNode(transition.ToState))
				continue;
			from->WiredTo.push_back({ "out", transition.ToState, "in" });
		}
	}

	// -- chrome ------------------------------------------------------------

	void AnimatorPanel::DrawToolbar()
	{
		if (ImGui::Button("Save"))
			SaveAsset();
	 ImGui::SameLine();
		if (ImGui::Button("Validate"))
		{
			if (!m_HasAsset)
				ApplyStatus("Nothing to validate: open a .wlanim first.");
			else if (!m_Asset.HasStateMachine)
				ApplyStatus("This asset has no state machine.");
			else
			{
				std::string error;
				if (m_Asset.StateMachine.Validate(error))
					ApplyStatus("State machine is valid.");
				else
					ApplyStatus("Invalid: " + error);
			}
		}
		if (!m_HasAsset)
			return;
		if (!m_Asset.HasStateMachine)
		{
			if (ImGui::Button("Add State Machine"))
			{
				m_Asset.HasStateMachine = true;
				ApplyStatus("State machine added. Add states below.");
			}
			return;
		}
		DrawAddStateRow();
	}

	void AnimatorPanel::DrawAddStateRow()
	{
		if (ImGui::Button("Add State"))
			AddStateNamed("");

		// The name field sits beside the button rather than in a popup: a popup
		// adds a modal step to the most common action in the panel. Enter adds the
		// state, so the two controls are one flow rather than two.
		if (ImGui::InputText("Name", m_NewStateBuffer.data(),
							static_cast<int>(m_NewStateBuffer.size()),
						 ImGuiInputTextFlags_EnterReturnsTrue))
			AddStateNamed(m_NewStateBuffer.data());
	}

	void AnimatorPanel::AddStateNamed(const std::string &rawName)
	{
		std::string name = rawName;

		// Trim: an all-whitespace name is a node nobody can read on the canvas.
		const auto isSpace = [](char c) {
			return std::isspace(static_cast<unsigned char>(c)) != 0;
		};
		name.erase(name.begin(),
				   std::find_if_not(name.begin(), name.end(), isSpace));
		name.erase(std::find_if_not(name.rbegin(), name.rend(), isSpace).base(),
				   name.end());

		if (name.empty())
		{
			// A generated name rather than a refusal: the button is the action, and
			// "you did not type a name" is not actionable. The suffix keeps repeated
			// clicks from colliding -- AddState with a duplicate name REPLACES, so a
			// bare "State" would silently discard the first one.
			std::size_t suffix = 1;
			do
			{
				name = "State" + std::to_string(suffix++);
			} while (m_Asset.StateMachine.GetStates().count(name) > 0);
		}

		// No clip: the state is created empty and the clip is assigned in the
		// inspector. AddState(name, clip) takes a clip, so nullptr is the
		// honest "no clip yet" -- passing an empty string would be a clip
		// NAME that resolves to nothing.
		m_Asset.StateMachine.AddState(name, nullptr);
		m_SelectedState = name;
		m_SelectedTransition = -1;
		std::fill(m_NewStateBuffer.begin(), m_NewStateBuffer.end(), '\0');
		// Buffers are keyed by state name, and the previous selection's fields are
		// still cached under its own key. Clearing means the new state's inspector
		// seeds from the state itself rather than from the state selected a moment
		// ago.
		m_TextBuffers.clear();
		ApplyStatus("Added state '" + name + "'.");
	}

	// -- canvas ------------------------------------------------------------

	void AnimatorPanel::DrawCanvas()
	{
		if (!m_HasAsset)
		{
			// Clicking a .wlanim in the content browser opens this panel. The empty
			// state has to say that, or an author sees a blank canvas and cannot
			// tell an empty machine from a failed load.
			if (ImGui::Button("Open .wlanim..."))
			{
				// The content browser owns file selection; this panel only needs to
				// be reachable from its own View menu entry, which is already wired.
			}
			return;
		}
		if (!m_Asset.HasStateMachine)
		{
			// No "add state machine" affordance on the canvas: the toolbar carries
			// it, and a second button doing the same thing in the middle of the
			// canvas would invite clicks on the empty area.
			return;
		}

		RebuildPresentation();
		if (m_Presentation.Nodes.empty())
			return;

		if (!m_FramedOnce)
		{
			m_Canvas.FrameAll(m_Presentation);
			m_FramedOnce = true;
		}

		NodeCanvas::Events events;
		m_Canvas.Render(m_Presentation, events);

		if (events.NodeSelected)
		{
			m_SelectedState = events.SelectedNodeId;
			m_SelectedTransition = -1;
		}
		if (events.NodeMoved)
		{
			// A drag writes the position into the asset, not just the presentation:
			// the presentation is rebuilt from the machine every frame, so a
			// position held only there would be discarded on the next rebuild and
			// the node would snap back.
			Animation::AnimationAsset::StatePosition position;
			position.X = events.MovedNodePosition.X;
			position.Y = events.MovedNodePosition.Y;
			m_Asset.StatePositions[events.MovedNodeId] = position;
		}
		if (events.WireAdded)
		{
			// Only a source "out" to a target "in" is a state transition. A wire
			// between two input pins is not a thing the machine can express, and
			// silently dropping it would look like the canvas ate the gesture.
			if (events.LinkFromPin == "out" && events.LinkToPin == "in")
			{
				Animation::AnimationTransition transition;
				transition.FromState = events.LinkFromNode;
				transition.ToState = events.LinkToNode;
				transition.Trigger.clear(); // unconditional
				transition.Duration = -1.0f; // use the source state's duration
				m_Asset.StateMachine.AddTransition(transition);
				ApplyStatus("Added transition " + transition.FromState + " -> " +
							transition.ToState + ".");
			}
			else
			{
				ApplyStatus("Wires run from an Out pin to an In pin.");
			}
		}
		if (events.NodeDeleted)
		{
			const std::string removed = NodeCanvas::TakeSelectedNodeIdForDelete(
				m_SelectedState);
			if (!removed.empty() && m_Asset.StateMachine.RemoveState(removed))
			{
				ApplyStatus("Removed state '" + removed + "'.");
				// Transitions naming it are left in place on purpose -- see
				// RemoveState's header comment. The canvas stops drawing them
				// because neither endpoint resolves, and Validate reports them.
			}
			else
			{
				ApplyStatus("No state selected to delete.");
			}
		}
	}

	// -- inspectors --------------------------------------------------------

	void AnimatorPanel::DrawStateInspector()
	{
		if (!m_HasAsset || !m_Asset.HasStateMachine)
			return;
		if (m_SelectedState.empty())
		{
			// Deliberately not "nothing selected" in grey: there is a real action
			// available (click a node), and saying so is more useful than a status.
			return;
		}

		const auto stateIt = m_Asset.StateMachine.GetStates().find(m_SelectedState);
		if (stateIt == m_Asset.StateMachine.GetStates().end())
		{
			// The selection can go stale: a delete leaves the name behind, and
			// this path is the only other thing that clears it. Re-checking rather
			// than trusting m_SelectedState is what keeps the inspector from
			// editing a state that no longer exists.
			m_SelectedState.clear();
			return;
		}
		const Animation::AnimationState *state = &stateIt->second;
		if (!state)
		{
			// The selection can go stale: RemoveState leaves the name behind, and
			// the panel clears it only on its own delete path. Re-checking here
			// rather than trusting m_SelectedState is what keeps the inspector from
			// editing a state that no longer exists.
			m_SelectedState.clear();
			return;
		}

		const std::string key = "state/" + m_SelectedState + "/";

		// Clip name. A NAME into the asset's clip table, not a path, so it is
		// edited as text and resolved by name -- matching how the .wlanim format
		// refers to clips. Committed on every keystroke because a string has no
		// invalid intermediate state to protect, unlike a float.
		const std::string currentClip = state->Clip ? state->Clip->GetName() : std::string();
		std::vector<char> &clipBuffer = TextFieldBuffer(key + "clip", currentClip);
		if (ImGui::InputText("Clip", clipBuffer.data(),
							static_cast<int>(clipBuffer.size()), 0))
		{
			const std::string edited(clipBuffer.data());
			if (edited != currentClip)
			{
				// An empty name clears the clip; anything else must resolve to a
				// clip in this asset. A typo silently clearing the clip would be
				// the worst outcome, so an unknown name is reported and kept.
				std::shared_ptr<Animation::AnimationClip> resolved;
				if (!edited.empty())
				{
					for (const auto &candidate : m_Asset.Clips)
					{
						if (candidate && candidate->GetName() == edited)
						{
							resolved = candidate;
							break;
						}
					}
					if (!resolved)
					{
						ApplyStatus("No clip named '" + edited + "' in this asset.");
						return;
					}
				}
				m_Asset.StateMachine.SetStateClip(m_SelectedState, resolved);
			}
		}

		float speed = state->Speed;
		if (DrawFloatField("Speed", speed))
			m_Asset.StateMachine.SetStateSpeed(m_SelectedState, speed);

		bool looping = state->Looping;
		if (ImGui::Checkbox("Loop", &looping))
			m_Asset.StateMachine.SetStateLooping(m_SelectedState, looping);

		float duration = state->TransitionDuration;
		if (DrawFloatField("Transition Duration", duration))
			m_Asset.StateMachine.SetStateTransitionDuration(m_SelectedState, duration);
	}

	void AnimatorPanel::DrawTransitionList()
	{
		if (!m_HasAsset || !m_Asset.HasStateMachine)
			return;

		const std::vector<Animation::AnimationTransition> &transitions =
			m_Asset.StateMachine.GetTransitions();
		if (transitions.empty())
		{
			return;
		}

		// Indexed by position in GetTransitions(), which is insertion order and
		// therefore priority order. The list is re-read every frame, so an index
		// is valid for the frame it was taken on -- the same rule the
		// GetTransitions comment gives for the vector.
		for (std::size_t i = 0; i < transitions.size(); ++i)
		{
			const Animation::AnimationTransition &transition = transitions[i];
			const bool selected = (static_cast<int>(i) == m_SelectedTransition);
			const std::string label = transition.FromState + " -> " + transition.ToState +
									  (transition.Trigger.empty() ? "" : " [" + transition.Trigger + "]");
			if (ImGui::Selectable(label.c_str(), selected))
				m_SelectedTransition = static_cast<int>(i);
		}

		if (m_SelectedTransition < 0 ||
			static_cast<std::size_t>(m_SelectedTransition) >= transitions.size())
		{
			return;
		}

		const std::string key = "transition/" + std::to_string(m_SelectedTransition) + "/";
		const Animation::AnimationTransition &transition =
			transitions[static_cast<std::size_t>(m_SelectedTransition)];

		// Committed on every keystroke: a trigger is a string, so there is no
		// invalid intermediate value to protect (unlike a float).
		std::vector<char> &triggerBuffer =
			TextFieldBuffer(key + "trigger", transition.Trigger);
		if (ImGui::InputText("Trigger", triggerBuffer.data(),
							static_cast<int>(triggerBuffer.size()), 0))
		{
			const std::string edited(triggerBuffer.data());
			if (edited != transition.Trigger)
				m_Asset.StateMachine.SetTransitionTrigger(
					static_cast<std::size_t>(m_SelectedTransition), edited);
		}

		float duration = transition.Duration;
		if (DrawFloatField("Duration", duration))
			m_Asset.StateMachine.SetTransitionDuration(
				static_cast<std::size_t>(m_SelectedTransition), duration);

		if (ImGui::Button("Delete Transition"))
		{
			// Re-fetched by the triple because the index is only valid this frame;
			// deleting by index directly is fine here because nothing was added
			// between the read and the click.
			m_Asset.StateMachine.RemoveTransition(transition.FromState,
												  transition.ToState,
												  transition.Trigger);
			m_SelectedTransition = -1;
		}
	}

	void AnimatorPanel::OnImGuiRender()
	{
		if (!m_Visible)
			return;

		if (ImGui::Begin("Animator", &m_Visible))
		{
			DrawToolbar();

			// The status is the only place a failed load, save, or validation result
			// appears. ImGui has no toast, and a modal would steal focus from the
			// canvas mid-drag. It auto-clears after ~5 seconds: a permanent strip
			// would reserve a line of canvas forever for a message nobody is reading,
			// and clearing on hover made it vanish before it could be read.
			if (!m_Status.empty())
			{
				// A plain line rather than a child window: a child reserves a row even
				// when empty, which would shift the canvas on every message.
				if (m_StatusFrames > 0)
				{
					--m_StatusFrames;
					ImGui::TextUnformatted(m_Status.c_str());
				}
				else
				{
					m_Status.clear();
				}
			}

			// -- body: canvas left, inspectors right --
			const float available = ImGui::GetContentRegionAvail().x;
			if (available <= 0.0f)
				return;

			const float canvasWidth = available * 0.68f;
			if (canvasWidth > 0.0f)
			{
				if (ImGui::BeginChild("Canvas", ImVec2(canvasWidth, 0.0f), true))
					DrawCanvas();
				// EndChild runs unconditionally: ImGui asserts on an unbalanced
				// Begin/End pair. The child is always opened above, so this matches.
			}
			// Inspector column: SameLine puts the second child beside the first.
			if (ImGui::BeginChild("Inspector", ImVec2(0.0f, 0.0f), true))
			{
				DrawStateInspector();
				DrawTransitionList();
			}
		}
	}
}
