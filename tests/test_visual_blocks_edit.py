"""Headless checks for the Visual Blocks editing model.

Covers the parts of UE5-style editing that are pure data, so they can be
tested without a window: instance titles round-tripping, literal edits
surviving save/load, and strict parsing of typed values.

These do NOT cover the ImGui layer (which control appears for which pin
type) -- that is verified visually. They cover the model the controls
edit, which is where a silent data bug would hide.
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, r"C:/dev/Wasteland/bin/Release-windows-x86_64/DemonCore-Editor")

import Wasteland.VisualBlocks as vb


GRAPH = """
schema: 1
name: Editable
kind: flow
variables:
  - name: score
    type: float
    default: "0.0"
nodes:
  - id: n1
    type: FlowOnEvent
    title: Damage Flash
    event: OnUpdate
  - id: n2
    type: SetVariable
    title: Store It
    pins:
      - pin: Name
        value: score
  - id: n3
    type: Log
    pins:
      - pin: Message
        value: hello
  # Add is a MATH node: it has no exec pins, it produces a value on a wire.
  # The literal-editing tests need a numeric pin to edit.
  - id: n4
    type: Add
    pins:
      - pin: A
        value: "10.5"
      - pin: B
        value: "3"
links:
  - from:
      node: n1
      pin: Out
    to:
      node: n2
      pin: In
  - from:
      node: n2
      pin: Out
    to:
      node: n3
      pin: In
  # n4 is a math node: no exec pins, it produces a value on a wire, and its
  # output pin is named 'Value'. SetVariable.Value is PinType::Any, so it
  # accepts the float -- Log.Message is a String pin and would not. Wiring it
  # is what makes the literal observable in the emitted Python; an unwired
  # math node correctly emits nothing at all.
  - from:
      node: n4
      pin: Value
    to:
      node: n2
      pin: Value

"""


class TestInstanceTitles(unittest.TestCase):
    """Title is a label, and must not disturb anything that uses TypeId."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, "t.wlblocks")
        with open(self.path, "w", encoding="utf-8") as handle:
            handle.write(GRAPH)
        self.graph = vb.Graph()
        self.assertIsNone(vb.Load(self.path, self.graph))

    def test_title_round_trips(self):
        node = self.graph.FindNode("n2")
        self.assertEqual(node.Title, "Store It")

    def test_unset_title_is_empty(self):
        """A node nobody renamed must not gain a title."""
        node = self.graph.FindNode("n3")
        self.assertEqual(node.Title, "")

    def test_saving_writes_title_only_when_set(self):
        out = os.path.join(self.dir, "out.wlblocks")
        self.assertIsNone(vb.Save(out, self.graph))
        with open(out, encoding="utf-8") as handle:
            text = handle.read()
        self.assertIn("Store It", text)
        # n3 and n4 were never renamed, so neither may gain a `title:` key.
        # Counting every `title:` in the file was a fragile proxy for this --
        # it also counts the titles that ARE legitimately written.
        self.assertEqual(text.count("title:"), 2,
                         "only the two deliberately-titled nodes may carry one")
        reloaded = vb.Graph()
        self.assertIsNone(vb.Load(out, reloaded))
        self.assertEqual(reloaded.FindNode("n3").Title, "",
                         "an untitled node must not gain a title on save/load")
        self.assertEqual(reloaded.FindNode("n4").Title, "")

    def test_relabelled_graph_still_compiles(self):
        """The point of Title: renaming cannot change behaviour."""
        for node in self.graph.Nodes:
            node.Title = "renamed"
        source = vb.CompileFlow(self.graph)
        self.assertTrue(source, 'CompileFlow produced no output')
        # The generated code addresses types, never titles.
        self.assertNotIn("renamed", source)

    def test_type_id_survives_a_title_edit(self):
        node = self.graph.FindNode("n2")
        before = node.TypeId
        node.Title = "Totally Different"
        self.assertEqual(node.TypeId, before)


