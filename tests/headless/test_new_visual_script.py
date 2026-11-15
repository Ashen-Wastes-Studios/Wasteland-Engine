"""A "New Visual Script" must produce a graph that parses, validates and compiles.

The Content Browser's CreateVisualScript writes the text returned by
GraphSerializer::NewFlowGraphText. That function is the SINGLE source of
truth -- the panel calls it, and this test calls the same binding -- so a
template that stopped loading, stopped validating or stopped compiling fails
here rather than producing a graph the editor can no longer open.

The assertions are on the PIPELINE, not on the text. A template that parses
but fails validation still writes a file the Visual Blocks panel would then
refuse to save, and one that compiles to unparseable Python fails at attach
time with no obvious cause. Each stage is checked separately so a failure
names the stage that broke.

Run:  python tests/headless/runner.py test_new_visual_script
"""

from __future__ import annotations

import os as _os
import sys as _sys
import tempfile
import types

_HERE = _os.path.dirname(_os.path.abspath(__file__))
if _HERE not in _sys.path:
    _sys.path.insert(0, _HERE)

import wlpath

wlpath.setup()

from Wasteland import Project, Scene, VisualBlocks  # noqa: E402
from runner import check, check_eq  # noqa: E402

REPO = _os.path.dirname(_os.path.dirname(_HERE))
ASSET_ROOT = _os.path.join(REPO, "DemonCore-Editor")

# The panel source, for the one assertion that has to read C++ rather than
# call into it: that CreateVisualScript writes the ENGINE template rather
# than a second copy of the text. A literal in the panel would drift from
# the one this test exercises, and nothing else would catch it.
PANEL_CPP = _os.path.join(
    REPO, "DemonCore-Editor", "src", "Panels", "ContentBrowserPanel.cpp")


def attach_root():
    Project.Get().AttachAssetRoot(ASSET_ROOT)


def parse_template(name):
    """The template the panel would write, parsed into a Graph.

    Returns (text, graph). The parse failure is reported by the caller's
    check, not here, so each test names the stage it is about.
    """
    text = VisualBlocks.NewFlowGraphText(name)
    graph = VisualBlocks.Graph()
    err = VisualBlocks.Parse(text, graph)
    return text, graph, err


# --------------------------------------------------------------------------
def test_template_parses():
    """Stage 1: the text must load. Anything else is unreadable later."""
    attach_root()
    _text, _graph, err = parse_template("Mover")
    check(err is None, "the new-graph template must parse (got %r)" % (err,))


def test_template_validates_without_errors():
    """Stage 2: it must pass validation with no ERRORS.

    Warnings are reported separately: a fresh graph with a single event node
    and no exec chain has no unreachable statement nodes, so there should be
    nothing to warn about either. That is a stronger check than "no errors",
    and it is what makes the template a clean starting point rather than one
    that opens with a wall of yellow.
    """
    attach_root()
    _text, graph, err = parse_template("Mover")
    check(err is None, "template must parse before it can be validated")

    result = VisualBlocks.ValidateFlow(graph)
    check_eq(result.ErrorCount(), 0,
             "the new-graph template must have no validation errors")
    check_eq(result.WarningCount(), 0,
             "the new-graph template must have no validation warnings")


def test_template_compiles_to_runnable_python():
    """Stage 3: it must compile, and the result must actually run.

    exec() plus a real call, rather than a substring check: the emitter's
    `pass` fallback only fires for an event with no exec chain, and a
    template that produced a syntax error would compile to nothing at all.
    """
    attach_root()
    _text, graph, err = parse_template("Mover")
    check(err is None, "template must parse before it can be compiled")

    source = VisualBlocks.CompileFlow(graph)
    check(isinstance(source, str) and len(source) > 0,
          "the template must compile to non-empty Python")
    check("def OnUpdate(self" in source,
          "the template's event must produce a method")
    check("pass" in source,
          "the empty exec chain must emit the pass fallback")

    # And it must RUN. exec() is the check a substring test cannot make.
    module = types.ModuleType("_test_new_script")
    exec(source, module.__dict__)
    cls = getattr(module, "Mover", None)
    check(cls is not None,
          "the compiled class must be named after the graph")

    scene = Scene()
    actor = scene.CreateEntity("actor")
    inst = cls(actor, scene)
    # No nodes beyond the event, so this is a no-op -- the point is that it
    # does not raise.
    inst.OnUpdate(0.016)


def test_display_name_is_not_an_identifier():
    """A designer types a phrase, not a Python identifier.

    "My Script" must still compile, and the generated class name must be the
    sanitised form. This is the case a bare template literal gets wrong: the
    name is substituted into a quoted YAML scalar, and the compiler
    sanitises it for the class.
    """
    attach_root()
    _text, graph, err = parse_template("My Script")
    check(err is None, "a name with a space must still parse")

    source = VisualBlocks.CompileFlow(graph)
    # SanitizeIdentifier replaces each run of non-identifier characters with
    # a single underscore, so "My Script" becomes My_Script -- a legal Python
    # identifier that keeps the two words readable.
    check("class My_Script" in source,
          "the class name must be the sanitised form of the display name, "
          "got: %s" % [l for l in source.splitlines() if l.startswith("class ")])

    module = types.ModuleType("_test_new_script_space")
    exec(source, module.__dict__)
    check(getattr(module, "My_Script", None) is not None,
          "the sanitised class must exist")


def test_template_round_trips_through_a_file():
    """The bytes written to disk must load back and compile.

    CreateVisualScript writes with std::ofstream in binary mode, which is
    what keeps the template's LF endings from becoming CRLF on Windows. A
    round trip through a real file checks both the bytes and the path the
    editor will actually take.
    """
    attach_root()
    text, _graph, err = parse_template("RoundTrip")
    check(err is None, "template must parse before it can be written")

    tmp = tempfile.mkdtemp(prefix="wl_new_script_")
    path = _os.path.join(tmp, "RoundTrip.wlblocks")
    # newline="" so Python does not translate "\n" to "\r\n" on Windows --
    # the same guarantee the C++ binary-mode write makes.
    with open(path, "w", newline="") as handle:
        handle.write(text)

    loaded = VisualBlocks.Graph()
    load_err = VisualBlocks.Load(path, loaded)
    check(load_err is None,
          "the written file must load back (got %r)" % (load_err,))

    source = VisualBlocks.CompileFlow(loaded)
    check("def OnUpdate(self" in source,
          "the reloaded graph must still compile")


def test_panel_writes_through_the_engine_template():
    """The panel must call GraphSerializer::NewFlowGraphText, not a literal.

    This is the assertion that keeps the other five honest: if the panel
    carried its own copy of the template, a change here would not reach it
    and this test would keep passing against a template nobody creates with.
    """
    check(_os.path.isfile(PANEL_CPP), "ContentBrowserPanel.cpp must exist")
    with open(PANEL_CPP, "r", encoding="utf-8", errors="replace") as handle:
        src = handle.read()

    check("CreateVisualScript" in src,
          "the panel must expose CreateVisualScript")
    check("NewFlowGraphText" in src,
          "CreateVisualScript must write the ENGINE template, so the two "
          "cannot drift")
    check("New Visual Script" in src,
          "the toolbar must offer New Visual Script")
    # The destination is the project's graphs/ directory, not the browsed
    # one -- a visual script has a conventional home.
    check('GetAssetPath("graphs")' in src,
          "the graph must be created under the project's graphs/ directory")
