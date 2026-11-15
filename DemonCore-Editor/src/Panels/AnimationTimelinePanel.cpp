// AnimationTimelinePanel -- see the header for the design contract.
//
// The panel edits an AnimationAsset in memory and writes it back through
// AnimationSerializer. It never touches a runtime, an Animator or an entity:
// scrubbing samples the clip into a pose the PANEL owns and draws a stick
// figure from it. That is what "no playback side effects" means here -- authoring
// a clip must not animate the level.

#include "wlpch.h"
#include "AnimationTimelinePanel.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "Wasteland/Animation/AnimationClip.h"
#include "Wasteland/Animation/Skeleton.h"

namespace Wasteland
{
	namespace
	{
		// ---- Layout ----

		constexpr float kTrackLabelWidth = 170.0f;
		constexpr float kTrackRowHeight = 22.0f;
		constexpr float kRulerHeight = 24.0f;
		constexpr float kPreviewHeight = 150.0f;
		constexpr float kLanePadding = 10.0f;

		// ---- Zoom ----

		// Clamped on use, not on set: a mouse-wheel delta can arrive as anything,
		// and a scale of zero or less would make the pixel<->time mapping divide
		// by zero. Compare SetZoomLevel in OrthographicCameraController.h, whose
		// comment flags the same missing clamp on a camera zoom.
		constexpr float kMinPixelsPerSecond = 8.0f;
		constexpr float kMaxPixelsPerSecond = 800.0f;

		// ---- Hit testing ----

		// Seven pixels is about the smallest target that is still comfortable with
		// a mouse, and generous enough that a dense track stays editable.
		constexpr float kKeyHitRadius = 7.0f;

		// ---- Colours ----

		// One per channel, distinct hues rather than shades of one hue: the
		// channels are not ordered, so a gradient would imply a sequence.
		constexpr ImU32 kColourPosition = IM_COL32(90, 170, 255, 255); // blue
		constexpr ImU32 kColourRotation = IM_COL32(255, 205, 70, 255); // amber
		constexpr ImU32 kColourScale = IM_COL32(120, 220, 130, 255);	 // green
		constexpr ImU32 kColourPlayhead = IM_COL32(255, 60, 60, 255);
		constexpr ImU32 kColourGrid = IM_COL32(72, 72, 80, 255);
		constexpr ImU32 kColourLane = IM_COL32(44, 44, 50, 255);
		constexpr ImU32 kColourBone = IM_COL32(190, 190, 200, 255);
		constexpr ImU32 kColourJoint = IM_COL32(240, 240, 240, 255);

		const char *BoneDisplayName(const Animation::Skeleton &skeleton,
									Animation::BoneIndex index)
		{
			const std::string &name = skeleton.GetBoneName(index);
			return name.empty() ? "<unnamed>" : name.c_str();
		}
	}

