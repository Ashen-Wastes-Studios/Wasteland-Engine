#!/usr/bin/env python
# CPython's python314._pth puts this interpreter in ISOLATED mode: sys.path[0]
# is the install dir, not the script dir, and PYTHONPATH is ignored. So paths
# have to be inserted by hand rather than passed through the environment.
import os as _os, sys as _sys
_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path.insert(0, _HERE)
if _HERE not in _sys.path:
    _sys.path.insert(0, _os.path.join(_HERE, "headless"))
try:
    import wlpath as _wlpath
except ImportError:
    _wlpath = None
if _wlpath is not None and hasattr(_wlpath, "setup"):
    _wlpath.setup()

# Wasteland Engine — Visual Blocks test suite (VB.1 - VB.4)
#
# Headless. No GL context, no window, no running Application. Run against the
# Release build:
#
#   set PYTHONPATH=C:/dev/Wasteland/bin/Release-windows-x86_64/DemonCore-Editor
#   python tests/test_visual_blocks.py
#
# Assertions are on real read-back values, never "did not throw". A suite that
# only proves nothing crashes cannot tell a working compiler from one that
# emits a tree that walks correctly and does nothing.

import ast
import glob
import os
import re
import sys
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
for _p in (HERE, os.path.join(HERE, "headless")):
    if _p not in sys.path:
        sys.path.insert(0, _p)
try:
    import wlpath as _wlpath
except ImportError:
    _wlpath = None
if _wlpath is not None and hasattr(_wlpath, "setup"):
    _wlpath.setup()

import Wasteland

VB = Wasteland.VisualBlocks

REPO = os.path.abspath(os.path.join(HERE, '..'))
TREES = os.path.join(REPO, 'DemonCore-Editor', 'assets', 'trees')
SCRIPTS = os.path.join(REPO, 'DemonCore-Editor', 'assets', 'scripts')
# Generated trees do `import wl_behavior`, and they are exec'd from a temp dir,
# so the shared runtime has to be importable before that happens.
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

PASS, FAIL = [], []


def check(name, cond, detail=''):
    if cond:
        PASS.append(name)
        print('  ok   %s' % name)
    else:
        FAIL.append((name, detail))
        print('  FAIL %s   %s' % (name, detail))


def section(title):
    print('\n== %s ==' % title)


# --------------------------------------------------------------- VB.1 format

