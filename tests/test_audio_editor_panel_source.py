"""Headless source contracts for the Audio editor workflow and its panels.

These exist because two of the Audio workflow's edits were silently dropped
while the rest of the change was written: the Layout menu entry and the dock
block. Both compiled, both looked present in the diff, and neither was in the
binary -- the workflow was reachable only through WASTELAND_EDITOR_WORKFLOW,
which is why the menu looked like it was missing the option entirely.

So the contracts here assert on the wiring, not just on the panel bodies: the
menu entry, the enum case, the dock targets, the visibility rule, and the
project registrations.
"""

from pathlib import Path
import unittest


REPO = Path(__file__).resolve().parents[1]
EDITOR = REPO / "DemonCore-Editor" / "src" / "EditorLayer.cpp"
EDITOR_HEADER = REPO / "DemonCore-Editor" / "src" / "EditorLayer.h"
PROJECT = REPO / "DemonCore-Editor" / "DemonCore-Editor.vcxproj"
FILTERS = REPO / "DemonCore-Editor" / "DemonCore-Editor.vcxproj.filters"
PANELS = REPO / "DemonCore-Editor" / "src" / "Panels"

AUDIO_PANELS = ("AudioClipPanel", "AudioMixerPanel", "AudioScenePanel")


def panel_path(name, suffix):
    return PANELS / f"{name}.{suffix}"