	void AnimationTimelinePanel::OnImGuiRender()
	{
		if (!m_Visible)
			return;

		// One-shot, not per-frame: the panel is re-docked on every layout switch,
		// and an unconditional ImGui::SetNextWindowSize would fight the dock node for
		// space on every frame after the first.
		if (!m_SizedOnce)
		{
			// FIRST_USE_EVER, so a user resize survives a layout switch. A plain
			// Cond_Always would override their size every time the panel is
			// re-docked.
			ImGui::SetNextWindowSize(ImVec2(1240.0f, 560.0f), ImGuiCond_FirstUseEver);
			m_SizedOnce = true;
		}

		if (!ImGui::Begin("Animation Timeline", &m_Visible, ImGuiWindowFlags_MenuBar))
		{
			ImGui::End();
			return;
		}

		if (m_AssetPath.empty())
		{
			// The empty state is the library: before anything is open the panel's
			// only job is to offer a clip to open.
			DrawLibrary();
			ImGui::End();
			return;
		}

		if (ImGui::BeginMenuBar())
		{
			if (ImGui::BeginMenu("File"))
			{
				// Re-read from disk, discarding in-memory edits. Labelled as a
				// revert because that is what it is; the panel does not prompt,
				// so an unsaved drag is lost on click.
				if (ImGui::MenuItem("Revert from Disk"))
				{
					Animation::AnimationError error;
					if (!Animation::AnimationSerializer::Load(m_AssetPath.string(), m_Asset, error))
						m_Status = "revert failed: " + error.Message;
					else
						m_Status = "reverted from disk";
					m_SelectedClip = m_Asset.Clips.empty() ? -1 : 0;
					SamplePreview(m_Playhead);
				}
				// Reload the library list. Cheap and explicit; the alternative is
				// a file watcher, which is a subsystem rather than a button.
				if (ImGui::MenuItem("Refresh Library"))
					RebuildAssetList();
				ImGui::EndMenu();
			}
			ImGui::EndMenuBar();
		}

		const Animation::AnimationClip *clip = GetSelectedClip();
		if (!clip)
		{
			ImGui::TextDisabled("No clip selected. Open a clip from the library.");
			ImGui::End();
			return;
		}

		const float duration = std::max(clip->GetDuration(), 1e-3f);

		// The status line is a permanent element, not a tooltip: a failed save
		// is the one outcome an author must not miss.
		if (!m_Status.empty())
		{
			// Red only for failures. A neutral colour for success would make the
			// line look like a warning every time a drag lands.
			const bool failed = m_Status.rfind("failed", 0) == 0 ||
								m_Status.rfind("revert failed", 0) == 0;
			ImGui::PushStyleColor(ImGuiCol_Text, failed ? IM_COL32(255, 96, 96, 255)
												 : IM_COL32(140, 200, 140, 255));
			ImGui::TextWrapped("%s", m_Status.c_str());
			ImGui::PopStyleColor();
		}

		DrawClipSelector();

		// Track list on the left, timeline filling the rest, preview pinned to
		// the bottom. Two ImGui::BeginChild calls rather than BeginHorizontal: the
		// timeline needs its own scroll state, and a child window carries that
		// while a horizontal group does not.
		ImGui::SetCursorPosX(0.0f);
		if (ImGui::BeginChild("AnimTrackList", ImVec2(kTrackLabelWidth, -kPreviewHeight - 8.0f), true,
					   ImGuiWindowFlags_NoScrollbar))
		{
			DrawTrackList();
		}
		ImGui::EndChild();

		ImGui::SameLine();

		ImGui::SetCursorPosY(ImGui::GetCursorPosY());
		if (ImGui::BeginChild("AnimTimeline", ImVec2(0.0f, -kPreviewHeight - 8.0f), true,
					   ImGuiWindowFlags_HorizontalScrollbar))
		{
			DrawTimeline();
		}
		ImGui::EndChild();

		if (ImGui::BeginChild("AnimPosePreview", ImVec2(0.0f, kPreviewHeight), true,
					   ImGuiWindowFlags_NoScrollbar))
		{
			DrawPosePreview();
		}
		ImGui::EndChild();

		ImGui::End();
	}

	void AnimationTimelinePanel::RebuildAssetList()
	{
		m_AssetFiles.clear();

		if (m_AssetRoot.empty())
			return;

		const std::filesystem::path animationDir = m_AssetRoot / "animation";
		std::error_code ec;
		if (!std::filesystem::exists(animationDir, ec))
			return;

		// A non-throwing overload everywhere: this runs from a menu click and
		// from OpenAsset, and a permissions error on one subdirectory must not
		// take down the editor frame.
		for (const auto &entry :
			 std::filesystem::directory_iterator(animationDir, ec))
		{
			if (ec)
				break;
			if (!entry.is_regular_file(ec) || ec)
				continue;
			if (entry.path().extension() != ".wlanim")
				continue;
			m_AssetFiles.push_back(entry.path().string());
		}

		std::sort(m_AssetFiles.begin(), m_AssetFiles.end());
	}

	void AnimationTimelinePanel::SetAssetRoot(const std::filesystem::path &root)
	{
		if (m_AssetRoot == root)
			return;
		m_AssetRoot = root;
		RebuildAssetList();
	}

