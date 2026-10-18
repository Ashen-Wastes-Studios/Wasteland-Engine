# Item 14 — Long-tail systems plan

Status: plan only, nothing implemented. Written 2026-09-29 after item 13
(animation) reached 673/673 green across 17 Release-path headless suites.

## What the audit actually found

Before planning, I checked what already exists so the plan does not rebuild
finished work:

| Area | State |
|---|---|
| Physics world | **Stepped and written back** (`b3World_Step` + transform sync). Verified 2026-09-29 |
| Physics reachability | `Scene::StepPhysics` added — physics was unreachable from Python (needed an EditorCamera) |
| Colliders | Box/Sphere/Capsule/Circle exist and are bound. **Box hulls have a contact bug — see below** |
| `RigidBody3DComponent` | Exists with `GravityScale`, damping, fixed-rotation flags, `RuntimeBody` handle. All confirmed working |
| ECS | EntTT registry, ~27 component types |
| Scene serialization | `.wastescene` working, gameplay components persisted |
| Renderer | Full Vulkan/D3D12/D3D11/GL via NVRHI, water, volumetrics, post |
| Scripting | pybind11 + file-loaded Python, ImGui editor |
| Inventory | **Nothing** |
| Audio | **Nothing** (no listener, no clip, no 3D attenuation) |
| Save/load | Scene save exists; **no game-state persistence** |
| NPC AI / behavior | **Nothing** — one hand-written `Enemy.py` (200 lines, flat state machine) |
| Blackboard / perception | **Nothing** |
| NavMesh / pathfinding | **Nothing** |
| Terrain | 1 incidental file, not a system |
| Streaming / chunking | **Nothing** |
| Dialogue | **Nothing** |

### Correction to the original audit

An earlier draft of this plan claimed "physics bodies are created and then
ignored — no `b3World_Step` anywhere". **That was wrong**, twice over: the step
exists, and my first audit test then wrongly claimed the 3D components were
unbound (the `BIND_COMP_*` macro generates both a working name and a mangled
`Wasteland::`-qualified one). The engine was fine; the audit was not.

The real gaps are narrower and more interesting:

1. **Physics could not be driven headlessly at all.** `OnUpdateSimulation`
   takes an `EditorCamera&`, so no Python caller could advance the solver.
   Fixed by adding `Scene::StepPhysics(seconds)` and routing both existing
   inline step blocks through it.
2. **Box-hull colliders push dynamic bodies upward** (+0.05/step, deterministic).
   Sphere and capsule colliders fall correctly. Reproducer and full list of
   ruled-out causes are in `test_physics_3d.py` and the skill reference.
   **This is the blocking unknown for 14.2.**
3. **Bodies added after `OnRuntimeStart()` never simulate** (`RuntimeBody`
   stays 0). Matters for dropped items and spawned enemies.

## Ordering principle

Each phase is independently shippable and independently testable headlessly.
Physics gates everything spatial; save/load must land before any system whose
state a player would hate to lose; the rest are independent and can follow in
any order.

The one exception to "physics gates everything spatial" is **14.5.1**, the NPC
authoring layer, which needs no physics prerequisite at all. That is deliberate:
it is the half of 14.5 that designers want first, and gating it behind the 14.1
box-hull fix would mean waiting on a bug fix to deliver a UI.

---

## Phase 14.1 — Physics: make it trustworthy, then make it usable

**Already working** (verified 2026-09-29, 673/673 regression green):

- Solvers step and write `TransformComponent` back.
- `GravityScale`, damping, and fixed-rotation locks all take effect.
- `Scene::StepPhysics(seconds)` added, so physics is drivable headlessly.
  Both former inline step blocks now route through it.

**Remaining, in priority order:**

1. **Fix the box-hull contact bug** (blocking). A static box floor pushes a
   dynamic body upward +0.05/step instead of letting it fall. Sphere and
   capsule colliders are fine. Until this is fixed, any level geometry built
   from box colliders — which is most of it — is not a valid physics surface.
   Reproducer and ruled-out causes: `test_physics_3d.py`.
2. **Create bodies outside `OnPhysics3DStart`.** `RuntimeBody` is only ever
   assigned during startup, so late-spawned entities never simulate. Needs a
   per-entity `EnsurePhysicsBody()` called on component add, not a one-shot
   registry walk.