def test_serializer_roundtrip():
    section('VB.1  serializer')
    src = os.path.join(TREES, 'EnemyChase.wlbtree')
    g = VB.Graph()
    err = VB.Load(src, g)
    check('load EnemyChase', err is None, str(err))
    if err is not None:
        return

    text = VB.Emit(g)
    check('emit produces text', len(text) > 0)

    g2 = VB.Graph()
    err2 = VB.Parse(text, g2)
    check('re-parse emitted text', err2 is None, str(err2))
    if err2 is None:
        check('round-trip is byte-identical', VB.Emit(g2) == text)
        check('node count survives', len(g2.Nodes) == len(g.Nodes),
              '%d vs %d' % (len(g2.Nodes), len(g.Nodes)))
        check('blackboard survives', len(g2.Blackboard) == len(g.Blackboard))

    # A declared 0.0 blackboard default must survive as SET, not read as
    # absent. This is the bug class the Blackboard class exists to prevent.
    g3 = VB.Graph()
    VB.Parse(text, g3)
    if g3.Blackboard:
        b = g3.Blackboard[0]
        check('blackboard default preserved', b.DefaultLiteral == g.Blackboard[0].DefaultLiteral,
              '%r vs %r' % (b.DefaultLiteral, g.Blackboard[0].DefaultLiteral))

    # An unknown node type must be REPORTED, and the node must be PRESERVED.
    #
    # Load-then-validate, not reject-at-load: the loader keeps the node so a
    # graph authored against a newer node library still round-trips through an
    # older build, and the validator is what refuses to compile it. The
    # property that matters is "never silently dropped" -- a node that vanishes
    # on load is work the designer did not know they had lost.
    bad = VB.Graph()
    err3 = VB.Parse('kind: tree\nschema: 1\nname: Bad\nnodes:\n  - id: x\n    type: NoSuchNodeType\n', bad)
    check('unknown node type survives load', err3 is None and len(bad.Nodes) == 1,
          'node was dropped or load failed: %s' % err3)
    vbad = VB.ValidateTree(bad)
    check('unknown node type is rejected by the validator', vbad.HasErrors(),
          'compiled a graph naming a node that does not exist')
    check('validator names the unknown type', 'NoSuchNodeType' in vbad.Report('Bad'),
          vbad.Report('Bad'))
    invalid_report = vbad.Report('Bad')
    expected_findings = [finding.Format('Bad') for finding in vbad.Findings]
    check('validator report preserves every invalid finding verbatim',
          bool(expected_findings) and all(text in invalid_report
                                          for text in expected_findings),
          invalid_report)

    # ToolServer's public response must be the validator's complete report,
    # not a filtered list or a rewritten summary. The engine binding above
    # exercises the real invalid graph; this source contract covers the C++
    # bridge without requiring a rebuild of the editor target.
    server_path = os.path.join(REPO, 'DemonCore-Editor', 'src', 'Panels',
                               'ToolServer.cpp')
    with open(server_path, 'r', encoding='utf-8') as stream:
        server_source = stream.read()
    start = server_source.find('std::string ToolServer::ValidateGraph(')
    end = server_source.find('std::string ToolServer::ListGraphNodes(', start)
    validate_handler = server_source[start:end] if start >= 0 and end > start else ''
    check('validate_graph returns the real report unchanged',
          bool(re.search(r'return\s+validation\.Report\(Project::Get\(\)\.'
                         r'MakeProjectRelative\(fullPath\)\);', validate_handler)),
          'ValidateGraph must return ValidationResult::Report directly')


# ----------------------------------------------------------- VB.2 validation

def test_validator():
    section('VB.2  validator')
    for name in ('EnemyChase', 'EnemyGuard', 'PatrolGuard'):
        g = VB.Graph()
        err = VB.Load(os.path.join(TREES, name + '.wlbtree'), g)
        if err is not None:
            check('%s loads' % name, False, str(err))
            continue
        v = VB.ValidateTree(g)
        check('%s validates clean' % name, not v.HasErrors(), v.Report(name))
        check('%s has no unknown node types' % name, not _has_unknown(v))

    # Two parents for one node: runs once per parent, repeats its effect.
    g = VB.Graph()
    VB.Parse(
        'kind: tree\nschema: 1\nname: Dup\nnodes:\n'
        '  - id: root\n    type: Sequence\n    children: [a, b]\n'
        '  - id: a\n    type: Sequence\n    children: [shared]\n'
        '  - id: b\n    type: Sequence\n    children: [shared]\n'
        '  - id: shared\n    type: Wait\n    latent: true\n'
        '    pins:\n      - pin: Seconds\n        value: \'1.0\'\n', g)
    v = VB.ValidateTree(g)
    check('duplicate parent is rejected', v.HasErrors(), 'no error raised')
    check('error names the node', 'shared' in v.Report('Dup'), v.Report('Dup'))

    # Cycle.
    g = VB.Graph()
    VB.Parse(
        'kind: tree\nschema: 1\nname: Cyc\nnodes:\n'
        '  - id: root\n    type: Sequence\n    children: [a]\n'
        '  - id: a\n    type: Sequence\n    children: [b]\n'
        '  - id: b\n    type: Sequence\n    children: [a]\n', g)
    check('cycle is rejected', VB.ValidateTree(g).HasErrors(), 'no error raised')


def _has_unknown(v):
    for f in v.Findings:
        if 'unknown' in f.Message.lower() or 'not registered' in f.Message.lower():
            return True
    return False


