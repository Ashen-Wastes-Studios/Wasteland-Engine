# NPC Behavioral Visual Blocks Implementation Plan

> **Canonical location:** this work is **Phase 14.5.1** in
> `.hermes/plans/item-14-long-tail-systems.md`, which is where the audit,
> the ordering argument against 14.5.2, and the phase dependencies live. This
> file is the detailed spec for that phase. If the two disagree on scope or
> ordering, 14.5 is the summary that wins and this file is the one to fix.
>
> 14.5.2 (navmesh + pathfinding) is the same phase and is **downstream** of this
> work — see its "Why second rather than first".

> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.

**Goal:** An authoring layer for NPC AI in which a designer wires a behavior
tree — composites, decorators, conditions and tasks — as **NPC Behavioral
Visual Blocks**, compiles it through the existing Visual Blocks transpiler to
Python, and watches it run. UE5 behavior trees' *authoring experience* without
UE5's reflection VM, EQS, or service layer.

**Architecture:** A new graph kind, `.wlbtree` (YAML), sharing the Visual Blocks
file format, node registry, validator and Python emitter. The tree **topology
is data** — a literal the evaluator walks — and each leaf node compiles to a
generated Python function. A ~200-line generated `wl_behavior.py` helper
evaluates the tree, holds latent-task state, and handles aborts. No VM, no new
binding layer, no scene format change: `ScriptComponent` still just points at a
`.py` file.

**Tech Stack:** C++17, yaml-cpp (vendored), ImGui 1.89.3, Python 3.14 +
pybind11 (existing runtime), entt ECS (existing). No new third-party dependency
for the runtime — the evaluator is written, not vendored.

---

## The relationship to the existing Visual Blocks plan

Read `.hermes/plans/2026-09-30_161447-visual-blocks.md` first. This plan is a
**consumer of it**, not a sibling.

| This plan reuses | From VB | If VB is not done |
|---|---|---|
| Graph/YAML serializer primitives | `VisualBlocks/GraphSerializer` | Write them here instead (small duplication) |
| Validator + error-naming infrastructure | `VisualBlocks/Validator` | Build the naming-by-node-id walk here first |
| Node registry / type system | `VisualBlocks/NodeRegistry` | Two incompatible registries forever — do not |
| Pretty-printed Python emitter | `VisualBlocks/Compiler` | Write a second emitter — do not |
| Content-browser extension dispatch | `ContentBrowserPanel.cpp` | One line either way |

**Hard dependency: VB.1–VB.4 must land first** (data model, registry/validator,
codegen, end-to-end). VB.0 (vendoring `imgui-node-editor`) and VB.5 (the canvas)
are *not* prerequisites — this plan's editor uses an indented tree list, which
does not need a node-canvas library at all. That is one of the reasons this plan
is cheap to execute.

**Shared acceptance test:** both plans claim `Enemy.py` as their end-to-end
rebuild. Sequence it: **VB.4.1 rebuilds `Enemy.py` as a flow graph first**, then
**NB.5 rebuilds it again as a tree**. Doing the tree first would confound two new
subsystems on one proof.

---

## Why a tree, and what kind

Three AI structures are on the table. Only one is asked for, and the other two
have honest uses later:

| Structure | Good at | Cost here |
|---|---|---|
| **Behavior tree** (chosen) | Discrete states, designer-tunable transitions, visible in the editor | Per-tick allocation + node dispatch; verbose for pure math |
| Flat imperative script (today) | Anything, fastest to write | `Enemy.py` is 200 lines and nobody can see its state machine without reading it |
| Utility AI | Smooth scoring, no discrete states | Needs tuning data nobody has yet |

The concrete argument for the tree, against this specific codebase: `Enemy.py`
*is* a state machine written flat. Idle → chase → attack → idle. The transitions
are `if horizontal > self.attackRange` and `if health.IsDead`, interleaved with
the movement math. A designer cannot retune "when does it give up" without
reading Python, and cannot *see* the machine at all. That is the problem this
solves — not "we should have behavior trees."

**Out of scope, named up front** (discovering these in month three costs more
than declaring them now):

- **EQS (Environment Query System)** — UE's real power, and the biggest thing
  missing here. Nearest v1 stand-in is a `FindNearestTarget` / `FindLowestHealth`
  task. If EQS ever lands, these tasks are its thin wrappers.
- **Tree inheritance / BTComposite parenting.** One flat tree per NPC type.
- **Per-agent blackboard key overrides** from a BT asset. One schema per tree.
- **Blackboard key observers** (react to a key change rather than poll).
- **Services** (interval tick attached to a branch). Use a `SetTimer` decorator
  around a task instead — same effect, one node instead of two concepts.
- **BT debugger breakpoints, tree snapshots, hot-swap of a running tree.**
- **Per-node LOD / tick throttling by distance.** A `TickEvery` decorator on the
  root covers the 80% case.
- **A separate high-level FSM layer** ("dead / combat / ambient") around the
  tree. When that is genuinely needed it is a second tree selected by a
  `SelectTree` task, not a new system.

---

## The one architectural decision

**Tree topology is data; leaf nodes are generated Python; a fixed evaluator
walks them.**

The tempting alternative is to transpile the tree into nested Python control
flow — `Selector` becomes `if/elif`, `Sequence` becomes a block. That is the
Visual Blocks approach and it does not survive contact with behavior trees,
for one concrete reason:

> A behavior tree must be able to **suspend a subtree at a latent task, resume
> that exact node next tick, and then re-evaluate from the root** to decide
> whether a higher-priority branch has become true.

Straight-line nested code cannot express "remember that node 4 was running and
resume it." Every attempt needs a hand-rolled explicit state machine in the
generated output, which is a worse tree evaluator than the ~200-line generic one
— plus far worse debugging.

So the split is:

| Layer | Where it lives | Why |
|---|---|---|
| Topology (nodes, parent/child, decorators, abort flags) | A literal in the generated module | Changes every time a designer edits the tree — recompile is cheap and it keeps state in one place |
| Leaf behavior (`MoveTo`, `IsDead`, `ApplyDamage`) | Generated Python function per leaf | This is where all the real work is, and it wants to be readable, traceback-able, hand-patchable Python |
| Traversal, latent state, aborts, blackboard | `assets/scripts/generated/wl_behavior.py`, written once and versioned | ~200 lines of *fixed, generic* logic. Not a VM: it reflects over nothing and binds no engine API |

The evaluator's cost is bounded and it is the honest price of the tree model.
Where even that is too much, a leaf can be a `CallPython` node pointing at a
hand-written hot script — the escape hatch exists and needs no new machinery.

**Leaf nodes are generated, not authored in Python, but their bodies must be
audited.** Every name a leaf emits must resolve against the built `.pyd`. Use
`cpp-pybind11-bindings` → `scripts/audit_script_bindings.py`. A leaf that emits
an unbound name compiles clean, renders clean, and raises `AttributeError` the
first time an NPC walks into that branch — an NPC that appears to work and is
silently inert on one state. That is worse than a missing node.

---

## File format: `.wlbtree`

Same YAML substrate as `.wlblocks`, different node vocabulary and one extra
structural concept (child slots).

```yaml
schema: 1
kind: behavior            # distinguishes it if a loader ever sees both
name: EnemyTree

# Blackboard keys are DECLARED AND TYPED. Undeclared key access is a compile
# error naming the node -- the single highest-value check in this system,
# because a typo'd key otherwise fails silently at runtime.
blackboard:
  - { key: target,    type: entity, default: null }
  - { key: lastSeen,  type: vec3,   default: null }
  - { key: attackAt,  type: float,  default: 0.0 }
  - { key: homePos,   type: vec3,   default: null }

nodes:
  - { id: root,  type: Selector,    x: 0, y: 0,   abort: lower }
  - { id: dead, type: IsDead,       parent: root }
  - { id: flee, type: Sequence,     parent: root, abort: self }
  - { id: f1,   type: Invert,       parent: flee }
  - { id: f2,   type: IsLowHealth,  parent: f1 }
  - { id: f3,   type: MoveTo,       parent: flee, abort: lower }
  - { id: f4,   type: SetBlackboard,parent: flee, pin: lastSeen }
  - { id: chase,type: Sequence,     parent: root }
  - { id: c1,   type: Cooldown,     parent: chase, seconds: 0.5, abort: none }
  - { id: c2,   type: SenseNearest, parent: c1, key: target }
  - { id: c3,   type: SetTimer,     parent: c1, pin: attackAt, seconds: 1.0 }

# Literal values for unconnected input pins, keyed by node id + pin id.
pins:
  - { node: c3, pin: Seconds, value: 1.0 }
```

Rules carried over unchanged from Visual Blocks, because getting them wrong is
expensive and the damage only shows up in graphs authored before the change:

- **Nodes addressed by stable string id, pins by stable string id.** Never an
  index. A node def gaining a pin must not break saved trees.
- **Child slots are not data pins.** A composite's children are structural
  (`parent`), and their order is the file's declaration order — that order *is*
  the priority. Do not model children as pin links.