3. **Fixed timestep accumulator.** `StepPhysics` clamps dt to 1/30 rather than
   accumulating the remainder, so at 30fps the sim runs at half real speed.
   Accumulate and step at a fixed rate with a bounded accumulator.
4. **Expose apply-impulse / set-velocity** so scripts can jump, knock back, and
   launch things. Reading physics out of Python is now possible; driving it is
   not.

**Why first:** every later spatial system (navmesh, streaming, item pickup
ranges) assumes believable motion, and 14.2 needs dropped items to land.

**Test:** a dynamic sphere dropped from height settles on the floor within
tolerance and sleeps; `GravityScale = 0` leaves it suspended; fixed rotation
preserves orientation; a late-spawned body falls; a 30fps caller still advances
real time correctly.

---

## Phase 14.2 — Inventory and items

The gameplay loop needs things to pick up, carry, and use.

- `ItemDefinitionComponent` — id, display name, stack size, weight, tags.
- `InventoryComponent` — slot list, per-item stacks, capacity by weight and/or
  slot count.
- `ItemStack` semantics: max stacks, partial stacks, stack-splitting on pickup
  overflow rather than refusing the pickup.
- Events on change (`OnInventoryChanged`) so the HUD and interaction prompts
  update without polling.
- `InteractableComponent` already exists and `Crate.py` demonstrates the
  dispatch path — inventory is where it becomes real.

**Dependency on 14.1:** dropped items should be physical bodies.

**Test:** pickup respects stack limits and capacity; dropping a partial stack
preserves the remainder; adding above capacity adds what fits and reports the
rest; a save/load round trip preserves stacks exactly.

---

## Phase 14.3 — Game-state persistence -- DONE

Implemented as `Scene/SaveGame.{h,cpp}`, deliberately separate from
`SceneSerializer`. A `.wastescene` is authored content and is never modified by
play; a save is mutable per-playthrough state. Reusing one format for both is
how someone eventually saves over the level design.

- `.wlsave`, keyed by slot name, written to a `saves/` directory beside the
  executable (not the asset tree -- a read-only install must still be playable).
- `SaveGame` takes a SLOT, never a path, so a caller cannot aim a save at a
  `.wastescene`. Slot names are sanitised (`../../../escape` becomes
  `.._.._escape`).
- `Scene::SaveGame` / `LoadGame` / `GameSlotExists` / `CurrentSchemaVersion` /
  `GetWorldTime` / `SetWorldTime` bound to Python.
- New components: `PersistentComponent` (save filter + streaming radius),
  `QuestFlagsComponent` (string map, so scripts can add flags without a C++
  change), `WorldTimerComponent` (named doubles).
- Schema version is the FIRST key and a mismatch is **rejected with a message**.
  Half-loading a save the build does not understand silently drops earned
  progress, and the player has no way to tell that from normal progression.
- A failed load leaves the running game untouched -- it does not clear the
  scene first, so "unsupported save" cannot become "corrupt session".

