# Remaining work — prioritized (P0–P3)

Audit: **2026-10-02**. Verified against files on disk and binaries actually run,
not plan text — three of the plans understated what was already built.

Consolidated from 7 documents in `.hermes/plans/`: `item-14-long-tail-systems`
(35 KB), `npc-behavioral-visual-blocks` (51 KB), `visual-blocks` (18 KB),
`auditory-visual-blocks` (61 KB), `wasteland-age-assurance` (60 KB),
`age-verification-backend` (38 KB), `vb-general-scripting` (6 KB).

Priority key:

- **P0** blocks other work, or invalidates a claim that something is done
- **P1** unblocks a whole downstream system, or is a one-time correctness fix
- **P2** real value, no dependents, safely deferrable
- **P3** tail — large, low near-term payoff, genuinely last

> **Superseded in part by the harness repair below.** The original audit said
> "no green regression" — that was true and is now fixed. See §P0.

---

## P0 — Verification debt

**There was no green regression.** Fixing the interpreter exposed 6 test defects
and 1 real engine bug. All fixed and green now.

- [x] **Pin the interpreter.** `python314._pth` puts CPython in **isolated
      mode**: `PYTHONPATH` is ignored *and* `sys.path[0]` is the install dir, so
      a script's own directory is never added. Every suite needs an explicit
      `sys.path.insert`. Hermes' `python` is 3.14.7; the module links **3.14.5**
      (`pythoncore-3.14-64`). Suites run under the wrong one die at import with
      `0xC0000005`, which reads as a native crash but is not one.
- [x] `tests/` suites run under 3.14.5 — Visual Blocks 54/0, flow 99/0,
      audio 61/61; scratch suites 881/881 (enemy tree 62, node library 662,
      visualblocks 101, wlbtree compiler 56).
- [x] **Age C++ suites: 585 checks, 0 failures** across 11 projects.
- [x] `age_binding_smoke.py`: **60 checks, 0 failures** against the real `.pyd`.
- [ ] Record the interpreter path in the runner so this cannot regress.

### The 7 defects this uncovered

| # | Where | Defect | Kind |
|---|---|---|---|
| 1 | `Bindings.cpp` | `GraphNode::Title` exists in C++ but was **never bound** — Python could not read or set it | **engine** |
| 1b | `GraphSerializer.cpp` | `IsKnownNodeKey` omitted `"title"`, so **every graph containing a titled node was REJECTED on load** — and the writer emits `title` whenever `Title` is non-empty, so a graph the editor saved could not be read back once a node was renamed | **engine** |
| 2 | 3 suites, 11 sites | `g.Nodes = [...]` — the list containers are `PYBIND11_MAKE_OPAQUE` + `bind_vector`, so assignment can never bind; needs `.extend()` | test |
| 3 | `test_audio_clip.py` | `"""docstring"""from __future__` — no newline, the docstring swallowed the import | test (pre-existing) |
| 4 | `test_audio_mixer.py` | `from __future__` placed after other imports | test (pre-existing) |
| 5 | `test_audio_mixer.py` | expected `cos(pi/8)` at 45°; the engine maps lateral→pan angle correctly and **L²+R² = 1.0 exactly** at 0°/45°/90° | test |
| 6 | `test_visual_blocks*.py` | `vb.NewGraph("flow")` does not exist — the real API is `Graph()` + `Kind` | test |
| 7 | `test_visual_blocks_edit.py` | link fixture used `from: n1.exec`; the serializer's format is `from: {node, pin}` | test |

Two tests also encoded the **NB.5 double-strike bug** (`strike` with two
parents) and a shared-child graph — both correctly rejected by the validator
added for that bug. Both were corrected to the single-parent form.

### `test_visual_blocks_edit.py` — 11/11 (was 6 failures / 5 errors)

This suite was targeting a **half-shipped feature**, not a regression. Ten
defects, mostly in its own fixture, which had drifted from the serializer's real
format:

- `type: Float` → `float`; variable types are lowercase
- pins are `pin:`/`value:`, not `id:`/`literal:`
- links are `from: {node, pin}`, not `from: n1.exec`
- `vb.NewGraph("flow")` does not exist → `Graph()` + `Kind`
- `vb.Save` returns `None` on success (a `GraphError` otherwise), not `True`
- `vb.CompileFlow` returns `str`, not a tuple
- `GreaterThan` is a **math node with no exec pins**, yet the fixture wired exec
  into it — so `CompileFlow` correctly refused the graph and returned `''`
- `Add`'s output pin is `Value`, not `Out`, and it must feed an `Any` pin
  (`SetVariable.Value`), not a `String` pin (`Log.Message`)
- an unwired math node emits nothing, so the "edit reaches the generated code"
  test needed `n4` actually wired to be observable
- the `title:` count assertion was a fragile proxy; it now checks that untitled
  nodes stay untitled through a round trip

**The string-quoting test was itself wrong.** It asserted `'\"hi\"'` appears in
the saved file. yaml-cpp emits a plain scalar when the value is unambiguous and
quotes on read, so `he said "hi"` is valid YAML. Verified all of
`he said "hi"` / `key: value` / `#hash` / `- dash` / `a: b: c` round-trip
byte-identically. The test was rewritten to assert the property that actually
matters — the **value survives** — rather than the presence of a quote char.

### Verified after the fixes

```
scratch aggregate   881/881  (enemy 62, node library 662, visualblocks 101,
                             wlbtree 56 ... 4 suites, 0 failures)
tests/              54/0, 99/0, 11/11 OK, audio 61/61
Age C++             585 checks, 0 failures (11 projects)
age_binding_smoke   60 checks, 0 failures
```

---

## P0 — Editor proof (screenshot, not a clean build)