	bool AnimationTimelinePanel::OpenAsset(const std::filesystem::path &path,
										   const std::filesystem::path &assetRoot)
	{
		Animation::AnimationError error;
		Animation::AnimationAsset loaded;
		if (!Animation::AnimationSerializer::Load(path.string(), loaded, error))
		{
			m_Status = "open failed: " + error.Message;
			return false;
		}

		m_Asset = std::move(loaded);
		m_AssetPath = path;
		m_AssetRoot = assetRoot;
		m_SelectedClip = m_Asset.Clips.empty() ? -1 : 0;
		m_Playhead = 0.0f;
		m_PreviewValid = false;
		m_Dragging = false;
		m_DragMoved = false;

		RebuildAssetList();

		m_Status = "opened " + path.filename().string();
		SamplePreview(m_Playhead);
		return true;
	}

	void AnimationTimelinePanel::Reset()
	{
		// Every field, not just the asset: a project switch that left the
		// library list behind would show another project's clips as if they were
		// this one's.
		m_Asset = Animation::AnimationAsset();
		m_AssetPath.clear();
		m_AssetRoot.clear();
		m_AssetFiles.clear();
		m_SelectedClip = -1;
		m_Playhead = 0.0f;
		m_PixelsPerSecond = 80.0f;
		m_Dragging = false;
		m_DragMoved = false;
		m_PreviewValid = false;
		m_PreviewPose = Animation::Pose();
		m_Status.clear();
	}

	Animation::AnimationClip *AnimationTimelinePanel::GetSelectedClip()
	{
		if (m_SelectedClip < 0 || m_SelectedClip >= (int)m_Asset.Clips.size())
			return nullptr;
		return m_Asset.Clips[(size_t)m_SelectedClip].get();
	}

	const Animation::AnimationClip *AnimationTimelinePanel::GetSelectedClip() const
	{
		if (m_SelectedClip < 0 || m_SelectedClip >= (int)m_Asset.Clips.size())
			return nullptr;
		return m_Asset.Clips[(size_t)m_SelectedClip].get();
	}

	bool AnimationTimelinePanel::SamplePreview(float time)
	{
		const Animation::AnimationClip *clip = GetSelectedClip();
		if (!clip || !m_Asset.Skeleton)
		{
			m_PreviewValid = false;
			return false;
		}

		const Animation::Skeleton &skeleton = *m_Asset.Skeleton;

		// Seed with the rest pose. Sample resizes the pose but only overwrites
		// the bones the clip names, so an unseeded pose would leave unnamed bones
		// at whatever the last sample held -- the contract documented on
		// AnimationClip::Sample.
		m_PreviewPose = skeleton.GetRestPose();
		clip->Sample(skeleton, time, m_PreviewPose);
		m_PreviewValid = true;
		return true;
	}

	bool AnimationTimelinePanel::SaveAsset()
	{
		if (m_AssetPath.empty())
		{
			m_Status = "save failed: no file is open";
			return false;
		}

		Animation::AnimationError error;
		if (!Animation::AnimationSerializer::Save(m_AssetPath.string(), m_Asset, error))
		{
			m_Status = "save failed: " + error.Message;
			return false;
		}

		m_Status = "saved " + m_AssetPath.filename().string();
		return true;
	}

	void AnimationTimelinePanel::DrawLibrary()
	{
		ImGui::Text("Animation Clips");
		ImGui::Separator();

		// The root is the project's asset root, which the caller supplies on
		// OpenAsset. Without it there is nothing to scan, and the panel says so
		// rather than drawing an empty list that looks like an empty project.
		if (m_AssetRoot.empty())
		{
			ImGui::TextDisabled("Open a .wlanim to populate the library.");
			return;
		}

		if (ImGui::Button("Refresh"))
			RebuildAssetList();
		ImGui::SameLine();
		ImGui::TextDisabled("%zu clip(s) under %s", m_AssetFiles.size(),
					 (m_AssetRoot / "animation").string().c_str());

		if (ImGui::BeginListBox("##AnimLibrary", ImVec2(-1.0f, 0.0f)))
		{
			for (const std::string &file : m_AssetFiles)
			{
				const std::filesystem::path path(file);
				const std::string name = path.filename().string();
				if (ImGui::Selectable(name.c_str(), file == m_AssetPath.string()))
					OpenAsset(path, m_AssetRoot);
			}
			ImGui::EndListBox();
		}
	}