**Never written to a save:** `RigidBody3DComponent::RuntimeBody` and other
process-local Box3D handles (restoring a stale one points a body at freed
memory); `HealthComponent::CooldownTimer` (a load must not inherit an
invulnerability window); `WeaponComponent::FireCooldown` (silently eats the
player's first shot with no visible cause); `WeaponComponent::IsReloading` /
`ReloadTimer` (a reload is discarded, not resumed).

**Test:** `test_savegame.py`, 57 checks. Save -> mutate everything -> load
restores exactly; transient state stays zeroed; future/past/absent schema
versions rejected with the world untouched; corrupt file fails without raising;
a scene file is byte-identical after a save and load; a save naming a deleted
entity still loads and applies the rest.

---

## Phase 14.4 — Audio — **DONE (core)** 

Device-independent audio core: WAV decoding, a real mixer with pooled
prioritised voices, 3D attenuation and panning, scene integration, and a
scriptable API. The platform output device is deliberately the one thing NOT
done — see "What is still missing" below.

### The decision that shaped it

**Everything that decides *what* you hear has no platform dependency.**
`AudioClip` decodes to float PCM; `Mixer` allocates voices, applies distance
attenuation and equal-power panning, loops, fades, and clamps. There is no
device, no audio thread, and no backend anywhere in the mix path. The device
only ever hands the mixer a buffer to fill.

That is what makes the whole subsystem testable with **no sound card**: a test
mixes 512 frames into a list and asserts on the samples. Every claim in
`test_audio.py` is about real DSP rather than about "it did not crash".

### What was built

| File | Purpose |
|---|---|
| `Audio/AudioClip.h/.cpp` | RIFF/WAVE parser; PCM 8/16/24/32 and IEEE float; extensible header; stereo→mono downmix (averaged, not summed) |
| `Audio/Mixer.h/.cpp` | Voice pool, priority stealing, linear-interpolated resampling, distance attenuation, equal-power panning, fades, clipping |
| `Audio/AudioSystem.h/.cpp` | Scene-facing layer: listener following, source resolution, autostart |
| `Scene/Components.h` | `AudioSourceComponent`, `AudioListenerComponent` |
| `Scene/Scene.h/.cpp` | `PlaySound` / `PlaySoundAt` / `PlayEntitySound` / `StopSound` / `LoadAudioClip` / master volume; lazy `m_AudioSystem` |

Exposed as a `Wasteland.Audio` submodule: `AudioClip`, `Mixer`, `AudioSystem`,
`PlayRequest`, `Voice`, `SpatialGains`.

### Deliberate design calls

- **Inverse-distance falloff, normalised to `[min, max]`.** An earlier version
  multiplied raw gain by `(1 - t)` and applied distance falloff *twice*; at
  10 units in a 1..100 window it produced 0.091 instead of 0.100, with the
  error growing with range. Normalising the curve rather than scaling it makes
  both endpoints exact.
- **Equal priority IS stealable.** The voice cap exists to bound simultaneous
  one-shots. Refusing ties means the fourth footstep in a burst is lost, which
  is the stutter the cap is meant to prevent. A *strictly higher* priority voice
  is still protected, and a loop is never stolen.
- **No buses, snapshots, occlusion, or reverb.** Nothing needed them yet; an
  empty DSP graph is pure maintenance cost.
- **Missing clip returns −1, not an assert.** A sound that was never imported
  should not take the game down mid-firefight. It logs, and counts.
- **`GetActiveVoiceCount` counts live voices, not slots.** Retired slots keep
  their memory for reuse, so returning `size()` reported stale slots as live
  and made the cap look permanently full.

### Verified

`test_audio.py` — **112 checks**, covering: bit depths and float WAVs, the
8-bit unsigned centre (the classic off-by-128 that turns silence into buzz),
opposite-phase stereo cancelling on downmix (proving average not sum), malformed
and truncated headers, a `data` chunk claiming more than the file holds
(clamped, not overread), mixing and centring, negative pitch clamping, one-shot
retirement vs. loop wrapping, fades, the four steal/drop cases, attenuation
monotonicity and exact endpoints, stereo mirror symmetry, NaN guards for a
degenerate listener basis, clipping at full scale, and extreme pitch.

**Full regression: 1011/1011 across 27 suites, 0 failures.**

### What is still missing

- **No platform output device.** `Mixer::Mix()` produces correct samples but
  nothing sends them to speakers. WASAPI via miniaudio (or a vendored backend)
  is the next step; it is isolated behind the mixer precisely so it is a
  separate, low-risk piece of work.
- No audio-source or clip `.wastescene` serialization yet — clips are loaded
  by path at runtime, so a saved scene does not carry its sound set.
- No compression (OGG/Vorbis/mp3) — WAV only.
- No `AudioClip` streaming for long music tracks; long clips are fully resident.
- No editor UI for auditioning clips or placing sources.
- Nothing is wired into `Player.py`/`Weapon.py` yet, and no footstep/weapon/
  hit/UI assets exist, so no audible gameplay has been heard.


- Clip registry, `AudioSourceComponent` (2D/3D, volume, pitch, loop),
  listener tied to the primary camera.
- 3D attenuation and panning relative to listener transform.
- Pooled voices with priority, so a firefight cannot starve the UI.
- Script API: `PlaySound(name)`, positional emitters.
- Footsteps, weapon fire, hits, UI — the set the existing blockout needs.

**Why after 14.3:** audio has no gameplay state worth saving, so nothing is
blocked by ordering — it is here because it is the most self-contained.

**Risk:** this is where "similar to Unity" is easiest to over-build. Start with
2D playback and 3D positioning; skip buses, mixing snapshots, and occlusion
until something actually needs them.

**Test:** clip loading and playback state machine headlessly; volume/pitch
applied to the source; a voice cap drops the lowest priority rather than
stalling.

---

## Phase 14.5 — NPC behavior and navigation

Two halves. They are in one phase because they are one problem — "NPCs should do
sensible things and get there without walking into walls" — but they are ordered
deliberately and **the authoring layer lands first**. Full detail in
`.hermes/plans/2026-09-30_162256-npc-behavioral-visual-blocks.md`; this section is
the summary and the ordering argument.

### The audit

| Area | State |
|---|---|
| NPC AI | **Nothing as a system.** `Enemy.py`, 200 lines of flat imperative chase logic |
| Blackboard | Nothing |
| Behavior tree / state machine | Nothing |
| Perception | Nothing (`FindTarget` is a hand-rolled O(n) scan, `Enemy.py:64`) |
| NavMesh / pathfinding | Nothing |

`Enemy.py` *is* a state machine written flat — idle → chase → attack → idle —
with the transitions (`if horizontal > self.attackRange`, `if health.IsDead`)
interleaved into the movement math. A designer cannot retune "when does it give
up" without reading Python and cannot see the machine at all. That is the actual
problem 14.5 solves. It is not "behavior trees are more professional than
scripts" — the existing script is not too *primitive*, it is too *opaque*.

### 14.5.1 — NPC Behavioral Visual Blocks (first)

A visual authoring layer for NPC AI: composites, decorators, conditions and
tasks wired as a behavior tree, compiled to Python, run on the entity.

**The decision that shapes it:** tree *topology* is data, leaf nodes are
generated Python functions, and a ~200-line shared evaluator walks the topology
holding latent-task state and aborts. It does **not** transpile into nested
`if`/`else`, because straight-line code cannot express the one thing behavior
trees exist for — suspend at a latent task, resume that exact node next tick,
then re-evaluate from the root to check whether a higher-priority branch fired.
Every attempt at that is a hand-rolled state machine in the generated output,
which is a worse evaluator than the generic one and far worse to debug.

**No VM, no new bindings.** Everything a leaf needs is already bound in
`Bindings.cpp`; the transpile decision's proof obligation is that `Bindings.cpp`
gains **zero** new entries. `ScriptComponent` still points at a `.py` file, so
the scene format does not change and a tree is as portable as the script it
replaces.

**Builds on the Visual Blocks plan** (`.hermes/plans/2026-09-30_161447-visual-blocks.md`):
shares the `.wlblocks` YAML substrate, node registry, validator and Python
emitter. Hard dependency on VB.1–VB.4. It does **not** need VB.0 —
a behavior tree is a hierarchy, and the right UI for a hierarchy is an indented
tree list, not a freeform canvas. That is why UE's BT editor is a tree list, and
it saves a third-party dependency.

- Shared evaluator `wl_behavior.py` — traversal, latent resume, abort rules,
  blackboard validity, trace hook. Written once, versioned, not generated.
- `.wlbtree` format: YAML, **nodes and pins addressed by stable string ids**,
  declared and typed blackboard keys, explicit per-node abort mode, child
  priority = declaration order.
- Undeclared blackboard key, unknown node type, unknown pin → **compile/load
  errors naming the node**, never a silent drop. A tree that quietly loses a task
  is a designer debugging an NPC that behaves like last week's build.
- Node set v1: composites (`Sequence`/`Selector`/`Parallel`), decorators
  (`Invert`/`Repeat`/`Cooldown`/`Timeout`/`SetTimer`/`TickEvery`), conditions
  (`IsDead`/`IsLowHealth`/`DistanceTo`/`HasLineOfSight`/`BlackboardIsSet`/
  `Compare`), tasks (`Wait`/`MoveTo`/`FaceTarget`/`ApplyDamage`/`SenseNearest`/
  `SetBlackboard`/`MoveToTransform`/`PlayAnimation`/`CallPython`).
- Editor: `NPCBehaviorPanel` — palette, indented tree list with drag-to-
  reparent, node detail, live blackboard values, error list, trace toggle.
- Trace toggle ships *with* the compiler, not after it. A visual language with
  no per-node trace is unfixable by the people who have to fix it.

**Out of scope, named now:** EQS (UE's real power and the biggest gap;
`SenseNearest` is where it would land), tree inheritance / BTComposite
parenting, per-agent blackboard overrides, key observers, services, BT debugger
breakpoints, per-node LOD. Declaring these now is cheaper than discovering them
in month three. UE5 took a team years; matching its *authoring experience* for
NPC logic is achievable in days.

**Test:** the 600-tick differential run against `Enemy.py` from identical spawns,
positions agreeing within tolerance. That is the strongest evidence the tree
semantics are right, and it catches a subtly wrong abort rule that no
hand-written assertion would. Plus per-leaf read-back assertions — damage lands
once per `attackInterval`, not once per frame — and a binding audit of every
name each leaf emits.

### 14.5.1a — The evaluator (DONE) — `wl_behavior.py`

NB.1 landed ahead of the compiler, deliberately: the evaluator is pure Python,
depends on **none** of VB.1-VB.4, and the plan's own argument is that the tree
semantics are the easiest thing in 14.5 to get wrong and the hardest to debug
later. Building it first means every later phase generates a tree that is
already known-correct.

`DemonCore-Editor/assets/scripts/wl_behavior.py` — hand-written, versioned,
shared by every tree (never generated). Provides `Tree`, `Blackboard`,
`Context`, `Result`, `trace_to_list`, `validate`.

**Verified: `test_behavior_tree.py` — 134 checks. Full regression 1143/1143
across 28 suites.**

**Latest (after NB.4): 1456 checks across 16 suites, 0 failures.** Counts in
this file above are historical snapshots, each taken when it was true; the
authoritative current total is the aggregate runner's output. It globs
`test_*.py`, so "16 suites" is the real file count — earlier "27/28/30 suites"
figures came from the shell aggregator that under-counted CRLF output by 157
checks and has since been replaced.

Implemented: `Sequence` / `Selector` / `Parallel` (one|all policies);
`Invert` / `ForceSuccess` / `ForceFailure` / `Repeat` / `Cooldown` / `Timeout` /
`TickEvery` / `SetTimer`; blackboard with validity distinct from truthiness;
latent resume; the trace hook; and a structural validator that reports errors
**naming the node** rather than dropping data.

Confirmed **zero new bindings** were needed — every API the node set requires
(`Raycast`, `MoveCapsule`, `GetPosition`/`SetPosition`, `HealthComponent`,
`GetEntities`) was already bound, so the plan's transpile proof obligation
holds.

#### Two engine bugs the tests found

- **`Timeout` could never fire.** It stamped `startedAt` only on the way back
  in, and re-stamped it every tick, so the deadline reset forever. A latent task
  that blocks forever is exactly what `Timeout` exists to catch, and it caught
  nothing. Now stamped at the moment of suspension, and disarming **deletes**
  the key — setting it to `None` leaves `"startedAt" in state` true and a stale
  armed state forever.
- **`abort` was not implemented at all.** Re-entry from the root let a
  higher-priority branch run, but nothing released the suspended lower node when
  that branch *decided*. The stale record then corrupted `Cooldown` and
  `Timeout` for the rest of the run, since both consult `_is_suspended_under`.
  `_abort_path()` now clears any suspension the current tick did not travel
  through. This is what `abort="lower"` promises and it was missing.

#### The mistakes that were in the TEST, not the evaluator

Worth recording because each fails silently — the trace shows a clean success
walk-through while the agent does the opposite of what the tree appears to say:

- **`ForceSuccess(IsDead)` gates nothing.** ForceSuccess is unconditional, so
  it succeeds whether or not the agent is dead and the root Selector stops on
  it *every tick*: an NPC that senses nobody, attacks nobody, and looks healthy
  in the trace. It belongs on a cleanup action, never on a branch gate.
- **`Sequence[approach, attack]` cannot express "close the gap OR attack".**
  Already in range fails the approach gate and short-circuits the whole branch,
  so the agent stands in melee range doing nothing. They are alternatives — a
  `Selector`.
- **A latent `MoveTo` stub that returns SUCCESS immediately** makes `approach`
  succeed on tick one, so the Selector never tries `attack`. It must return
  RUNNING while travelling.
- **The approach gate must WRAP MoveTo**, not sit beside it:
  `Sequence[Invert(DistanceTo <= range), MoveTo]`. Written as
  `Sequence[close, move]` it reads as "if in range, move".
- **Measure damage cadence by SPACING, not by a window offset.** Counting from
  the wrong tick reports a correct engine as broken. Sample the hit timestamps
  and assert the gaps equal `attackInterval`.

Two test-harness lessons, both of which had already cost time this session:

- **`grep "FAIL"` matches a PASSING assertion named "…returns FAILURE".** Match
  the checker's own `"  FAIL "` prefix. This produced a false red on a green
  suite.
- **Shared mutable scenario state asserts stale premises.** Blocks that inherit
  `dist` from an earlier block test whatever it happens to be. Set each
  scenario's inputs explicitly.

### 14.5.1b — Visual Blocks prerequisites (DONE) — VB.1-VB.4

The `.wlbtree` compiler is specified to SHARE Visual Blocks' graph model,
serializer, node registry, validator and emitter rather than duplicate them, so
VB.1-VB.4 is a hard prerequisite and not an optional extra. Built in
`Wasteland/src/Wasteland/VisualBlocks/`:

| File | Phase | What it is |
|---|---|---|
| `Graph.h` / `Graph.cpp` | VB.1 | Graph, node, link, variable, blackboard, pin defs, literal parsing |
| `GraphSerializer.h` / `.cpp` | VB.1 | YAML load/save for `.wlblocks` and `.wlbtree` |
| `NodeRegistry.h` / `.cpp` | VB.2 | `NodeDef`, `EmitContext`, the one registration table |
| `NodeLibrary.cpp` | VB.2 | Every BT composite, decorator, condition and task |
| `Validator.h` / `.cpp` | VB.2 | Arity, latent, cycles, types, exec wiring, reachability |
| `Compiler.h` / `.cpp` | VB.3 | `.wlbtree` and `.wlblocks` to Python |

**Deliberately NOT built:** VB.0 (vendoring `imgui-node-editor`) and VB.5/VB.6
(the editor graph canvas). The NB plan names them non-prerequisites, and the
NB plan's own proof obligation is that a **zero-binding** tree runs, so the
canvas would be code nothing tests. Ship it when someone can see it.

#### Decisions that shaped the format

- **Pins are addressed by stable string ID, never by index.** A node gaining a
  pin must not break every saved graph that used the old layout. This is the
  single most important format decision here and the easiest to get wrong.
- **`kind` is a field, not a type.** Flow graphs and behaviour trees share one
  `Graph` and one serializer; only the compiler's shape differs. Two types would
  mean two incompatible registries forever, which is the cost the plan refuses.
- **Tree structure is `ChildIds`, not links.** A tree's structure IS its
  priority, and declaration order is priority -- so it is emitted in order and
  **never sorted**. Sorting would silently change what the NPC does.
- **Latency is declared, not inferred.** The compiler cannot know whether a
  task spans ticks, and one that does but reports success on tick one makes the
  tree stop on it -- the exact bug 14.5's tests had to chase. A `MoveTo` without
  `latent: true` is a WARNING naming the consequence.

#### The rule that shaped the validator: report, never repair

A validator that substitutes a default for a missing required pin, or drops an
unknown node type, turns an authoring mistake into a silently wrong runtime: the
NPC still runs, the graph still "compiles", and the bug surfaces days later as
a design problem. So:

- An **unknown top-level, node, link or pin key is a load error.** A typo in a
  hand- or agent-authored file is reported, not silently ignored.
- A **dangling child or link is a load error**, naming the node.
- A **partial load is impossible** -- the graph is built into a local and
  swapped in only on success. A half graph compiles into a half NPC.
- A **malformed blackboard default is a load error naming the key.** A key
  silently defaulting to 0 because its default was malformed is nearly
  impossible to diagnose later.
- **Errors and warnings are distinct.** A validator that calls everything an
  error trains people to ignore it. Unreachable nodes warn (you may be keeping
  unfinished scaffolding); a missing required pin errors.

#### The security-relevant one

`ConditionCompare` and `ConditionDistanceTo` emit their operator **straight into
the generated Python**, so an unvalidated operator is a code-injection hole in
a generated file. `IsAllowedOperator()` gates it to `== != < <= > >=` at
validate time, and there is a test that `eval(__import__('os').system('x'))` is
rejected as an error.

#### Verification

`test_visualblocks.py` — **101 checks, all passing.** Engine, Python module
and editor all build at Release/x64.

Covers the registry, literal typing, YAML round-trip and determinism, every
unknown-key and dangling-reference rejection, partial-load impossible, decorator
arity, latent warnings, `ForceSuccess` warnings, operator injection, undeclared
blackboard keys, cycles, multiple roots, flow-graph type compatibility, and the
compiler. Two checks are load-bearing:

- **The generated Python is fed to `compile()`.** Without that, a
  string-concatenation bug in the emitter is invisible until an NPC is loaded.
- **The leaf/lambda invariant.** One lambda per leaf, none per composite. A
  generated-if/else compiler would emit a branch per node instead, so the ratio
  is what actually proves the topology stayed data.

#### Three bugs this phase found in code I had just written

- **`Wait` was registered as a decorator.** It wraps nothing -- it suspends on
  its own timer -- so it is a leaf. As a decorator the validator demanded a
  child it can never have, and the shipped example failed to validate.
- **The arity rule was backwards.** It demanded exactly one child for
  composites *and* decorators. Composites take many; that is what a composite
  IS. Only decorators take one, because they reinterpret a single sub-tree.
- **The node registry was never populated from Python.** Nothing called
  `RegisterAllNodes()`, so every lookup returned `None` and every graph
  reported "unknown node type" -- which reads like a data problem, not a
  missing initializer.

#### The build-graph landmine, and the include-order trap

Two things cost far more time than the feature itself, both now recorded in
`native-engine-rendering/references/headless-script-testing.md`:

1. **`Wasteland.lib` is a 393 KB zero-object stub** in `DemonCore-Editor/`,
   shadowing the engine's real 163 MB lib. The module's `ImportLibrary` pointed
   at that same path, so each module link **overwrote the engine lib with the
   module's own import stub** and the next link found no engine symbols at all.
   The module now emits `WastelandPython.lib`.
2. **Include order changes MSVC symbol mangling.** `GraphSerializer.cpp`
   included the forward-declaring header before the completing one, so it
   mangled `Emit(const Graph&)` differently from every caller. Header, source,
   object and library all *looked* correct and the symbol was visibly in the
   `.lib` -- but the definition and its callers disagreed by one character
   (`AEBV` vs `AEBU`). A file defining members of a class that takes that type
   must include the full type first.


Shipped `DemonCore-Editor/assets/trees/EnemyGuard.wlbtree` -- the plan's enemy
tree, authored as data, and it compiles to valid Python with zero errors and
zero warnings.

#### Build note: `Wasteland.lib` is a stale stub

`bin/Release-windows-x86_64/DemonCore-Editor/Wasteland.lib` is a 393 KB
**git-tracked placeholder with zero objects**, while the engine's real output
is a 163 MB lib in `bin/Release-windows-x86_64/Wasteland/`. Building
`WastelandPython.vcxproj` on its own resolves the stub, so **new engine symbols
link as unresolved externals** even though the engine compiled them. The fix is
to build the engine first (or build the editor, which pulls the reference), and
the symptom is `LNK2019` for symbols that plainly exist in the engine objects.
Not a code problem -- a build-graph problem that will recur for any new engine
subsystem.

### 14.5.1b — Remaining: the compiler and the node library

Not started. Still gated on VB.1-VB.4 (graph data model, YAML serializer, node
registry, type checker, validator, Python emitter), because the `.wlbtree`
compiler is specified to **share** those rather than duplicate them — two
incompatible registries forever is the cost the plan explicitly refuses.

Once the compiler lands: NB.2 blackboard codegen, NB.3 `.wlbtree` load/validate/
emit (deterministic — same tree twice must emit byte-identical output), NB.4 the
leaf library, NB.5 the 600-tick differential against `Enemy.py`, NB.6 the editor
tree-list panel. NB.7 (perception senses) and NB.8 (navmesh task) follow.

### 14.5.2 — NavMesh and pathfinding (second)

Unchanged in content: bake from level collision geometry, A* with string
pulling so agents do not walk cell-corner to cell-corner, agent types (walking
height, max slope, radius), path-following with local avoidance, simple
separation before any full RVO.

- `MoveTo` gains a `UsePathfinding` bool pin — off = today's direct behavior.
- A `FollowPath` task with local avoidance so agents do not stack.
- A blocked path re-plans instead of stalling.

**Dependency on 14.1:** pathfinding must query real collision, not authoring
data. With the box-hull contact bug (14.1) still open, a bake over box colliders
bakes a wrong surface — which is why this half cannot go first even though it
is the one with the harder prerequisite.

**Dependency on 14.3:** enemy state should persist.

**Why second rather than first:** 14.5.1 does not need a navmesh. `MoveTo`
walking straight at a target is *today's* behavior and is correct inside an
open arena blockout. Writing the pathfinding abstraction before the tree that
consumes it, against a navmesh that does not exist, produces an interface nobody
has tested. Ship the tree, then upgrade its movement node.

**Test:** a path exists between two reachable points and none between
disconnected ones; path length is finite; an agent following a path reaches its
goal; a blocked path re-plans instead of stalling; and the 14.5.1 differential
test still passes with `UsePathfinding` on.

---

## Phase 14.6 — Terrain and world streaming

Last, because it depends on everything above.

- Heightfield terrain from noise, with material splatting by slope/height.
- Chunked terrain with LOD rings, loaded around the camera and unloaded
  behind it.
- Budgeted async loading so streaming never stalls a frame.
- Persistence across sessions — do not bake terrain into saves.

**Reality check:** terrain is the largest effort here and the least needed for
the current blockout, which is a hand-built arena. It is genuinely last.

---

## Explicitly out of scope for 14

Naming these prevents scope drift:

- Multiplayer. The physics and save work above is the honest prerequisite;
  multiplayer is a separate item.
- Full IK, constraints, motion capture, animation compression from item 13 —
  still missing, but they belong to animation, not to long-tail systems.
- Root motion retargeting.
- Procedural foliage/vegetation.
- **EQS (Environment Query System)** and the rest of UE's behavior-tree
  generation layer — composite parenting, key observers, services, the BT
  debugger. Named in 14.5.1 rather than left to drift into scope; `SenseNearest`
  is where a future EQS would land.

## Sequencing summary

```
14.1 physics stepping        <- fixes an existing broken feature
14.2 inventory + items       <- needs 14.1
14.3 save/load               <- needs 14.2
14.4 audio                   <- independent, any time after
14.5.1 NPC behavior trees    <- needs Visual Blocks VB.1-VB.4, not 14.1
14.5.2 navmesh + pathfinding <- needs 14.1 (correct collision to bake) and 14.5.1
14.6 terrain + streaming     <- needs all of the above
```

Note 14.5.1 depends on **no** physics prerequisite — it is an authoring layer,
and `MoveTo` walking straight at a target is correct in an open blockout. That is
what lets the designer-facing half of 14.5 ship while 14.1's box-hull bug is
still open, which is also when it is most wanted. 14.5.2 is the half that must
wait, because baking a navmesh over a collider surface that is known to be
wrong bakes the wrong surface.

## Verification standard for all of it

The bar already set by items 11-13 and worth keeping:

- Headless Python suites, no GL context, no window.
- Explicit `PYTHONPATH` to the Release output.
- Never call `Scene::OnRuntimeStop()` or drive `ScriptEngine::OnUpdateEntity`
  standalone — both are known to segfault for pre-existing reasons.
- Collidered entities must exist before `OnRuntimeStart()`.
- Real assertions on read-back values, never "did not throw".
- MSBuild Release **and** Debug; clangd include-path diagnostics are noise
  because the PCH is invisible to it.