class AudioWorkflowWiringTests(unittest.TestCase):
    """The workflow has to be reachable from the Layout menu, not just from the
    env var. This is the contract the first attempt failed."""

    def test_audio_is_a_first_class_workflow(self):
        header = EDITOR_HEADER.read_text(encoding="utf-8")
        source = EDITOR.read_text(encoding="utf-8")

        self.assertIn("Audio,", header)
        self.assertIn('case Workflow::Audio:\n\t\t\treturn "Audio";', source)

    def test_layout_menu_lists_audio(self):
        source = EDITOR.read_text(encoding="utf-8")
        menu = source[source.index('ImGui::BeginMenu("Layout")'):]
        menu = menu[: menu.index("ImGui::EndMenu()")]

        # The exact failure mode this guards: the array is built in one
        # statement, so a dropped line here removes the entry from the menu
        # while leaving the enum, the ratios and the panels all intact.
        self.assertIn("Workflow::Audio", menu)
        self.assertIn("WorkflowName(workflow)", menu)

    def test_env_seed_still_accepts_audio(self):
        source = EDITOR.read_text(encoding="utf-8")
        seed = source[source.index("WASTELAND_EDITOR_WORKFLOW") :]
        seed = seed[: seed.index("m_PendingWorkflow = m_Workflow;")]

        self.assertIn('name == "Audio"', seed)
        self.assertIn("m_Workflow = Workflow::Audio;", seed)

    def test_graph_hook_dispatches_by_extension(self):
        """WASTELAND_EDITOR_GRAPH must open whichever graph it is handed.

        The hook predates .wlavb and was wired straight to the Visual Blocks
        panel, so passing it an AVB graph did nothing at all: no panel opened,
        no warning, and the AVB Graph panel stayed on its "open a file"
        message. Because nothing fails, the only way to notice is to assert on
        the routing -- which is also the only way this hook can be used to
        verify AVB headlessly, since opening a graph the normal way means
        clicking through the Content Browser.

        Asserting the dispatch call rather than the absence of the old one:
        a stray m_VisualBlocksPanel.OpenGraph elsewhere would not be caught by
        "the string is gone", but would be by the hook calling the dispatcher.
        """
        source = EDITOR.read_text(encoding="utf-8")
        # End at the DockSpace call rather than at the env-seed block: the
        # seed block sits EARLIER in the function, so slicing to it from here
        # finds nothing and raises instead of failing an assertion.
        hook = source[source.index("WASTELAND_EDITOR_GRAPH") :]
        hook = hook[: hook.index("ImGui::DockSpace(")]

        self.assertIn("OpenVisualFile(graphPath)", hook)
        self.assertNotIn("m_VisualBlocksPanel.OpenGraph(graphPath)", hook)

    def test_open_visual_file_handles_avb_graphs(self):
        """The dispatcher has to know .wlavb, or the hook above is a dead end.

        A dispatch that routes .wlavb to a panel which cannot load it would
        fail the same way the old hook did -- silently, with the file never
        reaching a loader.
        """
        source = EDITOR.read_text(encoding="utf-8")
        fn = source[source.index("void EditorLayer::OpenVisualFile") :]
        fn = fn[: fn.index("\n\t\telse\n\t\t{")]

        self.assertIn('ext == ".wlavb"', fn)
        self.assertIn("m_AvbGraphPanel.OpenGraph", fn)
        self.assertIn('ext == ".wlblocks"', fn)
        self.assertIn("m_VisualBlocksPanel.OpenGraph", fn)

    def test_shipped_avb_graph_is_part_of_the_asset_tree(self):
        """The graph the Audio workflow exists to edit has to actually ship.

        Without it the AVB Graph panel has nothing to open and the workflow's
        routing half is unverifiable -- which is the state this file was added
        to, where the panel existed and no .wlavb did anywhere in the tree.
        """
        graph = REPO / "DemonCore-Editor" / "assets" / "graphs" / "AvbEditorSmokeTest.wlavb"
        self.assertTrue(graph.is_file(), f"{graph} must exist in the asset tree")

        body = graph.read_text(encoding="utf-8")
        self.assertIn("schema: 1", body)
        self.assertIn("output: Stereo", body)
        # The nodes the C++ guard test asserts on, so a rename on one side
        # cannot leave the other asserting about a graph that is not there.
        for node in ("TriggerOnce", "WavePlayer", "Sine", "Gain",
                     "WaveShaper", "ADEnvelope", "StereoMixer", "Output"):
            self.assertIn(f'type: "{node}"', body)

    def test_audio_panels_are_constructed_and_rendered(self):
        header = EDITOR_HEADER.read_text(encoding="utf-8")
        source = EDITOR.read_text(encoding="utf-8")

        for name in AUDIO_PANELS:
            with self.subTest(panel=name):
                self.assertIn(f'#include "Panels/{name}.h"', header)
                self.assertIn(f"{name} m_{name}", header)
                self.assertIn(f"m_{name}.SetVisible(audio);", source)
                self.assertIn(f"m_{name}.OnImGuiRender(*m_ActiveScene);", source)

    def test_view_menu_can_toggle_each_audio_panel(self):
        # The menu label is the panel's window title, not the class name minus
        # "Panel" -- AudioClipPanel is shown as "Audio Clips".
        source = EDITOR.read_text(encoding="utf-8")
        view_menu = source[source.index('ImGui::MenuItem("Audio Clips"') - 400 :]
        view_menu = view_menu[: view_menu.index("ImGui::EndMenu()")]

        for label, member in (
            ("Audio Clips", "audioClips"),
            ("Audio Mixer", "audioMixer"),
            ("Audio Sources", "audioScene"),
        ):
            with self.subTest(label=label):
                self.assertIn(
                    f'ImGui::MenuItem("{label}", nullptr, &{member})', view_menu
                )

    def test_audio_layout_has_its_own_dock_regions(self):
        source = EDITOR.read_text(encoding="utf-8")
        layout = source[source.index("void EditorLayer::ApplyLayout") :]
        layout = layout[: layout.index("void EditorLayer::OnImGuiRender")]

        # Each audio panel must be docked into a node of its own, and the
        # workflow must skip the generic `dock(primary, top)` -- otherwise the
        # viewport lands in the half the AVB graph was given.
        for expected in (
            'dock("Audio Clips", audioClipsNode);',
            'dock("Audio Mixer", audioMixerNode);',
            'dock("Audio Sources", audioSourcesNode);',
            'dock("AVB Graph", avbNode);',
            'dock("Viewport", viewportNode);',
            "if (workflow != Workflow::Audio)\n\t\t\tdock(primary, top);",
        ):
            with self.subTest(expected=expected):
                self.assertIn(expected, layout)

    def test_audio_panels_are_excluded_from_the_generic_rail(self):
        source = EDITOR.read_text(encoding="utf-8")
        layout = source[source.index("void EditorLayer::ApplyLayout") :]
        layout = layout[: layout.index("void EditorLayer::OnImGuiRender")]

        # Left in the generic loop these pile into the side rail as tabs, which
        # is the arrangement that makes the mixer unusable.
        for name in ("Audio Clips", "Audio Mixer", "Audio Sources", "AVB Graph"):
            with self.subTest(panel=name):
                self.assertIn(f'std::strcmp(name, "{name}") == 0', layout)

        # ...and the panels Audio does not use must not land in the mixer's
        # region. Docking into `right` after it has been split sends them into
        # an arbitrary child -- which is how Settings came to share the voice
        # table's space.
        self.assertIn("dock(name, audioSourcesNode);", layout)