- [ ] **NB.6:** relaunch the editor, NPC Behavior panel, load
      `EnemyChase.wlbtree`, confirm **0 unknown-node errors**. The
      self-registering registry fix is in source but **has never been seen
      working in the running editor** — the original defect was found from a
      screenshot.
- [ ] **`.wlblocks`:** `NodeCanvas` + `VisualBlocksPanel` are built and wired but
      **never visually verified**. Canvas hit-testing is the likely silent bug.
- [ ] Editor **Debug** build — never run for any of this.
- [ ] Both at 4K with a real graph loaded.

---

## P1 — Scene-startup tree attachment

The gap that makes authored trees a demo rather than a feature.

- [ ] **Scene startup loads, compiles and attaches a `.wlbtree`.** `wlbtree`
      currently appears only in `Bindings.cpp`, `BehaviorTree.h`, `Graph.h`,
      `GraphSerializer.h` — **no code path wires a tree to an entity at load**.
- [ ] Port `Enemy.py` (200 lines) onto the authored tree.
- [ ] `UsePathfinding` pin (blocked on navmesh below).

---

## P1 — AVB.1 audio output backend

**No sound has ever been heard in this engine.** The mixer is complete and
correct (61/61 checks); nothing sends samples to a device. Verified absent
across all engine sources:

```
WASAPI 0  miniaudio 0  WinMM 0  waveOut 0
IAudioClient 0  SDL_Audio 0  OpenAL 0  FMOD 0  RtAudio 0
```

Gates **AVB.2–AVB.11** (11 phases, ~58 KB of plan) and sits behind the mixer by
design — low-risk, high-leverage.

- [ ] WASAPI or vendored miniaudio device backend
- [ ] Then DSP → voices → graph/serializer/validator → buses → effects/reverb
      → runtime → wave-2 + `DoomArray` → track/sequencer → editor → recording

Also open: `.wastescene` audio-source serialization, compression, clip
streaming, editor audition UI. **No footstep/weapon/hit/UI assets exist**, so
there is nothing audible to wire up even after the backend lands.

---

## P1 — Item 14.1 physics

### All four of the plan's claims are DISPROVEN. `tests/headless/test_physics_3d.py` — 5/5

The plan's reproducer `test_physics_3d.py` did not exist, so these claims had
never been tested. It does now, written from the components' own contracts:

| Plan claim | Measured | Verdict |
|---|---|---|
| box floor pushes a body **upward** +0.05/step | box falls 6.000 → 4.764; rests at **0.4999** (floor top 0 + half-height 0.5) | **false** |
| late-spawned entities never simulate | a body added after `OnSimulationStart()` falls 5.000 → 3.764 | **false** |
| `StepPhysics` clamps dt, so 30fps runs at half speed | 1.0s of 30fps frames falls **4.946 m** (ideal 4.905) | **false** |

Box **and** sphere both measured, so a shape-specific fault would show as a
shape-specific failure. A body resting at 0.4999 and staying at 0.4999 two
seconds later is correct contact behaviour.

**Why the claims were never caught:** `OnPhysics3DStart` / `OnPhysics2DStart`
were **not bound to Python**. A headless script could call `StepPhysics` forever
and see nothing move — no Box3D world means no solver — with no error at all.
`OnSimulationStart()` happens to reach them, which hid the trap: the first
version of this suite reported `0/0 checks`, a green that proved nothing.
`OnSimulationStart()` is the supported entry point and the suite uses it. An
attempt to bind the four `OnPhysics*Start`/`Stop` methods directly was reverted:
they are `private` **by design** (bringing a physics world up is a
scene-lifecycle action), and the compiler correctly refused with `C2248`. The
discovery is documented at the `StepPhysics` binding instead, so the next person
to write a physics suite does not lose an afternoon to a silent no-op.

Also note: there are **zero `b3HullShape_CreateBox` calls** in the engine.
`BoxCollider3DComponent` is real and is authorable — it goes through
`b3MakeOffsetBoxHull` — but a grep for a hull path finds nothing, so the plan's
description of the mechanism does not match the code.

### Still genuinely open

- [ ] **No apply-impulse / set-velocity** — scripts can read physics, not drive
      it. Nothing can jump, knock back, or launch.
- [ ] Re-verify after the `OnPhysics*Start` bindings land.

---

## P1 — NavMesh and pathfinding (14.5.2)

Zero files. Was blocked on the box-hull bug above — **that blocker is now gone**,
so this can start. Still worth baking against *real* collision rather than
authoring data.

