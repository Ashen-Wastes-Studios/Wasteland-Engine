// AnimationTimelinePanel -- the .wlanim clip library and keyframe timeline.
//
// The one panel that authors animation. It edits an AnimationAsset in memory
// and writes it back through AnimationSerializer, so a drag is a real edit to
// a real file rather than editor-only state.
//
// DELIBERATELY READ-ONLY ABOUT PLAYBACK. The panel never touches an
// AnimationSystem, an Animator, or an entity. Scrubbing samples the clip into
// a pose the PANEL owns and draws a stick figure from it; it does not advance
// any runtime and does not move anything in the scene. That is the "no playback
// side effects" contract: authoring a clip must not animate the level.
//
// v1 scope: track list, frame ruler, playhead, keyframe diamonds, drag-to-move,
// add/remove key, and the pose preview. No dope sheet, no curve editor, no
// retarget manager -- those are P1.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Wasteland/Animation/AnimationSerializer.h"

namespace Wasteland
{
	class AnimationTimelinePanel
	{
	public:
		AnimationTimelinePanel() = default;

		void OnImGuiRender();

		bool IsVisible() const { return m_Visible; }
		void SetVisible(bool visible) { m_Visible = visible; }

		// Load a .wlanim into the panel. `assetRoot` is only used to build the
		// library list; the file itself is read from `path`. Returns false with
		// a reason in GetStatus() when the file cannot be parsed -- the caller
		// shows it, the panel does not pop dialogs.
		// Sets the directory the library scans, without opening anything.
		//
		// Separate from OpenAsset because the two have different failure modes:
		// OpenAsset cannot succeed without a file, but the library CAN be useful
		// before one is open -- it is how the author picks the file. Requiring an
		// OpenAsset call to populate the list made the panel's empty state
		// unreachable through the UI.
		void SetAssetRoot(const std::filesystem::path &root);

		bool OpenAsset(const std::filesystem::path &path,
					   const std::filesystem::path &assetRoot);

		// Drop the loaded asset and rebuild the library. Used when the project
		// changes, so the panel cannot keep showing another project's clips.
		void Reset();

		const std::string &GetStatus() const { return m_Status; }

	private:
		// The .wlanim files under the asset root's animation directory. Rebuilt
		// on open and on demand rather than per frame: a directory scan is a
		// syscall per entry, and the set only changes when a file is added.
		void RebuildAssetList();

		void DrawLibrary();
		void DrawClipSelector();
		void DrawTrackList();
		void DrawTimeline();
		void DrawPosePreview();

		// Write the in-memory asset back to m_AssetPath. Returns false with a
		// reason when the path is empty or the write fails.
		bool SaveAsset();

		// The selected clip, or null. A clip index rather than a pointer
		// because the vector is rebuilt on every load and a pointer would
		// dangle across it.
		Animation::AnimationClip *GetSelectedClip();
		const Animation::AnimationClip *GetSelectedClip() const;

		// Samples the selected clip at `time` into m_PreviewPose. Returns false
		// when there is no clip or no rig, leaving m_PreviewValid false so the
		// preview draws its empty state instead of a stale pose.
		bool SamplePreview(float time);

		bool m_Visible = false;

		// One-shot, not a per-frame SetNextWindowSize: the panel is re-docked on
		// every layout switch, and an unconditional size would fight the dock
		// node for space on every frame after the first.
		bool m_SizedOnce = false;

		std::filesystem::path m_AssetRoot;
		std::filesystem::path m_AssetPath;
		std::vector<std::string> m_AssetFiles;

		Animation::AnimationAsset m_Asset;
		int m_SelectedClip = -1;

		// Seconds. The playhead is panel state, never runtime state.
		float m_Playhead = 0.0f;
		// Horizontal zoom, in pixels per second. Clamped on use rather than on
		// set so a wild mouse-wheel delta cannot produce a zero or negative
		// scale, which would make the pixel<->time mapping divide by zero.
		float m_PixelsPerSecond = 80.0f;

		// Keyframe drag state. `m_DragBone` is the animated-bone index into
		// the clip's GetAnimatedBones() list, not a BoneIndex, because the
		// timeline iterates that list and a BoneIndex would have to be
		// recovered from it anyway.
		bool m_Dragging = false;
		int m_DragBone = -1;
		float m_DragFrom = 0.0f;
		float m_DragTo = 0.0f;
		bool m_DragMoved = false;

		// The pose the scrub preview draws. Seeded from the rig's rest pose and
		// overwritten by SamplePreview; it is never handed to anything else.
		Animation::Pose m_PreviewPose;
		bool m_PreviewValid = false;

		std::string m_Status;
	};
}