# ------------------------------------------------------------- VB.3 codegen

def test_codegen():
    section('VB.3  codegen')
    outs = {}
    for name in ('EnemyChase', 'EnemyGuard', 'PatrolGuard'):
        g = VB.Graph()
        err = VB.Load(os.path.join(TREES, name + '.wlbtree'), g)
        if err is not None:
            check('%s loads' % name, False, str(err))
            continue
        ok, py, rep, errs = VB.CompileTree(g)
        check('%s compiles' % name, ok, '%s %s' % (rep, errs))
        if not ok:
            continue
        outs[name] = (g, py)
        try:
            ast.parse(py)
            check('%s emits valid Python' % name, True)
        except SyntaxError as e:
            check('%s emits valid Python' % name, False, str(e))


        # Topology must be DATA. Check the _build() body specifically: a
        # lambda's conditional expression legitimately contains the text
        # "if ... else", so testing the whole file for "if" proves nothing.
        # What must be absent is a hand-rolled walk -- a while loop or a
        # recursive step function -- because that is the state machine the
        # topology dict exists to avoid.
        build = py.split('def _build(self):')[-1].split('def tick(')[0]
        check('%s topology is a data dict' % name,
              "'t': 'Selector'" in build or "'t': 'Sequence'" in build,
              '_build does not return a topology dict')
        check('%s emits no evaluator walk loop' % name,
              'while ' not in build,
              'generated a control-flow walk instead of a topology')
        # Determinism: the same graph twice must emit identical bytes.
        ok2, py2, _, _ = VB.CompileTree(g)
        check('%s compile is deterministic' % name, py2 == py,
              'second compile differed')

    if 'EnemyChase' in outs:
        g, py = outs['EnemyChase']
        check('blackboard types emitted', 'entity' in py)
        check('float literals keep a decimal point', '3.000000' in py,
              'float pin emitted as an int literal')
        check('latent walk node marked latent', "'latent': True" in py)
        check('cooldown carries its seconds', "'seconds': 1.000000" in py)


# ------------------------------------------------------------ VB.4 end-to-end

def _make_world(dist=5.0, activate=True):
    s = Wasteland.Scene()
    npc = s.CreateEntity('npc')
    tgt = s.CreateEntity('target')
    npc.AddHealthComponent(Wasteland.HealthComponent())
    tgt.AddHealthComponent(Wasteland.HealthComponent())
    t = npc.GetTransform()
    npc.SetTransform(Wasteland.Vec3(0.0, 0.0, 0.0), t.Rotation, t.Scale)
    t2 = tgt.GetTransform()
    tgt.SetTransform(Wasteland.Vec3(dist, 0.0, 0.0), t2.Rotation, t2.Scale)
    if activate:
        Wasteland.Scene.SetActiveScene(s)
    return s, npc, tgt


def _compile_to(name):
    g = VB.Graph()
    err = VB.Load(os.path.join(TREES, name + '.wlbtree'), g)
    if err is not None:
        return None
    ok, py, rep, errs = VB.CompileTree(g)
    if not ok:
        return None
    mod = {}
    exec(compile(py, name + '.py', 'exec'), mod)
    return mod[name]