	void AnimationTimelinePanel::DrawClipSelector()
	{
		ImGui::Text("Clip");
		ImGui::SameLine();

		// The combo is rebuilt from the asset on every frame rather than cached:
		// a clip can be added or removed by an importer while the panel is open,
		// and a cached list would offer a clip that no longer exists.
		if (ImGui::BeginCombo("##AnimClip", m_SelectedClip >= 0
										  ? m_Asset.Clips[(size_t)m_SelectedClip]->GetName().c_str()
										  : "<none>"))
		{
			for (int i = 0; i < (int)m_Asset.Clips.size(); ++i)
			{
				const bool selected = (i == m_SelectedClip);
				if (ImGui::Selectable(m_Asset.Clips[(size_t)i]->GetName().c_str(), selected))
				{
					m_SelectedClip = i;
					m_Playhead = 0.0f;
					SamplePreview(m_Playhead);
				}
			}
			ImGui::EndCombo();
		}

		ImGui::SameLine();
		ImGui::TextDisabled("%.3fs, %zu track(s)",
					 GetSelectedClip() ? GetSelectedClip()->GetDuration() : 0.0f,
					 GetSelectedClip() ? GetSelectedClip()->GetTrackCount() : 0u);
	}

	void AnimationTimelinePanel::DrawTrackList()
	{
		const Animation::AnimationClip *clip = GetSelectedClip();
		if (!clip)
		{
			ImGui::TextDisabled("No clip.");
			return;
		}

		const std::vector<Animation::BoneIndex> bones = clip->GetAnimatedBones();

		// Header, aligned with the ruler drawn in DrawTimeline. It is drawn here
		// rather than in the timeline child so the label column does not scroll
		// horizontally with the keys.
		ImGui::PushStyleColor(ImGuiCol_Header, kColourLane);
		ImGui::Selectable("##TrackHeader", false, 0, ImVec2(0.0f, kRulerHeight));
		ImGui::PopStyleColor();
		ImGui::SameLine(0.0f, 0.0f);
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() - kRulerHeight * 0.5f);
		ImGui::TextDisabled("bone  P / R / S");

