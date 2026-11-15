#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <cstdint>
#include <vector>

#include "NodeCanvas.h"
#include "Wasteland/Animation/AnimationSerializer.h"

namespace Wasteland
{
	// Authoring surface for the state machine half of a .wlanim.
	//
	// NodeCanvas is only the presentation/interaction layer, exactly as it is for
	// .wlavb: AnimationStateMachine owns the model, this panel owns the picture of
	// it. Nothing here compiles or evaluates -- the runtime machine is already
	// C++, which is why this panel has no compiler step at all.
	//
	// The one piece of state that is NOT in the machine is node position, and it
	// lives in AnimationAsset::StatePositions rather than on AnimationState. See
	// that field's comment for why.
	class AnimatorPanel
	{
	public:
		void OnImGuiRender();

		// Opens a .wlanim. The asset root is needed so the panel can offer the
		// other clips in the project from its own library, matching the timeline
		// panel's behaviour.
		bool OpenAsset(const std::filesystem::path &path,
					   const std::filesystem::path &assetRoot);
		void SetAssetRoot(const std::filesystem::path &root);
		void Reset();

		bool SaveAsset();

		bool IsVisible() const { return m_Visible; }
		void SetVisible(bool visible) { m_Visible = visible; }
		const std::string &GetStatus() const { return m_Status; }

	private:
		void RebuildPresentation();
		void DrawToolbar();
		void DrawCanvas();
		void DrawStateInspector();
		void DrawTransitionList();
		void DrawAddStateRow();
		void ApplyStatus(const std::string &message);

		// Adds a state from the toolbar. An empty or all-whitespace name gets a
		// generated one, because the button is the action and a refusal would not
		// be actionable.
		void AddStateNamed(const std::string &rawName);

		// ImGui text widgets need a mutable char buffer, and the machine stores
		// typed values. Keyed by FIELD rather than by state: a buffer keyed by state
		// would make two states' Speed fields share one buffer and editing one
		// would edit the other on the next frame.
		std::vector<char> &TextFieldBuffer(const std::string &key,
											  const std::string &seed);

		// Returns true only when the field was COMMITTED (Enter or focus loss).
		// Writing on every keystroke would commit intermediate states -- "1." is
		// not a float, and strtof would read it as 1, silently discarding the dot
		// the user is about to finish typing.
		bool DrawFloatField(const char *label, float &value);

		bool m_Visible = false;
		bool m_SizedOnce = false;
		bool m_FramedOnce = false;

		std::filesystem::path m_AssetPath;
		std::filesystem::path m_AssetRoot;
		Animation::AnimationAsset m_Asset;
		bool m_HasAsset = false;

		// Presentation is rebuilt from the machine every frame rather than cached.
		// A cached copy would have to be invalidated by every edit, and a missed
		// invalidation would draw a machine the user has already changed -- the
		// canvas would show a transition that no longer exists while the file
		// says otherwise.
		NodeCanvas::Presentation m_Presentation;

		// The selected state's name, empty for none. A name rather than an index
		// because a state can be removed, and an index into a map is meaningless
		// after a rehash.
		std::string m_SelectedState;
		// The selected transition's index into GetTransitions(), -1 for none.
		int m_SelectedTransition = -1;

		// Per-field edit buffers for the inspector. ImGui input widgets need a
		// mutable char buffer, and the machine stores typed values; converting on
		// every keystroke would reject intermediate states ("1." is not a float).
		std::unordered_map<std::string, std::vector<char>> m_TextBuffers;

		// A fixed buffer for the toolbar name field. A std::string cannot back an
		// ImGui InputText directly: &str[0] on an empty string is undefined, and
		// ImGui writes a null terminator, so the capacity must already allow it.
		std::vector<char> m_NewStateBuffer = std::vector<char>(64, '\0');
		std::string m_Status;
		// Frames left before m_Status auto-clears. A permanent strip would reserve
		// a line of canvas forever for a message nobody is reading; clearing on
		// hover made it vanish before it could be read.
		int m_StatusFrames = 0;
		NodeCanvas m_Canvas;
	};
}