class AudioWorkflowLayoutTests(unittest.TestCase):
    """The AVB graph is part of the Audio workflow, not an optional extra."""

    def test_avb_graph_is_visible_in_the_audio_workflow(self):
        source = EDITOR.read_text(encoding="utf-8")

        self.assertIn(
            "m_AvbGraphPanel.SetVisible(workflow == Workflow::Visual || workflow == Workflow::Audio);",
            source,
        )

    def test_avb_graph_defaults_to_hidden(self):
        # Guards the reason the visibility line above is required: the panel is
        # constructed invisible, so a workflow that docks it without enabling it
        # shows an empty region rather than an error.
        header = (PANELS / "AvbGraphPanel.h").read_text(encoding="utf-8")
        self.assertIn("bool m_Visible = false;", header)


class AudioPanelBehaviourTests(unittest.TestCase):
    """Read-only about live audio state, and ImGui-safe."""

    def test_mixer_is_read_only_except_for_the_locked_controls(self):
        source = (PANELS / "AudioMixerPanel.cpp").read_text(encoding="utf-8")

        # The voice table is advanced by the device callback, so the panel may
        # only read it. StopAll and SetMasterVolume are internally locked and
        # are the two writes that are safe from the UI thread.
        self.assertIn("SetMasterVolume", source)
        self.assertIn("StopAll", source)
        for forbidden in ("Mixer::SetVoice", "SetVoiceGain", "SetVoicePitch"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, source)

    def test_mixer_does_not_mutate_the_voice_table_it_draws(self):
        source = (PANELS / "AudioMixerPanel.cpp").read_text(encoding="utf-8")
        draw = source[source.index("void AudioMixerPanel::Capture") :]
        draw = draw[: draw.index("void AudioMixerPanel::OnImGuiRender")]

        # Capture is a copy, taken before the controls, so the table the panel
        # iterates cannot be the one the audio thread advances.
        self.assertIn("GetVoicesSnapshot", draw)

    def test_panels_always_reach_end(self):
        # `if (!ImGui::Begin(...)) return;` skips End() when the window is
        # closed or collapsed, and ImGui asserts on the next panel's End():
        # "Mismatched Begin/BeginChild vs End/EndChild calls".
        for name in AUDIO_PANELS:
            with self.subTest(panel=name):
                lines = panel_path(name, "cpp").read_text(encoding="utf-8").split("\n")
                begin_i = next(
                    i for i, line in enumerate(lines) if f'(void)ImGui::Begin("' in line
                )
                end_i = next(
                    i
                    for i in range(begin_i + 1, len(lines))
                    if lines[i].strip() == "ImGui::End();"
                )
                body = lines[begin_i + 1 : end_i]
                self.assertNotIn("return;", [l.strip() for l in body])
                self.assertNotIn("if (!ImGui::Begin(", "\n".join(body))


class AudioPanelRegistrationTests(unittest.TestCase):
    def test_new_panel_sources_are_registered_in_both_explicit_project_files(self):
        project = PROJECT.read_text(encoding="utf-8-sig")
        filters = FILTERS.read_text(encoding="utf-8-sig")

        for name in AUDIO_PANELS:
            for suffix in ("h", "cpp"):
                entry = f"src\\Panels\\{name}.{suffix}"
                with self.subTest(entry=entry):
                    self.assertIn(entry, project)
                    self.assertIn(entry, filters)


if __name__ == "__main__":
    unittest.main(verbosity=2)