- [ ] Bake from level collision geometry
- [ ] A* with string pulling (agents must not walk cell-corner to cell-corner)
- [ ] Agent types: walking height, max slope, radius
- [ ] Path-following with local avoidance; separation before full RVO
- [ ] `MoveTo` gains `UsePathfinding` (off = today's direct behavior)
- [ ] `FollowPath` task; a blocked path re-plans instead of stalling

---

## P2 — Age assurance: 4 gaps in an otherwise-complete module

23 files in `Age/`, all registered in `Wasteland.vcxproj`; 11 suites /
585 checks / 0 failures; Python bindings 60/60.

- [x] **`test_no_identity_types` — WRITTEN, 17/17.** `tests/test_no_identity_types.py`.
      Reads the Age sources and asserts no string member is named like an
      identifier. Name-based rather than type-based on purpose: a type rule
      ("no `std::string`") cannot tell `m_Name = "apple-declared-age-range"`
      from a user handle. Allowlist entries each carry a reason, and a
      stale-entry check stops the list rotting after a rename.
      It is a SOURCE-reading test, so it runs with no module and no build —
      `t.sh d1`. Verified it actually fires: planting `std::string
      m_AccountId;` makes it exit 1 with two failures.
- [x] **`SeedingSmokeTest` — WIRED, and it PASSES.** `tools/tnative.sh`
      (also `t.sh native`). It had never run because it needs an assets dir
      on the command line and **silently SKIPs, exit 0**, when it cannot find
      one — so a wrong path looks identical to a success. That is exactly why
      it stayed orphaned unnoticed. Against the editor assets it reports
      `RESULT: PASS`: all 7 required shaders resolve and are non-empty, 13
      files seeded. The runner treats a SKIP as a FAILURE so it cannot recur.
- [ ] **No ImGui panel.** `GuardianToolsView`/`GuardianToolsLayer` are logic, not
      UI. Task 11 has no editor surface.
- [ ] **`AgeGate.py` not confirmed wired to a scene.**

`age-verification-backend.md` is a **separate deliverable** — a backend service
in an isolated venv, 14 tasks, all unstarted. Do not conflate it with the
in-engine C++ module that is built.

---

## P2 — OBJ GPU / scene / editor integration

CPU OBJ parsing is done and tested (84/84). No GPU upload, no scene or editor
path — so an OBJ mesh cannot be placed.

---

## P2 — Animation long tail

Curves, skeletons, quaternions, clips, animators, state machines, skinning all
exist (184/184). Missing: ECS components, serialization, GPU skinning, asset
interchange, retargeting, IK, constraints, editor authoring. Item 14 puts
IK/constraints/mocap compression out of scope as "animation, not long-tail".

---

## P2 — External Necroware project sync

**Checked 2026-10-03: the path does not exist.**
`C:\Users\rtoue\Documents\Wasteland Projects` exists but is EMPTY, so
there is no Necroware project to sync. This item is not "unverified" — there
is nothing on disk. Either it was never created, or it lives somewhere else.
Worth asking before spending time on it.

Related: `SeedSmokeTest` under that same parent is created and removed by
`tools/tnative.sh` on each run, so the directory stays empty by design.

---

## P3 — Terrain and streaming (14.6)

Zero files. Last because it depends on everything above.

- [ ] Heightfield from noise, splatting by slope/height
- [ ] Chunked terrain, LOD rings, load around camera / unload behind
- [ ] Budgeted async loading so streaming never stalls a frame
- [ ] Persistence across sessions — do not bake terrain into saves

---

## P3 — Housekeeping

- [x] `DemonCore-Editor/src/Panels/.hermes-tmp.MUiCbE` — **deleted.** Verified
      first that it was a stale pre-fix copy of `NPCBehaviorPanel.cpp`
      (it still contained the `ImGui::BeginItemTooltip` call that this
      vendored ImGui 1.89.3 does not have), not unique work. The real file is
      registered in the vcxproj and carries the fix.
- [ ] Asset mirroring does not run when the binary is up to date — consider
      `CopyToOutputDirectory=PreserveNewest` content items or an always-run
      mirror target, so a new asset cannot silently go missing from output.

---

## Build-environment hazards (not code)

Recorded in `tools/msb.sh` and the `native-engine-rendering` skill.

- **Never pipe MSBuild into `grep`/`tail`** — it returns before MSBuild finishes,
  and the next command then races the build it was waiting on. Use
  `> log.txt`, or background + notify.
- `vendor/imgui/imgui.h` had **two version blocks merged** (1.89.3 + 1.88),
  defining `IMGUI_CHECKVERSION()` twice → `CL.exe exited with code 1` with **no
  diagnostic**. Committed in git, so long-standing. Fixed.
- `/FS` on `ClCompile` only, never `<Link>`.
- `MinimalRebuild=false` is not a PDB-race fix; it breaks tlog cleanup (MSB6003).
- Wedged `mspdbsrv` → `C1090 ... error code '23' (0x000006BA)`. Restart it.
- **Never kill builds broadly.** Wait, or target one PID after confirming it is
  not the agent's own runtime.

---

## Scene-startup tree attachment — BUILT, first test run had real failures

`Project` is now bound (it had NO binding at all), plus
`BehaviorTreeComponent`, six Entity accessors, `Scene.OnUpdateRuntime`, and
`Scene.CompileBehaviorTree`. `WastelandPython.vcxproj` builds **RC=0, zero
errors**, and the module was verified as the one the tests load
(`bin/Release-windows-x86_64/DemonCore-Editor/Wasteland.pyd`, not the stale
scratch copies).

New engine fixes along the way:

- **`Scene::DestroyEntity` called NO teardown at all** — not
  `ScriptEngine::OnDestroyEntity`, not a tree teardown. Every destroyed entity
  leaked its Python instance for the life of the process. Both now called,
  before the registry drops `IDComponent`.
- `OnComponentAdded<BehaviorTreeComponent>` was missing. The primary template
  is `static_assert(false)`, so the linker wanted the symbol and the whole
  module failed `LNK1120` — a new component type needs an explicit
  specialization or nothing links.
- `tools/msb.sh` waited for `MSBuild.exe` to disappear, but MSBuild keeps idle
  `nodeReuse` workers alive by design, so it always burned its full 10-minute
  timeout and then got killed mid-LTCG **with no error in the log**. Now waits
  on `cl.exe`/`link.exe` only.

The pybind11 error that blocked three build rounds is worth remembering because
**MSVC reported it from inside `pybind11.h` with no line number in our file**:
a `.def()` on `class_<Scene>` whose lambda takes no `Scene&` makes pybind expect
a method signature plus an implicit `this`, so the annotation count no longer
matched. Fixed by making it `def_static` — which it should have been, since
compiling a tree needs no scene.

### `tests/headless/test_tree_attach.py` — first run: 3/5, two real defects

The suite exists and drives the real path (attach -> `OnUpdateRuntime` ->
assert health dropped), with negative controls for a missing tree, an unticked
tree, and a dead NPC. It immediately caught a genuine bug:

- **`types.Module` does not exist.** It is `types.ModuleType`, and the name
  must be passed as `py::str`. This failed at RUNTIME as a bare AttributeError
  that surfaced as an NPC that never attaches and never explains why.

### `test_tree_attach.py` — 5/5. **AUTHORED TREES NOW RUN IN A LIVE SCENE.**

```
health 100.0 -> 52.0        (48 damage = exactly 4 hits x the authored 12.0)
StepPhysics  x40 -> health 100.0, Attached=False
OnUpdateRuntime x40 -> health 52.0, Attached=True
dead NPC -> victim health 100.0
missing tree -> CompileError names the exact path
```

That middle pair is the proof worth having. **Same entity, same scene, same
tree — the only difference is which entry point ticks it.** `StepPhysics` does
not touch behaviour trees, so the control genuinely does nothing and the
positive case genuinely does something. A suite that only asserted the happy
path would pass just as well against a system that was entirely inert.

So: **an authored `.wlbtree` now drives an entity through a real scene.** The
`EnemyChase.wlbtree` demo is live runtime content, and `Enemy.py` can now
actually be migrated rather than merely planned.

### Ground following, so the Enemy.py migration is not a regression

Auditing `Enemy.py` against the tree before migrating it found a real gap: the
generated `_move` shim moved in a straight 3D line, interpolating Y toward the
TARGET's Y. That is not ground height, so an NPC on a slope hovers going up and
sinks coming down. `Enemy.py` solved this with an explicit `GroundHeightAt`
probe.

Migrating without it would have been a **silent regression on any level with
slopes** -- it presents as "the NPC looks broken", not as a port error. So:

- `_ground_height(ctx, ...)` probes under the NEW position and snaps to the
  surface, using `Enemy.py`'s exact 6-tuple `Raycast` contract
- returns `None`, never `0.0`, when nothing is hit, so an NPC over empty space
  keeps its height instead of teleporting to the void floor
- constants live once, next to the helper

`tests/headless/test_ground_snap.py` (4 checks) pins it. Writing that suite
surfaced a second lesson worth more than the code:

**A tree that attaches cleanly and does nothing is a perception problem.** The
target needs a `HealthComponent` (`SenseNearest` skips entities without one),
both entities must be in ONE scene (the sweep searches its own world), and the
radius is 3D -- a target 60m straight down is correctly not acquired inside a
30m radius. All three looked exactly like "movement is broken".

Critically: **the existing damage suite passes while sensing is broken**,
because damage only needs a target within attack range. A green attack test is
not evidence a chase test will move.

### Enemy.py is migrated onto the tree

`Enemy.py` is now a shim: it attaches `trees/EnemyChase.wlbtree` and its
`OnUpdateEntity` is deliberately empty. The original is kept verbatim as
`Enemy_legacy_reference.py` so the port can be diffed.

The shim exists rather than deleting the file because `ExampleGame.wastescene`
references it by ScriptPath/ScriptName, and because it is where the tunables
are written in a form a human reviews. It carries the numbers as class
constants; `tests/test_enemy_migration.py` (23 checks) parses them back OUT OF
THE COMPILED TREE and asserts they match, so the Python copy and the authored
tree cannot drift apart silently.

**Two real bugs died with the old `FindTarget`:**

```
legacy used Scene.GetEntities()    : True   # empty for runtime-created entities
legacy used 'other == self.entity' : True   # wrapper identity is unreliable
```

Both were already diagnosed and fixed in the generated `_sense_nearest`, which
uses `GetAllEntitiesWithTransform()` and compares exact position instead. So
this port is not only a move, it is a behaviour FIX: the legacy enemy was
effectively blind to any runtime-spawned entity, and could target itself.

The shim's tick is empty because `Scene::OnUpdateRuntime` ticks
`BehaviorTreeSystem` AFTER scripts. Anything in the script would move the enemy
once and then the tree would move it again. That is asserted, not assumed: a
test measures distance travelled in 1.0s and fails above 4.5m against an
authored 3.0 m/s.

### All three targets green

```
WastelandPython.vcxproj   RC=0   pyd 00:32:05
DemonCore-Editor.vcxproj  RC=0   exe 01:27:37, 8177664 bytes
assets mirrored: ai graphs scenes scripts shaders textures trees
                + all 3 .wlbtree and scripts/wl_behavior.py
tests/headless   75/75 (5 suites)   enemy migration 23/23   scratch 959/959   tests/ 54/0 99/0 edit OK   age 60/60
```

### The editor build fought back, and the cause was not in our code

It failed five different ways before it built: `MSB6006 CL.exe exited with
code 1` with **no diagnostic**, `C1083` on the PCH, `C1041` PDB contention, and
a bare `LNK2019`. Three real findings:

- **`/bigobj` was missing globally.** During LTCG codegen an object-file
  section overflow surfaces as a bare process exit, not a compile error. The
  module project passed `/bigobj` locally and built fine while the editor did
  not — which made this look like a flaky build rather than a missing flag.
  Now set once in `Directory.Build.props` next to `/FS`.
- **An external MSBuild was building `Wasteland.vcxproj` concurrently**
  (`-v:m -m:1`, same shape as our tooling). Two builds on the same
  intermediates produce every one of the above symptoms. Found by walking
  `cl.exe` -> `Tracker.exe` -> `MSBuild.exe` parents when a compiler appeared
  with no build of mine running.
- **That external build had reverted part of my work.** `git status` aside,
  the check that mattered: grep the source for the change. The
  `OnComponentAdded<BehaviorTreeComponent>` specialization was gone from
  `Scene.cpp` (the only casualty), causing the final `LNK2019`. Re-added and
  re-verified.

**Never trust a `RC=0`/`RC=1` from a build that overlapped another.** Check
parents before attributing a failure to your own change.

Two test-authoring traps this suite hit, both worth not repeating:

- **`check_le(a, b)` is an UPPER bound.** Asserting "damage happened" with
  `check_le(0.0, dealt)` asserts the opposite, and a zero-damage tree passes
  it. Lower bounds need `check()`.
- **Components handed to `AddOrReplace*` are SNAPSHOT COPIES.** Reading
  `.Current` off the object you passed in shows the value from attach time,
  forever -- so a working tree reported 0 damage. Every assertion must re-read
  from the entity (`entity.GetHealthComponent().Current`). This is the same
  class of bug as pybind entity wrappers failing `is`/`==`.

## Order

```
P0  fix the harness            (done)
P0  editor screenshots         (needs a real UI run)
P1  bind Project + prove tree attachment        (DONE -- 5/5, 48 damage)
P1  migrate Enemy.py onto EnemyChase.wlbtree    (now unblocked)
P1  AVB.1 audio backend                      (unblocks 11 phases)
P1  NavMesh + pathfinding       (physics blockers now DISPROVEN)
P2  Age: D1 guard, SeedingSmokeTest, panel
P2  OBJ GPU integration; animation long tail
P3  terrain + streaming; housekeeping
```

P0–P1 is cleanup and proof for work **already written**. The genuinely new
systems left are the audio backend and navmesh — the physics fixes turned out
not to exist.

### NB.8 `MoveTo` pathfinding — IMPLEMENTED, shim verified 10/10

`MoveTo` gains a `UsePathfinding` bool pin. **Default false, deliberately**: every
`.wlbtree` on disk omits the pin, and a true default would silently change all of
them on the next rebuild. Off costs nothing.

- `NodeLibrary.cpp` — the pin; emits `True`/`False` (not `1`/`0`).
- `Compiler.cpp` shim — `_agent_key`, `_navmesh`, `_path_to`; `_move` rewritten.
- `Scene.h/.cpp` — `BakeNavMesh` / `GetNavMesh` / `ClearNavMesh`, one `m_NavMesh`
  shared by every NPC. A per-tree bake would make pathfinding scale with NPC count.
- `Bindings.cpp` — added to the EXISTING `py::class_<Scene>` registration.
  Re-registering `py::class_<Scene>` mid-file is a pybind error; there is exactly
  one Scene binding in that file and the methods go on it.

**Two real bugs found and fixed by testing, not by reading:**

1. *Drift check was inverted.* It compared the agent to the waypoint it was
   walking TOWARD, so every agent looked lost and the route was re-planned every
   tick — 32 `FindPath` calls over 40 ticks. Now measures perpendicular distance
   to the traversed SEGMENT. Re-plans dropped to 5, then to 1.
2. *Reaching a waypoint discarded the route.* The `distance <= 1e-6` branch
   treated "standing on the current waypoint" as "path exhausted" and dropped
   every remaining waypoint. The NPC walked to the corner, re-planned, walked
   back, and froze there forever. Now advances to the next waypoint.

Verified without a build: `tests/headless/test_shim_pathfinding.py` extracts the
`R"WL_SHIM(...)"` literal and runs it against fakes — **10/10**, full detour
completes in 77 ticks with exactly **1** `FindPath` call for the whole 16 m trip.

`tests/headless/test_pathfinding_node.py` covers the same ground against the real
module and is **written but unrun** — needs the link to clear.

### NavMesh wall routing — BUILT AND VERIFIED 28/28

The bake fix was **incomplete**. Two further defects, both found only once a
binary finally contained them:

1. **The wall's own top was baked as walkable.** Choosing a candidate surface
   highest-first is not enough: a wall top trivially passes the headroom test,
   because an agent *could* stand there if it could reach it. Added `IsGround`,
   which rejects a surface thicker than `StepHeight + AgentHeight` -- a rooftop,
   not ground. Cells under a wall now resolve to the floor beneath.

2. **String pulling deleted the detour.** `HasLineOfWalk` raycast against the
   PHYSICS WORLD. With no physics world (headless, or before
   `OnSimulationStart`) the raycast hits nothing, every line reports clear, and
   `PullString` collapsed the whole route to two points -- straight through the
   wall, while the mesh itself was provably correct. It now walks the BAKED
   GRID. This is the invariant: pulling may SHORTEN a route, never change where
   it goes.

`tests/headless/test_navmesh.py` **28/28** and
`tests/headless/test_pathfinding_node.py` **15/15** against a real module.

Both were invisible while the build was blocked -- the source was right and the
tests were wrong in three separate ways (see the `Vec3` arithmetic, the
length-not-geometry detour assertion, and the "unreachable goal" that the ring
search quietly makes reachable).

### NB.7 perception — IMPLEMENTED, 24/24 verified

The shared, throttled perception cache the plan called for, plus real occlusion.

- `_perception_snapshot(ctx, scene)` — one snapshot of live perceivable entities,
  **held on the SCENE**, rebuilt every `_PERCEPTION_INTERVAL` = 0.2 s.
  Cost drops from O(R*N) per tick to O(R) per interval. On the scene, not the
  tree: a per-tree cache is one scan per NPC, which is the cost being removed.
- `_los_blocked(...)` — real `Scene.Raycast` trace, six-value contract. An
  entity-less hit is level geometry and blocks; a hit on the target itself does
  not. Returns False when no trace is possible, because "cannot prove blocked"
  and "is blocked" are different claims.
- `_sense_nearest(..., half_angle_deg=None, require_los=False)` — both new
  params default to the pre-NB.7 behaviour, so every authored tree senses
  exactly as it did.
- `_has_los(ctx, ...)` — signature CHANGED to take ctx. It was distance-only and
  reported "visible" through walls; that was documented as a limitation, but it
  meant `HasLineOfSight` was measuring distance and calling it sight.

New pins on `SenseNearest`: `HalfAngle` (0 = all directions) and `RequireLOS`.
**`HalfAngle` 0 must mean "no cone", not a zero-width cone** — 0 is what an
author leaves on a pin they do not care about, so taking it literally would make
every untouched node in every authored tree silently blind.

**A real bug fixed, and it predates NB.7.** `_sense_nearest` ended with
`ctx.bb.set(key, best)` with `best == None` when nothing was found. The
blackboard tracks validity SEPARATELY from value, so that marked the key
*written*: a `BlackboardIsSet(key)` gate read true forever and the branch that
re-acquires a target could never run again. Every existing test passed, because
they checked the VALUE — which reads `None` either way — and never whether the
key was still unset. Now `clear()` on the empty branch, and the negative case
asserts `key not in bb.valid`.

Verified: `tests/headless/test_perception_cache.py` **24/24**, extracting the
shim and running it against fakes. Includes a scan-count assertion with the
ceiling DERIVED from `ticks * dt / interval` rather than picked, and a check
that the cache lives on the scene.



The plan says do it after the editor. `SenseNearest` remains O(n) per NPC.

## 2026-10-05 — Phase 1 closed, and the C4010 macro trap

### Verified state
```
headless                174/174 across 15 suites
  test_audio_clip 35 · test_audio_mixer 28 · test_audio_scene 6
  test_ground_snap 4 · test_inventory 22 · test_los_contract 3
  test_navmesh 10 · test_pathfinding_node 6 · test_perception_cache 13
  test_physics_3d 5 · test_physics_controls 6 · test_physics_timestep 3
  test_savegame 21 · test_shim_pathfinding 7 · test_tree_attach 5
tests/                  test_no_identity_types 17 · test_enemy_migration 23
                        test_shim_arity 97
visualblocks           54/0 · 99/0 · edit OK
native C++             PASS
```

### Closed this session
| Item | Status |
|---|---|
| AVB-1c bind AudioSource/AudioListener | DONE — 6 accessors each, verified via `test_audio_scene` |
| AVB-1b clamp SetMasterVolume to [0,1] | DONE — `Mixer.h:254`, comment rewritten to explain saturation |
| 5.2 ApplyImpulse / SetLinearVelocity / GetLinearVelocity | DONE — declared, defined, bound, 6/6 |
| COV-1 `test_savegame.py` | DONE — 21 checks, 70 value assertions |
| COV-2 `test_inventory.py` | DONE — 22 checks, 141 value assertions |
| COV-3 audio scene layer | DONE — `test_audio_scene.py`, 6/6 |
| §2.2 stale timestep comment | DONE — removed |
| §2.2b `_los_blocked` tuple contract | DONE (earlier) |
| ST-3 module-under-test banner | DONE — prints path + mtime, warns when older than source |
| NB.9 `SetTimer` + RuntimeTypes cross-check | still OPEN |
| 5.1 `SyncPendingPhysics2D` | still OPEN |

### The C4010 macro trap — worth more than any single fix above

A `//` comment inside a multi-line macro continuation is **silently discarded by
MSVC**. The line ends in `\` so the macro continues, but the comment text is
dropped before the continuation is processed. The only symptom is

```
warning C4010: single-line comment contains line-continuation character
```

and the macro expands to only its code lines. With comments between
continuations it expands to **nothing**.

Observed in production: 22 `BIND_GAMEPLAY_COMP` invocations, a build reporting
**zero errors**, and an `Entity` class with **no component accessors at all** —
35 headless checks red with `AttributeError: ... has no attribute
'AddOrReplaceHealthComponent'`. Grepping the source proved nothing, because the
source was correct; the *expansion* was not.

Rules:
- All comments above the `#define`, never inside the body.
- `warning C4010` anywhere means a macro body is losing lines. Treat as an error.
- Verify with `cl /EP file.cpp`. Never trust a clean build for this.

### Interpreter: no longer hardcoded

`C:/Users/<user>/AppData/Local/Python/pythoncore-3.14-64` stopped existing on
2026-10-04, and reinstalling to it fails with *Access is denied* (something
holds the directory). Note that `where /R C:\ python.exe` still REPORTS the
missing interpreter from a stale cache — verify by RUNNING, never by stat-ing.

`tools/t.sh` now resolves a working CPython 3.14.5 dynamically and checks
`sys.version_info[:3] == (3,14,5)`. 3.14.7 is ABI-incompatible.

### Test-authoring lessons from this session

1. **A test that requires defaults may be the wrong test.** The emitters call
   shim helpers POSITIONALLY, so required parameters are correct by design.
   "Every parameter must have a default" would have meant rewriting 25 working
   signatures to placate an assertion with no evidence behind it.
2. **Flatten the emitter before regexing it.** Node emitters build output by C++
   string concatenation across several lines with comments between fragments, so
   a line-scoped regex reports "pin not forwarded" for a pin that IS forwarded.
   Strip comments, collapse whitespace, then match. Assert on pin NAMES, not
   argument counts.
3. **A suite that defines `test_*` and has no `__main__` exits 0 having checked
   nothing.** `test_shim_arity.py` did exactly this and looked green.
4. **`tests/` needed its own runner.** `runner.py` lives in `tests/headless/`; a
   top-level suite importing it failed. `tests/runner.py` now exists and is
   module-independent, so static source audits need no engine module at all.
5. **Components passed to `AddOrReplace*` are snapshot copies.** Re-read from
   the entity, always.

### ST-2 — 236 files staged by explicit path (2026-10-05)

The untracked-implementation hazard was larger than the route map recorded. With
`git status -uall` the real figure is **239 untracked**, not the handful a
collapsed `git status` shows:

```
engine-core   29   VisualBlocks/ Navigation/ Audio/ BehaviorTreeSystem SaveGame BindingsAge
engine-rest   40   Age/ Animation/ Project/ Core/Gamepad
editor        10   VisualBlocksPanel NPCBehaviorPanel NodeCanvas GraphHistory ValueEditor
assets        23   trees/ graphs/ scripts/
tests         74
tools         19   msb.sh buildpy*.sh t.sh
plans-other   41
              ---
              236 staged
```

Every one of those subsystems shipped with **zero git history**. That is a
correctness hazard, not hygiene: an untracked file has no history, so a revert is
invisible to every git-based check. It has already happened once —
`OnComponentAdded<BehaviorTreeComponent>` vanished from `Scene.cpp` and surfaced
only as `LNK2019`.

**Renderer files deliberately NOT staged** (5): `Renderer3D_NovaRenderer.spv`,
`MeshLoader.{cpp,h}`, `ProceduralMeshes.{cpp,h}`. Per R23 the renderer is
user-owned and in flight.

Staged with `git add -- <explicit paths>`, batched, never `-A`. A credential scan
(private-key blocks, AWS/GitHub/Slack tokens, `api_key=`/`password=`/`token=`
literals) over all 236 files found **no matches** before staging.

**Not committed.** Staging is the reversible half; committing is not, the user
has not asked for it, and the renderer work is still in flight. Suggested order
when they do: engine-core -> engine-rest -> editor -> assets -> tests -> tools
-> plans, building between stages.

### ST-1 — the scratch suites are GONE, not merely at risk

ST-1 said "land the four scratch suites in `tests/` before they evaporate". They
have already evaporated. `find` across both the repo and the hermes cache returns
nothing for `test_node_library.py`, `test_enemy_tree.py`,
`test_wlbtree_compiler.py`, `run_all_suites.py`.

The 1209-check node-library suite — the sole guard against a leaf emitter drifting
from its shim signature, the bug class that made `HasLineOfSight` dead at
runtime — no longer exists in any form. So ST-1 is no longer a move; it is a
**rewrite**.

What now stands in its place, all in-repo:
- `tests/test_shim_arity.py` 97/97 — static AST audit: every emitted helper call
  vs the real shim signatures, plus the pinned `_has_los`/`_move`/`_sense_nearest`
  contracts, Raycast indices, `clear()`-not-`set(None)`, bounded perception.
- `tests/test_enemy_migration.py` 23/23 — compiled-source and runtime behaviour.
- `tests/headless/test_perception_cache.py`, `test_pathfinding_node.py`,
  `test_shim_pathfinding.py`, `test_ground_snap.py` — NB.7/NB.8 behaviour.

That is real coverage of the *contracts*, but it is NOT the 1209 checks of
per-leaf-emitter detail. Writing that suite again is genuinely worthwhile and is
now the highest-value uncovered item.

### Two real SaveGame defects, found by writing COV-1 (2026-10-05)

`SaveGame::Load`'s header promises false-on-malformed. That holds for the YAML
parse — and for **nothing below it**. Both verified in source, both now pinned by
`test_savegame.py` asserting TODAY's behaviour with an explicit "invert this when
the fix lands" note, so the fix reads as a signal rather than a regression.

**1. A non-numeric UUID raises out of `Load`.** `StringToUUID` is an unguarded
`std::stoull`. It throws `std::invalid_argument` on a non-numeric string and
`std::out_of_range` on overflow, and neither is caught at `StringToUUID` or at
its single call site in `ReadEntity`. A hand-edited or truncated save therefore
propagates an exception out of a function documented never to throw one.

**2. A truncated save raises.** A file cut inside a `Position` sequence still
parses and passes the version check; `ReadEntity`'s positional `.as<float>()`
then throws.

Why this matters more than a normal defect: the documented failure mode in the
item-14 plan is *"half-loading a save the build does not understand silently
drops earned progress"*. These are the adjacent case — a corrupted save takes
the game down instead of degrading. Route A: wrap the `StringToUUID` conversion
and the per-entity read in a catch that fails the load cleanly, leaving the
running world untouched (which `test_savegame.py` already asserts for the
schema-version cases).

### `B3_NULL_ID` is a braced initialiser, not a value

`box3d/id.h:79` and `:83` define `#define B3_NULL_ID {}` / `{ 0 }`. So

```cpp
bodyId == B3_NULL_ID      //  expands to  bodyId == { 0 }   ->  C2059 / C2143
```

fails **at the comparison site**, naming neither the macro nor the intent — the
error points at innocent-looking physics code. Two agents hit this independently.
Correct forms: `b3_nullBodyId` for initialisation/returns, `!b3Body_IsValid(id)`
for guards. Audited the three remaining `B3_NULL_ID` uses in `Scene.cpp` (L1441,
L1569, plus prose) — all are initialiser positions, so all are safe.

### Exit 139 after all-green is the teardown, not a test failure

`tools/t.sh headless` prints `174/174 checks passed across 15 suite(s)` and then
exits **139**. Each suite run alone exits 0 (verified for `test_ground_snap`,
`test_tree_attach`, `test_physics_controls`). This is the R24 interpreter-teardown
crash, and per the documented rule the printed checks are the verdict. Do not
"fix" it by deleting suites.

### NB-9 DONE + mutation-verified (2026-10-05)

`SetTimer` is registered (`NodeLibraryV1.cpp:452`, `def.TypeId = "SetTimer"`).
It was in `BehaviorTree::RuntimeTypes()` and had a runtime handler
(`wl_behavior.py` `_eval_set_timer`) from the start, but **no NodeDef ever
registered it** — so a tree naming it validated, compiled, and then died at load
with "unknown node type".

Filed as `BtRole "task"`, **not** "decorator", despite `wl_behavior.py` listing
SetTimer among its DECORATORS. Those two lists answer different questions:
`DECORATORS` there means "types exempt from the leaf-must-carry-a-body check",
and SetTimer is exempt precisely because it carries no `leaf` key. `BtRole` is
what the VALIDATOR reads, and it demands exactly one child of a decorator
(`Validator.cpp:628`). SetTimer wraps nothing, so "decorator" would have made
every legal use of the node a validation error. `Wait` — the other runtime leaf —
is filed "task" for the same reason.

`tests/headless/test_runtime_types.py` — 15 checks, cross-checking
RuntimeTypes() against the registry in BOTH directions (one-directional coverage
hides half the bug), plus the role lists, flow-node exclusion, uniqueness, and
five SetTimer-specific behaviours including "deadline stamped once and held".

#### The mutation test — and why the first one proved nothing

The first mutation attempt commented out the `SetTimer` registration, rebuilt,
and reported a failure. The failure was:

```
error LNK1181: cannot open input file '...wlpch.obj'
```

That is a **missing precompiled header**, not the missing-node-type defect. The
build broke for an unrelated reason and would have broken identically with the
node still present. A mutation test that fails for the wrong reason reads exactly
like one that worked; the tell is that the error names a build input rather than
the behaviour under test.

Redone properly — build clean first, then mutate, then confirm the failure is the
EXPECTED one:

```
mutated build   MSBUILD_RC=0, 7,710,720 bytes   (mutation does not break the BUILD)
test_runtime_types   15/15 -> 5/15              (10 checks catch it)
restored       sha 8dd12be2a0b3cf8c, byte-identical
```

The mutation not breaking the build is the point: the whole failure mode is that
this defect **compiles clean**. Only the test catches it.

#### Vendor intermediates vanish mid-session

During this, `wlpch.obj`/`wlpch.pch` and `Box3D/joint.obj` disappeared, giving
`LNK1181` / `LNK1104` / `C1083`. They rebuilt themselves on the next build. When
a PCH error appears mid-session, re-run the build before concluding anything —
the first failure is often the invalidated intermediate, not your change.

### SyncPendingPhysics2D landed (5.1)

`Scene.cpp` now carries `SyncPendingPhysics2D`, mirroring `SyncPendingPhysicsBodies`
for 3D, so a 2D rigid body added after `OnSimulationStart()` gets a Box2D body
instead of hanging in mid-air forever with no error.

### 2026-10-05 late — what is verified, and one blocked-on-user item

```
headless  206/209 across 18 suites
   3 failures are ONE agent's in-flight DropItemAt work, not a regression:
   test_drop_item_at_removes_from_the_inventory_AND_spawns_a_pile
       expected 7, got 6 -- the test assumes a whole-stack drop; the engine
       splits a partial stack (the split-vs-all-or-nothing behaviour the
       inventory suite already documents for WEIGHT)
   test_dropped_item_round_trips_byte_identically
   test_the_pile_lands_at_the_carriers_position_plus_the_offset

new and green:
  test_shipped_graphs        5/5   VB-5 Route C
  test_physics_2d            6/6   5.1 SyncPendingPhysics2D
```

#### BLOCKED ON THE USER: NVRHIContext.h breaks the module build

```
NVRHIContext.h(173,48): error C2065: 'm_VkImGuiSampler': undeclared identifier
NVRHIContext.h(191,66): error C2065: 'm_VkImGuiAttachment': undeclared identifier
```

`NVRHIContext.h` is one of the five files the user owns and is actively editing
(R23). Per that rule this is REPORTED, not fixed. The members now exist at
:431/:436, so it compiles again, but a follow-on build failed with

```
error C1051: program database file has an obsolete format, delete it and recompile
error C2471: cannot update program database
```

with **no .pdb on disk** -- an obsolete-format PDB held open by a live process.
The documented C1041 recovery (delete the stale PDB/tlogs, rebuild) is what
clears it. Not performed, because the file is user-owned.

#### Three of my own bugs in the new lint suite, all caught by its own guard

Writing `test_shipped_graphs.py` cost more than it should have, all three from
not checking the real API first:

1. **Guessed the binding surface.** Wrote `W.ValidateTree(path)`. The truth is
   `Wasteland.VisualBlocks` -- a SUBMODULE -- and `Load(path, graph)` takes a
   LOADED Graph while only `CompileTreeFile` takes a path.
2. **Omitted `wlpath.setup()`.** The interpreter is isolated via
   `python314._pth`, so the module is not importable until wlpath puts it on
   sys.path. Every test raised `ModuleNotFoundError`, and because `run_module`
   catches `Exception` per test the run still printed a TIDY SUMMARY rather than
   failing loudly.
3. **`REPO` derived from cwd.** `t.sh` runs the runner from inside
   `tests/headless`, so the asset glob matched NOTHING: 0/4 checks, looking
   exactly like a suite that ran.

The discovery test -- which asserts the glob found >= 4 assets -- is what caught
2 and 3, and it is the reason to write it FIRST. Verified by mutation: pointing a
node at `NoSuchNodeTypeXyz` takes the suite 5/5 -> 2/5 and names the bad type.

#### The two runners differ; match the local one

`tests/runner.py` (written this session) and `tests/headless/runner.py` are NOT
interchangeable:

| | tests/runner.py | tests/headless/runner.py |
|---|---|---|
| `run_module` | takes a module OBJECT + name | takes a MODULE NAME STRING |
| predicates | check/eq/ne/in/ge/le | check/eq/near/ge/le |

A suite written against the wrong one dies on its LAST line, after doing all its
work. Read the runner in the tree you are adding to.