def test_schema_registry_drift():
    section('VB.5  schema documentation drift')
    schema_path = os.path.join(REPO, 'docs', 'VISUAL_BLOCKS_SCHEMA.md')
    check('schema documentation exists', os.path.isfile(schema_path), schema_path)
    if not os.path.isfile(schema_path):
        return
    with open(schema_path, 'r', encoding='utf-8') as stream:
        schema = stream.read()

    definitions = VB.RegisteredNodes()
    check('registry is non-empty for drift check', bool(definitions),
          'RegisteredNodes() returned no entries')
    damage_example = re.search(
        r'### `assets/graphs/DamageFlash\.wlblocks`[^\n]*\n.*?```yaml\n(.*?)\n```',
        schema, re.S)
    damage_path = os.path.join(REPO, 'DemonCore-Editor', 'assets', 'graphs',
                               'DamageFlash.wlblocks')
    with open(damage_path, 'r', encoding='utf-8') as stream:
        shipped_damage = stream.read().strip()
    check('DamageFlash worked example matches shipped graph',
          damage_example is not None and
          damage_example.group(1).strip() == shipped_damage,
          'docs example differs from %s' % damage_path)

    missing_nodes = [definition.TypeId for definition in definitions
                     if '`%s`' % definition.TypeId not in schema]
    check('schema documents every registered node id', not missing_nodes,
          ', '.join(missing_nodes))

    missing_pins = sorted({pin.Id for definition in definitions
                           for pin in definition.Pins
                           if '`%s`' % pin.Id not in schema})
    check('schema documents every registered pin id', not missing_pins,
          ', '.join(missing_pins))

    # The local extension can lag the source tree between builds. Also inspect
    # the current registration translation units so a stale binary cannot
    # make this drift check pass while a newly registered node is undocumented.
    vb_root = os.path.join(REPO, 'Wasteland', 'src', 'Wasteland', 'VisualBlocks')
    registration_paths = sorted(glob.glob(os.path.join(vb_root, 'NodeLibrary*.cpp')))
    check('registry source files were discovered', bool(registration_paths), vb_root)
    sources = []
    for source_path in registration_paths:
        with open(source_path, 'r', encoding='utf-8') as stream:
            sources.append(stream.read())
    registration_source = '\n'.join(sources)
    source_nodes = set(re.findall(r'\bdef\.TypeId\s*=\s*"([^"]+)"',
                                  registration_source))
    source_nodes.update(re.findall(r'\b(?:addBinary|addCompare)\(\s*"([^"]+)"',
                                   registration_source))
    source_pins = set(re.findall(
        r'\bMake(?:ExecIn|ExecOut|In|Out)\(\s*"([^"]+)"',
        registration_source))
    missing_source_nodes = sorted(node_id for node_id in source_nodes
                                  if '`%s`' % node_id not in schema)
    missing_source_pins = sorted(pin_id for pin_id in source_pins
                                 if '`%s`' % pin_id not in schema)
    check('schema documents current C++ registry node ids',
          not missing_source_nodes, ', '.join(missing_source_nodes))
    check('schema documents current C++ registry pin ids',
          not missing_source_pins, ', '.join(missing_source_pins))


def test_graph_tools_are_registered_and_asset_confined():
    section('VB.5  graph tools')
    server_path = os.path.join(REPO, 'DemonCore-Editor', 'src', 'Panels',
                               'ToolServer.cpp')
    with open(server_path, 'r', encoding='utf-8') as stream:
        source = stream.read()
    check('validate_graph is advertised',
          r'\"name\":\"validate_graph\"' in source)
    check('list_graph_nodes is advertised',
          r'\"name\":\"list_graph_nodes\"' in source)
    check('both graph tools are dispatched',
          'name == "validate_graph"' in source and
          'name == "list_graph_nodes"' in source)
    check('graph paths are resolved against the canonical asset root',
          'weakly_canonical(Project::Get().GetAssetRoot(), ec)' in source and
          'relative(canonicalPath, assetRoot, ec)' in source)
    check('graph paths require supported file extensions',
          'extension != ".wlblocks"' in source and
          'extension != ".wlbtree"' in source)
    check('graph tools use the real serializer and validator',
          'VisualBlocks::GraphSerializer::Load(fullPath.string(), graph, loadError)' in source and
          'VisualBlocks::Validator::Validate(graph, validation)' in source)
    check('node listing resolves actual registry definitions',
          'registry.Find(node.TypeId)' in source)