- **Abort is an explicit per-node field, not inferred.** `none` (default),
  `self` (re-enter this node), `lower` (abort lower-priority siblings when this
  subtree's condition flips).
- **Unknown node types, unknown pins, and unknown blackboard keys error on
  load.** Never silently drop. A tree that quietly loses a task is a designer
  debugging an NPC that behaves like last week's build.

---

## Node library v1

Small and complete beats large and partial. Every entry below is one registry
registration; the registry is the extension point.

**Composites** — evaluate children in declaration order, first non-failure wins.

| Node | Semantics |
|---|---|
| `Sequence` | Succeed on first child success; fail on first child failure |
| `Selector` | Fail on first child failure; succeed on first child success |
| `Parallel` | All children, success policy = `one` \| `all`, fail policy = `one` \| `all` |

**Decorators** — exactly one child, return a modified result.

| Node | Semantics |
|---|---|
| `Invert` | Swap success/failure |
| `ForceSuccess` / `ForceFailure` | Discard child result |
| `Repeat` | Re-run child `n` times; optional per-iteration break condition |
| `Cooldown` | Child may not re-enter for `seconds` after it completes |
| `Timeout` | Fail the subtree if the child runs longer than `seconds` |
| `SetTimer` | Set a blackboard float to `now + seconds` |
| `TickEvery` | Skip this subtree unless `seconds` have elapsed (the LOD node) |

**Conditions** — leaves, no side effects, run to completion in one tick.

| Node | Reads |
|---|---|
| `IsDead` | `HealthComponent.IsDead` |
| `IsLowHealth` | fraction below threshold pin |
| `DistanceTo` | float distance to `blackboard.target` |
| `HasLineOfSight` | `Scene.Raycast` eye→target, blocked by `RaycastIgnoreComponent` |
| `BlackboardIsSet` | key validity |
| `Compare` | two float pins, `<` `<=` `>` `>=` `==` `!=` |
| `Gate` | float pin mod gate — a cheap deterministic sampling node |

**Tasks** — leaves, may be **latent** (suspend across ticks).

| Node | Latent | Reads / writes |
|---|---|---|
| `Wait` | yes | duration pin |
| `MoveTo` | yes | moves via transform + ground raycast, same as `Enemy.py` |
| `FaceTarget` | no | shortest-arc yaw toward target |
| `ApplyDamage` | no | damage pin, applied to `blackboard.target` |
| `SenseNearest` | no | nearest live `HealthComponent` entity within radius → writes key |
| `SetBlackboard` | no | literal or input pin → key |
| `MoveToTransform` | yes | walk to an explicit position, not a target (flee, return) |
| `PlayAnimation` | no | `AudioSourceComponent`/animation name |
| `CallPython` | maybe | hand-written script method — the hot-loop escape hatch |

**Latency is a property of the node def, not a runtime guess.** `Wait` and the
movement tasks declare `Latent = true`, so the evaluator knows to hold them
across ticks. Declaring a task latent is a deliberate act; an accidental latent
task is an NPC that stops moving forever.

---

## Generated output shape

A compiled tree produces one Python module plus the shared evaluator. The
generated file is a **first-class artifact** — pretty-printed, header'd, and
openable in the existing `ScriptEditorPanel`.

```python
# GENERATED BY NPC BEHAVIORAL VISUAL BLOCKS — DO NOT EDIT
# Source: assets/blocks/EnemyTree.wlbtree
# Tree schema 1, engine 0.1.0
# Edits here are lost on the next compile. Edit the graph, or move the body of
# a task into a hand-written script and point a CallPython node at it.

from wl_behavior import Tree, Node, Task, run_tick

class EnemyTree:
    def __init__(self, entity):
        self.entity = entity
        self._tree = Tree(entity, root="root", blackboard={
            "target":   None,
            "lastSeen": None,
            "attackAt": 0.0,
            "homePos":  None,
        })

    def OnUpdateEntity(self, dt):
        self._tree.tick(dt.GetSeconds())

    # --- condition leaves -------------------------------------------------
    def _cond_dead(self, bb):
        h = self.entity.TryGetHealthComponent()
        return False if h is None else bool(h.IsDead)

    # --- task leaves ------------------------------------------------------
    def _task_c2_sense_nearest(self, bb, __ctx):
        """SenseNearest -> target, radius 30.0"""
        bb["target"] = self.entity.__wl_sense_nearest(30.0)   # NB.4 body
        return __ctx.success()

    # --- topology (data, not code) ---------------------------------------
    _TOPOLOGY = {
        "root":  {"t": "Selector", "kids": ["dead", "flee", "chase"],
                  "abort": "lower"},
        "dead": {"t": "IsDead", "leaf": _cond_dead},
        "flee": {"t": "Sequence", "kids": ["f1", "f3", "f4"],
                 "abort": "self"},
        "f1":   {"t": "Invert", "kids": ["f2"]},
        "f2":   {"t": "IsLowHealth", "leaf": _cond_f2},
        "f3":   {"t": "MoveTo", "kids": [], "leaf": _task_f3_move_to,
                 "latent": True, "abort": "lower"},
        "chase": {"t": "Sequence", "kids": ["c1", "c3"]},
        "c1":   {"t": "Cooldown", "kids": ["c2"], "seconds": 0.5,
                 "abort": "none"},
        "c2":   {"t": "SenseNearest", "kids": [], "leaf": _task_c2_sense_nearest,
                 "key": "target"},
        "c3":   {"t": "SetTimer", "kids": [], "seconds": 1.0,
                 "pin": "attackAt", "leaf": _task_c3_set_timer},
    }
```

Note what this is **not**: it is not a per-engine-API reflection table. It
references generated Python functions in the same module. There is no second
binding surface, which is the entire argument of the transpile decision.

---

## Editor: a tree list, not a node canvas

This is the decision most likely to be gotten wrong by assumption, so it is
stated early.

A behavior tree is a **hierarchy**, and freeform node canvases are the wrong
UI for hierarchies — designers lose track of nesting, and the canvas becomes a
spaghetti of wires that reads worse than the indented list it replaced. UE's
behavior tree editor is a nested list for this reason, not laziness.

`NPCBehaviorPanel.{h,cpp}` in `DemonCore-Editor/src/Panels/`, beside the existing
panels:

- **Palette** on the left, grouped Composite / Decorator / Condition / Task,
  searchable, showing which need a child.
- **Tree list** centre, indented, drag-to-reparent, reorder = priority. Composite
  rows show their abort mode inline.
- **Node editor** below, for the selected node's pins and literals. Reuses the
  same pin widget as the Visual Blocks details panel.
- **Blackboard** panel: declared keys and types, live values during play.
- **Error list** pinned to the node, same renderer as Visual Blocks.
- **Trace toggle**: logs each node entered/exited with the blackboard snapshot,
  with the tree open beside it.

That is **no new third-party dependency**, and no work that depends on
`imgui-node-editor` at all. If VB.0 is delayed, this plan is unaffected.

Content browser: `.wlbtree` double-click opens this panel — one extension check
next to the existing `.py` and `.wlblocks` ones in `ContentBrowserPanel.cpp`.

---

## Phasing

Every phase ships and is headlessly verifiable. Compiler before editor, again.

```
NB.0 format + taxonomy lock   ─┐
NB.1 wl_behavior.py evaluator ┤ headless, no editor
NB.2 blackboard                │
NB.3 .wlbtree compiler         │
NB.4 node library v1           ┘
NB.5 end-to-end on Enemy.py ──── the proof
NB.6 editor panel
NB.7 perception senses
NB.8 navmesh task (blocked on 14.5)
```

### NB.0 — Lock the format and the taxonomy

Write `wl_behavior.py`'s public API surface as a header comment, plus the
`.wlbtree` schema and the node taxonomy above, into
`.hermes/plans/` or `docs/`. No code. Purpose: if the tree semantics (abort
rules, latent rules, priority = declaration order) are wrong, every later phase
is wrong too, and this is the cheapest moment to catch it.

Also confirm against `Enemy.py` that the v1 node set can express all of it —
retarget on timer, move toward, smooth yaw, ground probe, contact damage on
interval, idle when no target, dead state. Any gap is a node-library gap and
must be named now.

### NB.1 — The evaluator, written once

`DemonCore-Editor/assets/scripts/wl_behavior.py`, hand-written and versioned
(not generated — every tree shares it). Contents: `Node`, `Tree`, `run_tick`,
`Task`, result enum, per-node runtime state dict, trace hook.

Semantics that must be exactly right:

- **Traversal.** Composite children in declaration order. Composite returns on
  the first child that decides the outcome; a `Sequence` returning success
  stops there.
- **Latent resume.** A latent task is entered, and if it returns `RUNNING` its
  node id is recorded in `tree.running`. Next tick, the subtree search finds
  that node and resumes *at* it rather than re-entering from the top of the
  subtree. A latent task that returns success/failure clears the record.
- **Abort lower.** When a node with `abort="lower"` or `abort="self"` is the
  currently running node, and its subtree's re-evaluation changes outcome, drop
  out of that subtree and re-search from the root.
- **Blackboard validity.** A key is valid once written and never unset except
  by an explicit `ClearBlackboard` task. `BlackboardIsSet` reads validity, not
  truthiness — `attackAt = 0.0` is set.
- **Re-entrancy.** Ticks are not re-entered. A latent task that would block
  forever must be caught by `Timeout`.

Test, headless, with hand-built topology dicts — no compiler involved:

- `Sequence` stops at the first failure and does not run later children.
- `Selector` stops at the first success.
- A latent task returns `RUNNING`, and on the next tick the *same* node resumes
  without its parent's earlier children re-running.
- A latent task that completes clears the running record.
- `Cooldown` blocks re-entry for exactly `seconds`, then allows it.
- `Invert` on a latent child propagates `RUNNING` unchanged (the classic bug:
  inverting `RUNNING` yields a bogus success).
- `Parallel` with `one`/`all` policies under mixed success/failure/running.
- `TickEvery` skips on the off-tick and resumes correctly on the on-tick.
- Blackboard validity survives writing a falsy value.
- Trace hook fires once per node entry and emits the node id.

### NB.2 — Blackboard codegen

Blackboard becomes a declared, typed dict on the generated class. The compiler
emits the schema; the validator rejects reading or writing an undeclared key
**naming the node**. Defaults are parsed per declared type — a malformed default
is a reported compile error, not a crash and not a silent zero.

Test: a tree that reads `targt` (typo) fails to compile and the message names
`c2.SenseNearest.key`. A tree with a bad `vec3` default fails and names the
blackboard key.

### NB.3 — The `.wlbtree` compiler

`VisualBlocks/BehaviorTree.{h,cpp}` — load/save YAML, build the topology map,
validate, emit. Shares the serializer, validator walk and Python emitter with
Visual Blocks (per the dependency table at the top).

Validator rules, each naming the node:

- unknown node type, unknown pin, unknown blackboard key — **load errors, not
  drops**
- a composite/decorator with zero children, or a condition/task with children
- a child slot pointing at a parent that is a leaf
- duplicate node ids; a `parent` referencing a nonexistent node
- a decorator with more than one child
- a leaf marked `Latent` that has no latent body in the emitter registry

Test, comparing emitted text against expected strings:

- a linear `Sequence` emits the topology with children in declaration order
- `abort` survives round-trip through the file
- re-compiling the same tree twice emits **byte-identical** output
  (determinism — a non-deterministic emitter makes every diff and every test a lie)
- an unknown node type on load errors and does **not** partially load

#### NB.3 — DONE

`VisualBlocks/BehaviorTree.{h,cpp}` is a **facade**, not a second
implementation: the serializer, registry, validator walk and emitter are all
shared with Visual Blocks. Two registries would mean two incompatible node
taxonomies forever, and a graph authored in one would be un-loadable in the
other. What lives in the façade is only the runtime-type contract and the
three-stage entry point (load → validate → emit).

`SaveFile` refuses to write a graph that would not load back. Writing a broken
asset to disk is how a tree loses a node in a way the designer only discovers
the next time they open it.

**Verification: `test_wlbtree_compiler.py`, 56 checks, all passing.** The
shipped `EnemyGuard.wlbtree` compiles with zero errors *and* zero warnings,
generates valid Python, and survives a save/load round-trip losslessly.
Full regression: **1300 checks across 30 suites, zero failures**, via
`scratch/run_all_suites.py` (see the counting note below).

#### The runtime-type contract

`BehaviorTree::RuntimeTypes()` is the list of node types `wl_behavior.py` can
dispatch. The compiler checks every node's type against it, and the test
cross-checks it against the dispatch chain in the runtime module itself — so
adding a primitive to one side and not the other fails as a test rather than as
an NPC that silently never acts.

This caught a real gap immediately: **`Wait` had no handler in
`wl_behavior.py` at all.** It compiled fine, passed `compile()`, and would have
failed on every single tick. It is now a runtime primitive with a per-node
deadline — the same bug shape as `Timeout`, because re-deriving the deadline
each visit restarts the timer and a `Wait` reached on consecutive ticks never
finishes.

#### Why the test executes the generated code

The first version of this suite was pure string comparison and it passed
against an emitter that raised `AttributeError` on construction. Five API
mismatches were invisible to every string check and to `compile()`:

| Emitted | Reality |
|---|---|
| `bb.declare(k, v)` | `Blackboard(initial, types)` — there is no `declare` |
| `Tree(self, topo)` | `Tree(entity, topo, bb, types)` — `self` is the *behavior class*, not the agent |
| passing a `Blackboard` | `Tree` builds its own from plain dicts; it has no `items()` |
| `ctx.blackboard` | `Context.__slots__` is `bb` |
| `self.health()` in a leaf | the agent is `ctx.entity` |

And one more: **`Wasteland.Vec3` has no arithmetic operators** — no `__sub__`,
no `__truediv__`, no `__mul__`, only `x/y/z`, `Length()`, `LengthSquared()` and
`Normalize()`. The `_dist`/`_move` shims now do the maths component-wise, and
`_move` clamps each step to the remaining distance so a long frame cannot
overshoot and oscillate around the target forever.

`compile()` proves syntax. Only ticking the tree proves the calls exist. The
suite now execs each generated class against the real runtime and ticks it.

### NB.4 — Node library v1 + the binding audit

Implement every node in the taxonomy above as one registry entry each. For each
leaf: emit the Python, then run
`cpp-pybind11-bindings/scripts/audit_script_bindings.py` and confirm every name
it emits resolves against the built `.pyd`.

Pay attention to the silent-write traps documented in that skill: a leaf that
writes a component through a by-value getter, or through an `Add*Component`
copy, produces an NPC that runs clean, changes nothing, and never raises.
`SetBlackboard` on a key that a leaf also writes by copy is the same trap.

Test per leaf: a stub scene, one tick, assert the observable result. Not "did
not throw". `SenseNearest` must be asserted on *which entity it picked* and
*that it skipped dead ones and itself*.

### NB.4 — Node library v1 + the binding audit — DONE

`VisualBlocks/NodeLibraryV1.cpp` registers the full v1 taxonomy as one registry
entry each: 3 composites, 7 decorators, 7 conditions, 9 tasks. Every leaf is
audited for unbound names, and every leaf is asserted on what it CHANGED.

Two things the plan asked for turned out to be the whole point:

- **`DistanceTo` is an alias for `ConditionDistanceTo`.** Node ids are a file
  format — saved trees name them — so renaming breaks every graph on disk.
  `NodeDef::AliasOf` plus alias resolution in `Find()` (with a depth cap, so a
  registration cycle reports "unknown node type" rather than hanging at startup)
  keeps both spellings loadable and behaves identically everywhere.
- **An emitter/shim arity cross-check.** Deriving both sides from the generated
  python and comparing catches a call that disagrees with its signature — which
  otherwise raises TypeError only when an NPC enters that branch.

#### The binding audit found eight invented APIs

The first shim was written from what the engine "should" look like. Every one of
these compiles, walks cleanly, and raises only when an NPC enters that branch —
an NPC that appears to work and is silently inert on exactly one state:

| Written | Reality |
|---|---|
| `entity.MoveCapsule(delta)` | does not exist — movement is `SetTransform(translation, rotation, scale)` |
| `entity.GetComponent(Wasteland.HealthComponent)` | `GetHealthComponent()` / `HasHealthComponent()`, per component type |
| `entity.HasComponent(...)` | same |
| `entity.AddComponent(...)` | `AddHealthComponent(...)` |
| `health.IsDead()` | `IsDead` is a **property** — calling it raises |
| `entity.SetTransform(TransformComponent)` | takes three Vec3s, not a component |
| `scene.GetEntities()` | returns `[]` for runtime-created entities — perception would find **nothing** |
| `Wasteland.AnimationComponent` | does not exist; only an `Animation` asset submodule |

The `GetEntities()` one is the dangerous kind: not an exception, but an empty
result. Every NPC would be permanently blind and the traces would look clean.

`Scene.GetEntities()` is empty while `GetAllEntitiesWithTransform()` returns
everything. Perception uses the latter.

#### Perception needs the scene passed in, not looked up

An `Entity` carries no back-reference to its `Scene`, `Application.Get()` is
`None` headless, and `Scene.GetActiveScene()` is `None` until a scene is
activated. So the generated class takes `scene=None` and the shim reads it
through `getattr(ctx.tree, "scene", None)`, degrading to a no-op when unset —
rather than reaching for a global that is None in the editor, in tests, and
during early startup.

#### Verification

`test_node_library.py`. The suite execs each generated class against the real
`wl_behavior` runtime and ticks it against a real `Scene` — string comparison
is not evidence that an engine call exists.

Three fixture lessons that looked like engine bugs and were not:
- `IsDead` is set by `ApplyDamage`, not derived from assigning `Current = 0`. A
  fixture that assigns `Current` builds a numerically-dead, behaviourally-alive
  entity.
- A blackboard key declared *with* a default is present from tick one, so it
  reads as set. Only a key declared *without* one is unwritten.
- A due-north target at yaw 0 has nothing to turn; the facing test needs a
  real bearing.

#### Verified

Release/x64, engine + `Wasteland.pyd` built from current source; suites run
against a fresh scratch `OutDir` because the in-tree module is held open by the
Hermes runtime and would otherwise test the *previous* build.

```
test_node_library.py       635 passed, 0 failed
test_wlbtree_compiler.py    56 passed, 0 failed
test_visualblocks.py       101 passed, 0 failed
aggregate (16 suites)     1456 checks, 0 failed, 0 failed suites
DemonCore-Editor.vcxproj   Release/x64 builds clean
```

The aggregate is a glob over `test_*.py`, so "16 suites" is the real file
count; the "30 suites" figure recorded earlier for NB.3 was a miscount from
the shell aggregator that this runner replaced.

Two fixes were needed to get there, both real:

- **`SenseNearest` was blind.** It scanned `Scene.GetEntities()`, which is
  empty for runtime-created entities — every NPC would have sensed nobody. It
  uses `GetAllEntitiesWithTransform()`.
- **The self-skip compared identity.** `if other == agent: continue` never
  matched, because pybind returns a *fresh wrapper* per call: `==`, `is` and
  `!=` are all False for the same entity. It compared position instead.


### NB.5 — End-to-end, headless: `Enemy.py` as a tree

The proof, and the same script the Visual Blocks plan uses so the comparison is
honest.

Translate all 200 lines of `Enemy.py`:

```
root (Selector, abort=lower)
├─ IsDead                          -> Succeed
├─ Sequence "no target"  (abort=self)
│  ├─ Cooldown 0.5s -> SenseNearest(radius=loseRange) -> target
│  └─ Wait until target invalid
└─ Sequence "engage" (abort=self)
   ├─ Sequence
   │  ├─ Invert(IsLowHealth)      # healthy enough to fight
   │  └─ Sequence
   │     ├─ Invert(DistanceTo(target) <= attackRange)
   │     ├─ MoveTo(target)        [latent]
   │     ├─ FaceTarget(target)
   │     └─ SetBlackboard(lastSeen)
   └─ Sequence
      ├─ DistanceTo(target) <= attackRange
      ├─ Cooldown 1.0s -> ApplyDamage(12.0)
      └─ SetTimer(attackAt, 1.0)
```

Assert on **read-back values after N ticks**, in increasing difficulty:

1. Straight-line `MoveTo` toward a stationary target actually moves the entity
   the expected distance. If this fails, nothing else matters.
2. Yaw converges on the target bearing and does not wrap the long way.
3. `IsDead` on the agent's own health → the tree succeeds instantly and stops
   moving it.
4. Dead and out-of-range targets are never selected by `SenseNearest`.
5. Contact damage lands **exactly once per `attackInterval`**, not once per
   frame — the single most valuable assertion in this phase, and the exact bug
   `Enemy.py` has to defend against manually with `DamageCooldown`.
6. Removing the target entity mid-run makes the tree return to idle, not crash
   and not keep charging.
7. Full-fidelity compare: run the original `Enemy.py` and the generated tree
   from identical spawns for 600 ticks and assert the positions agree within a
   tolerance. This is the strongest evidence the tree semantics match, and it is
   the test that would catch a subtly wrong abort rule.

**DONE — verified 62/62 in `test_enemy_tree.py`, aggregate 1518/17, 0 failures.**

Shipped assets: `DemonCore-Editor/assets/trees/EnemyChase.wlbtree` (the proof)
and `PatrolGuard.wlbtree` (the negative control — same taxonomy, deliberately
different behaviour: holds post, faces an intruder, never chases or strikes).
Both validate with zero errors and zero warnings.

All seven assertions pass, plus four added during the phase. Two shape changes
were required against the sketch above, both for reasons worth keeping:

**Acquisition must FAIL, not succeed.** The sketch wraps sensing in a branch
that *succeeds* once a target is held. But a `Selector` stops at its first
success, so that branch starves every lower-priority sibling on the tick it
acquires — and the first tick an NPC ever has becomes the one tick it cannot
attack on. The symptom is not a logic failure at all: it surfaces as a 1.2s
attack cadence instead of 1.0s, which reads as float drift in the deadline
arithmetic. Fixed by wrapping the sensing task in `ForceFailure`, which runs its
child for the effect and then reports failure regardless, so the root always
falls through to `engage` on the same tick.

**No latent `Wait` gating acquisition.** The sketch throttles sensing behind a
0.5s `Wait`. A `Wait` returns `RUNNING` for its whole duration, so `engage` —
and the attack `Cooldown` inside it — became reachable one tick in six, and a
1.0s cooldown released every 12 ticks. Perception is an O(n) scan; running it
every tick costs nothing and is what lets a dead or departed target retire via
`SenseNearest`'s own `bb.clear(key)`. `Enemy.py` caches for efficiency; the tree
corrects first.

**Two runtime bugs found in the shared evaluator (`wl_behavior.py`):**
- `SenseNearest` cleared its key with `bb.set(key, None)`, which marks the key
  *written*. Its own docstring said "CLEARED". `BlackboardIsSet` therefore
  answered True forever and the NPC idled indefinitely, silently. Now
  `bb.clear(key)`.
- Every deadline test (`Cooldown`, `Timeout`, `Wait`) compared `self.now`
  directly. `now` is a running sum of per-tick deltas and drifts (~1e-15/tick),
  so a raw `<` stayed true one tick *past* the deadline whenever the
  accumulation landed short — a 1.0s cooldown firing every 1.2s. Added
  `deadline_passed()` with a 1e-6 tolerance at all three sites.

**New validator rule: `CheckNoSharedChildren`.** `strike` was authored on its
own `Sequence` *and* inside its `Cooldown`, so it evaluated twice per tick and
dealt 24.0 instead of 12.0 — invisible in the editor, obvious only in the read-
back. A node reachable from two parents is now an error naming both parents.

**Compiler fix:** an entity-typed blackboard key with no default was seeded as
`None`, so `BlackboardIsSet("target")` answered True on tick 1 with nothing in
it. Entity keys without defaults are now omitted from the seeded blackboard.

**Differential fidelity:** the tree and `Enemy.py` do not land the same *hit
count* — 58 vs 52 over 600 ticks — and should not. Both spend ~29 ticks closing
the gap before anything can land, after which a 1.0s cadence over the remaining
571 ticks yields exactly 58. The tree hits 58; the reference reaches fewer
because its approach settles later than `MoveTo`'s clamped step, and every tick
spent walking is a tick with no damage. The suite now asserts the invariant that
is actually about the tree (hit count == cadence × engaged ticks, computed per
seed) rather than encoding the reference's caching quirk as a specification.

**Not done here:** `Enemy.py` itself is deliberately untouched. It remains the
working reference implementation, which is what made the differential test
possible at all; migrating it is a separate step and should only follow this.


Then a second, *different* tree as a negative control — a patrolling guard using
`Wait`, `MoveToTransform`, and `SenseNearest` with a different radius. If the
node library cannot express both, the taxonomy is wrong and that is worth
learning at NB.5, not at NB.6.

### NB.6 — Editor panel

`DemonCore-Editor/src/Panels/NPCBehaviorPanel.{h,cpp}`, wired into
`EditorLayer` beside the existing panels. Palette, tree list, node editor,
blackboard values, error list, trace toggle. Content-browser extension dispatch.

Verify with **screenshots of the actually-rendered panel at 4K**: build a tree by
dragging, hit compile, read the generated Python in the script editor, run it,
watch the trace and the blackboard values change, and see the NPC move. A clean
build log proves nothing here.


### NB.6 — Editor panel — DONE (code complete; two host-level bugs found and fixed)

`DemonCore-Editor/src/Panels/NPCBehaviorPanel.{h,cpp}`, wired into `EditorLayer`
with a **View > NPC Behavior** toggle, beside the existing panels. Verified by
screenshot of the running editor, not by a clean build log.

Shipped: a **tree list**, not a node canvas. A tree's structure *is* its priority
order, and a freeform canvas cannot show nesting or ordering at a glance — so
this needs no vendored node-editor dependency, which is the whole reason a
hierarchical graph ships without the flow-graph canvas phase.

Contents: node palette grouped by role (composite / decorator / condition /
task) from `NodeRegistry::BehaviorNodes()`, per-node pin literal editors driven
by each type's declared pins, `latent` and `abort` flags, the validator's
findings list with error/warning colours, the blackboard declaration list, the
generated Python, and Validate / Compile / Compile & Write / Save. Structural
edits refuse an invalid tree **at the point of the click with a reason** (e.g.
reparenting into your own subtree), rather than leaving the author to diagnose a
red error list from scratch. Save refuses a tree that would not load back.

The panel never runs anything. Compiling produces Python, saving writes the
`.wlbtree`, and attaching it to an entity is a scene concern — an editor that
starts ticking a tree as a side effect of opening a panel makes every authoring
action a gameplay event.

**Bug 1 — the node registry was never initialised in the editor.** The panel
reported **25 errors, 8 warnings** — "unknown node type" for `Sequence`,
`Invert`, `Wait`, `ConditionDistanceTo` and more — on a tree that validates
0/0 in the test suite. Only `Bindings.cpp` ever called `RegisterAllNodes()`,
so every non-scripting host reached an **empty registry**. The tests passed
*because they run through the bindings that do register it*; nothing was
exercising the editor path.

Fixed at the source rather than at each call site: `NodeRegistry::Get()` now
registers on first access. `Register()` already replaces by `TypeId`, so an
explicit early `RegisterAllNodes()` from a host that wants to force
initialisation stays harmless, and no future host can be forgotten. This is the
general lesson — **a registry validated only through the one entry point that
initialises it passes no matter how broken the others are.**

**Bug 2 — `assets/trees/` never reached the build output.** Neither did
`wl_behavior.py`: the copied script count was 13 of 15 and there was no `trees`
directory at all, so the editor could not load a single authored `.wlbtree`.
The post-build step used

```
xcopy /Y /E /I "$(SolutionDir)/DemonCore-Editor/assets" "$(TargetDir)assets"
```

and `$(SolutionDir)` is the **repo** root, so it resolved to
`DemonCore-Editor/DemonCore-Editor/assets` — a folder that does not exist. It
copied nothing, silently, and the missing folder was masked because
`assets/scripts` etc. already existed from an earlier layout. Fixed to
`$(ProjectDir)assets` and switched to `robocopy /MIR`, which cannot be skipped
by a prompt a non-interactive build is unable to display. Verified by
**deleting the destination and rebuilding**: `trees/` arrives with all three
`.wlbtree` files and `scripts/` is complete.

**Two bugs the screenshot caught that no build could.** `BeginItemTooltip`
needs ImGui 1.90 and the vendored copy is 1.89.3 WIP (used
`IsItemHovered` + `BeginTooltip`), and this ImGui build has no float
`TextColored` overload (converted to `ImVec4`). A dangling
`("abort=" + node->Abort).c_str()` temporary was also passing freed memory to
`TreeNodeEx`.

**Repo-level build breakage found and fixed while verifying NB.6.** Getting a
clean build took real digging, and the causes are worth recording because every
one of them *looks* like a code error and is not.

1. **`vendor/imgui/imgui.h` had two version blocks merged** — a 1.89.3 WIP block
   (matching the file's own banner) and a stale 1.88 block, both defining
   `IMGUI_VERSION`. Harmless on its own (C4005), but the 1.88 block also defined
   `IMGUI_CHECKVERSION()`, which the 1.89.3 block redefined at a second site:
   **two definitions of the same function-like macro is a hard error.** The
   symptom was the least informative one possible — `CL.exe exited with code 1`
   with *no diagnostic at all*, after every translation unit had compiled. This
   is committed in git (`git show HEAD:...imgui.h` shows both blocks), so it is
   long-standing rather than recent. Reconciled to a single 1.89.3 block; the
   canonical `IMGUI_CHECKVERSION` now exists once.

2. **`/FS` belongs on `ClCompile` only.** Adding it to `<Link>` as well broke the
   static-library targets. Removed — the PDB write race is entirely a `cl.exe`
   concern. Documented in `Directory.Build.props` so it is not re-added.

3. **`MinimalRebuild=false` is not the fix for a PDB race.** It swaps MSBuild
   into `TrackedVCToolTask.DeleteFiles`, which then fails with `MSB6003` on the
   tlog cleanup. Tried and reverted; noted in the props file so nobody repeats it.

4. **A wedged `mspdbsrv` presents as `C1090 ... error code '23' (0x000006BA)`.**
   That is `RPC_S_SERVER_UNAVAILABLE` — a *shared singleton* that survives a
   client crash in a bad state, so every later build fails instantly with
   nothing else running. `Stop-Process -Name mspdbsrv` fixes it outright.

5. **Deleting an intermediate directory has project-specific blast radius.**
   `IntDir` is `$(ProjectDir)..\bin-int\<Config>-windows-x86_64\<Proj>` for the
   engine but `$(ProjectDir)bin-int\<Config>-windows-x86_64\<Proj>` for the
   vendored libraries, and `$(ProjectDir)obj\<Config>` for the Python module —
   three different conventions. Recreating only the first produced
   `LNK1104` on a vendor object that had nowhere to go.

`tools/msb.sh` now wraps every build: it waits out any straggler `cl.exe` /
`MSBuild.exe`, resets `mspdbsrv`, then builds serially with `-m:1`. The
remaining trap is procedural, and it is the one that cost the most time:
**piping MSBuild into `grep`/`tail` returns before MSBuild finishes**, so the
next command races the build it was just waiting on. Capture to a file with
`>`, or use `background=true` with `notify_on_complete` — never a shell pipe.

**Known, pre-existing, unrelated:** the editor self-closes ~5–20 s after launch
in the automation environment (`OnWindowClose received!`). All screenshots were
taken inside that window. Also, concurrent MSBuild runs against this repo fail
with `C1041` / `MSB6003` / `C1083` because an orphaned `cl.exe` keeps the
intermediate handles; `/FS` in a new `Directory.Build.props` serialises PDB
writes and `tools/msb.sh` serialises builds, but this is a build-hygiene fix
rather than a product change.


### NB.7 — Perception

`SenseNearest` exists from NB.4 as a plain O(n) scan over `Scene.GetEntities()`
on a `Cooldown` — the same approach `Enemy.py:64` already takes, and correct for
a static roster at current entity counts. NB.7 is the *proper* version:

- A shared, throttled perception cache updated on an interval, not per-NPC per-
  frame. O(n²) perception across a roster is the obvious next scaling wall.
- Sight: cone + range + `Raycast` line of sight against `RaycastIgnoreComponent`.
- Hearing: radius, scaled by an event's loudness pin.

Do this after the editor, not before: it is a performance and fidelity
improvement to a node that already works. Note the O(n²) explicitly in the plan
so it is not a surprise.

### NB.8 — Navmesh task (blocked)

`MoveTo` walks straight at the target and walks into walls — `Enemy.py:11-13`
says so outright. Phase 14.5 (navmesh + A* + string pulling) does not exist
yet. When it lands:

- `MoveTo` gains a `UsePathfinding` bool pin; off = today's direct behavior.
- A `FollowPath` task, with local avoidance so agents do not stack.
- Path re-plan on a blocked path instead of stalling.

**Blocked, and it should stay blocked.** This is now Phase 14.5.2 in
`item-14-long-tail-systems.md`, which is **downstream of this plan by
construction**. Two independent reasons it cannot go first: a navmesh does not
exist, and — independently — the 14.1 box-hull contact bug means a bake over
the level's box colliders would bake a surface that is known to be wrong.
Writing a pathfinding abstraction against either produces an interface nobody has
tested. `MoveTo` stays direct; the escape hatch is `CallPython` into a
hand-written mover once 14.5.2 ships.

---

## Files likely to change

| Path | Change |
|---|---|
| `DemonCore-Editor/assets/scripts/wl_behavior.py` | **New** — the shared evaluator, hand-written |
| `Wasteland/src/Wasteland/VisualBlocks/BehaviorTree.{h,cpp}` | **New** — tree model + serializer |
| `Wasteland/src/Wasteland/VisualBlocks/BehaviorCompiler.{h,cpp}` | **New** — tree → Python |
| `Wasteland/src/Wasteland/VisualBlocks/NodeRegistry.cpp` | Register the tree node set |
| `Wasteland/src/Wasteland/VisualBlocks/Validator.cpp` | Tree-specific rules |
| `DemonCore-Editor/src/Panels/NPCBehaviorPanel.{h,cpp}` | **New** — editor panel |
| `DemonCore-Editor/src/EditorLayer.{h,cpp}` | Wire the panel |
| `DemonCore-Editor/src/Panels/ContentBrowserPanel.cpp` | `.wlbtree` dispatch |
| `DemonCore-Editor/assets/blocks/EnemyTree.wlbtree` | **New** — the demo tree |
| `Wasteland/premake5.lua` | New sources, if Visual Blocks added its own |
| `tests/test_wl_behavior.py` | **New** — evaluator suite, headless |
| `tests/test_behavior_compile.py` | **New** — compiler suite, headless |

`Bindings.cpp` gets **no new entries**. Everything a leaf needs is already bound.
That is the test of whether the transpile decision held.

---

## Verification standard

Inherited from `item-14-long-tail-systems.md`, unchanged:

- Headless Python suites, no GL context, no window.
- Explicit `PYTHONPATH` to the Release output.
- **Never** call `Scene::OnRuntimeStop()` or drive `ScriptEngine::OnUpdateEntity`
  standalone — both are known to segfault for pre-existing reasons. Drive the
  tree through the scene's normal update, and attach the `ScriptComponent` with
  collidered entities already present before `OnRuntimeStart()`.
- Real assertions on read-back values after N ticks, never "did not throw".
- MSBuild Release **and** Debug.
- Editor: screenshots of the actually-rendered panel, not a clean build log.

Two additions specific to this plan:

- **A binding audit per leaf node** before NB.5 is called done. A leaf that
  emits an unbound name is an NPC that works on every branch the designer
  happened to test and is inert on the rest.
- **The 600-tick differential test** against `Enemy.py` (NB.5 #7). Semantic
  equivalence to the known-good script is stronger evidence than any
  hand-written assertion set.

---

## Risks

**Abort semantics are the hard part, and they are invisible when wrong.** A
wrong abort rule produces an NPC that looks fine in the editor and behaves
subtly differently across a wall corner. Mitigation: NB.0 locks the semantics
before any code; NB.1 tests each rule in isolation on hand-built topologies; NB.5
#7 diffs against the known-good script over a long run.

**The tree list is a real UX investment.** The palette/tree/detail layout is
not the hard part of a node editor — the compiler is — but a tree list that
cannot drag-to-reparent will make every subsequent phase feel worse. Mitigation:
build the reparent interaction in NB.6 from the start rather than adding it later.

**Per-tick cost.** Every tick walks a Python dict tree and allocates result
objects. At tens of NPCs this is irrelevant; at hundreds it is not. Mitigations,
in order of preference: `TickEvery` on the root, `Cooldown` on perception tasks
(already in the design), `CallPython` for hot leaves, `wl_behavior` optimized
only if a real measurement demands it. **Do not build a native C++ BT runtime
to solve a problem this codebase does not yet have** — that is the VM mistake the
whole plan is structured to avoid.

**Scope creep toward UE5 parity.** The honest gaps are EQS, tree inheritance,
services, key observers, and the BT debugger. Named at the top; if EQS becomes a
genuine need it is its own plan, and `SenseNearest` is where it lands.

**Two node systems is a real cost.** `.wlblocks` and `.wlbtree` share a
serializer, a validator and an emitter. If they end up with two incompatible
registries or two emitters, that is the failure mode to watch for in NB.3 — and
the reason NB.3 is not allowed to start until Visual Blocks has landed.

## Counting suite results — the aggregate was lying

The shell one-liner used for the whole session
(`grep -c "^  PASS "`) under-reported by **157 checks** and still printed
"0 failed suites", because CRLF-emitting suites put a `\r` where the pattern
expected a space. Failures were detected correctly the whole time, which is
what made it dangerous: the verdict looked healthy while the count was wrong.

So the numbers previously reported for this project were **low**, and one
figure I quoted (1243 across 29 suites) never existed — the honest
pre-Visual-Blocks total was 1143, and it is now **1300 across 30**.

`run_all_suites.py` normalises newlines in Python, counts `PASS`/`FAIL`
markers, and reports a suite that emitted neither as **UNCOUNTED** rather than
zero. Use it instead of a shell loop.

The general rule: **when a total moves unexpectedly, find out why before
reporting it.** A count that drops while suites were added is a broken
counter, not a regression.