		for (size_t i = 0; i < bones.size(); ++i)
		{
			const Animation::BoneIndex bone = bones[i];
			const Animation::BoneTrack *track = clip->GetTrack(bone);
			if (!track)
				continue;

			// Key counts per channel, because a bone can be animated on one
			// channel only and the row must show which.
			int positionKeys = 0;
			for (int axis = 0; axis < 3; ++axis)
				positionKeys += (int)track->Position[axis].GetKeys().size();

			const int rotationKeys = (int)track->Rotation.GetKeys().size();

			int scaleKeys = 0;
			for (int axis = 0; axis < 3; ++axis)
				scaleKeys += (int)track->Scale[axis].GetKeys().size();

			// A fixed-width prefix so the counts line up as a column.
			char label[128];
			std::snprintf(label, sizeof(label), "%-18s %3d / %3d / %3d",
						  BoneDisplayName(*m_Asset.Skeleton, bone),
						  positionKeys, rotationKeys, scaleKeys);

			// Selecting a bone here only highlights it; there is no bone editing
			// in v1, so the selection is a reading aid for the timeline.
			if (ImGui::Selectable(label, m_DragBone == bone.Value))
			{
				m_DragBone = bone.Value;
			}
		}
	}

	void AnimationTimelinePanel::DrawTimeline()
	{
		// Non-const: this function adds, removes and moves keys. It takes the
		// mutable overload deliberately -- a const clip would make the mutation
		// paths fail to compile rather than silently no-op, which is the right
		// way for that mistake to surface.
		Animation::AnimationClip *clip = GetSelectedClip();
		if (!clip)
			return;

		const float pixelsPerSecond = std::clamp(m_PixelsPerSecond, kMinPixelsPerSecond,
												 kMaxPixelsPerSecond);
		const float duration = std::max(clip->GetDuration(), 1e-3f);

		// The content size drives the horizontal scrollbar, and the scrollbar
		// offset is what the pixel<->time mapping has to undo.
		const float contentWidth = duration * pixelsPerSecond + kLanePadding * 2.0f;
		const float contentHeight =
			kRulerHeight +
			std::max((float)clip->GetAnimatedBones().size(), 1.0f) * kTrackRowHeight;

		ImGui::SetNextWindowContentSize(ImVec2(contentWidth, contentHeight));
		ImGui::BeginChild("##AnimScroll", ImVec2(0.0f, 0.0f), false,
				   ImGuiWindowFlags_None);
		{
			const ImVec2 childPos = ImGui::GetWindowPos();
			const float scrollX = ImGui::GetScrollX();
			ImDrawList *draw = ImGui::GetWindowDrawList();

			const std::vector<Animation::BoneIndex> bones = clip->GetAnimatedBones();
			const float laneTop = kRulerHeight;

			// ---- Ruler ----

			draw->AddRectFilled(ImVec2(childPos.x, childPos.y),
								ImVec2(childPos.x + contentWidth - scrollX, childPos.y + laneTop),
								kColourLane);

			// A tick every 0.5s with a label, and a fainter one every 0.1s. The
			// step is chosen from the zoom so the labels never collide: at the
			// widest zoom 0.5s is ~400px, at the narrowest ~4px, where the
			// labels are skipped rather than drawn on top of each other.
			const float majorStep = 0.5f;
			const float minorStep = 0.1f;
			const bool labelsFit = (majorStep * pixelsPerSecond) > 24.0f;

			for (float t = 0.0f; t <= duration + minorStep * 0.5f; t += minorStep)
			{
				const bool major = std::fabs(std::fmod(t, majorStep)) < minorStep * 0.5f;
				if (!major && !labelsFit && (int)(t / minorStep) % 5 != 0)
					continue;

				const float x = childPos.x + t * pixelsPerSecond - scrollX;
				const float yBottom = childPos.y + laneTop;
				const float tickHeight = major ? 8.0f : 4.0f;
				draw->AddLine(ImVec2(x, yBottom - tickHeight), ImVec2(x, yBottom),
							  major ? kColourGrid : kColourLane, 1.0f);

				if (major && labelsFit)
				{
					char text[32];
					std::snprintf(text, sizeof(text), "%.1f", t);
					draw->AddText(ImVec2(x + 3.0f, childPos.y + 4.0f), kColourGrid, text);
				}
			}

			// ---- Lanes and keyframes ----

			// The mouse position is needed for both hit-testing and dragging, so
			// it is read once here.
			const ImVec2 mouse = ImGui::GetIO().MousePos;

			for (size_t i = 0; i < bones.size(); ++i)
			{
				const Animation::BoneIndex bone = bones[i];
				const float laneY = childPos.y + laneTop + (float)i * kTrackRowHeight;
				const float laneCentreY = laneY + kTrackRowHeight * 0.5f;

				draw->AddRectFilled(ImVec2(childPos.x, laneY),
									ImVec2(childPos.x + contentWidth - scrollX, laneY + kTrackRowHeight),
									(i % 2 == 0) ? kColourLane : IM_COL32(40, 40, 46, 255));

				const Animation::BoneTrack *track = clip->GetTrack(bone);
				if (!track)
					continue;

				// Position and scale keys are drawn as small squares and circles
				// so they cannot be confused with rotation diamonds, which are
				// the primary channel and the only one the panel edits.
				for (int axis = 0; axis < 3; ++axis)
				{
					for (const auto &key : track->Position[axis].GetKeys())
					{
						const float x = childPos.x + key.Time * pixelsPerSecond - scrollX;
						draw->AddRectFilled(ImVec2(x - 3.0f, laneCentreY + 2.0f),
											ImVec2(x + 3.0f, laneCentreY + 8.0f), kColourPosition);
					}
					for (const auto &key : track->Scale[axis].GetKeys())
					{
						const float x = childPos.x + key.Time * pixelsPerSecond - scrollX;
						draw->AddCircleFilled(ImVec2(x, laneCentreY - 7.0f), 3.0f, kColourScale);
					}
				}

				for (const auto &key : track->Rotation.GetKeys())
				{
					const float x = childPos.x + key.Time * pixelsPerSecond - scrollX;

					// The key being dragged is drawn at the dragged time, not at
					// its stored time, so the diamond follows the cursor and the
					// stored value stays untouched until the drop. That is what
					// makes a drag an edit rather than a preview.
					const bool isDragged = (m_Dragging && m_DragBone == bone.Value &&
											std::fabs(key.Time - m_DragFrom) <= 1e-6f);
					const float drawnX = isDragged
											? childPos.x + m_DragTo * pixelsPerSecond - scrollX
											: x;

					const ImVec2 centre(drawnX, laneCentreY - 1.0f);
					const float r = isDragged ? 6.0f : 5.0f;
					const ImU32 colour = isDragged ? IM_COL32(255, 255, 255, 255)
												   : kColourRotation;

					// A diamond, matching the convention every DCC tool uses for
					// a key. ImGui::AddRectFilled rotated would need the internal API;
					// four triangles via a triangle fan is the same shape.
					draw->AddTriangleFilled(ImVec2(centre.x, centre.y - r),
											ImVec2(centre.x + r, centre.y),
											ImVec2(centre.x, centre.y + r), colour);
					draw->AddTriangleFilled(ImVec2(centre.x, centre.y - r),
											ImVec2(centre.x - r, centre.y),
											ImVec2(centre.x, centre.y + r), colour);
				}

				// Right-click on a lane adds a rotation key at the playhead; on a
				// key it removes that key. The hit test runs first, so a click
				// that lands on a key never also adds one.
				if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
				{
					// Right-click is handled after the loop below, once, using the
					// same hit test.
				}
			}

			// ---- Playhead ----

			const float playheadX = childPos.x + m_Playhead * pixelsPerSecond - scrollX;
			draw->AddLine(ImVec2(playheadX, childPos.y), ImVec2(playheadX, childPos.y + contentHeight),
						  kColourPlayhead, 2.0f);

			// ---- Interaction ----

			if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
			{
				// A click in the ruler scrubs. Dragging continues it, so the
				// playhead tracks the cursor without a second press.
				const bool inRuler = (mouse.y >= childPos.y && mouse.y < childPos.y + laneTop);
				if (inRuler && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				{
					m_Playhead = std::clamp((mouse.x - childPos.x + scrollX) / pixelsPerSecond,
											0.0f, duration);
					SamplePreview(m_Playhead);
				}
				if (inRuler && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
				{
					m_Playhead = std::clamp((mouse.x - childPos.x + scrollX) / pixelsPerSecond,
											0.0f, duration);
					SamplePreview(m_Playhead);
				}

				// Mouse wheel zooms, anchored on the playhead so the clip does
				// not appear to jump: the time under the playhead is preserved
				// by adjusting the scroll offset after the scale change.
				if (ImGui::GetIO().MouseWheel != 0.0f)
				{
					const float oldScale = pixelsPerSecond;
					const float timeAtPlayhead = m_Playhead;

					m_PixelsPerSecond = std::clamp(
						m_PixelsPerSecond * (ImGui::GetIO().MouseWheel > 0.0f ? 1.25f : 0.8f),
						kMinPixelsPerSecond, kMaxPixelsPerSecond);

					// The playhead stays put in time, so its pixel position moves;
					// the scroll offset is corrected by the difference.
					const float newScale = std::clamp(m_PixelsPerSecond, kMinPixelsPerSecond,
													 kMaxPixelsPerSecond);
					ImGui::SetScrollX(std::max(scrollX + (oldScale - newScale) * timeAtPlayhead, 0.0f));
				}

				// Keyframe hit test: nearest rotation key within the radius,
				// across all bones. Rotation only, because it is the channel the
				// panel edits; position and scale keys are drawn but not draggable
				// in v1, and pretending otherwise would invite an edit that does
				// nothing.
				Animation::BoneIndex hitBone;
				float hitTime = 0.0f;
				bool hit = false;
				float bestDistance = kKeyHitRadius;

				for (size_t i = 0; i < bones.size(); ++i)
				{
					const Animation::BoneIndex bone = bones[i];
					const Animation::BoneTrack *track = clip->GetTrack(bone);
					if (!track)
						continue;

					const float laneCentreY =
						childPos.y + laneTop + (float)i * kTrackRowHeight + kTrackRowHeight * 0.5f - 1.0f;

					if (std::fabs(mouse.y - laneCentreY) > kKeyHitRadius)
						continue;

					for (const auto &key : track->Rotation.GetKeys())
					{
						const float x = childPos.x + key.Time * pixelsPerSecond - scrollX;
						const float distance = std::fabs(mouse.x - x);
						if (distance <= bestDistance)
						{
							bestDistance = distance;
							hitBone = bone;
							hitTime = key.Time;
							hit = true;
						}
					}
				}

				if (hit && ImGui::IsMouseDown(ImGuiMouseButton_Left))
				{
					m_Dragging = true;
					m_DragBone = hitBone.Value;
					m_DragFrom = hitTime;
					m_DragTo = hitTime;
					m_DragMoved = false;
				}

				// Right-click: on a key, remove it; on a lane, add one at the
				// playhead. Both re-serialize, so the file matches the picture
				// immediately rather than on the next save.
				if (hit && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
				{
					Animation::BoneTrack *track = clip->GetTrack(hitBone);
					if (track)
					{
						// Re-fetch rather than reuse a pointer: GetTrack returns
						// a pointer into an unordered_map, and the header warns
						// that an authoring call can invalidate it.
						track->Rotation.RemoveKeyAtTime(hitTime);
						m_Status = "removed key at " + std::to_string(hitTime);
						SaveAsset();
						SamplePreview(m_Playhead);
					}
				}
				else if (!hit && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
				{
					// Which lane was clicked? Recover it from the y coordinate,
					// which is the only thing the hit test did not record.
					const int laneIndex = (int)std::floor(
						(mouse.y - (childPos.y + laneTop)) / kTrackRowHeight);
					if (laneIndex >= 0 && laneIndex < (int)bones.size())
					{
						const Animation::BoneIndex bone = bones[(size_t)laneIndex];
						const float time = std::clamp(
							(mouse.x - childPos.x + scrollX) / pixelsPerSecond, 0.0f, duration);

						// The value comes from sampling at the clicked time, so
						// the new key holds the pose that is already on screen
						// rather than a default. A key that snapped to identity
						// would pop the rig.
						Animation::Pose sample = m_Asset.Skeleton->GetRestPose();
						clip->Sample(*m_Asset.Skeleton, time, sample);

						Animation::BoneTrack *track = clip->GetTrack(bone);
						if (track)
						{
							// No explicit interpolation arguments: both default to
							// Interpolation::Cubic, which is what authored motion
							// wants. The enum is Animation::Interpolation, not a
							// nested type of the clip.
							track->Rotation.AddKey(time, sample.GetBone(bone)->Rotation);
							m_Status = "added key at " + std::to_string(time);
							SaveAsset();
							SamplePreview(m_Playhead);
						}
					}
				}
			}

			// ---- Drag resolution ----

			if (m_Dragging)
			{
				if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
				{
					const float newTime =
						std::clamp((mouse.x - childPos.x + scrollX) / pixelsPerSecond, 0.0f, duration);
					if (std::fabs(newTime - m_DragTo) > 1e-6f)
					{
						m_DragTo = newTime;
						m_DragMoved = true;
					}
				}

				if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
				{
					// Only write on a real move. A click without movement is a
					// selection, and rewriting the file for it would churn the
					// asset for nothing.
					if (m_DragMoved)
					{
						Animation::BoneTrack *track =
							clip->GetTrack(Animation::BoneIndex(m_DragBone));
						if (track && track->Rotation.MoveKey(m_DragFrom, m_DragTo))
						{
							m_Status = "moved key " + std::to_string(m_DragFrom) + " -> " +
									   std::to_string(m_DragTo);
							SaveAsset();
							SamplePreview(m_Playhead);
						}
					}
					m_Dragging = false;
					m_DragMoved = false;
				}
			}
		}
		ImGui::EndChild();
	}

	void AnimationTimelinePanel::DrawPosePreview()
	{
		ImGui::TextDisabled("Pose preview -- side view (x right, y up). Scrubbing moves this pose only; it does not advance playback.");

		if (!m_PreviewValid || !m_Asset.Skeleton)
		{
			ImGui::TextDisabled("No pose to show.");
			return;
		}

		const Animation::Skeleton &skeleton = *m_Asset.Skeleton;
		const size_t boneCount = (size_t)skeleton.GetBoneCount();

		// World matrices from the panel's own pose. ComputeWorldMatrices is
		// const and has no side effects, which is exactly why the panel can call
		// it freely.
		std::vector<glm::mat4> world(boneCount, glm::mat4(1.0f));
		skeleton.ComputeWorldMatrices(m_PreviewPose, world, glm::mat4(1.0f));

		// Project to the panel: x stays horizontal, y becomes vertical, and z is
		// discarded. A side view is the most legible for a humanoid spine, and
		// the label says which axes are shown rather than letting the reader
		// guess.
		ImDrawList *draw = ImGui::GetWindowDrawList();
		const ImVec2 area = ImGui::GetContentRegionAvail();
		const ImVec2 origin = ImGui::GetCursorScreenPos();

		// A fixed world-to-panel scale, chosen so a 2m character fills most of
		// the preview height. Derived from the pose's extent rather than a
		// constant, so a small prop is not a dot and a large creature is not
		// clipped.
		float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
		for (size_t i = 0; i < boneCount; ++i)
		{
			const glm::vec3 p = glm::vec3(world[i][3]);
			minX = std::min(minX, p.x);
			maxX = std::max(maxX, p.x);
			minY = std::min(minY, p.y);
			maxY = std::max(maxY, p.y);
		}

		const float extentX = std::max(maxX - minX, 1e-3f);
		const float extentY = std::max(maxY - minY, 1e-3f);
		const float scale = std::min((area.x - 40.0f) / extentX, (area.y - 30.0f) / extentY);

		// Centre the skeleton in the preview area.
		const ImVec2 centre(origin.x + area.x * 0.5f, origin.y + area.y * 0.5f);
		const ImVec2 offset((minX + maxX) * 0.5f, (minY + maxY) * 0.5f);

		auto project = [&](size_t i) -> ImVec2
		{
			const glm::vec3 p = glm::vec3(world[i][3]);
			return ImVec2(centre.x + (p.x - offset.x) * scale,
						  // y up, so the panel y is inverted.
						  centre.y - (p.y - offset.y) * scale);
		};

		// Bones first, joints on top, so a joint is never hidden by its bone.
		for (size_t i = 0; i < boneCount; ++i)
		{
			const Animation::BoneIndex parent = skeleton.GetParent(Animation::BoneIndex((int32_t)i));
			if (!parent.IsValid())
				continue;
			const ImVec2 a = project((size_t)parent.Value);
			const ImVec2 b = project(i);
			draw->AddLine(a, b, kColourBone, 2.0f);
		}

		for (size_t i = 0; i < boneCount; ++i)
		{
			const ImVec2 p = project(i);
			const bool isRoot = !skeleton.GetParent(Animation::BoneIndex((int32_t)i)).IsValid();
			draw->AddCircleFilled(p, isRoot ? 4.0f : 3.0f, isRoot ? kColourRotation : kColourJoint);

			// Only the root and the first few bones get a label: a full skeleton
			// would be unreadable, and the names are in the track list anyway.
			if (i < 6)
				draw->AddText(ImVec2(p.x + 5.0f, p.y - 6.0f), kColourGrid,
							  skeleton.GetBoneName(Animation::BoneIndex((int32_t)i)).c_str());
		}
	}
}