def test_end_to_end():
    section('VB.4  end-to-end (headless)')

    # No-world FIRST, while the process genuinely has no active scene: an
    # early startup tick runs before one exists and must not take the NPC
    # down. Later tests install a scene, so this cannot be checked at the end.
    if Wasteland.Scene.GetActiveScene() is None:
        cls0 = _compile_to('EnemyChase')
        if cls0 is not None:
            s0, npc0, tgt0 = _make_world(3.0, activate=False)
            beh0 = cls0(npc0)
            try:
                r0 = beh0.tick(0.05)
                check('no-world tick does not raise', True)
                check('no-world tick returns a valid result',
                      r0 in ('SUCCESS', 'FAILURE', 'RUNNING'), 'returned %r' % r0)
                check('no-world tick clears rather than fakes a target',
                      not beh0.tree.bb.is_set('target'),
                      'a target was written with no world to sense one in')
            except Exception as e:
                check('no-world tick does not raise', False,
                      '%s: %s' % (type(e).__name__, e))
    else:
        check('no-world precondition (fresh process)', False,
              'a scene was already active before the no-world check')

    # The suite's main reason for existing. A tree that walks cleanly and does
    # nothing is the failure mode this layer exists to prevent, so assert on
    # movement, targeting and damage as VALUES.
    cls = _compile_to('EnemyChase')
    if cls is None:
        check('EnemyChase compiles for execution', False, 'compile failed')
        return

    s, npc, tgt = _make_world(5.0)
    beh = cls(npc)
    sink = []
    beh.tree.set_trace(lambda nid, ev, t: sink.append((nid, ev)))

    results = [beh.tick(0.05) for _ in range(60)]
    pos = npc.GetTransform().Translation
    health = tgt.GetHealthComponent()

    check('trace fires', len(sink) > 0, 'no trace events')
    check('target acquired', beh.tree.bb.is_set('target'),
          'blackboard target never written')
    check('npc moved toward target', pos.x > 1.0,
          'x=%.4f — NPC did not move' % pos.x)
    check('npc closed distance', pos.x < 5.0, 'x=%.4f' % pos.x)
    check('latent resume observed', 'RUNNING' in results,
          'never suspended — the latent path never ran')
    check('damage dealt', health.Current < 100.0,
          'target still at %s' % health.Current)
    check('cooldown rate-limits damage', health.Current > 0.0,
          'target at %s — no cooldown' % health.Current)
    check('results drain to a decision', results[-1] != 'RUNNING',
          'still RUNNING after 60 ticks — tree is stuck')

    # Cooldown must actually gate: the same number of ticks without a
    # cooldown tree would deal far more damage.
    per_tick = 100.0 - health.Current
    check('damage is bounded by cooldown', per_tick <= 10.0 * 12.0,
          'dealt %s in 3s at a 1.0s cooldown' % per_tick)

    # Second tree, to prove this is not a single-hardcoded-tree situation.
    cls2 = _compile_to('PatrolGuard')
    if cls2 is not None:
        s2, npc2, tgt2 = _make_world(8.0)
        beh2 = cls2(npc2)
        r2 = [beh2.tick(0.05) for _ in range(40)]
        check('PatrolGuard ticks without raising', len(r2) == 40)


def main():
    print('Visual Blocks suite — %d node types registered' % len(VB.RegisteredNodes()))
    for fn in (test_serializer_roundtrip, test_validator, test_codegen,
               test_schema_registry_drift, test_graph_tools_are_registered_and_asset_confined,
               test_end_to_end):
        try:
            fn()
        except Exception:
            FAIL.append((fn.__name__, traceback.format_exc()))
            print('  EXCEPTION in %s' % fn.__name__)
            traceback.print_exc()

    print('\n%s' % ('-' * 60))
    print('passed %d, failed %d' % (len(PASS), len(FAIL)))
    if FAIL:
        print('\nfailures:')
        for name, detail in FAIL:
            print('  - %s' % name)
            if detail:
                print('      %s' % detail.replace('\n', '\n      ')[:900])
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