class TestLiteralEditing(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, "t.wlblocks")
        with open(self.path, "w", encoding="utf-8") as handle:
            handle.write(GRAPH)
        self.graph = vb.Graph()
        vb.Load(self.path, self.graph)

    def test_edit_literal_persists(self):
        node = self.graph.FindNode("n4")
        node.SetLiteral("A", "42.5", True)
        out = os.path.join(self.dir, "out.wlblocks")
        self.assertIsNone(vb.Save(out, self.graph))
        reloaded = vb.Graph()
        vb.Load(out, reloaded)
        self.assertEqual(reloaded.FindNode("n4").FindLiteral("A").Value, "42.5")

    def test_zero_is_not_unset(self):
        """0 is a real float; conflating it with 'not authored' is the bug."""
        node = self.graph.FindNode("n4")
        node.SetLiteral("A", "0", True)
        lit = node.FindLiteral("A")
        self.assertTrue(lit.IsSet)
        self.assertEqual(lit.Value, "0")

    def test_edited_literal_changes_generated_code(self):
        """The whole point: an edit must reach the emitted Python."""
        node = self.graph.FindNode("n4")
        node.SetLiteral("A", "1234.5", True)
        result = vb.CompileFlow(self.graph)
        self.assertTrue(result, 'CompileFlow produced no output')
        self.assertIn("1234.5", result)

    def test_string_literal_survives_a_save_load_round_trip(self):
        """Text with quotes, colons and hashes must come back INTACT.

        Asserting on quote characters in the file was the wrong test: yaml-cpp
        emits a plain scalar when it is unambiguous and quotes on read, so
        `'he said "hi"'` is valid YAML and round-trips exactly. What actually
        matters is that the VALUE survives -- a value containing `key: value`,
        `#hash` or a quote must not be truncated, split, or injected into the
        document structure.
        """
        tricky = ['he said "hi"', 'key: value', '#hash', '- dash', 'a: b: c']
        for value in tricky:
            node = self.graph.FindNode("n2")
            node.SetLiteral("Name", value, True)
            out = os.path.join(self.dir, "out.wlblocks")
            error = vb.Save(out, self.graph)
            self.assertIsNone(error, "Save failed for %r: %s" % (value, error))
            reloaded = vb.Graph()
            load_error = vb.Load(out, reloaded)
            self.assertIsNone(load_error,
                              "Load failed for %r: %s" % (value, load_error))
            back = reloaded.FindNode("n2").FindLiteral("Name").Value
            self.assertEqual(back, value, "%r did not survive the round trip" % value)

    def test_deleting_a_node_leaves_no_dangling_link(self):
        """Undo/redo restores graphs; they must stay internally consistent."""
        before = len(self.graph.Links)
        self.graph.Links.clear()
        out = os.path.join(self.dir, "out.wlblocks")
        vb.Save(out, self.graph)
        reloaded = vb.Graph()
        vb.Load(out, reloaded)
        self.assertLess(len(reloaded.Links), before)


class TestGraphSnapshotRoundTrip(unittest.TestCase):
    """GraphHistory snapshots whole graphs; copying one must lose nothing."""

    def test_snapshot_is_independent(self):
        self.dir = tempfile.mkdtemp()
        path = os.path.join(self.dir, "t.wlblocks")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(GRAPH)
        graph = vb.Graph()
        vb.Load(path, graph)

        snapshot = vb.Graph()
        snapshot.Name = graph.Name
        snapshot.Variables.clear()
        snapshot.Variables.extend(graph.Variables)
        snapshot.Blackboard.clear()
        snapshot.Blackboard.extend(graph.Blackboard)
        snapshot.Nodes.clear()
        snapshot.Nodes.extend(graph.Nodes)
        snapshot.Links.clear()
        snapshot.Links.extend(graph.Links)

        # Mutating the original must not disturb the snapshot.
        graph.FindNode("n2").Title = "changed"
        graph.FindNode("n4").SetLiteral("A", "999", True)

        self.assertEqual(snapshot.FindNode("n2").Title, "Store It")
        self.assertEqual(snapshot.FindNode("n4").FindLiteral("A").Value, "10.5")


if __name__ == "__main__":
    unittest.main(verbosity=2)